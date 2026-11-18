#include "src/system/audio/audio.h"
#include "src/system/audio/tapepads.h"
#include "src/system/device/system.h"
#include "src/system/device/usb.h"

#include <assert.h>
#include <fcntl.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define BLOCK (TAPE_PAD_RATE / 100)
#define BAR (TAPE_PAD_RATE * 2) // 120 BPM
static int16_t pcm[2 * (2 * BAR + BLOCK)];
static int remaining, received, begins, ends, local_stops, card_opens;
static bool other_owner, local_playing, dac, fail_open, fail_write, have_card = true;
static atomic_bool keep_writing, wrote_audio;
static const void *owner;
static void (*action)(int frames);
static int last_card_fd = -1;

int storage_card_file_job_begin(bool writable) {
	assert(!writable);
	if (!have_card) return -1;
	last_card_fd = open(".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	assert(last_card_fd >= 0);
	card_opens++;
	return last_card_fd;
}
void storage_card_file_job_end(void) {}
bool usb_dac_mode(void) { return dac; }
bool audio_external_is_active(void) { return other_owner; }
bool audio_playback_context_active(void) { return local_playing; }
audio_status_t audio_get_status(void) { return local_playing ? AUDIO_STATUS_PLAYING : AUDIO_STATUS_STOPPED; }
void audio_stop_and_wait(int timeout) { assert(timeout > 0); local_playing = false; local_stops++; }
bool audio_external_begin_owned(const void *token, int rate, int channels, int bits, int latency) {
	assert(token && !owner && !local_playing && !other_owner && !dac);
	assert(rate == TAPE_PAD_RATE && channels == 2 && bits == 16 && latency > 0);
	if (fail_open) return false;
	owner = token;
	begins++;
	return true;
}
int audio_external_write_owned(const void *token, const void *data, int frames) {
	assert(token && token == owner && frames == BLOCK);
	if (fail_write) return 0;
	if (keep_writing) {
		wrote_audio = true;
		usleep(1000);
		return frames;
	}
	assert(2 * (received + frames) <= (int)(sizeof(pcm) / sizeof(*pcm)));
	memcpy(pcm + 2 * received, data, (size_t)frames * 2 * sizeof(*pcm));
	received += frames;
	if (action) action(received);
	// A fake PCM accepts frames immediately; stop after the specified span.
	remaining -= frames;
	if (remaining <= 0) tapepads_stop();
	return frames;
}
void audio_external_end_owned(const void *token) {
	assert(token && token == owner);
	owner = NULL;
	ends++;
}

static void wait_finished(tape_pads_state_t expected) {
	for (int i = 0; tapepads_active() && i < 1000; i++) usleep(2000);
	assert(tapepads_state() == expected);
	tapepads_finish();
	assert(!owner && tapepads_step() == -1 && tapepads_sounding() == 0 && begins == ends);
}

static void run(int pad, int frames, tape_pads_state_t expected) {
	assert(!tapepads_active());
	remaining = frames;
	received = 0;
	assert(pad < 0 ? tapepads_start() : tapepads_hit(pad));
	wait_finished(expected);
}

static bool nonzero(int first, int end) {
	assert(first <= end && end <= received);
	for (int i = first * 2; i < end * 2; i++) if (pcm[i]) return true;
	return false;
}

static void put_num(FILE *f, unsigned n, unsigned bytes) {
	for (unsigned i = 0; i < bytes; i++) assert(fputc((n >> (8 * i)) & 255, f) != EOF);
}

static void put_str(FILE *f, const char *s) { assert(fwrite(s, 1, 4, f) == 4); }

// Real RIFF files with an odd padded metadata chunk, optional extensible
// format, known signed values and useful stereo data across cache boundaries.
static void wav(const char *file, unsigned rate, unsigned bits, unsigned channels, unsigned frames, bool ext) {
	FILE *f = fopen(file, "wb");
	assert(f);
	unsigned bytes = bits / 8, stride = bytes * channels, data = frames * stride, fmtsize = ext ? 40 : 16;
	put_str(f, "RIFF"); put_num(f, 30 + fmtsize + data + (data & 1u), 4);
	put_str(f, "WAVE");
	put_str(f, "JUNK"); put_num(f, 1, 4); put_num(f, 0, 2);
	put_str(f, "fmt "); put_num(f, fmtsize, 4);
	put_num(f, ext ? 0xfffe : 1, 2); put_num(f, channels, 2); put_num(f, rate, 4);
	put_num(f, rate * stride, 4); put_num(f, stride, 2); put_num(f, bits, 2);
	if (ext) {
		static const uint8_t guid[] = {1,0,0,0,0,0,0x10,0,0x80,0,0,0xaa,0,0x38,0x9b,0x71};
		put_num(f, 22, 2); put_num(f, bits, 2); put_num(f, 3, 4);
		assert(fwrite(guid, 1, sizeof(guid), f) == sizeof(guid));
	}
	put_str(f, "data"); put_num(f, data, 4);
	for (unsigned i = 0; i < frames; i++) {
		int v = 2048 + (int)(i % 8) * 1024;
		for (unsigned ch = 0; ch < channels; ch++) {
			unsigned word = (uint16_t)(ch == 0 ? v : -v);
			put_num(f, bits == 8 ? (word >> 8) ^ 128u : word << (bits - 16), bytes);
		}
	}
	if (data & 1) put_num(f, 0, 1);
	assert(fclose(f) == 0);
}

static void overdub_once(int frames) {
	if (frames == 55 * BLOCK) {
		assert(tapepads_looping() && tapepads_step() == 4);
		tapepads_set_overdub(true);
		assert(tapepads_hit(12));
	}
}

static void layer_over_vocal(int frames) {
	if (frames == 8 * BLOCK) {
		assert(tapepads_sounding() & (1u << 15));
		assert(tapepads_hit(0));
	}
	if (frames == 12 * BLOCK) assert(tapepads_hit(15)); // cut it, drum keeps playing
}

static void test_factory(void) {
	have_card = false;
	assert(TAPE_PAD_COUNT == 16 && tapepads_bpm() == 120);
	assert(!tapepads_hit(-1) && !tapepads_hit(16));
	tapepads_set_bpm(-100);
	assert(tapepads_bpm() == TAPE_PAD_MIN_BPM);
	tapepads_set_bpm(1000);
	assert(tapepads_bpm() == TAPE_PAD_MAX_BPM);
	tapepads_set_bpm(120);
	other_owner = local_playing = true;
	run(0, BLOCK, TAPE_PADS_UNAVAILABLE);
	assert(begins == 0 && local_stops == 0 && local_playing);
	other_owner = false;
	dac = true;
	run(-1, BLOCK, TAPE_PADS_UNAVAILABLE);
	assert(begins == 0 && local_stops == 0);
	dac = false;

	uint32_t digest[16];
	for (int p = 0; p < TAPE_PAD_COUNT; p++) {
		run(p, BLOCK, TAPE_PADS_IDLE);
		assert(!tapepads_looping() && received == BLOCK && nonzero(0, BLOCK));
		unsigned hash = 2166136261u;
		for (int i = 0; i < received; i++) {
			assert(pcm[2 * i] == pcm[2 * i + 1]);
			hash = (hash ^ (uint16_t)pcm[2 * i]) * 16777619u;
		}
		for (int i = 0; i < p; i++) assert(digest[i] != hash);
		digest[p] = hash;
	}
	assert(!card_opens && local_stops == 1);
	tapepads_set_preset(15, 0); // moving a preset does not change its tone
	run(15, BLOCK, TAPE_PADS_IDLE);
	unsigned hash = 2166136261u;
	for (int i = 0; i < received; i++) hash = (hash ^ (uint16_t)pcm[2 * i]) * 16777619u;
	assert(hash == digest[0]);

	// Hat-only demo, bar and step boundaries are sample-clock exact.
	tapepads_set_preset(0, -1);
	tapepads_set_preset(4, -1);
	assert(!tapepads_hit(0));
	run(-1, 2 * BAR, TAPE_PADS_IDLE);
	assert(received == 2 * BAR && nonzero(0, BLOCK));
	assert(!memcmp(pcm, pcm + 2 * BAR, (size_t)2 * BAR * sizeof(*pcm)));
	assert(!memcmp(pcm, pcm + BAR / 4, (size_t)BAR / 4 * sizeof(*pcm)));

	// A manual clap around 0.55 s is heard immediately and at step 4 in the
	// next bar (0.5 seconds into it), with no extra factory hits.
	tapepads_pattern(false);
	action = overdub_once;
	run(-1, 2 * BAR, TAPE_PADS_IDLE);
	action = NULL;
	assert(!nonzero(0, 55 * BLOCK) && nonzero(55 * BLOCK, 58 * BLOCK));
	assert(!nonzero(BAR, BAR + 50 * BLOCK) && nonzero(BAR + 50 * BLOCK, BAR + 52 * BLOCK));
	run(-1, 60 * BLOCK, TAPE_PADS_IDLE);
	assert(!nonzero(0, 50 * BLOCK) && nonzero(50 * BLOCK, 52 * BLOCK));
	tapepads_pattern(true);
	tapepads_factory();
	have_card = true;
}

static void test_card(void) {
	assert(mkdir(TAPE_KIT_DIR, 0700) == 0);
	assert(mkdir(TAPE_KIT_DIR "/House", 0700) == 0 && mkdir(TAPE_KIT_DIR "/Sort", 0700) == 0);
	wav(TAPE_KIT_DIR "/House/01 Kick.wav", TAPE_PAD_RATE, 16, 1, BLOCK * 7, false);
	wav(TAPE_KIT_DIR "/House/16-Vox.WAV", 22050, 24, 2, 22050 * 10, true);
	wav(TAPE_KIT_DIR "/Sort/Z.wav", 48000, 32, 2, 2000, false);
	wav(TAPE_KIT_DIR "/Sort/A.wav", 8000, 8, 1, 601, false);
	assert(symlink(TAPE_KIT_DIR "/House/16-Vox.WAV", "link.wav") == 0);
	assert(symlink(TAPE_KIT_DIR "/House", "LinkedKit") == 0);
	FILE *bad = fopen("bad.wav", "wb");
	assert(bad && fputs("not a RIFF file", bad) >= 0 && fclose(bad) == 0);
	assert(!tapepads_assign(0, "bad.wav") && !tapepads_assign(0, "link.wav"));
	assert(!tapepads_assign(0, "LinkedKit/16-Vox.WAV") && !tapepads_assign(0, "../outside.wav"));
	assert(tapepads_load_kit("../House") < 0 && tapepads_load_kit("Missing") < 0);
	tape_file_t entries[3];
	assert(tape_card_list("", false, NULL, entries, 3) == 2);
	assert(entries[0].directory && !strcmp(entries[0].name, TAPE_KIT_DIR));
	assert(!entries[1].directory && !strcmp(entries[1].name, "bad.wav"));
	assert(tape_card_list(TAPE_KIT_DIR, true, NULL, entries, 1) == 1 && !strcmp(entries[0].name, "House"));
	tape_file_t after = entries[0];
	assert(tape_card_list(TAPE_KIT_DIR, true, &after, entries, 1) == 1 && !strcmp(entries[0].name, "Sort"));

	tape_pad_info_t info[TAPE_PAD_COUNT];
	assert(tapepads_load_kit("Sort") == 2);
	tapepads_info(info);
	assert(!strcmp(info[0].name, "A.wav") && !strcmp(info[1].name, "Z.wav"));
	run(0, BLOCK, TAPE_PADS_IDLE);
	assert(pcm[0] == 1536 && pcm[1] == 1536);
	assert(tapepads_load_kit("House") == 2);
	tapepads_info(info);
	assert(info[0].kind == TAPE_PAD_WAV && !info[0].long_clip);
	assert(info[1].kind == TAPE_PAD_EMPTY && !tapepads_hit(1));
	assert(info[15].kind == TAPE_PAD_WAV && info[15].long_clip && !strcmp(info[15].name, "16-Vox.WAV"));
	assert(tapepads_assign(7, TAPE_KIT_DIR "/Sort/Z.wav"));
	tapepads_info(info);
	assert(info[7].kind == TAPE_PAD_WAV && !strcmp(info[7].name, "Z.wav"));

	action = layer_over_vocal;
	run(15, 22 * BLOCK, TAPE_PADS_IDLE);
	action = NULL;
	assert(pcm[0] == 1536 && pcm[1] == -1536 && pcm[2] == 1920 && pcm[3] == -1920);
	// Verify read + interpolation across the first cache edge, not just for
	// the samples a speculative loader might have prefetched.
	int edge = 2 * TAPE_WAV_CACHE_FRAMES - 1;
	// Here the layer script already cut the backing track; sample from a
	// separate continuous run over the boundary below.
	assert(!nonzero(18 * BLOCK, 22 * BLOCK));
	assert(pcm[16 * BLOCK] > 1536 && pcm[16 * BLOCK + 1] <= 0); // two mixed voices
	run(15, 12 * BLOCK, TAPE_PADS_IDLE);
	assert(pcm[2 * edge] == 4224 && pcm[2 * edge + 1] == -4224);
	assert(pcm[2 * (edge + 1)] == 1536 && pcm[2 * (edge + 1) + 1] == -1536);

	// Long clips never fire from the sequencer, even if a pad with a demo
	// hit gets reassigned to one. Ordinary drums still work with it mapped.
	tapepads_set_preset(0, -1);
	assert(tapepads_assign(0, TAPE_KIT_DIR "/House/16-Vox.WAV"));
	run(-1, BAR, TAPE_PADS_IDLE);
	assert(!nonzero(0, BAR));

	keep_writing = true;
	wrote_audio = false;
	assert(tapepads_hit(15));
	for (int i = 0; i < 1000 && !wrote_audio; i++) usleep(1000);
	assert(wrote_audio && tapepads_active());
	int fd = last_card_fd;
	assert(fcntl(fd, F_GETFD) >= 0);
	tapepads_release_card();
	assert(!tapepads_active() && fcntl(fd, F_GETFD) == -1);
	keep_writing = false;
	tapepads_info(info);
	assert(info[15].kind == TAPE_PAD_FACTORY && info[15].preset == 15);
	have_card = false;
	run(15, BLOCK, TAPE_PADS_IDLE);
	have_card = true;

	assert(unlink("bad.wav") == 0 && unlink("link.wav") == 0 && unlink("LinkedKit") == 0);
	assert(unlink(TAPE_KIT_DIR "/House/01 Kick.wav") == 0 && unlink(TAPE_KIT_DIR "/House/16-Vox.WAV") == 0);
	assert(unlink(TAPE_KIT_DIR "/Sort/A.wav") == 0 && unlink(TAPE_KIT_DIR "/Sort/Z.wav") == 0);
	assert(rmdir(TAPE_KIT_DIR "/House") == 0 && rmdir(TAPE_KIT_DIR "/Sort") == 0 && rmdir(TAPE_KIT_DIR) == 0);
}

int main(void) {
	char temp[] = "/tmp/sonix-sampler-XXXXXX";
	assert(mkdtemp(temp) && chdir(temp) == 0);
	test_factory();
	test_card();
	fail_open = true;
	run(1, BLOCK, TAPE_PADS_UNAVAILABLE);
	fail_open = false;
	fail_write = true;
	run(1, BLOCK, TAPE_PADS_UNAVAILABLE);
	fail_write = false;
	run(1, BLOCK, TAPE_PADS_IDLE);
	assert(chdir("/") == 0 && rmdir(temp) == 0);
	puts("Tape sampler: 16 sounds, manual taps, overdub, kits, stereo WAV streaming and card release OK");
	return 0;
}
