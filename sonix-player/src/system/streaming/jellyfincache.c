#include "jellyfincache.h"

#include "../core/config.h"
#include "../core/utils.h"
#include "../decode/growfile.h"
#include "../net/http.h"
#include "streamturn.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>
#include <utime.h>

#define JF_CACHE_SUBDIR ".local/jellyfin-cache"
#define JF_INCOMPLETE_SUFFIX ".incompleto"
#define JF_PATH_MAX 600
#define JF_CHUNK_SIZE (16 * 1024)
#define JF_PREBUFFER_MIN (96L * 1024)
#define JF_PREBUFFER_FLAC (192L * 1024)
#define JF_COVER_MAX_BYTES (512 * 1024)
#define JF_WORKER_STACK (256 * 1024)
#define JF_STREAM_TIMEOUT_SECS 20

static pthread_once_t abandon_once = PTHREAD_ONCE_INIT;
static pthread_mutex_t cache_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cache_cond = PTHREAD_COND_INITIALIZER;

static char cache_dir[JF_PATH_MAX];
static char protected_path[JF_PATH_MAX];
static int cache_max_mb = JELLYFINCACHE_DEFAULT_MAX_MB;
static long long custom_max_bytes = -1; // >= 0 overrides cache_max_mb (tests)

static bool cancel_flag;
static bool active_download;
static bool bg_running;
static int active_sock_fd = -1;
static http_stream_t *active_stream;

typedef struct {
	http_stream_t st;
	FILE *fp;
	char path[JF_PATH_MAX];
	char incomplete_path[JF_PATH_MAX];
	char cover_id[JELLYFIN_ID_MAX];
	long written;
	long total;
} bg_job_t;

static void register_abandon_once(void) {
	streamturn_register_abandon(jellyfincache_cancel);
}

static void copy_text(char *dst, size_t dst_size, const char *src) {
	if (!dst || dst_size == 0)
		return;
	if (!src) {
		dst[0] = '\0';
		return;
	}
	snprintf(dst, dst_size, "%s", src);
}

static void set_error(char *out, size_t out_size, const char *msg) {
	if (out && out_size > 0)
		copy_text(out, out_size, (msg && msg[0]) ? msg : "jellyfin_stream_failed");
}

// ---------------------------------------------------------------------------
// Path & extension helpers
// ---------------------------------------------------------------------------

static bool valid_id(const char *id) {
	if (!id || !id[0])
		return false;
	for (const unsigned char *p = (const unsigned char *)id; *p; p++) {
		if (!(isalnum(*p) || *p == '-' || *p == '_'))
			return false;
	}
	return true;
}

static const char *quality_tag(jellyfin_quality_t q) {
	switch (q) {
	case JELLYFIN_QUALITY_MP3_320:
		return "__m320";
	case JELLYFIN_QUALITY_MP3_192:
		return "__m192";
	case JELLYFIN_QUALITY_OPUS_128:
		return "__o128";
	case JELLYFIN_QUALITY_ORIGINAL:
	default:
		return "";
	}
}

static bool ext_supports_growfile(const char *ext) {
	if (!ext || !ext[0])
		return false;
	return strcasecmp(ext, "mp3") == 0 || strcasecmp(ext, "flac") == 0 || strcasecmp(ext, "opus") == 0;
}

static bool is_audio_ext(const char *ext) {
	if (!ext || !ext[0])
		return false;
	return strcasecmp(ext, "flac") == 0 || strcasecmp(ext, "mp3") == 0 || strcasecmp(ext, "opus") == 0 ||
		   strcasecmp(ext, "ogg") == 0 || strcasecmp(ext, "m4a") == 0 || strcasecmp(ext, "aac") == 0 ||
		   strcasecmp(ext, "wav") == 0 || strcasecmp(ext, "wv") == 0 || strcasecmp(ext, "ape") == 0 ||
		   strcasecmp(ext, "aiff") == 0 || strcasecmp(ext, "dsf") == 0 || strcasecmp(ext, "dff") == 0;
}

static void ext_from_content_type(const char *ct, const char *fallback, char *out, size_t out_size) {
	if (ct && ct[0]) {
		if (strstr(ct, "flac")) {
			copy_text(out, out_size, "flac");
			return;
		}
		if (strstr(ct, "mpeg") || strstr(ct, "mp3")) {
			copy_text(out, out_size, "mp3");
			return;
		}
		if (strstr(ct, "opus")) {
			copy_text(out, out_size, "opus");
			return;
		}
		if (strstr(ct, "ogg")) {
			if (fallback && strcasecmp(fallback, "opus") == 0)
				copy_text(out, out_size, "opus");
			else
				copy_text(out, out_size, "ogg");
			return;
		}
		if (strstr(ct, "mp4") || strstr(ct, "m4a")) {
			copy_text(out, out_size, "m4a");
			return;
		}
		if (strstr(ct, "aac")) {
			copy_text(out, out_size, "aac");
			return;
		}
		if (strstr(ct, "wavpack")) {
			copy_text(out, out_size, "wv");
			return;
		}
		if (strstr(ct, "wav") || strstr(ct, "wave")) {
			copy_text(out, out_size, "wav");
			return;
		}
		if (strstr(ct, "ape")) {
			copy_text(out, out_size, "ape");
			return;
		}
		if (strstr(ct, "aiff") || strstr(ct, "aif")) {
			copy_text(out, out_size, "aiff");
			return;
		}
		if (strstr(ct, "dsf") || strstr(ct, "dsd")) {
			copy_text(out, out_size, "dsf");
			return;
		}
	}
	if (fallback && fallback[0] && is_audio_ext(fallback)) {
		copy_text(out, out_size, fallback);
		return;
	}
	copy_text(out, out_size, "mp3");
}

static bool build_stem(const char *dir, const char *item_id, jellyfin_quality_t quality, char *out, size_t out_size) {
	if (!dir || !dir[0] || !valid_id(item_id))
		return false;
	int n = snprintf(out, out_size, "%s/%s%s", dir, item_id, quality_tag(quality));
	return n > 0 && (size_t)n < out_size;
}

static bool build_audio_path(const char *dir, const char *item_id, jellyfin_quality_t quality, const char *ext,
							 char *out, size_t out_size) {
	char stem[JF_PATH_MAX];
	if (!build_stem(dir, item_id, quality, stem, sizeof(stem)))
		return false;
	const char *e = (ext && ext[0]) ? ext : "mp3";
	int n = snprintf(out, out_size, "%s.%s", stem, e);
	return n > 0 && (size_t)n < out_size;
}

static bool ensure_cache_dir(const char *dir) {
	if (!dir || !dir[0])
		return false;
	struct stat st;
	if (stat(dir, &st) == 0 && S_ISDIR(st.st_mode))
		return true;

	// Create parent `.local` directory first if needed.
	char parent[JF_PATH_MAX];
	copy_text(parent, sizeof(parent), dir);
	char *slash = strrchr(parent, '/');
	if (slash && slash > parent) {
		*slash = '\0';
		if (stat(parent, &st) != 0)
			mkdir(parent, 0755);
	}
	if (mkdir(dir, 0755) != 0 && errno != EEXIST)
		return false;
	return true;
}

static bool file_is_complete(const char *path) {
	struct stat st;
	if (stat(path, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size <= 0)
		return false;
	char inc[JF_PATH_MAX];
	if ((size_t)snprintf(inc, sizeof(inc), "%s%s", path, JF_INCOMPLETE_SUFFIX) >= sizeof(inc))
		return false;
	struct stat ist;
	if (stat(inc, &ist) == 0)
		return false;
	return !growfile_is_growing(path);
}

static void remove_track_and_sidecars(const char *audio_path) {
	if (!audio_path || !audio_path[0])
		return;
	unlink(audio_path);

	char buf[JF_PATH_MAX];
	if ((size_t)snprintf(buf, sizeof(buf), "%s.tags", audio_path) < sizeof(buf))
		unlink(buf);
	if ((size_t)snprintf(buf, sizeof(buf), "%s%s", audio_path, JF_INCOMPLETE_SUFFIX) < sizeof(buf))
		unlink(buf);

	// Remove matching `<stem>.jpg`.
	copy_text(buf, sizeof(buf), audio_path);
	char *dot = strrchr(buf, '.');
	char *slash = strrchr(buf, '/');
	if (dot && (!slash || dot > slash)) {
		*dot = '\0';
		char jpg[JF_PATH_MAX];
		if ((size_t)snprintf(jpg, sizeof(jpg), "%s.jpg", buf) < sizeof(jpg))
			unlink(jpg);
	}
}

// ---------------------------------------------------------------------------
// Configuration & root lifecycle
// ---------------------------------------------------------------------------

static int normalize_max_mb(int mb) {
	if (mb <= 0)
		return 0;
	if (mb < JELLYFINCACHE_MIN_MAX_MB)
		return JELLYFINCACHE_MIN_MAX_MB;
	if (mb > JELLYFINCACHE_MAX_MAX_MB)
		return JELLYFINCACHE_MAX_MAX_MB;
	return mb;
}

void jellyfincache_set_root(const char *sd_root) {
	pthread_once(&abandon_once, register_abandon_once);
	jellyfincache_cancel();

	int cfg_mb = normalize_max_mb((int)config_get_int("jellyfin", "cache_max_mb", JELLYFINCACHE_DEFAULT_MAX_MB));

	pthread_mutex_lock(&cache_lock);
	cache_max_mb = cfg_mb;
	protected_path[0] = '\0';
	if (!sd_root || !sd_root[0]) {
		cache_dir[0] = '\0';
	} else {
		size_t len = strlen(sd_root);
		while (len > 1 && sd_root[len - 1] == '/')
			len--;
		if ((size_t)snprintf(cache_dir, sizeof(cache_dir), "%.*s/%s", (int)len, sd_root, JF_CACHE_SUBDIR) >=
			sizeof(cache_dir)) {
			cache_dir[0] = '\0';
		}
	}
	pthread_mutex_unlock(&cache_lock);
}

const char *jellyfincache_dir(void) {
	return cache_dir;
}

int jellyfincache_get_max_mb(void) {
	pthread_mutex_lock(&cache_lock);
	int mb = cache_max_mb;
	pthread_mutex_unlock(&cache_lock);
	return mb;
}

void jellyfincache_set_max_mb(int mb) {
	int norm = normalize_max_mb(mb);
	pthread_mutex_lock(&cache_lock);
	cache_max_mb = norm;
	custom_max_bytes = -1;
	pthread_mutex_unlock(&cache_lock);
	config_set_int("jellyfin", "cache_max_mb", norm);
	config_save();
}

long long jellyfincache_get_max_bytes(void) {
	pthread_mutex_lock(&cache_lock);
	long long bytes = (custom_max_bytes >= 0) ? custom_max_bytes : ((long long)cache_max_mb * 1024LL * 1024LL);
	pthread_mutex_unlock(&cache_lock);
	return bytes;
}

void jellyfincache_set_max_bytes(long long max_bytes) {
	pthread_mutex_lock(&cache_lock);
	custom_max_bytes = max_bytes;
	pthread_mutex_unlock(&cache_lock);
}

bool jellyfincache_owns(const char *path) {
	if (!path || !path[0])
		return false;
	char dir[JF_PATH_MAX];
	pthread_mutex_lock(&cache_lock);
	copy_text(dir, sizeof(dir), cache_dir);
	pthread_mutex_unlock(&cache_lock);
	if (!dir[0])
		return false;
	size_t dlen = strlen(dir);
	return strncmp(path, dir, dlen) == 0 && path[dlen] == '/';
}

bool jellyfincache_item_id_of(const char *path, char *out, size_t out_size) {
	if (!out || out_size == 0)
		return false;
	out[0] = '\0';
	if (!jellyfincache_owns(path))
		return false;

	const char *base = strrchr(path, '/');
	if (!base || !base[1])
		return false;
	base++;

	size_t len = 0;
	while (base[len] && base[len] != '.') {
		if (base[len] == '_' && base[len + 1] == '_')
			break;
		len++;
	}
	if (len == 0 || len >= out_size)
		return false;

	memcpy(out, base, len);
	out[len] = '\0';
	if (!valid_id(out)) {
		out[0] = '\0';
		return false;
	}
	return true;
}

bool jellyfincache_placeholder_path(const char *item_id, const char *container_hint, char *out, size_t out_size) {
	if (!out || out_size == 0)
		return false;
	out[0] = '\0';

	char dir[JF_PATH_MAX];
	pthread_mutex_lock(&cache_lock);
	copy_text(dir, sizeof(dir), cache_dir);
	pthread_mutex_unlock(&cache_lock);
	if (!dir[0] || !valid_id(item_id))
		return false;

	jellyfin_quality_t quality = jellyfin_get_quality();
	char ext[JELLYFIN_CONTAINER_MAX] = "";
	if (quality == JELLYFIN_QUALITY_MP3_320 || quality == JELLYFIN_QUALITY_MP3_192) {
		copy_text(ext, sizeof(ext), "mp3");
	} else if (quality == JELLYFIN_QUALITY_OPUS_128) {
		copy_text(ext, sizeof(ext), "opus");
	} else if (container_hint && container_hint[0] && jellyfin_container_playable(container_hint)) {
		copy_text(ext, sizeof(ext), container_hint);
	} else {
		copy_text(ext, sizeof(ext), "mp3");
	}

	return build_audio_path(dir, item_id, quality, ext, out, out_size);
}

bool jellyfincache_find_playable(const char *item_id, char *out, size_t out_size) {
	if (!out || out_size == 0)
		return false;
	out[0] = '\0';
	if (!valid_id(item_id))
		return false;

	char dir[JF_PATH_MAX];
	pthread_mutex_lock(&cache_lock);
	copy_text(dir, sizeof(dir), cache_dir);
	pthread_mutex_unlock(&cache_lock);
	if (!dir[0])
		return false;

	jellyfin_quality_t quality = jellyfin_get_quality();
	char prefix[JELLYFIN_ID_MAX + 16];
	snprintf(prefix, sizeof(prefix), "%s%s.", item_id, quality_tag(quality));
	size_t plen = strlen(prefix);

	DIR *d = opendir(dir);
	if (!d)
		return false;
	struct dirent *de;
	bool found = false;
	while ((de = readdir(d)) != NULL) {
		if (strncmp(de->d_name, prefix, plen) != 0)
			continue;
		const char *ext = de->d_name + plen;
		if (!is_audio_ext(ext))
			continue;
		char cand[JF_PATH_MAX];
		if ((size_t)snprintf(cand, sizeof(cand), "%s/%s", dir, de->d_name) >= sizeof(cand))
			continue;
		if (file_is_complete(cand)) {
			copy_text(out, out_size, cand);
			found = true;
			break;
		}
	}
	closedir(d);
	return found;
}

// ---------------------------------------------------------------------------
// Sidecar writers (.tags + cover .jpg)
// ---------------------------------------------------------------------------

static void write_sanitized_tag_line(FILE *f, const char *key, const char *val) {
	if (!val || !val[0])
		return;
	fprintf(f, "%s=", key);
	for (const unsigned char *p = (const unsigned char *)val; *p; p++) {
		if (*p == '\r' || *p == '\n')
			fputc(' ', f);
		else
			fputc((int)*p, f);
	}
	fputc('\n', f);
}

static void write_tags_sidecar(const char *audio_path, const jellyfin_entry_t *track) {
	if (!audio_path || !track)
		return;
	char tags_path[JF_PATH_MAX];
	char tmp_path[JF_PATH_MAX];
	if ((size_t)snprintf(tags_path, sizeof(tags_path), "%s.tags", audio_path) >= sizeof(tags_path))
		return;
	if ((size_t)snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", tags_path) >= sizeof(tmp_path))
		return;

	FILE *f = fopen(tmp_path, "wb");
	if (!f)
		return;
	write_sanitized_tag_line(f, "title", track->title);
	write_sanitized_tag_line(f, "artist", track->artist);
	write_sanitized_tag_line(f, "album", track->album);
	write_sanitized_tag_line(f, "album_id", track->album_id);
	if (track->track_number > 0)
		fprintf(f, "track=%d\n", track->track_number);
	if (track->duration > 0)
		fprintf(f, "duration=%d\n", track->duration);
	fclose(f);
	rename(tmp_path, tags_path);
}

static void fetch_cover_sidecar(const char *audio_path, const char *cover_id) {
	if (!audio_path || !cover_id || !cover_id[0])
		return;

	char jpg_path[JF_PATH_MAX];
	copy_text(jpg_path, sizeof(jpg_path), audio_path);
	char *dot = strrchr(jpg_path, '.');
	char *slash = strrchr(jpg_path, '/');
	if (!dot || (slash && dot <= slash))
		return;
	*dot = '\0';
	char final_jpg[JF_PATH_MAX];
	char tmp_jpg[JF_PATH_MAX];
	if ((size_t)snprintf(final_jpg, sizeof(final_jpg), "%s.jpg", jpg_path) >= sizeof(final_jpg))
		return;
	if ((size_t)snprintf(tmp_jpg, sizeof(tmp_jpg), "%s.jpg.tmp", jpg_path) >= sizeof(tmp_jpg))
		return;

	struct stat st;
	if (stat(final_jpg, &st) == 0 && st.st_size > 0)
		return;

	char url[JELLYFIN_URL_MAX];
	if (!jellyfin_cover_url(cover_id, url, sizeof(url)))
		return;

	char auth_hdr[512];
	jellyfin_headers_for_url(url, auth_hdr, sizeof(auth_hdr));

	http_req_t req;
	memset(&req, 0, sizeof(req));
	req.method = "GET";
	req.extra_headers = auth_hdr[0] ? auth_hdr : NULL;
	req.no_https_downgrade = true;

	char *body = NULL;
	size_t body_len = 0;
	int status = 0;
	if (!http_request(url, &req, &body, &body_len, JF_COVER_MAX_BYTES, 8, &status) || status < 200 || status >= 300 ||
		!body || body_len < 16) {
		free(body);
		return;
	}

	FILE *f = fopen(tmp_jpg, "wb");
	if (f) {
		bool ok = (fwrite(body, 1, body_len, f) == body_len);
		fclose(f);
		if (ok)
			rename(tmp_jpg, final_jpg);
		else
			unlink(tmp_jpg);
	}
	free(body);
}

// ---------------------------------------------------------------------------
// LRU eviction & stats
// ---------------------------------------------------------------------------

typedef struct {
	char path[JF_PATH_MAX];
	long long bytes;
	time_t mtime;
} cache_item_t;

static int cmp_cache_item_oldest(const void *a, const void *b) {
	const cache_item_t *ca = (const cache_item_t *)a;
	const cache_item_t *cb = (const cache_item_t *)b;
	if (ca->mtime < cb->mtime)
		return -1;
	if (ca->mtime > cb->mtime)
		return 1;
	return strcmp(ca->path, cb->path);
}

static long long free_bytes_for_dir(const char *dir) {
	struct statvfs vfs;
	if (statvfs(dir, &vfs) != 0)
		return JELLYFINCACHE_MIN_FREE;
	return (long long)vfs.f_bavail * (long long)vfs.f_frsize;
}

static bool evict_if_needed(const char *dir, const char *keep_path, long long reserve_bytes) {
	if (!dir || !dir[0])
		return false;

	char prot[JF_PATH_MAX];
	long long cap_bytes = jellyfincache_get_max_bytes();
	pthread_mutex_lock(&cache_lock);
	copy_text(prot, sizeof(prot), protected_path);
	pthread_mutex_unlock(&cache_lock);

	DIR *d = opendir(dir);
	if (!d)
		return true;

	cache_item_t *items = NULL;
	int count = 0;
	int cap = 0;
	long long total_bytes = 0;

	struct dirent *de;
	while ((de = readdir(d)) != NULL) {
		if (de->d_name[0] == '.')
			continue;
		const char *dot = strrchr(de->d_name, '.');
		if (!dot || !is_audio_ext(dot + 1))
			continue;
		char full[JF_PATH_MAX];
		if ((size_t)snprintf(full, sizeof(full), "%s/%s", dir, de->d_name) >= sizeof(full))
			continue;
		struct stat st;
		if (stat(full, &st) != 0 || !S_ISREG(st.st_mode))
			continue;
		total_bytes += (long long)st.st_size;

		if (count >= cap) {
			int next_cap = cap ? cap * 2 : 32;
			cache_item_t *grown = realloc(items, (size_t)next_cap * sizeof(cache_item_t));
			if (!grown)
				break;
			items = grown;
			cap = next_cap;
		}
		copy_text(items[count].path, sizeof(items[count].path), full);
		items[count].bytes = (long long)st.st_size;
		items[count].mtime = st.st_mtime;
		count++;
	}
	closedir(d);

	if (count > 1)
		qsort(items, (size_t)count, sizeof(cache_item_t), cmp_cache_item_oldest);

	long long avail = free_bytes_for_dir(dir);
	for (int i = 0; i < count; i++) {
		bool over_cap = (cap_bytes > 0) && (total_bytes + reserve_bytes > cap_bytes);
		bool low_free = (avail - reserve_bytes < JELLYFINCACHE_MIN_FREE);
		if (!over_cap && !low_free)
			break;
		if (keep_path && keep_path[0] && strcmp(items[i].path, keep_path) == 0)
			continue;
		if (prot[0] && strcmp(items[i].path, prot) == 0)
			continue;
		if (growfile_is_growing(items[i].path))
			continue;

		remove_track_and_sidecars(items[i].path);
		total_bytes -= items[i].bytes;
		avail += items[i].bytes;
	}

	free(items);
	return free_bytes_for_dir(dir) >= (JELLYFINCACHE_MIN_FREE / 4);
}

void jellyfincache_protect(const char *path) {
	pthread_mutex_lock(&cache_lock);
	if (path && path[0])
		copy_text(protected_path, sizeof(protected_path), path);
	else
		protected_path[0] = '\0';
	pthread_mutex_unlock(&cache_lock);
}

void jellyfincache_stats(int *track_count, uint64_t *bytes) {
	int tracks = 0;
	uint64_t total = 0;

	char dir[JF_PATH_MAX];
	pthread_mutex_lock(&cache_lock);
	copy_text(dir, sizeof(dir), cache_dir);
	pthread_mutex_unlock(&cache_lock);

	if (dir[0]) {
		DIR *d = opendir(dir);
		if (d) {
			struct dirent *de;
			while ((de = readdir(d)) != NULL) {
				if (de->d_name[0] == '.')
					continue;
				char full[JF_PATH_MAX];
				if ((size_t)snprintf(full, sizeof(full), "%s/%s", dir, de->d_name) >= sizeof(full))
					continue;
				struct stat st;
				if (stat(full, &st) != 0 || !S_ISREG(st.st_mode))
					continue;
				total += (uint64_t)st.st_size;
				const char *dot = strrchr(de->d_name, '.');
				if (dot && is_audio_ext(dot + 1))
					tracks++;
			}
			closedir(d);
		}
	}

	if (track_count)
		*track_count = tracks;
	if (bytes)
		*bytes = total;
}

bool jellyfincache_clear(void) {
	jellyfincache_cancel();

	char dir[JF_PATH_MAX];
	char prot[JF_PATH_MAX];
	pthread_mutex_lock(&cache_lock);
	copy_text(dir, sizeof(dir), cache_dir);
	copy_text(prot, sizeof(prot), protected_path);
	pthread_mutex_unlock(&cache_lock);

	if (!dir[0])
		return true;

	DIR *d = opendir(dir);
	if (!d)
		return true;

	struct dirent *de;
	while ((de = readdir(d)) != NULL) {
		if (de->d_name[0] == '.')
			continue;
		char full[JF_PATH_MAX];
		if ((size_t)snprintf(full, sizeof(full), "%s/%s", dir, de->d_name) >= sizeof(full))
			continue;
		if (prot[0] && strncmp(full, prot, strlen(prot)) == 0)
			continue;
		if (growfile_prefix_is_growing(full))
			continue;
		unlink(full);
	}
	closedir(d);
	return true;
}

void jellyfincache_clear_on_exit(void) {
	jellyfincache_protect(NULL);
	jellyfincache_clear();
}

// ---------------------------------------------------------------------------
// Cancellation & socket tracking
// ---------------------------------------------------------------------------

static bool is_cancelled(void) {
	pthread_mutex_lock(&cache_lock);
	bool c = cancel_flag;
	pthread_mutex_unlock(&cache_lock);
	return c;
}

static bool socket_observer_cb(void *user, int fd) {
	(void)user;
	pthread_mutex_lock(&cache_lock);
	bool cancelled = cancel_flag;
	active_sock_fd = cancelled ? -1 : fd;
	pthread_mutex_unlock(&cache_lock);
	return !cancelled;
}

void jellyfincache_cancel(void) {
	pthread_mutex_lock(&cache_lock);
	cancel_flag = true;
	int fd = active_sock_fd;
	http_stream_t *st = active_stream;
	pthread_mutex_unlock(&cache_lock);

	if (fd >= 0)
		shutdown(fd, SHUT_RDWR);
	if (st)
		http_stream_wake(st);

	// Wait for any active background completion thread to finish cleaning up.
	pthread_mutex_lock(&cache_lock);
	while (bg_running)
		pthread_cond_wait(&cache_cond, &cache_lock);
	pthread_mutex_unlock(&cache_lock);
}

bool jellyfincache_downloading(void) {
	pthread_mutex_lock(&cache_lock);
	bool busy = active_download || bg_running;
	pthread_mutex_unlock(&cache_lock);
	return busy;
}

// ---------------------------------------------------------------------------
// Background completion worker (for progressive growfile tracks)
// ---------------------------------------------------------------------------

static void *bg_download_worker(void *arg) {
	bg_job_t *job = (bg_job_t *)arg;
	thread_be_background("jf-cache");

	unsigned char buf[JF_CHUNK_SIZE];
	bool ok = true;

	pthread_mutex_lock(&cache_lock);
	active_stream = &job->st;
	pthread_mutex_unlock(&cache_lock);

	for (;;) {
		if (is_cancelled()) {
			ok = false;
			break;
		}
		int n = http_stream_read(&job->st, buf, (int)sizeof(buf));
		if (n == 0)
			break;
		if (n < 0) {
			ok = false;
			break;
		}
		if (fwrite(buf, 1, (size_t)n, job->fp) != (size_t)n) {
			ok = false;
			break;
		}
		fflush(job->fp);
		job->written += (long)n;
		growfile_progress(job->path, job->written);
	}

	if (ok && job->st.chunked && job->st.chunk_failed)
		ok = false;
	if (ok && job->total > 0 && job->written < job->total)
		ok = false;

	pthread_mutex_lock(&cache_lock);
	active_stream = NULL;
	active_sock_fd = -1;
	pthread_mutex_unlock(&cache_lock);

	fclose(job->fp);
	http_stream_close(&job->st);

	if (ok) {
		unlink(job->incomplete_path);
		growfile_finish(job->path, true);
		utime(job->path, NULL);
		if (!is_cancelled() && job->cover_id[0])
			fetch_cover_sidecar(job->path, job->cover_id);
	} else {
		growfile_finish(job->path, false);
		remove_track_and_sidecars(job->path);
	}

	char dir[JF_PATH_MAX];
	pthread_mutex_lock(&cache_lock);
	copy_text(dir, sizeof(dir), cache_dir);
	bg_running = false;
	pthread_cond_broadcast(&cache_cond);
	pthread_mutex_unlock(&cache_lock);

	if (ok && dir[0])
		evict_if_needed(dir, job->path, 0);

	streamturn_release();
	free(job);
	return NULL;
}

// ---------------------------------------------------------------------------
// Foreground get & lookahead prefetch
// ---------------------------------------------------------------------------

static bool download_track_locked_turn(const jellyfin_entry_t *track, bool wait_complete, char *out_path,
									   size_t out_size, char *error, size_t error_size, bool *released_turn_out) {
	*released_turn_out = false;

	char dir[JF_PATH_MAX];
	pthread_mutex_lock(&cache_lock);
	copy_text(dir, sizeof(dir), cache_dir);
	cancel_flag = false;
	active_download = true;
	pthread_mutex_unlock(&cache_lock);

	if (!dir[0]) {
		set_error(error, error_size, "jellyfin_no_card");
		goto fail_no_turn_handoff;
	}
	if (!ensure_cache_dir(dir)) {
		set_error(error, error_size, "jellyfin_cache_write_failed");
		goto fail_no_turn_handoff;
	}

	// Fast path: already completely cached at the active quality!
	char existing[JF_PATH_MAX];
	if (jellyfincache_find_playable(track->id, existing, sizeof(existing))) {
		utime(existing, NULL);
		write_tags_sidecar(existing, track);
		if (track->cover_id[0])
			fetch_cover_sidecar(existing, track->cover_id);
		copy_text(out_path, out_size, existing);
		pthread_mutex_lock(&cache_lock);
		active_download = false;
		pthread_mutex_unlock(&cache_lock);
		return true;
	}

	if (!evict_if_needed(dir, NULL, 1024 * 1024)) {
		set_error(error, error_size, "jellyfin_card_full");
		goto fail_no_turn_handoff;
	}

	jellyfin_quality_t quality = jellyfin_get_quality();
	char url[JELLYFIN_URL_MAX];
	char ext_hint[JELLYFIN_CONTAINER_MAX] = "";
	bool will_transcode = false;
	if (!jellyfin_stream_url(track->id, quality, track->container, url, sizeof(url), ext_hint, sizeof(ext_hint),
							 &will_transcode, error, error_size)) {
		goto fail_no_turn_handoff;
	}

	char auth_hdr[512];
	jellyfin_headers_for_url(url, auth_hdr, sizeof(auth_hdr));

	http_stream_t st;
	memset(&st, 0, sizeof(st));
	int status = 0;
	http_stream_options_t opts;
	memset(&opts, 0, sizeof(opts));
	opts.socket_cb = socket_observer_cb;
	opts.no_icy = true;
	opts.no_https_downgrade = true;
	opts.extra_headers = auth_hdr[0] ? auth_hdr : NULL;
	opts.status_out = &status;

	if (!http_stream_open_ex(&st, url, JF_STREAM_TIMEOUT_SECS, &opts)) {
		pthread_mutex_lock(&cache_lock);
		active_sock_fd = -1;
		pthread_mutex_unlock(&cache_lock);
		if (status == 401)
			set_error(error, error_size, "jellyfin_auth_failed");
		else if (status == 403)
			set_error(error, error_size, "jellyfin_access_denied");
		else if (status == 404)
			set_error(error, error_size, "jellyfin_not_found");
		else
			set_error(error, error_size, "jellyfin_stream_failed");
		goto fail_no_turn_handoff;
	}

	char ext[JELLYFIN_CONTAINER_MAX];
	if (will_transcode && ext_hint[0]) {
		copy_text(ext, sizeof(ext), ext_hint);
	} else {
		ext_from_content_type(st.content_type, ext_hint[0] ? ext_hint : track->container, ext, sizeof(ext));
	}

	char final_path[JF_PATH_MAX];
	char inc_path[JF_PATH_MAX];
	if (!build_audio_path(dir, track->id, quality, ext, final_path, sizeof(final_path)) ||
		(size_t)snprintf(inc_path, sizeof(inc_path), "%s%s", final_path, JF_INCOMPLETE_SUFFIX) >= sizeof(inc_path)) {
		http_stream_close(&st);
		set_error(error, error_size, "jellyfin_cache_write_failed");
		goto fail_no_turn_handoff;
	}

	// Reserve space if server stated exact Content-Length.
	long long expected_bytes = (st.content_length64 > 0) ? (long long)st.content_length64 : 0;
	if (expected_bytes > 0)
		evict_if_needed(dir, final_path, expected_bytes);

	// Mark `.incompleto` so an interrupted download is never mistaken for full.
	FILE *incf = fopen(inc_path, "wb");
	if (incf)
		fclose(incf);

	FILE *fp = fopen(final_path, "wb");
	if (!fp) {
		unlink(inc_path);
		http_stream_close(&st);
		set_error(error, error_size, "jellyfin_cache_write_failed");
		goto fail_no_turn_handoff;
	}

	write_tags_sidecar(final_path, track);

	bool can_grow = !wait_complete && ext_supports_growfile(ext);
	long prebuffer_target = (strcasecmp(ext, "flac") == 0) ? JF_PREBUFFER_FLAC : JF_PREBUFFER_MIN;
	long total = (st.content_length > 0) ? st.content_length : 0;

	pthread_mutex_lock(&cache_lock);
	active_stream = &st;
	pthread_mutex_unlock(&cache_lock);

	unsigned char buf[JF_CHUNK_SIZE];
	long written = 0;
	bool eof = false;
	bool failed = false;

	while (!eof) {
		if (is_cancelled()) {
			failed = true;
			break;
		}
		int n = http_stream_read(&st, buf, (int)sizeof(buf));
		if (n == 0) {
			eof = true;
			break;
		}
		if (n < 0) {
			failed = true;
			break;
		}
		if (fwrite(buf, 1, (size_t)n, fp) != (size_t)n) {
			failed = true;
			set_error(error, error_size, "jellyfin_cache_write_failed");
			break;
		}
		written += (long)n;
		if (can_grow && written >= prebuffer_target) {
			fflush(fp);
			break;
		}
	}

	if (failed || written <= 0 || (eof && st.chunked && st.chunk_failed) || (eof && total > 0 && written < total)) {
		pthread_mutex_lock(&cache_lock);
		active_stream = NULL;
		active_sock_fd = -1;
		active_download = false;
		pthread_mutex_unlock(&cache_lock);
		fclose(fp);
		http_stream_close(&st);
		remove_track_and_sidecars(final_path);
		if (error && !error[0])
			set_error(error, error_size, "jellyfin_stream_failed");
		return false;
	}

	if (eof || !can_grow) {
		// Entire file arrived synchronously!
		pthread_mutex_lock(&cache_lock);
		active_stream = NULL;
		active_sock_fd = -1;
		active_download = false;
		pthread_mutex_unlock(&cache_lock);

		fclose(fp);
		http_stream_close(&st);
		unlink(inc_path);
		utime(final_path, NULL);
		if (!is_cancelled() && track->cover_id[0])
			fetch_cover_sidecar(final_path, track->cover_id);
		evict_if_needed(dir, final_path, 0);
		copy_text(out_path, out_size, final_path);
		return true;
	}

	// Hand off remaining progressive growfile bytes to background thread.
	bg_job_t *job = calloc(1, sizeof(*job));
	if (!job) {
		pthread_mutex_lock(&cache_lock);
		active_stream = NULL;
		active_sock_fd = -1;
		active_download = false;
		pthread_mutex_unlock(&cache_lock);
		fclose(fp);
		http_stream_close(&st);
		remove_track_and_sidecars(final_path);
		set_error(error, error_size, "jellyfin_out_of_memory");
		return false;
	}

	job->st = st;
	job->fp = fp;
	copy_text(job->path, sizeof(job->path), final_path);
	copy_text(job->incomplete_path, sizeof(job->incomplete_path), inc_path);
	copy_text(job->cover_id, sizeof(job->cover_id), track->cover_id);
	job->written = written;
	job->total = total;

	growfile_announce(job->path, total);
	growfile_progress(job->path, written);

	pthread_mutex_lock(&cache_lock);
	bg_running = true;
	active_stream = &job->st;
	active_download = false;
	pthread_mutex_unlock(&cache_lock);

	pthread_attr_t attr;
	pthread_t tid;
	pthread_attr_init(&attr);
	pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
	pthread_attr_setstacksize(&attr, JF_WORKER_STACK);
	if (pthread_create(&tid, &attr, bg_download_worker, job) != 0) {
		pthread_attr_destroy(&attr);
		pthread_mutex_lock(&cache_lock);
		bg_running = false;
		active_stream = NULL;
		active_sock_fd = -1;
		pthread_cond_broadcast(&cache_cond);
		pthread_mutex_unlock(&cache_lock);
		growfile_finish(job->path, false);
		fclose(job->fp);
		http_stream_close(&job->st);
		remove_track_and_sidecars(job->path);
		free(job);
		set_error(error, error_size, "jellyfin_stream_failed");
		return false;
	}
	pthread_attr_destroy(&attr);

	// Background thread now owns `streamturn_release()`.
	*released_turn_out = true;
	copy_text(out_path, out_size, final_path);
	return true;

fail_no_turn_handoff:
	pthread_mutex_lock(&cache_lock);
	active_download = false;
	active_stream = NULL;
	active_sock_fd = -1;
	pthread_mutex_unlock(&cache_lock);
	return false;
}

bool jellyfincache_get(const jellyfin_entry_t *track, bool wait_complete, char *out_path, size_t out_size, char *error,
					   size_t error_size) {
	pthread_once(&abandon_once, register_abandon_once);
	if (error && error_size > 0)
		error[0] = '\0';
	if (out_path && out_size > 0)
		out_path[0] = '\0';
	if (!track || !valid_id(track->id) || !out_path || out_size == 0) {
		set_error(error, error_size, "jellyfin_invalid_id");
		return false;
	}

	// Fast path before grabbing the global download turn: if already complete
	// on disk, serve immediately without disturbing any other transfer!
	char existing[JF_PATH_MAX];
	if (jellyfincache_find_playable(track->id, existing, sizeof(existing))) {
		utime(existing, NULL);
		write_tags_sidecar(existing, track);
		copy_text(out_path, out_size, existing);
		return true;
	}

	streamturn_abandon_all();
	streamturn_acquire();

	bool handed_off = false;
	bool ok = download_track_locked_turn(track, wait_complete, out_path, out_size, error, error_size, &handed_off);
	if (!handed_off)
		streamturn_release();
	return ok;
}

void jellyfincache_prefetch(const jellyfin_entry_t *track) {
	pthread_once(&abandon_once, register_abandon_once);
	if (!track || !valid_id(track->id))
		return;

	char existing[JF_PATH_MAX];
	if (jellyfincache_find_playable(track->id, existing, sizeof(existing)))
		return;

	streamturn_wait_idle();
	streamturn_acquire();

	char out[JF_PATH_MAX];
	bool handed_off = false;
	// Complete prefetch silently so the next track starts with zero stall.
	download_track_locked_turn(track, true, out, sizeof(out), NULL, 0, &handed_off);
	if (!handed_off)
		streamturn_release();
}
