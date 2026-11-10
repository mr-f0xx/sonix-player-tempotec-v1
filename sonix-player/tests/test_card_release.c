#define _GNU_SOURCE
#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "src/system/audio/waveform.h"
#include "src/system/core/logging.h"
#include "src/system/decode/decode.h"

/* ---- minimal stubs for logging.c + waveform.c ----------------------------- */

static bool cfg_log_to_sd = true;

bool config_get_bool(const char *section, const char *key, bool fallback) {
	if (strcmp(section, "system") == 0 && strcmp(key, "log_to_sd") == 0) {
		return cfg_log_to_sd;
	}
	return fallback;
}

void config_set_bool(const char *section, const char *key, bool value) {
	if (strcmp(section, "system") == 0 && strcmp(key, "log_to_sd") == 0) {
		cfg_log_to_sd = value;
	}
}

bool config_save(void) { return true; }

void thread_be_background(const char *name) { (void)name; }
void thread_be_realtime(const char *name, int priority) {
	(void)name;
	(void)priority;
}

uint32_t utils_isqrt64(uint64_t x) {
	uint64_t r = 0, bit = 1ull << 62;
	while (bit > x) bit >>= 2;
	while (bit != 0) {
		if (x >= r + bit) {
			x -= r + bit;
			r = (r >> 1) + bit;
		} else {
			r >>= 1;
		}
		bit >>= 2;
	}
	return (uint32_t)r;
}

long audio_quiet_ms(void) { return 10000; }
bool sndfile_available(void) { return false; }

/* ---- slow fake decoder that opens a real file on the card ----------------- */

struct decoder {
	FILE *file;
	char path[512];
	uint64_t remaining;
};

static atomic_bool decode_in_flight;

decode_format_t decode_detect_format(const char *filepath) {
	(void)filepath;
	return DECODE_FORMAT_FLAC;
}

decoder_t *decoder_open(const char *filepath, decode_format_t format) {
	(void)format;
	FILE *f = fopen(filepath, "rb"); // deliberately without O_CLOEXEC, like dr_flac
	if (!f) {
		return NULL;
	}
	decoder_t *dec = calloc(1, sizeof(*dec));
	assert(dec != NULL);
	dec->file = f;
	snprintf(dec->path, sizeof(dec->path), "%s", filepath);
	dec->remaining = 4096 * 200; // 200 chunks * 10 ms = 2 s unless cancelled
	atomic_store(&decode_in_flight, true);
	return dec;
}

void decoder_close(decoder_t *dec) {
	if (!dec) {
		return;
	}
	fclose(dec->file);
	free(dec);
	atomic_store(&decode_in_flight, false);
}

bool decoder_passthrough(const decoder_t *dec) {
	(void)dec;
	return false;
}

uint64_t decoder_total_pcm_frames(const decoder_t *dec) { return dec->remaining; }
int decoder_channels(const decoder_t *dec) {
	(void)dec;
	return 2;
}

bool decoder_source_span(const decoder_t *dec, char *file, size_t size, double *begin, double *end) {
	snprintf(file, size, "%s", dec->path);
	*begin = 0.0;
	*end = 1.0;
	return true;
}

uint64_t decoder_read_pcm_frames_s16(decoder_t *dec, uint64_t frame_count, short *pBuffer) {
	if (dec->remaining == 0) {
		return 0;
	}
	usleep(10 * 1000);
	uint64_t n = frame_count < dec->remaining ? frame_count : dec->remaining;
	memset(pBuffer, 0, (size_t)n * 2 * sizeof(short));
	dec->remaining -= n;
	return n;
}

/* ---- /proc/self/fd inspector ---------------------------------------------- */

static int count_card_fds(const char *card_root, bool *non_cloexec_above_2) {
	char canonical[PATH_MAX];
	assert(realpath(card_root, canonical) != NULL);
	size_t root_len = strlen(canonical);
	if (non_cloexec_above_2) {
		*non_cloexec_above_2 = false;
	}

	DIR *dir = opendir("/proc/self/fd");
	assert(dir != NULL);
	int count = 0;
	struct dirent *de;
	while ((de = readdir(dir)) != NULL) {
		if (de->d_name[0] < '0' || de->d_name[0] > '9') {
			continue;
		}
		int fd = atoi(de->d_name);
		if (fd == dirfd(dir)) {
			continue;
		}
		char link[320], target[PATH_MAX];
		if ((size_t)snprintf(link, sizeof(link), "/proc/self/fd/%s", de->d_name) >= sizeof(link)) {
			continue;
		}
		ssize_t n = readlink(link, target, sizeof(target) - 1);
		if (n <= 0) {
			continue;
		}
		target[n] = '\0';
		if (strncmp(target, canonical, root_len) == 0 &&
			(target[root_len] == '/' || target[root_len] == '\0')) {
			count++;
			if (fd > 2 && non_cloexec_above_2) {
				int flags = fcntl(fd, F_GETFD);
				if (flags >= 0 && (flags & FD_CLOEXEC) == 0) {
					*non_cloexec_above_2 = true;
				}
			}
		}
	}
	closedir(dir);
	return count;
}

// What the next start says about the end of the last one. Each case makes a
// card, leaves a log on it, starts logging there in a child of its own (the
// logging state is one-shot per process) and looks at the lines that were
// added. `reject` is asserted absent when it is not NULL.
static void check_previous_end(const char *previous, const char *expect, const char *reject) {
	int pipe_fds[2];
	assert(pipe(pipe_fds) == 0);
	pid_t pid = fork();
	assert(pid >= 0);
	if (pid == 0) {
		close(pipe_fds[0]);
		char root[] = "/tmp/sonix-previous-XXXXXX";
		if (!mkdtemp(root)) {
			_exit(2);
		}
		char card[320], folder[360], log_path[420];
		snprintf(card, sizeof(card), "%s/sd_0", root);
		snprintf(folder, sizeof(folder), "%s/.local", card);
		snprintf(log_path, sizeof(log_path), "%s/sonix_player.log", folder);
		if (mkdir(card, 0700) != 0 || mkdir(folder, 0700) != 0) {
			_exit(3);
		}
		FILE *prev = fopen(log_path, "w");
		if (!prev) {
			_exit(4);
		}
		fputs(previous, prev);
		fclose(prev);

		int saved = dup(STDOUT_FILENO);
		unsetenv("SONIX_LOG");
		logging_init();
		logging_attach_sd(card);
		logging_flush();
		if (saved >= 0) {
			fflush(stdout);
			dup2(saved, STDOUT_FILENO);
		}

		FILE *lf = fopen(log_path, "r");
		if (!lf) {
			_exit(5);
		}
		char data[8192];
		size_t n = fread(data, 1, sizeof(data) - 1, lf);
		fclose(lf);
		data[n] = '\0';
		bool ok = strstr(data, expect) != NULL && (!reject || !strstr(data, reject));
		if (ok) {
			write(pipe_fds[1], "1", 1);
		}
		_exit(0);
	}
	close(pipe_fds[1]);
	char verdict = 0;
	ssize_t got = read(pipe_fds[0], &verdict, 1);
	close(pipe_fds[0]);
	int status = 0;
	waitpid(pid, &status, 0);
	assert(got == 1 && verdict == '1');
}

int main(void) {
	char root[] = "/tmp/sonix-cardrelease-XXXXXX";
	assert(mkdtemp(root) != NULL);
	char card[320], track[512], log_path[512];
	snprintf(card, sizeof(card), "%s/sd_0", root);
	snprintf(track, sizeof(track), "%s/album.flac", card);
	snprintf(log_path, sizeof(log_path), "%s/.local/sonix_player.log", card);
	assert(mkdir(card, 0700) == 0);

	FILE *tf = fopen(track, "wb");
	assert(tf != NULL);
	char zeros[4096] = {0};
	assert(fwrite(zeros, 1, sizeof(zeros), tf) == sizeof(zeros));
	assert(fclose(tf) == 0);

	// The three endings a card log can be left with. Each is decided before the
	// file is appended to, in a process of its own: logging is one-shot.
	check_previous_end("13:00:35.674 audio[1978313]: starting '/mnt/sd_0/.local/tidal-cache/5113376.flac'\n"
			   "13:00:36.012 audio[1978651]: the track started coming out after 93 ms (32768 frames written)\n",
			   "the previous run did not shut down", "shut down the way it should");
	check_previous_end("12:26:56.979 clock: writing 1794169616 through to the RTC before shutdown\n"
			   "12:26:56.980 clock: written through to /dev/rtc0\n",
			   "shut down the way it should", "did not shut down");
	check_previous_end("13:01:18.816 kernel: <4> [   23.157746] cs43198_dai_prepare pcm\n"
			   "sonix_launch: player killed by signal 11, rebooting\n",
			   "sonix_launch: player killed by signal 11", "did not shut down");

	int saved_out = dup(STDOUT_FILENO);
	int saved_err = dup(STDERR_FILENO);
	assert(saved_out >= 0 && saved_err >= 0);
	assert(fcntl(saved_out, F_SETFD, FD_CLOEXEC) == 0);
	assert(fcntl(saved_err, F_SETFD, FD_CLOEXEC) == 0);

	unsetenv("SONIX_LOG");
	logging_init();
	logging_attach_sd(card);

	// Card log is active on 1, 2, and active_stream; active_stream (>2) must
	// carry FD_CLOEXEC so exec'd helpers cannot inherit it.
	bool non_cloexec_above_2 = false;
	assert(count_card_fds(card, &non_cloexec_above_2) >= 3);
	assert(!non_cloexec_above_2);

	// Suspending for USB export must leave zero open descriptors on the card.
	logging_suspend_for_usb();
	printf("usb export in progress line\n");
	logging_flush();
	assert(count_card_fds(card, NULL) == 0);

	// Resuming after USB export reattaches with FD_CLOEXEC and drains held lines.
	logging_resume_after_usb();
	logging_flush();
	assert(count_card_fds(card, &non_cloexec_above_2) >= 3);
	assert(!non_cloexec_above_2);

	// Suspend logging again so only waveform holds card descriptors next.
	logging_suspend_for_usb();
	assert(count_card_fds(card, NULL) == 0);

	// Start a slow background waveform decode on the card and verify that
	// waveform_set_cache_dir(NULL) cancels it and waits until every card
	// descriptor (both decoder FILE* and posix_fadvise drop fd) is closed.
	waveform_set_cache_dir(card);
	waveform_start();
	uint8_t bars[WAVEFORM_BARS];
	assert(!waveform_get(track, bars));
	for (int waited = 0; waited < 1000 && !atomic_load(&decode_in_flight); waited += 10) {
		usleep(10 * 1000);
	}
	assert(atomic_load(&decode_in_flight));
	assert(count_card_fds(card, NULL) >= 2);

	waveform_set_cache_dir(NULL);
	assert(!atomic_load(&decode_in_flight));
	assert(count_card_fds(card, NULL) == 0);

	// Restore test runner stdio so PASS prints to the terminal.
	logging_flush();
	assert(dup2(saved_out, STDOUT_FILENO) >= 0);
	assert(dup2(saved_err, STDERR_FILENO) >= 0);
	close(saved_out);
	close(saved_err);

	FILE *lf = fopen(log_path, "r");
	assert(lf != NULL);
	char log_data[4096];
	size_t n = fread(log_data, 1, sizeof(log_data) - 1, lf);
	fclose(lf);
	log_data[n] = '\0';
	assert(strstr(log_data, "usb export in progress line") != NULL);

	char wave_dat[512], local_dir[512];
	snprintf(wave_dat, sizeof(wave_dat), "%s/.local/waveform.dat", card);
	snprintf(local_dir, sizeof(local_dir), "%s/.local", card);
	unlink(wave_dat);
	unlink(log_path);
	rmdir(local_dir);
	unlink(track);
	rmdir(card);
	rmdir(root);

	dprintf(STDOUT_FILENO, "PASS card release (log O_CLOEXEC, USB log suspend/resume and mid-decode waveform abort)\n");
	return 0;
}
