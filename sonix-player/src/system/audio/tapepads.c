#include "tapepads.h"

#include "src/system/audio/audio.h"
#include "src/system/device/usb.h"

#include <dirent.h>
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#ifdef HOST_WEB_EMU
#include <time.h>
#endif

#define BLOCK (TAPE_PAD_RATE / 100) // 10 ms
#define DRUM_FRAMES (TAPE_PAD_RATE * 27 / 100)
#define STEP_PHASE (TAPE_PAD_RATE * 60u)
#define IDLE_BLOCKS 80 // release the PCM after the last one-shot's tail

// Lock order: card reservation -> worker_lock -> command_lock. The worker
// only takes command_lock, for short mailbox operations, never for I/O.
static pthread_mutex_t worker_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t command_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_t worker;
static bool joinable, accepting;
static unsigned pending;
static int job_card = -1;
static atomic_int state, step = -1, tempo = 120;
static atomic_uint sounding, revision, playable = (1u << TAPE_PAD_COUNT) - 1;
static atomic_bool stop_requested, looping, overdub, needs_card;
static const char output_owner;

typedef struct {
	tape_pad_kind_t kind;
	int preset; // zero means the pad's default; otherwise preset index + 1
	bool long_clip;
	char path[TAPE_PATH_SIZE];
} pad_slot_t;
static pad_slot_t slots[TAPE_PAD_COUNT]; // frozen until the worker is joined

#define KICK (1u << 0)
#define SNARE (1u << 4)
#define HAT (1u << 8)
#define DEMO {KICK | HAT, 0, HAT, 0, SNARE | HAT, 0, HAT, 0, HAT, 0, KICK | HAT, 0, SNARE | HAT, 0, HAT, 0}
static const unsigned demo_pattern[TAPE_PAD_STEPS] = DEMO;
static atomic_uint pattern[TAPE_PAD_STEPS] = DEMO;

static void finish_locked(void) {
	tapepads_stop();
	if (!joinable) return;
	pthread_join(worker, NULL);
	joinable = false;
}

void tapepads_finish(void) {
	pthread_mutex_lock(&worker_lock);
	finish_locked();
	pthread_mutex_unlock(&worker_lock);
}

static int preset_of(const pad_slot_t *slot, int pad) { return slot->preset ? slot->preset - 1 : pad; }

static void publish_slots(void) {
	unsigned mask = 0;
	bool wavs = false;
	for (int p = 0; p < TAPE_PAD_COUNT; p++) {
		if (slots[p].kind != TAPE_PAD_EMPTY) mask |= 1u << p;
		wavs |= slots[p].kind == TAPE_PAD_WAV;
	}
	playable = mask;
	needs_card = wavs;
	state = TAPE_PADS_IDLE;
	revision++;
}

void tapepads_factory(void) {
	pthread_mutex_lock(&worker_lock);
	finish_locked();
	memset(slots, 0, sizeof(slots));
	publish_slots();
	pthread_mutex_unlock(&worker_lock);
}

void tapepads_release_card(void) {
	pthread_mutex_lock(&worker_lock);
	if (needs_card) {
		finish_locked(); // every sample and the anchored directory close before unmount
		memset(slots, 0, sizeof(slots));
		publish_slots();
	}
	pthread_mutex_unlock(&worker_lock);
}

void tapepads_set_preset(int pad, int preset) {
	if (pad < 0 || pad >= TAPE_PAD_COUNT || preset < -1 || preset >= TAPE_PAD_COUNT) return;
	pthread_mutex_lock(&worker_lock);
	finish_locked();
	slots[pad] = (pad_slot_t){.kind = preset < 0 ? TAPE_PAD_EMPTY : TAPE_PAD_FACTORY, .preset = preset + 1};
	publish_slots();
	pthread_mutex_unlock(&worker_lock);
}

unsigned tapepads_revision(void) { return revision; }

void tapepads_info(tape_pad_info_t out[TAPE_PAD_COUNT]) {
	pthread_mutex_lock(&worker_lock);
	for (int p = 0; p < TAPE_PAD_COUNT; p++) {
		const pad_slot_t *s = &slots[p];
		out[p] = (tape_pad_info_t){.kind = s->kind, .preset = preset_of(s, p), .long_clip = s->long_clip};
		if (s->kind == TAPE_PAD_WAV) {
			const char *base = strrchr(s->path, '/');
			// Every path component was bounded when the file was opened.
			strcpy(out[p].name, base ? base + 1 : s->path);
		}
	}
	pthread_mutex_unlock(&worker_lock);
}

static bool read_slot(int card, const char *path, pad_slot_t *slot) {
	if (!path || !tape_wav_name(path)) return false;
	int fd = tape_open_at(card, path, false);
	if (fd < 0) return false;
	tape_wav_t wav;
	bool ok = tape_wav_probe(fd, &wav);
	close(fd);
	if (!ok) return false;
	*slot = (pad_slot_t){.kind = TAPE_PAD_WAV, .long_clip = tape_wav_is_long(&wav)};
	strcpy(slot->path, path); // tape_open_at checked the length
	return true;
}

bool tapepads_assign(int pad, const char *path) {
	if (pad < 0 || pad >= TAPE_PAD_COUNT) return false;
	int card = tape_card_begin();
	if (card < 0) return false;
	pad_slot_t sample;
	bool ok = read_slot(card, path, &sample);
	if (ok) {
		pthread_mutex_lock(&worker_lock);
		finish_locked();
		slots[pad] = sample;
		publish_slots();
		pthread_mutex_unlock(&worker_lock);
	}
	close(card);
	tape_card_end();
	return ok;
}

static int numbered_pad(const char *name) {
	if (!tape_wav_name(name) || name[0] < '0' || name[0] > '9' || name[1] < '0' || name[1] > '9' ||
		(!strchr(" -_", name[2]) && !(name[2] == '.' && strlen(name) == 6))) return -1;
	int pad = (name[0] - '0') * 10 + name[1] - '0';
	return pad > 0 && pad <= TAPE_PAD_COUNT ? pad - 1 : -1;
}

int tapepads_load_kit(const char *name) {
	if (!name || !*name || name[0] == '.' || strchr(name, '/')) return -2;
	char path[TAPE_PATH_SIZE];
	int prefix = snprintf(path, sizeof(path), "%s/%s", TAPE_KIT_DIR, name);
	if (prefix < 0 || prefix >= (int)sizeof(path)) return -2;
	int card = tape_card_begin();
	if (card < 0) return -1;
	int fd = tape_open_at(card, path, true);
	DIR *dir = fd >= 0 ? fdopendir(fd) : NULL;
	if (!dir && fd >= 0) close(fd);
	pad_slot_t kit[TAPE_PAD_COUNT] = {0};
	for (int p = 0; p < TAPE_PAD_COUNT; p++) kit[p].kind = TAPE_PAD_EMPTY;
	int count = 0;
	if (dir) {
		struct dirent *d;
		while ((d = readdir(dir)) != NULL) {
			int pad = numbered_pad(d->d_name);
			if (pad < 0) continue;
			int length = snprintf(path + prefix, sizeof(path) - (size_t)prefix, "/%s", d->d_name);
			if (length < 0 || (size_t)(prefix + length) >= sizeof(path)) continue;
			bool empty = kit[pad].kind == TAPE_PAD_EMPTY;
			// If a number is shared, take the first valid filename in byte order.
			if ((empty || strcmp(path, kit[pad].path) < 0) && read_slot(card, path, &kit[pad]) && empty) count++;
		}
		// Unnumbered packs also work: keep the first 16 valid WAVs in
		// alphabetical (byte) order. If any pads were numbered, gaps stay
		// empty instead, so a prepared kit always maps exactly as intended.
		if (!count) {
			rewinddir(dir);
			while ((d = readdir(dir)) != NULL) {
				if (d->d_name[0] == '.' || !tape_wav_name(d->d_name)) continue;
				int length = snprintf(path + prefix, sizeof(path) - (size_t)prefix, "/%s", d->d_name);
				if (length < 0 || (size_t)(prefix + length) >= sizeof(path)) continue;
				int at = 0;
				while (at < count && strcmp(kit[at].path, path) < 0) at++;
				if (at == TAPE_PAD_COUNT) continue;
				pad_slot_t sample;
				if (!read_slot(card, path, &sample)) continue;
				if (count < TAPE_PAD_COUNT) count++;
				memmove(kit + at + 1, kit + at, (size_t)(count - at - 1) * sizeof(*kit));
				kit[at] = sample;
			}
		}
		closedir(dir);
	}
	if (count) {
		pthread_mutex_lock(&worker_lock);
		finish_locked();
		memcpy(slots, kit, sizeof(kit));
		publish_slots();
		pthread_mutex_unlock(&worker_lock);
	}
	close(card);
	tape_card_end();
	return count ? count : -2;
}

typedef struct { unsigned frames; int16_t pcm[DRUM_FRAMES]; } drum_t;
// Only touched by the audio worker. Lazily render each factory sound once, so
// the next tap is cheap even after the idle PCM has been released (~380 KiB max).
static drum_t drums[TAPE_PAD_COUNT];

static const drum_t *factory_drum(int preset) {
	drum_t *drum = &drums[preset];
	if (drum->frames) return drum;
	const float tau = 6.2831853f;
	static const unsigned duration[4][4] = {{210, 155, 265, 125}, {170, 220, 145, 250},
		{55, 85, 135, 217}, {165, 208, 232, 260}};
	static const float tuning[4][4] = {{50, 68, 41, 83}, {185, 233, 137, 284},
		{310, 510, 615, 745}, {0, 0, 0, 0}};
	int type = preset / 4, variant = preset % 4;
	unsigned length = TAPE_PAD_RATE * duration[type][variant] / 1000;
	float fall = expf(-4.2f / length), env = 1.0f, phase = 0, low = 0, prev = 0;
	float pitch_drop = type == 0 ? 100.0f + 23.0f * variant : 0;
	float sweep = expf(-(60.0f + 11.0f * variant) / TAPE_PAD_RATE);
	uint32_t rng = 0x1a2b3c4du + (unsigned)preset * 3919;
	unsigned scatter = TAPE_PAD_RATE * (8 + variant * 4) / 1000;
	float clap_fade = expf(-6.0f / scatter), clap_env = 1;
	for (unsigned i = 0; i < length; i++) {
		rng = rng * 1664525u + 1013904223u;
		float noise = ((int)(rng >> 16) - 32768) / 32768.0f;
		low += (0.12f + 0.08f * variant) * (noise - low);
		phase += tau * (tuning[type][variant] + pitch_drop) / TAPE_PAD_RATE;
		pitch_drop *= sweep;
		float sample;
		if (type == 0) {
			sample = 15400 * sinf(phase) + (noise - low) * (100 + 500 * variant) * pitch_drop / 120.0f;
		} else if (type == 1) {
			sample = (4600 + 750 * variant) * (noise - low) + (6000 - 980 * variant) * sinf(phase);
		} else if (type == 2) {
			sample = (2000 + 310 * variant) * (noise - prev) + 950 * (noise - low);
		} else {
			if (i == scatter || i == 2 * scatter) clap_env = 1.0f;
			sample = (9300 * clap_env + 2700) * (noise - 0.28f * variant * low);
			clap_env *= clap_fade;
		}
		float attack = fminf(1, i / (48.0f + 8.0f * variant));
		float tail = fminf(1, (length - 1 - i) / 128.0f);
		drum->pcm[i] = (int16_t)(sample * env * attack * tail);
		env *= fall;
		prev = noise;
	}
	drum->frames = length;
	return drum;
}

typedef struct {
	const drum_t *drum;
	tape_wav_cache_t *cache;
	tape_wav_t wav;
	int fd;
	uint32_t advance;
	uint64_t pos;
	bool on;
} voice_t;

static bool open_sample(voice_t *voice, const pad_slot_t *slot) {
	voice->fd = tape_open_at(job_card, slot->path, false);
	if (voice->fd < 0 || !tape_wav_probe(voice->fd, &voice->wav)) return false;
	voice->cache = calloc(1, sizeof(*voice->cache));
	voice->advance = (uint32_t)((((uint64_t)voice->wav.rate << 16) + TAPE_PAD_RATE / 2) / TAPE_PAD_RATE);
	return voice->cache != NULL;
}

static bool trigger(voice_t voices[TAPE_PAD_COUNT], unsigned mask, bool manual, unsigned *recordable) {
	for (int p = 0; p < TAPE_PAD_COUNT; p++) {
		if (!(mask & (1u << p))) continue;
		voice_t *v = &voices[p];
		const pad_slot_t *s = &slots[p];
		if (s->kind == TAPE_PAD_EMPTY) continue;
		if (s->kind == TAPE_PAD_WAV) {
			if (!manual && s->long_clip) continue; // a backing track is not a drum hit
			if (v->fd < 0 && !open_sample(v, s)) return false;
			if (tape_wav_is_long(&v->wav)) {
				if (!manual) continue;
				bool want = !v->on;
				for (int k = 0; k < TAPE_PAD_COUNT; k++) {
					if (voices[k].fd >= 0 && tape_wav_is_long(&voices[k].wav)) voices[k].on = false;
				}
				if (!want) continue;
			} else if (recordable) *recordable |= 1u << p;
		} else {
			v->drum = factory_drum(preset_of(s, p));
			if (recordable) *recordable |= 1u << p;
		}
		v->pos = 0;
		v->on = true;
	}
	return true;
}

static bool mix(voice_t voices[TAPE_PAD_COUNT], int32_t *sum, unsigned frames) {
	for (int p = 0; p < TAPE_PAD_COUNT; p++) {
		voice_t *v = &voices[p];
		if (!v->on) continue;
		for (unsigned i = 0; i < frames; i++) {
			int left, right;
			if (v->drum) {
				left = right = v->drum->pcm[v->pos++];
				v->on = v->pos < v->drum->frames;
			} else {
				if (!tape_wav_sample(v->fd, &v->wav, v->cache, v->pos, &left, &right)) return false;
				v->pos += v->advance;
				v->on = (v->pos >> 16) < v->wav.frames;
				// Leave a little room for drums over a full-scale vocal.
				left = left * 3 / 4;
				right = right * 3 / 4;
			}
			sum[i * 2] += left;
			sum[i * 2 + 1] += right;
			if (!v->on) break;
		}
	}
	return true;
}

#ifdef HOST_WEB_EMU
static int64_t clock_us(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}
#endif

static void *pads_main(void *unused) {
	(void)unused;
	voice_t voices[TAPE_PAD_COUNT] = {0};
	for (int p = 0; p < TAPE_PAD_COUNT; p++) voices[p].fd = -1;
	tape_pads_state_t result = TAPE_PADS_UNAVAILABLE;
	bool owned = false;
	if (usb_dac_mode() || audio_external_is_active() || stop_requested) goto done;
	// Do not borrow an existing external stream; a deliberate tap/Loop is an
	// input change, like Play in the metronome or starting a track.
	if (audio_playback_context_active() || audio_get_status() != AUDIO_STATUS_STOPPED) audio_stop_and_wait(1000);
	owned = !stop_requested && !audio_playback_context_active() &&
		audio_external_begin_owned(&output_owner, TAPE_PAD_RATE, 2, 16, 32);
	if (!owned) goto done;
	state = TAPE_PADS_PLAYING;
	result = TAPE_PADS_IDLE;
	unsigned quiet = 0, phase = STEP_PHASE;
	int current = TAPE_PAD_STEPS - 1;
	bool was_looping = false;
#ifdef HOST_WEB_EMU
	int64_t next_write = clock_us();
#endif
	while (!stop_requested && !usb_dac_mode() && !audio_playback_context_active()) {
		pthread_mutex_lock(&command_lock);
		unsigned hits = pending;
		pending = 0;
		bool loop = looping, dub = overdub;
		pthread_mutex_unlock(&command_lock);
		if (loop && !was_looping) { phase = STEP_PHASE; current = TAPE_PAD_STEPS - 1; }
		was_looping = loop;
		unsigned performed = 0;
		if (!trigger(voices, hits, true, &performed)) goto bad_sample;
		if (loop && dub && performed) {
			unsigned at = phase >= STEP_PHASE ? (current + 1) % TAPE_PAD_STEPS : current;
			atomic_fetch_or(&pattern[at], performed);
		}

		int32_t sum[BLOCK * 2] = {0};
		unsigned advance = (unsigned)tempo * 4;
		for (unsigned at = 0; at < BLOCK;) {
			unsigned n = BLOCK - at;
			if (loop) {
				if (phase >= STEP_PHASE) {
					phase -= STEP_PHASE;
					current = (current + 1) % TAPE_PAD_STEPS;
					step = current;
					if (!trigger(voices, pattern[current] & ~(at == 0 ? hits : 0u), false, NULL)) goto bad_sample;
				}
				unsigned left = (STEP_PHASE - phase + advance - 1) / advance;
				if (n > left) n = left;
				phase += n * advance;
			}
			if (!mix(voices, sum + at * 2, n)) goto bad_sample;
			at += n;
		}
		unsigned active = 0;
		for (int p = 0; p < TAPE_PAD_COUNT; p++) if (voices[p].on) active |= 1u << p;
		sounding = active;
		if (active || loop || hits) quiet = 0;
		else if (++quiet >= IDLE_BLOCKS) {
			// No tap can land between our last mailbox check and retiring. If
			// it found us closing, the caller joins and opens a new stream.
			pthread_mutex_lock(&command_lock);
			bool done = !pending && !looping;
			if (done) accepting = false;
			pthread_mutex_unlock(&command_lock);
			if (done) break;
			quiet = 0;
		}
		int16_t out[BLOCK * 2];
		for (unsigned i = 0; i < BLOCK * 2; i++) {
			int v = sum[i];
			out[i] = (int16_t)(v > INT16_MAX ? INT16_MAX : v < INT16_MIN ? INT16_MIN : v);
		}
		if (audio_external_write_owned(&output_owner, out, BLOCK) != BLOCK) {
			result = TAPE_PADS_UNAVAILABLE;
			break;
		}
#ifdef HOST_WEB_EMU
		// Virtual ALSA accepts writes immediately; the physical PCM paces itself.
		next_write += 10000;
		int64_t now = clock_us(), wait = next_write - now;
		if (wait > 0) usleep((useconds_t)wait);
		else if (wait < -30000) next_write = now;
#endif
	}
	goto done;
bad_sample:
	result = TAPE_PADS_SAMPLE_ERROR;
done:
	pthread_mutex_lock(&command_lock);
	accepting = false;
	pending = 0;
	looping = overdub = false;
	pthread_mutex_unlock(&command_lock);
	if (owned) audio_external_end_owned(&output_owner);
	for (int p = 0; p < TAPE_PAD_COUNT; p++) {
		if (voices[p].fd >= 0) close(voices[p].fd);
		free(voices[p].cache);
	}
	if (job_card >= 0) close(job_card);
	sounding = 0;
	step = -1;
	state = stop_requested ? TAPE_PADS_IDLE : result;
	return NULL;
}

static bool enqueue(unsigned hits, bool play) {
	pthread_mutex_lock(&command_lock);
	bool ok = accepting && !stop_requested;
	if (ok) {
		pending |= hits;
		if (play) looping = true;
	}
	pthread_mutex_unlock(&command_lock);
	return ok;
}

static bool request(unsigned hits, bool play) {
	if (enqueue(hits, play)) return true;
	int card = needs_card ? tape_card_begin() : -1;
	bool reserved = card >= 0;
	pthread_mutex_lock(&worker_lock);
	bool ok = enqueue(hits, play);
	if (!ok) {
		finish_locked();
		if (needs_card && card < 0) state = TAPE_PADS_SAMPLE_ERROR;
		else {
			job_card = card;
			pthread_mutex_lock(&command_lock);
			pending = hits;
			stop_requested = false;
			looping = play;
			state = TAPE_PADS_OPENING;
			if (pthread_create(&worker, NULL, pads_main, NULL) == 0) {
				accepting = joinable = ok = true;
				card = -1; // now the worker's fd
			} else {
				looping = false;
				state = TAPE_PADS_UNAVAILABLE;
			}
			pthread_mutex_unlock(&command_lock);
		}
	}
	pthread_mutex_unlock(&worker_lock);
	if (card >= 0) close(card);
	if (reserved) tape_card_end();
	return ok;
}

bool tapepads_hit(int pad) {
	return pad >= 0 && pad < TAPE_PAD_COUNT && (playable & (1u << pad)) && request(1u << pad, false);
}
bool tapepads_start(void) { return request(0, true); }
void tapepads_stop(void) { stop_requested = true; looping = overdub = false; }
tape_pads_state_t tapepads_state(void) { return state; }
bool tapepads_active(void) {
	tape_pads_state_t now = state;
	return now == TAPE_PADS_OPENING || now == TAPE_PADS_PLAYING;
}
unsigned tapepads_sounding(void) { return sounding; }
bool tapepads_looping(void) { return looping; }
bool tapepads_overdub(void) { return overdub; }
void tapepads_set_overdub(bool enabled) { overdub = enabled && looping; }
int tapepads_step(void) { return step; }
void tapepads_pattern(bool demo) {
	pthread_mutex_lock(&worker_lock);
	finish_locked(); // a last in-flight overdub cannot enter the new pattern
	for (int i = 0; i < TAPE_PAD_STEPS; i++) pattern[i] = demo ? demo_pattern[i] : 0;
	pthread_mutex_unlock(&worker_lock);
}
int tapepads_bpm(void) { return tempo; }
void tapepads_set_bpm(int bpm) {
	if (bpm < TAPE_PAD_MIN_BPM) bpm = TAPE_PAD_MIN_BPM;
	if (bpm > TAPE_PAD_MAX_BPM) bpm = TAPE_PAD_MAX_BPM;
	tempo = bpm;
}
