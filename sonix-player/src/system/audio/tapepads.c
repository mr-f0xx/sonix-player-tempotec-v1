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
static atomic_int state, step = -1, tempo = 120, resample_mode = TAPE_INTERP_LINEAR;
static atomic_uint sounding, revision, effects, playable = (1u << TAPE_PAD_COUNT) - 1;
static atomic_bool stop_requested, looping, overdub, needs_card;
static const char output_owner;

typedef struct {
	tape_pad_kind_t kind;
	int preset; // zero means the pad's default; otherwise preset index + 1
	bool long_clip, levels;
	uint32_t frames, rate; // complete source WAV, used to validate markers
	uint32_t trim_start, trim_end; // source frames; trim_end == 0 means full
	uint32_t pitch_q16; // zero on untouched factory pads means unity
	int semitone;
	int8_t choke; // positive cuts, negative is cut, zero is independent
	uint8_t interpolation;
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

static uint32_t slot_pitch(const pad_slot_t *slot) {
	return slot->pitch_q16 ? slot->pitch_q16 : TAPE_PITCH_UNITY;
}

static tape_interp_t slot_interp(const pad_slot_t *slot) {
	return slot->interpolation == TAPE_INTERP_CUBIC ? TAPE_INTERP_CUBIC : TAPE_INTERP_LINEAR;
}

static uint32_t slot_end(const pad_slot_t *slot) {
	return slot->trim_end ? slot->trim_end : slot->frames;
}

static bool trimmed(const pad_slot_t *slot) {
	return slot->kind == TAPE_PAD_WAV && (slot->trim_start || (slot->trim_end && slot->trim_end < slot->frames));
}

// A trimmed region of a long file becomes a normal one-shot, and pitching the
// sample changes how long it takes to play. The sequencer uses this decision,
// not the length of the entire WAV on the card.
static void update_long_clip(pad_slot_t *slot) {
	if (slot->kind != TAPE_PAD_WAV || !slot->frames || !slot->rate) {
		slot->long_clip = false;
		return;
	}
	uint32_t end = slot_end(slot);
	if (end > slot->frames) end = slot->frames;
	uint64_t length = end > slot->trim_start ? end - slot->trim_start : 0;
	uint64_t denom = (uint64_t)slot->rate * slot_pitch(slot);
	uint64_t played = denom ? ((length * (uint64_t)TAPE_PAD_RATE * TAPE_PITCH_UNITY) + denom / 2) / denom : 0;
	slot->long_clip = played > (uint64_t)TAPE_PAD_RATE * TAPE_SHORT_CLIP_SECONDS;
}

static pad_slot_t factory_slot(int pad, int preset) {
	(void)pad;
	return (pad_slot_t){.kind = preset < 0 ? TAPE_PAD_EMPTY : TAPE_PAD_FACTORY,
		.preset = preset + 1, .pitch_q16 = TAPE_PITCH_UNITY,
		.interpolation = (uint8_t)resample_mode};
}

static void reset_factory(pad_slot_t *dst, int count) {
	for (int p = 0; p < count; p++) dst[p] = factory_slot(p, p);
}

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
	reset_factory(slots, TAPE_PAD_COUNT);
	publish_slots();
	pthread_mutex_unlock(&worker_lock);
}

void tapepads_release_card(void) {
	pthread_mutex_lock(&worker_lock);
	if (needs_card) {
		finish_locked(); // every sample and the anchored directory close before unmount
		reset_factory(slots, TAPE_PAD_COUNT);
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
	int8_t choke = slots[pad].choke; // editing the sound keeps the performance routing
	slots[pad] = factory_slot(pad, preset);
	slots[pad].choke = choke;
	publish_slots();
	pthread_mutex_unlock(&worker_lock);
}

unsigned tapepads_revision(void) { return revision; }

void tapepads_info(tape_pad_info_t out[TAPE_PAD_COUNT]) {
	pthread_mutex_lock(&worker_lock);
	for (int p = 0; p < TAPE_PAD_COUNT; p++) {
		const pad_slot_t *s = &slots[p];
		out[p] = (tape_pad_info_t){
			.kind = s->kind, .preset = preset_of(s, p), .long_clip = s->long_clip,
			.frames = s->frames, .rate = s->rate, .trim_start = s->trim_start,
			.trim_end = slot_end(s), .pitch_q16 = slot_pitch(s), .semitone = s->semitone,
			.choke = s->choke, .interpolation = slot_interp(s), .levels = s->levels,
			.trimmed = trimmed(s),
		};
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
	*slot = (pad_slot_t){.kind = TAPE_PAD_WAV, .frames = wav.frames, .rate = wav.rate,
		.trim_end = wav.frames, .pitch_q16 = TAPE_PITCH_UNITY,
		.interpolation = (uint8_t)resample_mode};
	strcpy(slot->path, path); // tape_open_at checked the length
	update_long_clip(slot);
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
		sample.choke = slots[pad].choke; // keep the pad's choke assignment
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
// length and raw card-relative path (no NUL). V2 keeps that layout and adds
// one fixed 16-byte record per pad: trim start/end, Q16 pitch, semitone,
// choke group, interpolation and level flags. No samples or native structs.
static const uint8_t seq_prefix[] = {'S','N','X','S','E','Q'};
#define SEQ_V1 1
#define SEQ_V2 2
#define SEQ_HEADER (sizeof(seq_prefix) + 2 + 2 + TAPE_PAD_STEPS * 2)
#define SEQ_META 16
#define SEQ_EMPTY 16
#define SEQ_WAV 17

static void put_le16(uint8_t *p, unsigned value) {
	p[0] = (uint8_t)value;
	p[1] = (uint8_t)(value >> 8);
}

static unsigned get_le16(const uint8_t *p) { return (unsigned)p[0] | (unsigned)p[1] << 8; }

static void put_le32(uint8_t *p, uint32_t value) {
	for (unsigned i = 0; i < 4; i++) p[i] = (uint8_t)(value >> (8 * i));
}

static uint32_t get_le32(const uint8_t *p) {
	return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

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

static bool seq_has_metadata(const pad_slot_t saved[TAPE_PAD_COUNT]) {
	for (int p = 0; p < TAPE_PAD_COUNT; p++) {
		const pad_slot_t *slot = &saved[p];
		if (slot->choke || slot->interpolation == TAPE_INTERP_CUBIC || slot->levels ||
			slot->semitone || slot_pitch(slot) != TAPE_PITCH_UNITY || trimmed(slot)) return true;
	}
	return false;
}

static void seq_metadata(uint8_t out[SEQ_META], const pad_slot_t *slot) {
	memset(out, 0, SEQ_META);
	put_le32(out, slot->trim_start);
	put_le32(out + 4, slot_end(slot));
	put_le32(out + 8, slot_pitch(slot));
	out[12] = (uint8_t)(int8_t)(slot->semitone < -24 ? -24 : slot->semitone > 24 ? 24 : slot->semitone);
	out[13] = (uint8_t)slot->choke;
	out[14] = (uint8_t)slot_interp(slot);
	out[15] = (uint8_t)(slot->levels && slot->kind == TAPE_PAD_WAV ? 1 : 0);
}

tape_seq_result_t tapepads_save_sequence(void) {
	int card = tape_card_begin(true);
	if (card < 0) return TAPE_SEQ_NO_CARD;
	uint8_t head[SEQ_HEADER];
	memcpy(head, seq_prefix, sizeof(seq_prefix));
	head[sizeof(seq_prefix) + 1] = 0;
	pad_slot_t saved[TAPE_PAD_COUNT];
	pthread_mutex_lock(&worker_lock);
	memcpy(saved, slots, sizeof(saved));
	bool metadata = seq_has_metadata(saved);
	head[sizeof(seq_prefix)] = metadata ? SEQ_V2 : SEQ_V1;
	pthread_mutex_lock(&command_lock);
	put_le16(head + sizeof(seq_prefix) + 2, (unsigned)tempo);
	for (int i = 0; i < TAPE_PAD_STEPS; i++) put_le16(head + sizeof(seq_prefix) + 4 + 2 * i, pattern[i]);
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
		if (metadata) {
			for (int p = 0; p < TAPE_PAD_COUNT && ok; p++) {
				uint8_t meta[SEQ_META];
				seq_metadata(meta, &saved[p]);
				ok = tape_store_write(file.fd, meta, sizeof(meta));
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
	if (!read_exact(fd, head, sizeof(head)) || memcmp(head, seq_prefix, sizeof(seq_prefix)) ||
		head[sizeof(seq_prefix) + 1] || (head[sizeof(seq_prefix)] != SEQ_V1 && head[sizeof(seq_prefix)] != SEQ_V2)) {
		return TAPE_SEQ_INVALID;
	}
	bool v2 = head[sizeof(seq_prefix)] == SEQ_V2;
	*bpm = (int)get_le16(head + sizeof(seq_prefix) + 2);
	if (*bpm < TAPE_PAD_MIN_BPM || *bpm > TAPE_PAD_MAX_BPM) return TAPE_SEQ_INVALID;
	for (int i = 0; i < TAPE_PAD_STEPS; i++) beats[i] = get_le16(head + sizeof(seq_prefix) + 4 + 2 * i);
	for (int p = 0; p < TAPE_PAD_COUNT; p++) {
		uint8_t tag;
		if (!read_exact(fd, &tag, 1)) return TAPE_SEQ_INVALID;
		if (tag < TAPE_PAD_COUNT) pads[p] = factory_slot(p, tag);
		else if (tag == SEQ_EMPTY) pads[p] = factory_slot(p, -1);
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
	if (v2) {
		for (int p = 0; p < TAPE_PAD_COUNT; p++) {
			uint8_t meta[SEQ_META];
			if (!read_exact(fd, meta, sizeof(meta))) return TAPE_SEQ_INVALID;
			uint32_t start = get_le32(meta), end = get_le32(meta + 4), pitch = get_le32(meta + 8);
			int semitone = (int8_t)meta[12], choke = (int8_t)meta[13];
			if (pitch < TAPE_PITCH_MIN || pitch > TAPE_PITCH_MAX || meta[14] > TAPE_INTERP_CUBIC ||
				choke < -TAPE_CHOKE_MAX || choke > TAPE_CHOKE_MAX || (meta[15] & ~1u)) return TAPE_SEQ_INVALID;
			pads[p].choke = (int8_t)choke;
			pads[p].interpolation = meta[14];
			pads[p].pitch_q16 = pitch;
			pads[p].semitone = semitone;
			pads[p].levels = ((meta[15] & 1u) != 0) && pads[p].kind == TAPE_PAD_WAV;
			if (pads[p].kind != TAPE_PAD_WAV) continue;
			uint32_t frames = pads[p].frames;
			if (!end || end > frames) end = frames;
			if (start >= end) { start = 0; end = frames; }
			pads[p].trim_start = start;
			pads[p].trim_end = end;
			update_long_clip(&pads[p]);
		}
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

static tape_interp_t checked_interp(tape_interp_t interpolation) {
	return interpolation == TAPE_INTERP_CUBIC ? TAPE_INTERP_CUBIC : TAPE_INTERP_LINEAR;
}

void tapepads_trim(int pad, uint32_t *start, uint32_t *end) {
	if (start) *start = 0;
	if (end) *end = 0;
	if (pad < 0 || pad >= TAPE_PAD_COUNT) return;
	pthread_mutex_lock(&worker_lock);
	if (slots[pad].kind == TAPE_PAD_WAV) {
		if (start) *start = slots[pad].trim_start;
		if (end) *end = slot_end(&slots[pad]);
	}
	pthread_mutex_unlock(&worker_lock);
}

bool tapepads_set_trim(int pad, uint32_t start, uint32_t end) {
	if (pad < 0 || pad >= TAPE_PAD_COUNT) return false;
	pthread_mutex_lock(&worker_lock);
	pad_slot_t *slot = &slots[pad];
	if (slot->kind != TAPE_PAD_WAV || !slot->frames) {
		pthread_mutex_unlock(&worker_lock);
		return false;
	}
	finish_locked();
	uint32_t frames = slot->frames;
	if (!end || end > frames) end = frames;
	if (start >= frames) start = frames - 1;
	if (start >= end) {
		if (end > 1) start = end - 1;
		else { start = 0; end = 1; }
	}
	slot->trim_start = start;
	slot->trim_end = end;
	update_long_clip(slot);
	publish_slots();
	pthread_mutex_unlock(&worker_lock);
	return true;
}

bool tapepads_set_trim_ms(int pad, unsigned start_ms, unsigned end_ms) {
	if (pad < 0 || pad >= TAPE_PAD_COUNT) return false;
	pthread_mutex_lock(&worker_lock);
	uint32_t rate = slots[pad].rate;
	pthread_mutex_unlock(&worker_lock);
	if (!rate) return false;
	uint64_t start = (uint64_t)start_ms * rate / 1000;
	uint64_t end = end_ms ? (uint64_t)end_ms * rate / 1000 : 0;
	if (start > UINT32_MAX) start = UINT32_MAX;
	if (end > UINT32_MAX) end = UINT32_MAX;
	return tapepads_set_trim(pad, (uint32_t)start, (uint32_t)end);
}

bool tapepads_set_pitch(int pad, int semitones) {
	if (pad < 0 || pad >= TAPE_PAD_COUNT) return false;
	pthread_mutex_lock(&worker_lock);
	pad_slot_t *slot = &slots[pad];
	if (slot->kind == TAPE_PAD_EMPTY) {
		pthread_mutex_unlock(&worker_lock);
		return false;
	}
	finish_locked();
	if (semitones < -24) semitones = -24;
	if (semitones > 24) semitones = 24;
	slot->semitone = semitones;
	slot->pitch_q16 = tape_pitch_q16(semitones);
	update_long_clip(slot);
	publish_slots();
	pthread_mutex_unlock(&worker_lock);
	return true;
}

void tapepads_set_interpolation(tape_interp_t interpolation) {
	tape_interp_t mode = checked_interp(interpolation);
	resample_mode = mode;
	pthread_mutex_lock(&worker_lock);
	finish_locked();
	for (int p = 0; p < TAPE_PAD_COUNT; p++) {
		if (slots[p].kind != TAPE_PAD_EMPTY) slots[p].interpolation = (uint8_t)mode;
	}
	publish_slots();
	pthread_mutex_unlock(&worker_lock);
}

tape_interp_t tapepads_interpolation(void) { return checked_interp((tape_interp_t)resample_mode); }

bool tapepads_slice(const char *path, int slices, tape_interp_t interpolation) {
	if (!path) return false;
	if (slices == 0) slices = TAPE_PAD_COUNT;
	if (slices < 1 || slices > TAPE_PAD_COUNT) return false;
	tape_interp_t mode = checked_interp(interpolation);
	resample_mode = mode;
	int card = tape_card_begin(false);
	if (card < 0) return false;
	pad_slot_t sample;
	bool ok = read_slot(card, path, &sample);
	if (ok) {
		pthread_mutex_lock(&worker_lock);
		finish_locked();
		for (int p = 0; p < TAPE_PAD_COUNT; p++) {
			if (p >= slices) {
				slots[p] = factory_slot(p, -1);
				continue;
			}
			slots[p] = sample;
			slots[p].trim_start = (uint32_t)((uint64_t)sample.frames * (unsigned)p / (unsigned)slices);
			slots[p].trim_end = (uint32_t)((uint64_t)sample.frames * (unsigned)(p + 1) / (unsigned)slices);
			slots[p].interpolation = (uint8_t)mode;
			slots[p].levels = false;
			slots[p].choke = 0;
			if (slots[p].trim_end <= slots[p].trim_start) {
				slots[p].trim_end = slots[p].trim_start + 1;
				if (slots[p].trim_end > sample.frames) {
					slots[p].trim_end = sample.frames;
					slots[p].trim_start = sample.frames - 1;
				}
			}
			update_long_clip(&slots[p]);
		}
		publish_slots();
		pthread_mutex_unlock(&worker_lock);
	}
	close(card);
	tape_card_end();
	return ok;
}

bool tapepads_set_levels(const char *path, int root_pad, int semitone_step, tape_interp_t interpolation) {
	if (!path) return false;
	if (root_pad < 0) root_pad = TAPE_LEVELS_ROOT;
	if (root_pad >= TAPE_PAD_COUNT) root_pad = TAPE_PAD_COUNT - 1;
	if (semitone_step < 1) semitone_step = 1;
	if (semitone_step > 4) semitone_step = 4;
	tape_interp_t mode = checked_interp(interpolation);
	resample_mode = mode;
	int card = tape_card_begin(false);
	if (card < 0) return false;
	pad_slot_t sample;
	bool ok = read_slot(card, path, &sample);
	if (ok) {
		pthread_mutex_lock(&worker_lock);
		finish_locked();
		for (int p = 0; p < TAPE_PAD_COUNT; p++) {
			slots[p] = sample;
			int semitones = (p - root_pad) * semitone_step;
			if (semitones < -24) semitones = -24;
			if (semitones > 24) semitones = 24;
			slots[p].semitone = semitones;
			slots[p].pitch_q16 = tape_pitch_q16(semitones);
			slots[p].interpolation = (uint8_t)mode;
			slots[p].levels = true;
			slots[p].choke = 0;
			update_long_clip(&slots[p]);
		}
		publish_slots();
		pthread_mutex_unlock(&worker_lock);
	}
	close(card);
	tape_card_end();
	return ok;
}

bool tapepads_levels(const char *path, tape_levels_mode_t mode, tape_interp_t interpolation) {
	int step = mode == TAPE_LEVELS_SPREAD ? 2 : 1;
	return tapepads_set_levels(path, TAPE_LEVELS_ROOT, step, interpolation);
}

bool tapepads_enable_levels(const char *path, int root_pad, tape_interp_t interpolation) {
	return tapepads_set_levels(path, root_pad, 1, interpolation);
}

bool tapepads_set_16levels(const char *path, int root_pad, tape_interp_t interpolation) {
	return tapepads_set_levels(path, root_pad, 1, interpolation);
}

bool tapepads_levels_active(void) {
	pthread_mutex_lock(&worker_lock);
	bool active = true;
	for (int p = 0; p < TAPE_PAD_COUNT && active; p++) {
		active = slots[p].kind == TAPE_PAD_WAV && slots[p].levels;
	}
	pthread_mutex_unlock(&worker_lock);
	return active;
}

bool tapepads_set_choke_group(int pad, int group) {
	if (pad < 0 || pad >= TAPE_PAD_COUNT || group < -TAPE_CHOKE_MAX || group > TAPE_CHOKE_MAX) return false;
	pthread_mutex_lock(&worker_lock);
	finish_locked();
	slots[pad].choke = (int8_t)group;
	publish_slots();
	pthread_mutex_unlock(&worker_lock);
	return true;
}

int tapepads_choke_group(int pad) {
	if (pad < 0 || pad >= TAPE_PAD_COUNT) return 0;
	pthread_mutex_lock(&worker_lock);
	int group = slots[pad].choke;
	pthread_mutex_unlock(&worker_lock);
	return group;
}

bool tapepads_set_choke(int pad, int group) { return tapepads_set_choke_group(pad, group); }
int tapepads_choke(int pad) { return tapepads_choke_group(pad); }

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
	tape_wav_resample_cache_t *cache;
	tape_wav_t wav;
	int fd;
	uint32_t advance;
	uint32_t start, end, drum_end;
	uint64_t pos;
	tape_interp_t interpolation;
	bool on;
} voice_t;

static bool open_sample(voice_t *voice, const pad_slot_t *slot) {
	voice->fd = tape_open_at(job_card, slot->path, false);
	if (voice->fd < 0 || !tape_wav_probe(voice->fd, &voice->wav)) return false;
	voice->cache = calloc(1, sizeof(*voice->cache));
	if (!voice->cache) return false;
	uint32_t pitch = slot_pitch(slot);
	voice->advance = (uint32_t)(((uint64_t)voice->wav.rate * pitch + TAPE_PAD_RATE / 2) / TAPE_PAD_RATE);
	if (!voice->advance) voice->advance = 1;
	voice->interpolation = slot_interp(slot);
	voice->start = slot->trim_start;
	voice->end = slot_end(slot);
	if (!voice->end || voice->end > voice->wav.frames) voice->end = voice->wav.frames;
	if (voice->start >= voice->end) { voice->start = 0; voice->end = voice->wav.frames; }
	voice->drum = NULL;
	return true;
}

static unsigned choke_groups(unsigned mask) {
	unsigned groups = 0;
	for (int p = 0; p < TAPE_PAD_COUNT; p++) {
		if ((mask & (1u << p)) && slots[p].choke > 0) groups |= 1u << (slots[p].choke - 1);
	}
	return groups;
}

// A positive group is a cutter (closed hi-hat). Negative pads are receivers
// (open hi-hat): triggering one never cuts the cutter, so the choke is one-way.
// `extra` carries cutters from manual taps earlier in the same mix block, and
// `protected_hits` keeps those newly started cutters from being cut again.
// When a step contains both, cutters win; if several cutters share a group,
// the highest-numbered pad in that trigger is the one that remains.
static unsigned prepare_trigger(voice_t voices[TAPE_PAD_COUNT], unsigned mask,
	unsigned extra, unsigned protected_hits) {
	unsigned groups = choke_groups(mask) | extra;
	if (!groups) return mask;
	for (int p = 0; p < TAPE_PAD_COUNT; p++) {
		if (protected_hits & (1u << p)) continue;
		int group = slots[p].choke;
		if (group < 0) group = -group;
		if (group && (groups & (1u << (group - 1)))) voices[p].on = false;
	}
	unsigned allowed = mask;
	for (int p = 0; p < TAPE_PAD_COUNT; p++) {
		if (!(mask & (1u << p))) continue;
		int group = slots[p].choke;
		if (group < 0) {
			if (groups & (1u << (-group - 1))) allowed &= ~(1u << p);
		} else if (group > 0) {
			bool cut = (extra & (1u << (group - 1))) != 0;
			for (int q = p + 1; q < TAPE_PAD_COUNT && !cut; q++) {
				if ((mask & (1u << q)) && slots[q].choke == group) cut = true;
			}
			if (cut) allowed &= ~(1u << p);
		}
	}
	return allowed;
}

static bool trigger(voice_t voices[TAPE_PAD_COUNT], unsigned hits, bool manual,
	unsigned *recordable, unsigned extra, unsigned protected_hits) {
	unsigned mask = prepare_trigger(voices, hits, extra, protected_hits);
	for (int p = 0; p < TAPE_PAD_COUNT; p++) {
		if (!(mask & (1u << p))) continue;
		voice_t *v = &voices[p];
		const pad_slot_t *s = &slots[p];
		if (s->kind == TAPE_PAD_EMPTY) continue;
		if (s->kind == TAPE_PAD_WAV) {
			if (!manual && s->long_clip) continue; // a backing track is not a drum hit
			if (v->fd < 0 && !open_sample(v, s)) return false;
			if (s->long_clip) {
				if (!manual) continue;
				bool want = !v->on;
				for (int k = 0; k < TAPE_PAD_COUNT; k++) {
					if (voices[k].fd >= 0 && slots[k].long_clip) voices[k].on = false;
				}
				if (!want) continue;
			} else if (recordable) *recordable |= 1u << p;
			v->drum = NULL;
			v->interpolation = slot_interp(s);
			v->pos = (uint64_t)v->start << 16;
		} else {
			v->drum = factory_drum(preset_of(s, p));
			v->advance = slot_pitch(s);
			v->interpolation = slot_interp(s);
			v->drum_end = s->trim_end && s->trim_end < v->drum->frames ? s->trim_end : v->drum->frames;
			v->start = s->trim_start;
			if (v->start >= v->drum_end) { v->start = 0; v->drum_end = v->drum->frames; }
			v->pos = (uint64_t)v->start << 16;
			if (recordable) *recordable |= 1u << p;
		}
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
				uint64_t end = (uint64_t)v->drum_end << 16;
				uint32_t frame = (uint32_t)(v->pos >> 16), last = v->drum_end - 1;
				uint32_t at[4] = {frame ? frame - 1 : 0, frame,
					frame < last ? frame + 1 : last, frame + 1 < last ? frame + 2 : last};
				if (at[0] > last) at[0] = last;
				if (at[1] > last) at[1] = last;
				int32_t p[4] = {v->drum->pcm[at[0]], v->drum->pcm[at[1]],
					v->drum->pcm[at[2]], v->drum->pcm[at[3]]};
				left = right = tape_interpolate(p[0], p[1], p[2], p[3],
					(uint16_t)(v->pos & 0xffffu), v->interpolation);
				v->pos += v->advance;
				v->on = v->pos < end;
			} else {
				if (!tape_wav_resample(v->fd, &v->wav, v->cache, v->pos, v->end,
					v->interpolation, &left, &right)) return false;
				v->pos += v->advance;
				v->on = v->pos < ((uint64_t)v->end << 16);
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
	// input change, like Play or starting a track.
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
		unsigned performed = 0, manual_groups = choke_groups(hits);
		if (!trigger(voices, hits, true, &performed, 0, 0)) goto bad_sample;
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
					if (!trigger(voices, pattern[current] & ~(at == 0 ? hits : 0u), false, NULL,
						manual_groups, hits)) goto bad_sample;
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
