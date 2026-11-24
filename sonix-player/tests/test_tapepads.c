#include "src/system/audio/audio.h"
#include "src/system/audio/tapepads.h"
#include "src/system/device/system.h"
#include "src/system/device/usb.h"

#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <fcntl.h>
#include <math.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define BLOCK (TAPE_PAD_RATE / 100)
#define BAR (TAPE_PAD_RATE * 2) // 120 BPM
static int16_t pcm[2 * (2 * BAR + BLOCK)];
static int remaining, received, begins, ends, local_stops, card_opens;
static bool other_owner, local_playing, dac, fail_open, fail_write, have_card = true;
static bool writable_card = true, paced_pcm;
static atomic_bool keep_writing, wrote_audio, fail_sync, fail_payload, stall_disk, writer_stalled;
static char card_root[] = "/tmp/sonix-sampler-XXXXXX";
static const void *owner;
static void (*action)(int frames);
static int last_card_fd = -1;

time_t __wrap_time(time_t *out) {
	const time_t t = 1795003200;
	if (out) *out = t;
	return t;
}
int __real_fsync(int fd);
int __wrap_fsync(int fd) {
	if (fail_sync) { errno = ENOSPC; return -1; }
	return __real_fsync(fd);
}
ssize_t __real_write(int fd, const void *p, size_t size);
ssize_t __wrap_write(int fd, const void *p, size_t size) {
	if (size > 44 && fail_payload) { errno = ENOSPC; return -1; }
	if (size > 44 && stall_disk) {
		writer_stalled = true;
		while (stall_disk) usleep(1000);
	}
	return __real_write(fd, p, size);
}

const char *storage_sd_root(void) { return card_root; }
int storage_card_file_job_begin(bool writable) {
	if (!have_card || (writable && !writable_card)) return -1;
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
	if (paced_pcm) usleep(1000);
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

// A clean sine makes pitch and choke assertions possible without an FFT.
static void sine_wav(const char *file, unsigned rate, unsigned freq, unsigned frames) {
	FILE *f = fopen(file, "wb");
	assert(f);
	unsigned data = frames * 2;
	put_str(f, "RIFF"); put_num(f, 36 + data, 4);
	put_str(f, "WAVE");
	put_str(f, "fmt "); put_num(f, 16, 4);
	put_num(f, 1, 2); put_num(f, 1, 2); put_num(f, rate, 4);
	put_num(f, rate * 2, 4); put_num(f, 2, 2); put_num(f, 16, 2);
	put_str(f, "data"); put_num(f, data, 4);
	for (unsigned i = 0; i < frames; i++) {
		double phase = 6.283185307179586 * (double)freq * i / rate;
		put_num(f, (uint16_t)(int16_t)lrint(12000.0 * sin(phase)), 2);
	}
	assert(fclose(f) == 0);
}

static unsigned rising_edges(int first, int end) {
	assert(first >= 0 && end <= received);
	bool positive = pcm[2 * first] > 0;
	unsigned edges = 0;
	for (int i = first + 1; i < end; i++) {
		bool now = pcm[2 * i] > 0;
		if (now && !positive) edges++;
		positive = now;
	}
	return edges;
}

static void cut_open_hat(int frames) {
	if (frames == 3 * BLOCK) assert(tapepads_hit(1));
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

static void remove_saves(tape_store_kind_t kind) {
	const char *dir_name = kind == TAPE_STORE_SEQUENCE ? "Sequences" : "Recordings";
	DIR *dir = opendir(dir_name);
	assert(dir);
	struct dirent *d;
	while ((d = readdir(dir))) {
		if (!strcmp(d->d_name, ".") || !strcmp(d->d_name, "..")) continue;
		assert(d->d_name[0] != '.'); // no abandoned parts after clean stop/failure
		assert(unlinkat(dirfd(dir), d->d_name, 0) == 0);
	}
	assert(closedir(dir) == 0 && rmdir(dir_name) == 0);
}

static const char sample[] = TAPE_KIT_DIR "/House/01 Kick.wav";
static const char backing[] = TAPE_KIT_DIR "/House/16-Vox.WAV";

static void test_sequences(void) {
	assert(tapepads_save_sequence() == TAPE_SEQ_OK); // factory demo
	tapepads_pattern(false);
	assert(tapepads_assign(0, sample) && tapepads_assign(15, backing));
	tapepads_set_preset(1, -1);
	tapepads_set_preset(12, 5);
	action = overdub_once; // record pad 13 at step 4, then change tempo
	run(-1, 60 * BLOCK, TAPE_PADS_IDLE);
	action = NULL;
	tapepads_set_bpm(150);
	assert(tapepads_save_sequence() == TAPE_SEQ_OK);
	char saved[3][TAPE_SAVE_NAME_SIZE];
	assert(tape_store_list(TAPE_STORE_SEQUENCE, NULL, saved, 3) == 2);
	assert(strstr(saved[0], "-002.seq") && strstr(saved[1], "-001.seq"));
	char older[1][TAPE_SAVE_NAME_SIZE];
	assert(tape_store_list(TAPE_STORE_SEQUENCE, saved[0], older, 1) == 1 && !strcmp(older[0], saved[1]));

	// Simulate the live state being gone. The file references both WAVs,
	// restores factory/empty assignments and retains the quantized hit.
	tapepads_release_card();
	tapepads_pattern(true);
	tapepads_set_bpm(90);
	assert(tapepads_load_sequence(saved[0]) == TAPE_SEQ_OK && tapepads_bpm() == 150);
	tape_pad_info_t info[TAPE_PAD_COUNT];
	tapepads_info(info);
	assert(info[0].kind == TAPE_PAD_WAV && !strcmp(info[0].name, "01 Kick.wav"));
	assert(info[1].kind == TAPE_PAD_EMPTY && info[12].kind == TAPE_PAD_FACTORY && info[12].preset == 5);
	assert(info[15].long_clip && !strcmp(info[15].name, "16-Vox.WAV"));
	run(-1, 70 * BLOCK, TAPE_PADS_IDLE);
	assert(!nonzero(0, 40 * BLOCK) && nonzero(40 * BLOCK, 42 * BLOCK));

	// Missing samples, unsupported versions and unsafe paths must not
	// replace the current mapping or even part of the current pattern.
	const unsigned rev = tapepads_revision();
	assert(rename(backing, "moved.wav") == 0);
	assert(tapepads_load_sequence(saved[0]) == TAPE_SEQ_SAMPLE_MISSING);
	assert(symlink("../../moved.wav", backing) == 0);
	assert(tapepads_load_sequence(saved[0]) == TAPE_SEQ_SAMPLE_MISSING);
	assert(unlink(backing) == 0 && rename("moved.wav", backing) == 0);
	assert(tapepads_load_sequence("../outside.seq") == TAPE_SEQ_INVALID);
	char target[sizeof("Sequences/") + TAPE_SAVE_NAME_SIZE];
	snprintf(target, sizeof(target), "Sequences/%s", saved[1]);
	int fd = open(target, O_WRONLY | O_CLOEXEC);
	assert(fd >= 0 && pwrite(fd, "\x02", 1, 6) == 1 && close(fd) == 0);
	assert(tapepads_load_sequence(saved[1]) == TAPE_SEQ_INVALID);
	assert(tapepads_revision() == rev && tapepads_bpm() == 150);
	run(-1, 60 * BLOCK, TAPE_PADS_IDLE);
	assert(!nonzero(0, 40 * BLOCK) && nonzero(40 * BLOCK, 42 * BLOCK));

	have_card = false;
	assert(tapepads_load_sequence(saved[0]) == TAPE_SEQ_NO_CARD);
	assert(tapepads_save_sequence() == TAPE_SEQ_NO_CARD);
	have_card = true;
	writable_card = false;
	assert(tapepads_save_sequence() == TAPE_SEQ_NO_CARD && tapepads_load_sequence(saved[0]) == TAPE_SEQ_OK);
	writable_card = true;
	fail_sync = true;
	assert(tapepads_save_sequence() == TAPE_SEQ_SAVE_ERROR);
	fail_sync = false;
	assert(rename("Sequences", "Relocated") == 0 && symlink("Relocated", "Sequences") == 0);
	assert(tapepads_save_sequence() == TAPE_SEQ_SAVE_ERROR && tapepads_load_sequence(saved[0]) == TAPE_SEQ_INVALID);
	assert(unlink("Sequences") == 0 && rename("Relocated", "Sequences") == 0);
	assert(tape_store_list(TAPE_STORE_SEQUENCE, NULL, saved, 3) == 2);
	remove_saves(TAPE_STORE_SEQUENCE);
	tapepads_factory();
	tapepads_pattern(true);
	tapepads_set_bpm(120);
}

static unsigned peak(void) {
	unsigned high = 0;
	for (int i = 0; i < received * 2; i++) {
		unsigned v = (unsigned)abs(pcm[i]);
		if (v > high) high = v;
	}
	return high;
}

static void test_fx(void) {
	// The 55 ms hat dies in the first 6 blocks; only the Echo switch
	// can make it audible a quarter-second later.
	run(8, 38 * BLOCK, TAPE_PADS_IDLE);
	assert(nonzero(0, 6 * BLOCK) && !nonzero(25 * BLOCK, 32 * BLOCK));
	unsigned dry = peak();
	tapepads_toggle_effect(TAPE_FX_FILTER);
	run(8, 38 * BLOCK, TAPE_PADS_IDLE);
	assert(nonzero(0, 6 * BLOCK) && peak() < dry);
	tapepads_toggle_effect(TAPE_FX_FILTER);
	tapepads_toggle_effect(TAPE_FX_ECHO);
	run(8, 38 * BLOCK, TAPE_PADS_IDLE);
	assert(!nonzero(7 * BLOCK, 25 * BLOCK) && nonzero(25 * BLOCK, 32 * BLOCK));
	tapepads_toggle_effect(TAPE_FX_ECHO);
	tapepads_toggle_effect(TAPE_FX_DRIVE);
	run(8, 38 * BLOCK, TAPE_PADS_IDLE);
	assert(peak() > dry);
	tapepads_toggle_effect(TAPE_FX_DRIVE);
	assert(tapepads_effects() == 0);
}

static tape_mix_info_t recorded(tape_mix_state_t expected) {
	for (int i = 0; tapemix_active() && i < 2000; i++) usleep(1000);
	tape_mix_info_t info;
	tapemix_info(&info);
	assert(info.state == expected);
	return info;
}

static void verify_take(tape_mix_info_t take, uint32_t frames, bool compare) {
	assert(take.frames == frames && take.name[0]);
	char path[PATH_MAX];
	assert(tape_store_take_path(take.name, path, sizeof(path)));
	char tiny[1];
	assert(!tape_store_take_path(take.name, tiny, sizeof(tiny)));
	int fd = open(path, O_RDONLY | O_CLOEXEC);
	assert(fd >= 0);
	tape_wav_t wav;
	struct stat st;
	assert(tape_wav_probe(fd, &wav) && fstat(fd, &st) == 0);
	assert(wav.frames == frames && wav.rate == TAPE_PAD_RATE && wav.channels == 2 && wav.bytes_per_sample == 2);
	assert(st.st_size == wav.offset + (off_t)frames * 4);
	if (compare) {
		assert(frames <= (unsigned)received);
		for (uint32_t at = 0; at < frames; at += BLOCK) {
			int16_t block[2 * BLOCK];
			size_t size = (frames - at < BLOCK ? frames - at : BLOCK) * 4;
			assert(pread(fd, block, size, wav.offset + (off_t)at * 4) == (ssize_t)size);
			assert(!memcmp(block, pcm + at * 2, size)); // exact post-FX monitor stream
		}
	}
	assert(close(fd) == 0);
}

static void live_take(int frames) {
	if (frames == 1 * BLOCK) assert(tapepads_start());
	if (frames == 10 * BLOCK) assert(tapepads_hit(15)); // add long stereo backing
	if (frames == 20 * BLOCK) tapepads_toggle_effect(TAPE_FX_FILTER);
	if (frames == 30 * BLOCK) {
		assert(tapepads_hit(12));
		tapepads_toggle_effect(TAPE_FX_ECHO);
	}
	if (frames == 40 * BLOCK) {
		assert(tapepads_sounding() & (1u << 15));
		tapepads_toggle_effect(TAPE_FX_DRIVE);
	}
	if (frames == 50 * BLOCK) tapepads_stop_loop();
	if (frames == 60 * BLOCK) {
		assert(tapepads_step() == -1 && (tapepads_sounding() & (1u << 15)));
		assert(tapepads_start());
	}
	if (frames == 75 * BLOCK) tapemix_stop();
	if (frames == 78 * BLOCK) assert(tapepads_looping() && tapepads_step() >= 0);
}

static void record_blocks(int blocks, void (*script)(int), tape_pads_state_t expected) {
	remaining = blocks * BLOCK;
	received = 0;
	action = script;
	paced_pcm = true;
	assert(tapepads_record() == TAPE_MIX_OPENING);
	wait_finished(expected);
	paced_pcm = false;
	action = NULL;
}

static void lose_output(int frames) {
	if (frames == 3 * BLOCK) fail_write = true;
}

static void test_takes(void) {
	have_card = false;
	assert(tapepads_record() == TAPE_MIX_NO_CARD && !tapepads_active());
	have_card = true;
	writable_card = false;
	assert(tapepads_record() == TAPE_MIX_NO_CARD && !tapepads_active());
	writable_card = true;
	assert(tape_store_list(TAPE_STORE_TAKE, NULL, (char [1][TAPE_SAVE_NAME_SIZE]){{0}}, 1) == 0);

	assert(tapepads_assign(15, backing));
	int n = begins;
	record_blocks(85, live_take, TAPE_PADS_IDLE);
	tape_mix_info_t first_take = recorded(TAPE_MIX_SAVED);
	verify_take(first_take, 74 * BLOCK, true);
	assert(begins == n + 1 && !nonzero(0, BLOCK) && nonzero(BLOCK, 9 * BLOCK));
	assert(pcm[20 * BLOCK] != pcm[20 * BLOCK + 1]); // stereo intact
	assert(tapepads_effects() == (TAPE_FX_FILTER | TAPE_FX_ECHO | TAPE_FX_DRIVE));
	tapepads_toggle_effect(TAPE_FX_FILTER);
	tapepads_toggle_effect(TAPE_FX_ECHO);
	tapepads_toggle_effect(TAPE_FX_DRIVE);
	tapepads_factory();

	// Capture while the factory-only loop is already running. Card export
	// must join the writer, but need not disturb a card-independent loop.
	keep_writing = true;
	wrote_audio = false;
	assert(tapepads_start());
	for (int i = 0; i < 1000 && !wrote_audio; i++) usleep(1000);
	assert(wrote_audio);
	n = begins;
	assert(tapepads_record() == TAPE_MIX_OPENING && tapepads_record() == TAPE_MIX_BUSY);
	tape_mix_info_t info;
	for (int i = 0; i < 1000; i++) {
		tapemix_info(&info);
		if (info.frames >= 3 * BLOCK) break;
		usleep(1000);
	}
	assert(info.frames >= 3 * BLOCK);
	tapepads_release_card();
	info = recorded(TAPE_MIX_SAVED);
	assert(info.frames >= 3 * BLOCK && tapepads_looping() && begins == n);
	verify_take(info, info.frames, false);
	assert(strcmp(info.name, first_take.name) != 0); // same second, new file
	tapepads_stop();
	wait_finished(TAPE_PADS_IDLE);
	keep_writing = false;

	// Cap finalizes automatically and consumes exactly the advertised number
	// of frames. This also drives multiple laps of the writer's ring buffer.
	record_blocks(205, NULL, TAPE_PADS_IDLE);
	verify_take(recorded(TAPE_MIX_SAVED), 2 * TAPE_PAD_RATE, true);

	// Output disappearing doesn't publish a bogus empty file or throw away
	// the accepted audio. A write/fsync failure publishes nothing instead.
	record_blocks(25, lose_output, TAPE_PADS_UNAVAILABLE);
	fail_write = false;
	verify_take(recorded(TAPE_MIX_PARTIAL), 3 * BLOCK, true);
	fail_payload = true;
	record_blocks(25, NULL, TAPE_PADS_IDLE);
	recorded(TAPE_MIX_ERROR);
	fail_payload = false;
	fail_sync = true;
	record_blocks(25, NULL, TAPE_PADS_IDLE);
	recorded(TAPE_MIX_ERROR);
	fail_sync = false;
	other_owner = true;
	record_blocks(25, NULL, TAPE_PADS_UNAVAILABLE);
	recorded(TAPE_MIX_ERROR);
	other_owner = false;

	// Stall the card longer than the entire capture queue. Playback must
	// still run; publish a gap-free prefix instead of skipping blocks.
	stall_disk = true;
	writer_stalled = false;
	keep_writing = true;
	assert(tapepads_start() && tapepads_record() == TAPE_MIX_OPENING);
	for (int i = 0; i < 2000; i++) {
		tapemix_info(&info);
		if (info.state == TAPE_MIX_SAVING) break;
		usleep(1000);
	}
	assert(writer_stalled && info.state == TAPE_MIX_SAVING && tapepads_looping());
	assert(info.frames >= TAPE_PAD_RATE && info.frames < 2 * TAPE_PAD_RATE);
	stall_disk = false;
	verify_take(recorded(TAPE_MIX_PARTIAL), info.frames, false);
	assert(tapepads_looping());
	tapepads_stop();
	wait_finished(TAPE_PADS_IDLE);
	keep_writing = false;
	char names[6][TAPE_SAVE_NAME_SIZE];
	assert(tape_store_list(TAPE_STORE_TAKE, NULL, names, 6) == 5);
	char path[PATH_MAX];
	assert(!tape_store_take_path("../other.wav", path, sizeof(path)));
	remove_saves(TAPE_STORE_TAKE);
}

static void test_editing(void) {
	static const char tone[] = "tone.wav";
	const unsigned frames = TAPE_PAD_RATE / 2;
	sine_wav(tone, TAPE_PAD_RATE, 441, frames);
	tapepads_set_interpolation(TAPE_INTERP_LINEAR);
	tapepads_factory();

	// The pure C interpolators: linear follows the two inner points, cubic
	// bends through all four and can overshoot only inside the PCM range.
	assert(tape_interpolate(0, 1000, 2000, 3000, 32768, TAPE_INTERP_LINEAR) == 1500);
	assert(tape_interpolate(0, 1000, 2000, 3000, 32768, TAPE_INTERP_CUBIC) == 1500);
	int bent = tape_interpolate(0, 0, 1000, 0, 32768, TAPE_INTERP_CUBIC);
	assert(bent > 500 && bent < 650);
	assert(tape_pitch_q16(0) == TAPE_PITCH_UNITY && tape_pitch_q16(12) == 2 * TAPE_PITCH_UNITY);
	assert(tape_pitch_q16(-60) == TAPE_PITCH_UNITY / 4 && tape_pitch_q16(60) == 4 * TAPE_PITCH_UNITY);

	// Pitch is a pad property even for a factory drum: +12 semitones halves
	// the 210 ms kick's resampled duration. Unison still renders sample-exact.
	run(0, 30 * BLOCK, TAPE_PADS_IDLE);
	assert(nonzero(15 * BLOCK, 18 * BLOCK));
	assert(tapepads_set_pitch(0, 12));
	tape_pad_info_t pitched[TAPE_PAD_COUNT];
	tapepads_info(pitched);
	assert(pitched[0].semitone == 12 && pitched[0].pitch_q16 == 2 * TAPE_PITCH_UNITY);
	run(0, 30 * BLOCK, TAPE_PADS_IDLE);
	assert(!nonzero(15 * BLOCK, 18 * BLOCK));
	tapepads_factory();

	// Source-frame trim markers are exact and stop at the end marker even
	// when the underlying WAV continues. Frame 2048 is the start of a ramp.
	assert(tapepads_assign(0, sample));
	assert(tapepads_set_trim(0, 2048, 2052));
	tape_pad_info_t info[TAPE_PAD_COUNT];
	tapepads_info(info);
	assert(info[0].trim_start == 2048 && info[0].trim_end == 2052 && info[0].trimmed && !info[0].long_clip);
	run(0, BLOCK, TAPE_PADS_IDLE);
	assert(pcm[0] == 1536 && pcm[1] == 1536 && pcm[2] == 2304);
	assert(!nonzero(5, BLOCK));
	assert(tapepads_set_trim_ms(0, 0, 10));
	tapepads_info(info);
	assert(info[0].trim_end == TAPE_PAD_RATE / 100 && info[0].trim_start == 0);

	// 16 Levels: one WAV on all pads, a chromatic semitone per pad, Q16
	// playback-rate resampling and selectable interpolation. Pad 13 is an
	// octave above pad 01 when the root is pad 01.
	assert(tapepads_enable_levels(tone, 0, TAPE_INTERP_CUBIC));
	assert(tapepads_levels_active() && tapepads_interpolation() == TAPE_INTERP_CUBIC);
	tapepads_info(info);
	for (int p = 0; p < TAPE_PAD_COUNT; p++) {
		assert(info[p].kind == TAPE_PAD_WAV && info[p].levels && info[p].semitone == p);
		assert(info[p].interpolation == TAPE_INTERP_CUBIC && info[p].trim_end == frames);
	}
	assert(info[0].pitch_q16 == TAPE_PITCH_UNITY && info[12].pitch_q16 == 2 * TAPE_PITCH_UNITY);
	run(0, 10 * BLOCK, TAPE_PADS_IDLE);
	unsigned root_edges = rising_edges(0, 10 * BLOCK);
	run(12, 10 * BLOCK, TAPE_PADS_IDLE);
	unsigned octave_edges = rising_edges(0, 10 * BLOCK);
	assert(root_edges >= 35 && root_edges <= 50);
	assert(octave_edges > root_edges * 17 / 10 && octave_edges < root_edges * 23 / 10);
	tapepads_set_interpolation(TAPE_INTERP_LINEAR);
	assert(tapepads_interpolation() == TAPE_INTERP_LINEAR);
	run(12, 10 * BLOCK, TAPE_PADS_IDLE);
	assert(rising_edges(0, 10 * BLOCK) > root_edges * 17 / 10);

	// A slice is just sixteen non-overlapping, persistent trim regions.
	assert(tapepads_slice(sample, 16, TAPE_INTERP_LINEAR));
	assert(!tapepads_levels_active());
	tapepads_info(info);
	uint32_t previous = 0;
	for (int p = 0; p < TAPE_PAD_COUNT; p++) {
		assert(info[p].kind == TAPE_PAD_WAV && info[p].trim_start == previous);
		assert(info[p].trim_end > info[p].trim_start);
		previous = info[p].trim_end;
	}
	assert(previous == info[0].frames);

	// Without a choke the long open tone keeps sounding. A closed hat with
	// group +1 cuts the open tone with group -1; the reverse is not true.
	tapepads_factory();
	assert(tapepads_assign(0, tone));
	tapepads_set_preset(1, 8);
	assert(tapepads_set_choke_group(0, 0) && tapepads_set_choke_group(1, 0));
	action = cut_open_hat;
	run(0, 20 * BLOCK, TAPE_PADS_IDLE);
	assert(nonzero(12 * BLOCK, 16 * BLOCK));
	assert(tapepads_set_choke_group(0, -1) && tapepads_set_choke_group(1, 1));
	assert(tapepads_choke_group(0) == -1 && tapepads_choke(1) == 1);
	run(0, 20 * BLOCK, TAPE_PADS_IDLE);
	assert(!nonzero(12 * BLOCK, 16 * BLOCK));
	action = NULL;

	tapepads_factory();
	tapepads_set_interpolation(TAPE_INTERP_LINEAR);
}

static void test_sequence_metadata(void) {
	tapepads_factory();
	assert(tapepads_assign(0, sample));
	assert(tapepads_set_trim(0, 100, 1000));
	assert(tapepads_set_pitch(0, 5));
	assert(tapepads_set_choke_group(0, -2));
	assert(tapepads_set_choke_group(1, 2));
	tapepads_set_interpolation(TAPE_INTERP_CUBIC);
	assert(tapepads_save_sequence() == TAPE_SEQ_OK);
	char saved[2][TAPE_SAVE_NAME_SIZE];
	assert(tape_store_list(TAPE_STORE_SEQUENCE, NULL, saved, 2) == 1);
	char path[PATH_MAX];
	assert(snprintf(path, sizeof(path), "Sequences/%s", saved[0]) < (int)sizeof(path));
	int fd = open(path, O_RDONLY | O_CLOEXEC);
	uint8_t version;
	assert(fd >= 0 && pread(fd, &version, 1, 6) == 1 && version == 2 && close(fd) == 0);

	tapepads_factory();
	assert(tapepads_load_sequence(saved[0]) == TAPE_SEQ_OK);
	tape_pad_info_t info[TAPE_PAD_COUNT];
	tapepads_info(info);
	assert(info[0].kind == TAPE_PAD_WAV && info[0].trim_start == 100 && info[0].trim_end == 1000);
	assert(info[0].semitone == 5 && info[0].pitch_q16 == tape_pitch_q16(5));
	assert(info[0].interpolation == TAPE_INTERP_CUBIC && info[0].trimmed);
	assert(info[0].choke == -2 && info[1].choke == 2);
	remove_saves(TAPE_STORE_SEQUENCE);
	tapepads_factory();
	tapepads_set_interpolation(TAPE_INTERP_LINEAR);
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

	test_editing();
	test_sequences();
	test_sequence_metadata();
	test_takes();

	assert(unlink("bad.wav") == 0 && unlink("link.wav") == 0 && unlink("LinkedKit") == 0 && unlink("tone.wav") == 0);
	assert(unlink(TAPE_KIT_DIR "/House/01 Kick.wav") == 0 && unlink(TAPE_KIT_DIR "/House/16-Vox.WAV") == 0);
	assert(unlink(TAPE_KIT_DIR "/Sort/A.wav") == 0 && unlink(TAPE_KIT_DIR "/Sort/Z.wav") == 0);
	assert(rmdir(TAPE_KIT_DIR "/House") == 0 && rmdir(TAPE_KIT_DIR "/Sort") == 0 && rmdir(TAPE_KIT_DIR) == 0);
}

int main(void) {
	assert(mkdtemp(card_root) && chdir(card_root) == 0);
	assert(setenv("TZ", "UTC", 1) == 0);
	tzset();
	test_factory();
	test_fx();
	test_card();
	fail_open = true;
	run(1, BLOCK, TAPE_PADS_UNAVAILABLE);
	fail_open = false;
	fail_write = true;
	run(1, BLOCK, TAPE_PADS_UNAVAILABLE);
	fail_write = false;
	run(1, BLOCK, TAPE_PADS_IDLE);
	assert(chdir("/") == 0 && rmdir(card_root) == 0);
	puts("Tape sampler: pads, overdub, kits, sequences, live FX, stereo takes, trim/slice, 16 Levels, choke groups and card release OK");
	return 0;
}
