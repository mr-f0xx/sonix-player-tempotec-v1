#include "jellyfin.h"

#include "../core/config.h"
#include "../core/json.h"
#include "../core/utils.h"
#include "../net/http.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define JELLYFIN_SECTION "jellyfin"
#define JELLYFIN_CREDENTIALS_FILE "jellyfin-credentials.json"
#define JELLYFIN_HTTP_TIMEOUT_MS 15000
#define JELLYFIN_REPORT_TIMEOUT_MS 5000
#define JELLYFIN_MAX_BODY (512 * 1024)
#define JELLYFIN_MAX_JSON_TOKENS 12000
#define JELLYFIN_FAV_MAX 256
#define JELLYFIN_REPORT_QUEUE_MAX 8
#define JELLYFIN_WORKER_STACK (256 * 1024)

// 1 second = 10,000,000 Jellyfin ticks (100 ns per tick).
#define JELLYFIN_TICKS_PER_SEC 10000000LL

typedef struct {
	char server[JELLYFIN_SERVER_MAX];
	char server_name[JELLYFIN_NAME_MAX];
	char user_id[JELLYFIN_ID_MAX];
	char username[JELLYFIN_USER_MAX];
	char access_token[JELLYFIN_TOKEN_MAX];
	char library_id[JELLYFIN_ID_MAX];
	char library_name[JELLYFIN_TITLE_MAX];
	bool logged_in;
	bool remembered;
} jellyfin_session_t;

static pthread_mutex_t session_lock = PTHREAD_MUTEX_INITIALIZER;
static jellyfin_session_t session;
static char device_id_cache[JELLYFIN_DEVICE_ID_MAX];
static char last_error_buf[160];
static int api_busy_count;

// Small session-local cache of favourite states so the player heart / quick
// panel stays responsive without blocking the GUI thread on an HTTP round-trip.
typedef struct {
	char id[JELLYFIN_ID_MAX];
	bool favorite;
} jellyfin_fav_slot_t;

static pthread_mutex_t fav_lock = PTHREAD_MUTEX_INITIALIZER;
static jellyfin_fav_slot_t fav_slots[JELLYFIN_FAV_MAX];
static int fav_count;

// Background non-blocking playback reporting queue.
typedef enum {
	JF_REP_START = 0,
	JF_REP_PROGRESS = 1,
	JF_REP_STOPPED = 2,
} jf_report_kind_t;

typedef struct {
	jf_report_kind_t kind;
	char item_id[JELLYFIN_ID_MAX];
	char play_session_id[JELLYFIN_ID_MAX];
	long long position_ticks;
	bool is_paused;
	bool transcoded;
} jf_report_event_t;

static pthread_mutex_t report_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t report_cond = PTHREAD_COND_INITIALIZER;
static jf_report_event_t report_queue[JELLYFIN_REPORT_QUEUE_MAX];
static int report_head;
static int report_tail;
static int report_count;
static bool report_thread_running;

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
	const char *text = (msg && msg[0]) ? msg : "jellyfin_connection_failed";
	pthread_mutex_lock(&session_lock);
	copy_text(last_error_buf, sizeof(last_error_buf), text);
	pthread_mutex_unlock(&session_lock);
	if (out && out_size > 0)
		copy_text(out, out_size, text);
}

static void clear_error(char *out, size_t out_size) {
	pthread_mutex_lock(&session_lock);
	last_error_buf[0] = '\0';
	pthread_mutex_unlock(&session_lock);
	if (out && out_size > 0)
		out[0] = '\0';
}

static void api_enter(void) {
	pthread_mutex_lock(&session_lock);
	api_busy_count++;
	pthread_mutex_unlock(&session_lock);
}

static void api_leave(void) {
	pthread_mutex_lock(&session_lock);
	if (api_busy_count > 0)
		api_busy_count--;
	pthread_mutex_unlock(&session_lock);
}

bool jellyfin_api_busy(void) {
	pthread_mutex_lock(&session_lock);
	bool busy = api_busy_count > 0;
	pthread_mutex_unlock(&session_lock);
	return busy;
}

const char *jellyfin_last_error(void) {
	return last_error_buf;
}

// ---------------------------------------------------------------------------
// Device ID & credentials file path
// ---------------------------------------------------------------------------

static bool valid_id_chars(const char *id) {
	if (!id || !id[0])
		return false;
	for (const unsigned char *p = (const unsigned char *)id; *p; p++) {
		if (!(isalnum(*p) || *p == '-' || *p == '_'))
			return false;
	}
	return true;
}

static void ensure_device_id_locked(void) {
	if (device_id_cache[0])
		return;

	const char *saved = config_get(JELLYFIN_SECTION, "device_id", "");
	if (saved && saved[0] && valid_id_chars(saved) && strlen(saved) < sizeof(device_id_cache)) {
		copy_text(device_id_cache, sizeof(device_id_cache), saved);
		return;
	}

	unsigned char rnd[16];
	memset(rnd, 0, sizeof(rnd));
	int fd = open("/dev/urandom", O_RDONLY);
	if (fd >= 0) {
		size_t got = 0;
		while (got < sizeof(rnd)) {
			ssize_t n = read(fd, rnd + got, sizeof(rnd) - got);
			if (n <= 0)
				break;
			got += (size_t)n;
		}
		close(fd);
	} else {
		unsigned int seed = (unsigned int)time(NULL) ^ (unsigned int)getpid();
		for (size_t i = 0; i < sizeof(rnd); i++) {
			seed = seed * 1103515245u + 12345u;
			rnd[i] = (unsigned char)((seed >> 16) & 0xffu);
		}
	}

	static const char hex[] = "0123456789abcdef";
	char id[JELLYFIN_DEVICE_ID_MAX];
	int pos = snprintf(id, sizeof(id), "sonix-v1-");
	for (size_t i = 0; i < sizeof(rnd) && pos + 2 < (int)sizeof(id); i++) {
		id[pos++] = hex[(rnd[i] >> 4) & 0x0f];
		id[pos++] = hex[rnd[i] & 0x0f];
	}
	id[pos] = '\0';

	copy_text(device_id_cache, sizeof(device_id_cache), id);
	config_set(JELLYFIN_SECTION, "device_id", device_id_cache);
	config_save();
}

void jellyfin_device_id(char *out, size_t out_size) {
	if (!out || out_size == 0)
		return;
	pthread_mutex_lock(&session_lock);
	ensure_device_id_locked();
	copy_text(out, out_size, device_id_cache);
	pthread_mutex_unlock(&session_lock);
}

static bool credentials_path(char *out, size_t out_size) {
	if (!out || out_size == 0)
		return false;
	const char *cfg = config_path();
	if (!cfg || !cfg[0])
		return false;
	const char *slash = strrchr(cfg, '/');
	if (!slash) {
		return (size_t)snprintf(out, out_size, "%s", JELLYFIN_CREDENTIALS_FILE) < out_size;
	}
	size_t dir_len = (size_t)(slash - cfg);
	if (dir_len == 0) {
		return (size_t)snprintf(out, out_size, "/%s", JELLYFIN_CREDENTIALS_FILE) < out_size;
	}
	return (size_t)snprintf(out, out_size, "%.*s/%s", (int)dir_len, cfg, JELLYFIN_CREDENTIALS_FILE) < out_size;
}

// ---------------------------------------------------------------------------
// JSON string escaping & URL encoding
// ---------------------------------------------------------------------------

static bool json_append_escaped(char *buf, size_t buf_size, size_t *pos, const char *s) {
	if (!buf || !pos)
		return false;
	if (!s)
		s = "";
	if (*pos + 2 >= buf_size)
		return false;
	buf[(*pos)++] = '"';
	for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
		if (*p == '"' || *p == '\\') {
			if (*pos + 2 >= buf_size)
				return false;
			buf[(*pos)++] = '\\';
			buf[(*pos)++] = (char)*p;
		} else if (*p == '\n') {
			if (*pos + 2 >= buf_size)
				return false;
			buf[(*pos)++] = '\\';
			buf[(*pos)++] = 'n';
		} else if (*p == '\r') {
			if (*pos + 2 >= buf_size)
				return false;
			buf[(*pos)++] = '\\';
			buf[(*pos)++] = 'r';
		} else if (*p == '\t') {
			if (*pos + 2 >= buf_size)
				return false;
			buf[(*pos)++] = '\\';
			buf[(*pos)++] = 't';
		} else if (*p < 0x20) {
			if (*pos + 6 >= buf_size)
				return false;
			int n = snprintf(buf + *pos, buf_size - *pos, "\\u%04x", (unsigned int)*p);
			if (n < 0 || *pos + (size_t)n >= buf_size)
				return false;
			*pos += (size_t)n;
		} else {
			if (*pos + 1 >= buf_size)
				return false;
			buf[(*pos)++] = (char)*p;
		}
	}
	if (*pos + 2 > buf_size)
		return false;
	buf[(*pos)++] = '"';
	buf[*pos] = '\0';
	return true;
}

static bool url_encode(const char *in, char *out, size_t out_size) {
	if (!out || out_size == 0)
		return false;
	if (!in) {
		out[0] = '\0';
		return true;
	}
	static const char hex[] = "0123456789ABCDEF";
	size_t pos = 0;
	for (const unsigned char *p = (const unsigned char *)in; *p; p++) {
		if (isalnum(*p) || *p == '-' || *p == '_' || *p == '.' || *p == '~') {
			if (pos + 2 > out_size)
				return false;
			out[pos++] = (char)*p;
		} else {
			if (pos + 4 > out_size)
				return false;
			out[pos++] = '%';
			out[pos++] = hex[(*p >> 4) & 0x0f];
			out[pos++] = hex[*p & 0x0f];
		}
	}
	out[pos] = '\0';
	return true;
}

// ---------------------------------------------------------------------------
// Server URL normalization & strict origin matching
// ---------------------------------------------------------------------------

typedef struct {
	bool https;
	char host[256];
	int port;
} jellyfin_origin_t;

static bool parse_origin(const char *url, jellyfin_origin_t *out) {
	if (!url || !out)
		return false;
	memset(out, 0, sizeof(*out));
	const char *p = url;
	if (strncasecmp(p, "https://", 8) == 0) {
		out->https = true;
		out->port = 443;
		p += 8;
	} else if (strncasecmp(p, "http://", 7) == 0) {
		out->https = false;
		out->port = 80;
		p += 7;
	} else {
		return false;
	}

	// Reject IPv6 brackets and embedded credentials (`user@host`).
	if (*p == '[' || *p == '\0' || *p == '/')
		return false;

	const char *host_start = p;
	const char *host_end = p;
	while (*host_end && *host_end != '/' && *host_end != ':' && *host_end != '?' && *host_end != '#') {
		if (*host_end == '@' || (unsigned char)*host_end <= 0x20 || (unsigned char)*host_end >= 0x7f)
			return false;
		host_end++;
	}
	size_t host_len = (size_t)(host_end - host_start);
	if (host_len == 0 || host_len >= sizeof(out->host))
		return false;
	for (size_t i = 0; i < host_len; i++)
		out->host[i] = (char)tolower((unsigned char)host_start[i]);
	out->host[host_len] = '\0';

	if (*host_end == ':') {
		const char *port_start = host_end + 1;
		if (!isdigit((unsigned char)*port_start))
			return false;
		long port = 0;
		const char *q = port_start;
		while (isdigit((unsigned char)*q)) {
			port = port * 10 + (*q - '0');
			if (port > 65535)
				return false;
			q++;
		}
		if (port <= 0 || (*q && *q != '/' && *q != '?' && *q != '#'))
			return false;
		out->port = (int)port;
	}
	return true;
}

static bool origins_match(const char *a, const char *b) {
	jellyfin_origin_t oa, ob;
	if (!parse_origin(a, &oa) || !parse_origin(b, &ob))
		return false;
	return oa.https == ob.https && oa.port == ob.port && strcmp(oa.host, ob.host) == 0;
}

bool jellyfin_normalize_server(const char *raw, char *out, size_t out_size, char *error, size_t error_size) {
	if (!out || out_size == 0)
		return false;
	out[0] = '\0';
	if (!raw) {
		set_error(error, error_size, "jellyfin_invalid_server");
		return false;
	}

	while (*raw && isspace((unsigned char)*raw))
		raw++;
	size_t len = strlen(raw);
	while (len > 0 && isspace((unsigned char)raw[len - 1]))
		len--;
	if (len == 0) {
		set_error(error, error_size, "jellyfin_invalid_server");
		return false;
	}

	// Reject control characters, whitespace, query strings, and fragments.
	for (size_t i = 0; i < len; i++) {
		unsigned char c = (unsigned char)raw[i];
		if (c <= 0x20 || c >= 0x7f || c == '?' || c == '#') {
			set_error(error, error_size, "jellyfin_invalid_server");
			return false;
		}
	}

	char candidate[JELLYFIN_SERVER_MAX];
	bool has_scheme = (len >= 7 && strncasecmp(raw, "http://", 7) == 0) ||
					  (len >= 8 && strncasecmp(raw, "https://", 8) == 0);
	if (has_scheme) {
		if (len >= sizeof(candidate)) {
			set_error(error, error_size, "jellyfin_invalid_server");
			return false;
		}
		memcpy(candidate, raw, len);
		candidate[len] = '\0';
	} else {
		// Reject other explicit schemes (ftp://, file://, etc.).
		if (strstr(raw, "://")) {
			set_error(error, error_size, "jellyfin_invalid_server");
			return false;
		}
		// Default to https:// unless a bare IP/localhost/port is used, where
		// http:// is common on home LANs (e.g. 192.168.1.50:8096).
		const char *default_scheme = "https://";
		if (isdigit((unsigned char)raw[0]) || strncasecmp(raw, "localhost", 9) == 0 ||
			strchr(raw, ':') != NULL) {
			default_scheme = "http://";
		}
		if ((size_t)snprintf(candidate, sizeof(candidate), "%s%.*s", default_scheme, (int)len, raw) >=
			sizeof(candidate)) {
			set_error(error, error_size, "jellyfin_invalid_server");
			return false;
		}
	}

	// Lowercase scheme so prefix comparisons are canonical.
	if (strncasecmp(candidate, "https://", 8) == 0)
		memcpy(candidate, "https://", 8);
	else if (strncasecmp(candidate, "http://", 7) == 0)
		memcpy(candidate, "http://", 7);

	// Validate host & port.
	jellyfin_origin_t origin;
	if (!parse_origin(candidate, &origin)) {
		set_error(error, error_size, "jellyfin_invalid_server");
		return false;
	}

	// Strip trailing slashes (preserving any reverse-proxy subpath like `/jellyfin`).
	size_t clen = strlen(candidate);
	const char *scheme_end = strstr(candidate, "://") + 3;
	while (clen > (size_t)(scheme_end - candidate) && candidate[clen - 1] == '/') {
		candidate[--clen] = '\0';
	}
	if (clen >= out_size) {
		set_error(error, error_size, "jellyfin_invalid_server");
		return false;
	}
	memcpy(out, candidate, clen + 1);
	clear_error(error, error_size);
	return true;
}

// ---------------------------------------------------------------------------
// Authorization header (MediaBrowser scheme)
// ---------------------------------------------------------------------------

static bool header_safe_token(const char *s) {
	if (!s)
		return true;
	for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
		if (*p < 0x20 || *p >= 0x7f || *p == '"' || *p == '\\')
			return false;
	}
	return true;
}

static bool build_auth_header(const char *device_id, const char *token, char *out, size_t out_size) {
	if (!out || out_size == 0)
		return false;
	out[0] = '\0';
	if (!device_id || !device_id[0] || !header_safe_token(device_id))
		return false;
	if (token && token[0] && !header_safe_token(token))
		return false;

	int n;
	if (token && token[0]) {
		n = snprintf(out, out_size,
					 "Authorization: MediaBrowser Client=\"%s\", Device=\"%s\", DeviceId=\"%s\", "
					 "Version=\"%s\", Token=\"%s\"\r\n",
					 JELLYFIN_CLIENT_NAME, JELLYFIN_DEVICE_NAME, device_id, JELLYFIN_CLIENT_VERSION, token);
	} else {
		n = snprintf(out, out_size,
					 "Authorization: MediaBrowser Client=\"%s\", Device=\"%s\", DeviceId=\"%s\", "
					 "Version=\"%s\"\r\n",
					 JELLYFIN_CLIENT_NAME, JELLYFIN_DEVICE_NAME, device_id, JELLYFIN_CLIENT_VERSION);
	}
	return n > 0 && (size_t)n < out_size;
}

void jellyfin_headers_for_url(const char *for_url, char *out, size_t out_size) {
	if (!out || out_size == 0)
		return;
	out[0] = '\0';
	if (!for_url || !for_url[0])
		return;

	char server[JELLYFIN_SERVER_MAX];
	char token[JELLYFIN_TOKEN_MAX];
	char dev_id[JELLYFIN_DEVICE_ID_MAX];
	bool logged_in = false;

	pthread_mutex_lock(&session_lock);
	ensure_device_id_locked();
	copy_text(server, sizeof(server), session.server);
	copy_text(token, sizeof(token), session.access_token);
	copy_text(dev_id, sizeof(dev_id), device_id_cache);
	logged_in = session.logged_in;
	pthread_mutex_unlock(&session_lock);

	if (!logged_in || !server[0] || !token[0])
		return;
	if (!origins_match(server, for_url))
		return;

	build_auth_header(dev_id, token, out, out_size);
}

// ---------------------------------------------------------------------------
// Credentials persistence (0600 JSON, token only — NEVER password)
// ---------------------------------------------------------------------------

static bool write_credentials_file(const jellyfin_session_t *s, char *error, size_t error_size) {
	char path[512];
	if (!credentials_path(path, sizeof(path))) {
		set_error(error, error_size, "jellyfin_cannot_save_credentials");
		return false;
	}
	char tmp[528];
	if ((size_t)snprintf(tmp, sizeof(tmp), "%s.tmp", path) >= sizeof(tmp)) {
		set_error(error, error_size, "jellyfin_cannot_save_credentials");
		return false;
	}

	char buf[2048];
	size_t pos = 0;
	int n = snprintf(buf, sizeof(buf), "{\"server\":");
	if (n < 0 || (size_t)n >= sizeof(buf))
		goto fail_buf;
	pos = (size_t)n;
	if (!json_append_escaped(buf, sizeof(buf), &pos, s->server))
		goto fail_buf;

	n = snprintf(buf + pos, sizeof(buf) - pos, ",\"server_name\":");
	if (n < 0 || pos + (size_t)n >= sizeof(buf))
		goto fail_buf;
	pos += (size_t)n;
	if (!json_append_escaped(buf, sizeof(buf), &pos, s->server_name))
		goto fail_buf;

	n = snprintf(buf + pos, sizeof(buf) - pos, ",\"user_id\":");
	if (n < 0 || pos + (size_t)n >= sizeof(buf))
		goto fail_buf;
	pos += (size_t)n;
	if (!json_append_escaped(buf, sizeof(buf), &pos, s->user_id))
		goto fail_buf;

	n = snprintf(buf + pos, sizeof(buf) - pos, ",\"username\":");
	if (n < 0 || pos + (size_t)n >= sizeof(buf))
		goto fail_buf;
	pos += (size_t)n;
	if (!json_append_escaped(buf, sizeof(buf), &pos, s->username))
		goto fail_buf;

	n = snprintf(buf + pos, sizeof(buf) - pos, ",\"access_token\":");
	if (n < 0 || pos + (size_t)n >= sizeof(buf))
		goto fail_buf;
	pos += (size_t)n;
	if (!json_append_escaped(buf, sizeof(buf), &pos, s->access_token))
		goto fail_buf;

	n = snprintf(buf + pos, sizeof(buf) - pos, ",\"library_id\":");
	if (n < 0 || pos + (size_t)n >= sizeof(buf))
		goto fail_buf;
	pos += (size_t)n;
	if (!json_append_escaped(buf, sizeof(buf), &pos, s->library_id))
		goto fail_buf;

	n = snprintf(buf + pos, sizeof(buf) - pos, ",\"library_name\":");
	if (n < 0 || pos + (size_t)n >= sizeof(buf))
		goto fail_buf;
	pos += (size_t)n;
	if (!json_append_escaped(buf, sizeof(buf), &pos, s->library_name))
		goto fail_buf;

	n = snprintf(buf + pos, sizeof(buf) - pos, "}\n");
	if (n < 0 || pos + (size_t)n >= sizeof(buf))
		goto fail_buf;
	pos += (size_t)n;

	int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0600);
	if (fd < 0) {
		set_error(error, error_size, "jellyfin_cannot_save_credentials");
		return false;
	}
	fchmod(fd, 0600);

	size_t written = 0;
	while (written < pos) {
		ssize_t w = write(fd, buf + written, pos - written);
		if (w < 0) {
			if (errno == EINTR)
				continue;
			close(fd);
			unlink(tmp);
			set_error(error, error_size, "jellyfin_cannot_save_credentials");
			return false;
		}
		written += (size_t)w;
	}
	if (fsync(fd) != 0) {
		close(fd);
		unlink(tmp);
		set_error(error, error_size, "jellyfin_cannot_save_credentials");
		return false;
	}
	close(fd);

	if (rename(tmp, path) != 0) {
		unlink(tmp);
		set_error(error, error_size, "jellyfin_cannot_save_credentials");
		return false;
	}
	return true;

fail_buf:
	set_error(error, error_size, "jellyfin_cannot_save_credentials");
	return false;
}

static void delete_credentials_file(void) {
	char path[512];
	if (credentials_path(path, sizeof(path)))
		unlink(path);
}

static void load_credentials_file(void) {
	char path[512];
	if (!credentials_path(path, sizeof(path)))
		return;
	char *raw = read_file_content(path);
	if (!raw)
		return;

	json_doc_t doc;
	if (!json_parse_limited(raw, &doc, 256) || doc.count <= 0 || doc.toks[0].type != JSON_OBJECT) {
		json_free(&doc);
		free(raw);
		return;
	}

	char server_raw[JELLYFIN_SERVER_MAX] = "";
	char server[JELLYFIN_SERVER_MAX] = "";
	char server_name[JELLYFIN_NAME_MAX] = "";
	char user_id[JELLYFIN_ID_MAX] = "";
	char username[JELLYFIN_USER_MAX] = "";
	char token[JELLYFIN_TOKEN_MAX] = "";
	char library_id[JELLYFIN_ID_MAX] = "";
	char library_name[JELLYFIN_TITLE_MAX] = "";

	json_obj_str(&doc, 0, "server", server_raw, sizeof(server_raw));
	json_obj_str(&doc, 0, "server_name", server_name, sizeof(server_name));
	json_str_exact(&doc, json_get(&doc, 0, "user_id"), user_id, sizeof(user_id));
	json_obj_str(&doc, 0, "username", username, sizeof(username));
	json_str_exact(&doc, json_get(&doc, 0, "access_token"), token, sizeof(token));
	json_obj_str(&doc, 0, "library_id", library_id, sizeof(library_id));
	json_obj_str(&doc, 0, "library_name", library_name, sizeof(library_name));

	if (server_raw[0] && user_id[0] && token[0] && valid_id_chars(user_id) && header_safe_token(token) &&
		jellyfin_normalize_server(server_raw, server, sizeof(server), NULL, 0)) {
		pthread_mutex_lock(&session_lock);
		memset(&session, 0, sizeof(session));
		copy_text(session.server, sizeof(session.server), server);
		copy_text(session.server_name, sizeof(session.server_name), server_name);
		copy_text(session.user_id, sizeof(session.user_id), user_id);
		copy_text(session.username, sizeof(session.username), username);
		copy_text(session.access_token, sizeof(session.access_token), token);
		if (valid_id_chars(library_id)) {
			copy_text(session.library_id, sizeof(session.library_id), library_id);
			copy_text(session.library_name, sizeof(session.library_name), library_name);
		}
		session.logged_in = true;
		session.remembered = true;
		pthread_mutex_unlock(&session_lock);
	}

	json_free(&doc);
	free(raw);
}

void jellyfin_init(void) {
	pthread_mutex_lock(&session_lock);
	memset(&session, 0, sizeof(session));
	device_id_cache[0] = '\0';
	last_error_buf[0] = '\0';
	ensure_device_id_locked();
	pthread_mutex_unlock(&session_lock);

	pthread_mutex_lock(&fav_lock);
	fav_count = 0;
	pthread_mutex_unlock(&fav_lock);

	load_credentials_file();
}

// ---------------------------------------------------------------------------
// HTTP helpers
// ---------------------------------------------------------------------------

static bool http_status_error(int status, char *error, size_t error_size) {
	if (status == 401) {
		set_error(error, error_size, "jellyfin_auth_failed");
		return true;
	}
	if (status == 403) {
		set_error(error, error_size, "jellyfin_access_denied");
		return true;
	}
	if (status == 404) {
		set_error(error, error_size, "jellyfin_not_found");
		return true;
	}
	if (status >= 500) {
		set_error(error, error_size, "jellyfin_server_error");
		return true;
	}
	if (status == 0) {
		set_error(error, error_size, "jellyfin_connection_failed");
		return true;
	}
	set_error(error, error_size, "jellyfin_invalid_response");
	return true;
}

typedef struct {
	int status;
	char *body;
	size_t body_len;
} http_response_t;

static void http_response_free(http_response_t *res) {
	if (!res)
		return;
	free(res->body);
	res->body = NULL;
	res->body_len = 0;
	res->status = 0;
}

static bool jf_http_call(const char *method, const char *url, const char *auth_header, const char *body,
						 bool allow_empty, int timeout_ms, size_t max_body, http_response_t *res) {
	memset(res, 0, sizeof(*res));

	http_req_t req;
	memset(&req, 0, sizeof(req));
	req.method = method;
	req.extra_headers = (auth_header && auth_header[0]) ? auth_header : NULL;
	req.body = body;
	req.body_len = body ? strlen(body) : 0;
	req.content_type = body ? "application/json" : NULL;
	req.allow_empty_body = allow_empty;
	req.no_https_downgrade = true;
	req.accept_gzip = true;

	int timeout_secs = (timeout_ms > 0) ? ((timeout_ms + 999) / 1000) : (JELLYFIN_HTTP_TIMEOUT_MS / 1000);
	if (timeout_secs < 1)
		timeout_secs = 1;

	api_enter();
	bool ok = http_request(url, &req, &res->body, &res->body_len, max_body, timeout_secs, &res->status);
	api_leave();
	return ok;
}

static bool jf_request(const char *method, const char *url, const char *auth_header, const char *body,
					   bool allow_empty, int timeout_ms, http_response_t *res, char *error, size_t error_size) {
	bool ok = jf_http_call(method, url, auth_header, body, allow_empty, timeout_ms, JELLYFIN_MAX_BODY, res);
	if (!ok) {
		if (res->status > 0)
			http_status_error(res->status, error, error_size);
		else
			set_error(error, error_size, "jellyfin_connection_failed");
		http_response_free(res);
		return false;
	}
	if (res->status < 200 || res->status >= 300) {
		http_status_error(res->status, error, error_size);
		http_response_free(res);
		return false;
	}
	clear_error(error, error_size);
	return true;
}

// Fetches `/System/Info/Public` (best effort) to get the friendly server name.
static void fetch_public_server_name(const char *server, char *name_out, size_t name_size) {
	if (!name_out || name_size == 0)
		return;
	name_out[0] = '\0';
	if (!server || !server[0])
		return;

	char url[JELLYFIN_URL_MAX];
	if ((size_t)snprintf(url, sizeof(url), "%s/System/Info/Public", server) >= sizeof(url))
		return;

	http_response_t res;
	memset(&res, 0, sizeof(res));
	if (!jf_request("GET", url, NULL, NULL, false, 6000, &res, NULL, 0))
		return;

	json_doc_t doc;
	if (res.body && json_parse_limited(res.body, &doc, 512) && doc.count > 0 && doc.toks[0].type == JSON_OBJECT) {
		json_obj_str(&doc, 0, "ServerName", name_out, name_size);
	}
	json_free(&doc);
	http_response_free(&res);
}

// Parses an AuthenticationResult JSON payload (from AuthenticateByName or
// AuthenticateWithQuickConnect) and activates the session.
static bool commit_auth_response(const char *server, const char *fallback_user, const char *json_body, size_t json_len,
								 bool remember, char *error, size_t error_size) {
	(void)json_len;
	json_doc_t doc;
	if (!json_body || !json_parse_limited(json_body, &doc, 2048) || doc.count <= 0 || doc.toks[0].type != JSON_OBJECT) {
		json_free(&doc);
		set_error(error, error_size, "jellyfin_invalid_response");
		return false;
	}

	char token[JELLYFIN_TOKEN_MAX] = "";
	char user_id[JELLYFIN_ID_MAX] = "";
	char username[JELLYFIN_USER_MAX] = "";

	json_str_exact(&doc, json_get(&doc, 0, "AccessToken"), token, sizeof(token));
	int user_tok = json_get(&doc, 0, "User");
	if (user_tok >= 0 && doc.toks[user_tok].type == JSON_OBJECT) {
		json_str_exact(&doc, json_get(&doc, user_tok, "Id"), user_id, sizeof(user_id));
		json_obj_str(&doc, user_tok, "Name", username, sizeof(username));
	}
	json_free(&doc);

	if (!token[0] || !user_id[0] || !valid_id_chars(user_id) || !header_safe_token(token)) {
		set_error(error, error_size, "jellyfin_invalid_response");
		return false;
	}
	if (!username[0] && fallback_user)
		copy_text(username, sizeof(username), fallback_user);

	char server_name[JELLYFIN_NAME_MAX] = "";
	fetch_public_server_name(server, server_name, sizeof(server_name));

	jellyfin_session_t next;
	memset(&next, 0, sizeof(next));
	copy_text(next.server, sizeof(next.server), server);
	copy_text(next.server_name, sizeof(next.server_name), server_name);
	copy_text(next.user_id, sizeof(next.user_id), user_id);
	copy_text(next.username, sizeof(next.username), username);
	copy_text(next.access_token, sizeof(next.access_token), token);
	next.logged_in = true;
	next.remembered = remember;

	// Preserve selected library if re-authenticating to the same server + user.
	pthread_mutex_lock(&session_lock);
	if (session.logged_in && strcmp(session.server, next.server) == 0 && strcmp(session.user_id, next.user_id) == 0) {
		copy_text(next.library_id, sizeof(next.library_id), session.library_id);
		copy_text(next.library_name, sizeof(next.library_name), session.library_name);
	}
	pthread_mutex_unlock(&session_lock);

	if (remember) {
		if (!write_credentials_file(&next, error, error_size)) {
			// Keep in-memory session active but report inability to persist.
			next.remembered = false;
			pthread_mutex_lock(&session_lock);
			session = next;
			pthread_mutex_unlock(&session_lock);
			return false;
		}
	} else {
		delete_credentials_file();
	}

	pthread_mutex_lock(&session_lock);
	session = next;
	pthread_mutex_unlock(&session_lock);

	pthread_mutex_lock(&fav_lock);
	fav_count = 0;
	pthread_mutex_unlock(&fav_lock);

	clear_error(error, error_size);
	return true;
}

// ---------------------------------------------------------------------------
// Authentication: Username + Password
// ---------------------------------------------------------------------------

bool jellyfin_authenticate(const jellyfin_credentials_t *credentials, bool remember, char *error, size_t error_size) {
	if (!credentials) {
		set_error(error, error_size, "jellyfin_invalid_credentials");
		return false;
	}

	char server[JELLYFIN_SERVER_MAX];
	if (!jellyfin_normalize_server(credentials->server, server, sizeof(server), error, error_size))
		return false;

	if (!credentials->username[0]) {
		set_error(error, error_size, "jellyfin_invalid_credentials");
		return false;
	}

	char url[JELLYFIN_URL_MAX];
	if ((size_t)snprintf(url, sizeof(url), "%s/Users/AuthenticateByName", server) >= sizeof(url)) {
		set_error(error, error_size, "jellyfin_invalid_server");
		return false;
	}

	char dev_id[JELLYFIN_DEVICE_ID_MAX];
	jellyfin_device_id(dev_id, sizeof(dev_id));
	char auth_hdr[512];
	if (!build_auth_header(dev_id, NULL, auth_hdr, sizeof(auth_hdr))) {
		set_error(error, error_size, "jellyfin_invalid_credentials");
		return false;
	}

	char body[1024];
	size_t pos = 0;
	int n = snprintf(body, sizeof(body), "{\"Username\":");
	if (n < 0 || (size_t)n >= sizeof(body)) {
		set_error(error, error_size, "jellyfin_invalid_credentials");
		return false;
	}
	pos = (size_t)n;
	if (!json_append_escaped(body, sizeof(body), &pos, credentials->username)) {
		set_error(error, error_size, "jellyfin_invalid_credentials");
		return false;
	}
	n = snprintf(body + pos, sizeof(body) - pos, ",\"Pw\":");
	if (n < 0 || pos + (size_t)n >= sizeof(body)) {
		set_error(error, error_size, "jellyfin_invalid_credentials");
		return false;
	}
	pos += (size_t)n;
	if (!json_append_escaped(body, sizeof(body), &pos, credentials->password)) {
		set_error(error, error_size, "jellyfin_invalid_credentials");
		return false;
	}
	n = snprintf(body + pos, sizeof(body) - pos, "}");
	if (n < 0 || pos + (size_t)n >= sizeof(body)) {
		set_error(error, error_size, "jellyfin_invalid_credentials");
		return false;
	}

	http_response_t res;
	memset(&res, 0, sizeof(res));
	bool ok = jf_request("POST", url, auth_hdr, body, false, JELLYFIN_HTTP_TIMEOUT_MS, &res, error, error_size);
	// Clear transient password bytes from stack buffer immediately.
	memset(body, 0, sizeof(body));
	if (!ok)
		return false;

	bool saved = commit_auth_response(server, credentials->username, res.body, res.body_len, remember, error, error_size);
	http_response_free(&res);
	return saved;
}

// ---------------------------------------------------------------------------
// Authentication: Quick Connect
// ---------------------------------------------------------------------------

jellyfin_qc_state_t jellyfin_qc_initiate(const char *server_url, jellyfin_qc_t *out, char *error, size_t error_size) {
	if (!out) {
		set_error(error, error_size, "jellyfin_invalid_server");
		return JELLYFIN_QC_ERROR;
	}
	memset(out, 0, sizeof(*out));

	char server[JELLYFIN_SERVER_MAX];
	if (!jellyfin_normalize_server(server_url, server, sizeof(server), error, error_size))
		return JELLYFIN_QC_ERROR;

	char url[JELLYFIN_URL_MAX];
	if ((size_t)snprintf(url, sizeof(url), "%s/QuickConnect/Initiate", server) >= sizeof(url)) {
		set_error(error, error_size, "jellyfin_invalid_server");
		return JELLYFIN_QC_ERROR;
	}

	char dev_id[JELLYFIN_DEVICE_ID_MAX];
	jellyfin_device_id(dev_id, sizeof(dev_id));
	char auth_hdr[512];
	if (!build_auth_header(dev_id, NULL, auth_hdr, sizeof(auth_hdr))) {
		set_error(error, error_size, "jellyfin_connection_failed");
		return JELLYFIN_QC_ERROR;
	}

	http_response_t res;
	bool ok = jf_http_call("POST", url, auth_hdr, NULL, true, JELLYFIN_HTTP_TIMEOUT_MS, 65536, &res);

	if (res.status == 401 || res.status == 403 || res.status == 404) {
		http_response_free(&res);
		set_error(error, error_size, "jellyfin_qc_disabled");
		return JELLYFIN_QC_DISABLED;
	}
	if (!ok || res.status == 0) {
		int st = res.status;
		http_response_free(&res);
		if (st > 0)
			http_status_error(st, error, error_size);
		else
			set_error(error, error_size, "jellyfin_connection_failed");
		return JELLYFIN_QC_ERROR;
	}
	if (res.status < 200 || res.status >= 300 || !res.body) {
		int st = res.status;
		http_response_free(&res);
		http_status_error(st, error, error_size);
		return JELLYFIN_QC_ERROR;
	}

	json_doc_t doc;
	bool parsed = json_parse_limited(res.body, &doc, 256) && doc.count > 0 && doc.toks[0].type == JSON_OBJECT;
	char code[32] = "";
	char secret[128] = "";
	if (parsed) {
		json_obj_str(&doc, 0, "Code", code, sizeof(code));
		json_str_exact(&doc, json_get(&doc, 0, "Secret"), secret, sizeof(secret));
	}
	json_free(&doc);
	http_response_free(&res);

	if (!code[0] || !secret[0]) {
		set_error(error, error_size, "jellyfin_invalid_response");
		return JELLYFIN_QC_ERROR;
	}

	copy_text(out->server, sizeof(out->server), server);
	copy_text(out->code, sizeof(out->code), code);
	copy_text(out->secret, sizeof(out->secret), secret);
	snprintf(out->web_url, sizeof(out->web_url), "%s/web/#/quickconnect", server);
	clear_error(error, error_size);
	return JELLYFIN_QC_PENDING;
}

jellyfin_qc_state_t jellyfin_qc_poll(const jellyfin_qc_t *qc, bool remember, char *error, size_t error_size) {
	if (!qc || !qc->server[0] || !qc->secret[0]) {
		set_error(error, error_size, "jellyfin_invalid_response");
		return JELLYFIN_QC_ERROR;
	}

	char enc_secret[384];
	if (!url_encode(qc->secret, enc_secret, sizeof(enc_secret))) {
		set_error(error, error_size, "jellyfin_invalid_response");
		return JELLYFIN_QC_ERROR;
	}

	char url[JELLYFIN_URL_MAX];
	if ((size_t)snprintf(url, sizeof(url), "%s/QuickConnect/Connect?secret=%s", qc->server, enc_secret) >= sizeof(url)) {
		set_error(error, error_size, "jellyfin_invalid_server");
		return JELLYFIN_QC_ERROR;
	}

	char dev_id[JELLYFIN_DEVICE_ID_MAX];
	jellyfin_device_id(dev_id, sizeof(dev_id));
	char auth_hdr[512];
	if (!build_auth_header(dev_id, NULL, auth_hdr, sizeof(auth_hdr))) {
		set_error(error, error_size, "jellyfin_connection_failed");
		return JELLYFIN_QC_ERROR;
	}

	http_response_t res;
	bool ok = jf_http_call("GET", url, auth_hdr, NULL, false, 10000, 65536, &res);

	if (res.status == 404) {
		http_response_free(&res);
		set_error(error, error_size, "jellyfin_qc_expired");
		return JELLYFIN_QC_EXPIRED;
	}
	if (res.status == 401 || res.status == 403) {
		http_response_free(&res);
		set_error(error, error_size, "jellyfin_qc_disabled");
		return JELLYFIN_QC_DISABLED;
	}
	if (!ok || res.status == 0) {
		int st = res.status;
		http_response_free(&res);
		if (st > 0)
			http_status_error(st, error, error_size);
		else
			set_error(error, error_size, "jellyfin_connection_failed");
		return JELLYFIN_QC_ERROR;
	}
	if (res.status < 200 || res.status >= 300 || !res.body) {
		int st = res.status;
		http_response_free(&res);
		http_status_error(st, error, error_size);
		return JELLYFIN_QC_ERROR;
	}

	json_doc_t doc;
	bool authenticated = false;
	if (json_parse_limited(res.body, &doc, 256) && doc.count > 0 && doc.toks[0].type == JSON_OBJECT) {
		authenticated = json_obj_bool(&doc, 0, "Authenticated", false);
	}
	json_free(&doc);
	http_response_free(&res);

	if (!authenticated) {
		clear_error(error, error_size);
		return JELLYFIN_QC_PENDING;
	}

	// Exchange the approved secret for a real AccessToken.
	if ((size_t)snprintf(url, sizeof(url), "%s/Users/AuthenticateWithQuickConnect", qc->server) >= sizeof(url)) {
		set_error(error, error_size, "jellyfin_invalid_server");
		return JELLYFIN_QC_ERROR;
	}

	char body[384];
	size_t pos = 0;
	int n = snprintf(body, sizeof(body), "{\"Secret\":");
	if (n < 0 || (size_t)n >= sizeof(body))
		return JELLYFIN_QC_ERROR;
	pos = (size_t)n;
	if (!json_append_escaped(body, sizeof(body), &pos, qc->secret))
		return JELLYFIN_QC_ERROR;
	if ((size_t)snprintf(body + pos, sizeof(body) - pos, "}") >= sizeof(body) - pos)
		return JELLYFIN_QC_ERROR;

	memset(&res, 0, sizeof(res));
	if (!jf_request("POST", url, auth_hdr, body, false, JELLYFIN_HTTP_TIMEOUT_MS, &res, error, error_size))
		return JELLYFIN_QC_ERROR;

	bool saved = commit_auth_response(qc->server, "", res.body, res.body_len, remember, error, error_size);
	http_response_free(&res);
	return saved ? JELLYFIN_QC_OK : JELLYFIN_QC_ERROR;
}

// ---------------------------------------------------------------------------
// Account state, library selection, ping, logout, settings
// ---------------------------------------------------------------------------

void jellyfin_account_info(jellyfin_account_info_t *out) {
	if (!out)
		return;
	memset(out, 0, sizeof(*out));
	pthread_mutex_lock(&session_lock);
	copy_text(out->server, sizeof(out->server), session.server);
	copy_text(out->server_name, sizeof(out->server_name), session.server_name);
	copy_text(out->user_id, sizeof(out->user_id), session.user_id);
	copy_text(out->username, sizeof(out->username), session.username);
	copy_text(out->library_id, sizeof(out->library_id), session.library_id);
	copy_text(out->library_name, sizeof(out->library_name), session.library_name);
	out->logged_in = session.logged_in;
	out->remembered = session.remembered;
	pthread_mutex_unlock(&session_lock);
}

bool jellyfin_logged_in(void) {
	pthread_mutex_lock(&session_lock);
	bool ok = session.logged_in && session.server[0] && session.user_id[0] && session.access_token[0];
	pthread_mutex_unlock(&session_lock);
	return ok;
}

bool jellyfin_set_remember(bool remember, char *error, size_t error_size) {
	jellyfin_session_t snap;
	pthread_mutex_lock(&session_lock);
	snap = session;
	pthread_mutex_unlock(&session_lock);

	if (!snap.logged_in) {
		if (!remember)
			delete_credentials_file();
		clear_error(error, error_size);
		return true;
	}
	if (remember) {
		snap.remembered = true;
		if (!write_credentials_file(&snap, error, error_size))
			return false;
	} else {
		delete_credentials_file();
		snap.remembered = false;
	}
	pthread_mutex_lock(&session_lock);
	session.remembered = snap.remembered;
	pthread_mutex_unlock(&session_lock);
	clear_error(error, error_size);
	return true;
}

bool jellyfin_set_library(const char *library_id, const char *library_name) {
	if (library_id && library_id[0] && !valid_id_chars(library_id))
		return false;

	jellyfin_session_t snap;
	pthread_mutex_lock(&session_lock);
	copy_text(session.library_id, sizeof(session.library_id), library_id ? library_id : "");
	copy_text(session.library_name, sizeof(session.library_name),
			  (library_id && library_id[0] && library_name) ? library_name : "");
	snap = session;
	pthread_mutex_unlock(&session_lock);

	if (snap.logged_in && snap.remembered)
		write_credentials_file(&snap, NULL, 0);
	return true;
}

bool jellyfin_forget(void) {
	delete_credentials_file();
	pthread_mutex_lock(&session_lock);
	memset(&session, 0, sizeof(session));
	last_error_buf[0] = '\0';
	pthread_mutex_unlock(&session_lock);

	pthread_mutex_lock(&fav_lock);
	fav_count = 0;
	pthread_mutex_unlock(&fav_lock);
	return true;
}

bool jellyfin_logout_and_forget(void) {
	char server[JELLYFIN_SERVER_MAX];
	char token[JELLYFIN_TOKEN_MAX];
	char dev_id[JELLYFIN_DEVICE_ID_MAX];
	bool logged = false;

	pthread_mutex_lock(&session_lock);
	ensure_device_id_locked();
	copy_text(server, sizeof(server), session.server);
	copy_text(token, sizeof(token), session.access_token);
	copy_text(dev_id, sizeof(dev_id), device_id_cache);
	logged = session.logged_in;
	pthread_mutex_unlock(&session_lock);

	if (logged && server[0] && token[0]) {
		char url[JELLYFIN_URL_MAX];
		char auth_hdr[512];
		if ((size_t)snprintf(url, sizeof(url), "%s/Sessions/Logout", server) < sizeof(url) &&
			build_auth_header(dev_id, token, auth_hdr, sizeof(auth_hdr))) {
			http_response_t res;
			memset(&res, 0, sizeof(res));
			if (jf_request("POST", url, auth_hdr, NULL, true, 5000, &res, NULL, 0))
				http_response_free(&res);
		}
	}
	return jellyfin_forget();
}

static bool snapshot_auth(char *server, size_t server_size, char *user_id, size_t uid_size, char *lib_id,
						  size_t lib_size, char *auth_hdr, size_t hdr_size, char *error, size_t error_size) {
	char token[JELLYFIN_TOKEN_MAX];
	char dev_id[JELLYFIN_DEVICE_ID_MAX];
	bool logged = false;

	pthread_mutex_lock(&session_lock);
	ensure_device_id_locked();
	copy_text(server, server_size, session.server);
	copy_text(user_id, uid_size, session.user_id);
	if (lib_id && lib_size > 0)
		copy_text(lib_id, lib_size, session.library_id);
	copy_text(token, sizeof(token), session.access_token);
	copy_text(dev_id, sizeof(dev_id), device_id_cache);
	logged = session.logged_in;
	pthread_mutex_unlock(&session_lock);

	if (!logged || !server[0] || !user_id[0] || !token[0]) {
		set_error(error, error_size, "jellyfin_not_configured");
		return false;
	}
	if (!build_auth_header(dev_id, token, auth_hdr, hdr_size)) {
		set_error(error, error_size, "jellyfin_invalid_credentials");
		return false;
	}
	return true;
}

bool jellyfin_ping(char *error, size_t error_size) {
	char server[JELLYFIN_SERVER_MAX];
	char user_id[JELLYFIN_ID_MAX];
	char auth_hdr[512];
	if (!snapshot_auth(server, sizeof(server), user_id, sizeof(user_id), NULL, 0, auth_hdr, sizeof(auth_hdr), error,
					   error_size))
		return false;

	char url[JELLYFIN_URL_MAX];
	if ((size_t)snprintf(url, sizeof(url), "%s/Users/Me", server) >= sizeof(url)) {
		set_error(error, error_size, "jellyfin_invalid_server");
		return false;
	}

	http_response_t res;
	memset(&res, 0, sizeof(res));
	if (!jf_request("GET", url, auth_hdr, NULL, false, JELLYFIN_HTTP_TIMEOUT_MS, &res, error, error_size))
		return false;

	json_doc_t doc;
	char username[JELLYFIN_USER_MAX] = "";
	if (res.body && json_parse_limited(res.body, &doc, 1024) && doc.count > 0 && doc.toks[0].type == JSON_OBJECT) {
		json_obj_str(&doc, 0, "Name", username, sizeof(username));
	}
	json_free(&doc);
	http_response_free(&res);

	char server_name[JELLYFIN_NAME_MAX] = "";
	fetch_public_server_name(server, server_name, sizeof(server_name));

	jellyfin_session_t snap;
	pthread_mutex_lock(&session_lock);
	if (username[0])
		copy_text(session.username, sizeof(session.username), username);
	if (server_name[0])
		copy_text(session.server_name, sizeof(session.server_name), server_name);
	snap = session;
	pthread_mutex_unlock(&session_lock);

	if (snap.logged_in && snap.remembered)
		write_credentials_file(&snap, NULL, 0);

	clear_error(error, error_size);
	return true;
}

jellyfin_quality_t jellyfin_get_quality(void) {
	const char *val = config_get(JELLYFIN_SECTION, "quality", "original");
	if (!val)
		return JELLYFIN_QUALITY_ORIGINAL;
	if (strcmp(val, "mp3_320") == 0)
		return JELLYFIN_QUALITY_MP3_320;
	if (strcmp(val, "mp3_192") == 0)
		return JELLYFIN_QUALITY_MP3_192;
	if (strcmp(val, "opus_128") == 0)
		return JELLYFIN_QUALITY_OPUS_128;
	return JELLYFIN_QUALITY_ORIGINAL;
}

void jellyfin_set_quality(jellyfin_quality_t quality) {
	const char *val = "original";
	switch (quality) {
	case JELLYFIN_QUALITY_MP3_320:
		val = "mp3_320";
		break;
	case JELLYFIN_QUALITY_MP3_192:
		val = "mp3_192";
		break;
	case JELLYFIN_QUALITY_OPUS_128:
		val = "opus_128";
		break;
	case JELLYFIN_QUALITY_ORIGINAL:
	default:
		val = "original";
		break;
	}
	config_set(JELLYFIN_SECTION, "quality", val);
	config_save();
}

bool jellyfin_report_playback_enabled(void) {
	return config_get_bool(JELLYFIN_SECTION, "report_playback", true);
}

void jellyfin_set_report_playback(bool enabled) {
	config_set_bool(JELLYFIN_SECTION, "report_playback", enabled);
	config_save();
}

// ---------------------------------------------------------------------------
// Container / format classification & stream/cover URLs
// ---------------------------------------------------------------------------

static void normalize_container(const char *raw, char *out, size_t out_size) {
	if (!out || out_size == 0)
		return;
	out[0] = '\0';
	if (!raw || !raw[0])
		return;

	// If a full path was passed as fallback, grab its extension.
	const char *dot = strrchr(raw, '.');
	const char *src = (strchr(raw, '/') || strchr(raw, '\\')) ? (dot ? dot + 1 : "") : raw;
	if (!src[0])
		return;
	if (*src == '.')
		src++;

	// Strip comma-separated container lists (e.g. "mov,mp4,m4a" -> "m4a" if present, else first).
	if (strchr(src, ',')) {
		if (strstr(src, "m4a"))
			src = "m4a";
		else if (strstr(src, "opus"))
			src = "opus";
		else if (strstr(src, "ogg"))
			src = "ogg";
		else if (strstr(src, "mp4"))
			src = "m4a";
	}

	size_t i = 0;
	while (src[i] && src[i] != ',' && i + 1 < out_size) {
		unsigned char c = (unsigned char)src[i];
		if (!isalnum(c))
			break;
		out[i] = (char)tolower(c);
		i++;
	}
	out[i] = '\0';

	if (strcmp(out, "mp4") == 0 || strcmp(out, "m4b") == 0)
		copy_text(out, out_size, "m4a");
	else if (strcmp(out, "aif") == 0)
		copy_text(out, out_size, "aiff");
	else if (strcmp(out, "oga") == 0)
		copy_text(out, out_size, "ogg");
}

bool jellyfin_container_playable(const char *container) {
	if (!container || !container[0])
		return true; // Unknown container: let stream response Content-Type decide.
	char norm[JELLYFIN_CONTAINER_MAX];
	normalize_container(container, norm, sizeof(norm));
	if (!norm[0])
		return true;
	return strcmp(norm, "flac") == 0 || strcmp(norm, "mp3") == 0 || strcmp(norm, "opus") == 0 ||
		   strcmp(norm, "ogg") == 0 || strcmp(norm, "m4a") == 0 || strcmp(norm, "aac") == 0 ||
		   strcmp(norm, "wav") == 0 || strcmp(norm, "wv") == 0 || strcmp(norm, "ape") == 0 ||
		   strcmp(norm, "aiff") == 0 || strcmp(norm, "dsf") == 0 || strcmp(norm, "dff") == 0;
}

bool jellyfin_stream_url(const char *item_id, jellyfin_quality_t quality, const char *container_hint, char *out,
						 size_t size, char *ext_out, size_t ext_size, bool *will_transcode, char *error,
						 size_t error_size) {
	if (!out || size == 0)
		return false;
	out[0] = '\0';
	if (ext_out && ext_size > 0)
		ext_out[0] = '\0';
	if (will_transcode)
		*will_transcode = false;

	if (!valid_id_chars(item_id)) {
		set_error(error, error_size, "jellyfin_invalid_id");
		return false;
	}

	char server[JELLYFIN_SERVER_MAX];
	char dev_id[JELLYFIN_DEVICE_ID_MAX];
	bool logged = false;
	pthread_mutex_lock(&session_lock);
	ensure_device_id_locked();
	copy_text(server, sizeof(server), session.server);
	copy_text(dev_id, sizeof(dev_id), device_id_cache);
	logged = session.logged_in;
	pthread_mutex_unlock(&session_lock);

	if (!logged || !server[0]) {
		set_error(error, error_size, "jellyfin_not_configured");
		return false;
	}

	char norm[JELLYFIN_CONTAINER_MAX] = "";
	normalize_container(container_hint, norm, sizeof(norm));

	// Fall back to MP3 320 transcode when an original container cannot be
	// decoded natively on TempoTec V1 (for example WMA).
	jellyfin_quality_t effective = quality;
	if (effective == JELLYFIN_QUALITY_ORIGINAL && norm[0] && !jellyfin_container_playable(norm))
		effective = JELLYFIN_QUALITY_MP3_320;

	int n = 0;
	switch (effective) {
	case JELLYFIN_QUALITY_MP3_320:
		n = snprintf(out, size,
					 "%s/Audio/%s/stream.mp3?audioCodec=mp3&audioBitRate=320000&maxStreamingBitrate=320000&deviceId=%s",
					 server, item_id, dev_id);
		if (ext_out && ext_size > 0)
			copy_text(ext_out, ext_size, "mp3");
		if (will_transcode)
			*will_transcode = true;
		break;
	case JELLYFIN_QUALITY_MP3_192:
		n = snprintf(out, size,
					 "%s/Audio/%s/stream.mp3?audioCodec=mp3&audioBitRate=192000&maxStreamingBitrate=192000&deviceId=%s",
					 server, item_id, dev_id);
		if (ext_out && ext_size > 0)
			copy_text(ext_out, ext_size, "mp3");
		if (will_transcode)
			*will_transcode = true;
		break;
	case JELLYFIN_QUALITY_OPUS_128:
		n = snprintf(out, size,
					 "%s/Audio/%s/stream.opus?audioCodec=opus&audioBitRate=128000&maxStreamingBitrate=128000&deviceId=%s",
					 server, item_id, dev_id);
		if (ext_out && ext_size > 0)
			copy_text(ext_out, ext_size, "opus");
		if (will_transcode)
			*will_transcode = true;
		break;
	case JELLYFIN_QUALITY_ORIGINAL:
	default:
		n = snprintf(out, size, "%s/Audio/%s/stream?static=true&deviceId=%s", server, item_id, dev_id);
		if (ext_out && ext_size > 0)
			copy_text(ext_out, ext_size, norm);
		if (will_transcode)
			*will_transcode = false;
		break;
	}

	if (n <= 0 || (size_t)n >= size) {
		out[0] = '\0';
		set_error(error, error_size, "jellyfin_invalid_server");
		return false;
	}
	clear_error(error, error_size);
	return true;
}

bool jellyfin_cover_url(const char *cover_id, char *out, size_t size) {
	if (!out || size == 0)
		return false;
	out[0] = '\0';
	if (!valid_id_chars(cover_id))
		return false;

	char server[JELLYFIN_SERVER_MAX];
	bool logged = false;
	pthread_mutex_lock(&session_lock);
	copy_text(server, sizeof(server), session.server);
	logged = session.logged_in;
	pthread_mutex_unlock(&session_lock);

	if (!logged || !server[0])
		return false;

	int n = snprintf(out, size, "%s/Items/%s/Images/Primary?maxWidth=300&quality=80", server, cover_id);
	return n > 0 && (size_t)n < size;
}

// ---------------------------------------------------------------------------
// Favourites cache & API
// ---------------------------------------------------------------------------

void jellyfin_note_favorite(const char *item_id, bool favorite) {
	if (!valid_id_chars(item_id))
		return;
	pthread_mutex_lock(&fav_lock);
	for (int i = 0; i < fav_count; i++) {
		if (strcmp(fav_slots[i].id, item_id) == 0) {
			fav_slots[i].favorite = favorite;
			pthread_mutex_unlock(&fav_lock);
			return;
		}
	}
	if (fav_count < JELLYFIN_FAV_MAX) {
		copy_text(fav_slots[fav_count].id, sizeof(fav_slots[fav_count].id), item_id);
		fav_slots[fav_count].favorite = favorite;
		fav_count++;
	} else {
		memmove(&fav_slots[0], &fav_slots[1], sizeof(fav_slots[0]) * (JELLYFIN_FAV_MAX - 1));
		copy_text(fav_slots[JELLYFIN_FAV_MAX - 1].id, sizeof(fav_slots[JELLYFIN_FAV_MAX - 1].id), item_id);
		fav_slots[JELLYFIN_FAV_MAX - 1].favorite = favorite;
	}
	pthread_mutex_unlock(&fav_lock);
}

bool jellyfin_is_favorite(const char *item_id) {
	if (!item_id || !item_id[0])
		return false;
	pthread_mutex_lock(&fav_lock);
	for (int i = 0; i < fav_count; i++) {
		if (strcmp(fav_slots[i].id, item_id) == 0) {
			bool fav = fav_slots[i].favorite;
			pthread_mutex_unlock(&fav_lock);
			return fav;
		}
	}
	pthread_mutex_unlock(&fav_lock);
	return false;
}

bool jellyfin_set_favorite(const char *item_id, bool favorite, char *error, size_t error_size) {
	if (!valid_id_chars(item_id)) {
		set_error(error, error_size, "jellyfin_invalid_id");
		return false;
	}

	char server[JELLYFIN_SERVER_MAX];
	char user_id[JELLYFIN_ID_MAX];
	char auth_hdr[512];
	if (!snapshot_auth(server, sizeof(server), user_id, sizeof(user_id), NULL, 0, auth_hdr, sizeof(auth_hdr), error,
					   error_size))
		return false;

	char url[JELLYFIN_URL_MAX];
	if ((size_t)snprintf(url, sizeof(url), "%s/Users/%s/FavoriteItems/%s", server, user_id, item_id) >= sizeof(url)) {
		set_error(error, error_size, "jellyfin_invalid_server");
		return false;
	}

	http_response_t res;
	memset(&res, 0, sizeof(res));
	const char *method = favorite ? "POST" : "DELETE";
	if (!jf_request(method, url, auth_hdr, NULL, true, JELLYFIN_HTTP_TIMEOUT_MS, &res, error, error_size))
		return false;
	http_response_free(&res);

	jellyfin_note_favorite(item_id, favorite);
	clear_error(error, error_size);
	return true;
}

// ---------------------------------------------------------------------------
// JSON item parser (64-bit RunTimeTicks safe on 32-bit MIPS)
// ---------------------------------------------------------------------------

static bool extract_first_string_array(const json_doc_t *doc, int obj_idx, const char *key, char *out,
									   size_t out_size) {
	int arr = json_get(doc, obj_idx, key);
	if (arr < 0 || doc->toks[arr].type != JSON_ARRAY)
		return false;
	int first = json_at(doc, arr, 0);
	if (first < 0)
		return false;
	return json_str(doc, first, out, out_size);
}

static bool parse_jellyfin_item(const json_doc_t *doc, int idx, jellyfin_item_kind_t default_kind,
								jellyfin_entry_t *out) {
	if (!doc || idx < 0 || doc->toks[idx].type != JSON_OBJECT || !out)
		return false;
	memset(out, 0, sizeof(*out));

	if (!json_str_exact(doc, json_get(doc, idx, "Id"), out->id, sizeof(out->id)) || !valid_id_chars(out->id))
		return false;
	if (!json_obj_str(doc, idx, "Name", out->title, sizeof(out->title)) || !out->title[0])
		copy_text(out->title, sizeof(out->title), out->id);

	char type_str[64] = "";
	json_obj_str(doc, idx, "Type", type_str, sizeof(type_str));
	if (strcmp(type_str, "Audio") == 0)
		out->kind = JELLYFIN_ITEM_TRACK;
	else if (strcmp(type_str, "MusicAlbum") == 0)
		out->kind = JELLYFIN_ITEM_ALBUM;
	else if (strcmp(type_str, "MusicArtist") == 0)
		out->kind = JELLYFIN_ITEM_ARTIST;
	else if (strcmp(type_str, "Playlist") == 0)
		out->kind = JELLYFIN_ITEM_PLAYLIST;
	else if (strcmp(type_str, "MusicGenre") == 0 || strcmp(type_str, "Genre") == 0)
		out->kind = JELLYFIN_ITEM_GENRE;
	else if (strcmp(type_str, "Folder") == 0 || strcmp(type_str, "CollectionFolder") == 0 ||
			 strcmp(type_str, "UserView") == 0)
		out->kind = JELLYFIN_ITEM_FOLDER;
	else
		out->kind = default_kind;

	// Artist: prefer Artists[0], else AlbumArtist, else ArtistItems[0].Name.
	if (!extract_first_string_array(doc, idx, "Artists", out->artist, sizeof(out->artist)) || !out->artist[0]) {
		if (!json_obj_str(doc, idx, "AlbumArtist", out->artist, sizeof(out->artist)) || !out->artist[0]) {
			int aitems = json_get(doc, idx, "ArtistItems");
			if (aitems >= 0 && doc->toks[aitems].type == JSON_ARRAY) {
				int first = json_at(doc, aitems, 0);
				if (first >= 0 && doc->toks[first].type == JSON_OBJECT)
					json_obj_str(doc, first, "Name", out->artist, sizeof(out->artist));
			}
		}
	}

	json_obj_str(doc, idx, "Album", out->album, sizeof(out->album));
	json_obj_str(doc, idx, "AlbumId", out->album_id, sizeof(out->album_id));
	if (out->album_id[0] && !valid_id_chars(out->album_id))
		out->album_id[0] = '\0';

	// Cover art ID: item's own Primary tag if present, else AlbumId when album
	// primary tag exists.
	bool has_primary = false;
	int img_tags = json_get(doc, idx, "ImageTags");
	if (img_tags >= 0 && doc->toks[img_tags].type == JSON_OBJECT) {
		char tag[64] = "";
		if (json_obj_str(doc, img_tags, "Primary", tag, sizeof(tag)) && tag[0])
			has_primary = true;
	}
	if (has_primary) {
		copy_text(out->cover_id, sizeof(out->cover_id), out->id);
	} else if (out->album_id[0]) {
		char album_tag[64] = "";
		if (json_obj_str(doc, idx, "AlbumPrimaryImageTag", album_tag, sizeof(album_tag)) && album_tag[0])
			copy_text(out->cover_id, sizeof(out->cover_id), out->album_id);
	}

	// Container / extension hint: Container field first, else Path extension.
	char container_raw[64] = "";
	if (json_obj_str(doc, idx, "Container", container_raw, sizeof(container_raw)) && container_raw[0]) {
		normalize_container(container_raw, out->container, sizeof(out->container));
	}
	if (!out->container[0]) {
		char item_path[512] = "";
		if (json_obj_str(doc, idx, "Path", item_path, sizeof(item_path)) && item_path[0])
			normalize_container(item_path, out->container, sizeof(out->container));
	}

	// 64-bit RunTimeTicks -> seconds.
	long long ticks = json_obj_llong(doc, idx, "RunTimeTicks", 0);
	if (ticks > 0) {
		long long secs = (ticks + JELLYFIN_TICKS_PER_SEC / 2) / JELLYFIN_TICKS_PER_SEC;
		if (secs > 86400LL)
			secs = 86400LL;
		out->duration = (int)secs;
	}

	long num = json_obj_long(doc, idx, "IndexNumber", 0);
	if (num > 0)
		out->track_number = (int)num;
	num = json_obj_long(doc, idx, "ParentIndexNumber", 0);
	if (num > 0)
		out->disc_number = (int)num;
	num = json_obj_long(doc, idx, "ProductionYear", 0);
	if (num > 0)
		out->year = (int)num;
	num = json_obj_long(doc, idx, "ChildCount", 0);
	if (num <= 0)
		num = json_obj_long(doc, idx, "SongCount", 0);
	if (num <= 0)
		num = json_obj_long(doc, idx, "AlbumCount", 0);
	if (num > 0)
		out->child_count = (int)num;

	int udata = json_get(doc, idx, "UserData");
	if (udata >= 0 && doc->toks[udata].type == JSON_OBJECT) {
		int fav_tok = json_get(doc, udata, "IsFavorite");
		if (fav_tok >= 0 && doc->toks[fav_tok].type == JSON_BOOL) {
			bool fav = json_bool(doc, fav_tok, false);
			out->is_favorite = fav;
			if (out->kind == JELLYFIN_ITEM_TRACK)
				jellyfin_note_favorite(out->id, fav);
		}
	}

	return true;
}

static int fetch_items_page(const char *url, const char *auth_hdr, jellyfin_item_kind_t default_kind,
							bool filter_music_views, int offset, jellyfin_entry_t *entries, int limit, bool *more,
							char *error, size_t error_size) {
	if (more)
		*more = false;
	if (!entries || limit <= 0)
		return 0;

	http_response_t res;
	memset(&res, 0, sizeof(res));
	if (!jf_request("GET", url, auth_hdr, NULL, false, JELLYFIN_HTTP_TIMEOUT_MS, &res, error, error_size))
		return -1;

	json_doc_t doc;
	if (!res.body || !json_parse_limited(res.body, &doc, JELLYFIN_MAX_JSON_TOKENS) || doc.count <= 0 ||
		doc.toks[0].type != JSON_OBJECT) {
		json_free(&doc);
		http_response_free(&res);
		set_error(error, error_size, "jellyfin_invalid_response");
		return -1;
	}

	int items_arr = json_get(&doc, 0, "Items");
	if (items_arr < 0 || doc.toks[items_arr].type != JSON_ARRAY) {
		json_free(&doc);
		http_response_free(&res);
		set_error(error, error_size, "jellyfin_invalid_response");
		return -1;
	}

	long total_records = json_obj_long(&doc, 0, "TotalRecordCount", -1);

	int arr_len = json_len(&doc, items_arr);
	int out_count = 0;
	for (int i = 0; i < arr_len && out_count < limit; i++) {
		int elem = json_at(&doc, items_arr, i);
		if (elem < 0 || doc.toks[elem].type != JSON_OBJECT)
			continue;

		if (filter_music_views) {
			char ctype[32] = "";
			json_obj_str(&doc, elem, "CollectionType", ctype, sizeof(ctype));
			if (strcasecmp(ctype, "music") != 0)
				continue;
		}

		if (parse_jellyfin_item(&doc, elem, default_kind, &entries[out_count]))
			out_count++;
	}

	if (more && !filter_music_views) {
		if (total_records >= 0)
			*more = ((long)offset + (long)arr_len) < total_records;
		else
			*more = (arr_len >= limit);
	}

	json_free(&doc);
	http_response_free(&res);
	clear_error(error, error_size);
	return out_count;
}

// ---------------------------------------------------------------------------
// Browsing & Search
// ---------------------------------------------------------------------------

int jellyfin_browse(jellyfin_browse_t browse, const char *id_or_query, int offset, jellyfin_entry_t *entries, int limit,
					bool *more, char *error, size_t error_size) {
	if (more)
		*more = false;
	if (!entries || limit <= 0)
		return 0;
	if (offset < 0)
		offset = 0;
	if (limit > JELLYFIN_PAGE_SIZE)
		limit = JELLYFIN_PAGE_SIZE;

	char server[JELLYFIN_SERVER_MAX];
	char user_id[JELLYFIN_ID_MAX];
	char lib_id[JELLYFIN_ID_MAX];
	char auth_hdr[512];
	if (!snapshot_auth(server, sizeof(server), user_id, sizeof(user_id), lib_id, sizeof(lib_id), auth_hdr,
					   sizeof(auth_hdr), error, error_size))
		return -1;

	char parent_param[JELLYFIN_ID_MAX + 16] = "";
	if (lib_id[0])
		snprintf(parent_param, sizeof(parent_param), "&ParentId=%s", lib_id);

	char url[JELLYFIN_URL_MAX];
	int n = 0;

	switch (browse) {
	case JELLYFIN_BROWSE_LIBRARIES:
		n = snprintf(url, sizeof(url), "%s/Users/%s/Views", server, user_id);
		if (n <= 0 || (size_t)n >= sizeof(url))
			goto bad_url;
		return fetch_items_page(url, auth_hdr, JELLYFIN_ITEM_FOLDER, true, 0, entries, limit, more, error, error_size);

	case JELLYFIN_BROWSE_FOLDERS:
		if (lib_id[0]) {
			// Active music library chosen: show its root folders/items directly.
			n = snprintf(url, sizeof(url),
						 "%s/Users/%s/Items?ParentId=%s&Recursive=false&StartIndex=%d&Limit=%d"
						 "&Fields=Path&SortBy=IsFolder,SortName&SortOrder=Ascending",
						 server, user_id, lib_id, offset, limit);
			if (n <= 0 || (size_t)n >= sizeof(url))
				goto bad_url;
			return fetch_items_page(url, auth_hdr, JELLYFIN_ITEM_FOLDER, false, offset, entries, limit, more, error,
									error_size);
		}
		// No specific library pinned: inspect `/Users/{uid}/Views` for music
		// libraries. If there is exactly one, step directly into its root
		// folder; if there are several, list them as top-level folders.
		n = snprintf(url, sizeof(url), "%s/Users/%s/Views", server, user_id);
		if (n <= 0 || (size_t)n >= sizeof(url))
			goto bad_url;
		{
			int libs = fetch_items_page(url, auth_hdr, JELLYFIN_ITEM_FOLDER, true, 0, entries, limit, NULL, error,
										error_size);
			if (libs < 0)
				return -1;
			if (libs == 1) {
				char only_lib[JELLYFIN_ID_MAX];
				copy_text(only_lib, sizeof(only_lib), entries[0].id);
				n = snprintf(url, sizeof(url),
							 "%s/Users/%s/Items?ParentId=%s&Recursive=false&StartIndex=%d&Limit=%d"
							 "&Fields=Path&SortBy=IsFolder,SortName&SortOrder=Ascending",
							 server, user_id, only_lib, offset, limit);
				if (n <= 0 || (size_t)n >= sizeof(url))
					goto bad_url;
				return fetch_items_page(url, auth_hdr, JELLYFIN_ITEM_FOLDER, false, offset, entries, limit, more, error,
										error_size);
			}
			return libs;
		}

	case JELLYFIN_BROWSE_FOLDER:
		if (!valid_id_chars(id_or_query)) {
			set_error(error, error_size, "jellyfin_invalid_id");
			return -1;
		}
		n = snprintf(url, sizeof(url),
					 "%s/Users/%s/Items?ParentId=%s&Recursive=false&StartIndex=%d&Limit=%d"
					 "&Fields=Path&SortBy=IsFolder,ParentIndexNumber,IndexNumber,SortName&SortOrder=Ascending",
					 server, user_id, id_or_query, offset, limit);
		if (n <= 0 || (size_t)n >= sizeof(url))
			goto bad_url;
		return fetch_items_page(url, auth_hdr, JELLYFIN_ITEM_FOLDER, false, offset, entries, limit, more, error,
								error_size);

	case JELLYFIN_BROWSE_PLAYLISTS:
		// Playlists live in Jellyfin's virtual Playlists collection, so never
		// pass a music library ParentId here.
		n = snprintf(url, sizeof(url),
					 "%s/Users/%s/Items?IncludeItemTypes=Playlist&Recursive=true&StartIndex=%d&Limit=%d"
					 "&SortBy=SortName&SortOrder=Ascending",
					 server, user_id, offset, limit);
		if (n <= 0 || (size_t)n >= sizeof(url))
			goto bad_url;
		return fetch_items_page(url, auth_hdr, JELLYFIN_ITEM_PLAYLIST, false, offset, entries, limit, more, error,
								error_size);

	case JELLYFIN_BROWSE_PLAYLIST_ITEMS:
		if (!valid_id_chars(id_or_query)) {
			set_error(error, error_size, "jellyfin_invalid_id");
			return -1;
		}
		n = snprintf(url, sizeof(url),
					 "%s/Playlists/%s/Items?UserId=%s&MediaTypes=Audio&StartIndex=%d&Limit=%d&Fields=Path", server,
					 id_or_query, user_id, offset, limit);
		if (n <= 0 || (size_t)n >= sizeof(url))
			goto bad_url;
		return fetch_items_page(url, auth_hdr, JELLYFIN_ITEM_TRACK, false, offset, entries, limit, more, error,
								error_size);

	case JELLYFIN_BROWSE_ARTISTS:
		n = snprintf(url, sizeof(url),
					 "%s/Artists/AlbumArtists?UserId=%s%s&StartIndex=%d&Limit=%d&SortBy=SortName&SortOrder=Ascending",
					 server, user_id, parent_param, offset, limit);
		if (n <= 0 || (size_t)n >= sizeof(url))
			goto bad_url;
		return fetch_items_page(url, auth_hdr, JELLYFIN_ITEM_ARTIST, false, offset, entries, limit, more, error,
								error_size);

	case JELLYFIN_BROWSE_ARTIST_ALBUMS:
		if (!valid_id_chars(id_or_query)) {
			set_error(error, error_size, "jellyfin_invalid_id");
			return -1;
		}
		n = snprintf(url, sizeof(url),
					 "%s/Users/%s/Items?ArtistIds=%s&IncludeItemTypes=MusicAlbum&Recursive=true"
					 "&StartIndex=%d&Limit=%d&SortBy=ProductionYear,SortName&SortOrder=Descending",
					 server, user_id, id_or_query, offset, limit);
		if (n <= 0 || (size_t)n >= sizeof(url))
			goto bad_url;
		return fetch_items_page(url, auth_hdr, JELLYFIN_ITEM_ALBUM, false, offset, entries, limit, more, error,
								error_size);

	case JELLYFIN_BROWSE_ALBUMS:
		n = snprintf(url, sizeof(url),
					 "%s/Users/%s/Items?IncludeItemTypes=MusicAlbum&Recursive=true%s"
					 "&StartIndex=%d&Limit=%d&SortBy=SortName&SortOrder=Ascending",
					 server, user_id, parent_param, offset, limit);
		if (n <= 0 || (size_t)n >= sizeof(url))
			goto bad_url;
		return fetch_items_page(url, auth_hdr, JELLYFIN_ITEM_ALBUM, false, offset, entries, limit, more, error,
								error_size);

	case JELLYFIN_BROWSE_RECENT:
		n = snprintf(url, sizeof(url),
					 "%s/Users/%s/Items?IncludeItemTypes=MusicAlbum&Recursive=true%s"
					 "&StartIndex=%d&Limit=%d&SortBy=DateCreated&SortOrder=Descending",
					 server, user_id, parent_param, offset, limit);
		if (n <= 0 || (size_t)n >= sizeof(url))
			goto bad_url;
		return fetch_items_page(url, auth_hdr, JELLYFIN_ITEM_ALBUM, false, offset, entries, limit, more, error,
								error_size);

	case JELLYFIN_BROWSE_ALBUM_TRACKS:
		if (!valid_id_chars(id_or_query)) {
			set_error(error, error_size, "jellyfin_invalid_id");
			return -1;
		}
		n = snprintf(url, sizeof(url),
					 "%s/Users/%s/Items?ParentId=%s&IncludeItemTypes=Audio&Recursive=true"
					 "&StartIndex=%d&Limit=%d&Fields=Path&SortBy=ParentIndexNumber,IndexNumber,SortName",
					 server, user_id, id_or_query, offset, limit);
		if (n <= 0 || (size_t)n >= sizeof(url))
			goto bad_url;
		return fetch_items_page(url, auth_hdr, JELLYFIN_ITEM_TRACK, false, offset, entries, limit, more, error,
								error_size);

	case JELLYFIN_BROWSE_GENRES:
		n = snprintf(url, sizeof(url),
					 "%s/MusicGenres?UserId=%s%s&StartIndex=%d&Limit=%d&SortBy=SortName&SortOrder=Ascending", server,
					 user_id, parent_param, offset, limit);
		if (n <= 0 || (size_t)n >= sizeof(url))
			goto bad_url;
		return fetch_items_page(url, auth_hdr, JELLYFIN_ITEM_GENRE, false, offset, entries, limit, more, error,
								error_size);

	case JELLYFIN_BROWSE_GENRE_ALBUMS:
		if (!valid_id_chars(id_or_query)) {
			set_error(error, error_size, "jellyfin_invalid_id");
			return -1;
		}
		n = snprintf(url, sizeof(url),
					 "%s/Users/%s/Items?GenreIds=%s&IncludeItemTypes=MusicAlbum&Recursive=true%s"
					 "&StartIndex=%d&Limit=%d&SortBy=SortName&SortOrder=Ascending",
					 server, user_id, id_or_query, parent_param, offset, limit);
		if (n <= 0 || (size_t)n >= sizeof(url))
			goto bad_url;
		return fetch_items_page(url, auth_hdr, JELLYFIN_ITEM_ALBUM, false, offset, entries, limit, more, error,
								error_size);

	case JELLYFIN_BROWSE_FAVORITES:
		n = snprintf(url, sizeof(url),
					 "%s/Users/%s/Items?Filters=IsFavorite&IncludeItemTypes=Audio&Recursive=true%s"
					 "&StartIndex=%d&Limit=%d&Fields=Path&SortBy=SortName&SortOrder=Ascending",
					 server, user_id, parent_param, offset, limit);
		if (n <= 0 || (size_t)n >= sizeof(url))
			goto bad_url;
		return fetch_items_page(url, auth_hdr, JELLYFIN_ITEM_TRACK, false, offset, entries, limit, more, error,
								error_size);

	case JELLYFIN_BROWSE_SEARCH: {
		if (!id_or_query || !id_or_query[0]) {
			set_error(error, error_size, "jellyfin_invalid_query");
			return -1;
		}
		char enc_query[512];
		if (!url_encode(id_or_query, enc_query, sizeof(enc_query))) {
			set_error(error, error_size, "jellyfin_invalid_query");
			return -1;
		}
		n = snprintf(url, sizeof(url),
					 "%s/Users/%s/Items?SearchTerm=%s&IncludeItemTypes=Audio,MusicAlbum,MusicArtist,Playlist"
					 "&Recursive=true%s&StartIndex=%d&Limit=%d&Fields=Path",
					 server, user_id, enc_query, parent_param, offset, limit);
		if (n <= 0 || (size_t)n >= sizeof(url))
			goto bad_url;
		return fetch_items_page(url, auth_hdr, JELLYFIN_ITEM_TRACK, false, offset, entries, limit, more, error,
								error_size);
	}
	}

bad_url:
	set_error(error, error_size, "jellyfin_invalid_server");
	return -1;
}

// ---------------------------------------------------------------------------
// Playback reporting (`/Sessions/Playing`, `/Progress`, `/Stopped`)
// ---------------------------------------------------------------------------

static bool send_playback_report(jf_report_kind_t kind, const char *item_id, const char *play_session_id,
								 long long position_ticks, bool is_paused, bool transcoded) {
	if (!jellyfin_report_playback_enabled())
		return true;
	if (!valid_id_chars(item_id))
		return false;

	char server[JELLYFIN_SERVER_MAX];
	char user_id[JELLYFIN_ID_MAX];
	char auth_hdr[512];
	if (!snapshot_auth(server, sizeof(server), user_id, sizeof(user_id), NULL, 0, auth_hdr, sizeof(auth_hdr), NULL, 0))
		return false;

	const char *endpoint = "/Sessions/Playing";
	if (kind == JF_REP_PROGRESS)
		endpoint = "/Sessions/Playing/Progress";
	else if (kind == JF_REP_STOPPED)
		endpoint = "/Sessions/Playing/Stopped";

	char url[JELLYFIN_URL_MAX];
	if ((size_t)snprintf(url, sizeof(url), "%s%s", server, endpoint) >= sizeof(url))
		return false;

	if (position_ticks < 0)
		position_ticks = 0;

	char body[512];
	size_t pos = 0;
	int n = snprintf(body, sizeof(body), "{\"ItemId\":");
	if (n < 0 || (size_t)n >= sizeof(body))
		return false;
	pos = (size_t)n;
	if (!json_append_escaped(body, sizeof(body), &pos, item_id))
		return false;

	n = snprintf(body + pos, sizeof(body) - pos, ",\"MediaSourceId\":");
	if (n < 0 || pos + (size_t)n >= sizeof(body))
		return false;
	pos += (size_t)n;
	if (!json_append_escaped(body, sizeof(body), &pos, item_id))
		return false;

	if (play_session_id && play_session_id[0]) {
		n = snprintf(body + pos, sizeof(body) - pos, ",\"PlaySessionId\":");
		if (n < 0 || pos + (size_t)n >= sizeof(body))
			return false;
		pos += (size_t)n;
		if (!json_append_escaped(body, sizeof(body), &pos, play_session_id))
			return false;
	}

	if (kind == JF_REP_STOPPED) {
		n = snprintf(body + pos, sizeof(body) - pos, ",\"PositionTicks\":%lld}", position_ticks);
	} else {
		n = snprintf(body + pos, sizeof(body) - pos,
					 ",\"PositionTicks\":%lld,\"IsPaused\":%s,\"CanSeek\":true,\"PlayMethod\":\"%s\"}",
					 position_ticks, is_paused ? "true" : "false", transcoded ? "Transcode" : "DirectStream");
	}
	if (n < 0 || pos + (size_t)n >= sizeof(body))
		return false;

	http_response_t res;
	memset(&res, 0, sizeof(res));
	bool ok = jf_request("POST", url, auth_hdr, body, true, JELLYFIN_REPORT_TIMEOUT_MS, &res, NULL, 0);
	if (ok)
		http_response_free(&res);
	return ok;
}

bool jellyfin_report_playback_start(const char *item_id, const char *play_session_id, long long position_ticks,
									bool transcoded) {
	return send_playback_report(JF_REP_START, item_id, play_session_id, position_ticks, false, transcoded);
}

bool jellyfin_report_playback_progress(const char *item_id, const char *play_session_id, long long position_ticks,
									   bool is_paused, bool transcoded) {
	return send_playback_report(JF_REP_PROGRESS, item_id, play_session_id, position_ticks, is_paused, transcoded);
}

bool jellyfin_report_playback_stopped(const char *item_id, const char *play_session_id, long long position_ticks) {
	return send_playback_report(JF_REP_STOPPED, item_id, play_session_id, position_ticks, false, false);
}

static void *report_worker(void *arg) {
	(void)arg;
	thread_be_background("jf-report");

	for (;;) {
		jf_report_event_t ev;
		pthread_mutex_lock(&report_lock);
		if (report_count == 0) {
			struct timespec ts;
			deadline_in_ms(&ts, 5000);
			pthread_cond_timedwait(&report_cond, &report_lock, &ts);
			if (report_count == 0) {
				report_thread_running = false;
				pthread_mutex_unlock(&report_lock);
				return NULL;
			}
		}
		ev = report_queue[report_head];
		report_head = (report_head + 1) % JELLYFIN_REPORT_QUEUE_MAX;
		report_count--;
		pthread_mutex_unlock(&report_lock);

		send_playback_report(ev.kind, ev.item_id, ev.play_session_id, ev.position_ticks, ev.is_paused, ev.transcoded);
	}
}

static void enqueue_report_async(jf_report_kind_t kind, const char *item_id, const char *play_session_id,
								 long long position_ticks, bool is_paused, bool transcoded) {
	if (!jellyfin_report_playback_enabled() || !jellyfin_logged_in() || !valid_id_chars(item_id))
		return;

	pthread_mutex_lock(&report_lock);
	// Coalesce consecutive progress pings so the queue never backs up on slow Wi-Fi.
	if (kind == JF_REP_PROGRESS && report_count > 0) {
		int last = (report_tail + JELLYFIN_REPORT_QUEUE_MAX - 1) % JELLYFIN_REPORT_QUEUE_MAX;
		if (report_queue[last].kind == JF_REP_PROGRESS && strcmp(report_queue[last].item_id, item_id) == 0) {
			report_queue[last].position_ticks = position_ticks;
			report_queue[last].is_paused = is_paused;
			report_queue[last].transcoded = transcoded;
			pthread_cond_signal(&report_cond);
			pthread_mutex_unlock(&report_lock);
			return;
		}
	}
	if (report_count >= JELLYFIN_REPORT_QUEUE_MAX) {
		// Drop the oldest entry to keep Start/Stopped transitions fresh.
		report_head = (report_head + 1) % JELLYFIN_REPORT_QUEUE_MAX;
		report_count--;
	}
	jf_report_event_t *slot = &report_queue[report_tail];
	memset(slot, 0, sizeof(*slot));
	slot->kind = kind;
	copy_text(slot->item_id, sizeof(slot->item_id), item_id);
	copy_text(slot->play_session_id, sizeof(slot->play_session_id), play_session_id ? play_session_id : "");
	slot->position_ticks = position_ticks;
	slot->is_paused = is_paused;
	slot->transcoded = transcoded;
	report_tail = (report_tail + 1) % JELLYFIN_REPORT_QUEUE_MAX;
	report_count++;

	if (!report_thread_running) {
		pthread_attr_t attr;
		pthread_t tid;
		pthread_attr_init(&attr);
		pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
		pthread_attr_setstacksize(&attr, JELLYFIN_WORKER_STACK);
		if (pthread_create(&tid, &attr, report_worker, NULL) == 0)
			report_thread_running = true;
		pthread_attr_destroy(&attr);
	}
	pthread_cond_signal(&report_cond);
	pthread_mutex_unlock(&report_lock);
}

void jellyfin_report_start_async(const char *item_id, const char *play_session_id, long long position_ticks,
								 bool transcoded) {
	enqueue_report_async(JF_REP_START, item_id, play_session_id, position_ticks, false, transcoded);
}

void jellyfin_report_progress_async(const char *item_id, const char *play_session_id, long long position_ticks,
									bool is_paused, bool transcoded) {
	enqueue_report_async(JF_REP_PROGRESS, item_id, play_session_id, position_ticks, is_paused, transcoded);
}

void jellyfin_report_stopped_async(const char *item_id, const char *play_session_id, long long position_ticks) {
	enqueue_report_async(JF_REP_STOPPED, item_id, play_session_id, position_ticks, false, false);
}
