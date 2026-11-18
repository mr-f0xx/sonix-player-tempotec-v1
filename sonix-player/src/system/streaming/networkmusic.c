#define _GNU_SOURCE 1
#include "networkmusic.h"

#include "src/system/core/utils.h"
#include "src/system/device/system.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

static pthread_mutex_t job_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t disk_lock = PTHREAD_MUTEX_INITIALIZER;
static atomic_bool cancelled;
static bool running, result_ready;
static networkmusic_result_t result;
static networkmusic_progress_t progress;

// Only the file callbacks and the short local prepare/publish steps hold this
// lock. USB can take all three descriptors away without waiting for DNS, TLS
// or a stalled server. Downloads never open the card by pathname again.
static struct {
	int root, dir, part;
	char temp[64];
} disk = {.root = -1, .dir = -1, .part = -1};

typedef struct {
	share_server_t server;
	char folder[SHARE_FOLDER_MAX];
	char card[NETWORKMUSIC_LOCAL_PATH];
	size_t count; // zero means list the folder instead
	share_entry_t entries[];
} job_t;

static bool is_cancelled(void *user) {
	(void)user;
	return atomic_load(&cancelled);
}

static void remove_part(void) {
	if (disk.part >= 0) close(disk.part);
	if (disk.temp[0]) unlinkat(disk.dir, disk.temp, 0);
	disk.part = -1;
	disk.temp[0] = '\0';
}

static void close_card(void) {
	remove_part();
	if (disk.dir >= 0) close(disk.dir);
	if (disk.root >= 0) close(disk.root);
	disk.dir = disk.root = -1;
}

void networkmusic_release_card(void) {
	pthread_mutex_lock(&disk_lock);
	if (disk.root >= 0) {
		atomic_store(&cancelled, true);
		close_card();
	}
	pthread_mutex_unlock(&disk_lock);
}

static share_status_t disk_error(void) {
	if (errno == ENOSPC || errno == EDQUOT) return SHARE_CARD_FULL;
	if (errno == EFBIG) return SHARE_FILE_TOO_LARGE;
	if (errno == ENAMETOOLONG) return SHARE_PATH_TOO_LONG;
	return SHARE_DISK_ERROR;
}

// Escape FAT-forbidden bytes instead of substituting '_' (which would make
// "A:B.flac" and "A_B.flac" collide). Escape '%' too so this map is reversible
// apart from FAT's own case folding. The audio extension stays unchanged.
static bool card_name(const char *name, char out[SHARE_NAME_MAX], size_t len) {
	static const char hex[] = "0123456789ABCDEF";
	size_t used = 0, end = len;
	while (end && (name[end - 1] == ' ' || name[end - 1] == '.')) end--;
	for (size_t i = 0; i < len; i++) {
		unsigned char c = (unsigned char)name[i];
		if (i >= end || strchr("<>:\"\\|?*%", c)) {
			if (used + 3 >= SHARE_NAME_MAX) return false;
			out[used++] = '%';
			out[used++] = hex[c >> 4];
			out[used++] = hex[c & 15];
		} else {
			if (used + 1 >= SHARE_NAME_MAX) return false;
			out[used++] = (char)c;
		}
	}
	out[used] = '\0';
	return used > 0;
}

static bool append_path(char *path, const char *name) {
	size_t used = strlen(path), n = strlen(name);
	if (used + 1 + n >= NETWORKMUSIC_LOCAL_PATH) return false;
	if (!used || path[used - 1] != '/') path[used++] = '/';
	memcpy(path + used, name, n + 1);
	return true;
}

static share_status_t enter_dir(const char *name, char *path, dev_t device) {
	if (!append_path(path, name)) return SHARE_PATH_TOO_LONG;
	int parent = disk.dir >= 0 ? disk.dir : disk.root;
	if (mkdirat(parent, name, 0755) < 0 && errno != EEXIST) return disk_error();
	int fd = openat(parent, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
	if (fd < 0) return disk_error();
	struct stat st;
	if (fstat(fd, &st) < 0 || st.st_dev != device) {
		close(fd);
		return SHARE_DISK_ERROR;
	}
	if (disk.dir >= 0) close(disk.dir);
	disk.dir = fd;
	return SHARE_OK;
}

static share_status_t prepare_folder(const job_t *job, char *path) {
	if (disk.root < 0 || is_cancelled(NULL)) return SHARE_CANCELLED;
	struct stat st;
	if (fstat(disk.root, &st) < 0) return disk_error();
	strcpy(path, job->card);
	const char *authority = strstr(job->server.origin, "://");
	if (!authority || !authority[3]) return SHARE_BAD_ADDRESS;
	char server[SHARE_NAME_MAX];
	if (!card_name(authority + 3, server, strlen(authority + 3))) return SHARE_PATH_TOO_LONG;
	const char *components[] = {"Music", "Network Downloads", job->server.kind == SHARE_WEBDAV ? "WebDAV" : "FTP", server};
	for (size_t i = 0; i < sizeof(components) / sizeof(components[0]); i++) {
		share_status_t status = enter_dir(components[i], path, st.st_dev);
		if (status != SHARE_OK) return status;
	}
	for (const char *p = job->folder + 1; *p;) {
		size_t n = strcspn(p, "/");
		char name[SHARE_NAME_MAX];
		if (!card_name(p, name, n)) return SHARE_PATH_TOO_LONG;
		share_status_t status = enter_dir(name, path, st.st_dev);
		if (status != SHARE_OK) return status;
		p += n + 1;
	}
	return SHARE_OK;
}

static share_status_t existing_file(const char *name) {
	struct stat st;
	if (fstatat(disk.dir, name, &st, AT_SYMLINK_NOFOLLOW) == 0) {
		return S_ISREG(st.st_mode) ? SHARE_EXISTS : SHARE_DISK_ERROR;
	}
	return errno == ENOENT ? SHARE_OK : disk_error();
}

static share_status_t prepare_file(const char *name) {
	if (disk.dir < 0 || is_cancelled(NULL)) return SHARE_CANCELLED;
	share_status_t status = existing_file(name);
	if (status != SHARE_OK) return status;
	for (unsigned i = 0; i < 1024; i++) {
		char part[sizeof(disk.temp)];
		snprintf(part, sizeof(part), ".sonix-network-%ld-%u.part", (long)getpid(), i);
		int fd = openat(disk.dir, part, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0644);
		if (fd >= 0) {
			disk.part = fd;
			strcpy(disk.temp, part);
			return SHARE_OK;
		}
		if (errno != EEXIST) return disk_error();
	}
	return SHARE_DISK_ERROR;
}

static share_status_t write_part(void *unused, const void *data, size_t size) {
	(void)unused;
	const char *p = data;
	size_t left = size;
	share_status_t status = SHARE_OK;
	pthread_mutex_lock(&disk_lock);
	while (left) {
		if (disk.part < 0 || is_cancelled(NULL)) { status = SHARE_CANCELLED; break; }
		ssize_t n = write(disk.part, p, left);
		if (n < 0 && errno == EINTR) continue;
		if (n <= 0) { status = n < 0 ? disk_error() : SHARE_DISK_ERROR; break; }
		p += n;
		left -= (size_t)n;
	}
	pthread_mutex_unlock(&disk_lock);
	pthread_mutex_lock(&job_lock);
	progress.bytes_done += size - left;
	pthread_mutex_unlock(&job_lock);
	return status;
}

static void content_size(void *unused, uint64_t size) {
	(void)unused;
	pthread_mutex_lock(&job_lock);
	progress.bytes_total = size;
	pthread_mutex_unlock(&job_lock);
}

static share_status_t publish_file(const char *name) {
	if (disk.part < 0 || is_cancelled(NULL)) return SHARE_CANCELLED;
	if (fsync(disk.part) < 0) return disk_error();
	int error = close(disk.part);
	disk.part = -1;
	if (error) return disk_error();
	if (is_cancelled(NULL)) return SHARE_CANCELLED;
#ifdef SYS_renameat2
	if (syscall(SYS_renameat2, disk.dir, disk.temp, disk.dir, name, 1U /* RENAME_NOREPLACE */) == 0) {
		disk.temp[0] = '\0';
		return SHARE_OK;
	}
	if (errno == EEXIST) return existing_file(name) == SHARE_EXISTS ? SHARE_EXISTS : SHARE_DISK_ERROR;
	if (errno != ENOSYS && errno != EINVAL && errno != EOPNOTSUPP) return disk_error();
#endif
	// Older V1 kernels/FAT drivers lack RENAME_NOREPLACE. Try an atomic link
	// on filesystems supporting it, otherwise recheck just before rename.
	if (linkat(disk.dir, disk.temp, disk.dir, name, 0) == 0) return SHARE_OK;
	if (errno != EEXIST && errno != EPERM && errno != ENOSYS && errno != EOPNOTSUPP) return disk_error();
	share_status_t status = existing_file(name);
	if (status != SHARE_OK) return status;
	if (renameat(disk.dir, disk.temp, disk.dir, name) < 0) return disk_error();
	disk.temp[0] = '\0';
	return SHARE_OK;
}

static share_status_t save_batch(job_t *job, networkmusic_result_t *out) {
	char folder[NETWORKMUSIC_LOCAL_PATH];
	pthread_mutex_lock(&disk_lock);
	share_status_t status = prepare_folder(job, folder);
	pthread_mutex_unlock(&disk_lock);
	if (status != SHARE_OK) return status;
	for (size_t i = 0; i < job->count; i++) {
		const char *remote = job->entries[i].name;
		char name[SHARE_NAME_MAX], local[NETWORKMUSIC_LOCAL_PATH];
		if (!card_name(remote, name, strlen(remote))) return SHARE_PATH_TOO_LONG;
		strcpy(local, folder);
		if (!append_path(local, name)) return SHARE_PATH_TOO_LONG;
		pthread_mutex_lock(&job_lock);
		progress.bytes_done = progress.bytes_total = 0;
		pthread_mutex_unlock(&job_lock);
		pthread_mutex_lock(&disk_lock);
		status = prepare_file(name);
		pthread_mutex_unlock(&disk_lock);
		if (status == SHARE_OK) {
			status = networkshare_fetch(&job->server, job->folder, remote, is_cancelled, write_part, content_size, NULL);
			pthread_mutex_lock(&disk_lock);
			if (status == SHARE_OK) status = publish_file(name);
			remove_part();
			pthread_mutex_unlock(&disk_lock);
		}
		if (status == SHARE_EXISTS) out->existing++;
		else if (status == SHARE_OK) out->saved++;
		else return status;
		if (job->count == 1) strcpy(out->path, local);
		pthread_mutex_lock(&job_lock);
		progress.completed++;
		pthread_mutex_unlock(&job_lock);
	}
	return SHARE_OK;
}

static void *run_job(void *arg) {
	job_t *job = arg;
	thread_be_low_priority("network folders");
	networkmusic_result_t out = {0};
	if (job->count) {
		out.status = save_batch(job, &out);
		pthread_mutex_lock(&disk_lock);
		close_card();
		pthread_mutex_unlock(&disk_lock);
	} else {
		out.listing = malloc(sizeof(*out.listing));
		out.status = out.listing ? networkshare_list(&job->server, job->folder, out.listing, is_cancelled, NULL) : SHARE_NO_MEMORY;
		if (out.status != SHARE_OK) {
			free(out.listing);
			out.listing = NULL;
		}
	}
	free(job);
	pthread_mutex_lock(&job_lock);
	result = out;
	running = false;
	result_ready = true;
	pthread_mutex_unlock(&job_lock);
	return NULL;
}

static share_status_t start_job(job_t *job) {
	int card = -1;
	bool writing = job->count != 0;
	if (writing) {
		card = storage_card_file_job_begin(true);
		if (card < 0) return SHARE_NO_CARD;
		const char *root = storage_sd_root();
		if (!root || strlen(root) >= sizeof(job->card)) {
			close(card);
			storage_card_file_job_end();
			return SHARE_PATH_TOO_LONG;
		}
		strcpy(job->card, root);
	}
	// Lock order for registration is card -> job -> disk. The worker and
	// release path never acquire the card lock or hold both inner locks.
	pthread_mutex_lock(&job_lock);
	share_status_t status = SHARE_BUSY;
	if (!running && !result_ready) {
		atomic_store(&cancelled, false);
		progress = (networkmusic_progress_t){.total = job->count};
		pthread_mutex_lock(&disk_lock);
		disk.root = card;
		pthread_mutex_unlock(&disk_lock);
		pthread_t thread;
		if (pthread_create(&thread, NULL, run_job, job) == 0) {
			pthread_detach(thread);
			card = -1; // worker owns the descriptor; card handoff can already find it
			running = true;
			status = SHARE_OK;
		} else {
			pthread_mutex_lock(&disk_lock);
			disk.root = -1;
			pthread_mutex_unlock(&disk_lock);
			status = SHARE_NO_MEMORY;
		}
	}
	pthread_mutex_unlock(&job_lock);
	if (card >= 0) close(card);
	if (writing) storage_card_file_job_end();
	return status;
}

static share_status_t submit(const share_server_t *server, const char *folder, const share_entry_t *entries, size_t count) {
	if (!networkshare_valid_folder(server, folder) || count > SHARE_LIST_MAX || (count && !entries)) {
		return SHARE_BAD_ADDRESS;
	}
	job_t *job = malloc(sizeof(*job) + count * sizeof(job->entries[0]));
	if (!job) return SHARE_NO_MEMORY;
	job->server = *server;
	strcpy(job->folder, folder);
	job->count = 0;
	for (size_t i = 0; i < count; i++) {
		if (!entries[i].directory && networkshare_valid_track(entries[i].name)) {
			job->entries[job->count++] = entries[i];
		}
	}
	share_status_t status = count && !job->count ? SHARE_BAD_ADDRESS : start_job(job);
	if (status != SHARE_OK) free(job);
	return status;
}

share_status_t networkmusic_list_start(const share_server_t *server, const char *folder) {
	return submit(server, folder, NULL, 0);
}

share_status_t networkmusic_save_start(const share_server_t *server, const char *folder,
									  const share_entry_t *entries, size_t count) {
	return count ? submit(server, folder, entries, count) : SHARE_BAD_ADDRESS;
}

void networkmusic_progress(networkmusic_progress_t *out) {
	pthread_mutex_lock(&job_lock);
	*out = progress;
	pthread_mutex_unlock(&job_lock);
}

bool networkmusic_busy(void) {
	pthread_mutex_lock(&job_lock);
	bool busy = running;
	pthread_mutex_unlock(&job_lock);
	return busy;
}

bool networkmusic_take_result(networkmusic_result_t *out) {
	pthread_mutex_lock(&job_lock);
	bool ready = result_ready;
	if (ready) {
		*out = result;
		result = (networkmusic_result_t){0};
		result_ready = false;
	}
	pthread_mutex_unlock(&job_lock);
	return ready;
}

void networkmusic_cancel(void) {
	pthread_mutex_lock(&job_lock);
	if (running) atomic_store(&cancelled, true);
	pthread_mutex_unlock(&job_lock);
}
