#include "tapepads.h"

#include "src/system/audio/audio.h"
#include "src/system/device/usb.h"

#include <dirent.h>
#include <errno.h>
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

// Lock order: card reservation -> worker_lock -> command_lock -> take lock.
// The PCM worker doesn't acquire the first two or hold either mailbox over I/O.
static pthread_mutex_t worker_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t command_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_t worker;
static bool joinable, accepting;
static unsigned pending;
static int job_card = -1;
static atomic_int state, step = -1, tempo = 120;
static atomic_uint sounding, revision, effects, playable = (1u << TAPE_PAD_COUNT) - 1;
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
	if (joinable) {
		pthread_join(worker, NULL);
		joinable = false;
	}
	tapemix_finish();
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
	} else {
		// A factory-only performance has no sample fds, but the take is
		// still writing to this card. The instruments can keep running.
		tapemix_finish();
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
	int card = tape_card_begin(false);
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
	int card = tape_card_begin(false);
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

// Sequence v1: six magic bytes, version/reserved, LE16 BPM, sixteen LE16
// step masks, then one tag per pad. A WAV tag is followed by its LE16 path
// length and raw card-relative path (no NUL). No samples or native structs.
static const uint8_t seq_magic[] = {'S','N','X','S','E','Q',1,0};
#define SEQ_HEADER (sizeof(seq_magic) + 2 + TAPE_PAD_STEPS * 2)
#define SEQ_EMPTY 16
#define SEQ_WAV 17

static void put_le16(uint8_t *p, unsigned value) {
	p[0] = (uint8_t)value;
	p[1] = (uint8_t)(value >> 8);
}

static unsigned get_le16(const uint8_t *p) { return (unsigned)p[0] | (unsigned)p[1] << 8; }

static ssize_t seq_read(int fd, void *out, size_t size) {
	ssize_t n;
	do n = read(fd, out, size); while (n < 0 && errno == EINTR);
	return n;
}

static bool read_exact(int fd, void *out, size_t size) {
	uint8_t *p = out;
	while (size) {
		ssize_t n = seq_read(fd, p, size);
		if (n <= 0) return false;
		p += n;
		size -= (size_t)n;
	}
	return true;
}

tape_seq_result_t tapepads_save_sequence(void) {
	int card = tape_card_begin(true);
	if (card < 0) return TAPE_SEQ_NO_CARD;
	uint8_t head[SEQ_HEADER];
	memcpy(head, seq_magic, sizeof(seq_magic));
	pad_slot_t saved[TAPE_PAD_COUNT];
	pthread_mutex_lock(&worker_lock);
	memcpy(saved, slots, sizeof(saved));
	pthread_mutex_lock(&command_lock);
	put_le16(head + sizeof(seq_magic), (unsigned)tempo);
	for (int i = 0; i < TAPE_PAD_STEPS; i++) put_le16(head + sizeof(seq_magic) + 2 + 2 * i, pattern[i]);
	pthread_mutex_unlock(&command_lock);
	pthread_mutex_unlock(&worker_lock);

	tape_savefile_t file;
	bool ok = tape_store_begin(card, TAPE_STORE_SEQUENCE, &file);
	if (ok) {
		ok = tape_store_write(file.fd, head, sizeof(head));
		for (int p = 0; p < TAPE_PAD_COUNT && ok; p++) {
			const pad_slot_t *slot = &saved[p];
			uint8_t tag = slot->kind == TAPE_PAD_WAV ? SEQ_WAV :
				slot->kind == TAPE_PAD_EMPTY ? SEQ_EMPTY : (uint8_t)preset_of(slot, p);
			ok = tape_store_write(file.fd, &tag, 1);
			if (ok && tag == SEQ_WAV) {
				size_t len = strlen(slot->path);
				uint8_t size[2];
				put_le16(size, (unsigned)len);
				ok = tape_store_write(file.fd, size, sizeof(size)) && tape_store_write(file.fd, slot->path, len);
			}
		}
		ok = tape_store_end(&file, ok);
	}
	close(card);
	tape_card_end();
	return ok ? TAPE_SEQ_OK : TAPE_SEQ_SAVE_ERROR;
}

static tape_seq_result_t read_sequence(int card, int fd, int *bpm, unsigned *beats, pad_slot_t *pads) {
	uint8_t head[SEQ_HEADER];
	if (!read_exact(fd, head, sizeof(head)) || memcmp(head, seq_magic, sizeof(seq_magic))) return TAPE_SEQ_INVALID;
	*bpm = (int)get_le16(head + sizeof(seq_magic));
	if (*bpm < TAPE_PAD_MIN_BPM || *bpm > TAPE_PAD_MAX_BPM) return TAPE_SEQ_INVALID;
	for (int i = 0; i < TAPE_PAD_STEPS; i++) beats[i] = get_le16(head + sizeof(seq_magic) + 2 + 2 * i);
	for (int p = 0; p < TAPE_PAD_COUNT; p++) {
		uint8_t tag;
		if (!read_exact(fd, &tag, 1)) return TAPE_SEQ_INVALID;
		if (tag < TAPE_PAD_COUNT) pads[p] = (pad_slot_t){.kind = TAPE_PAD_FACTORY, .preset = tag + 1};
		else if (tag == SEQ_EMPTY) pads[p] = (pad_slot_t){.kind = TAPE_PAD_EMPTY};
		else if (tag == SEQ_WAV) {
			uint8_t size[2];
			char path[TAPE_PATH_SIZE];
			if (!read_exact(fd, size, sizeof(size))) return TAPE_SEQ_INVALID;
			unsigned len = get_le16(size);
			if (!len || len >= sizeof(path) || !read_exact(fd, path, len) || memchr(path, 0, len)) return TAPE_SEQ_INVALID;
			path[len] = '\0';
			if (!read_slot(card, path, &pads[p])) return TAPE_SEQ_SAMPLE_MISSING;
		} else return TAPE_SEQ_INVALID;
	}
	uint8_t extra;
	return seq_read(fd, &extra, 1) == 0 ? TAPE_SEQ_OK : TAPE_SEQ_INVALID;
}

tape_seq_result_t tapepads_load_sequence(const char *name) {
	int card = tape_card_begin(false);
	if (card < 0) return TAPE_SEQ_NO_CARD;
	int fd = tape_store_open(card, TAPE_STORE_SEQUENCE, name);
	int bpm;
	unsigned beats[TAPE_PAD_STEPS];
	pad_slot_t pads[TAPE_PAD_COUNT];
	tape_seq_result_t result = fd < 0 ? TAPE_SEQ_INVALID : read_sequence(card, fd, &bpm, beats, pads);
	if (fd >= 0) close(fd);
	if (result == TAPE_SEQ_OK) {
		pthread_mutex_lock(&worker_lock);
		finish_locked();
		memcpy(slots, pads, sizeof(slots));
		tempo = bpm;
		for (int i = 0; i < TAPE_PAD_STEPS; i++) pattern[i] = beats[i];
		publish_slots();
		pthread_mutex_unlock(&worker_lock);
	}
	close(card);
	tape_card_end();
	return result;
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

// Integer DSP for the small V1. Each toggle ramps over ~12 ms, without
// changing the playback clock or restarting any voices. Controls are on the
// full stereo bus, so the monitor and the WAV get the very same samples.
#define FX_WET 512u
#define ECHO_FRAMES (TAPE_PAD_RATE / 4)
static struct {
	int32_t low[2]; // Q8 filter state
	unsigned filter, drive, echo, at;
	int16_t delay[ECHO_FRAMES][2];
} fx; // mixer worker only

static unsigned fade(unsigned current, bool target) {
	return target ? current + (current < FX_WET) : current - (current > 0);
}

static int32_t blend(int32_t dry, int32_t wet, unsigned amount) {
	return dry + (wet - dry) * (int32_t)amount / (int32_t)FX_WET;
}

static int16_t clip16(int32_t v) {
	return (int16_t)(v > INT16_MAX ? INT16_MAX : v < INT16_MIN ? INT16_MIN : v);
}

static int32_t saturate(int32_t v) {
	int32_t amp = clip16(v);
	if (amp < 0) amp = -amp;
	amp *= 3;
	if (amp > 24576) {
		int32_t over = amp - 24576;
		amp = 24576 + 8191 * over / (8192 + over);
	}
	return v < 0 ? -amp : amp;
}

static bool render(const int32_t *sum, int16_t *out) {
	unsigned mask = effects;
	if (!(mask || fx.filter || fx.drive || fx.echo)) {
		for (unsigned i = 0; i < BLOCK * 2; i++) out[i] = clip16(sum[i]);
		return false;
	}
	bool clear_delay = fx.echo && !(mask & TAPE_FX_ECHO);
	bool tail = false;
	for (unsigned i = 0; i < BLOCK; i++) {
		fx.filter = fade(fx.filter, mask & TAPE_FX_FILTER);
		fx.drive = fade(fx.drive, mask & TAPE_FX_DRIVE);
		fx.echo = fade(fx.echo, mask & TAPE_FX_ECHO);
		for (int ch = 0; ch < 2; ch++) {
			int32_t x = sum[i * 2 + ch];
			if (fx.filter) {
				fx.low[ch] += (x * 256 - fx.low[ch]) / 8;
				x = blend(x, fx.low[ch] / 256, fx.filter);
			}
			if (fx.drive) x = blend(x, saturate(x), fx.drive);
			if (fx.echo) {
				int32_t y = fx.delay[fx.at][ch];
				fx.delay[fx.at][ch] = clip16(x + y * 3 / 8);
				x += y * (int32_t)fx.echo / (int32_t)(2 * FX_WET);
			}
			out[i * 2 + ch] = clip16(x);
			tail |= out[i * 2 + ch] != 0;
		}
		if (fx.echo && ++fx.at == ECHO_FRAMES) fx.at = 0;
	}
	if (!fx.filter) fx.low[0] = fx.low[1] = 0;
	if (clear_delay && !fx.echo) {
		memset(fx.delay, 0, sizeof(fx.delay));
		fx.at = 0;
	}
	return tail;
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
	memset(&fx, 0, sizeof(fx));
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
		if (!loop && was_looping) step = -1;
		was_looping = loop;
		unsigned performed = 0;
		if (!trigger(voices, hits, true, &performed)) goto bad_sample;
		if (loop && dub && performed) {
			unsigned at = phase >= STEP_PHASE ? (current + 1) % TAPE_PAD_STEPS : current;
			pthread_mutex_lock(&command_lock);
			pattern[at] |= performed;
			pthread_mutex_unlock(&command_lock);
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
		int16_t out[BLOCK * 2];
		bool tail = render(sum, out);
		if (active || loop || hits || tail || tapemix_accepting()) quiet = 0;
		else if (++quiet >= IDLE_BLOCKS) {
			// No tap can land between our last mailbox check and retiring. If
			// it found us closing, the caller joins and opens a new stream.
			pthread_mutex_lock(&command_lock);
			bool done = !pending && !looping && !tapemix_accepting();
			if (done) accepting = false;
			pthread_mutex_unlock(&command_lock);
			if (done) break;
			quiet = 0;
		}
		int written = audio_external_write_owned(&output_owner, out, BLOCK);
		if (written > 0) tapemix_push(out, (unsigned)written);
		if (written != BLOCK) {
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
	if (!stop_requested && (usb_dac_mode() || audio_playback_context_active())) result = TAPE_PADS_UNAVAILABLE;
	goto done;
bad_sample:
	result = TAPE_PADS_SAMPLE_ERROR;
done:
	pthread_mutex_lock(&command_lock);
	accepting = false;
	pending = 0;
	looping = overdub = false;
	if (stop_requested) tapemix_stop();
	else tapemix_interrupt();
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

// worker_lock and command_lock held, with the previous PCM joined.
static bool start_locked(unsigned hits, bool play, int samples) {
	job_card = samples;
	pending = hits;
	stop_requested = false;
	looping = play;
	state = TAPE_PADS_OPENING;
	if (pthread_create(&worker, NULL, pads_main, NULL) == 0) {
		accepting = joinable = true;
		return true;
	}
	looping = false;
	state = TAPE_PADS_UNAVAILABLE;
	return false;
}

static bool request(unsigned hits, bool play) {
	if (enqueue(hits, play)) return true;
	int card = needs_card ? tape_card_begin(false) : -1;
	bool reserved = card >= 0;
	pthread_mutex_lock(&worker_lock);
	bool ok = enqueue(hits, play);
	if (!ok) {
		finish_locked();
		if (needs_card && card < 0) state = TAPE_PADS_SAMPLE_ERROR;
		else {
			int samples = needs_card ? card : -1;
			pthread_mutex_lock(&command_lock);
			ok = start_locked(hits, play, samples);
			pthread_mutex_unlock(&command_lock);
			if (ok && samples >= 0) card = -1; // now the worker's fd
		}
	}
	pthread_mutex_unlock(&worker_lock);
	if (card >= 0) close(card);
	if (reserved) tape_card_end();
	return ok;
}

tape_mix_state_t tapepads_record(void) {
	int card = tape_card_begin(true);
	if (card < 0) return TAPE_MIX_NO_CARD;
	pthread_mutex_lock(&worker_lock);
	tape_mix_state_t result = TAPE_MIX_BUSY;
	if (!tapemix_active()) {
		pthread_mutex_lock(&command_lock);
		bool reuse = accepting && !stop_requested;
		int samples = -1;
		if (!reuse) {
			pthread_mutex_unlock(&command_lock);
			finish_locked();
			// A take writer and a sampler importing WAVs each own their fd.
			if (needs_card) samples = tape_open_at(card, "", true);
			pthread_mutex_lock(&command_lock);
		}
		result = !reuse && needs_card && samples < 0 ? TAPE_MIX_ERROR : tapemix_start(card);
		if (result == TAPE_MIX_OPENING) {
			card = -1; // now the writer's fd
			if (!reuse) {
				if (start_locked(0, false, samples)) samples = -1;
				else { tapemix_interrupt(); result = TAPE_MIX_ERROR; }
			}
		}
		if (samples >= 0) close(samples);
		pthread_mutex_unlock(&command_lock);
	}
	pthread_mutex_unlock(&worker_lock);
	if (card >= 0) close(card);
	tape_card_end();
	return result;
}

bool tapepads_hit(int pad) {
	return pad >= 0 && pad < TAPE_PAD_COUNT && (playable & (1u << pad)) && request(1u << pad, false);
}
bool tapepads_start(void) { return request(0, true); }
void tapepads_stop_loop(void) {
	pthread_mutex_lock(&command_lock);
	looping = overdub = false;
	pthread_mutex_unlock(&command_lock);
}
void tapepads_stop(void) {
	pthread_mutex_lock(&command_lock);
	stop_requested = true;
	looping = overdub = false;
	tapemix_stop();
	pthread_mutex_unlock(&command_lock);
}
unsigned tapepads_effects(void) { return effects; }
void tapepads_toggle_effect(tape_pad_effect_t effect) {
	atomic_fetch_xor(&effects, (unsigned)effect & (TAPE_FX_FILTER | TAPE_FX_ECHO | TAPE_FX_DRIVE));
}
tape_pads_state_t tapepads_state(void) { return state; }
bool tapepads_active(void) {
	tape_pads_state_t now = state;
	return now == TAPE_PADS_OPENING || now == TAPE_PADS_PLAYING || tapemix_active();
}
unsigned tapepads_sounding(void) { return sounding; }
bool tapepads_looping(void) { return looping; }
bool tapepads_overdub(void) { return overdub; }
void tapepads_set_overdub(bool enabled) {
	pthread_mutex_lock(&command_lock);
	overdub = enabled && looping;
	pthread_mutex_unlock(&command_lock);
}
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
