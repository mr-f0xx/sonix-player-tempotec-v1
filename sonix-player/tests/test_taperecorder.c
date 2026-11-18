#include "src/system/audio/taperecorder.h"
#include "src/system/device/system.h"

#include <alsa/asoundlib.h>
#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static bool have_card = true, writable = true, card_reserved, fail_sync;
static int opens, closes, pcm_error;
static char card_root[256];
static atomic_int available, read_error;
static struct snd_pcm { int unused; } capture;

int storage_card_file_job_begin(bool write_access) {
	assert(!card_reserved);
	if (!have_card || (write_access && !writable)) return -1;
	int fd = open(card_root, O_RDONLY | O_DIRECTORY);
	assert(fd >= 0);
	card_reserved = true;
	return fd;
}
void storage_card_file_job_end(void) { assert(card_reserved); card_reserved = false; }
const char *storage_sd_root(void) { return card_root; }

time_t __wrap_time(time_t *out) {
	time_t stamp = 1767225600; // 2026-01-01 00:00:00 UTC
	if (out) *out = stamp;
	return stamp;
}
int __real_fsync(int fd);
int __wrap_fsync(int fd) {
	if (fail_sync) {
		errno = ENOSPC;
		return -1;
	}
	return __real_fsync(fd);
}

int snd_pcm_open(snd_pcm_t **out, const char *device, snd_pcm_stream_t stream, int mode) {
	assert(stream == SND_PCM_STREAM_CAPTURE && mode == SND_PCM_NONBLOCK);
	assert(!strcmp(device, "plughw:2,1"));
	if (pcm_error) return pcm_error;
	assert(opens == closes);
	opens++;
	*out = &capture;
	return 0;
}
int snd_pcm_set_params(snd_pcm_t *pcm, snd_pcm_format_t format, snd_pcm_access_t access, unsigned int channels,
					   unsigned int rate, int soft_resample, unsigned int latency) {
	assert(pcm == &capture && format == SND_PCM_FORMAT_S16_LE && access == SND_PCM_ACCESS_RW_INTERLEAVED);
	assert(channels == 1 && rate == TAPE_REC_RATE && soft_resample == 1 && latency > 0);
	return 0;
}
int snd_pcm_start(snd_pcm_t *pcm) { assert(pcm == &capture); return 0; }
int snd_pcm_close(snd_pcm_t *pcm) { assert(pcm == &capture && opens == closes + 1); closes++; return 0; }
const char *snd_strerror(int err) { return strerror(-err); }
int snd_pcm_wait(snd_pcm_t *pcm, int timeout) {
	assert(pcm == &capture && timeout > 0 && timeout <= 100);
	usleep(3000);
	return 0;
}
snd_pcm_sframes_t snd_pcm_readi(snd_pcm_t *pcm, void *data, snd_pcm_uframes_t wanted) {
	assert(pcm == &capture && wanted > 0);
	int error = read_error;
	if (error < 0) return error;
	unsigned frames = (unsigned)available;
	if (!frames) return -EAGAIN;
	if (frames > wanted) frames = (unsigned)wanted;
	atomic_fetch_sub(&available, (int)frames);
	for (unsigned i = 0; i < frames; i++) {
		uint8_t *sample = (uint8_t *)data + 2 * i;
		sample[0] = 0;
		sample[1] = 0x80; // the signed negative rail: abs(-32768) must not wrap
	}
	return frames;
}

static void touch(const char *path) {
	FILE *f = fopen(path, "wb");
	assert(f && fclose(f) == 0);
}

static tape_rec_status_t wait_for(tape_rec_state_t state, uint32_t frames, bool clipping) {
	for (int i = 0; i < 1500; i++) {
		tape_rec_status_t now;
		taperecorder_status(&now);
		if (now.state == state && now.frames == frames && (!clipping || (now.clipped && now.peak == 32768))) return now;
		usleep(2000);
	}
	assert(!"Timed out waiting for the capture worker");
	return (tape_rec_status_t){0};
}

static uint32_t le32(const uint8_t *p) {
	return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static void assert_wav(const char *name, unsigned frames) {
	char path[512];
	assert(taperecorder_path(name, path, sizeof(path)));
	struct stat st;
	assert(stat(path, &st) == 0 && st.st_size == (off_t)(44 + 2 * frames));
	assert((st.st_mode & 0777) == 0600);
	FILE *f = fopen(path, "rb");
	uint8_t h[44];
	assert(f && fread(h, 1, sizeof(h), f) == sizeof(h));
	assert(!memcmp(h, "RIFF", 4) && !memcmp(h + 8, "WAVEfmt ", 8) && !memcmp(h + 36, "data", 4));
	assert(le32(h + 4) == 36 + 2 * frames && le32(h + 16) == 16 && le32(h + 20) == 0x10001);
	assert(le32(h + 24) == TAPE_REC_RATE && le32(h + 28) == TAPE_REC_RATE * 2);
	assert(le32(h + 32) == 0x100002 && le32(h + 40) == 2 * frames);
	for (unsigned i = 0; i < frames; i++) assert(fgetc(f) == 0 && fgetc(f) == 0x80);
	assert(fgetc(f) == EOF && fclose(f) == 0);
}

int main(void) {
	char tmp[] = "/tmp/sonix-tape-XXXXXX";
	assert(mkdtemp(tmp) && chdir(tmp) == 0);
	assert(setenv("TZ", "UTC", 1) == 0);
	tzset();
	snprintf(card_root, sizeof(card_root), "%s/card", tmp);
	assert(mkdir("sound", 0700) == 0 && mkdir("card", 0700) == 0);
	tape_input_t input;
	assert(!taperecorder_find_input(&input));
	touch("sound/pcmC0D0p");
	touch("sound/pcmC0D0c-incomplete");
	assert(!taperecorder_find_input(&input));
	assert(taperecorder_start(NULL, true) == TAPE_REC_NO_INPUT);
	touch("sound/pcmC0D3c");
	assert(taperecorder_find_input(&input) && !input.usb && input.card == 0 && input.device == 3);
	touch("sound/pcmC2D1c");
	assert(mkdir("asound", 0700) == 0 && mkdir("asound/card2", 0700) == 0);
	touch("asound/card2/usbid");
	assert(taperecorder_find_input(&input) && input.usb && input.card == 2 && input.device == 1);
	assert(opens == 0); // discovery does not listen or create a file

	writable = false;
	assert(taperecorder_start(&input, true) == TAPE_REC_NO_CARD);
	pcm_error = -EBUSY;
	assert(taperecorder_start(&input, false) == TAPE_REC_OPENING);
	wait_for(TAPE_REC_INPUT_ERROR, 0, false);
	pcm_error = 0;
	assert(opens == 0 && access("card/Recordings", F_OK) != 0);

	// Meter-only is useful without a writable card. Promoting it acquires
	// a new capture, with no pre-roll from the meter.
	assert(taperecorder_start(&input, false) == TAPE_REC_OPENING);
	available = 41;
	wait_for(TAPE_REC_MONITORING, 0, true);
	assert(access("card/Recordings", F_OK) != 0);
	assert(taperecorder_start(&input, true) == TAPE_REC_NO_CARD);
	wait_for(TAPE_REC_MONITORING, 0, true);
	writable = true;
	assert(taperecorder_start(&input, true) == TAPE_REC_OPENING);
	wait_for(TAPE_REC_RECORDING, 0, false);
	assert(taperecorder_start(&input, true) == TAPE_REC_BUSY);
	available = 1700;
	wait_for(TAPE_REC_RECORDING, 1700, true);
	char names[4][TAPE_MEMO_NAME_SIZE];
	assert(taperecorder_list(names, 4) == 0); // the .part is not a memo
	taperecorder_finish(); // same path as USB export/card removal
	wait_for(TAPE_REC_SAVED, 1700, false);
	assert(opens == closes && !taperecorder_active());
	assert(taperecorder_list(names, 4) == 1 && !strcmp(names[0], "Memo-20260101-000000-001.wav"));
	assert_wav(names[0], 1700);

	// Unplugging after useful data was captured finalizes it, with a distinct
	// status. A repeated timestamp cannot overwrite the earlier memo.
	assert(taperecorder_start(&input, true) == TAPE_REC_OPENING);
	available = 49;
	wait_for(TAPE_REC_RECORDING, 49, true);
	read_error = -ENODEV;
	wait_for(TAPE_REC_INTERRUPTED, 49, false);
	read_error = 0;
	assert(taperecorder_list(names, 4) == 2 && !strcmp(names[0], "Memo-20260101-000000-002.wav"));
	assert_wav(names[0], 49);
	assert_wav(names[1], 1700);

	// Stopping before any frames, and a failed save, never publish an empty
	// or truncated WAV. A test-only one-second cap exercises the RIFF limit.
	assert(taperecorder_start(&input, true) == TAPE_REC_OPENING);
	wait_for(TAPE_REC_RECORDING, 0, false);
	taperecorder_finish();
	wait_for(TAPE_REC_IDLE, 0, false);
	assert(taperecorder_list(names, 4) == 2);
	fail_sync = true;
	assert(taperecorder_start(&input, true) == TAPE_REC_OPENING);
	available = 80;
	wait_for(TAPE_REC_RECORDING, 80, true);
	taperecorder_finish();
	wait_for(TAPE_REC_DISK_ERROR, 80, false);
	fail_sync = false;
	assert(taperecorder_list(names, 4) == 2);

	assert(taperecorder_start(&input, true) == TAPE_REC_OPENING);
	available = TAPE_REC_RATE + 33;
	wait_for(TAPE_REC_SAVED, TAPE_REC_RATE, false);
	taperecorder_finish();
	assert(available == 33);
	assert(taperecorder_list(names, 1) == 1 && !strcmp(names[0], "Memo-20260101-000000-003.wav"));
	assert_wav(names[0], TAPE_REC_RATE);

	// Bad card paths and stale input nodes do not redirect recording or playback.
	assert(symlink(names[0], "card/Recordings/Memo-20300101-000000-001.wav") == 0);
	assert(taperecorder_list(names, 4) == 3);
	char path[512];
	assert(!taperecorder_path("Memo-20300101-000000-001.wav", path, sizeof(path)));
	assert(!taperecorder_path("../Recordings/a.wav", path, sizeof(path)));
	have_card = false;
	assert(taperecorder_list(names, 4) == -1);
	assert(!taperecorder_path(names[0], path, sizeof(path)));
	have_card = true;
	for (int i = 0; i < 3; i++) {
		assert(taperecorder_path(names[i], path, sizeof(path)) && unlink(path) == 0);
	}
	assert(unlink("card/Recordings/Memo-20300101-000000-001.wav") == 0);
	assert(rmdir("card/Recordings") == 0); // no partial files left after any stop
	assert(symlink("../sound", "card/Recordings") == 0);
	assert(taperecorder_start(&input, true) == TAPE_REC_OPENING);
	wait_for(TAPE_REC_DISK_ERROR, 0, false);
	taperecorder_finish();
	assert(taperecorder_list(names, 4) == -1);
	assert(unlink("sound/pcmC2D1c") == 0);
	assert(taperecorder_start(&input, false) == TAPE_REC_NO_INPUT);
	assert(opens == closes && !card_reserved);

	assert(unlink("card/Recordings") == 0 && rmdir("card") == 0);
	assert(unlink("sound/pcmC0D0p") == 0 && unlink("sound/pcmC0D0c-incomplete") == 0);
	assert(unlink("sound/pcmC0D3c") == 0 && rmdir("sound") == 0);
	assert(unlink("asound/card2/usbid") == 0 && rmdir("asound/card2") == 0 && rmdir("asound") == 0);
	assert(chdir("/") == 0 && rmdir(tmp) == 0);
	puts("Tape recorder: capture gate, meter, WAVs, hot-unplug and card handoff OK");
	return 0;
}
