#include "webdavdl.h"

#include "src/system/core/utils.h"
#include "src/system/device/system.h"
#include "src/system/device/usb.h"
#include "src/system/net/http.h"

#include <ctype.h>
#include <dirent.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#define CHUNK_SIZE 32768
#define NAME_MAX_BYTES 150
#define PATH_MAX_BYTES 500
#define MAX_SCAN_DEPTH 4

typedef struct {
	webdav_entry_t *tracks;
	int count;
	bool browse_folder;
	char folder_path[WEBDAV_PATH_MAX];
	char folder_title[WEBDAV_TITLE_MAX];
} dl_job_t;

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static webdavdl_state_t state = WEBDAVDL_IDLE;
static webdavdl_progress_t progress;
static volatile bool cancel_asked;
static int active_connect_fd = -1;
static http_stream_t *active_stream;

static void wipe_bytes(void *buf, size_t len) {
	volatile unsigned char *p = (volatile unsigned char *)buf;
	while (len--) {
		*p++ = 0;
	}
}

static void reason(char *out, size_t size, const char *text) {
	if (out && size) {
		snprintf(out, size, "%s", text ? text : "");
	}
}

const char *webdavdl_root(void) {
	static char root[512];
	const char *sd = storage_sd_root();
	if (!sd || !sd[0]) {
		root[0] = '\0';
	} else if (snprintf(root, sizeof(root), "%s/Music/WebDAV Downloads", sd) >= (int)sizeof(root)) {
		root[0] = '\0';
	}
	return root;
}

bool webdavdl_owns(const char *path) {
	const char *base = webdavdl_root();
	size_t n = strlen(base);
	return n > 0 && path && strncmp(path, base, n) == 0 && (path[n] == '/' || path[n] == '\0');
}

void webdavdl_safe_name(const char *raw, const char *fallback, char *out, size_t size) {
	if (!out || size == 0) {
		return;
	}
	size_t cap = size - 1 < NAME_MAX_BYTES ? size - 1 : NAME_MAX_BYTES;
	size_t n = 0;
	for (const unsigned char *p = (const unsigned char *)(raw ? raw : ""); *p && n < cap; p++) {
		unsigned char c = *p;
		if (c < 0x20 || strchr("/\\*?\"<>|", c)) {
			continue;
		}
		if (c == ':') {
			if (p[1] == ' ' && n + 2 <= cap) {
				out[n++] = ' ';
			}
			c = '-';
		}
		out[n++] = (char)c;
	}
	if (n == cap) {
		while (n > 0 && ((unsigned char)out[n - 1] & 0xC0) == 0x80) {
			n--;
		}
		if (n > 0 && ((unsigned char)out[n - 1] & 0x80)) {
			n--;
		}
	}
	out[n] = '\0';
	while (n > 0 && (out[n - 1] == ' ' || out[n - 1] == '.')) {
		out[--n] = '\0';
	}
	size_t lead = 0;
	while (out[lead] == ' ' || out[lead] == '.') {
		lead++;
	}
	if (lead) {
		memmove(out, out + lead, n - lead + 1);
	}
	if (!out[0]) {
		snprintf(out, size, "%s", fallback ? fallback : "WebDAV");
	}
}

static bool extract_stem_and_ext(const webdav_entry_t *track, char *stem_out, size_t stem_size,
								 char *ext_out, size_t ext_size) {
	if (!track || track->kind != WEBDAV_TRACK || !track->path[0]) {
		return false;
	}
	const char *base_name = strrchr(track->path, '/');
	base_name = base_name ? base_name + 1 : track->path;

	const char *raw_ext = track->suffix[0] ? track->suffix : NULL;
	const char *dot = strrchr(base_name, '.');
	if (!raw_ext && dot && dot[1]) {
		raw_ext = dot + 1;
	}
	if (!raw_ext || !webdav_is_audio_suffix(raw_ext)) {
		return false;
	}
	size_t k = 0;
	for (; raw_ext[k] && k + 1 < ext_size; k++) {
		ext_out[k] = (char)tolower((unsigned char)raw_ext[k]);
	}
	ext_out[k] = '\0';

	char raw_stem[WEBDAV_TITLE_MAX];
	if (dot && dot > base_name && webdav_is_audio_suffix(dot + 1)) {
		size_t slen = (size_t)(dot - base_name);
		if (slen >= sizeof(raw_stem)) {
			slen = sizeof(raw_stem) - 1;
		}
		memcpy(raw_stem, base_name, slen);
		raw_stem[slen] = '\0';
	} else {
		size_t slen = strlen(base_name);
		if (slen >= sizeof(raw_stem)) {
			slen = sizeof(raw_stem) - 1;
		}
		memcpy(raw_stem, base_name, slen);
		raw_stem[slen] = '\0';
	}
	webdavdl_safe_name(raw_stem, "Track", stem_out, stem_size);
	return true;
}

bool webdavdl_path(const webdav_entry_t *track, char *out, size_t size) {
	if (!track || !out || size == 0) {
		return false;
	}
	const char *base = webdavdl_root();
	if (!base[0]) {
		return false;
	}
	char stem[200], ext[16];
	if (!extract_stem_and_ext(track, stem, sizeof(stem), ext, sizeof(ext))) {
		return false;
	}

	// Sanitize each parent folder segment in `track->path`.
	char rel_dirs[512] = "";
	char last_dir[160] = "";
	const char *last_slash = strrchr(track->path, '/');
	if (last_slash) {
		size_t dir_span = (size_t)(last_slash - track->path);
		size_t pos = 0;
		size_t i = 0;
		while (i < dir_span) {
			while (i < dir_span && track->path[i] == '/') {
				i++;
			}
			size_t start = i;
			while (i < dir_span && track->path[i] != '/') {
				i++;
			}
			if (i > start) {
				char raw_seg[WEBDAV_TITLE_MAX], safe_seg[160];
				size_t len = i - start;
				if (len >= sizeof(raw_seg)) {
					len = sizeof(raw_seg) - 1;
				}
				memcpy(raw_seg, track->path + start, len);
				raw_seg[len] = '\0';
				webdavdl_safe_name(raw_seg, "Folder", safe_seg, sizeof(safe_seg));
				snprintf(last_dir, sizeof(last_dir), "%s", safe_seg);
				int w = snprintf(rel_dirs + pos, sizeof(rel_dirs) - pos, "/%s", safe_seg);
				if (w > 0 && pos + (size_t)w < sizeof(rel_dirs)) {
					pos += (size_t)w;
				}
			}
		}
	}

	int n = snprintf(out, size, "%s%s/%s.%s", base, rel_dirs, stem, ext);
	if (n > 0 && n <= PATH_MAX_BYTES && (size_t)n < size) {
		return true;
	}
	// Fallback when deeply nested folders exceed FAT path budget: keep immediate album folder.
	if (last_dir[0]) {
		n = snprintf(out, size, "%s/%s/%s.%s", base, last_dir, stem, ext);
		if (n > 0 && n <= PATH_MAX_BYTES && (size_t)n < size) {
			return true;
		}
	}
	n = snprintf(out, size, "%s/%s.%s", base, stem, ext);
	return n > 0 && n <= PATH_MAX_BYTES && (size_t)n < size;
}

bool webdavdl_exists(const webdav_entry_t *track) {
	char path[560];
	struct stat st;
	return webdavdl_path(track, path, sizeof(path)) && stat(path, &st) == 0 &&
		   S_ISREG(st.st_mode) && st.st_size > 0;
}

static bool card_ok(const char *card) {
	return storage_card_attached() && storage_sd_writable() && !usb_storage_active() &&
		   storage_sd_root() && card[0] && strcmp(storage_sd_root(), card) == 0;
}

static bool ensure_dir_chain(const char *file_path, const char *card) {
	if (!card_ok(card)) {
		return false;
	}
	char dir[560];
	snprintf(dir, sizeof(dir), "%s", file_path);
	char *last_slash = strrchr(dir, '/');
	if (!last_slash || last_slash == dir) {
		return false;
	}
	*last_slash = '\0';

	size_t card_len = strlen(card);
	if (strncmp(dir, card, card_len) != 0) {
		return false;
	}
	for (char *p = dir + card_len + 1; *p; p++) {
		if (*p == '/') {
			*p = '\0';
			struct stat st;
			if (mkdir(dir, 0755) != 0 && !(stat(dir, &st) == 0 && S_ISDIR(st.st_mode))) {
				return false;
			}
			*p = '/';
		}
	}
	struct stat st;
	return mkdir(dir, 0755) == 0 || (stat(dir, &st) == 0 && S_ISDIR(st.st_mode));
}

static bool dl_socket_cb(void *user, int fd) {
	(void)user;
	pthread_mutex_lock(&lock);
	bool stopped = cancel_asked;
	active_connect_fd = stopped ? -1 : fd;
	pthread_mutex_unlock(&lock);
	return !stopped;
}

static bool download_one(const webdav_entry_t *track, const char *card) {
	char dest[560];
	if (!webdavdl_path(track, dest, sizeof(dest))) {
		pthread_mutex_lock(&lock);
		reason(progress.error, sizeof(progress.error), "webdav_invalid_path");
		pthread_mutex_unlock(&lock);
		return false;
	}
	struct stat existing;
	if (stat(dest, &existing) == 0 && S_ISREG(existing.st_mode) && existing.st_size > 0) {
		return true; // already downloaded
	}
	if (!ensure_dir_chain(dest, card)) {
		pthread_mutex_lock(&lock);
		reason(progress.error, sizeof(progress.error),
			   card_ok(card) ? "webdav_download_write_failed" : "webdav_no_card");
		pthread_mutex_unlock(&lock);
		return false;
	}

	char part[580];
	snprintf(part, sizeof(part), "%s.part", dest);
	remove(part);

	char url[WEBDAV_URL_MAX];
	char auth[WEBDAV_AUTH_HEADER_MAX];
	char req_err[96] = "";
	if (!webdav_file_request_info(track->path, url, sizeof(url), auth, sizeof(auth), req_err, sizeof(req_err))) {
		wipe_bytes(url, sizeof(url));
		wipe_bytes(auth, sizeof(auth));
		pthread_mutex_lock(&lock);
		reason(progress.error, sizeof(progress.error), req_err[0] ? req_err : "webdav_stream_failed");
		pthread_mutex_unlock(&lock);
		return false;
	}

	http_stream_t *stream = calloc(1, sizeof(*stream));
	if (!stream) {
		wipe_bytes(url, sizeof(url));
		wipe_bytes(auth, sizeof(auth));
		pthread_mutex_lock(&lock);
		reason(progress.error, sizeof(progress.error), "webdav_out_of_memory");
		pthread_mutex_unlock(&lock);
		return false;
	}

	int status = 0;
	http_stream_options_t opts = {
		.socket_cb = dl_socket_cb,
		.no_icy = true,
		.no_https_downgrade = true,
		.extra_headers = auth[0] ? auth : NULL,
		.status_out = &status,
	};
	bool opened = http_stream_open_ex(stream, url, 30, &opts);
	wipe_bytes(url, sizeof(url));
	wipe_bytes(auth, sizeof(auth));

	pthread_mutex_lock(&lock);
	active_connect_fd = -1;
	if (!opened) {
		if (cancel_asked) {
			reason(progress.error, sizeof(progress.error), "webdav_download_cancelled");
		} else if (status == 401 || status == 403) {
			reason(progress.error, sizeof(progress.error), "webdav_auth_failed");
		} else if (status == 404) {
			reason(progress.error, sizeof(progress.error), "webdav_not_found");
		} else {
			reason(progress.error, sizeof(progress.error), "webdav_stream_failed");
		}
		pthread_mutex_unlock(&lock);
		free(stream);
		return false;
	}
	progress.bytes_total = stream->content_length64;
	progress.bytes_done = 0;
	active_stream = stream;
	pthread_mutex_unlock(&lock);

	FILE *f = fopen(part, "wb");
	char *buf = f ? malloc(CHUNK_SIZE) : NULL;
	bool ok = (f != NULL && buf != NULL);
	if (!ok) {
		pthread_mutex_lock(&lock);
		reason(progress.error, sizeof(progress.error), "webdav_download_write_failed");
		pthread_mutex_unlock(&lock);
	}

	uint64_t done = 0;
	while (ok) {
		if (cancel_asked) {
			ok = false;
			pthread_mutex_lock(&lock);
			reason(progress.error, sizeof(progress.error), "webdav_download_cancelled");
			pthread_mutex_unlock(&lock);
			break;
		}
		if (!card_ok(card)) {
			ok = false;
			pthread_mutex_lock(&lock);
			reason(progress.error, sizeof(progress.error), "webdav_no_card");
			pthread_mutex_unlock(&lock);
			break;
		}
		int n = http_stream_read(stream, buf, CHUNK_SIZE);
		if (n < 0) {
			ok = false;
			pthread_mutex_lock(&lock);
			reason(progress.error, sizeof(progress.error),
				   cancel_asked ? "webdav_download_cancelled" : "webdav_stream_failed");
			pthread_mutex_unlock(&lock);
			break;
		}
		if (n == 0) {
			break;
		}
		if (fwrite(buf, 1, (size_t)n, f) != (size_t)n) {
			ok = false;
			pthread_mutex_lock(&lock);
			reason(progress.error, sizeof(progress.error), "webdav_download_write_failed");
			pthread_mutex_unlock(&lock);
			break;
		}
		done += (uint64_t)n;
		pthread_mutex_lock(&lock);
		progress.bytes_done = done;
		pthread_mutex_unlock(&lock);
	}

	free(buf);
	if (f) {
		if (ok && (fflush(f) != 0 || fsync(fileno(f)) != 0)) {
			ok = false;
			pthread_mutex_lock(&lock);
			reason(progress.error, sizeof(progress.error), "webdav_download_write_failed");
			pthread_mutex_unlock(&lock);
		}
		if (fclose(f) != 0 && ok) {
			ok = false;
			pthread_mutex_lock(&lock);
			reason(progress.error, sizeof(progress.error), "webdav_download_write_failed");
			pthread_mutex_unlock(&lock);
		}
	}

	if (ok && (done == 0 || (stream->content_length64 > 0 && done != stream->content_length64))) {
		ok = false;
		pthread_mutex_lock(&lock);
		reason(progress.error, sizeof(progress.error), "webdav_stream_truncated");
		pthread_mutex_unlock(&lock);
	}

	pthread_mutex_lock(&lock);
	active_stream = NULL;
	pthread_mutex_unlock(&lock);
	http_stream_close(stream);
	free(stream);

	if (!ok || rename(part, dest) != 0) {
		remove(part);
		if (ok) {
			pthread_mutex_lock(&lock);
			reason(progress.error, sizeof(progress.error), "webdav_download_write_failed");
			pthread_mutex_unlock(&lock);
		}
		return false;
	}
	return true;
}

static bool collect_folder_tracks(const char *rel_path, int depth,
								  webdav_entry_t *out, int *count, int max_tracks,
								  char *err, size_t err_size) {
	if (depth > MAX_SCAN_DEPTH || *count >= max_tracks || cancel_asked) {
		return true;
	}
	webdav_entry_t *page = calloc(WEBDAV_PAGE_SIZE, sizeof(*page));
	if (!page) {
		reason(err, err_size, "webdav_out_of_memory");
		return false;
	}
	int offset = 0;
	bool more = true;
	while (more && *count < max_tracks && !cancel_asked) {
		int n = webdav_browse(rel_path, offset, page, WEBDAV_PAGE_SIZE, &more, err, err_size);
		if (n < 0) {
			free(page);
			return false;
		}
		if (n == 0) {
			break;
		}
		for (int i = 0; i < n && *count < max_tracks && !cancel_asked; i++) {
			if (page[i].kind == WEBDAV_TRACK) {
				out[(*count)++] = page[i];
			} else if (page[i].kind == WEBDAV_DIR && depth < MAX_SCAN_DEPTH) {
				if (!collect_folder_tracks(page[i].path, depth + 1, out, count, max_tracks, err, err_size)) {
					free(page);
					return false;
				}
			}
		}
		offset += n;
	}
	free(page);
	return true;
}

static void *dl_worker(void *arg) {
	dl_job_t *job = (dl_job_t *)arg;
	thread_be_low_priority("webdav save");

	char card[512];
	snprintf(card, sizeof(card), "%s", storage_sd_root() ? storage_sd_root() : "");

	if (job->browse_folder) {
		webdav_entry_t *found = calloc(WEBDAV_QUEUE_MAX, sizeof(*found));
		char scan_err[128] = "";
		int found_count = 0;
		bool ok = found && collect_folder_tracks(job->folder_path, 0, found, &found_count,
												 WEBDAV_QUEUE_MAX, scan_err, sizeof(scan_err));
		if (!ok || (!cancel_asked && found_count == 0)) {
			free(found);
			free(job);
			pthread_mutex_lock(&lock);
			if (cancel_asked) {
				state = WEBDAVDL_CANCELLED;
				progress.state = WEBDAVDL_CANCELLED;
				reason(progress.error, sizeof(progress.error), "webdav_download_cancelled");
			} else {
				state = WEBDAVDL_FAILED;
				progress.state = WEBDAVDL_FAILED;
				reason(progress.error, sizeof(progress.error),
					   scan_err[0] ? scan_err : "webdav_empty");
			}
			pthread_mutex_unlock(&lock);
			return NULL;
		}
		job->tracks = found;
		job->count = found_count;
		pthread_mutex_lock(&lock);
		progress.total = found_count;
		pthread_mutex_unlock(&lock);
	}

	int completed = 0;
	int failed = 0;
	for (int i = 0; i < job->count && !cancel_asked; i++) {
		pthread_mutex_lock(&lock);
		progress.current = i + 1;
		progress.bytes_done = 0;
		progress.bytes_total = 0;
		snprintf(progress.title, sizeof(progress.title), "%s", job->tracks[i].title);
		pthread_mutex_unlock(&lock);

		if (download_one(&job->tracks[i], card)) {
			completed++;
		} else if (!cancel_asked) {
			failed++;
		}

		pthread_mutex_lock(&lock);
		progress.completed = completed;
		progress.failed = failed;
		pthread_mutex_unlock(&lock);
	}

	pthread_mutex_lock(&lock);
	if (cancel_asked) {
		state = WEBDAVDL_CANCELLED;
		progress.state = WEBDAVDL_CANCELLED;
		reason(progress.error, sizeof(progress.error), "webdav_download_cancelled");
	} else if (completed == 0 && failed > 0) {
		state = WEBDAVDL_FAILED;
		progress.state = WEBDAVDL_FAILED;
	} else {
		state = WEBDAVDL_DONE;
		progress.state = WEBDAVDL_DONE;
	}
	pthread_mutex_unlock(&lock);

	free(job->tracks);
	free(job);
	return NULL;
}

static bool preflight_check(char *error, size_t error_size) {
	if (!webdav_configured()) {
		reason(error, error_size, "webdav_not_configured");
		return false;
	}
	if (usb_storage_active()) {
		reason(error, error_size, "webdav_no_card");
		return false;
	}
	if (!storage_card_attached() || !storage_sd_writable() || !webdavdl_root()[0]) {
		reason(error, error_size, "webdav_no_card");
		return false;
	}
	return true;
}

bool webdavdl_start_tracks(const webdav_entry_t *tracks, int count, int index,
						   char *error, size_t error_size) {
	reason(error, error_size, "");
	if (!tracks || count <= 0 || index >= count) {
		reason(error, error_size, "webdav_invalid_path");
		return false;
	}
	if (!preflight_check(error, error_size)) {
		return false;
	}

	int cap = index >= 0 ? 1 : count;
	webdav_entry_t *copy = calloc((size_t)cap, sizeof(*copy));
	if (!copy) {
		reason(error, error_size, "webdav_out_of_memory");
		return false;
	}
	int n = 0;
	if (index >= 0) {
		if (tracks[index].kind == WEBDAV_TRACK) {
			copy[n++] = tracks[index];
		}
	} else {
		for (int i = 0; i < count; i++) {
			if (tracks[i].kind == WEBDAV_TRACK) {
				copy[n++] = tracks[i];
			}
		}
	}
	if (n == 0) {
		free(copy);
		reason(error, error_size, "webdav_empty");
		return false;
	}

	dl_job_t *job = calloc(1, sizeof(*job));
	if (!job) {
		free(copy);
		reason(error, error_size, "webdav_out_of_memory");
		return false;
	}
	job->tracks = copy;
	job->count = n;

	pthread_mutex_lock(&lock);
	if (state == WEBDAVDL_RUNNING) {
		pthread_mutex_unlock(&lock);
		free(copy);
		free(job);
		reason(error, error_size, "webdav_download_busy");
		return false;
	}
	cancel_asked = false;
	active_connect_fd = -1;
	active_stream = NULL;
	memset(&progress, 0, sizeof(progress));
	state = WEBDAVDL_RUNNING;
	progress.state = WEBDAVDL_RUNNING;
	progress.current = 1;
	progress.total = n;
	snprintf(progress.title, sizeof(progress.title), "%s", copy[0].title);

	pthread_t tid;
	if (pthread_create(&tid, NULL, dl_worker, job) != 0) {
		state = WEBDAVDL_IDLE;
		progress.state = WEBDAVDL_IDLE;
		pthread_mutex_unlock(&lock);
		free(copy);
		free(job);
		reason(error, error_size, "webdav_out_of_memory");
		return false;
	}
	pthread_detach(tid);
	pthread_mutex_unlock(&lock);
	return true;
}

bool webdavdl_start_folder(const char *folder_rel_path, const char *folder_title,
						   char *error, size_t error_size) {
	reason(error, error_size, "");
	if (!folder_rel_path) {
		reason(error, error_size, "webdav_invalid_path");
		return false;
	}
	if (!preflight_check(error, error_size)) {
		return false;
	}

	dl_job_t *job = calloc(1, sizeof(*job));
	if (!job) {
		reason(error, error_size, "webdav_out_of_memory");
		return false;
	}
	job->browse_folder = true;
	snprintf(job->folder_path, sizeof(job->folder_path), "%s", folder_rel_path);
	snprintf(job->folder_title, sizeof(job->folder_title), "%s",
			 (folder_title && folder_title[0]) ? folder_title : folder_rel_path);

	pthread_mutex_lock(&lock);
	if (state == WEBDAVDL_RUNNING) {
		pthread_mutex_unlock(&lock);
		free(job);
		reason(error, error_size, "webdav_download_busy");
		return false;
	}
	cancel_asked = false;
	active_connect_fd = -1;
	active_stream = NULL;
	memset(&progress, 0, sizeof(progress));
	state = WEBDAVDL_RUNNING;
	progress.state = WEBDAVDL_RUNNING;
	progress.current = 0;
	progress.total = 0;
	snprintf(progress.title, sizeof(progress.title), "%s", job->folder_title);

	pthread_t tid;
	if (pthread_create(&tid, NULL, dl_worker, job) != 0) {
		state = WEBDAVDL_IDLE;
		progress.state = WEBDAVDL_IDLE;
		pthread_mutex_unlock(&lock);
		free(job);
		reason(error, error_size, "webdav_out_of_memory");
		return false;
	}
	pthread_detach(tid);
	pthread_mutex_unlock(&lock);
	return true;
}

void webdavdl_cancel(void) {
	pthread_mutex_lock(&lock);
	cancel_asked = true;
	if (active_connect_fd >= 0) {
		shutdown(active_connect_fd, SHUT_RDWR);
	}
	if (active_stream) {
		http_stream_wake(active_stream);
	}
	pthread_mutex_unlock(&lock);
}

void webdavdl_progress(webdavdl_progress_t *out) {
	if (!out) {
		return;
	}
	pthread_mutex_lock(&lock);
	*out = progress;
	pthread_mutex_unlock(&lock);
}

bool webdavdl_busy(void) {
	pthread_mutex_lock(&lock);
	bool busy = (state == WEBDAVDL_RUNNING);
	pthread_mutex_unlock(&lock);
	return busy;
}

void webdavdl_acknowledge(void) {
	pthread_mutex_lock(&lock);
	if (state != WEBDAVDL_RUNNING) {
		state = WEBDAVDL_IDLE;
		progress.state = WEBDAVDL_IDLE;
	}
	pthread_mutex_unlock(&lock);
}

static int compare_dl_entries(const void *a, const void *b) {
	const webdavdl_entry_t *ea = (const webdavdl_entry_t *)a;
	const webdavdl_entry_t *eb = (const webdavdl_entry_t *)b;
	if (ea->modified != eb->modified) {
		return ea->modified < eb->modified ? 1 : -1;
	}
	return strcmp(ea->path, eb->path);
}

static void scan_downloads(const char *root, const char *dir, int depth,
						   webdavdl_entry_t *entries, int max, int *count) {
	DIR *d = opendir(dir);
	if (!d) {
		return;
	}
	struct dirent *de;
	while ((de = readdir(d)) != NULL) {
		if (de->d_name[0] == '.') {
			continue;
		}
		char full[560];
		int n = snprintf(full, sizeof(full), "%s/%s", dir, de->d_name);
		if (n <= 0 || n > PATH_MAX_BYTES || (size_t)n >= sizeof(full)) {
			continue;
		}
		struct stat st;
		if (lstat(full, &st) != 0) {
			continue;
		}
		if (S_ISDIR(st.st_mode)) {
			if (depth < MAX_SCAN_DEPTH + 1) {
				scan_downloads(root, full, depth + 1, entries, max, count);
			}
			continue;
		}
		if (!S_ISREG(st.st_mode) || st.st_size <= 0) {
			continue;
		}
		const char *dot = strrchr(de->d_name, '.');
		if (!dot || !dot[1] || !webdav_is_audio_suffix(dot + 1)) {
			continue;
		}
		if (*count >= max) {
			continue;
		}
		if (strlen(full) >= sizeof(entries[0].path)) {
			continue;
		}
		webdavdl_entry_t *e = &entries[(*count)++];
		memset(e, 0, sizeof(*e));
		memcpy(e->path, full, strlen(full) + 1);
		e->modified = (long long)st.st_mtime;

		size_t stem_len = (size_t)(dot - de->d_name);
		if (stem_len >= sizeof(e->title)) {
			stem_len = sizeof(e->title) - 1;
		}
		memcpy(e->title, de->d_name, stem_len);
		e->title[stem_len] = '\0';

		size_t rlen = strlen(root);
		if (strncmp(dir, root, rlen) == 0 && dir[rlen] == '/') {
			snprintf(e->folder, sizeof(e->folder), "%s", dir + rlen + 1);
		}
	}
	closedir(d);
}

int webdavdl_history(webdavdl_entry_t *entries, int max) {
	if (!entries || max <= 0) {
		return 0;
	}
	const char *root = webdavdl_root();
	if (!root[0] || !storage_card_attached()) {
		return 0;
	}
	int count = 0;
	scan_downloads(root, root, 0, entries, max, &count);
	if (count > 1) {
		qsort(entries, (size_t)count, sizeof(*entries), compare_dl_entries);
	}
	return count;
}

bool webdavdl_delete(const char *path) {
	const char *root = webdavdl_root();
	if (!path || !root[0] || !webdavdl_owns(path) || strcmp(path, root) == 0 ||
		!storage_card_attached() || !storage_sd_writable() || usb_storage_active()) {
		return false;
	}
	if (unlink(path) != 0) {
		return false;
	}
	// Prune empty parent folders up to (but excluding) `root`.
	char folder[560];
	snprintf(folder, sizeof(folder), "%s", path);
	for (int level = 0; level <= MAX_SCAN_DEPTH + 1; level++) {
		char *slash = strrchr(folder, '/');
		if (!slash) {
			break;
		}
		*slash = '\0';
		if (strcmp(folder, root) == 0 || strlen(folder) <= strlen(root)) {
			break;
		}
		if (rmdir(folder) != 0) {
			break;
		}
	}
	return true;
}
