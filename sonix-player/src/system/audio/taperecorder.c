#include "taperecorder.h"

#include "src/system/device/system.h"

#include <alsa/asoundlib.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#ifndef TAPE_SOUND_CLASS
#define TAPE_SOUND_CLASS "/sys/class/sound"
#endif
#ifndef TAPE_ASOUND_ROOT
#define TAPE_ASOUND_ROOT "/proc/asound"
#endif
#ifndef TAPE_REC_LIMIT_SECONDS
#define TAPE_REC_LIMIT_SECONDS 3600
#endif

#define MEMO_DIR "Recordings"
#define BLOCK_FRAMES (TAPE_REC_RATE / 50) // 20 ms
#define INPUT_TIMEOUT_MS 2000
#define INPUT_WAIT_MS 40
#define INFO_SIZE 44
#define MAX_FRAMES ((uint32_t)TAPE_REC_LIMIT_SECONDS * TAPE_REC_RATE)

// A hardware capture node is evidence of an input, not evidence that the mic
// is connected to that particular board. Never test output PCMs or open an
// ALSA default (which could be a network/loopback plugin).
static bool capture_present(const tape_input_t *input) {
#ifdef HOST_BUILD
	(void)input;
	return false; // no mic passthrough in the host or browser builds
#else
	if (!input || input->card < 0 || input->device < 0) return false;
	char path[sizeof(TAPE_SOUND_CLASS) + 40];
	snprintf(path, sizeof(path), "%s/pcmC%dD%dc", TAPE_SOUND_CLASS, input->card, input->device);
	return access(path, F_OK) == 0;
#endif
}

bool taperecorder_find_input(tape_input_t *out) {
#ifdef HOST_BUILD
	(void)out;
	return false;
#else
	if (!out) return false;
	DIR *dir = opendir(TAPE_SOUND_CLASS);
	if (!dir) return false;
	bool found = false;
	struct dirent *entry;
	while ((entry = readdir(dir))) {
		tape_input_t candidate = {0};
		int end = 0;
		if (sscanf(entry->d_name, "pcmC%dD%dc%n", &candidate.card, &candidate.device, &end) != 2 ||
			!end || entry->d_name[end] || !capture_present(&candidate)) continue;
		char path[sizeof(TAPE_ASOUND_ROOT) + 32];
		snprintf(path, sizeof(path), "%s/card%d/usbid", TAPE_ASOUND_ROOT, candidate.card);
		candidate.usb = access(path, F_OK) == 0;
		bool earlier = found && (candidate.card < out->card ||
			(candidate.card == out->card && candidate.device < out->device));
		if (!found || (candidate.usb && !out->usb) || (candidate.usb == out->usb && earlier)) {
			*out = candidate;
			found = true;
		}
	}
	closedir(dir);
	return found;
#endif
}

// Open relative to the reserved card, never the underlying mount-point path.
// Do not follow a Recordings symlink (e.g. to internal storage).
static int memo_dir(int card, bool create) {
	if (create && mkdirat(card, MEMO_DIR, 0755) < 0 && errno != EEXIST) return -1;
	return openat(card, MEMO_DIR, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
}

static bool memo_name(const char *name) {
	return name && strlen(name) == TAPE_MEMO_NAME_SIZE - 1 && !strncmp(name, "Memo-", 5) &&
		strspn(name + 5, "0123456789") == 8 && name[13] == '-' &&
		strspn(name + 14, "0123456789") == 6 && name[20] == '-' &&
		strspn(name + 21, "0123456789") == 3 && !strcmp(name + 24, ".wav");
}

static bool memo_exists(int dir, const char *name) {
	struct stat st;
	return fstatat(dir, name, &st, AT_SYMLINK_NOFOLLOW) == 0 && S_ISREG(st.st_mode) && st.st_size > INFO_SIZE;
}

int taperecorder_list(char names[][TAPE_MEMO_NAME_SIZE], int capacity) {
	if (!names || capacity <= 0) return 0;
	int card = storage_card_file_job_begin(false);
	if (card < 0) return -1;
	int dir = memo_dir(card, false);
	int count = dir < 0 && errno != ENOENT ? -1 : 0;
	DIR *listing = dir < 0 ? NULL : fdopendir(dir);
	if (listing) {
		struct dirent *entry;
		while ((entry = readdir(listing))) {
			const char *name = entry->d_name;
			if (!memo_name(name)) continue;
			int at = 0;
			while (at < count && strcmp(name, names[at]) < 0) at++;
			if (at == capacity || !memo_exists(dir, name)) continue;
			if (count < capacity) count++;
			memmove(names + at + 1, names + at, (size_t)(count - at - 1) * sizeof(*names));
			memcpy(names[at], name, TAPE_MEMO_NAME_SIZE);
		}
		closedir(listing);
	} else if (dir >= 0) {
		close(dir);
		count = -1;
	}
	close(card);
	storage_card_file_job_end();
	return count;
}

bool taperecorder_path(const char *name, char *out, size_t size) {
	if (!out || !size || !memo_name(name)) return false;
	int card = storage_card_file_job_begin(false);
	if (card < 0) return false;
	int dir = memo_dir(card, false);
	const char *root = storage_sd_root();
	bool ok = false;
	if (dir >= 0 && root && *root && memo_exists(dir, name)) {
		int n = snprintf(out, size, "%s/%s/%s", root, MEMO_DIR, name);
		ok = n >= 0 && (size_t)n < size;
	}
	if (dir >= 0) close(dir);
	close(card);
	storage_card_file_job_end();
	return ok;
}

static bool write_all(int fd, const void *buffer, size_t size) {
	const uint8_t *bytes = buffer;
	while (size) {
		ssize_t n = write(fd, bytes, size);
		if (n < 0 && errno == EINTR) continue;
		if (n <= 0) return false;
		bytes += n;
		size -= (size_t)n;
	}
	return true;
}

static void le32(uint8_t out[4], uint32_t n) {
	for (int i = 0; i < 4; i++) out[i] = (uint8_t)(n >> (i * 8));
}

static bool write_header(int fd, uint32_t frames) {
	uint8_t info[INFO_SIZE] = {
		'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'A', 'V', 'E', 'f', 'm', 't', ' ',
		16, 0, 0, 0, 1, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2, 0, 16, 0,
		'd', 'a', 't', 'a', 0, 0, 0, 0,
	};
	le32(info + 4, 36 + frames * 2);
	le32(info + 24, TAPE_REC_RATE);
	le32(info + 28, TAPE_REC_RATE * 2);
	le32(info + 40, frames * 2);
	return lseek(fd, 0, SEEK_SET) == 0 && write_all(fd, info, sizeof(info));
}

#define PART_SIZE (TAPE_MEMO_NAME_SIZE + sizeof("..part") - 1)

static bool name_free(int dir, const char *name) {
	struct stat st;
	return fstatat(dir, name, &st, AT_SYMLINK_NOFOLLOW) < 0 && errno == ENOENT;
}

static int create_part(int dir, char name[TAPE_MEMO_NAME_SIZE], char part[PART_SIZE]) {
	char stamp[16];
	time_t now = time(NULL);
	struct tm tm;
	if (!localtime_r(&now, &tm) || strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", &tm) != sizeof(stamp) - 1) return -1;
	for (unsigned i = 1; i <= 999; i++) {
		snprintf(name, TAPE_MEMO_NAME_SIZE, "Memo-%s-%03u.wav", stamp, i);
		if (!name_free(dir, name)) continue;
		snprintf(part, PART_SIZE, ".%s.part", name);
		int fd = openat(dir, part, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
		if (fd >= 0 || errno != EEXIST) return fd;
	}
	return -1;
}

// Lock order for starts and card handoff: storage -> worker -> status. The
// worker takes only the status lock, never a lock held by a thread joining it.
static pthread_mutex_t worker_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t status_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_t worker;
static bool joinable;
static atomic_bool stop_requested;
static tape_rec_status_t status;
static struct {
	tape_input_t input;
	int card; // -1 for meter-only sessions; otherwise owned by the worker
} job;

static void publish(tape_rec_status_t value) {
	pthread_mutex_lock(&status_lock);
	status = value;
	pthread_mutex_unlock(&status_lock);
}

void taperecorder_status(tape_rec_status_t *out) {
	pthread_mutex_lock(&status_lock);
	*out = status;
	pthread_mutex_unlock(&status_lock);
}

static bool active(tape_rec_state_t state) {
	return state == TAPE_REC_OPENING || state == TAPE_REC_MONITORING ||
		state == TAPE_REC_RECORDING || state == TAPE_REC_SAVING;
}

bool taperecorder_active(void) {
	tape_rec_status_t now;
	taperecorder_status(&now);
	return active(now.state);
}

static uint32_t monotonic_ms(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint32_t)ts.tv_sec * 1000u + (uint32_t)(ts.tv_nsec / 1000000);
}

static unsigned block_peak(const uint8_t *data, unsigned frames) {
	unsigned peak = 0;
	for (unsigned i = 0; i < frames; i++) {
		unsigned word = (unsigned)data[2 * i] | (unsigned)data[2 * i + 1] << 8;
		unsigned level = word >= 32768 ? 65536 - word : word;
		if (level > peak) peak = level;
	}
	return peak;
}

static void *capture_main(void *unused) {
	(void)unused;
	int card = job.card, dir = -1, fd = -1;
	char name[TAPE_MEMO_NAME_SIZE], part[PART_SIZE];
	char device[40];
	snd_pcm_t *pcm = NULL;
	tape_rec_status_t now = {.record = card >= 0};
	tape_rec_state_t result = TAPE_REC_IDLE;
	bool input_lost = false;

	// The plug adapter can negotiate ordinary USB interfaces that do not do
	// native mono/48 kHz. The WAV always describes the client format below.
	snprintf(device, sizeof(device), "plughw:%d,%d", job.input.card, job.input.device);
	int err = snd_pcm_open(&pcm, device, SND_PCM_STREAM_CAPTURE, SND_PCM_NONBLOCK);
	if (err >= 0) err = snd_pcm_set_params(pcm, SND_PCM_FORMAT_S16_LE, SND_PCM_ACCESS_RW_INTERLEAVED,
		1, TAPE_REC_RATE, 1, 160000);
	if (err < 0) {
		fprintf(stderr, "tape: %s not available for capture: %s\n", device, snd_strerror(err));
		result = TAPE_REC_INPUT_ERROR;
		goto done;
	}
	if (stop_requested) goto done;
	if (now.record) {
		dir = memo_dir(card, true);
		close(card);
		card = -1;
		if (dir >= 0) fd = create_part(dir, name, part);
		if (fd < 0 || !write_header(fd, 0)) {
			result = TAPE_REC_DISK_ERROR;
			goto done;
		}
	}
	if (stop_requested) goto done;
	err = snd_pcm_start(pcm);
	if (err < 0) {
		fprintf(stderr, "tape: %s could not start: %s\n", device, snd_strerror(err));
		result = TAPE_REC_INPUT_ERROR;
		goto done;
	}
	now.state = now.record ? TAPE_REC_RECORDING : TAPE_REC_MONITORING;
	publish(now);

	int16_t block[BLOCK_FRAMES]; // keep the ALSA sample buffer naturally aligned
	uint32_t last_data = monotonic_ms();
	unsigned clip_left = 0;
	while (!stop_requested && now.frames < MAX_FRAMES) {
		unsigned want = BLOCK_FRAMES;
		if (now.record && MAX_FRAMES - now.frames < want) want = MAX_FRAMES - now.frames;
		snd_pcm_sframes_t got = snd_pcm_readi(pcm, block, want);
		if (got == -EINTR) continue;
		if (got == -EAGAIN || got == 0) {
			if (monotonic_ms() - last_data < INPUT_TIMEOUT_MS) {
				int ready = snd_pcm_wait(pcm, INPUT_WAIT_MS);
				if (ready >= 0 || ready == -EINTR) continue;
				got = ready;
			} else {
				got = -ETIMEDOUT;
			}
		}
		if (got < 0 || (unsigned long)got > want) {
			if (!stop_requested && got < 0) fprintf(stderr, "tape: capture ended: %s\n", snd_strerror((int)got));
			input_lost = !stop_requested;
			break;
		}
		last_data = monotonic_ms();
		if (now.record) {
			if (!write_all(fd, block, (size_t)got * 2)) {
				result = TAPE_REC_DISK_ERROR;
				break;
			}
			now.frames += (uint32_t)got;
		}
		unsigned peak = block_peak((const uint8_t *)block, (unsigned)got);
		// Fast attack, ~200 ms decay, and a separate clip hold so a short
		// overload is still visible between two UI refreshes.
		now.peak = now.peak * (TAPE_REC_RATE - (unsigned)got * 5) / TAPE_REC_RATE;
		if (peak > now.peak) now.peak = peak;
		if (peak >= 32767) clip_left = TAPE_REC_RATE;
		else clip_left = clip_left > (unsigned)got ? clip_left - (unsigned)got : 0;
		now.clipped = clip_left > 0;
		publish(now);
	}

done:
	if (pcm) snd_pcm_close(pcm);
	if (card >= 0) close(card);
	if (input_lost) result = TAPE_REC_INPUT_ERROR;
	if (fd >= 0) {
		bool keep = now.frames > 0 && result != TAPE_REC_DISK_ERROR;
		if (keep) {
			now.state = TAPE_REC_SAVING;
			publish(now);
			keep = write_header(fd, now.frames) && fsync(fd) == 0;
		}
		if (close(fd) != 0) keep = false;
		if (keep && name_free(dir, name) && renameat(dir, part, dir, name) == 0) {
			(void)fsync(dir); // best effort on filesystems without directory fsync
			result = input_lost ? TAPE_REC_INTERRUPTED : TAPE_REC_SAVED;
		} else {
			unlinkat(dir, part, 0);
			if (now.frames > 0) result = TAPE_REC_DISK_ERROR;
		}
	}
	if (dir >= 0) close(dir);
	publish((tape_rec_status_t){.state = result, .frames = now.frames});
	return NULL;
}

void taperecorder_request_stop(void) { stop_requested = true; }

static void finish_locked(void) {
	if (!joinable) return;
	taperecorder_request_stop();
	pthread_join(worker, NULL);
	joinable = false;
}

void taperecorder_finish(void) {
	pthread_mutex_lock(&worker_lock);
	finish_locked();
	pthread_mutex_unlock(&worker_lock);
}

tape_rec_state_t taperecorder_start(const tape_input_t *input, bool record) {
	if (!capture_present(input)) return TAPE_REC_NO_INPUT;
	int card = record ? storage_card_file_job_begin(true) : -1;
	if (record && card < 0) return TAPE_REC_NO_CARD;

	pthread_mutex_lock(&worker_lock);
	tape_rec_status_t previous;
	taperecorder_status(&previous);
	tape_rec_state_t result = TAPE_REC_BUSY;
	if (!active(previous.state) || (record && previous.state == TAPE_REC_MONITORING)) {
		finish_locked(); // if the meter was on, stop it before opening a memo
		job.input = *input;
		job.card = card;
		stop_requested = false;
		publish((tape_rec_status_t){.state = TAPE_REC_OPENING, .record = record});
		if (pthread_create(&worker, NULL, capture_main, NULL) == 0) {
			joinable = true;
			card = -1;
			result = TAPE_REC_OPENING;
		} else {
			result = TAPE_REC_START_ERROR;
			publish((tape_rec_status_t){.state = result});
		}
	}
	pthread_mutex_unlock(&worker_lock);
	if (card >= 0) close(card);
	if (record) storage_card_file_job_end();
	return result;
}
