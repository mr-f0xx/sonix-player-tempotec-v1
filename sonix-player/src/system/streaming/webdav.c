#include "webdav.h"

#include "src/system/core/config.h"
#include "src/system/core/json.h"
#include "src/system/ebook/epub_xml.h"
#include "src/system/net/http.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#define WEBDAV_XML_LIMIT (2 * 1024 * 1024)
#define WEBDAV_TIMEOUT_SECS 15
#define WEBDAV_MAX_DIR_ITEMS 4096

static const char PROPFIND_BODY[] =
	"<?xml version=\"1.0\" encoding=\"utf-8\"?>"
	"<D:propfind xmlns:D=\"DAV:\"><D:prop>"
	"<D:resourcetype/><D:getcontentlength/>"
	"</D:prop></D:propfind>";

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t credentials_io_lock = PTHREAD_MUTEX_INITIALIZER;
static webdav_credentials_t account;
static bool remembered;
static bool is_configured;
static bool sd_cache_enabled;
static unsigned api_inflight;
static char credentials_path[600];

static void wipe_bytes(void *buf, size_t len) {
	volatile unsigned char *p = (volatile unsigned char *)buf;
	while (len--) {
		*p++ = 0;
	}
}

static void copy_str(char *dst, size_t size, const char *src) {
	if (!dst || size == 0) {
		return;
	}
	if (!src) {
		dst[0] = '\0';
		return;
	}
	size_t n = strlen(src);
	if (n >= size) {
		n = size - 1;
	}
	memcpy(dst, src, n);
	dst[n] = '\0';
}

static void reason(char *out, size_t size, const char *text) {
	if (out && size) {
		snprintf(out, size, "%s", text ? text : "");
	}
}

static bool valid_text(const char *text, size_t capacity) {
	return text && strnlen(text, capacity) < capacity;
}

static int hex_val(char c) {
	if (c >= '0' && c <= '9') return c - '0';
	if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
	if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
	return -1;
}

static bool is_unreserved_uri_char(unsigned char c) {
	return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
		   c == '-' || c == '.' || c == '_' || c == '~';
}

// Decodes a URI path segment-by-segment into clean UTF-8, rejecting NUL bytes,
// encoded slashes (%2F), backslashes, and "." / ".." traversal segments.
// Resulting path has no trailing slash ("" represents root "/").
static bool decode_uri_path(const char *raw_path, size_t raw_len, char *out, size_t out_size) {
	if (!raw_path || !out || out_size == 0) {
		return false;
	}
	out[0] = '\0';
	size_t pos = 0;
	size_t i = 0;
	while (i < raw_len && raw_path[i] == '/') {
		i++;
	}
	while (i < raw_len) {
		size_t seg_start = i;
		while (i < raw_len && raw_path[i] != '/') {
			i++;
		}
		size_t seg_len = i - seg_start;
		while (i < raw_len && raw_path[i] == '/') {
			i++;
		}
		if (seg_len == 0) {
			continue;
		}
		char seg[WEBDAV_PATH_MAX];
		size_t sp = 0;
		for (size_t k = 0; k < seg_len; k++) {
			unsigned char ch = (unsigned char)raw_path[seg_start + k];
			if (ch == '%' && k + 2 < seg_len) {
				int hi = hex_val(raw_path[seg_start + k + 1]);
				int lo = hex_val(raw_path[seg_start + k + 2]);
				if (hi >= 0 && lo >= 0) {
					ch = (unsigned char)((hi << 4) | lo);
					k += 2;
				}
			}
			if (ch < 32 || ch == 127 || ch == '/' || ch == '\\') {
				return false;
			}
			if (sp + 1 >= sizeof(seg)) {
				return false;
			}
			seg[sp++] = (char)ch;
		}
		seg[sp] = '\0';
		if (strcmp(seg, ".") == 0 || strcmp(seg, "..") == 0) {
			return false;
		}
		if (pos + 1 + sp >= out_size) {
			return false;
		}
		out[pos++] = '/';
		memcpy(out + pos, seg, sp);
		pos += sp;
		out[pos] = '\0';
	}
	return true;
}

// Percent-encodes a `/` separated UTF-8 path (no leading slash required) into
// `out`, writing a leading `/` for each non-empty segment.
static bool encode_uri_segments(const char *decoded_path, char *out, size_t out_size) {
	if (!decoded_path || !out || out_size == 0) {
		return false;
	}
	out[0] = '\0';
	size_t pos = 0;
	const char *p = decoded_path;
	while (*p == '/') {
		p++;
	}
	while (*p) {
		const char *slash = strchr(p, '/');
		size_t seg_len = slash ? (size_t)(slash - p) : strlen(p);
		if (seg_len > 0) {
			if ((seg_len == 1 && p[0] == '.') || (seg_len == 2 && p[0] == '.' && p[1] == '.')) {
				return false;
			}
			if (pos + 1 >= out_size) {
				return false;
			}
			out[pos++] = '/';
			for (size_t i = 0; i < seg_len; i++) {
				unsigned char c = (unsigned char)p[i];
				if (c < 32 || c == 127 || c == '\\') {
					return false;
				}
				if (is_unreserved_uri_char(c)) {
					if (pos + 1 >= out_size) {
						return false;
					}
					out[pos++] = (char)c;
				} else {
					if (pos + 3 >= out_size) {
						return false;
					}
					static const char HEX[] = "0123456789ABCDEF";
					out[pos++] = '%';
					out[pos++] = HEX[(c >> 4) & 0x0F];
					out[pos++] = HEX[c & 0x0F];
				}
			}
			out[pos] = '\0';
		}
		if (!slash) {
			break;
		}
		p = slash + 1;
		while (*p == '/') {
			p++;
		}
	}
	return true;
}

static bool valid_rel_path(const char *rel_path) {
	if (!rel_path) {
		return false;
	}
	if (rel_path[0] == '/' || strnlen(rel_path, WEBDAV_PATH_MAX) >= WEBDAV_PATH_MAX) {
		return false;
	}
	const char *p = rel_path;
	while (*p) {
		const char *slash = strchr(p, '/');
		size_t len = slash ? (size_t)(slash - p) : strlen(p);
		if (len == 0 && slash) {
			return false; // empty double-slash segment
		}
		if ((len == 1 && p[0] == '.') || (len == 2 && p[0] == '.' && p[1] == '.')) {
			return false;
		}
		for (size_t i = 0; i < len; i++) {
			unsigned char c = (unsigned char)p[i];
			if (c < 32 || c == 127 || c == '\\') {
				return false;
			}
		}
		if (!slash) {
			break;
		}
		p = slash + 1;
	}
	return true;
}

static bool normalize(webdav_credentials_t *c, char *error, size_t size) {
	if (!c || !valid_text(c->server, sizeof(c->server)) ||
		!valid_text(c->username, sizeof(c->username)) ||
		!valid_text(c->password, sizeof(c->password))) {
		reason(error, size, "webdav_invalid_credentials");
		return false;
	}
	// Anonymous servers have empty username and password. A password without a
	// username, or a colon inside a Basic-Auth username (RFC 7617), is invalid.
	if ((!c->username[0] && c->password[0]) || strchr(c->username, ':') != NULL) {
		reason(error, size, "webdav_invalid_credentials");
		return false;
	}
	for (const unsigned char *p = (const unsigned char *)c->username; *p; p++) {
		if (*p < 32 || *p == 127) {
			reason(error, size, "webdav_invalid_credentials");
			return false;
		}
	}
	for (const unsigned char *p = (const unsigned char *)c->password; *p; p++) {
		if (*p < 32 || *p == 127) {
			reason(error, size, "webdav_invalid_credentials");
			return false;
		}
	}

	// Trim leading/trailing ASCII whitespace around pasted URLs.
	const char *s = c->server;
	while (*s == ' ' || *s == '\t') {
		s++;
	}
	size_t slen = strlen(s);
	while (slen > 0 && (s[slen - 1] == ' ' || s[slen - 1] == '\t' || s[slen - 1] == '\r' || s[slen - 1] == '\n')) {
		slen--;
	}
	char raw[WEBDAV_SERVER_MAX];
	if (slen == 0 || slen >= sizeof(raw)) {
		reason(error, size, "webdav_invalid_url");
		return false;
	}
	memcpy(raw, s, slen);
	raw[slen] = '\0';

	bool secure = strncasecmp(raw, "https://", 8) == 0;
	if (!secure && strncasecmp(raw, "http://", 7) != 0) {
		reason(error, size, "webdav_invalid_url");
		return false;
	}
	const char *authority = raw + (secure ? 8 : 7);
	if (!*authority || *authority == '/' || strpbrk(authority, "@?#\\")) {
		reason(error, size, "webdav_invalid_url");
		return false;
	}
	for (const unsigned char *p = (const unsigned char *)authority; *p; p++) {
		if (*p < 32 || *p == 127) {
			reason(error, size, "webdav_invalid_url");
			return false;
		}
	}
	const char *path_start = strchr(authority, '/');
	const char *auth_end = path_start ? path_start : authority + strlen(authority);
	const char *colon = memchr(authority, ':', (size_t)(auth_end - authority));
	const char *host_end = colon ? colon : auth_end;
	if (host_end == authority || host_end - authority >= 256) {
		reason(error, size, "webdav_invalid_url");
		return false;
	}
	for (const unsigned char *p = (const unsigned char *)authority; p < (const unsigned char *)host_end; p++) {
		if (!isalnum(*p) && *p != '.' && *p != '-' && *p != '_') {
			reason(error, size, "webdav_invalid_url");
			return false;
		}
	}
	if (colon) {
		unsigned port = 0;
		if (colon + 1 == auth_end) {
			reason(error, size, "webdav_invalid_url");
			return false;
		}
		for (const char *p = colon + 1; p < auth_end; p++) {
			if (*p < '0' || *p > '9' || (port = port * 10 + (unsigned)(*p - '0')) > 65535) {
				reason(error, size, "webdav_invalid_url");
				return false;
			}
		}
		if (!port) {
			reason(error, size, "webdav_invalid_url");
			return false;
		}
	}

	char decoded_base[WEBDAV_SERVER_MAX] = "";
	char encoded_base[WEBDAV_SERVER_MAX] = "";
	if (path_start) {
		if (!decode_uri_path(path_start, strlen(path_start), decoded_base, sizeof(decoded_base)) ||
			!encode_uri_segments(decoded_base, encoded_base, sizeof(encoded_base))) {
			reason(error, size, "webdav_invalid_url");
			return false;
		}
	}

	int n = snprintf(c->server, sizeof(c->server), "%s%.*s%s",
					 secure ? "https://" : "http://",
					 (int)(auth_end - authority), authority, encoded_base);
	if (n <= 0 || (size_t)n >= sizeof(c->server)) {
		reason(error, size, "webdav_invalid_url");
		return false;
	}
	return true;
}

static bool base64_encode(const unsigned char *in, size_t in_len, char *out, size_t out_size) {
	static const char B64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
	size_t needed = 4 * ((in_len + 2) / 3) + 1;
	if (!out || out_size < needed) {
		return false;
	}
	size_t j = 0;
	for (size_t i = 0; i < in_len; i += 3) {
		uint32_t a = in[i];
		uint32_t b = (i + 1 < in_len) ? in[i + 1] : 0;
		uint32_t c = (i + 2 < in_len) ? in[i + 2] : 0;
		uint32_t triple = (a << 16) | (b << 8) | c;
		out[j++] = B64[(triple >> 18) & 0x3F];
		out[j++] = B64[(triple >> 12) & 0x3F];
		out[j++] = (i + 1 < in_len) ? B64[(triple >> 6) & 0x3F] : '=';
		out[j++] = (i + 2 < in_len) ? B64[triple & 0x3F] : '=';
	}
	out[j] = '\0';
	return true;
}

static bool format_basic_auth(const webdav_credentials_t *c, char *out, size_t out_size) {
	if (!out || out_size == 0) {
		return false;
	}
	out[0] = '\0';
	if (!c->username[0]) {
		return true; // anonymous WebDAV server
	}
	char raw[WEBDAV_USER_MAX + WEBDAV_PASSWORD_MAX + 2];
	int n = snprintf(raw, sizeof(raw), "%s:%s", c->username, c->password);
	if (n <= 0 || (size_t)n >= sizeof(raw)) {
		wipe_bytes(raw, sizeof(raw));
		return false;
	}
	char b64[WEBDAV_AUTH_HEADER_MAX];
	bool ok = base64_encode((const unsigned char *)raw, (size_t)n, b64, sizeof(b64));
	wipe_bytes(raw, sizeof(raw));
	if (!ok) {
		wipe_bytes(b64, sizeof(b64));
		return false;
	}
	int written = snprintf(out, out_size, "Authorization: Basic %s\r\n", b64);
	wipe_bytes(b64, sizeof(b64));
	if (written <= 0 || (size_t)written >= out_size) {
		wipe_bytes(out, out_size);
		return false;
	}
	return true;
}

static bool write_json_string(FILE *f, const char *s) {
	if (fputc('"', f) == EOF) {
		return false;
	}
	for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
		if (*p == '"' || *p == '\\') {
			if (fputc('\\', f) == EOF || fputc(*p, f) == EOF) {
				return false;
			}
		} else if (*p < 32) {
			if (fprintf(f, "\\u%04x", *p) < 0) {
				return false;
			}
		} else if (fputc(*p, f) == EOF) {
			return false;
		}
	}
	return fputc('"', f) != EOF;
}

static bool save_credentials_file(const webdav_credentials_t *c) {
	if (!credentials_path[0]) {
		return false;
	}
	char tmp[sizeof(credentials_path) + 8];
	snprintf(tmp, sizeof(tmp), "%s.tmp", credentials_path);
	int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW | O_CLOEXEC, 0600);
	if (fd < 0) {
		return false;
	}
	if (fchmod(fd, 0600) != 0) {
		close(fd);
		unlink(tmp);
		return false;
	}
	FILE *f = fdopen(fd, "w");
	if (!f) {
		close(fd);
		unlink(tmp);
		return false;
	}
	bool ok = fputs("{\"server\":", f) >= 0 && write_json_string(f, c->server) &&
			  fputs(",\"username\":", f) >= 0 && write_json_string(f, c->username) &&
			  fputs(",\"password\":", f) >= 0 && write_json_string(f, c->password) &&
			  fputs("}\n", f) >= 0;
	if (fflush(f) != 0 || fsync(fd) != 0) {
		ok = false;
	}
	if (fclose(f) != 0) {
		ok = false;
	}
	if (ok) {
		ok = rename(tmp, credentials_path) == 0;
	}
	if (!ok) {
		unlink(tmp);
	}
	return ok;
}

void webdav_init(void) {
	credentials_path[0] = '\0';
	const char *path = config_path();
	if (path && path[0] && strlen(path) < sizeof(credentials_path) - 40) {
		snprintf(credentials_path, sizeof(credentials_path), "%s", path);
		char *slash = strrchr(credentials_path, '/');
		if (slash) {
			slash[1] = '\0';
			strcat(credentials_path, "webdav-credentials.json");
		} else {
			credentials_path[0] = '\0';
		}
	}

	webdav_credentials_t loaded = {0};
	char text[8192];
	int fd = credentials_path[0] ? open(credentials_path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC) : -1;
	bool have = false;
	if (fd >= 0) {
		struct stat st;
		if (fstat(fd, &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 0 && st.st_size < (off_t)sizeof(text)) {
			FILE *f = fdopen(fd, "r");
			if (f) {
				size_t n = fread(text, 1, sizeof(text) - 1, f);
				text[n] = '\0';
				json_doc_t doc;
				if (!ferror(f) && json_parse(text, &doc)) {
					int root = json_root(&doc);
					have = json_str_exact(&doc, json_get(&doc, root, "server"), loaded.server, sizeof(loaded.server)) &&
						   json_str_exact(&doc, json_get(&doc, root, "username"), loaded.username, sizeof(loaded.username)) &&
						   json_str_exact(&doc, json_get(&doc, root, "password"), loaded.password, sizeof(loaded.password)) &&
						   normalize(&loaded, NULL, 0);
					json_free(&doc);
				}
				fclose(f);
				fd = -1;
			}
		}
		if (fd >= 0) {
			close(fd);
		}
	}
	wipe_bytes(text, sizeof(text));

	bool cached_mode = config_get_bool("webdav", "sd_cache", false);

	pthread_mutex_lock(&lock);
	account = have ? loaded : (webdav_credentials_t){0};
	remembered = have;
	is_configured = have;
	sd_cache_enabled = cached_mode;
	pthread_mutex_unlock(&lock);
	wipe_bytes(&loaded, sizeof(loaded));
}

bool webdav_configure(const webdav_credentials_t *credentials, bool remember, char *error, size_t error_size) {
	reason(error, error_size, "");
	if (!credentials) {
		reason(error, error_size, "webdav_invalid_credentials");
		return false;
	}
	webdav_credentials_t clean = *credentials;
	if (!normalize(&clean, error, error_size)) {
		wipe_bytes(&clean, sizeof(clean));
		return false;
	}
	pthread_mutex_lock(&credentials_io_lock);
	if (remember) {
		if (!save_credentials_file(&clean)) {
			pthread_mutex_unlock(&credentials_io_lock);
			wipe_bytes(&clean, sizeof(clean));
			reason(error, error_size, "webdav_cannot_save_credentials");
			return false;
		}
	} else if (credentials_path[0] && unlink(credentials_path) != 0 && errno != ENOENT) {
		pthread_mutex_unlock(&credentials_io_lock);
		wipe_bytes(&clean, sizeof(clean));
		reason(error, error_size, "webdav_cannot_save_credentials");
		return false;
	}
	pthread_mutex_lock(&lock);
	account = clean;
	remembered = remember;
	is_configured = true;
	pthread_mutex_unlock(&lock);
	pthread_mutex_unlock(&credentials_io_lock);
	wipe_bytes(&clean, sizeof(clean));
	return true;
}

void webdav_credentials(webdav_credentials_t *out, bool *remembered_out) {
	pthread_mutex_lock(&lock);
	if (out) {
		*out = account;
	}
	if (remembered_out) {
		*remembered_out = remembered;
	}
	pthread_mutex_unlock(&lock);
}

bool webdav_configured(void) {
	pthread_mutex_lock(&lock);
	bool ok = is_configured && account.server[0] != '\0';
	pthread_mutex_unlock(&lock);
	return ok;
}

bool webdav_forget(void) {
	pthread_mutex_lock(&credentials_io_lock);
	pthread_mutex_lock(&lock);
	wipe_bytes(&account, sizeof(account));
	remembered = false;
	is_configured = false;
	pthread_mutex_unlock(&lock);
	bool removed = !credentials_path[0] || unlink(credentials_path) == 0 || errno == ENOENT;
	pthread_mutex_unlock(&credentials_io_lock);
	return removed;
}

bool webdav_api_busy(void) {
	pthread_mutex_lock(&lock);
	bool busy = api_inflight > 0;
	pthread_mutex_unlock(&lock);
	return busy;
}

bool webdav_sd_cache(void) {
	pthread_mutex_lock(&lock);
	bool enabled = sd_cache_enabled;
	pthread_mutex_unlock(&lock);
	return enabled;
}

void webdav_set_sd_cache(bool enabled) {
	pthread_mutex_lock(&lock);
	sd_cache_enabled = enabled;
	pthread_mutex_unlock(&lock);
	config_set_bool("webdav", "sd_cache", enabled);
	config_save();
}

bool webdav_is_ram_streamable_suffix(const char *ext) {
	if (!ext || !ext[0]) {
		return false;
	}
	return strcasecmp(ext, "flac") == 0 || strcasecmp(ext, "mp3") == 0 || strcasecmp(ext, "opus") == 0;
}

bool webdav_is_audio_suffix(const char *ext) {
	static const char *const EXTS[] = {
		"flac", "mp3", "opus", "ogg", "oga", "m4a", "mp4", "aac",
		"alac", "wav", "aiff", "aif", "wv", "ape", "dsf", "dff",
	};
	if (!ext || !ext[0]) {
		return false;
	}
	for (size_t i = 0; i < sizeof(EXTS) / sizeof(EXTS[0]); i++) {
		if (strcasecmp(ext, EXTS[i]) == 0) {
			return true;
		}
	}
	return false;
}

static bool build_target_url(const webdav_credentials_t *c, const char *rel_path, bool collection,
							 char *out, size_t out_size, char *error, size_t error_size) {
	if (!c->server[0]) {
		reason(error, error_size, "webdav_not_configured");
		return false;
	}
	if (!valid_rel_path(rel_path)) {
		reason(error, error_size, "webdav_invalid_path");
		return false;
	}
	char encoded_rel[WEBDAV_URL_MAX];
	if (!encode_uri_segments(rel_path, encoded_rel, sizeof(encoded_rel))) {
		reason(error, error_size, "webdav_invalid_path");
		return false;
	}
	int n = snprintf(out, out_size, "%s%s%s", c->server, encoded_rel, collection ? "/" : "");
	if (n <= 0 || (size_t)n >= out_size) {
		reason(error, error_size, "webdav_invalid_path");
		return false;
	}
	return true;
}

bool webdav_file_request_info(const char *rel_path, char *url_out, size_t url_size,
							  char *auth_header_out, size_t auth_size,
							  char *error, size_t error_size) {
	reason(error, error_size, "");
	if (!rel_path || !rel_path[0] || !url_out || url_size == 0) {
		reason(error, error_size, "webdav_invalid_path");
		return false;
	}
	if (auth_header_out && auth_size > 0) {
		auth_header_out[0] = '\0';
	}
	webdav_credentials_t copy;
	pthread_mutex_lock(&lock);
	bool configured = is_configured && account.server[0] != '\0';
	copy = account;
	pthread_mutex_unlock(&lock);
	if (!configured) {
		wipe_bytes(&copy, sizeof(copy));
		reason(error, error_size, "webdav_not_configured");
		return false;
	}
	bool ok = build_target_url(&copy, rel_path, false, url_out, url_size, error, error_size);
	if (ok && auth_header_out && auth_size > 0) {
		ok = format_basic_auth(&copy, auth_header_out, auth_size);
		if (!ok) {
			reason(error, error_size, "webdav_invalid_credentials");
		}
	}
	wipe_bytes(&copy, sizeof(copy));
	return ok;
}

bool webdav_server_key(char *out, size_t size) {
	if (!out || size == 0) {
		return false;
	}
	out[0] = '\0';
	pthread_mutex_lock(&lock);
	bool configured = is_configured && account.server[0] != '\0';
	int n = configured ? snprintf(out, size, "%s\n%s", account.server, account.username) : -1;
	pthread_mutex_unlock(&lock);
	return configured && n > 0 && (size_t)n < size;
}

static void map_http_error(int status, char *error, size_t error_size) {
	const char *last = http_last_error();
	if (status == 401 || status == 403) {
		reason(error, error_size, "webdav_auth_failed");
	} else if (status == 404) {
		reason(error, error_size, "webdav_not_found");
	} else if (status == 405 || status == 501) {
		reason(error, error_size, "webdav_not_supported");
	} else if (last && strcmp(last, "subsonic_https_downgrade") == 0) {
		reason(error, error_size, "webdav_https_downgrade");
	} else if (last && strcmp(last, "subsonic_invalid_url") == 0) {
		reason(error, error_size, "webdav_invalid_url");
	} else if (last && last[0]) {
		reason(error, error_size, last);
	} else {
		reason(error, error_size, "webdav_connection_failed");
	}
}

static bool propfind_request(const char *rel_path, int depth, char **xml_out, size_t *xml_len_out,
							 char *final_url_out, size_t final_url_size, char *error, size_t error_size) {
	webdav_credentials_t copy;
	pthread_mutex_lock(&lock);
	bool configured = is_configured && account.server[0] != '\0';
	copy = account;
	if (configured) {
		api_inflight++;
	}
	pthread_mutex_unlock(&lock);
	if (!configured) {
		wipe_bytes(&copy, sizeof(copy));
		reason(error, error_size, "webdav_not_configured");
		return false;
	}

	char url[WEBDAV_URL_MAX];
	char auth[WEBDAV_AUTH_HEADER_MAX];
	if (!build_target_url(&copy, rel_path ? rel_path : "", true, url, sizeof(url), error, error_size) ||
		!format_basic_auth(&copy, auth, sizeof(auth))) {
		wipe_bytes(&copy, sizeof(copy));
		wipe_bytes(auth, sizeof(auth));
		pthread_mutex_lock(&lock);
		api_inflight--;
		pthread_mutex_unlock(&lock);
		if (error && error_size && !error[0]) {
			reason(error, error_size, "webdav_invalid_credentials");
		}
		return false;
	}
	wipe_bytes(&copy, sizeof(copy));

	char headers[WEBDAV_AUTH_HEADER_MAX + 64];
	snprintf(headers, sizeof(headers), "Depth: %d\r\n%s", depth, auth);
	wipe_bytes(auth, sizeof(auth));

	http_req_t req = {
		.method = "PROPFIND",
		.extra_headers = headers,
		.body = PROPFIND_BODY,
		.body_len = sizeof(PROPFIND_BODY) - 1,
		.content_type = "application/xml; charset=utf-8",
		.allow_empty_body = (depth == 0),
		.no_https_downgrade = true,
		.accept_gzip = true,
	};

	int status = 0;
	char *body = NULL;
	size_t body_len = 0;
	bool ok = http_request(url, &req, &body, &body_len, WEBDAV_XML_LIMIT, WEBDAV_TIMEOUT_SECS, &status);
	wipe_bytes(headers, sizeof(headers));

	if (ok && final_url_out && final_url_size > 0) {
		const char *final = http_last_final_url();
		snprintf(final_url_out, final_url_size, "%s", (final && final[0]) ? final : url);
	}

	pthread_mutex_lock(&lock);
	api_inflight--;
	pthread_mutex_unlock(&lock);

	if (!ok) {
		free(body);
		map_http_error(status, error, error_size);
		return false;
	}
	if (status != 207 && !(depth == 0 && status >= 200 && status < 300)) {
		free(body);
		reason(error, error_size, "webdav_not_supported");
		return false;
	}
	if (body_len >= WEBDAV_XML_LIMIT - 1) {
		free(body);
		reason(error, error_size, "webdav_response_too_large");
		return false;
	}
	*xml_out = body;
	if (xml_len_out) {
		*xml_len_out = body_len;
	}
	return true;
}

static bool ascii_contains_ci(const char *haystack, const char *needle) {
	if (!haystack || !needle || !needle[0]) {
		return false;
	}
	size_t nlen = strlen(needle);
	for (const char *p = haystack; *p; p++) {
		if (strncasecmp(p, needle, nlen) == 0) {
			return true;
		}
	}
	return false;
}

bool webdav_ping(char *error, size_t error_size) {
	reason(error, error_size, "");
	char *xml = NULL;
	size_t xml_len = 0;
	if (!propfind_request("", 0, &xml, &xml_len, NULL, 0, error, error_size)) {
		return false;
	}
	bool has_dav = (xml_len == 0) || ascii_contains_ci(xml, "multistatus") || ascii_contains_ci(xml, "response");
	free(xml);
	if (!has_dav) {
		reason(error, error_size, "webdav_invalid_response");
		return false;
	}
	return true;
}

// Extracts the raw URI path component (starting with '/') from an http(s) URL.
static const char *url_raw_path(const char *url, size_t *len_out) {
	if (!url) {
		if (len_out) *len_out = 0;
		return "";
	}
	const char *p = url;
	if (strncasecmp(p, "https://", 8) == 0) {
		p += 8;
	} else if (strncasecmp(p, "http://", 7) == 0) {
		p += 7;
	}
	const char *slash = strchr(p, '/');
	if (!slash) {
		if (len_out) *len_out = 1;
		return "/";
	}
	size_t span = strcspn(slash, "?#");
	if (len_out) {
		*len_out = span;
	}
	return slash;
}

static bool propstat_is_ok(const char *status_line) {
	if (!status_line || !status_line[0]) {
		return true; // servers omitting <status> inside a single <propstat>
	}
	const char *sp = strchr(status_line, ' ');
	if (!sp) {
		return false;
	}
	while (*sp == ' ') {
		sp++;
	}
	int code = atoi(sp);
	return code >= 200 && code < 300;
}

static void extract_parent_album(const char *rel_path, char *out, size_t out_size) {
	if (!out || out_size == 0) {
		return;
	}
	out[0] = '\0';
	if (!rel_path || !rel_path[0]) {
		return;
	}
	const char *slash = strrchr(rel_path, '/');
	const char *seg = slash ? slash + 1 : rel_path;
	snprintf(out, out_size, "%s", seg);
}

static int natural_casecmp(const char *a, const char *b) {
	const unsigned char *pa = (const unsigned char *)(a ? a : "");
	const unsigned char *pb = (const unsigned char *)(b ? b : "");
	while (*pa && *pb) {
		if (isdigit(*pa) && isdigit(*pb)) {
			const unsigned char *za = pa;
			const unsigned char *zb = pb;
			while (*za == '0') za++;
			while (*zb == '0') zb++;
			const unsigned char *ea = za;
			const unsigned char *eb = zb;
			while (isdigit(*ea)) ea++;
			while (isdigit(*eb)) eb++;
			size_t la = (size_t)(ea - za);
			size_t lb = (size_t)(eb - zb);
			if (la != lb) {
				return la < lb ? -1 : 1;
			}
			int dcmp = memcmp(za, zb, la);
			if (dcmp != 0) {
				return dcmp < 0 ? -1 : 1;
			}
			size_t raw_a = (size_t)(ea - pa);
			size_t raw_b = (size_t)(eb - pb);
			if (raw_a != raw_b) {
				return raw_a > raw_b ? -1 : 1; // "02" before "2" on tie
			}
			pa = ea;
			pb = eb;
			continue;
		}
		int ca = tolower(*pa);
		int cb = tolower(*pb);
		if (ca != cb) {
			return ca - cb;
		}
		pa++;
		pb++;
	}
	return (int)tolower(*pa) - (int)tolower(*pb);
}

static int compare_entries(const void *a, const void *b) {
	const webdav_entry_t *ea = (const webdav_entry_t *)a;
	const webdav_entry_t *eb = (const webdav_entry_t *)b;
	if (ea->kind != eb->kind) {
		return ea->kind == WEBDAV_DIR ? -1 : 1; // directories before tracks
	}
	int cmp = natural_casecmp(ea->title, eb->title);
	return cmp != 0 ? cmp : strcmp(ea->path, eb->path);
}

typedef enum {
	TEXT_NONE,
	TEXT_HREF,
	TEXT_STATUS,
	TEXT_LENGTH,
} text_field_t;

static void append_unescaped(char *dst, size_t dst_size, const char *xml_text, size_t xml_len) {
	size_t cur = strlen(dst);
	if (cur + 1 >= dst_size || xml_len == 0) {
		return;
	}
	epub_xml_unescape(dst + cur, dst_size - cur, xml_text, xml_len);
}

int webdav_browse(const char *rel_path, int offset, webdav_entry_t *entries, int limit, bool *more,
				  char *error, size_t error_size) {
	reason(error, error_size, "");
	if (more) {
		*more = false;
	}
	if (!rel_path) {
		rel_path = "";
	}
	if (offset < 0 || limit <= 0 || limit > WEBDAV_PAGE_SIZE || !entries) {
		reason(error, error_size, "webdav_invalid_path");
		return -1;
	}
	if (!valid_rel_path(rel_path)) {
		reason(error, error_size, "webdav_invalid_path");
		return -1;
	}

	char *xml = NULL;
	size_t xml_len = 0;
	char final_url[WEBDAV_URL_MAX] = "";
	if (!propfind_request(rel_path, 1, &xml, &xml_len, final_url, sizeof(final_url), error, error_size)) {
		return -1;
	}

	size_t req_raw_len = 0;
	const char *req_raw = url_raw_path(final_url, &req_raw_len);
	char req_dir_decoded[WEBDAV_URL_MAX];
	if (!decode_uri_path(req_raw, req_raw_len, req_dir_decoded, sizeof(req_dir_decoded))) {
		free(xml);
		reason(error, error_size, "webdav_invalid_response");
		return -1;
	}
	size_t req_dir_len = strlen(req_dir_decoded);

	char folder_album[WEBDAV_TITLE_MAX];
	extract_parent_album(rel_path, folder_album, sizeof(folder_album));

	size_t cap = 64;
	size_t total = 0;
	webdav_entry_t *all = calloc(cap, sizeof(*all));
	if (!all) {
		free(xml);
		reason(error, error_size, "webdav_out_of_memory");
		return -1;
	}

	bool saw_multistatus = false;
	bool in_response = false;
	bool in_propstat = false;
	bool in_resourcetype = false;
	text_field_t active_field = TEXT_NONE;

	char resp_href[WEBDAV_URL_MAX] = "";
	bool resp_is_dir = false;
	uint64_t resp_size = 0;

	char ps_status[96] = "";
	char ps_length[48] = "";
	bool ps_is_dir = false;

	epub_xml_t x;
	epub_xml_init(&x, xml);
	while (epub_xml_next(&x)) {
		if (x.event == EPUB_XML_OPEN) {
			active_field = TEXT_NONE;
			if (epub_xml_is(&x, "multistatus")) {
				saw_multistatus = true;
			} else if (epub_xml_is(&x, "response") && !x.self_closing) {
				in_response = true;
				in_propstat = false;
				in_resourcetype = false;
				resp_href[0] = '\0';
				resp_is_dir = false;
				resp_size = 0;
			} else if (in_response && epub_xml_is(&x, "href") && !in_propstat && !x.self_closing) {
				resp_href[0] = '\0';
				active_field = TEXT_HREF;
			} else if (in_response && epub_xml_is(&x, "propstat") && !x.self_closing) {
				in_propstat = true;
				in_resourcetype = false;
				ps_status[0] = '\0';
				ps_length[0] = '\0';
				ps_is_dir = false;
			} else if (in_propstat && epub_xml_is(&x, "resourcetype") && !x.self_closing) {
				in_resourcetype = true;
			} else if (in_resourcetype && epub_xml_is(&x, "collection")) {
				ps_is_dir = true;
			} else if (in_propstat && epub_xml_is(&x, "status") && !x.self_closing) {
				ps_status[0] = '\0';
				active_field = TEXT_STATUS;
			} else if (in_propstat && epub_xml_is(&x, "getcontentlength") && !x.self_closing) {
				ps_length[0] = '\0';
				active_field = TEXT_LENGTH;
			}
		} else if (x.event == EPUB_XML_TEXT) {
			switch (active_field) {
			case TEXT_HREF:
				append_unescaped(resp_href, sizeof(resp_href), x.text, x.text_len);
				break;
			case TEXT_STATUS:
				append_unescaped(ps_status, sizeof(ps_status), x.text, x.text_len);
				break;
			case TEXT_LENGTH:
				append_unescaped(ps_length, sizeof(ps_length), x.text, x.text_len);
				break;
			case TEXT_NONE:
				break;
			}
		} else if (x.event == EPUB_XML_CLOSE) {
			active_field = TEXT_NONE;
			if (in_resourcetype && epub_xml_is(&x, "resourcetype")) {
				in_resourcetype = false;
			} else if (in_propstat && epub_xml_is(&x, "propstat")) {
				if (propstat_is_ok(ps_status)) {
					if (ps_is_dir) {
						resp_is_dir = true;
					}
					if (ps_length[0]) {
						char *end = NULL;
						unsigned long long v = strtoull(ps_length, &end, 10);
						if (end && end != ps_length) {
							resp_size = (uint64_t)v;
						}
					}
				}
				in_propstat = false;
			} else if (in_response && epub_xml_is(&x, "response")) {
				in_response = false;
				// Trim whitespace around <href>.
				const char *h = resp_href;
				while (*h == ' ' || *h == '\t' || *h == '\r' || *h == '\n') {
					h++;
				}
				size_t hlen = strlen(h);
				while (hlen > 0 && (h[hlen - 1] == ' ' || h[hlen - 1] == '\t' || h[hlen - 1] == '\r' || h[hlen - 1] == '\n')) {
					hlen--;
				}
				if (hlen == 0) {
					continue;
				}
				char href_clean[WEBDAV_URL_MAX];
				if (hlen >= sizeof(href_clean)) {
					continue;
				}
				memcpy(href_clean, h, hlen);
				href_clean[hlen] = '\0';

				char resolved_raw[WEBDAV_URL_MAX];
				const char *raw_p = NULL;
				size_t raw_l = 0;
				if (strncasecmp(href_clean, "http://", 7) == 0 || strncasecmp(href_clean, "https://", 8) == 0) {
					raw_p = url_raw_path(href_clean, &raw_l);
				} else if (href_clean[0] == '/') {
					raw_p = href_clean;
					raw_l = strcspn(href_clean, "?#");
				} else {
					// Relative href: resolve against requested collection path.
					size_t base_l = req_raw_len;
					while (base_l > 1 && req_raw[base_l - 1] == '/') {
						base_l--;
					}
					size_t rel_l = strcspn(href_clean, "?#");
					int rn = snprintf(resolved_raw, sizeof(resolved_raw), "%.*s/%.*s",
									  (int)base_l, req_raw, (int)rel_l, href_clean);
					if (rn <= 0 || (size_t)rn >= sizeof(resolved_raw)) {
						continue;
					}
					raw_p = resolved_raw;
					raw_l = (size_t)rn;
				}

				bool href_trailing_slash = (raw_l > 1 && raw_p[raw_l - 1] == '/');
				char href_decoded[WEBDAV_URL_MAX];
				if (!decode_uri_path(raw_p, raw_l, href_decoded, sizeof(href_decoded))) {
					continue;
				}
				if (strcmp(href_decoded, req_dir_decoded) == 0) {
					continue; // self-entry for the requested directory
				}

				const char *child = NULL;
				if (req_dir_len == 0) {
					if (href_decoded[0] == '/') {
						child = href_decoded + 1;
					}
				} else if (strncmp(href_decoded, req_dir_decoded, req_dir_len) == 0 &&
						   href_decoded[req_dir_len] == '/') {
					child = href_decoded + req_dir_len + 1;
				}
				if (!child || !child[0] || strchr(child, '/') != NULL) {
					continue; // outside folder or deeper than Depth: 1
				}
				if (child[0] == '.') {
					continue; // hidden dot-file / dot-folder
				}

				bool is_dir = resp_is_dir || href_trailing_slash;
				char suffix[16] = "";
				if (!is_dir) {
					const char *dot = strrchr(child, '.');
					if (!dot || !dot[1] || strlen(dot + 1) >= sizeof(suffix)) {
						continue;
					}
					for (size_t k = 0; dot[1 + k]; k++) {
						suffix[k] = (char)tolower((unsigned char)dot[1 + k]);
						suffix[k + 1] = '\0';
					}
					if (!webdav_is_audio_suffix(suffix)) {
						continue;
					}
				}

				char entry_rel[WEBDAV_PATH_MAX];
				int pn = rel_path[0] ? snprintf(entry_rel, sizeof(entry_rel), "%s/%s", rel_path, child)
									 : snprintf(entry_rel, sizeof(entry_rel), "%s", child);
				if (pn <= 0 || (size_t)pn >= sizeof(entry_rel)) {
					continue;
				}

				// Deduplicate if a server emits duplicate <response> blocks.
				bool dup = false;
				for (size_t d = 0; d < total; d++) {
					if (strcmp(all[d].path, entry_rel) == 0) {
						dup = true;
						break;
					}
				}
				if (dup) {
					continue;
				}
				if (total >= WEBDAV_MAX_DIR_ITEMS) {
					break;
				}
				if (total >= cap) {
					size_t next_cap = cap * 2;
					if (next_cap > WEBDAV_MAX_DIR_ITEMS) {
						next_cap = WEBDAV_MAX_DIR_ITEMS;
					}
					webdav_entry_t *grown = realloc(all, next_cap * sizeof(*grown));
					if (!grown) {
						free(all);
						free(xml);
						reason(error, error_size, "webdav_out_of_memory");
						return -1;
					}
					all = grown;
					cap = next_cap;
				}

				webdav_entry_t *item = &all[total++];
				memset(item, 0, sizeof(*item));
				item->kind = is_dir ? WEBDAV_DIR : WEBDAV_TRACK;
				copy_str(item->path, sizeof(item->path), entry_rel);
				copy_str(item->title, sizeof(item->title), child);
				copy_str(item->album, sizeof(item->album), folder_album);
				copy_str(item->suffix, sizeof(item->suffix), suffix);
				item->size = is_dir ? 0 : resp_size;
			}
		}
	}

	free(xml);
	if (!saw_multistatus) {
		free(all);
		reason(error, error_size, "webdav_invalid_response");
		return -1;
	}

	if (total > 1) {
		qsort(all, total, sizeof(*all), compare_entries);
	}

	int out_count = 0;
	if ((size_t)offset < total) {
		size_t avail = total - (size_t)offset;
		out_count = avail > (size_t)limit ? limit : (int)avail;
		memcpy(entries, all + offset, (size_t)out_count * sizeof(*entries));
		if (more) {
			*more = (size_t)offset + (size_t)out_count < total;
		}
	}
	free(all);
	return out_count;
}
