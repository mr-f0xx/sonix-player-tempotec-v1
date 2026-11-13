#include "webdavcache.h"
#include "streamturn.h"

#include "src/system/audio/audio.h"
#include "src/system/core/config.h"
#include "src/system/decode/growfile.h"
#include "src/system/device/system.h"
#include "src/system/device/usb.h"
#include "src/system/library/metadata.h"
#include "src/system/net/http.h"

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
#include <time.h>
#include <unistd.h>
#include <utime.h>

#define STREAM_TIMEOUT_SECS 15
#define CHUNK_SIZE 32768
#define FREE_FLOOR_BYTES (256ULL * 1024 * 1024)
#define MAX_TRACK_BYTES (2ULL * 1024 * 1024 * 1024)

static char active_cache_dir[512];
static bool root_explicitly_cleared;

static void stop_active_worker(void);

static pthread_mutex_t dl_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t dl_cv = PTHREAD_COND_INITIALIZER;
static pthread_t dl_thread;
static bool dl_thread_live;
static bool dl_running;
static bool dl_done;
static bool dl_ok;
static bool dl_cancel;
static uint64_t dl_have;
static int dl_connect_fd = -1;
static http_stream_t dl_stream;
static bool dl_stream_open;
static FILE *dl_file;
static char dl_audio_path[560];
static char dl_title[WEBDAV_TITLE_MAX];
static char dl_album[WEBDAV_TITLE_MAX];
static char dl_error[96];
static bool abandon_registered;

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

// Compact FIPS 180-4 SHA-256 for deterministic cache filenames without OpenSSL.
typedef struct {
	uint32_t state[8];
	uint64_t bitlen;
	uint8_t buf[64];
	size_t buflen;
} sha256_ctx_t;

static uint32_t rotr32(uint32_t x, unsigned n) {
	return (x >> n) | (x << (32 - n));
}

static void sha256_transform(sha256_ctx_t *ctx, const uint8_t data[64]) {
	static const uint32_t K[64] = {
		0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
		0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
		0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
		0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
		0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
		0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
		0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
		0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u,
	};
	uint32_t w[64];
	for (int i = 0; i < 16; i++) {
		w[i] = ((uint32_t)data[i * 4] << 24) | ((uint32_t)data[i * 4 + 1] << 16) |
			   ((uint32_t)data[i * 4 + 2] << 8) | (uint32_t)data[i * 4 + 3];
	}
	for (int i = 16; i < 64; i++) {
		uint32_t s0 = rotr32(w[i - 15], 7) ^ rotr32(w[i - 15], 18) ^ (w[i - 15] >> 3);
		uint32_t s1 = rotr32(w[i - 2], 17) ^ rotr32(w[i - 2], 19) ^ (w[i - 2] >> 10);
		w[i] = w[i - 16] + s0 + w[i - 7] + s1;
	}
	uint32_t a = ctx->state[0], b = ctx->state[1], c = ctx->state[2], d = ctx->state[3];
	uint32_t e = ctx->state[4], f = ctx->state[5], g = ctx->state[6], h = ctx->state[7];
	for (int i = 0; i < 64; i++) {
		uint32_t S1 = rotr32(e, 6) ^ rotr32(e, 11) ^ rotr32(e, 25);
		uint32_t ch = (e & f) ^ ((~e) & g);
		uint32_t temp1 = h + S1 + ch + K[i] + w[i];
		uint32_t S0 = rotr32(a, 2) ^ rotr32(a, 13) ^ rotr32(a, 22);
		uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
		uint32_t temp2 = S0 + maj;
		h = g; g = f; f = e; e = d + temp1;
		d = c; c = b; b = a; a = temp1 + temp2;
	}
	ctx->state[0] += a; ctx->state[1] += b; ctx->state[2] += c; ctx->state[3] += d;
	ctx->state[4] += e; ctx->state[5] += f; ctx->state[6] += g; ctx->state[7] += h;
}

static void sha256_init(sha256_ctx_t *ctx) {
	ctx->state[0] = 0x6a09e667u; ctx->state[1] = 0xbb67ae85u;
	ctx->state[2] = 0x3c6ef372u; ctx->state[3] = 0xa54ff53au;
	ctx->state[4] = 0x510e527fu; ctx->state[5] = 0x9b05688cu;
	ctx->state[6] = 0x1f83d9abu; ctx->state[7] = 0x5be0cd19u;
	ctx->bitlen = 0;
	ctx->buflen = 0;
}

static void sha256_update(sha256_ctx_t *ctx, const void *data, size_t len) {
	const uint8_t *p = (const uint8_t *)data;
	for (size_t i = 0; i < len; i++) {
		ctx->buf[ctx->buflen++] = p[i];
		if (ctx->buflen == 64) {
			sha256_transform(ctx, ctx->buf);
			ctx->bitlen += 512;
			ctx->buflen = 0;
		}
	}
}

static void sha256_final(sha256_ctx_t *ctx, uint8_t hash[32]) {
	size_t i = ctx->buflen;
	ctx->bitlen += (uint64_t)ctx->buflen * 8;
	ctx->buf[i++] = 0x80;
	if (i > 56) {
		while (i < 64) ctx->buf[i++] = 0;
		sha256_transform(ctx, ctx->buf);
		i = 0;
	}
	while (i < 56) ctx->buf[i++] = 0;
	for (int b = 7; b >= 0; b--) {
		ctx->buf[i++] = (uint8_t)((ctx->bitlen >> (b * 8)) & 0xFF);
	}
	sha256_transform(ctx, ctx->buf);
	for (int j = 0; j < 8; j++) {
		hash[j * 4] = (uint8_t)(ctx->state[j] >> 24);
		hash[j * 4 + 1] = (uint8_t)(ctx->state[j] >> 16);
		hash[j * 4 + 2] = (uint8_t)(ctx->state[j] >> 8);
		hash[j * 4 + 3] = (uint8_t)ctx->state[j];
	}
}

void webdavcache_set_root(const char *sd_root) {
	if (!sd_root || !sd_root[0]) {
		stop_active_worker();
		pthread_mutex_lock(&dl_lock);
		active_cache_dir[0] = '\0';
		root_explicitly_cleared = true;
		pthread_mutex_unlock(&dl_lock);
		return;
	}
	pthread_mutex_lock(&dl_lock);
	root_explicitly_cleared = false;
	if (snprintf(active_cache_dir, sizeof(active_cache_dir), "%s/.local/webdav-cache", sd_root) >=
		(int)sizeof(active_cache_dir)) {
		active_cache_dir[0] = '\0';
	}
	pthread_mutex_unlock(&dl_lock);
}

static bool cache_dir(char *out, size_t size) {
	if (!out || size == 0) {
		return false;
	}
	pthread_mutex_lock(&dl_lock);
	if (active_cache_dir[0]) {
		int n = snprintf(out, size, "%s", active_cache_dir);
		pthread_mutex_unlock(&dl_lock);
		return n > 0 && (size_t)n < size;
	}
	bool cleared = root_explicitly_cleared;
	pthread_mutex_unlock(&dl_lock);
	if (cleared) {
		out[0] = '\0';
		return false;
	}
	const char *sd = storage_sd_root();
	if (!sd || !sd[0]) {
		out[0] = '\0';
		return false;
	}
	int n = snprintf(out, size, "%s/.local/webdav-cache", sd);
	return n > 0 && (size_t)n < size;
}

const char *webdavcache_dir(void) {
	static char buf[512];
	if (!cache_dir(buf, sizeof(buf))) {
		buf[0] = '\0';
	}
	return buf;
}

static bool ensure_cache_dir(char *dir_out, size_t size) {
	if (!cache_dir(dir_out, size)) {
		return false;
	}
	char parent[512];
	snprintf(parent, sizeof(parent), "%s", dir_out);
	char *slash = strrchr(parent, '/');
	if (slash && slash != parent) {
		*slash = '\0';
		mkdir(parent, 0755);
	}
	if (mkdir(dir_out, 0755) != 0 && errno != EEXIST) {
		return false;
	}
	return true;
}

bool webdavcache_owns(const char *path) {
	char dir[512];
	if (!path || !cache_dir(dir, sizeof(dir))) {
		return false;
	}
	size_t n = strlen(dir);
	return n > 0 && strncmp(path, dir, n) == 0 && (path[n] == '/' || path[n] == '\0');
}

static bool track_hash_and_ext(const webdav_entry_t *track, char hex_out[33], char ext_out[16]) {
	if (!track || track->kind != WEBDAV_TRACK || !track->path[0]) {
		return false;
	}
	const char *raw_ext = track->suffix[0] ? track->suffix : NULL;
	if (!raw_ext) {
		const char *dot = strrchr(track->path, '.');
		if (dot && dot[1]) {
			raw_ext = dot + 1;
		}
	}
	if (!raw_ext || !webdav_is_audio_suffix(raw_ext)) {
		return false;
	}
	size_t k = 0;
	for (; raw_ext[k] && k + 1 < 16; k++) {
		ext_out[k] = (char)tolower((unsigned char)raw_ext[k]);
	}
	ext_out[k] = '\0';

	char skey[WEBDAV_SERVER_MAX + WEBDAV_USER_MAX + 8];
	if (!webdav_server_key(skey, sizeof(skey))) {
		snprintf(skey, sizeof(skey), "%s", "unconfigured");
	}
	sha256_ctx_t ctx;
	uint8_t digest[32];
	sha256_init(&ctx);
	sha256_update(&ctx, skey, strlen(skey));
	sha256_update(&ctx, "\n", 1);
	sha256_update(&ctx, track->path, strlen(track->path));
	sha256_final(&ctx, digest);

	static const char HEX[] = "0123456789abcdef";
	for (int i = 0; i < 16; i++) {
		hex_out[i * 2] = HEX[(digest[i] >> 4) & 0x0F];
		hex_out[i * 2 + 1] = HEX[digest[i] & 0x0F];
	}
	hex_out[32] = '\0';
	return true;
}

static bool build_audio_path(const webdav_entry_t *track, char *out, size_t size) {
	char dir[512], hex[33], ext[16];
	if (!cache_dir(dir, sizeof(dir)) || !track_hash_and_ext(track, hex, ext)) {
		return false;
	}
	int n = snprintf(out, size, "%s/%s.%s", dir, hex, ext);
	return n > 0 && (size_t)n < size;
}

bool webdavcache_placeholder(const webdav_entry_t *track, char *out, size_t size) {
	char dir[512], hex[33], ext[16];
	if (!out || size == 0 || !cache_dir(dir, sizeof(dir)) || !track_hash_and_ext(track, hex, ext)) {
		return false;
	}
	int n = snprintf(out, size, "%s/.pending-%s.%s", dir, hex, ext);
	return n > 0 && (size_t)n < size;
}

bool webdavcache_is_placeholder(const char *path) {
	if (!webdavcache_owns(path)) {
		return false;
	}
	const char *base = strrchr(path, '/');
	return base && strncmp(base + 1, ".pending-", 9) == 0;
}

static void marker_path(const char *audio_path, char *out, size_t size) {
	snprintf(out, size, "%.530s.incompleto", audio_path);
}

static void tags_path(const char *audio_path, char *out, size_t size) {
	snprintf(out, size, "%.530s.tags", audio_path);
}

bool webdavcache_cached(const webdav_entry_t *track, char *out, size_t size) {
	char path[560];
	if (!build_audio_path(track, path, sizeof(path))) {
		return false;
	}
	char inc[580];
	marker_path(path, inc, sizeof(inc));
	struct stat st;
	if (stat(inc, &st) == 0) {
		return false;
	}
	if (stat(path, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size <= 0) {
		return false;
	}
	if (out && size > 0) {
		snprintf(out, size, "%s", path);
	}
	return true;
}

int webdavcache_max_mb(void) {
	int mb = config_get_int("webdav", "cache_max_mb", WEBDAVCACHE_DEFAULT_MAX_MB);
	return mb < 0 ? WEBDAVCACHE_DEFAULT_MAX_MB : mb;
}

static bool is_cached_audio_name(const char *name) {
	if (!name || name[0] == '.') {
		return false;
	}
	const char *dot = strrchr(name, '.');
	if (!dot || !dot[1]) {
		return false;
	}
	if (strcasecmp(dot + 1, "tags") == 0 || strcasecmp(dot + 1, "incompleto") == 0) {
		return false;
	}
	return webdav_is_audio_suffix(dot + 1);
}

typedef struct {
	char path[560];
	uint64_t bytes;
	time_t mtime;
} cache_item_t;

static int cmp_mtime_asc(const void *a, const void *b) {
	const cache_item_t *ca = (const cache_item_t *)a;
	const cache_item_t *cb = (const cache_item_t *)b;
	if (ca->mtime < cb->mtime) return -1;
	if (ca->mtime > cb->mtime) return 1;
	return strcmp(ca->path, cb->path);
}

static void unlink_entry_and_sidecars(const char *audio_path) {
	char inc[580], tpath[580];
	marker_path(audio_path, inc, sizeof(inc));
	tags_path(audio_path, tpath, sizeof(tpath));
	unlink(audio_path);
	unlink(inc);
	unlink(tpath);
}

static void trim_cache_locked_hint(const char *protect_path) {
	int max_mb = webdavcache_max_mb();
	if (max_mb <= 0) {
		return; // unlimited (bounded by FREE_FLOOR_BYTES on download start)
	}
	uint64_t cap_bytes = (uint64_t)max_mb * 1024ULL * 1024ULL;
	char dir[512];
	if (!cache_dir(dir, sizeof(dir))) {
		return;
	}
	DIR *d = opendir(dir);
	if (!d) {
		return;
	}

	char playing[560];
	audio_get_current_file(playing, sizeof(playing));

	size_t list_cap = 64, count = 0;
	uint64_t total_bytes = 0;
	cache_item_t *items = calloc(list_cap, sizeof(*items));
	if (!items) {
		closedir(d);
		return;
	}

	struct dirent *de;
	while ((de = readdir(d)) != NULL) {
		if (!is_cached_audio_name(de->d_name)) {
			continue;
		}
		char full[560];
		int n = snprintf(full, sizeof(full), "%s/%s", dir, de->d_name);
		if (n <= 0 || (size_t)n >= sizeof(full)) {
			continue;
		}
		struct stat st;
		if (stat(full, &st) != 0 || !S_ISREG(st.st_mode)) {
			continue;
		}
		uint64_t sz = st.st_size > 0 ? (uint64_t)st.st_size : 0;
		total_bytes += sz;
		if ((protect_path && strcmp(full, protect_path) == 0) ||
			(playing[0] && strcmp(full, playing) == 0)) {
			continue;
		}
		char inc[580];
		marker_path(full, inc, sizeof(inc));
		struct stat ist;
		if (stat(inc, &ist) == 0) {
			continue; // active download
		}
		if (count >= list_cap) {
			size_t next = list_cap * 2;
			cache_item_t *grown = realloc(items, next * sizeof(*grown));
			if (!grown) {
				break;
			}
			items = grown;
			list_cap = next;
		}
		snprintf(items[count].path, sizeof(items[count].path), "%s", full);
		items[count].bytes = sz;
		items[count].mtime = st.st_mtime;
		count++;
	}
	closedir(d);

	if (total_bytes > cap_bytes && count > 0) {
		qsort(items, count, sizeof(*items), cmp_mtime_asc);
		for (size_t i = 0; i < count && total_bytes > cap_bytes; i++) {
			unlink_entry_and_sidecars(items[i].path);
			total_bytes = total_bytes > items[i].bytes ? total_bytes - items[i].bytes : 0;
		}
	}
	free(items);
}

void webdavcache_set_max_mb(int mb) {
	if (mb < 0) {
		mb = 0;
	}
	config_set_int("webdav", "cache_max_mb", mb);
	config_save();

	char active[560] = "";
	pthread_mutex_lock(&dl_lock);
	if (dl_running) {
		snprintf(active, sizeof(active), "%s", dl_audio_path);
	}
	pthread_mutex_unlock(&dl_lock);
	trim_cache_locked_hint(active[0] ? active : NULL);
}

void webdavcache_stats(int *files_out, uint64_t *bytes_out) {
	if (files_out) *files_out = 0;
	if (bytes_out) *bytes_out = 0;
	char dir[512];
	if (!cache_dir(dir, sizeof(dir))) {
		return;
	}
	DIR *d = opendir(dir);
	if (!d) {
		return;
	}
	int files = 0;
	uint64_t bytes = 0;
	struct dirent *de;
	while ((de = readdir(d)) != NULL) {
		if (!is_cached_audio_name(de->d_name)) {
			continue;
		}
		char full[560];
		int n = snprintf(full, sizeof(full), "%s/%s", dir, de->d_name);
		if (n <= 0 || (size_t)n >= sizeof(full)) {
			continue;
		}
		struct stat st;
		if (stat(full, &st) == 0 && S_ISREG(st.st_mode)) {
			files++;
			if (st.st_size > 0) {
				bytes += (uint64_t)st.st_size;
			}
		}
	}
	closedir(d);
	if (files_out) *files_out = files;
	if (bytes_out) *bytes_out = bytes;
}

void webdavcache_prune_incomplete(void) {
	char dir[512];
	if (!cache_dir(dir, sizeof(dir))) {
		return;
	}
	DIR *d = opendir(dir);
	if (!d) {
		return;
	}
	char active[560] = "";
	pthread_mutex_lock(&dl_lock);
	if (dl_running) {
		snprintf(active, sizeof(active), "%s", dl_audio_path);
	}
	pthread_mutex_unlock(&dl_lock);

	struct dirent *de;
	while ((de = readdir(d)) != NULL) {
		size_t len = strlen(de->d_name);
		if (len <= 11 || strcmp(de->d_name + len - 11, ".incompleto") != 0) {
			continue;
		}
		char audio[560];
		int n = snprintf(audio, sizeof(audio), "%s/%.*s", dir, (int)(len - 11), de->d_name);
		if (n <= 0 || (size_t)n >= sizeof(audio)) {
			continue;
		}
		if (active[0] && strcmp(audio, active) == 0) {
			continue;
		}
		unlink_entry_and_sidecars(audio);
	}
	closedir(d);
}

bool webdavcache_clear(int *removed_out) {
	if (removed_out) {
		*removed_out = 0;
	}
	if (usb_storage_active()) {
		return false;
	}
	webdavcache_cancel();
	char dir[512];
	if (!cache_dir(dir, sizeof(dir))) {
		return true;
	}
	DIR *d = opendir(dir);
	if (!d) {
		return true;
	}
	char playing[560];
	audio_get_current_file(playing, sizeof(playing));

	int removed = 0;
	struct dirent *de;
	while ((de = readdir(d)) != NULL) {
		if (!is_cached_audio_name(de->d_name)) {
			continue;
		}
		char full[560];
		int n = snprintf(full, sizeof(full), "%s/%s", dir, de->d_name);
		if (n <= 0 || (size_t)n >= sizeof(full)) {
			continue;
		}
		if (playing[0] && strcmp(full, playing) == 0) {
			continue;
		}
		unlink_entry_and_sidecars(full);
		removed++;
	}
	closedir(d);
	if (removed_out) {
		*removed_out = removed;
	}
	return true;
}

void webdavcache_clear_on_exit(void) {
	stop_active_worker();
	char dir[512];
	if (!cache_dir(dir, sizeof(dir))) {
		return;
	}
	DIR *d = opendir(dir);
	if (!d) {
		return;
	}
	struct dirent *de;
	while ((de = readdir(d)) != NULL) {
		if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) {
			continue;
		}
		char full[560];
		int n = snprintf(full, sizeof(full), "%s/%s", dir, de->d_name);
		if (n > 0 && (size_t)n < sizeof(full)) {
			unlink(full);
		}
	}
	closedir(d);
}

static void strip_filename_ext(const char *filename, char *out, size_t size) {
	snprintf(out, size, "%s", filename ? filename : "");
	char *dot = strrchr(out, '.');
	if (dot && dot != out && webdav_is_audio_suffix(dot + 1)) {
		*dot = '\0';
	}
}

static void sanitize_tag_line(const char *src, char *out, size_t size) {
	size_t j = 0;
	for (size_t i = 0; src && src[i] && j + 1 < size; i++) {
		unsigned char c = (unsigned char)src[i];
		if (c == '\r' || c == '\n') {
			out[j++] = ' ';
		} else if (c >= 32) {
			out[j++] = (char)c;
		}
	}
	out[j] = '\0';
}

// Writes a fallback `.tags` sidecar ONLY if the audio file has no embedded
// title tag, so user ID3/Vorbis/MP4 tags are preserved intact.
static bool maybe_write_fallback_tags(const char *audio_path, const char *title, const char *album) {
	char tpath[580];
	tags_path(audio_path, tpath, sizeof(tpath));
	unlink(tpath);

	song_metadata_t meta;
	memset(&meta, 0, sizeof(meta));
	metadata_read(audio_path, &meta);
	if (meta.title[0] != '\0') {
		return false; // file already carries real embedded tags
	}

	char clean_title[WEBDAV_TITLE_MAX], clean_album[WEBDAV_TITLE_MAX];
	strip_filename_ext(title, clean_title, sizeof(clean_title));
	sanitize_tag_line(clean_title, clean_title, sizeof(clean_title));
	sanitize_tag_line(album, clean_album, sizeof(clean_album));
	if (!clean_title[0]) {
		return false;
	}

	FILE *f = fopen(tpath, "w");
	if (!f) {
		return false;
	}
	fprintf(f, "title=%s\n", clean_title);
	if (clean_album[0]) {
		fprintf(f, "album=%s\n", clean_album);
	}
	fclose(f);
	return true;
}

static bool socket_observer_cb(void *user, int fd) {
	(void)user;
	pthread_mutex_lock(&dl_lock);
	bool cancelled = dl_cancel;
	dl_connect_fd = cancelled ? -1 : fd;
	pthread_mutex_unlock(&dl_lock);
	return !cancelled;
}

static void stop_active_worker(void) {
	pthread_t join_tid;
	bool need_join = false;

	pthread_mutex_lock(&dl_lock);
	if (dl_running) {
		dl_cancel = true;
		if (dl_connect_fd >= 0) {
			shutdown(dl_connect_fd, SHUT_RDWR);
		}
		if (dl_stream_open) {
			http_stream_wake(&dl_stream);
		}
		pthread_cond_broadcast(&dl_cv);
	}
	if (dl_thread_live) {
		join_tid = dl_thread;
		dl_thread_live = false;
		need_join = true;
	}
	pthread_mutex_unlock(&dl_lock);

	if (need_join) {
		pthread_join(join_tid, NULL);
	}
}

void webdavcache_cancel(void) {
	stop_active_worker();
}

bool webdavcache_downloading(void) {
	pthread_mutex_lock(&dl_lock);
	bool r = dl_running;
	pthread_mutex_unlock(&dl_lock);
	return r;
}

static bool verify_audio_magic(const char *audio_path, const uint8_t *head, size_t len) {
	if (!audio_path || !head || len < 4) {
		return false;
	}
	const char *dot = strrchr(audio_path, '.');
	const char *ext = dot ? dot + 1 : "";

	// Reject HTML/XML error portals disguised behind HTTP 200 OK.
	size_t s = 0;
	while (s < len && (head[s] == ' ' || head[s] == '\t' || head[s] == '\r' || head[s] == '\n')) {
		s++;
	}
	if (s + 4 <= len && head[s] == '<' &&
		(strncasecmp((const char *)(head + s), "<!do", 4) == 0 ||
		 strncasecmp((const char *)(head + s), "<htm", 4) == 0 ||
		 strncasecmp((const char *)(head + s), "<?xm", 4) == 0 ||
		 strncasecmp((const char *)(head + s), "<err", 4) == 0)) {
		return false;
	}

	if (strcasecmp(ext, "flac") == 0) {
		return memcmp(head, "fLaC", 4) == 0 || memcmp(head, "ID3", 3) == 0;
	}
	if (strcasecmp(ext, "ogg") == 0 || strcasecmp(ext, "oga") == 0 || strcasecmp(ext, "opus") == 0) {
		return memcmp(head, "OggS", 4) == 0;
	}
	if (strcasecmp(ext, "wav") == 0) {
		return memcmp(head, "RIFF", 4) == 0 || memcmp(head, "RF64", 4) == 0;
	}
	if (strcasecmp(ext, "aif") == 0 || strcasecmp(ext, "aiff") == 0) {
		return memcmp(head, "FORM", 4) == 0;
	}
	if (strcasecmp(ext, "m4a") == 0 || strcasecmp(ext, "alac") == 0) {
		return len >= 8 && (memcmp(head + 4, "ftyp", 4) == 0 || memcmp(head + 4, "moov", 4) == 0 ||
							memcmp(head + 4, "free", 4) == 0 || memcmp(head + 4, "wide", 4) == 0);
	}
	if (strcasecmp(ext, "ape") == 0) {
		return memcmp(head, "MAC ", 4) == 0 || memcmp(head, "ID3", 3) == 0;
	}
	if (strcasecmp(ext, "wv") == 0) {
		return memcmp(head, "wvpk", 4) == 0 || memcmp(head, "ID3", 3) == 0;
	}
	if (strcasecmp(ext, "dsf") == 0) {
		return memcmp(head, "DSD ", 4) == 0;
	}
	if (strcasecmp(ext, "dff") == 0) {
		return memcmp(head, "FRM8", 4) == 0;
	}
	if (strcasecmp(ext, "mp3") == 0 || strcasecmp(ext, "aac") == 0) {
		if (memcmp(head, "ID3", 3) == 0 || memcmp(head, "RIFF", 4) == 0) {
			return true;
		}
		for (size_t i = 0; i + 1 < len; i++) {
			if (head[i] == 0xFF && (head[i + 1] & 0xE0) == 0xE0) {
				return true;
			}
		}
		return false;
	}
	return true;
}

static void *download_worker(void *arg) {
	(void)arg;
	uint8_t *buf = malloc(CHUNK_SIZE);
	bool ok = (buf != NULL);
	char err[96] = "";
	if (!ok) {
		snprintf(err, sizeof(err), "%s", "webdav_out_of_memory");
	}

	uint64_t expected = dl_stream.content_length64;
	uint64_t written = 0;
	uint8_t head_probe[512];
	size_t head_len = 0;
	bool magic_checked = false;

	while (ok) {
		pthread_mutex_lock(&dl_lock);
		bool cancelled = dl_cancel;
		pthread_mutex_unlock(&dl_lock);
		if (cancelled) {
			ok = false;
			snprintf(err, sizeof(err), "%s", "cancelled");
			break;
		}

		int n = http_stream_read(&dl_stream, buf, CHUNK_SIZE);
		if (n == 0) {
			break;
		}
		if (n < 0) {
			ok = false;
			const char *he = http_last_error();
			snprintf(err, sizeof(err), "%s", (he && he[0]) ? he : "webdav_connection_failed");
			break;
		}
		if (!magic_checked) {
			size_t copy_n = (size_t)n < (sizeof(head_probe) - head_len)
								? (size_t)n
								: (sizeof(head_probe) - head_len);
			memcpy(head_probe + head_len, buf, copy_n);
			head_len += copy_n;
			if (head_len >= 12 || (expected > 0 && head_len >= expected)) {
				magic_checked = true;
				if (!verify_audio_magic(dl_audio_path, head_probe, head_len)) {
					ok = false;
					snprintf(err, sizeof(err), "%s", "webdav_codec_mismatch");
					break;
				}
			}
		}
		if (written + (uint64_t)n > MAX_TRACK_BYTES) {
			ok = false;
			snprintf(err, sizeof(err), "%s", "webdav_file_too_large");
			break;
		}
		if (fwrite(buf, 1, (size_t)n, dl_file) != (size_t)n || fflush(dl_file) != 0) {
			ok = false;
			snprintf(err, sizeof(err), "%s", "webdav_disk_full");
			break;
		}
		written += (uint64_t)n;
		growfile_progress(dl_audio_path, written > (uint64_t)LONG_MAX ? LONG_MAX : (long)written);

		pthread_mutex_lock(&dl_lock);
		dl_have = written;
		pthread_cond_broadcast(&dl_cv);
		pthread_mutex_unlock(&dl_lock);
	}

	free(buf);

	if (ok && !magic_checked && head_len > 0 && !verify_audio_magic(dl_audio_path, head_probe, head_len)) {
		ok = false;
		snprintf(err, sizeof(err), "%s", "webdav_codec_mismatch");
	}

	if (ok && written == 0) {
		ok = false;
		snprintf(err, sizeof(err), "%s", "webdav_stream_truncated");
	}
	if (ok && expected > 0 && written < expected) {
		ok = false;
		snprintf(err, sizeof(err), "%s", "webdav_stream_truncated");
	}

	if (ok) {
		if (fflush(dl_file) != 0 || fsync(fileno(dl_file)) != 0) {
			ok = false;
			snprintf(err, sizeof(err), "%s", "webdav_disk_full");
		}
	}

	pthread_mutex_lock(&dl_lock);
	dl_stream_open = false;
	pthread_mutex_unlock(&dl_lock);
	http_stream_close(&dl_stream);

	if (fclose(dl_file) != 0 && ok) {
		ok = false;
		snprintf(err, sizeof(err), "%s", "webdav_disk_full");
	}
	dl_file = NULL;

	growfile_finish(dl_audio_path, ok);

	if (ok) {
		// Re-evaluate fallback tags on the completed file (handles FLACs where
		// large embedded art pushed Vorbis comments past STREAM_PREBUFFER).
		maybe_write_fallback_tags(dl_audio_path, dl_title, dl_album);
		char inc[580];
		marker_path(dl_audio_path, inc, sizeof(inc));
		unlink(inc);
		trim_cache_locked_hint(dl_audio_path);
	} else {
		unlink_entry_and_sidecars(dl_audio_path);
	}

	pthread_mutex_lock(&dl_lock);
	dl_ok = ok;
	dl_done = true;
	dl_running = false;
	snprintf(dl_error, sizeof(dl_error), "%s", err);
	pthread_cond_broadcast(&dl_cv);
	pthread_mutex_unlock(&dl_lock);
	streamturn_release();
	return NULL;
}

static bool has_free_floor(const char *dir) {
	if (strncmp(dir, "/tmp/", 5) == 0) {
		return true; // host test tmpfs need not enforce 256 MiB SD floor
	}
	struct statvfs vfs;
	if (statvfs(dir, &vfs) != 0) {
		return true;
	}
	uint64_t avail = (uint64_t)vfs.f_bavail * (uint64_t)vfs.f_frsize;
	return avail >= FREE_FLOOR_BYTES;
}

bool webdavcache_get(const webdav_entry_t *track, char *out_path, size_t out_size,
					 char *error, size_t error_size) {
	reason(error, error_size, "");
	if (!track || track->kind != WEBDAV_TRACK || !out_path || out_size == 0) {
		reason(error, error_size, "webdav_invalid_path");
		return false;
	}
	if (usb_storage_active()) {
		reason(error, error_size, "webdav_no_card");
		return false;
	}

	pthread_mutex_lock(&dl_lock);
	if (!abandon_registered) {
		streamturn_register_abandon(webdavcache_cancel);
		abandon_registered = true;
	}
	pthread_mutex_unlock(&dl_lock);

	streamturn_abandon_all();
	stop_active_worker();

	if (webdavcache_cached(track, out_path, out_size)) {
		utime(out_path, NULL); // refresh LRU timestamp
		return true;
	}

	if (!webdav_configured()) {
		reason(error, error_size, "webdav_not_configured");
		return false;
	}

	char dir[512];
	if (!ensure_cache_dir(dir, sizeof(dir))) {
		reason(error, error_size, "webdav_no_card");
		return false;
	}
	if (!has_free_floor(dir)) {
		trim_cache_locked_hint(NULL);
		if (!has_free_floor(dir)) {
			reason(error, error_size, "webdav_disk_full");
			return false;
		}
	}

	char audio_path[560];
	if (!build_audio_path(track, audio_path, sizeof(audio_path))) {
		reason(error, error_size, "webdav_invalid_path");
		return false;
	}

	char url[WEBDAV_URL_MAX];
	char auth[WEBDAV_AUTH_HEADER_MAX];
	if (!webdav_file_request_info(track->path, url, sizeof(url), auth, sizeof(auth), error, error_size)) {
		wipe_bytes(url, sizeof(url));
		wipe_bytes(auth, sizeof(auth));
		return false;
	}

	streamturn_acquire();

	pthread_mutex_lock(&dl_lock);
	dl_cancel = false;
	dl_done = false;
	dl_ok = false;
	dl_have = 0;
	dl_connect_fd = -1;
	dl_stream_open = false;
	dl_error[0] = '\0';
	snprintf(dl_audio_path, sizeof(dl_audio_path), "%s", audio_path);
	snprintf(dl_title, sizeof(dl_title), "%s", track->title);
	snprintf(dl_album, sizeof(dl_album), "%s", track->album);
	dl_running = true;
	pthread_mutex_unlock(&dl_lock);

	int status = 0;
	http_stream_options_t opts = {
		.socket_cb = socket_observer_cb,
		.no_icy = true,
		.no_https_downgrade = true,
		.extra_headers = auth[0] ? auth : NULL,
		.status_out = &status,
	};
	bool opened = http_stream_open_ex(&dl_stream, url, STREAM_TIMEOUT_SECS, &opts);
	wipe_bytes(url, sizeof(url));
	wipe_bytes(auth, sizeof(auth));

	pthread_mutex_lock(&dl_lock);
	dl_connect_fd = -1;
	bool was_cancelled = dl_cancel;
	if (!opened || was_cancelled) {
		dl_running = false;
		pthread_mutex_unlock(&dl_lock);
		if (opened) {
			http_stream_close(&dl_stream);
		}
		streamturn_release();
		if (was_cancelled) {
			reason(error, error_size, "cancelled");
		} else if (status == 401 || status == 403) {
			reason(error, error_size, "webdav_auth_failed");
		} else if (status == 404) {
			reason(error, error_size, "webdav_not_found");
		} else {
			const char *he = http_last_error();
			if (he && strcmp(he, "subsonic_https_downgrade") == 0) {
				reason(error, error_size, "webdav_https_downgrade");
			} else {
				reason(error, error_size, (he && he[0]) ? he : "webdav_connection_failed");
			}
		}
		return false;
	}
	dl_stream_open = true;
	pthread_mutex_unlock(&dl_lock);

	char inc[580];
	marker_path(audio_path, inc, sizeof(inc));
	int mfd = open(inc, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
	if (mfd >= 0) {
		close(mfd);
	}
	dl_file = fopen(audio_path, "wb");
	if (!dl_file) {
		pthread_mutex_lock(&dl_lock);
		dl_stream_open = false;
		dl_running = false;
		pthread_mutex_unlock(&dl_lock);
		http_stream_close(&dl_stream);
		unlink(inc);
		streamturn_release();
		reason(error, error_size, "webdav_disk_full");
		return false;
	}

	long total_announce = dl_stream.content_length64 > (uint64_t)LONG_MAX
							  ? LONG_MAX
							  : (long)dl_stream.content_length64;
	growfile_announce(audio_path, total_announce);

	pthread_mutex_lock(&dl_lock);
	if (pthread_create(&dl_thread, NULL, download_worker, NULL) != 0) {
		dl_stream_open = false;
		dl_running = false;
		pthread_mutex_unlock(&dl_lock);
		growfile_finish(audio_path, false);
		fclose(dl_file);
		dl_file = NULL;
		http_stream_close(&dl_stream);
		unlink_entry_and_sidecars(audio_path);
		streamturn_release();
		reason(error, error_size, "webdav_connection_failed");
		return false;
	}
	dl_thread_live = true;

	// Progressive growfile codecs (FLAC, MP3, Opus) start as soon as
	// STREAM_PREBUFFER (512 KiB) lands; seek-on-open codecs wait for EOF.
	bool progressive = webdav_is_ram_streamable_suffix(track->suffix);
	while (!dl_done && !dl_cancel) {
		if (progressive && dl_have >= STREAM_PREBUFFER) {
			break;
		}
		pthread_cond_wait(&dl_cv, &dl_lock);
	}

	bool cancelled = dl_cancel;
	bool done = dl_done;
	bool ok = dl_ok;
	char err_copy[96];
	snprintf(err_copy, sizeof(err_copy), "%s", dl_error);
	pthread_mutex_unlock(&dl_lock);

	if (cancelled) {
		stop_active_worker();
		reason(error, error_size, "cancelled");
		return false;
	}
	if (done && !ok) {
		stop_active_worker();
		reason(error, error_size, err_copy[0] ? err_copy : "webdav_connection_failed");
		return false;
	}
	if (!done) {
		// Prebuffered progressive stream: write fallback sidecar if header had no title.
		maybe_write_fallback_tags(audio_path, track->title, track->album);
	}

	snprintf(out_path, out_size, "%s", audio_path);
	return true;
}
