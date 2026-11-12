#define _GNU_SOURCE

#include "soundcloud.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include "src/system/core/config.h"
#include "src/system/core/json.h"
#include "src/system/core/lang.h"
#include "src/system/net/http.h"
#include "src/system/streaming/streamkeys.h"

#define SC_API_BASE_DEFAULT "https://api-v2.soundcloud.com"
#define SC_WEB_BASE_DEFAULT "https://soundcloud.com"

#define SC_TIMEOUT_SECS 18
#define SC_API_BODY_LIMIT (4 * 1024 * 1024)
#define SC_WEB_HTML_LIMIT (1024 * 1024)
#define SC_WEB_JS_LIMIT (3 * 1024 * 1024)
#define SC_MAX_SCRIPTS 20
#define SC_LIKED_MIRROR_CAP 512
#define SC_CREDENTIALS_FILE "soundcloud-credentials.json"

static pthread_mutex_t state_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t creds_io_lock = PTHREAD_MUTEX_INITIALIZER;

static char api_base[256];
static char web_base[256];
static char credentials_file[600];

static soundcloud_credentials_t creds;
static bool creds_remembered = true;
static bool creds_from_file;

static char discovered_client_id[SOUNDCLOUD_CLIENT_ID_MAX];
static long long account_user_id;
static char account_display_name[SOUNDCLOUD_NAME_MAX];
static char account_permalink[SOUNDCLOUD_USER_MAX];
static char account_tier[32];

static int quality_setting = SOUNDCLOUD_QUALITY_STANDARD;
static unsigned api_inflight;

static long long liked_ids[SC_LIKED_MIRROR_CAP];
static int liked_count;

static __thread char last_error[256];

const char *soundcloud_last_error(void) { return last_error; }

static void set_error(const char *msg) {
	snprintf(last_error, sizeof(last_error), "%s", msg ? msg : "");
}

static void clear_error(void) { last_error[0] = '\0'; }

static void wipe(void *p, size_t n) {
	volatile unsigned char *v = p;
	while (n--) {
		*v++ = 0;
	}
}

static void begin_api(void) {
	pthread_mutex_lock(&state_lock);
	api_inflight++;
	pthread_mutex_unlock(&state_lock);
}

static void end_api(void) {
	pthread_mutex_lock(&state_lock);
	if (api_inflight > 0) {
		api_inflight--;
	}
	pthread_mutex_unlock(&state_lock);
}

bool soundcloud_api_busy(void) {
	pthread_mutex_lock(&state_lock);
	bool busy = api_inflight > 0;
	pthread_mutex_unlock(&state_lock);
	return busy;
}

// ---------------------------------------------------------------------------
// Liked-track session mirror
// ---------------------------------------------------------------------------

static void liked_note_locked(long long id, bool liked) {
	if (id <= 0) {
		return;
	}
	for (int i = 0; i < liked_count; i++) {
		if (liked_ids[i] == id) {
			if (!liked) {
				memmove(&liked_ids[i], &liked_ids[i + 1], (size_t)(liked_count - i - 1) * sizeof(liked_ids[0]));
				liked_count--;
			}
			return;
		}
	}
	if (!liked) {
		return;
	}
	if (liked_count == SC_LIKED_MIRROR_CAP) {
		memmove(&liked_ids[0], &liked_ids[1], (SC_LIKED_MIRROR_CAP - 1) * sizeof(liked_ids[0]));
		liked_count--;
	}
	liked_ids[liked_count++] = id;
}

void soundcloud_note_liked(long long track_id, bool liked) {
	pthread_mutex_lock(&state_lock);
	liked_note_locked(track_id, liked);
	pthread_mutex_unlock(&state_lock);
}

bool soundcloud_is_liked(long long track_id) {
	if (track_id <= 0) {
		return false;
	}
	pthread_mutex_lock(&state_lock);
	bool found = false;
	for (int i = 0; i < liked_count; i++) {
		if (liked_ids[i] == track_id) {
			found = true;
			break;
		}
	}
	pthread_mutex_unlock(&state_lock);
	return found;
}

// ---------------------------------------------------------------------------
// Input normalization & credential persistence
// ---------------------------------------------------------------------------

static void trim_inplace(char *s) {
	if (!s) {
		return;
	}
	char *start = s;
	while (*start && isspace((unsigned char)*start)) {
		start++;
	}
	if (start != s) {
		memmove(s, start, strlen(start) + 1);
	}
	size_t len = strlen(s);
	while (len > 0 && isspace((unsigned char)s[len - 1])) {
		s[--len] = '\0';
	}
}

static bool valid_client_id(const char *s) {
	if (!s) {
		return false;
	}
	size_t len = strlen(s);
	if (len < 8 || len >= SOUNDCLOUD_CLIENT_ID_MAX) {
		return false;
	}
	for (size_t i = 0; i < len; i++) {
		unsigned char c = (unsigned char)s[i];
		if (!isalnum(c) && c != '_' && c != '-') {
			return false;
		}
	}
	return true;
}

static bool is_on_soundcloud_shortlink(const char *s) {
	return s && strncasecmp(s, "https://on.soundcloud.com/", 26) == 0 && s[26] != '\0';
}

// Accepts:
//   "dj-example"
//   "@dj-example"
//   "soundcloud.com/dj-example"
//   "m.soundcloud.com/dj-example" / "mobile.soundcloud.com/dj-example"
//   "https://soundcloud.com/dj-example/likes?si=...&utm=..." -> "dj-example"
//   "on.soundcloud.com/AbCdEf" -> "https://on.soundcloud.com/AbCdEf"
static bool normalize_username(char *u) {
	trim_inplace(u);
	if (!u[0]) {
		return true;
	}
	const char *p = u;
	bool has_scheme = false;
	if (strncasecmp(p, "https://", 8) == 0) {
		p += 8;
		has_scheme = true;
	} else if (strncasecmp(p, "http://", 7) == 0) {
		p += 7;
		has_scheme = true;
	}

	const char *slash = strchr(p, '/');
	if (slash) {
		size_t hlen = (size_t)(slash - p);
		if (hlen == 17 && strncasecmp(p, "on.soundcloud.com", 17) == 0) {
			const char *sp = slash + 1;
			while (*sp == '/') sp++;
			char slug[128];
			size_t sn = 0;
			while (*sp && *sp != '/' && *sp != '?' && *sp != '#') {
				unsigned char c = (unsigned char)*sp++;
				if (c <= 32 || c == 127) {
					return false;
				}
				if (sn + 1 >= sizeof(slug)) {
					return false;
				}
				slug[sn++] = (char)c;
			}
			slug[sn] = '\0';
			if (!slug[0]) {
				return false;
			}
			int w = snprintf(u, SOUNDCLOUD_USER_MAX, "https://on.soundcloud.com/%s", slug);
			return w > 0 && (size_t)w < SOUNDCLOUD_USER_MAX;
		}
		bool is_sc_host = (hlen == 14 && strncasecmp(p, "soundcloud.com", 14) == 0) ||
						  (hlen > 15 && strncasecmp(p + hlen - 15, ".soundcloud.com", 15) == 0);
		if (!is_sc_host) {
			return false;
		}
		p = slash + 1;
	} else if (has_scheme) {
		return false;
	}

	while (*p == '/' || *p == '@') {
		p++;
	}
	char clean[SOUNDCLOUD_USER_MAX];
	size_t n = 0;
	while (*p && *p != '/' && *p != '?' && *p != '#') {
		unsigned char c = (unsigned char)*p++;
		if (c < 32 || c == 127 || isspace(c)) {
			return false;
		}
		if (n + 1 >= sizeof(clean)) {
			return false;
		}
		clean[n++] = (char)c;
	}
	clean[n] = '\0';
	if (!clean[0] || strcasecmp(clean, "soundcloud.com") == 0) {
		return false;
	}
	snprintf(u, SOUNDCLOUD_USER_MAX, "%s", clean);
	return true;
}

// Strips accidental "OAuth ", "Bearer ", or quotes pasted from browser DevTools.
static bool normalize_token(char *tok) {
	trim_inplace(tok);
	if (!tok[0]) {
		return true;
	}
	size_t len = strlen(tok);
	if (len >= 2 && ((tok[0] == '"' && tok[len - 1] == '"') || (tok[0] == '\'' && tok[len - 1] == '\''))) {
		memmove(tok, tok + 1, len - 2);
		tok[len - 2] = '\0';
		trim_inplace(tok);
	}
	len = strlen(tok);
	if (len >= 6 && strncasecmp(tok, "OAuth ", 6) == 0) {
		memmove(tok, tok + 6, len - 6 + 1);
		trim_inplace(tok);
	} else if (len >= 7 && strncasecmp(tok, "Bearer ", 7) == 0) {
		memmove(tok, tok + 7, len - 7 + 1);
		trim_inplace(tok);
	}
	for (const unsigned char *p = (const unsigned char *)tok; *p; p++) {
		if (*p <= 32 || *p == 127 || *p == '\r' || *p == '\n') {
			return false;
		}
	}
	return tok[0] != '\0';
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

static void ensure_credentials_path(void) {
	if (credentials_file[0]) {
		return;
	}
	const char *cfg_path = config_path();
	if (cfg_path && cfg_path[0]) {
		const char *slash = strrchr(cfg_path, '/');
		if (slash) {
			snprintf(credentials_file, sizeof(credentials_file), "%.*s/%s", (int)(slash - cfg_path), cfg_path,
					 SC_CREDENTIALS_FILE);
		} else {
			snprintf(credentials_file, sizeof(credentials_file), "%s", SC_CREDENTIALS_FILE);
		}
	}
}

static bool save_credentials_file(const soundcloud_credentials_t *c, long long uid, const char *disp,
								  const char *perm, const char *tier) {
	ensure_credentials_path();
	if (!credentials_file[0]) {
		fprintf(stderr, "soundcloud: cannot save credentials (no config path)\n");
		return false;
	}
	char tmp[sizeof(credentials_file) + 8];
	if (snprintf(tmp, sizeof(tmp), "%s.tmp", credentials_file) >= (int)sizeof(tmp)) {
		return false;
	}
	int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW | O_CLOEXEC, 0600);
	if (fd < 0) {
		fprintf(stderr, "soundcloud: cannot open %s (%s)\n", tmp, strerror(errno));
		return false;
	}
	(void)fchmod(fd, 0600);
	FILE *f = fdopen(fd, "w");
	if (!f) {
		close(fd);
		unlink(tmp);
		return false;
	}
	bool ok = fputs("{\"username\":", f) >= 0 && write_json_string(f, c->username) &&
			  fputs(",\"oauth_token\":", f) >= 0 && write_json_string(f, c->oauth_token) &&
			  fputs(",\"client_id\":", f) >= 0 && write_json_string(f, c->client_id) &&
			  fprintf(f, ",\"user_id\":%lld", uid) > 0 &&
			  fputs(",\"display_name\":", f) >= 0 && write_json_string(f, disp ? disp : "") &&
			  fputs(",\"permalink\":", f) >= 0 && write_json_string(f, perm ? perm : "") &&
			  fputs(",\"tier\":", f) >= 0 && write_json_string(f, tier ? tier : "") &&
			  fputs("}\n", f) >= 0;
	if (fflush(f) != 0 || fsync(fd) != 0) {
		ok = false;
	}
	if (fclose(f) != 0) {
		ok = false;
	}
	if (ok) {
		ok = rename(tmp, credentials_file) == 0;
	}
	if (!ok) {
		fprintf(stderr, "soundcloud: failed to write %s (%s)\n", credentials_file, strerror(errno));
		unlink(tmp);
	}
	return ok;
}

static bool remove_credentials_file(void) {
	ensure_credentials_path();
	if (!credentials_file[0]) {
		return true;
	}
	char tmp[sizeof(credentials_file) + 8];
	if (snprintf(tmp, sizeof(tmp), "%s.tmp", credentials_file) < (int)sizeof(tmp)) {
		(void)unlink(tmp);
	}
	return unlink(credentials_file) == 0 || errno == ENOENT;
}

static bool load_credentials_file(void) {
	if (!credentials_file[0]) {
		return false;
	}
	int fd = open(credentials_file, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
	if (fd < 0) {
		return false;
	}
	struct stat st;
	if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size <= 0 || st.st_size > 16384) {
		close(fd);
		return false;
	}
	char *buf = malloc((size_t)st.st_size + 1);
	if (!buf) {
		close(fd);
		return false;
	}
	ssize_t got = read(fd, buf, (size_t)st.st_size);
	close(fd);
	if (got != st.st_size) {
		wipe(buf, (size_t)st.st_size);
		free(buf);
		return false;
	}
	buf[got] = '\0';

	json_doc_t doc = {0};
	bool ok = false;
	if (json_parse_limited(buf, &doc, 256)) {
		int root = json_root(&doc);
		soundcloud_credentials_t loaded = {0};
		int u_tok = json_get(&doc, root, "username");
		int t_tok = json_get(&doc, root, "oauth_token");
		int c_tok = json_get(&doc, root, "client_id");
		bool valid = true;
		if (u_tok >= 0 && !json_str_exact(&doc, u_tok, loaded.username, sizeof(loaded.username))) {
			valid = false;
		}
		if (t_tok >= 0 && !json_str_exact(&doc, t_tok, loaded.oauth_token, sizeof(loaded.oauth_token))) {
			valid = false;
		}
		if (c_tok >= 0 && !json_str_exact(&doc, c_tok, loaded.client_id, sizeof(loaded.client_id))) {
			valid = false;
		}
		if (valid && (loaded.username[0] || loaded.oauth_token[0] || loaded.client_id[0])) {
			creds = loaded;
			creds_remembered = true;
			creds_from_file = true;
			account_user_id = json_obj_llong(&doc, root, "user_id", 0);
			json_obj_str(&doc, root, "display_name", account_display_name, sizeof(account_display_name));
			json_obj_str(&doc, root, "permalink", account_permalink, sizeof(account_permalink));
			json_obj_str(&doc, root, "tier", account_tier, sizeof(account_tier));
			ok = true;
		}
		wipe(&loaded, sizeof(loaded));
		json_free(&doc);
	}
	wipe(buf, (size_t)st.st_size);
	free(buf);
	return ok;
}

static void apply_streamkeys_fallback_locked(void) {
	if (creds_from_file) {
		return;
	}
	const char *sk_cid = streamkeys_soundcloud_client_id();
	const char *sk_tok = streamkeys_soundcloud_oauth_token();
	const char *sk_usr = streamkeys_soundcloud_username();
	if (sk_cid && sk_cid[0] && !creds.client_id[0]) {
		snprintf(creds.client_id, sizeof(creds.client_id), "%s", sk_cid);
	}
	if (sk_tok && sk_tok[0] && !creds.oauth_token[0]) {
		snprintf(creds.oauth_token, sizeof(creds.oauth_token), "%s", sk_tok);
		(void)normalize_token(creds.oauth_token);
	}
	if (sk_usr && sk_usr[0] && !creds.username[0]) {
		snprintf(creds.username, sizeof(creds.username), "%s", sk_usr);
		(void)normalize_username(creds.username);
	}
}

void soundcloud_init(void) {
	pthread_mutex_lock(&creds_io_lock);
	pthread_mutex_lock(&state_lock);

	const char *cfg_api = config_get("soundcloud", "api_base", SC_API_BASE_DEFAULT);
	const char *cfg_web = config_get("soundcloud", "web_base", SC_WEB_BASE_DEFAULT);
	snprintf(api_base, sizeof(api_base), "%s", cfg_api && cfg_api[0] ? cfg_api : SC_API_BASE_DEFAULT);
	snprintf(web_base, sizeof(web_base), "%s", cfg_web && cfg_web[0] ? cfg_web : SC_WEB_BASE_DEFAULT);
	size_t alen = strlen(api_base);
	while (alen > 0 && api_base[alen - 1] == '/') {
		api_base[--alen] = '\0';
	}
	size_t wlen = strlen(web_base);
	while (wlen > 0 && web_base[wlen - 1] == '/') {
		web_base[--wlen] = '\0';
	}

	int q = config_get_int("soundcloud", "quality", SOUNDCLOUD_QUALITY_STANDARD);
	if (q < SOUNDCLOUD_QUALITY_LOW || q > SOUNDCLOUD_QUALITY_HIGH) {
		q = SOUNDCLOUD_QUALITY_STANDARD;
	}
	quality_setting = q;

	const char *cached_cid = config_get("soundcloud", "cached_client_id", "");
	if (cached_cid && valid_client_id(cached_cid)) {
		snprintf(discovered_client_id, sizeof(discovered_client_id), "%s", cached_cid);
	} else {
		discovered_client_id[0] = '\0';
	}

	credentials_file[0] = '\0';
	const char *cfg_path = config_path();
	if (cfg_path && cfg_path[0]) {
		const char *slash = strrchr(cfg_path, '/');
		if (slash) {
			snprintf(credentials_file, sizeof(credentials_file), "%.*s/%s", (int)(slash - cfg_path), cfg_path,
					 SC_CREDENTIALS_FILE);
		} else {
			snprintf(credentials_file, sizeof(credentials_file), "%s", SC_CREDENTIALS_FILE);
		}
	}

	wipe(&creds, sizeof(creds));
	creds_remembered = true;
	creds_from_file = false;
	account_user_id = 0;
	account_display_name[0] = '\0';
	account_permalink[0] = '\0';
	account_tier[0] = '\0';
	liked_count = 0;

	(void)load_credentials_file();
	apply_streamkeys_fallback_locked();

	pthread_mutex_unlock(&state_lock);
	pthread_mutex_unlock(&creds_io_lock);
}

void soundcloud_reload_streamkeys(void) {
	pthread_mutex_lock(&state_lock);
	apply_streamkeys_fallback_locked();
	pthread_mutex_unlock(&state_lock);
}

int soundcloud_get_quality(void) {
	pthread_mutex_lock(&state_lock);
	int q = quality_setting;
	pthread_mutex_unlock(&state_lock);
	return q;
}

void soundcloud_set_quality(int quality) {
	if (quality < SOUNDCLOUD_QUALITY_LOW || quality > SOUNDCLOUD_QUALITY_HIGH) {
		return;
	}
	pthread_mutex_lock(&state_lock);
	quality_setting = quality;
	pthread_mutex_unlock(&state_lock);
	config_set_int("soundcloud", "quality", quality);
	config_save();
}

const char *soundcloud_expected_mime(void) {
	int q = soundcloud_get_quality();
	if (q == SOUNDCLOUD_QUALITY_LOW) {
		return "audio/ogg";
	}
	return "audio/mpeg";
}

void soundcloud_credentials(soundcloud_credentials_t *out, bool *remembered) {
	pthread_mutex_lock(&state_lock);
	if (out) {
		*out = creds;
	}
	if (remembered) {
		*remembered = creds_remembered;
	}
	pthread_mutex_unlock(&state_lock);
}

void soundcloud_account_info(soundcloud_account_info_t *out) {
	if (!out) {
		return;
	}
	memset(out, 0, sizeof(*out));
	pthread_mutex_lock(&state_lock);
	out->user_id = account_user_id;
	snprintf(out->display_name, sizeof(out->display_name), "%.*s",
			 (int)(sizeof(out->display_name) - 1),
			 account_display_name[0] ? account_display_name : creds.username);
	snprintf(out->permalink, sizeof(out->permalink), "%s",
			 account_permalink[0] ? account_permalink : creds.username);
	snprintf(out->tier, sizeof(out->tier), "%s", account_tier);
	out->has_token = creds.oauth_token[0] != '\0';
	out->remembered = creds_remembered;
	pthread_mutex_unlock(&state_lock);
}

bool soundcloud_has_account(void) {
	pthread_mutex_lock(&state_lock);
	bool ok = account_user_id > 0 || creds.username[0] != '\0' || creds.oauth_token[0] != '\0';
	pthread_mutex_unlock(&state_lock);
	return ok;
}

bool soundcloud_can_like(void) {
	pthread_mutex_lock(&state_lock);
	bool ok = creds.oauth_token[0] != '\0';
	pthread_mutex_unlock(&state_lock);
	return ok;
}

bool soundcloud_forget(void) {
	pthread_mutex_lock(&creds_io_lock);
	pthread_mutex_lock(&state_lock);
	wipe(&creds, sizeof(creds));
	creds_remembered = true;
	creds_from_file = false;
	account_user_id = 0;
	account_display_name[0] = '\0';
	account_permalink[0] = '\0';
	account_tier[0] = '\0';
	liked_count = 0;
	pthread_mutex_unlock(&state_lock);
	bool removed = remove_credentials_file();
	pthread_mutex_unlock(&creds_io_lock);
	return removed;
}

// ---------------------------------------------------------------------------
// URL & Header helpers
// ---------------------------------------------------------------------------

static void extract_authority(const char *url, char *auth, size_t size) {
	auth[0] = '\0';
	if (!url) {
		return;
	}
	const char *p = strstr(url, "://");
	if (!p) {
		return;
	}
	p += 3;
	size_t n = 0;
	while (*p && *p != '/' && *p != '?' && *p != '#') {
		if (n + 1 < size) {
			auth[n++] = (char)tolower((unsigned char)*p);
		}
		p++;
	}
	auth[n] = '\0';
}

static void extract_host(const char *url, char *host, size_t size) {
	extract_authority(url, host, size);
	char *colon = strchr(host, ':');
	if (colon) {
		*colon = '\0';
	}
}

static bool url_targets_soundcloud_api(const char *url, const char *configured_api_base) {
	char auth[256], api_auth[256], host[256];
	extract_authority(url, auth, sizeof(auth));
	if (!auth[0]) {
		return false;
	}
	extract_authority(configured_api_base, api_auth, sizeof(api_auth));
	if (api_auth[0] && strcmp(auth, api_auth) == 0) {
		return true;
	}
	extract_host(url, host, sizeof(host));
	if (strcmp(host, "api-v2.soundcloud.com") == 0 || strcmp(host, "api.soundcloud.com") == 0 ||
		strcmp(host, "api-mobi.soundcloud.com") == 0) {
		return true;
	}
	size_t len = strlen(host);
	const char *suffix = ".soundcloud.com";
	size_t slen = strlen(suffix);
	return len > slen && strcmp(host + len - slen, suffix) == 0;
}

void soundcloud_headers_for_url(const char *for_url, char *out, size_t out_size) {
	if (!out || out_size == 0) {
		return;
	}
	char tok[SOUNDCLOUD_TOKEN_MAX];
	char base[sizeof(api_base)];
	pthread_mutex_lock(&state_lock);
	memcpy(tok, creds.oauth_token, sizeof(tok));
	snprintf(base, sizeof(base), "%s", api_base);
	pthread_mutex_unlock(&state_lock);

	bool send_auth = tok[0] != '\0' && url_targets_soundcloud_api(for_url, base);
	if (send_auth) {
		snprintf(out, out_size,
				 "Origin: https://soundcloud.com\r\n"
				 "Referer: https://soundcloud.com/\r\n"
				 "Authorization: OAuth %s\r\n",
				 tok);
	} else {
		snprintf(out, out_size,
				 "Origin: https://soundcloud.com\r\n"
				 "Referer: https://soundcloud.com/\r\n");
	}
	wipe(tok, sizeof(tok));
}

void soundcloud_artwork_url(const char *raw, int size_px, char *out, size_t out_size) {
	if (!out || out_size == 0) {
		return;
	}
	out[0] = '\0';
	if (!raw || !raw[0]) {
		return;
	}
	const char *repl = size_px >= 400 ? "-t500x500." : "-t300x300.";
	const char *tag = strstr(raw, "-large.");
	if (!tag) {
		tag = strstr(raw, "-badge.");
	}
	if (tag) {
		size_t prefix = (size_t)(tag - raw);
		const char *after = strchr(tag + 1, '.');
		if (after) {
			snprintf(out, out_size, "%.*s%s%s", (int)prefix, raw, repl, after + 1);
			return;
		}
	}
	snprintf(out, out_size, "%s", raw);
}

// ---------------------------------------------------------------------------
// Automatic client_id discovery from SoundCloud's web assets
// ---------------------------------------------------------------------------

static void resolve_asset_url(const char *base, const char *src, char *out, size_t size) {
	if (strncasecmp(src, "http://", 7) == 0 || strncasecmp(src, "https://", 8) == 0) {
		snprintf(out, size, "%s", src);
	} else if (src[0] == '/' && src[1] == '/') {
		snprintf(out, size, "https:%s", src);
	} else if (src[0] == '/') {
		snprintf(out, size, "%s%s", base, src);
	} else {
		snprintf(out, size, "%s/%s", base, src);
	}
}

static bool extract_client_id_from_js(const char *js, size_t len, char *out, size_t out_size) {
	static const char *const patterns[] = {
		"client_id:\"",
		"client_id:'",
		"\"client_id\":\"",
		"client_id=",
	};
	for (size_t p = 0; p < sizeof(patterns) / sizeof(patterns[0]); p++) {
		const char *pat = patterns[p];
		size_t plen = strlen(pat);
		char quote = pat[plen - 1];
		bool unquoted = (quote == '=');

		const char *cur = js;
		const char *end = js + len;
		while (cur + plen + 16 <= end) {
			const char *hit = strstr(cur, pat);
			if (!hit || hit + plen >= end) {
				break;
			}
			const char *val = hit + plen;
			size_t vlen = 0;
			while (val + vlen < end && vlen < SOUNDCLOUD_CLIENT_ID_MAX) {
				unsigned char c = (unsigned char)val[vlen];
				if (isalnum(c) || c == '_' || c == '-') {
					vlen++;
				} else {
					break;
				}
			}
			// SoundCloud web client_ids are 32 alphanumeric chars; accept 20..48
			// when explicitly delimited so minor format updates still match.
			bool delim_ok = unquoted ? (vlen == 32) : (val + vlen < end && val[vlen] == quote && vlen >= 20 && vlen <= 48);
			if (delim_ok && vlen < out_size) {
				memcpy(out, val, vlen);
				out[vlen] = '\0';
				return true;
			}
			cur = hit + plen;
		}
	}
	return false;
}

static bool discover_client_id(char *out, size_t out_size) {
	char wbase[sizeof(web_base)];
	pthread_mutex_lock(&state_lock);
	snprintf(wbase, sizeof(wbase), "%s", web_base);
	pthread_mutex_unlock(&state_lock);

	bool secure_only = strncasecmp(wbase, "https://", 8) == 0;
	http_req_t req = {
		.extra_headers = "Referer: https://soundcloud.com/\r\n",
		.no_https_downgrade = secure_only,
		.accept_gzip = true,
		.user_agent = SOUNDCLOUD_USER_AGENT,
	};

	char *html = NULL;
	size_t html_len = 0;
	int status = 0;
	if (!http_request(wbase, &req, &html, &html_len, SC_WEB_HTML_LIMIT, SC_TIMEOUT_SECS, &status) || !html) {
		free(html);
		const char *why = http_last_error();
		set_error(why && why[0] ? why : "soundcloud_client_id_failed");
		return false;
	}

	// Sometimes inline config in HTML carries client_id directly.
	if (extract_client_id_from_js(html, html_len, out, out_size)) {
		free(html);
		goto found;
	}

	char scripts[SC_MAX_SCRIPTS][768];
	int script_count = 0;
	const char *scan = html;
	while ((scan = strcasestr(scan, "<script")) != NULL) {
		const char *gt = strchr(scan, '>');
		if (!gt) {
			break;
		}
		const char *src = strcasestr(scan, "src=");
		if (src && src < gt) {
			src += 4;
			while (src < gt && isspace((unsigned char)*src)) {
				src++;
			}
			char quote = (*src == '"' || *src == '\'') ? *src++ : '\0';
			const char *send = src;
			while (send < gt && ((quote && *send != quote) || (!quote && !isspace((unsigned char)*send)))) {
				send++;
			}
			size_t slen = (size_t)(send - src);
			if (slen > 3 && slen < 450) {
				char raw_src[460];
				memcpy(raw_src, src, slen);
				raw_src[slen] = '\0';
				if (strstr(raw_src, ".js") != NULL) {
					if (script_count == SC_MAX_SCRIPTS) {
						memmove(&scripts[0], &scripts[1], (SC_MAX_SCRIPTS - 1) * sizeof(scripts[0]));
						script_count--;
					}
					resolve_asset_url(wbase, raw_src, scripts[script_count++], sizeof(scripts[0]));
				}
			}
		}
		scan = gt + 1;
	}
	free(html);

	// SoundCloud places its app configuration in the final webpack bundles.
	for (int i = script_count - 1; i >= 0; i--) {
		char *js = NULL;
		size_t js_len = 0;
		int js_status = 0;
		bool js_secure = strncasecmp(scripts[i], "https://", 8) == 0;
		http_req_t js_req = {
			.extra_headers = "Origin: https://soundcloud.com\r\nReferer: https://soundcloud.com/\r\n",
			.no_https_downgrade = js_secure,
			.accept_gzip = true,
			.user_agent = SOUNDCLOUD_USER_AGENT,
		};
		if (!http_request(scripts[i], &js_req, &js, &js_len, SC_WEB_JS_LIMIT, SC_TIMEOUT_SECS, &js_status) || !js) {
			free(js);
			continue;
		}
		bool ok = extract_client_id_from_js(js, js_len, out, out_size);
		free(js);
		if (ok) {
			goto found;
		}
	}

	set_error("soundcloud_client_id_failed");
	return false;

found:
	pthread_mutex_lock(&state_lock);
	snprintf(discovered_client_id, sizeof(discovered_client_id), "%s", out);
	pthread_mutex_unlock(&state_lock);
	config_set("soundcloud", "cached_client_id", out);
	config_save();
	return true;
}

static bool get_active_client_id(char *out, size_t size, bool *is_custom) {
	pthread_mutex_lock(&state_lock);
	if (creds.client_id[0]) {
		snprintf(out, size, "%s", creds.client_id);
		if (is_custom) {
			*is_custom = true;
		}
		pthread_mutex_unlock(&state_lock);
		return true;
	}
	if (is_custom) {
		*is_custom = false;
	}
	if (discovered_client_id[0]) {
		snprintf(out, size, "%s", discovered_client_id);
		pthread_mutex_unlock(&state_lock);
		return true;
	}
	pthread_mutex_unlock(&state_lock);
	return discover_client_id(out, size);
}

// ---------------------------------------------------------------------------
// Authenticated / public API v2 request execution with automatic client_id rotation
// ---------------------------------------------------------------------------

static bool append_client_id(const char *base_url_or_path, const char *cid, char *out, size_t size) {
	char abase[sizeof(api_base)];
	pthread_mutex_lock(&state_lock);
	snprintf(abase, sizeof(abase), "%s", api_base);
	pthread_mutex_unlock(&state_lock);

	char full[SOUNDCLOUD_URL_MAX];
	if (strncasecmp(base_url_or_path, "http://", 7) == 0 || strncasecmp(base_url_or_path, "https://", 8) == 0) {
		// Strip any stale client_id=... query parameter from cursor next_href URLs.
		snprintf(full, sizeof(full), "%s", base_url_or_path);
		char *existing = strstr(full, "client_id=");
		if (existing && (existing == full || existing[-1] == '?' || existing[-1] == '&')) {
			char *after = strchr(existing, '&');
			if (after) {
				memmove(existing, after + 1, strlen(after + 1) + 1);
			} else if (existing > full && (existing[-1] == '?' || existing[-1] == '&')) {
				existing[-1] = '\0';
			}
		}
	} else {
		snprintf(full, sizeof(full), "%s%s%s", abase, base_url_or_path[0] == '/' ? "" : "/", base_url_or_path);
	}

	char enc_cid[192];
	http_url_encode(cid, enc_cid, sizeof(enc_cid));
	const char *sep = strchr(full, '?') ? "&" : "?";
	int n = snprintf(out, size, "%s%sclient_id=%s", full, sep, enc_cid);
	return n > 0 && (size_t)n < size;
}

static bool api_request_json(const char *method, const char *path_or_url, const char *override_token,
							 const char *override_cid, json_doc_t *doc_out, char **text_out, int *status_out) {
	if (doc_out) {
		memset(doc_out, 0, sizeof(*doc_out));
	}
	if (text_out) {
		*text_out = NULL;
	}
	if (status_out) {
		*status_out = 0;
	}

	char cid[SOUNDCLOUD_CLIENT_ID_MAX];
	bool custom_cid = false;
	if (override_cid && override_cid[0]) {
		snprintf(cid, sizeof(cid), "%s", override_cid);
		custom_cid = true;
	} else if (!get_active_client_id(cid, sizeof(cid), &custom_cid)) {
		return false;
	}

	char token[SOUNDCLOUD_TOKEN_MAX];
	if (override_token) {
		snprintf(token, sizeof(token), "%s", override_token);
	} else {
		pthread_mutex_lock(&state_lock);
		memcpy(token, creds.oauth_token, sizeof(token));
		pthread_mutex_unlock(&state_lock);
	}

	for (int attempt = 0; attempt < 2; attempt++) {
		char url[SOUNDCLOUD_URL_MAX + 256];
		if (!append_client_id(path_or_url, cid, url, sizeof(url))) {
			wipe(token, sizeof(token));
			set_error("soundcloud_invalid_url");
			return false;
		}

		char headers[SOUNDCLOUD_TOKEN_MAX + 192];
		char abase[sizeof(api_base)];
		pthread_mutex_lock(&state_lock);
		snprintf(abase, sizeof(abase), "%s", api_base);
		pthread_mutex_unlock(&state_lock);

		if (token[0] && url_targets_soundcloud_api(url, abase)) {
			snprintf(headers, sizeof(headers),
					 "Origin: https://soundcloud.com\r\n"
					 "Referer: https://soundcloud.com/\r\n"
					 "Authorization: OAuth %s\r\n",
					 token);
		} else {
			snprintf(headers, sizeof(headers),
					 "Origin: https://soundcloud.com\r\n"
					 "Referer: https://soundcloud.com/\r\n");
		}

		bool is_write = method && (strcmp(method, "PUT") == 0 || strcmp(method, "POST") == 0 || strcmp(method, "DELETE") == 0);
		bool secure_only = strncasecmp(url, "https://", 8) == 0;
		http_req_t req = {
			.method = method,
			.extra_headers = headers,
			.body = (method && strcmp(method, "PUT") == 0) ? "" : NULL,
			.content_type = (method && strcmp(method, "PUT") == 0) ? "application/json" : NULL,
			.want_error_body = true,
			.allow_empty_body = true,
			.no_https_downgrade = secure_only,
			.accept_gzip = !is_write,
			.user_agent = SOUNDCLOUD_USER_AGENT,
		};

		char *body = NULL;
		size_t body_len = 0;
		int status = 0;
		bool transport_ok = http_request(url, &req, &body, &body_len, SC_API_BODY_LIMIT, SC_TIMEOUT_SECS, &status);
		wipe(headers, sizeof(headers));
		if (status_out) {
			*status_out = status;
		}

		if (!transport_ok) {
			free(body);
			wipe(token, sizeof(token));
			const char *why = http_last_error();
			set_error(why && why[0] ? why : "soundcloud_connection_failed");
			return false;
		}

		// Automatic rotation: if our cached web client_id was invalidated, fetch
		// a fresh bundle client_id once and retry transparently.
		if ((status == 401 || status == 403) && attempt == 0 && !custom_cid) {
			free(body);
			char fresh[SOUNDCLOUD_CLIENT_ID_MAX] = "";
			if (discover_client_id(fresh, sizeof(fresh)) && strcmp(fresh, cid) != 0) {
				snprintf(cid, sizeof(cid), "%s", fresh);
				continue;
			}
			wipe(token, sizeof(token));
			set_error(status == 401 && token[0] ? "soundcloud_auth_failed" : "soundcloud_client_id_failed");
			return false;
		}

		if (status < 200 || status >= 300) {
			free(body);
			bool had_tok = token[0] != '\0';
			wipe(token, sizeof(token));
			if (status == 401) {
				set_error(had_tok ? "soundcloud_auth_failed" : "soundcloud_client_id_failed");
			} else if (status == 403) {
				set_error("soundcloud_stream_refused");
			} else if (status == 404) {
				set_error("soundcloud_not_found");
			} else if (status == 429) {
				set_error("soundcloud_rate_limited");
			} else {
				set_error("soundcloud_connection_failed");
			}
			return false;
		}

		wipe(token, sizeof(token));
		if (!doc_out) {
			free(body);
			return true;
		}
		if (!body || !body[0] || !json_parse(body, doc_out)) {
			free(body);
			set_error("soundcloud_invalid_response");
			return false;
		}
		if (text_out) {
			*text_out = body;
		} else {
			json_free(doc_out);
			free(body);
		}
		return true;
	}

	wipe(token, sizeof(token));
	set_error("soundcloud_connection_failed");
	return false;
}

// ---------------------------------------------------------------------------
// Parsing SoundCloud v2 user / track / playlist objects
// ---------------------------------------------------------------------------

static void parse_user_tier(const json_doc_t *doc, int user_obj, char *tier, size_t size) {
	char prod[64] = "";
	int sub = json_path(doc, user_obj, "consumer_subscription.product");
	if (sub >= 0) {
		json_obj_str(doc, sub, "id", prod, sizeof(prod));
	}
	if (!prod[0]) {
		int subs = json_get(doc, user_obj, "subscriptions");
		int first = json_at(doc, subs, 0);
		int p2 = json_get(doc, first, "product");
		if (p2 >= 0) {
			json_obj_str(doc, p2, "id", prod, sizeof(prod));
		}
	}
	if (strstr(prod, "high") || strstr(prod, "go-plus") || strstr(prod, "plus")) {
		snprintf(tier, size, "Go+");
	} else if (strstr(prod, "mid") || strstr(prod, "consumer-go") || strcmp(prod, "go") == 0) {
		snprintf(tier, size, "Go");
	} else {
		snprintf(tier, size, "Free");
	}
}

static bool preset_is_abr(const char *preset) {
	if (!preset || !preset[0]) {
		return false;
	}
	return strncmp(preset, "abr", 3) == 0 && (preset[3] == '\0' || preset[3] == '_');
}

static bool resolve_user_from_web_html(const char *handle, long long *uid_out, char *disp_out, size_t disp_size,
									   char *perm_out, size_t perm_size, char *tier_out, size_t tier_size) {
	if (!handle || !handle[0] || is_on_soundcloud_shortlink(handle)) {
		return false;
	}
	char wbase[sizeof(web_base)];
	pthread_mutex_lock(&state_lock);
	snprintf(wbase, sizeof(wbase), "%s", web_base);
	pthread_mutex_unlock(&state_lock);

	char enc_h[512];
	http_url_encode(handle, enc_h, sizeof(enc_h));
	char page_url[800];
	snprintf(page_url, sizeof(page_url), "%s/%s", wbase, enc_h);

	char *html = NULL;
	size_t html_len = 0;
	int status = 0;
	http_req_t req = {
		.extra_headers = "Origin: https://soundcloud.com\r\nReferer: https://soundcloud.com/\r\n",
		.no_https_downgrade = strncasecmp(page_url, "https://", 8) == 0,
		.accept_gzip = true,
		.user_agent = SOUNDCLOUD_USER_AGENT,
	};
	if (!http_request(page_url, &req, &html, &html_len, SC_WEB_HTML_LIMIT, SC_TIMEOUT_SECS, &status) || !html ||
		status < 200 || status >= 300) {
		free(html);
		return false;
	}

	const char *hit = strstr(html, "\"hydratable\":\"user\"");
	if (!hit) {
		hit = strstr(html, "\"hydratable\": \"user\"");
	}
	if (!hit) {
		free(html);
		return false;
	}
	const char *data_key = strstr(hit, "\"data\":");
	if (!data_key) {
		free(html);
		return false;
	}
	const char *brace = data_key + 7;
	while (*brace == ' ' || *brace == '\t' || *brace == '\r' || *brace == '\n') {
		brace++;
	}
	if (*brace != '{') {
		free(html);
		return false;
	}

	int depth = 0;
	bool in_str = false;
	bool esc = false;
	size_t span = 0;
	size_t max_span = (size_t)((html + html_len) - brace);
	if (max_span > 65536) {
		max_span = 65536;
	}
	for (size_t i = 0; i < max_span; i++) {
		char ch = brace[i];
		if (in_str) {
			if (esc) {
				esc = false;
			} else if (ch == '\\') {
				esc = true;
			} else if (ch == '"') {
				in_str = false;
			}
		} else {
			if (ch == '"') {
				in_str = true;
			} else if (ch == '{') {
				depth++;
			} else if (ch == '}') {
				depth--;
				if (depth == 0) {
					span = i + 1;
					break;
				}
			}
		}
	}
	if (span == 0) {
		free(html);
		return false;
	}

	char *obj_json = malloc(span + 1);
	if (!obj_json) {
		free(html);
		return false;
	}
	memcpy(obj_json, brace, span);
	obj_json[span] = '\0';
	free(html);

	json_doc_t doc = {0};
	bool ok = false;
	if (json_parse_limited(obj_json, &doc, 1024)) {
		int root = json_root(&doc);
		long long uid = json_obj_llong(&doc, root, "id", 0);
		if (uid > 0) {
			json_obj_str(&doc, root, "username", disp_out, disp_size);
			json_obj_str(&doc, root, "permalink", perm_out, perm_size);
			snprintf(tier_out, tier_size, "Public");
			*uid_out = uid;
			ok = true;
		}
		json_free(&doc);
	}
	free(obj_json);
	return ok;
}

static bool resolve_account_network(const soundcloud_credentials_t *c, long long *uid_out, char *disp_out,
									size_t disp_size, char *perm_out, size_t perm_size, char *tier_out,
									size_t tier_size) {
	*uid_out = 0;
	disp_out[0] = '\0';
	perm_out[0] = '\0';
	tier_out[0] = '\0';

	if (c->oauth_token[0]) {
		json_doc_t doc;
		char *text = NULL;
		if (!api_request_json("GET", "/me", c->oauth_token, c->client_id, &doc, &text, NULL)) {
			return false;
		}
		int root = json_root(&doc);
		long long uid = json_obj_llong(&doc, root, "id", 0);
		json_obj_str(&doc, root, "username", disp_out, disp_size);
		json_obj_str(&doc, root, "permalink", perm_out, perm_size);
		parse_user_tier(&doc, root, tier_out, tier_size);
		json_free(&doc);
		free(text);
		if (uid <= 0) {
			set_error("soundcloud_auth_failed");
			return false;
		}
		*uid_out = uid;
		return true;
	}

	if (!c->username[0]) {
		set_error("soundcloud_not_configured");
		return false;
	}

	char lookup_user[SOUNDCLOUD_USER_MAX];
	snprintf(lookup_user, sizeof(lookup_user), "%s", c->username);

	// Expand mobile share shortlinks (`https://on.soundcloud.com/...`) via HTTP 302 redirect.
	if (is_on_soundcloud_shortlink(lookup_user)) {
		char *sl_body = NULL;
		size_t sl_len = 0;
		int sl_status = 0;
		http_req_t sl_req = {
			.allow_empty_body = true,
			.no_https_downgrade = true,
			.accept_gzip = true,
			.user_agent = SOUNDCLOUD_USER_AGENT,
		};
		if (http_request(lookup_user, &sl_req, &sl_body, &sl_len, 65536, SC_TIMEOUT_SECS, &sl_status)) {
			const char *final_u = http_last_final_url();
			if (final_u && final_u[0]) {
				char expanded[SOUNDCLOUD_USER_MAX];
				snprintf(expanded, sizeof(expanded), "%s", final_u);
				if (normalize_username(expanded) && !is_on_soundcloud_shortlink(expanded)) {
					snprintf(lookup_user, sizeof(lookup_user), "%s", expanded);
				}
			}
		}
		free(sl_body);
	}

	char profile_url[512];
	char enc_profile[896];
	if (is_on_soundcloud_shortlink(lookup_user)) {
		snprintf(profile_url, sizeof(profile_url), "%s", lookup_user);
	} else {
		snprintf(profile_url, sizeof(profile_url), "https://soundcloud.com/%s", lookup_user);
	}
	http_url_encode(profile_url, enc_profile, sizeof(enc_profile));

	char resolve_path[1024];
	snprintf(resolve_path, sizeof(resolve_path), "/resolve?url=%s", enc_profile);

	json_doc_t doc;
	char *text = NULL;
	int status = 0;
	if (api_request_json("GET", resolve_path, "", c->client_id, &doc, &text, &status)) {
		int root = json_root(&doc);
		char kind[32] = "";
		json_obj_str(&doc, root, "kind", kind, sizeof(kind));
		long long uid = json_obj_llong(&doc, root, "id", 0);
		if (uid > 0 && (!kind[0] || strcmp(kind, "user") == 0)) {
			json_obj_str(&doc, root, "username", disp_out, disp_size);
			json_obj_str(&doc, root, "permalink", perm_out, perm_size);
			snprintf(tier_out, tier_size, "Public");
			json_free(&doc);
			free(text);
			*uid_out = uid;
			return true;
		}
		json_free(&doc);
		free(text);
	}

	if (is_on_soundcloud_shortlink(lookup_user)) {
		set_error("soundcloud_user_not_found");
		return false;
	}

	// Tier 2 fallback: search users by permalink / display name, preferring exact permalink match.
	char enc_q[512];
	http_url_encode(lookup_user, enc_q, sizeof(enc_q));
	snprintf(resolve_path, sizeof(resolve_path), "/search/users?q=%s&limit=10", enc_q);
	if (api_request_json("GET", resolve_path, "", c->client_id, &doc, &text, NULL)) {
		int root = json_root(&doc);
		int col = json_get(&doc, root, "collection");
		int n = json_len(&doc, col);
		long long matched_uid = 0;
		int best_rank = 0;
		for (int i = 0; i < n; i++) {
			int u = json_at(&doc, col, i);
			long long uid = json_obj_llong(&doc, u, "id", 0);
			if (uid <= 0) {
				continue;
			}
			char perm[SOUNDCLOUD_USER_MAX] = "";
			char uname[SOUNDCLOUD_NAME_MAX] = "";
			json_obj_str(&doc, u, "permalink", perm, sizeof(perm));
			json_obj_str(&doc, u, "username", uname, sizeof(uname));
			int rank = 1;
			if (strcasecmp(perm, lookup_user) == 0) {
				rank = 3;
			} else if (strcasecmp(uname, lookup_user) == 0) {
				rank = 2;
			}
			if (rank > best_rank) {
				best_rank = rank;
				matched_uid = uid;
				snprintf(disp_out, disp_size, "%s", uname[0] ? uname : perm);
				snprintf(perm_out, perm_size, "%s", perm);
				if (rank == 3) {
					break;
				}
			}
		}
		json_free(&doc);
		free(text);

		if (matched_uid > 0) {
			clear_error();
			snprintf(tier_out, tier_size, "Public");
			*uid_out = matched_uid;
			return true;
		}
	}

	// Tier 3 fallback: extract `window.__sc_hydration` user object from SoundCloud profile page.
	if (resolve_user_from_web_html(lookup_user, uid_out, disp_out, disp_size, perm_out, perm_size, tier_out,
								   tier_size)) {
		clear_error();
		return true;
	}

	if (!last_error[0]) {
		set_error("soundcloud_user_not_found");
	}
	return false;
}

static bool ensure_account_user_id(long long *uid_out) {
	soundcloud_credentials_t snap;
	pthread_mutex_lock(&state_lock);
	if (account_user_id > 0) {
		*uid_out = account_user_id;
		pthread_mutex_unlock(&state_lock);
		return true;
	}
	snap = creds;
	pthread_mutex_unlock(&state_lock);

	if (!snap.username[0] && !snap.oauth_token[0]) {
		wipe(&snap, sizeof(snap));
		set_error("soundcloud_not_configured");
		return false;
	}

	long long uid = 0;
	char disp[SOUNDCLOUD_NAME_MAX], perm[SOUNDCLOUD_USER_MAX], tier[32];
	bool ok = resolve_account_network(&snap, &uid, disp, sizeof(disp), perm, sizeof(perm), tier, sizeof(tier));
	wipe(&snap, sizeof(snap));
	if (!ok) {
		return false;
	}

	pthread_mutex_lock(&creds_io_lock);
	pthread_mutex_lock(&state_lock);
	account_user_id = uid;
	snprintf(account_display_name, sizeof(account_display_name), "%s", disp);
	snprintf(account_permalink, sizeof(account_permalink), "%s", perm);
	snprintf(account_tier, sizeof(account_tier), "%s", tier);
	if ((!creds.username[0] || is_on_soundcloud_shortlink(creds.username)) && perm[0]) {
		snprintf(creds.username, sizeof(creds.username), "%s", perm);
	}
	soundcloud_credentials_t to_save = creds;
	bool should_save = creds_remembered && (to_save.username[0] || to_save.oauth_token[0] || to_save.client_id[0]);
	pthread_mutex_unlock(&state_lock);

	if (should_save) {
		(void)save_credentials_file(&to_save, uid, disp, perm, tier);
	}
	wipe(&to_save, sizeof(to_save));
	pthread_mutex_unlock(&creds_io_lock);

	*uid_out = uid;
	return true;
}

static bool clean_and_validate_credentials(const soundcloud_credentials_t *input, bool require_account,
										   soundcloud_credentials_t *clean_out, char *error, size_t error_size) {
	if (!input) {
		set_error("soundcloud_invalid_credentials");
		if (error && error_size) snprintf(error, error_size, "%s", last_error);
		return false;
	}
	*clean_out = *input;
	trim_inplace(clean_out->client_id);
	if (!normalize_username(clean_out->username) || !normalize_token(clean_out->oauth_token) ||
		(clean_out->client_id[0] && !valid_client_id(clean_out->client_id)) ||
		(require_account && !clean_out->username[0] && !clean_out->oauth_token[0])) {
		wipe(clean_out, sizeof(*clean_out));
		set_error("soundcloud_invalid_credentials");
		if (error && error_size) snprintf(error, error_size, "%s", last_error);
		return false;
	}
	return true;
}

bool soundcloud_save_draft(const soundcloud_credentials_t *input, bool remember, char *error, size_t error_size) {
	if (error && error_size) {
		error[0] = '\0';
	}
	clear_error();

	soundcloud_credentials_t clean;
	if (!clean_and_validate_credentials(input, false, &clean, error, error_size)) {
		return false;
	}

	pthread_mutex_lock(&creds_io_lock);
	pthread_mutex_lock(&state_lock);
	bool same_identity = strcmp(creds.username, clean.username) == 0 &&
						 strcmp(creds.oauth_token, clean.oauth_token) == 0;
	long long uid = same_identity ? account_user_id : 0;
	char disp[SOUNDCLOUD_NAME_MAX] = "";
	char perm[SOUNDCLOUD_USER_MAX] = "";
	char tier[32] = "";
	if (same_identity) {
		snprintf(disp, sizeof(disp), "%s", account_display_name);
		snprintf(perm, sizeof(perm), "%s", account_permalink);
		snprintf(tier, sizeof(tier), "%s", account_tier);
	} else if (clean.username[0] && !is_on_soundcloud_shortlink(clean.username)) {
		snprintf(perm, sizeof(perm), "%s", clean.username);
	}
	pthread_mutex_unlock(&state_lock);

	bool has_any = clean.username[0] != '\0' || clean.oauth_token[0] != '\0' || clean.client_id[0] != '\0';
	bool persisted = (remember && has_any) ? save_credentials_file(&clean, uid, disp, perm, tier)
										   : remove_credentials_file();
	if (!persisted) {
		pthread_mutex_unlock(&creds_io_lock);
		wipe(&clean, sizeof(clean));
		set_error("soundcloud_cannot_save_credentials");
		if (error && error_size) snprintf(error, error_size, "%s", last_error);
		return false;
	}

	pthread_mutex_lock(&state_lock);
	wipe(&creds, sizeof(creds));
	creds = clean;
	creds_remembered = remember;
	creds_from_file = remember && has_any;
	account_user_id = uid;
	snprintf(account_display_name, sizeof(account_display_name), "%s", disp);
	snprintf(account_permalink, sizeof(account_permalink), "%s", perm);
	snprintf(account_tier, sizeof(account_tier), "%s", tier);
	if (!same_identity) {
		liked_count = 0;
	}
	pthread_mutex_unlock(&state_lock);
	pthread_mutex_unlock(&creds_io_lock);

	wipe(&clean, sizeof(clean));
	return true;
}

bool soundcloud_configure(const soundcloud_credentials_t *input, bool remember, char *error, size_t error_size) {
	if (error && error_size) {
		error[0] = '\0';
	}
	clear_error();

	soundcloud_credentials_t clean;
	if (!clean_and_validate_credentials(input, true, &clean, error, error_size)) {
		return false;
	}

	// Persist normalized credentials first (matching Subsonic's contract) so a
	// temporary network failure never discards the URL/token the user entered.
	if (!soundcloud_save_draft(&clean, remember, error, error_size)) {
		wipe(&clean, sizeof(clean));
		return false;
	}

	begin_api();
	long long uid = 0;
	char disp[SOUNDCLOUD_NAME_MAX] = "";
	char perm[SOUNDCLOUD_USER_MAX] = "";
	char tier[32] = "";
	bool ok = resolve_account_network(&clean, &uid, disp, sizeof(disp), perm, sizeof(perm), tier, sizeof(tier));
	end_api();

	if (!ok) {
		wipe(&clean, sizeof(clean));
		if (error && error_size) {
			snprintf(error, error_size, "%s", last_error[0] ? last_error : "soundcloud_connection_failed");
		}
		return false;
	}

	if ((!clean.username[0] || is_on_soundcloud_shortlink(clean.username)) && perm[0]) {
		snprintf(clean.username, sizeof(clean.username), "%s", perm);
	}

	pthread_mutex_lock(&creds_io_lock);
	bool persisted = remember ? save_credentials_file(&clean, uid, disp, perm, tier) : remove_credentials_file();
	if (!persisted) {
		pthread_mutex_unlock(&creds_io_lock);
		wipe(&clean, sizeof(clean));
		set_error("soundcloud_cannot_save_credentials");
		if (error && error_size) snprintf(error, error_size, "%s", last_error);
		return false;
	}

	pthread_mutex_lock(&state_lock);
	wipe(&creds, sizeof(creds));
	creds = clean;
	creds_remembered = remember;
	creds_from_file = remember;
	account_user_id = uid;
	snprintf(account_display_name, sizeof(account_display_name), "%s", disp);
	snprintf(account_permalink, sizeof(account_permalink), "%s", perm);
	snprintf(account_tier, sizeof(account_tier), "%s", tier);
	liked_count = 0;
	pthread_mutex_unlock(&state_lock);
	pthread_mutex_unlock(&creds_io_lock);

	wipe(&clean, sizeof(clean));
	return true;
}

// ---------------------------------------------------------------------------
// Parsing tracks, playlists, and cursor pages
// ---------------------------------------------------------------------------

static bool parse_track_obj(const json_doc_t *doc, int tok, soundcloud_track_t *out) {
	if (tok < 0 || !out) {
		return false;
	}
	memset(out, 0, sizeof(*out));
	long long id = json_obj_llong(doc, tok, "id", 0);
	if (id <= 0) {
		return false;
	}
	out->id = id;
	json_obj_str(doc, tok, "title", out->title, sizeof(out->title));
	json_obj_str(doc, tok, "secret_token", out->secret_token, sizeof(out->secret_token));
	json_obj_str(doc, tok, "permalink_url", out->permalink_url, sizeof(out->permalink_url));
	json_obj_str(doc, tok, "track_authorization", out->track_authorization, sizeof(out->track_authorization));

	int pub = json_get(doc, tok, "publisher_metadata");
	if (pub >= 0) {
		json_obj_str(doc, pub, "artist", out->artist, sizeof(out->artist));
		json_obj_str(doc, pub, "album_title", out->album, sizeof(out->album));
	}
	int user = json_get(doc, tok, "user");
	if (!out->artist[0] && user >= 0) {
		json_obj_str(doc, user, "username", out->artist, sizeof(out->artist));
	}
	if (!out->album[0]) {
		snprintf(out->album, sizeof(out->album), "SoundCloud");
	}

	char raw_art[SOUNDCLOUD_URL_MAX] = "";
	json_obj_str(doc, tok, "artwork_url", raw_art, sizeof(raw_art));
	if (!raw_art[0] && user >= 0) {
		json_obj_str(doc, user, "avatar_url", raw_art, sizeof(raw_art));
	}
	soundcloud_artwork_url(raw_art, 500, out->cover, sizeof(out->cover));

	long long dur_ms = json_obj_llong(doc, tok, "duration", 0);
	long long full_ms = json_obj_llong(doc, tok, "full_duration", dur_ms);
	out->duration = dur_ms > 0 ? (int)((dur_ms + 500) / 1000) : 0;
	out->full_duration = full_ms > 0 ? (int)((full_ms + 500) / 1000) : out->duration;

	char access[32] = "";
	char policy[32] = "";
	json_obj_str(doc, tok, "access", access, sizeof(access));
	json_obj_str(doc, tok, "policy", policy, sizeof(policy));
	bool streamable = json_obj_bool(doc, tok, "streamable", true);

	int transcodings = json_path(doc, tok, "media.transcodings");
	int tc_len = json_len(doc, transcodings);
	bool any_playable_tc = false;
	bool all_snipped = tc_len > 0;
	int non_abr_count = 0;
	for (int i = 0; i < tc_len; i++) {
		int tc = json_at(doc, transcodings, i);
		char url[64] = "";
		char preset[48] = "";
		json_obj_str(doc, tc, "url", url, sizeof(url));
		json_obj_str(doc, tc, "preset", preset, sizeof(preset));
		if (preset_is_abr(preset)) {
			continue;
		}
		non_abr_count++;
		if (url[0]) {
			any_playable_tc = true;
		}
		if (!json_obj_bool(doc, tc, "snipped", false)) {
			all_snipped = false;
		}
	}
	if (non_abr_count == 0) {
		all_snipped = false;
	}

	out->preview = (strcmp(access, "preview") == 0) || (strcmp(policy, "SNIP") == 0 && out->duration <= 45) ||
				   (non_abr_count > 0 && all_snipped);
	if (strcmp(access, "blocked") == 0 || strcmp(policy, "BLOCK") == 0 || !streamable) {
		out->streamable = false;
	} else if (transcodings >= 0) {
		out->streamable = any_playable_tc;
	} else {
		out->streamable = true;
	}
	return out->title[0] != '\0';
}

static bool parse_playlist_obj(const json_doc_t *doc, int tok, bool liked, soundcloud_playlist_t *out) {
	if (tok < 0 || !out) {
		return false;
	}
	memset(out, 0, sizeof(*out));
	long long id = json_obj_llong(doc, tok, "id", 0);
	if (id <= 0) {
		return false;
	}
	out->id = id;
	out->is_liked_playlist = liked;
	json_obj_str(doc, tok, "title", out->title, sizeof(out->title));
	json_obj_str(doc, tok, "secret_token", out->secret_token, sizeof(out->secret_token));
	out->track_count = (int)json_obj_long(doc, tok, "track_count", 0);

	int user = json_get(doc, tok, "user");
	if (user >= 0) {
		json_obj_str(doc, user, "username", out->owner, sizeof(out->owner));
	}

	char raw_art[SOUNDCLOUD_URL_MAX] = "";
	json_obj_str(doc, tok, "artwork_url", raw_art, sizeof(raw_art));
	if (!raw_art[0]) {
		int tracks = json_get(doc, tok, "tracks");
		int first = json_at(doc, tracks, 0);
		if (first >= 0) {
			json_obj_str(doc, first, "artwork_url", raw_art, sizeof(raw_art));
		}
	}
	if (!raw_art[0] && user >= 0) {
		json_obj_str(doc, user, "avatar_url", raw_art, sizeof(raw_art));
	}
	soundcloud_artwork_url(raw_art, 300, out->cover, sizeof(out->cover));
	return out->title[0] != '\0';
}

// ---------------------------------------------------------------------------
// Public API functions: Search, Likes, Reposts, Playlists, Playlist Tracks
// ---------------------------------------------------------------------------

int soundcloud_search_tracks(const char *query, int offset, soundcloud_track_t *out, int max, bool *more) {
	clear_error();
	if (more) {
		*more = false;
	}
	if (!query || !query[0] || !out || max <= 0 || offset < 0) {
		set_error("soundcloud_invalid_query");
		return -1;
	}
	int limit = max < SOUNDCLOUD_PAGE_LIMIT ? max : SOUNDCLOUD_PAGE_LIMIT;
	char enc[512];
	http_url_encode(query, enc, sizeof(enc));
	char path[768];
	snprintf(path, sizeof(path), "/search/tracks?q=%s&limit=%d&offset=%d&linked_partitioning=1", enc, limit, offset);

	begin_api();
	json_doc_t doc;
	char *text = NULL;
	bool ok = api_request_json("GET", path, NULL, NULL, &doc, &text, NULL);
	end_api();
	if (!ok) {
		return -1;
	}

	int root = json_root(&doc);
	int col = json_get(&doc, root, "collection");
	int len = json_len(&doc, col);
	int count = 0;
	for (int i = 0; i < len && count < max; i++) {
		if (parse_track_obj(&doc, json_at(&doc, col, i), &out[count])) {
			count++;
		}
	}
	char next_href[64] = "";
	json_obj_str(&doc, root, "next_href", next_href, sizeof(next_href));
	long total = json_obj_long(&doc, root, "total_results", 0);
	if (more) {
		*more = next_href[0] != '\0' || (total > 0 && offset + len < total);
	}
	json_free(&doc);
	free(text);
	return count;
}

int soundcloud_liked_tracks(const char *cursor, soundcloud_track_t *out, int max, char *next_cursor,
							size_t next_cursor_size) {
	clear_error();
	if (next_cursor && next_cursor_size) {
		next_cursor[0] = '\0';
	}
	if (!out || max <= 0) {
		return -1;
	}

	begin_api();
	long long uid = 0;
	if (!ensure_account_user_id(&uid)) {
		end_api();
		return -1;
	}

	char path[SOUNDCLOUD_CURSOR_MAX];
	int limit = max < SOUNDCLOUD_PAGE_LIMIT ? max : SOUNDCLOUD_PAGE_LIMIT;
	if (cursor && cursor[0]) {
		snprintf(path, sizeof(path), "%s", cursor);
	} else {
		snprintf(path, sizeof(path), "/users/%lld/track_likes?limit=%d&linked_partitioning=1", uid, limit);
	}

	json_doc_t doc;
	char *text = NULL;
	bool ok = api_request_json("GET", path, NULL, NULL, &doc, &text, NULL);
	end_api();
	if (!ok) {
		return -1;
	}

	int root = json_root(&doc);
	int col = json_get(&doc, root, "collection");
	int len = json_len(&doc, col);
	int count = 0;
	for (int i = 0; i < len && count < max; i++) {
		int item = json_at(&doc, col, i);
		int trk = json_get(&doc, item, "track");
		if (trk < 0) {
			trk = item;
		}
		if (parse_track_obj(&doc, trk, &out[count])) {
			soundcloud_note_liked(out[count].id, true);
			count++;
		}
	}
	if (next_cursor && next_cursor_size) {
		json_obj_str(&doc, root, "next_href", next_cursor, next_cursor_size);
	}
	json_free(&doc);
	free(text);
	return count;
}

int soundcloud_reposts(const char *cursor, soundcloud_repost_t *out, int max, char *next_cursor,
					   size_t next_cursor_size) {
	clear_error();
	if (next_cursor && next_cursor_size) {
		next_cursor[0] = '\0';
	}
	if (!out || max <= 0) {
		return -1;
	}

	begin_api();
	long long uid = 0;
	if (!ensure_account_user_id(&uid)) {
		end_api();
		return -1;
	}

	char path[SOUNDCLOUD_CURSOR_MAX];
	int limit = max < SOUNDCLOUD_PAGE_LIMIT ? max : SOUNDCLOUD_PAGE_LIMIT;
	if (cursor && cursor[0]) {
		snprintf(path, sizeof(path), "%s", cursor);
	} else {
		snprintf(path, sizeof(path), "/stream/users/%lld/reposts?limit=%d&linked_partitioning=1", uid, limit);
	}

	json_doc_t doc;
	char *text = NULL;
	int status = 0;
	bool ok = api_request_json("GET", path, NULL, NULL, &doc, &text, &status);
	if (!ok && status == 404 && (!cursor || !cursor[0])) {
		snprintf(path, sizeof(path), "/stream/users/%lld?limit=%d&linked_partitioning=1", uid, limit);
		ok = api_request_json("GET", path, NULL, NULL, &doc, &text, &status);
	}
	end_api();
	if (!ok) {
		return -1;
	}

	int root = json_root(&doc);
	int col = json_get(&doc, root, "collection");
	int len = json_len(&doc, col);
	int count = 0;
	for (int i = 0; i < len && count < max; i++) {
		int item = json_at(&doc, col, i);
		char type[40] = "";
		json_obj_str(&doc, item, "type", type, sizeof(type));
		if (type[0] && strstr(type, "repost") == NULL) {
			continue;
		}
		memset(&out[count], 0, sizeof(out[count]));
		int trk = json_get(&doc, item, "track");
		int pl = json_get(&doc, item, "playlist");
		if (trk >= 0 && parse_track_obj(&doc, trk, &out[count].track)) {
			out[count].is_playlist = false;
			count++;
		} else if (pl >= 0 && parse_playlist_obj(&doc, pl, true, &out[count].playlist)) {
			out[count].is_playlist = true;
			count++;
		}
	}
	if (next_cursor && next_cursor_size) {
		json_obj_str(&doc, root, "next_href", next_cursor, next_cursor_size);
	}
	json_free(&doc);
	free(text);
	return count;
}

int soundcloud_playlists(const char *cursor, soundcloud_playlist_t *out, int max, char *next_cursor,
						 size_t next_cursor_size) {
	clear_error();
	if (next_cursor && next_cursor_size) {
		next_cursor[0] = '\0';
	}
	if (!out || max <= 0) {
		return -1;
	}

	begin_api();
	long long uid = 0;
	if (!ensure_account_user_id(&uid)) {
		end_api();
		return -1;
	}

	int limit = max < SOUNDCLOUD_PAGE_LIMIT ? max : SOUNDCLOUD_PAGE_LIMIT;
	int count = 0;

	// Cursor encoding: "L|<url>" continues liked playlists; a raw URL continues
	// the user's created playlists first, then falls through to liked playlists.
	bool in_liked_phase = cursor && strncmp(cursor, "L|", 2) == 0;
	const char *real_cursor = in_liked_phase ? cursor + 2 : cursor;

	if (!in_liked_phase) {
		char path[SOUNDCLOUD_CURSOR_MAX];
		if (real_cursor && real_cursor[0]) {
			snprintf(path, sizeof(path), "%s", real_cursor);
		} else {
			snprintf(path, sizeof(path), "/users/%lld/playlists?limit=%d&linked_partitioning=1", uid, limit);
		}
		json_doc_t doc;
		char *text = NULL;
		if (!api_request_json("GET", path, NULL, NULL, &doc, &text, NULL)) {
			end_api();
			return -1;
		}
		int root = json_root(&doc);
		int col = json_get(&doc, root, "collection");
		int len = json_len(&doc, col);
		for (int i = 0; i < len && count < max; i++) {
			if (parse_playlist_obj(&doc, json_at(&doc, col, i), false, &out[count])) {
				count++;
			}
		}
		char own_next[SOUNDCLOUD_CURSOR_MAX] = "";
		json_obj_str(&doc, root, "next_href", own_next, sizeof(own_next));
		json_free(&doc);
		free(text);

		if (own_next[0]) {
			if (next_cursor && next_cursor_size) {
				snprintf(next_cursor, next_cursor_size, "%s", own_next);
			}
			end_api();
			return count;
		}
		if (count >= max) {
			if (next_cursor && next_cursor_size) {
				snprintf(next_cursor, next_cursor_size, "L|/users/%lld/playlist_likes?limit=%d&linked_partitioning=1",
						 uid, limit);
			}
			end_api();
			return count;
		}
		real_cursor = NULL;
	}

	// Liked / saved playlists from other creators.
	char liked_path[SOUNDCLOUD_CURSOR_MAX];
	if (real_cursor && real_cursor[0]) {
		snprintf(liked_path, sizeof(liked_path), "%s", real_cursor);
	} else {
		snprintf(liked_path, sizeof(liked_path), "/users/%lld/playlist_likes?limit=%d&linked_partitioning=1", uid,
				 limit);
	}

	json_doc_t doc;
	char *text = NULL;
	if (api_request_json("GET", liked_path, NULL, NULL, &doc, &text, NULL)) {
		int root = json_root(&doc);
		int col = json_get(&doc, root, "collection");
		int len = json_len(&doc, col);
		for (int i = 0; i < len && count < max; i++) {
			int item = json_at(&doc, col, i);
			int pl = json_get(&doc, item, "playlist");
			if (pl < 0) {
				pl = item;
			}
			soundcloud_playlist_t cand;
			if (!parse_playlist_obj(&doc, pl, true, &cand)) {
				continue;
			}
			bool dup = false;
			for (int k = 0; k < count; k++) {
				if (out[k].id == cand.id) {
					dup = true;
					break;
				}
			}
			if (!dup) {
				out[count++] = cand;
			}
		}
		char liked_next[SOUNDCLOUD_CURSOR_MAX - 4] = "";
		json_obj_str(&doc, root, "next_href", liked_next, sizeof(liked_next));
		if (liked_next[0] && next_cursor && next_cursor_size) {
			snprintf(next_cursor, next_cursor_size, "L|%s", liked_next);
		}
		json_free(&doc);
		free(text);
	}

	end_api();
	return count;
}

static void hydrate_stub_tracks(soundcloud_track_t *tracks, int count, const long long *stub_ids, int stub_count) {
	for (int start = 0; start < stub_count; start += 50) {
		int batch = stub_count - start < 50 ? stub_count - start : 50;
		char ids_csv[1200];
		int pos = 0;
		for (int i = 0; i < batch; i++) {
			int n = snprintf(ids_csv + pos, sizeof(ids_csv) - (size_t)pos, "%s%lld", i ? "," : "", stub_ids[start + i]);
			if (n < 0 || (size_t)(pos + n) >= sizeof(ids_csv)) {
				break;
			}
			pos += n;
		}
		char path[1400];
		snprintf(path, sizeof(path), "/tracks?ids=%s", ids_csv);

		json_doc_t doc;
		char *text = NULL;
		if (!api_request_json("GET", path, NULL, NULL, &doc, &text, NULL)) {
			continue;
		}
		int root = json_root(&doc);
		int len = json_len(&doc, root);
		for (int i = 0; i < len; i++) {
			soundcloud_track_t hydrated;
			if (!parse_track_obj(&doc, json_at(&doc, root, i), &hydrated)) {
				continue;
			}
			for (int k = 0; k < count; k++) {
				if (tracks[k].id == hydrated.id && !tracks[k].title[0]) {
					tracks[k] = hydrated;
				}
			}
		}
		json_free(&doc);
		free(text);
	}
}

int soundcloud_playlist_tracks(long long playlist_id, const char *secret_token, int offset, soundcloud_track_t *out,
							   int max, bool *more) {
	clear_error();
	if (more) {
		*more = false;
	}
	if (playlist_id <= 0 || !out || max <= 0 || offset < 0) {
		set_error("soundcloud_invalid_id");
		return -1;
	}

	char path[512];
	if (secret_token && secret_token[0]) {
		char enc_sec[192];
		http_url_encode(secret_token, enc_sec, sizeof(enc_sec));
		snprintf(path, sizeof(path), "/playlists/%lld?representation=full&secret_token=%s", playlist_id, enc_sec);
	} else {
		snprintf(path, sizeof(path), "/playlists/%lld?representation=full", playlist_id);
	}

	begin_api();
	json_doc_t doc;
	char *text = NULL;
	bool ok = api_request_json("GET", path, NULL, NULL, &doc, &text, NULL);
	if (!ok) {
		end_api();
		return -1;
	}

	int root = json_root(&doc);
	int tracks_arr = json_get(&doc, root, "tracks");
	int total = json_len(&doc, tracks_arr);
	if (offset >= total) {
		json_free(&doc);
		free(text);
		end_api();
		return 0;
	}

	int slice_end = offset + max < total ? offset + max : total;
	if (more) {
		*more = slice_end < total;
	}

	long long *stubs = calloc((size_t)(slice_end - offset), sizeof(*stubs));
	int stub_count = 0;
	int count = 0;

	for (int i = offset; i < slice_end; i++) {
		int item = json_at(&doc, tracks_arr, i);
		long long id = json_obj_llong(&doc, item, "id", 0);
		if (id <= 0) {
			continue;
		}
		if (!parse_track_obj(&doc, item, &out[count])) {
			memset(&out[count], 0, sizeof(out[count]));
			out[count].id = id;
			if (stubs) {
				stubs[stub_count++] = id;
			}
		}
		count++;
	}
	json_free(&doc);
	free(text);

	if (stub_count > 0 && stubs) {
		hydrate_stub_tracks(out, count, stubs, stub_count);
	}
	free(stubs);
	end_api();

	// Compact out any deleted/private tracks that `/tracks?ids=` omitted.
	int kept = 0;
	for (int i = 0; i < count; i++) {
		if (out[i].title[0]) {
			if (kept != i) {
				out[kept] = out[i];
			}
			kept++;
		}
	}
	return kept;
}

// ---------------------------------------------------------------------------
// Stream resolution & Likes write API
// ---------------------------------------------------------------------------

static int score_transcoding(const char *protocol, const char *mime, const char *quality, int pref_quality,
							 bool allow_aac) {
	if (!protocol || !mime) {
		return -1;
	}
	// Encrypted HLS (Widevine / CENC ctr-encrypted-hls) requires a proprietary
	// CDM not available on Linux/MIPS; skip it in favour of standard streams.
	if (strstr(protocol, "encrypted") != NULL || strncmp(protocol, "ctr-", 4) == 0 ||
		strncmp(protocol, "cbc-", 4) == 0) {
		return -1;
	}
	bool is_prog = strcmp(protocol, "progressive") == 0;
	bool is_hls = strcmp(protocol, "hls") == 0;
	if (!is_prog && !is_hls) {
		return -1;
	}

	bool is_mp3 = strstr(mime, "audio/mpeg") != NULL || strstr(mime, "mp3") != NULL;
	bool is_opus = strstr(mime, "opus") != NULL || strstr(mime, "audio/ogg") != NULL;
	bool is_aac = strstr(mime, "mp4") != NULL || strstr(mime, "aac") != NULL;
	bool is_hq = quality && strcmp(quality, "hq") == 0;

	if (is_aac && !allow_aac) {
		return -1;
	}
	if (!is_mp3 && !is_opus && !is_aac) {
		return -1;
	}

	if (pref_quality == SOUNDCLOUD_QUALITY_HIGH) {
		if (is_hq && is_aac && is_hls) return 100;
		if (is_aac && is_hls) return 95;
		if (is_hq && is_mp3 && is_prog) return 92;
		if (is_hq && is_mp3 && is_hls) return 88;
		if (is_mp3 && is_prog) return 80;
		if (is_mp3 && is_hls) return 75;
		if (is_opus && is_hls) return 60;
	} else if (pref_quality == SOUNDCLOUD_QUALITY_LOW) {
		if (is_opus && is_hls) return 100;
		if (is_mp3 && is_prog) return 80;
		if (is_mp3 && is_hls) return 75;
		if (is_aac && is_hls) return 40;
	} else {
		// Standard (default): progressive MP3 first (single HTTP stream, instant
		// start + full seeking), then HLS MP3, then HLS Opus, then AAC.
		if (is_mp3 && is_prog) return 100;
		if (is_mp3 && is_hls) return 90;
		if (is_opus && is_hls) return 75;
		if (is_aac && is_hls) return 60;
	}
	return 10;
}

typedef struct {
	int score;
	char url[SOUNDCLOUD_URL_MAX];
	soundcloud_stream_kind_t kind;
	soundcloud_codec_t codec;
	bool snipped;
	long long dur_ms;
} tc_cand_t;

static bool fetch_track_metadata_doc(long long track_id, const char *secret_token, const char *permalink_url,
									 json_doc_t *doc_out, char **text_out, int *track_tok_out) {
	char path[512];
	char enc_sec[192] = "";
	if (secret_token && secret_token[0]) {
		http_url_encode(secret_token, enc_sec, sizeof(enc_sec));
		snprintf(path, sizeof(path), "/tracks/%lld?secret_token=%s", track_id, enc_sec);
	} else {
		snprintf(path, sizeof(path), "/tracks/%lld", track_id);
	}

	int status = 0;
	if (api_request_json("GET", path, NULL, NULL, doc_out, text_out, &status)) {
		int root = json_root(doc_out);
		if (root >= 0 && doc_out->toks[root].type == JSON_OBJECT && json_obj_llong(doc_out, root, "id", 0) > 0) {
			*track_tok_out = root;
			return true;
		}
		json_free(doc_out);
		free(*text_out);
		*text_out = NULL;
	}

	if (status == 429) {
		return false;
	}

	// Fallback 1: batch `/tracks?ids=<id>` endpoint (returns a 1-element JSON array).
	if (enc_sec[0]) {
		snprintf(path, sizeof(path), "/tracks?ids=%lld&secret_token=%s", track_id, enc_sec);
	} else {
		snprintf(path, sizeof(path), "/tracks?ids=%lld", track_id);
	}
	if (api_request_json("GET", path, NULL, NULL, doc_out, text_out, &status)) {
		int root = json_root(doc_out);
		int first = (root >= 0 && doc_out->toks[root].type == JSON_ARRAY) ? json_at(doc_out, root, 0) : -1;
		if (first >= 0 && json_obj_llong(doc_out, first, "id", 0) > 0) {
			clear_error();
			*track_tok_out = first;
			return true;
		}
		json_free(doc_out);
		free(*text_out);
		*text_out = NULL;
	}

	// Fallback 2: `/resolve?url=<permalink_url>` when the track's permalink URL is known.
	if (permalink_url && permalink_url[0]) {
		char enc_perm[768];
		char rpath[896];
		http_url_encode(permalink_url, enc_perm, sizeof(enc_perm));
		snprintf(rpath, sizeof(rpath), "/resolve?url=%s", enc_perm);
		if (api_request_json("GET", rpath, NULL, NULL, doc_out, text_out, &status)) {
			int root = json_root(doc_out);
			if (root >= 0 && doc_out->toks[root].type == JSON_OBJECT && json_obj_llong(doc_out, root, "id", 0) > 0) {
				clear_error();
				*track_tok_out = root;
				return true;
			}
			json_free(doc_out);
			free(*text_out);
			*text_out = NULL;
		}
	}

	return false;
}

static bool resolve_stream_internal(long long track_id, const char *secret_token, const char *permalink_url,
									const char *track_auth_hint, bool allow_aac, bool allow_progressive,
									soundcloud_stream_t *out) {
	clear_error();
	if (!out || track_id <= 0) {
		set_error("soundcloud_invalid_id");
		return false;
	}
	memset(out, 0, sizeof(*out));

	int pref_q = soundcloud_get_quality();

	begin_api();
	json_doc_t doc = {0};
	char *text = NULL;
	int track_tok = -1;
	if (!fetch_track_metadata_doc(track_id, secret_token, permalink_url, &doc, &text, &track_tok)) {
		end_api();
		return false;
	}

	char access[32] = "";
	char track_auth[SOUNDCLOUD_AUTH_MAX] = "";
	json_obj_str(&doc, track_tok, "access", access, sizeof(access));
	json_obj_str(&doc, track_tok, "track_authorization", track_auth, sizeof(track_auth));
	if (!track_auth[0] && track_auth_hint && track_auth_hint[0]) {
		snprintf(track_auth, sizeof(track_auth), "%s", track_auth_hint);
	}
	long long dur_ms = json_obj_llong(&doc, track_tok, "duration", 0);

	int transcodings = json_path(&doc, track_tok, "media.transcodings");
	int tc_len = json_len(&doc, transcodings);
	tc_cand_t *cands = tc_len > 0 ? calloc((size_t)tc_len, sizeof(*cands)) : NULL;
	int cand_count = 0;
	bool saw_encrypted = false;

	for (int i = 0; i < tc_len && cands; i++) {
		int tc = json_at(&doc, transcodings, i);
		char tc_url[SOUNDCLOUD_URL_MAX] = "";
		char preset[48] = "";
		char proto[48] = "";
		char mime[96] = "";
		char qual[16] = "";
		json_obj_str(&doc, tc, "url", tc_url, sizeof(tc_url));
		json_obj_str(&doc, tc, "preset", preset, sizeof(preset));
		int fmt = json_get(&doc, tc, "format");
		if (fmt >= 0) {
			json_obj_str(&doc, fmt, "protocol", proto, sizeof(proto));
			json_obj_str(&doc, fmt, "mime_type", mime, sizeof(mime));
		}
		json_obj_str(&doc, tc, "quality", qual, sizeof(qual));
		if (!tc_url[0]) {
			continue;
		}
		// SoundCloud's `abr` / `abr_sq` transcodings return 404 on API v2; skip them.
		if (preset_is_abr(preset)) {
			continue;
		}
		if (strstr(proto, "encrypted") != NULL || strncmp(proto, "ctr-", 4) == 0 || strncmp(proto, "cbc-", 4) == 0 ||
			strstr(tc_url, "/encrypted-hls") != NULL) {
			saw_encrypted = true;
			continue;
		}
		if (strcmp(proto, "hls") != 0 && strstr(tc_url, "/hls") != NULL) {
			snprintf(proto, sizeof(proto), "hls");
		}
		bool is_prog = strcmp(proto, "progressive") == 0;
		if (is_prog && !allow_progressive) {
			continue;
		}

		bool tc_snipped = json_obj_bool(&doc, tc, "snipped", false) || strcmp(access, "preview") == 0;
		int sc = score_transcoding(proto, mime, qual, pref_q, allow_aac);
		if (sc < 0) {
			continue;
		}
		if (!tc_snipped) {
			sc += 1000; // Always prefer a full-length transcoding over a 30s Go+ snippet
		}

		tc_cand_t *c = &cands[cand_count++];
		c->score = sc;
		snprintf(c->url, sizeof(c->url), "%s", tc_url);
		c->kind = is_prog ? SOUNDCLOUD_STREAM_PROGRESSIVE : SOUNDCLOUD_STREAM_HLS;
		if (strstr(mime, "opus") || strstr(mime, "ogg")) {
			c->codec = SOUNDCLOUD_CODEC_OPUS;
		} else if (strstr(mime, "mp4") || strstr(mime, "aac")) {
			c->codec = SOUNDCLOUD_CODEC_AAC;
		} else {
			c->codec = SOUNDCLOUD_CODEC_MP3;
		}
		c->snipped = tc_snipped;
		c->dur_ms = json_obj_llong(&doc, tc, "duration", dur_ms);
	}
	json_free(&doc);
	free(text);

	if (cand_count == 0) {
		free(cands);
		end_api();
		if (saw_encrypted && tc_len > 0) {
			set_error("soundcloud_drm_unsupported");
		} else {
			set_error("soundcloud_track_unavailable");
		}
		return false;
	}

	// Insertion-sort candidates in descending score order (typically 2..6 entries).
	for (int i = 1; i < cand_count; i++) {
		tc_cand_t key = cands[i];
		int j = i - 1;
		while (j >= 0 && cands[j].score < key.score) {
			cands[j + 1] = cands[j];
			j--;
		}
		cands[j + 1] = key;
	}

	char enc_auth[SOUNDCLOUD_AUTH_MAX * 2] = "";
	if (track_auth[0]) {
		http_url_encode(track_auth, enc_auth, sizeof(enc_auth));
	}
	char enc_sec[192] = "";
	if (secret_token && secret_token[0]) {
		http_url_encode(secret_token, enc_sec, sizeof(enc_sec));
	}

	// Try each ranked transcoding candidate in turn: SoundCloud frequently lists
	// legacy `mp3_1_0` formats that return HTTP 404 while `opus_0_0` / `aac_160k`
	// HLS formats in the same `media.transcodings` array succeed.
	for (int i = 0; i < cand_count; i++) {
		char req_url[SOUNDCLOUD_URL_MAX + SOUNDCLOUD_AUTH_MAX * 2 + 256];
		snprintf(req_url, sizeof(req_url), "%s", cands[i].url);

		if (enc_auth[0] && strstr(req_url, "track_authorization=") == NULL) {
			size_t cur = strlen(req_url);
			snprintf(req_url + cur, sizeof(req_url) - cur, "%strack_authorization=%s",
					 strchr(req_url, '?') ? "&" : "?", enc_auth);
		}
		if (enc_sec[0] && strstr(req_url, "secret_token=") == NULL) {
			size_t cur = strlen(req_url);
			snprintf(req_url + cur, sizeof(req_url) - cur, "%ssecret_token=%s",
					 strchr(req_url, '?') ? "&" : "?", enc_sec);
		}

		json_doc_t sdoc = {0};
		char *stext = NULL;
		int tc_status = 0;
		bool ok = api_request_json("GET", req_url, NULL, NULL, &sdoc, &stext, &tc_status);
		if (!ok) {
			if (tc_status == 429) {
				free(cands);
				end_api();
				return false;
			}
			continue;
		}

		char cdn_url[SOUNDCLOUD_URL_MAX] = "";
		json_obj_str(&sdoc, json_root(&sdoc), "url", cdn_url, sizeof(cdn_url));
		json_free(&sdoc);
		free(stext);

		if (!cdn_url[0]) {
			continue;
		}

		clear_error();
		out->kind = cands[i].kind;
		out->codec = cands[i].codec;
		snprintf(out->url, sizeof(out->url), "%s", cdn_url);
		out->duration_secs = cands[i].dur_ms > 0 ? (int)((cands[i].dur_ms + 500) / 1000) : 0;
		out->snipped = cands[i].snipped;
		free(cands);
		end_api();
		return true;
	}

	free(cands);
	end_api();
	set_error("soundcloud_track_unavailable");
	return false;
}

bool soundcloud_get_track_stream_ex(const soundcloud_track_t *track, bool allow_aac, bool allow_progressive,
									soundcloud_stream_t *out) {
	if (!track) {
		set_error("soundcloud_invalid_id");
		return false;
	}
	return resolve_stream_internal(track->id, track->secret_token, track->permalink_url, track->track_authorization,
								   allow_aac, allow_progressive, out);
}

bool soundcloud_get_track_stream(const soundcloud_track_t *track, bool allow_aac, soundcloud_stream_t *out) {
	return soundcloud_get_track_stream_ex(track, allow_aac, true, out);
}

bool soundcloud_get_stream(long long track_id, const char *secret_token, bool allow_aac, soundcloud_stream_t *out) {
	return resolve_stream_internal(track_id, secret_token, NULL, NULL, allow_aac, true, out);
}

bool soundcloud_like_track(long long track_id, bool like) {
	clear_error();
	if (track_id <= 0) {
		set_error("soundcloud_invalid_id");
		return false;
	}
	if (!soundcloud_can_like()) {
		set_error("soundcloud_token_needed_for_likes");
		return false;
	}

	begin_api();
	long long uid = 0;
	if (!ensure_account_user_id(&uid)) {
		end_api();
		return false;
	}

	char path[160];
	snprintf(path, sizeof(path), "/users/%lld/track_likes/%lld", uid, track_id);
	bool ok = api_request_json(like ? "PUT" : "DELETE", path, NULL, NULL, NULL, NULL, NULL);
	end_api();
	if (ok) {
		soundcloud_note_liked(track_id, like);
	}
	return ok;
}

void soundcloud_set_endpoints_for_test(const char *api_url, const char *web_url) {
	pthread_mutex_lock(&state_lock);
	snprintf(api_base, sizeof(api_base), "%s", (api_url && api_url[0]) ? api_url : SC_API_BASE_DEFAULT);
	snprintf(web_base, sizeof(web_base), "%s", (web_url && web_url[0]) ? web_url : SC_WEB_BASE_DEFAULT);
	discovered_client_id[0] = '\0';
	pthread_mutex_unlock(&state_lock);
}
