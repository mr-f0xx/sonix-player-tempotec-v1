#define _GNU_SOURCE 1
#include "ytmusic.h"

#include "src/system/core/config.h"
#include "src/system/core/json.h"
#include "src/system/core/region.h"
#include "src/system/core/sha1.h"
#include "src/system/decode/growfile.h"
#include "src/system/decode/mp4.h"

// Declared here because the tag writer near the top of the file also needs to
// know where a file it is describing sits; defined beside the downloader.
static const char *playback_cache_dir(void);
static bool ensure_cache_dir(void);
#include "src/system/net/http.h"
#include "src/system/streaming/streamkeys.h"
#include "src/system/streaming/ytlikes.h"
#include "src/system/streaming/ytpodcastplayed.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <sys/stat.h>
#include <time.h>

#define YTMUSIC_SESSION_FILE "ytmusic_session.ini"
#define YTMUSIC_COOKIE_MAX 6000
#define YTMUSIC_RESPONSE_LIMIT (3 * 1024 * 1024)
#define YTMUSIC_RENDERER_SIZE_LIMIT (256 * 1024)
#define YTMUSIC_RENDERER_TOKEN_LIMIT 8192
#define YTMUSIC_PLAYER_TOKEN_LIMIT 24000
#define YTMUSIC_TIMEOUT 15
#define YTMUSIC_SITE "https://music.youtube.com"

// The web client's InnerTube key, supplied by the build.
//
// It is not an account credential and not a secret of anybody's: it is the key
// the music.youtube.com page hands every visitor, and it is used directly
// rather than scraped from the page because that page answers a client whose
// User-Agent it does not recognise -- and this player is not a browser -- with
// a "your browser is deprecated" stub that contains no key at all. That stub is
// what ytmusic_context_unreadable was.
//
// It is still not committed: it belongs to Google rather than to this
// repository, secret scanners flag the AIza... form wherever it appears (GitHub
// did, on the commit that first carried it), and a key baked into a public
// image is one Google can rotate out from under every build of it. So the
// value arrives from the build -- `make YTMUSIC_API_KEY=...`, the environment,
// or a .ytmusic-key file the Makefile reads -- and a build that was given none
// says so at the first request rather than sending one that cannot work.
#ifndef YTMUSIC_API_KEY
#define YTMUSIC_API_KEY ""
#endif

// The client version is the page's build stamp, in the same "1.YYYYMMDD.01.00"
// form the public clients use, derived from today's date so it never goes
// stale in a shipped image.
#define YTMUSIC_CLIENT_VERSION_FALLBACK "1.20250101.01.00"

static pthread_mutex_t session_lock = PTHREAD_MUTEX_INITIALIZER;
static char session_cookie[YTMUSIC_COOKIE_MAX];
static pthread_mutex_t api_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t request_lock = PTHREAD_MUTEX_INITIALIZER;

// The bitrate of the stream the last resolution chose, in kbit/s: what the
// instance's own reply said, and the only honest figure for a cache file that is
// still growing. The decoder cannot tell while the sample table is incomplete,
// and the file's size over its length gave "3 kbps" on screen.
static volatile int resolved_bitrate_kbps;

int ytmusic_last_bitrate_kbps(void) { return resolved_bitrate_kbps; }
static char api_key[128];
static char client_version[64];

static void set_error(char *out, size_t size, const char *reason) {
	if (out && size) snprintf(out, size, "%s", reason ? reason : "ytmusic_request_failed");
}

static void wipe_free(char *s) {
	if (!s) return;
	volatile unsigned char *p = (volatile unsigned char *)s;
	size_t n = strlen(s);
	while (n--) *p++ = 0;
	free(s);
}

static void session_path(char *out, size_t size) {
	out[0] = '\0';
	const char *cfg = config_path();
	if (!cfg || !cfg[0]) return;
	const char *slash = strrchr(cfg, '/');
	if (slash) snprintf(out, size, "%.*s/%s", (int)(slash - cfg), cfg, YTMUSIC_SESSION_FILE);
	else snprintf(out, size, "%s", YTMUSIC_SESSION_FILE);
}

static bool save_session(void) {
	char path[600], tmp[640];
	session_path(path, sizeof(path));
	if (!path[0]) return false;
	snprintf(tmp, sizeof(tmp), "%s.tmp", path);
	FILE *f = fopen(tmp, "w");
	if (!f) return false;
	if (fchmod(fileno(f), 0600) != 0) {
		fclose(f);
		remove(tmp);
		return false;
	}
	bool wrote = fprintf(f, "cookie=%s\n", session_cookie) > 0;
	bool closed = fclose(f) == 0;
	bool ok = wrote && closed;
	if (!ok || rename(tmp, path) != 0) {
		remove(tmp);
		return false;
	}
	return true;
}

static bool has_session_cookie(const char *cookie) {
	static const char *const names[] = {"SAPISID=", "__Secure-3PAPISID=", "__Secure-1PAPISID="};
	for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
		const char *p = cookie;
		while ((p = strstr(p, names[i])) != NULL) {
			if (p == cookie || p[-1] == ';' || isspace((unsigned char)p[-1])) {
				const char *v = p + strlen(names[i]);
				if (*v && *v != ';' && *v != '\r' && *v != '\n') return true;
			}
			p++;
		}
	}
	return false;
}

static void load_session(void) {
	char path[600];
	session_path(path, sizeof(path));
	if (!path[0]) return;
	FILE *f = fopen(path, "r");
	if (!f) return;
	char *line = malloc(YTMUSIC_COOKIE_MAX);
	if (!line) { fclose(f); return; }
	while (fgets(line, YTMUSIC_COOKIE_MAX, f)) {
		char *nl = strpbrk(line, "\r\n");
		if (nl) *nl = '\0';
		// Only the cookie is read. Files written by builds that also kept a
		// Google token have those lines still in them; they are ignored, which
		// is what "the token was never worth keeping" means for an old card.
		if (strncmp(line, "cookie=", 7) == 0 && has_session_cookie(line + 7)) {
			snprintf(session_cookie, sizeof(session_cookie), "%s", line + 7);
		}
	}
	memset(line, 0, YTMUSIC_COOKIE_MAX);
	free(line);
	fclose(f);
}

void ytmusic_init(void) {
	pthread_mutex_lock(&session_lock);
	load_session();
	pthread_mutex_unlock(&session_lock);
	// The songs hearted on this player. Read here because this is where the
	// other YouTube Music state beside device_config.ini is read, and because a
	// heart has to answer at once -- it cannot wait for a page.
	ytlikes_init();
	ytpodcastplayed_init();
}

bool ytmusic_configured(void) {
	const char *from_file = streamkeys_ytmusic_api_key();
	if (from_file && from_file[0]) {
		return true;
	}
	if (YTMUSIC_API_KEY && YTMUSIC_API_KEY[0]) {
		return true;
	}
	const char *cfg_key = config_get("ytmusic", "api_key", "");
	if (cfg_key && cfg_key[0]) {
		return true;
	}
	const char *relay = config_get("ytmusic", "relay_base", "");
	if (relay && relay[0]) {
		return true;
	}
	return false;
}

bool ytmusic_set_session_cookie(const char *cookie, char *error, size_t error_size) {
	if (error && error_size) error[0] = '\0';
	if (!cookie || strnlen(cookie, YTMUSIC_COOKIE_MAX) >= YTMUSIC_COOKIE_MAX ||
		!has_session_cookie(cookie) || strpbrk(cookie, "\r\n")) {
		set_error(error, error_size, "ytmusic_invalid_session");
		return false;
	}
	pthread_mutex_lock(&session_lock);
	char old_cookie[sizeof(session_cookie)];
	snprintf(old_cookie, sizeof(old_cookie), "%s", session_cookie);
	snprintf(session_cookie, sizeof(session_cookie), "%s", cookie);
	if (!save_session()) {
		snprintf(session_cookie, sizeof(session_cookie), "%s", old_cookie);
		memset(old_cookie, 0, sizeof(old_cookie));
		pthread_mutex_unlock(&session_lock);
		set_error(error, error_size, "ytmusic_session_save_failed");
		return false;
	}
	memset(old_cookie, 0, sizeof(old_cookie));
	pthread_mutex_unlock(&session_lock);
	return true;
}

// The card file may be a whole browser export -- a Netscape cookies.txt of a
// few thousand bytes -- while the Cookie header built from it is only the
// YouTube and Google lines, which is why the file has its own, larger limit.
#define YTMUSIC_SESSION_FILE_MAX (64 * 1024)

// A browser's "export cookies" writes a Netscape file -- one tab-separated
// line per cookie -- and that is what people put on the card, because it is the
// only thing their browser will hand them. Reading only a single bare Cookie
// header meant the usual export was refused as "invalid", which looked exactly
// like a session Google had rejected. Both shapes are read now: a Netscape file
// is turned into a Cookie header here, and anything else is taken as one.
static bool cookies_from_netscape(const char *file, char *out, size_t size) {
	size_t used = 0;
	out[0] = '\0';
	char line[1024];
	const char *p = file;
	while (*p) {
		size_t n = 0;
		while (p[n] && p[n] != '\n' && n + 1 < sizeof(line)) n++;
		memcpy(line, p, n);
		line[n] = '\0';
		// Whatever is left of an over-long line is dropped rather than split:
		// half a cookie value is a wrong cookie, not a shorter one.
		while (p[n] && p[n] != '\n') n++;
		p += n;
		if (*p == '\n') p++;
		char *cr = strchr(line, '\r');
		if (cr) *cr = '\0';
		if (line[0] == '#' || line[0] == '\0') continue;
		// domain, flag, path, secure, expiry, name, value
		char *field[7];
		int count = 0;
		char *at = line;
		while (count < 7) {
			field[count++] = at;
			char *tab = strchr(at, '\t');
			if (!tab) break;
			*tab = '\0';
			at = tab + 1;
		}
		if (count < 7) continue;
		if (!strstr(field[0], "youtube.com") && !strstr(field[0], "google.com")) continue;
		if (!field[5][0]) continue;
		int wrote = snprintf(out + used, size - used, "%s%s=%s", used ? "; " : "", field[5], field[6]);
		if (wrote <= 0 || (size_t)wrote >= size - used) return false;
		used += (size_t)wrote;
	}
	return used > 0;
}

bool ytmusic_import_session_file(const char *card_root, char *error, size_t error_size) {
	if (error && error_size) error[0] = '\0';
	if (!card_root || !card_root[0] || strnlen(card_root, 512) >= 512) {
		set_error(error, error_size, "ytmusic_session_file_missing");
		return false;
	}
	char path[600];
	int n = snprintf(path, sizeof(path), "%s/%s", card_root, "ytmusic_session.txt");
	if (n <= 0 || (size_t)n >= sizeof(path)) {
		set_error(error, error_size, "ytmusic_session_file_missing");
		return false;
	}
	struct stat st;
	if (lstat(path, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size <= 0 || st.st_size > YTMUSIC_SESSION_FILE_MAX) {
		set_error(error, error_size, "ytmusic_session_file_invalid");
		return false;
	}
	FILE *f = fopen(path, "r");
	if (!f) {
		set_error(error, error_size, "ytmusic_session_file_unreadable");
		return false;
	}
	char *file = calloc(YTMUSIC_SESSION_FILE_MAX + 1, 1);
	if (!file) {
		fclose(f);
		set_error(error, error_size, "ytmusic_out_of_memory");
		return false;
	}
	size_t got = fread(file, 1, YTMUSIC_SESSION_FILE_MAX, f);
	file[got] = '\0';
	fclose(f);
	bool ok = false;
	if (got > 0 && strchr(file, '\t') &&
		(strstr(file, "youtube.com") || strstr(file, "google.com"))) {
		char *header = calloc(YTMUSIC_COOKIE_MAX, 1);
		if (header) {
			if (cookies_from_netscape(file, header, YTMUSIC_COOKIE_MAX)) {
				ok = ytmusic_set_session_cookie(header, error, error_size);
			}
			memset(header, 0, YTMUSIC_COOKIE_MAX);
			free(header);
		}
	} else {
		char *line_end = strpbrk(file, "\r\n");
		if (line_end) *line_end = '\0';
		char *value = strncmp(file, "cookie=", 7) == 0 ? file + 7 : file;
		// Accept the exact value emitted by browser cookie exporters as well as a
		// bare Cookie header value. Keeping the field name would otherwise result
		// in an invalid nested `Cookie: Cookie: ...` request after import/reboot.
		if (strncasecmp(value, "Cookie:", 7) == 0) {
			value += 7;
			while (*value == ' ' || *value == '\t') value++;
		}
		ok = got > 0 && ytmusic_set_session_cookie(value, error, error_size);
	}
	memset(file, 0, YTMUSIC_SESSION_FILE_MAX);
	free(file);
	if (!ok) {
		if (!error || !error_size || !error[0]) set_error(error, error_size, "ytmusic_session_file_invalid");
		return false;
	}
	// The internal copy is now persisted with mode 0600. Remove the removable
	// card copy as well so it is not left exposed if the card changes hands.
	if (remove(path) != 0) {
		fprintf(stderr, "ytmusic: imported session file could not be removed\n");
	}
	return true;
}

void ytmusic_session_cookie(char *out, size_t size) {
	if (!out || !size) return;
	pthread_mutex_lock(&session_lock);
	snprintf(out, size, "%s", session_cookie);
	pthread_mutex_unlock(&session_lock);
}

bool ytmusic_logout(void) {
	pthread_mutex_lock(&session_lock);
	memset(session_cookie, 0, sizeof(session_cookie));
	char path[600];
	session_path(path, sizeof(path));
	bool ok = !path[0] || remove(path) == 0 || errno == ENOENT;
	pthread_mutex_unlock(&session_lock);
	return ok;
}

// The API key and the client version the request headers and bodies carry.
// Fixed at the first request and never refetched: neither changes while the
// player runs, and the version is today's date at boot.
static bool ensure_context(char *error, size_t error_size) {
	pthread_mutex_lock(&api_lock);
	if (api_key[0] && client_version[0]) {
		pthread_mutex_unlock(&api_lock);
		return true;
	}

	// Where the key comes from: the keys file first -- the image's, or the one
	// at the root of the microSD card, which is how a personal player carries
	// a key without putting it in a build, in an archive or in any repository
	// -- and the value this build was compiled with when the file has none.
	const char *from_file = streamkeys_ytmusic_api_key();
	snprintf(api_key, sizeof(api_key), "%s", (from_file && from_file[0]) ? from_file : YTMUSIC_API_KEY);
	bool have_key = api_key[0] != '\0';
	time_t now = time(NULL);
	struct tm utc;
	if (gmtime_r(&now, &utc)) {
		snprintf(client_version, sizeof(client_version), "1.%04d%02d%02d.01.00", utc.tm_year + 1900,
				 utc.tm_mon + 1, utc.tm_mday);
	} else {
		snprintf(client_version, sizeof(client_version), "%s", YTMUSIC_CLIENT_VERSION_FALLBACK);
	}
	pthread_mutex_unlock(&api_lock);

	// Without a key no InnerTube request can be made at all: the endpoint
	// answers one with no key with the same stub an unknown User-Agent gets,
	// which used to be reported as "no rows" and left the page looking empty.
	// Say which piece is missing, once, and name both ways to supply it.
	if (!have_key) {
		static bool told;
		if (!told) {
			told = true;
			fprintf(stderr, "ytmusic: no InnerTube key -- put api_key under [ytmusic] in "
					"streaming-keys.ini (the microSD card's root or the image's), or "
					"rebuild with YTMUSIC_API_KEY set (see YOUTUBE_MUSIC_SETUP.md)\n");
		}
		set_error(error, error_size, "ytmusic_api_key_missing");
		return false;
	}
	return true;
}

// True when the account is the imported browser session, which is the only
// credential the music endpoints accept for the library pages.
static bool session_cookie_attached(void) {
	char cookie[YTMUSIC_COOKIE_MAX];
	ytmusic_session_cookie(cookie, sizeof(cookie));
	bool has = cookie[0] != '\0';
	memset(cookie, 0, sizeof(cookie));
	return has;
}

bool ytmusic_session_cookie_attached(void) { return session_cookie_attached(); }

// The credential lines for one request.
//
// One credential exists: the browser session imported from the card. InnerTube
// signs a request with it (SAPISIDHASH), and it is the only thing the music
// endpoints accept as an account. There is no Google sign-in any more -- the
// device token it issued was refused by exactly these endpoints, so a build
// that still offered it was offering a switch that could not work.
//
// `allow_account` false leaves the request anonymous, which is what the retry
// in youtubei_post() uses to keep the public pages working.
static bool auth_headers(char *out, size_t size, bool required, bool allow_account,
						 char *error, size_t error_size) {
	char cookie[YTMUSIC_COOKIE_MAX];
	if (!allow_account) {
		if (required) {
			set_error(error, error_size, "ytmusic_library_needs_session");
			return false;
		}
		if (out && size) out[0] = '\0';
		return true;
	}
	ytmusic_session_cookie(cookie, sizeof(cookie));
	if (!cookie[0]) {
		if (required) set_error(error, error_size, "ytmusic_library_needs_session");
		memset(cookie, 0, sizeof(cookie));
		return !required;
	}
	const char *names[] = {"SAPISID", "__Secure-3PAPISID", "__Secure-1PAPISID"};
	char sapisid[512] = "";
	for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
		char name[64];
		snprintf(name, sizeof(name), "%s=", names[i]);
		const char *p = strstr(cookie, name);
		if (!p || (p != cookie && p[-1] != ';' && !isspace((unsigned char)p[-1]))) continue;
		p += strlen(name);
		const char *end = strpbrk(p, "; \t");
		if (!end) end = p + strlen(p);
		if (end > p && (size_t)(end - p) < sizeof(sapisid)) {
			memcpy(sapisid, p, (size_t)(end - p));
			sapisid[end - p] = '\0';
			break;
		}
	}
	if (!sapisid[0]) {
		memset(cookie, 0, sizeof(cookie));
		set_error(error, error_size, "ytmusic_invalid_session");
		return false;
	}
	char input[1024], hash[SHA1_HEX_LEN];
	long stamp = (long)time(NULL);
	snprintf(input, sizeof(input), "%ld %s %s", stamp, sapisid, YTMUSIC_SITE);
	sha1_hex(input, strlen(input), hash);
	int n = snprintf(out, size,
		"Cookie: %s\r\nAuthorization: SAPISIDHASH %ld_%s\r\nOrigin: %s\r\nReferer: %s/\r\n",
		cookie, stamp, hash, YTMUSIC_SITE, YTMUSIC_SITE);
	memset(cookie, 0, sizeof(cookie));
	memset(sapisid, 0, sizeof(sapisid));
	memset(input, 0, sizeof(input));
	return n > 0 && (size_t)n < size;
}

static bool json_escape(const char *in, char *out, size_t size) {
	if (!in || !out || size < 3) return false;
	size_t used = 0;
	for (const unsigned char *p = (const unsigned char *)in; *p; p++) {
		const char *escape = NULL;
		char simple[2] = {(char)*p, '\0'};
		switch (*p) {
		case '"': escape = "\\\""; break;
		case '\\': escape = "\\\\"; break;
		case '\b': escape = "\\b"; break;
		case '\f': escape = "\\f"; break;
		case '\n': escape = "\\n"; break;
		case '\r': escape = "\\r"; break;
		case '\t': escape = "\\t"; break;
		default: if (*p < 0x20) return false;
		}
		const char *s = escape ? escape : simple;
		size_t n = strlen(s);
		if (n >= size - used) return false;
		memcpy(out + used, s, n);
		used += n;
	}
	out[used] = '\0';
	return true;
}

// Some Google endpoints protect JSON replies with the standard anti-XSSI
// prefix. The YouTube Music API usually omits it, but tolerate it here so a
// harmless transport wrapper does not turn a valid search reply into a parse
// error.
static const char *youtubei_json_start(const char *body) {
	if (!body) return NULL;
	while (*body == ' ' || *body == '\t' || *body == '\r' || *body == '\n') body++;
	if (strncmp(body, ")]}'", 4) == 0) {
		body += 4;
		if (*body == ',') body++;
		while (*body == ' ' || *body == '\t' || *body == '\r' || *body == '\n') body++;
	}
	return body;
}

// Whether a JSON body ended like one: the last non-space byte of any complete
// JSON document is '}' or ']'. The InnerTube request goes out with
// "Connection: close", and a reply delimited by the closing connection cannot
// be told from one cut off by a dropping Wi-Fi link -- several hundred
// kilobytes arrive, the last kilobyte does not, and the page parses into
// nothing at all. The body itself is the only witness.
static bool response_is_complete(const char *body) {
	if (!body) {
		return false;
	}
	size_t n = strlen(body);
	while (n > 0 && (body[n - 1] == ' ' || body[n - 1] == '\t' || body[n - 1] == '\r' || body[n - 1] == '\n')) {
		n--;
	}
	return n > 0 && (body[n - 1] == '}' || body[n - 1] == ']');
}

// One InnerTube POST. `force_anonymous` leaves the account out of the request
// altogether: a caller uses it to ask the same public question the signed-out
// player asks, after the account's own answer came back with no rows in it.
static bool youtubei_post_ex(const char *endpoint, const char *body, bool login_required,
							 bool force_anonymous, char **response, int *status,
							 char *error, size_t error_size) {
	pthread_mutex_lock(&request_lock);
	if (!ensure_context(error, error_size)) {
		pthread_mutex_unlock(&request_lock);
		return false;
	}
	char key[sizeof(api_key)], version[sizeof(client_version)];
	pthread_mutex_lock(&api_lock);
	snprintf(key, sizeof(key), "%s", api_key);
	snprintf(version, sizeof(version), "%s", client_version);
	pthread_mutex_unlock(&api_lock);
	char url[512], headers[YTMUSIC_COOKIE_MAX + 1024];
	snprintf(url, sizeof(url), "%s/youtubei/v1/%s?key=%s&prettyPrint=false", YTMUSIC_SITE, endpoint, key);
	snprintf(headers, sizeof(headers), "Content-Type: application/json\r\nOrigin: %s\r\nReferer: %s/\r\nX-YouTube-Client-Name: 67\r\nX-YouTube-Client-Version: %s\r\n", YTMUSIC_SITE, YTMUSIC_SITE, version);
	char request_body[4096];
	const char *stale = "1.20250101.00.00";
	const char *at = strstr(body, stale);
	if (at) {
		size_t before = (size_t)(at - body);
		size_t stale_len = strlen(stale), version_len = strlen(version), tail = strlen(at + stale_len);
		if (before + version_len + tail + 1 > sizeof(request_body)) {
			set_error(error, error_size, "ytmusic_request_too_large");
			pthread_mutex_unlock(&request_lock);
			return false;
		}
		memcpy(request_body, body, before);
		memcpy(request_body + before, version, version_len);
		memcpy(request_body + before + version_len, at + stale_len, tail + 1);
	} else {
		if (strlen(body) >= sizeof(request_body)) {
			set_error(error, error_size, "ytmusic_request_too_large");
			pthread_mutex_unlock(&request_lock);
			return false;
		}
		snprintf(request_body, sizeof(request_body), "%s", body);
	}
	http_req_t req = {.method = "POST", .extra_headers = headers, .body = request_body,
		.content_type = "application/json", .want_error_body = true, .no_https_downgrade = true};
	int status_code = 0;
	bool ok = false;
	bool retried_truncated = false;
	bool anonymous = false;
	// At most three passes: with the session, without it, and one more for a
	// body that arrived truncated. A page that does not need an account is
	// never allowed to fail just because the session was refused -- that is
	// what made search come back empty, and Top Songs report a refusal, for
	// anyone who had imported one.
	for (int attempt = 0; attempt < 3; attempt++) {
		size_t used = strlen(headers);
		bool want_account = !anonymous && !force_anonymous;
		if (!auth_headers(headers + used, sizeof(headers) - used, login_required && want_account,
						  want_account, error, error_size)) {
			memset(headers, 0, sizeof(headers));
			memset(request_body, 0, sizeof(request_body));
			pthread_mutex_unlock(&request_lock);
			return false;
		}
		if (!want_account) {
			// The consent wall: from a European address Google answers the
			// public pages with framework JSON that carries no shelves at all
			// -- hundreds of kilobytes in, zero rows out, which is what an
			// empty search page was. These are the cookies the interstitial
			// sets on acceptance; where no wall stands they are ignored.
			static const char consent[] = "Cookie: SOCS=CAI; CONSENT=YES+cb\r\n";
			size_t room = sizeof(headers) - strlen(headers) - 1;
			strncat(headers, consent, room);
		}
		ok = http_request(url, &req, response, NULL, YTMUSIC_RESPONSE_LIMIT, YTMUSIC_TIMEOUT, &status_code);
		if (!ok) break;
		if (status_code >= 200 && status_code < 300) {
			// The socket cannot tell a finished close-delimited reply from one
			// cut off halfway, so the body is asked instead. An incomplete one
			// is fetched once more before it is given up on: the pages are
			// read-only and idempotent, and half an Explore page is worth less
			// than none, because it parses into an empty screen with no error.
			if (!response_is_complete(*response)) {
				if (response && *response) {
					wipe_free(*response);
					*response = NULL;
				}
				if (!retried_truncated) {
					retried_truncated = true;
					memset(headers + used, 0, sizeof(headers) - used);
					continue;
				}
				ok = false;
				set_error(error, error_size, "ytmusic_connection_cut");
				break;
			}
			break;
		}

		// Not a 2xx. The reply body is of no use to the caller, and the next
		// attempt would otherwise leak it.
		if (response && *response) {
			wipe_free(*response);
			*response = NULL;
		}

		// Google refused the session (an expired or logged-out cookie). Try the
		// same read without it: the catalogue pages are public, and a refusal is
		// not a reason to show the user nothing.
		if (!anonymous && !login_required && !force_anonymous && session_cookie_attached()) {
			anonymous = true;
			memset(headers + used, 0, sizeof(headers) - used);
			continue;
		}
		break;
	}
	memset(headers, 0, sizeof(headers));
	memset(request_body, 0, sizeof(request_body));
	if (!ok) {
		set_error(error, error_size, http_last_error() ? http_last_error() : "ytmusic_no_reply");
		pthread_mutex_unlock(&request_lock);
		return false;
	}
	if (status) *status = status_code;
	if (status_code < 200 || status_code >= 300) {
		set_error(error, error_size, status_code == 401 || status_code == 403 ? "ytmusic_session_expired" : "ytmusic_request_refused");
		free(*response);
		*response = NULL;
		pthread_mutex_unlock(&request_lock);
		return false;
	}
	pthread_mutex_unlock(&request_lock);
	return true;
}

static bool youtubei_post(const char *endpoint, const char *body, bool login_required,
						  char **response, int *status, char *error, size_t error_size) {
	return youtubei_post_ex(endpoint, body, login_required, false, response, status, error, error_size);
}

// The phone app's player endpoint. It is what resolves the media bytes: the
// web music client answers with a ciphered stream the device cannot solve (it
// has no JavaScript engine), and it wants a signed-in session even to try. This
// one answers with plain https URLs and needs no account, so a track plays
// whether or not anybody has signed in.
//
// ANDROID_VR used to be the direct-URL client of choice; Google has since put
// it behind a "sign in to confirm you're not a bot" check that answers no
// streaming data at all, which is why playback stopped working.
#define YTMUSIC_ANDROID_VERSION "20.10.38"
#define YTMUSIC_ANDROID_SDK 30

// ---------------------------------------------------------------------------
// the market the catalogue answers in
// ---------------------------------------------------------------------------

// YouTube Music chooses what to recommend -- Explore, Top songs, the charts --
// from `gl`, the country's store, and `hl`, the language its labels come back
// in. Both come from the one region choice the whole player shares (region.h):
// Poland answers Polish, and Worldwide answers the pair a request with no
// preference gets, US and en.
static void catalogue_bounds(char *gl, size_t gl_size, char *hl, size_t hl_size) {
	region_market(gl, gl_size, hl, hl_size);
}

// Whether the best format offered is wanted, or the safe one. 140 is the
// 128 kbit/s AAC every instance carries; 141 is the same codec at 256, which
// sounds markedly better on anything but earbuds and costs twice the bytes over
// a public instance. High is the default and Standard is the way back when a
// download over a busy instance is too slow to start.
static bool high_quality_wanted(void) {
	const char *choice = config_get("ytmusic", "quality", "high");
	return !choice || strcmp(choice, "standard") != 0;
}

bool ytmusic_quality_high(void) { return high_quality_wanted(); }

void ytmusic_quality_set(bool high) {
	config_set("ytmusic", "quality", high ? "high" : "standard");
	config_save();
}

// The region row draws the shared list: the same Worldwide-first options the
// Tidal, Podcasts and Radio settings pages offer (region.h), in the same
// order and with the same default. Only the pair this endpoint wants is worked
// out here -- one entry of that list becomes the `gl` / `hl` a request carries.
int ytmusic_regions(ytmusic_region_t *out, int limit) {
	region_t list[REGION_LIST_MAX];
	int have = region_list(list, REGION_LIST_MAX);
	if (!out || limit <= 0) return 0;
	int count = have < limit ? have : limit;
	for (int i = 0; i < count; i++) {
		char market[3], language[3];
		snprintf(market, sizeof(market), "%s", list[i].country[0] ? list[i].country : "US");
		snprintf(language, sizeof(language), "%s", list[i].language[0] ? list[i].language : "en");
		snprintf(out[i].region, sizeof(out[i].region), "%s", market);
		snprintf(out[i].language, sizeof(out[i].language), "%s", language);
		// A tag is put through tr() where the row is drawn; a language's own
		// name has no translation to look for and comes back unchanged.
		snprintf(out[i].label, sizeof(out[i].label), "%s", list[i].label);
	}
	return count;
}

void ytmusic_region_active(char *region, size_t region_size, char *language, size_t language_size) {
	region_market(region, region_size, language, language_size);
}

bool ytmusic_region_select(const char *region, const char *language) {
	if (!region || !language) return false;
	region_t list[REGION_LIST_MAX];
	int count = region_list(list, REGION_LIST_MAX);
	// The pair the caller holds is the pair this entry presents, Worldwide
	// included (US / en), so the row that was tapped is the row that sticks.
	for (int i = 0; i < count; i++) {
		char market[3], language_code[3];
		snprintf(market, sizeof(market), "%s", list[i].country[0] ? list[i].country : "US");
		snprintf(language_code, sizeof(language_code), "%s", list[i].language[0] ? list[i].language : "en");
		if (strcmp(market, region) == 0 && strcmp(language_code, language) == 0) {
			return region_select(i);
		}
	}
	return false;
}

static bool android_player(const char *video_id, char **response, char *error, size_t error_size) {
	if (!ensure_context(error, error_size)) return false;
	char key[sizeof(api_key)];
	pthread_mutex_lock(&api_lock);
	snprintf(key, sizeof(key), "%s", api_key);
	pthread_mutex_unlock(&api_lock);
	char url[512], body[1024], gl[3], hl[3];
	catalogue_bounds(gl, sizeof(gl), hl, sizeof(hl));
	snprintf(url, sizeof(url), "https://www.youtube.com/youtubei/v1/player?key=%s&prettyPrint=false", key);
	snprintf(body, sizeof(body),
		"{\"context\":{\"client\":{\"clientName\":\"ANDROID\",\"clientVersion\":\"%s\","
		"\"androidSdkVersion\":%d,\"osName\":\"Android\",\"osVersion\":\"11\","
		"\"hl\":\"%s\",\"gl\":\"%s\"}},\"videoId\":\"%s\","
		"\"contentCheckOk\":true,\"racyCheckOk\":true}",
		YTMUSIC_ANDROID_VERSION, YTMUSIC_ANDROID_SDK, hl, gl, video_id);
	const char *headers = "Content-Type: application/json\r\nOrigin: https://www.youtube.com\r\nReferer: https://www.youtube.com/\r\n"
						  "X-YouTube-Client-Name: 3\r\nX-YouTube-Client-Version: " YTMUSIC_ANDROID_VERSION "\r\n";
	http_req_t req = {.method = "POST", .extra_headers = headers, .body = body,
		.content_type = "application/json", .want_error_body = true, .no_https_downgrade = true};
	int status = 0;
	pthread_mutex_lock(&request_lock);
	bool ok = http_request(url, &req, response, NULL, YTMUSIC_RESPONSE_LIMIT, YTMUSIC_TIMEOUT, &status);
	pthread_mutex_unlock(&request_lock);
	if (!ok) {
		set_error(error, error_size, http_last_error() ? http_last_error() : "ytmusic_no_reply");
		return false;
	}
	if (status < 200 || status >= 300) {
		free(*response);
		*response = NULL;
		set_error(error, error_size, "ytmusic_stream_refused");
		return false;
	}
	return true;
}

// The relay: a Piped-API-compatible server (GET /streams/{videoId}).
//
// Google now demands a browser-grade attestation -- a PO token its JavaScript
// computes -- from every first-party client before it hands out media URLs.
// The phone-app client answers "precondition check failed", the web music
// player "the page needs to be reloaded", the VR client "sign in to confirm
// you're not a bot", all without giving one byte of streaming data. This
// device has no JavaScript engine and no way to run that attestation, so a
// direct guest stream from YouTube is no longer possible from here at all.
//
// A relay is a small self-hostable service (Piped, Invidious, or anything
// that answers the same two endpoints) that runs the attestation on its own
// connection and passes plain audio URLs back. It is a URL in the settings,
// not a built-in host: public instances appear and die weekly, and pointing
// every shipped player at one would break it the day it does. Search and
// browsing need none of this -- the web music client still answers those
// signed out, which is why only playback asks for a relay.
//
// Only the audio streams are read, and only the AAC-in-MP4 ones: the player
// decodes AAC in an MP4 container, its Opus decoder reads Ogg and not the
// WebM YouTube wraps Opus in, and the video streams are never fetched at all.
#define YTMUSIC_RELAY_MAX_KBPS 320

// A relay that resolves with yt-dlp has to run it before it can answer, and a
// cold run fetches the player script and solves a signature first: fifteen
// seconds is enough for a request to Google and not for that. The relay calls
// get their own, longer one; everything else keeps the short one.
#define YTMUSIC_RELAY_TIMEOUT 60

// The relay's own address, without the trailing slashes the setting may carry.
// False when nothing usable is configured -- no setting, or not an address.
static bool relay_base_address(char *out, size_t size) {
	const char *configured = config_get("ytmusic", "relay_base", "");
	if (!configured[0]) {
		return false;
	}
	snprintf(out, size, "%s", configured);
	size_t len = strlen(out);
	while (len > 0 && out[len - 1] == '/') out[--len] = '\0';
	// http:// is accepted as well as https://: a relay is often a machine on
	// the same LAN, where there is no certificate to serve.
	if (!len || (strncmp(out, "https://", 8) != 0 && strncmp(out, "http://", 7) != 0)) {
		out[0] = '\0';
		return false;
	}
	return true;
}

// Where the player fetches a track's bytes from.
//
// A relay answers with a media URL, and the ones a Piped or Invidious instance
// answers with point at Google's own servers. Those cannot be fetched from here
// at all: the address is bound to the machine that resolved it -- the same
// request from a different client on a different address is answered 403 -- and
// it is several thousand characters long, past this player's URL buffer, so
// what would be requested is a truncated address that is refused without saying
// why. That is what "could not reach the stream" used to mean, and it left no
// trace anywhere.
//
// So the bytes are always fetched from the relay itself: from the URL it
// answered with when that URL is its own address, and from /audio/<video-id>
// -- the endpoint tools/ytmusic-relay.py serves -- when it is somebody else's.
// A relay that does neither is reported as such instead of having its address
// quietly fetched and refused.
//
// `itag` is the format the reply was read for, carried into the proxied address
// so the relay serves that one rather than choosing again -- the choice of
// m4a/AAC under the bitrate cap is this end's, and it should stay that way.
//
// `proxied` (optional) says the second path was taken: the one that needs the
// relay to serve the bytes itself.
bool ytmusic_relay_stream_url(const char *base, const char *video_id, long itag, const char *reported,
							  char *out, size_t out_size, bool *proxied) {
	if (proxied) {
		*proxied = false;
	}
	if (!base || !base[0] || !video_id || strlen(video_id) != 11 || !out || out_size == 0) {
		return false;
	}
	size_t base_len = strlen(base);
	if (reported && reported[0] && strncmp(reported, base, base_len) == 0 &&
		(reported[base_len] == '\0' || reported[base_len] == '/' || reported[base_len] == '?')) {
		snprintf(out, out_size, "%s", reported);
		return true;
	}
	int built = itag > 0 ? snprintf(out, out_size, "%s/audio/%.11s?f=%ld", base, video_id, itag)
						 : snprintf(out, out_size, "%s/audio/%.11s", base, video_id);
	if (built < 0 || built >= (int)out_size) {
		out[0] = '\0';
		return false;
	}
	if (proxied) {
		*proxied = true;
	}
	return true;
}

// ---------------------------------------------------------------------------
// Instances
// ---------------------------------------------------------------------------
//
// A Piped or Invidious instance resolves a track on a machine that can do the
// attestation this device cannot. What it hands back decides whether that is
// any use here:
//
//   * Piped answers /streams/{id} with audioStreams whose URLs point at
//     Google's own hosts. Those are bound to the address that resolved them,
//     so fetching one from here is refused; a Piped instance only helps when
//     it fronts the bytes itself, and then its URL already points at it.
//   * Invidious answers /api/v1/videos/{id}, and has /latest_version?id=...
//     &itag=...&local=true for the bytes: the instance fetches the audio and
//     passes it on, which is the one shape that works from another machine.
//     That is what is asked for once the formats reply names an itag.
//
// Public instances come and go, and the survivors are counted in single
// digits: when this list was last measured against a real track, one public
// host answered /api/v1/videos and then served actual audio bytes through
// /latest_version?local=true, while every Piped host on the published list
// answered 502, 403 or nothing at all. So the list starts with the host that
// was measured, and the settings page can pin another (a self-hosted instance
// is the version of this that keeps working).
static const char *const builtin_instances[] = {
	"https://yt.chocolatemoo53.com",
	"https://inv.nadeko.net",
	"https://invidious.nerdvpn.de",
	"https://invidious.tiekoetter.com",
	// Piped hosts answer this player's API shape when they are up; every one
	// named on the official list was down when the hosts above were measured,
	// so they sit after the hosts that were not.
	"https://pipedapi.kavin.rocks",
	"https://pipedapi.adminforge.de",
};

// One instance request, not a page: the ones that are down fail on the
// connection, and the ones that answer do it quickly. Waiting the relay's
// minute on each would turn a failed track into a two-minute stall.
#define YTMUSIC_INSTANCE_TIMEOUT 12
// How many of the built-in hosts are tried before the player gives up: they
// are guesses, and a play attempt should not wait on six of them.
#define YTMUSIC_INSTANCE_TRIES 4
// Room for the [ytmusic] instances line, which lists hosts by hand.
#define YTMUSIC_INSTANCE_LIST_MAX 512

// A base address as the player needs it: scheme kept, trailing slashes gone.
// False when it is not an http(s) address at all, which is what a bare host
// name typed into the settings is.
static bool normalise_instance(const char *in, char *out, size_t size) {
	if (!in || !out || size < 16) return false;
	while (*in == ' ' || *in == '\t') in++;
	if (!in[0]) return false;
	// A bare host is what people type -- "invidious.example" -- and https is
	// what every instance serves, so it is filled in rather than refused.
	bool scheme = strncmp(in, "https://", 8) == 0 || strncmp(in, "http://", 7) == 0;
	int n = scheme ? snprintf(out, size, "%s", in) : snprintf(out, size, "https://%s", in);
	if (n <= 0 || (size_t)n >= size) {
		out[0] = '\0';
		return false;
	}
	size_t len = strlen(out);
	while (len > 8 && out[len - 1] == '/') out[--len] = '\0';
	// A host name is all that may follow the scheme: no path, no space.
	const char *host = out + (strncmp(out, "https://", 8) == 0 ? 8 : 7);
	return host[0] != '\0' && strpbrk(host, " \t/?#") == NULL;
}

// The instance a choice pins, or empty for the three built-in answers.
static bool pinned_instance(char *out, size_t size) {
	const char *active = config_get("ytmusic", "source", "auto");
	if (!active[0] || strcmp(active, "auto") == 0 || strcmp(active, "direct") == 0 ||
		strcmp(active, "relay") == 0) {
		return false;
	}
	return normalise_instance(active, out, size);
}

static bool instance_add(char out[][YTMUSIC_SOURCE_MAX], int *count, int limit, const char *base) {
	char normalised[YTMUSIC_SOURCE_MAX];
	if (*count >= limit || !normalise_instance(base, normalised, sizeof(normalised))) return false;
	for (int i = 0; i < *count; i++) {
		if (strcmp(out[i], normalised) == 0) return false;
	}
	snprintf(out[(*count)++], YTMUSIC_SOURCE_MAX, "%s", normalised);
	return true;
}

int ytmusic_instance_bases(char out[][YTMUSIC_SOURCE_MAX], int limit) {
	int count = 0;
	char pinned[YTMUSIC_SOURCE_MAX];
	if (pinned_instance(pinned, sizeof(pinned))) instance_add(out, &count, limit, pinned);
	// [ytmusic] instances is a comma or space separated list, and replaces the
	// built-in hosts rather than adding to them: someone who has a working
	// instance does not want six dead ones tried before it.
	const char *configured = config_get("ytmusic", "instances", "");
	if (configured[0]) {
		char list[YTMUSIC_INSTANCE_LIST_MAX];
		snprintf(list, sizeof(list), "%s", configured);
		for (char *token = strtok(list, " ,;\t\r\n"); token; token = strtok(NULL, " ,;\t\r\n")) {
			instance_add(out, &count, limit, token);
		}
		return count;
	}
	for (size_t i = 0; i < sizeof(builtin_instances) / sizeof(builtin_instances[0]); i++) {
		instance_add(out, &count, limit, builtin_instances[i]);
	}
	return count;
}

// The stored choice, "auto" when nothing was ever chosen.
static const char *source_value(void) {
	const char *active = config_get("ytmusic", "source", "auto");
	return active[0] ? active : "auto";
}

int ytmusic_sources(ytmusic_source_t *out, int limit) {
	if (!out || limit <= 0) return 0;
	int count = 0;
	static const struct {
		const char *value;
		const char *label;
	} built_in[] = {
		{"auto", "ytmusic_source_auto"},
		{"direct", "ytmusic_source_direct"},
		{"relay", "ytmusic_source_relay"},
	};
	for (size_t i = 0; i < sizeof(built_in) / sizeof(built_in[0]) && count < limit; i++) {
		// The relay is only a choice once there is one configured: a row that
		// cannot play anything is worse than no row.
		if (strcmp(built_in[i].value, "relay") == 0) {
			char base[256];
			bool pinned = strcmp(source_value(), "relay") == 0;
			if (!relay_base_address(base, sizeof(base)) && !pinned) continue;
			snprintf(out[count].value, YTMUSIC_SOURCE_MAX, "%s", built_in[i].value);
			snprintf(out[count].label, YTMUSIC_SOURCE_MAX, "%s", built_in[i].label);
			count++;
			continue;
		}
		snprintf(out[count].value, YTMUSIC_SOURCE_MAX, "%s", built_in[i].value);
		snprintf(out[count].label, YTMUSIC_SOURCE_MAX, "%s", built_in[i].label);
		count++;
	}
	char bases[YTMUSIC_SOURCES_MAX][YTMUSIC_SOURCE_MAX];
	int instances = ytmusic_instance_bases(bases, YTMUSIC_SOURCES_MAX);
	for (int i = 0; i < instances && count < limit; i++) {
		const char *host = strstr(bases[i], "://");
		snprintf(out[count].value, YTMUSIC_SOURCE_MAX, "%.*s", YTMUSIC_SOURCE_MAX - 1, bases[i]);
		snprintf(out[count].label, YTMUSIC_SOURCE_MAX, "%.*s", YTMUSIC_SOURCE_MAX - 1,
				 host ? host + 3 : bases[i]);
		count++;
	}
	return count;
}

void ytmusic_source_active(char *out, size_t size) {
	if (!out || !size) return;
	snprintf(out, size, "%s", source_value());
}

bool ytmusic_source_select(const char *value) {
	if (!value || !value[0]) return false;
	char chosen[YTMUSIC_SOURCE_MAX];
	if (strcmp(value, "auto") == 0 || strcmp(value, "direct") == 0 || strcmp(value, "relay") == 0) {
		snprintf(chosen, sizeof(chosen), "%s", value);
	} else if (!normalise_instance(value, chosen, sizeof(chosen))) {
		snprintf(chosen, sizeof(chosen), "%s", value);
		return false;
	}
	config_set("ytmusic", "source", chosen);
	// The choice is in force either way; a settings file that cannot be
	// written only means it will not survive a reboot, which is not a reason
	// to refuse it now.
	if (!config_save()) {
		fprintf(stderr, "ytmusic: the source could not be saved, kept for this session\n");
	}
	return true;
}

static bool relay_audio_url(const char *video_id, char *url, size_t url_size,
							int *duration, char *title, size_t title_size,
							char *artist, size_t artist_size, bool *proxied,
							char *error, size_t error_size) {
	if (proxied) *proxied = false;
	char base[256];
	if (!relay_base_address(base, sizeof(base))) {
		// No relay is not a missing setup step any more: the device plays a
		// track by itself, and an instance is the other way in. What is left
		// to say is that every one of them refused.
		set_error(error, error_size, "ytmusic_no_source");
		return false;
	}

	char endpoint[320];
	snprintf(endpoint, sizeof(endpoint), "%s/streams/%.11s", base, video_id);
	char *body = NULL;
	int relay_status = 0;
	pthread_mutex_lock(&request_lock);
	// The body of a refusal is kept: it is where the relay says why, and that
	// belongs in the log rather than in the guess of "no reply".
	bool got = http_get_ex(endpoint, NULL, true, &body, NULL, YTMUSIC_RESPONSE_LIMIT,
						   YTMUSIC_RELAY_TIMEOUT, &relay_status);
	pthread_mutex_unlock(&request_lock);
	if (!got) {
		fprintf(stderr, "ytmusic: relay did not answer at %.64s (%s)\n", endpoint,
				http_last_error() ? http_last_error() : "no reason given");
		set_error(error, error_size, "ytmusic_relay_unreachable");
		return false;
	}
	if (relay_status >= 400) {
		// A relay that does not know /streams/{id} is not the kind of relay
		// this player can use, whatever else it answers: Invidious serves its
		// own API shape, not this one. Its reply says which it is, in the log.
		fprintf(stderr, "ytmusic: relay answered %d for %.11s: %.160s\n", relay_status, video_id,
				body ? body : "");
		free(body);
		set_error(error, error_size,
				  relay_status == 404 || relay_status == 405 ? "ytmusic_relay_not_serving"
															 : "ytmusic_relay_unreadable");
		return false;
	}

	json_doc_t doc;
	bool ok = false;
	if (json_parse_limited(youtubei_json_start(body), &doc, 32000)) {
		int root = json_root(&doc);
		int streams = json_get(&doc, root, "audioStreams");
		int best = -1;
		long best_bitrate = -1;
		for (int i = 0; i < json_len(&doc, streams); i++) {
			int stream = json_at(&doc, streams, i);
			char mime[96] = "", candidate[YTMUSIC_URL_MAX] = "";
			json_obj_str(&doc, stream, "mimeType", mime, sizeof(mime));
			if (strncasecmp(mime, "audio/mp4", 9) != 0) continue;
			if (!json_obj_str(&doc, stream, "url", candidate, sizeof(candidate)) || !candidate[0]) continue;
			long bitrate = json_obj_long(&doc, stream, "bitrate", 0);
			if (bitrate <= 0 || bitrate > YTMUSIC_RELAY_MAX_KBPS * 1000L) continue;
			if (bitrate > best_bitrate) {
				best = stream;
				best_bitrate = bitrate;
			}
		}
		if (best >= 0 && json_obj_str(&doc, best, "url", url, url_size) && url[0]) {
			// The address the relay answered with is only worth fetching when it
			// is the relay's own; see ytmusic_relay_stream_url(). This is the
			// one place that rewrite belongs: an address the device resolved
			// itself, through a signed-in session, is fetchable as it stands
			// and is left alone.
			char chosen[YTMUSIC_URL_MAX];
			bool through_relay = false;
			long itag = json_obj_long(&doc, best, "itag", 0);
			if (ytmusic_relay_stream_url(base, video_id, itag, url, chosen, sizeof(chosen),
										&through_relay) &&
				through_relay) {
				snprintf(url, url_size, "%s", chosen);
				printf("ytmusic: %.11s: the relay's own address is elsewhere, so its /audio endpoint is used\n",
					   video_id);
			}
			if (proxied) *proxied = through_relay;
			ok = true;
			resolved_bitrate_kbps = (int)(best_bitrate / 1000);
			if (duration) *duration = (int)json_obj_long(&doc, root, "duration", 0);
			if (title && title_size) json_obj_str(&doc, root, "title", title, title_size);
			if (artist && artist_size) json_obj_str(&doc, root, "uploader", artist, artist_size);
		} else {
			set_error(error, error_size, "ytmusic_aac_stream_unavailable");
		}
		json_free(&doc);
	} else {
		set_error(error, error_size, "ytmusic_relay_unreadable");
	}
	free(body);
	if (ok) {
		printf("ytmusic: relay served an audio stream for %.11s\n", video_id);
	}
	return ok;
}

// One instance, asked for the formats of a track and turned into an address
// this player can fetch. Two API shapes are tried, because a Piped host and an
// Invidious host answer different ones and either kind can be pinned:
//
//   Piped     /streams/{id}                 audioStreams[].url
//   Invidious /api/v1/videos/{id}           adaptiveFormats[].itag
//
// The second only turns into bytes through /latest_version?local=true, which
// is why the itag is what is carried out of that reply rather than its URL.
static bool instance_audio_url(const char *base, const char *video_id, char *url, size_t url_size,
							   int *duration, char *title, size_t title_size,
							   char *artist, size_t artist_size, bool *proxied,
							   uint64_t *expected_bytes, char *error, size_t error_size) {
	if (proxied) *proxied = false;
	if (expected_bytes) *expected_bytes = 0;
	if (!base || !base[0] || !video_id || strlen(video_id) != 11) return false;
	size_t base_len = strlen(base);
	char endpoint[YTMUSIC_SOURCE_MAX + 160], *body = NULL;
	int status = 0;
	// Whether the host answered at all, which decides which failure to name:
	// "no playable audio" is only true of a host that replied.
	bool answered = false;

	// Piped first: it is the shape the relay already speaks, and the one a
	// Piped instance answers without a query string of its own.
	snprintf(endpoint, sizeof(endpoint), "%s/streams/%.11s", base, video_id);
	pthread_mutex_lock(&request_lock);
	bool got = http_get_ex(endpoint, NULL, true, &body, NULL, YTMUSIC_RESPONSE_LIMIT,
						   YTMUSIC_INSTANCE_TIMEOUT, &status);
	pthread_mutex_unlock(&request_lock);
	if (got && body && status < 400) {
		answered = true;
		json_doc_t doc;
		if (json_parse_limited(youtubei_json_start(body), &doc, 128 * 1024)) {
			int root = json_root(&doc);
			int streams = json_get(&doc, root, "audioStreams");
			int best = -1;
			long best_rate = -1;
			for (int i = 0; i < json_len(&doc, streams); i++) {
				int stream = json_at(&doc, streams, i);
				char mime[96] = "", candidate[YTMUSIC_URL_MAX] = "";
				json_obj_str(&doc, stream, "mimeType", mime, sizeof(mime));
				if (strncasecmp(mime, "audio/mp4", 9) != 0) continue;
				if (!json_obj_str(&doc, stream, "url", candidate, sizeof(candidate)) || !candidate[0]) continue;
				long rate = json_obj_long(&doc, stream, "bitrate", 0);
				if (rate <= 0 || rate > YTMUSIC_RELAY_MAX_KBPS * 1000L) continue;
				if (rate > best_rate) {
					best = stream;
					best_rate = rate;
				}
			}
			if (best >= 0) {
				char candidate[YTMUSIC_URL_MAX] = "";
				if (json_obj_str(&doc, best, "url", candidate, sizeof(candidate)) && candidate[0] &&
					strncmp(candidate, base, base_len) == 0 &&
					(candidate[base_len] == '/' || candidate[base_len] == '?' ||
					 candidate[base_len] == '\0')) {
					// The instance fronts the bytes itself, which is the only kind
					// of address from somebody else this player can fetch.
					snprintf(url, url_size, "%s", candidate);
					resolved_bitrate_kbps = (int)(best_rate / 1000);
					if (duration) *duration = (int)json_obj_long(&doc, root, "duration", 0);
					if (title && title_size) json_obj_str(&doc, root, "title", title, title_size);
					if (artist && artist_size) json_obj_str(&doc, root, "uploader", artist, artist_size);
					json_free(&doc);
					free(body);
					return true;
				}
			}
			json_free(&doc);
		}
	}
	free(body);
	body = NULL;

	// Invidious: the formats reply names the itag, and /latest_version with
	// local=true is the endpoint that fetches and passes the audio on.
	snprintf(endpoint, sizeof(endpoint), "%s/api/v1/videos/%.11s?fields=title,author,lengthSeconds,adaptiveFormats",
			 base, video_id);
	status = 0;
	pthread_mutex_lock(&request_lock);
	got = http_get_ex(endpoint, NULL, true, &body, NULL, YTMUSIC_RESPONSE_LIMIT, YTMUSIC_INSTANCE_TIMEOUT,
					  &status);
	pthread_mutex_unlock(&request_lock);
	bool ok = false;
	if (got && body && status < 400) {
		answered = true;
		json_doc_t doc;
		if (json_parse_limited(youtubei_json_start(body), &doc, YTMUSIC_RESPONSE_LIMIT)) {
			int root = json_root(&doc);
			int formats = json_get(&doc, root, "adaptiveFormats");
			int best = -1;
			long best_rate = -1;
			for (int i = 0; i < json_len(&doc, formats); i++) {
				int format = json_at(&doc, formats, i);
				char mime[96] = "";
				json_obj_str(&doc, format, "type", mime, sizeof(mime));
				if (strncasecmp(mime, "audio/mp4", 9) != 0) continue;
				long itag = json_obj_long(&doc, format, "itag", 0);
				if (itag <= 0) continue;
				// Past the cap the device is decoding audio it cannot hear
				// anyway. High takes the best the instance offers -- 141 is 256
				// kbit/s AAC where it exists; Standard pins 140, the 128 kbit/s
				// format every instance carries.
				long rate = json_obj_long(&doc, format, "bitrate", 0);
				if (rate <= 0 || rate > YTMUSIC_RELAY_MAX_KBPS * 1000L) continue;
				long score = high_quality_wanted() ? rate : (itag == 140 ? (1L << 30) : rate);
				if (score > best_rate) {
					best = format;
					best_rate = score;
				}
			}
			if (best >= 0) {
				long itag = json_obj_long(&doc, best, "itag", 0);
				int built = snprintf(url, url_size, "%s/latest_version?id=%.11s&itag=%ld&local=true",
									 base, video_id, itag);
				if (built > 0 && (size_t)built < url_size) {
					ok = true;
					if (proxied) *proxied = true;
					// What the whole stream is, from the instance's own reply: the
					// downloader checks the file against it, so a transfer that
					// stopped halfway cannot pass for a finished track.
					if (expected_bytes) {
						*expected_bytes = (uint64_t)json_obj_long(&doc, best, "clen", 0);
					}
					resolved_bitrate_kbps = (int)(json_obj_long(&doc, best, "bitrate", 0) / 1000);
					printf("ytmusic: %.11s: %.48s offers itag %ld at %ld kbps\n", video_id, base,
						   itag, json_obj_long(&doc, best, "bitrate", 0) / 1000);
					if (duration) *duration = (int)json_obj_long(&doc, root, "lengthSeconds", 0);
					if (title && title_size) json_obj_str(&doc, root, "title", title, title_size);
					if (artist && artist_size) json_obj_str(&doc, root, "author", artist, artist_size);
				}
			}
		}
		json_free(&doc);
	}
	free(body);
	if (!ok && answered) set_error(error, error_size, "ytmusic_instance_no_stream");
	return ok;
}

static bool token_video_id(const json_doc_t *doc, int renderer, char out[YTMUSIC_ID_MAX]) {
	static const char *const paths[] = {
		"playlistItemData.videoId",
		"navigationEndpoint.watchEndpoint.videoId",
		"flexColumns.0.musicResponsiveListItemFlexColumnRenderer.text.runs.0.navigationEndpoint.watchEndpoint.videoId",
		"overlay.musicItemThumbnailOverlayRenderer.content.musicPlayButtonRenderer.playNavigationEndpoint.watchEndpoint.videoId",
	};
	for (size_t i = 0; i < sizeof(paths) / sizeof(paths[0]); i++) {
		if (json_str(doc, json_path(doc, renderer, paths[i]), out, YTMUSIC_ID_MAX) && out[0]) return true;
	}
	return false;
}

static int parse_duration(const char *text) {
	int a = 0, b = 0, c = 0;
	if (sscanf(text, "%d:%d:%d", &a, &b, &c) == 3) return a * 3600 + b * 60 + c;
	if (sscanf(text, "%d:%d", &a, &b) == 2) return a * 60 + b;
	return 0;
}

// The page a container row opens: a playlist ("VL..."), an album ("MPRE..."),
// a podcast show ("MPSP...") or an artist channel.
static bool token_browse_id(const json_doc_t *doc, int renderer, char *out, size_t size) {
	static const char *const paths[] = {
		"navigationEndpoint.browseEndpoint.browseId",
		"title.runs.0.navigationEndpoint.browseEndpoint.browseId",
		"subtitle.runs.0.navigationEndpoint.browseEndpoint.browseId",
	};
	for (size_t i = 0; i < sizeof(paths) / sizeof(paths[0]); i++) {
		if (json_str(doc, json_path(doc, renderer, paths[i]), out, size) && out[0]) return true;
	}
	return false;
}

// The artwork of one row: the largest size the page offers, which is the last
// entry of the thumbnails array. A song carries it under "thumbnail" and a
// playlist, album or show card under "thumbnailRenderer"; below that the two
// shapes are the same. Empty when the row has no artwork, which is a row that
// keeps its placeholder glyph.
static bool read_thumbnail(const json_doc_t *doc, int renderer, char *out, size_t size) {
	static const char *const paths[] = {
		"thumbnail.musicThumbnailRenderer.thumbnail.thumbnails",
		"thumbnailRenderer.musicThumbnailRenderer.thumbnail.thumbnails",
		"thumbnail.thumbnails",
	};
	out[0] = '\0';
	for (size_t i = 0; i < sizeof(paths) / sizeof(paths[0]); i++) {
		int list = json_path(doc, renderer, paths[i]);
		for (int j = json_len(doc, list) - 1; j >= 0; j--) {
			if (json_obj_str(doc, json_at(doc, list, j), "url", out, size) && out[0]) {
				return true;
			}
		}
	}
	return false;
}

// Is this text a listen count rather than a name? YouTube writes one where an
// album's track list would name the artist ("1,2 mln plays", "12K plays"), and
// on a row that plays it is noise: the row is a song, not a chart position. The
// stored pages are not all English, so both the word and the bare number are
// recognised: a leading count followed by a short unit is a count whatever
// language the unit is in, and a name does not start that way.
static bool text_is_play_count(const char *text) {
	if (!text || !text[0]) {
		return false;
	}
	static const char *const words[] = {
		" play", "play count", "odtworz", "wyswietl", "wy\xC5\x9Bwietl", "reproduc",
		"wiedergabe", "visualizz", "visualizac", "afspil", "soittok", "aflspr",
	};
	char lower[128];
	size_t n = 0;
	for (; text[n] && n + 1 < sizeof(lower); n++) {
		char c = text[n];
		lower[n] = (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
	}
	lower[n] = '\0';
	for (size_t i = 0; i < sizeof(words) / sizeof(words[0]); i++) {
		if (strstr(lower, words[i])) {
			return true;
		}
	}
	// A number, then a short unit: "1,2 mln", "12K", "3.4M", "1 234". A name
	// that happens to start with a digit ("2Pac") has more than five letters
	// after it and is left alone.
	const char *p = text;
	while (*p == ' ') p++;
	if (!isdigit((unsigned char)*p)) {
		return false;
	}
	while (isdigit((unsigned char)*p) || *p == ',' || *p == '.' || *p == ' ' || *p == '\'' ||
		   (unsigned char)*p == 0xC2 || (unsigned char)*p == 0xA0) {
		p++;
	}
	return p != text && strlen(p) <= 5;
}

// The album of a song row.
//
// The second column is "artist <bullet> album <bullet> duration", one run per
// piece, and the artist itself is several runs when a track lists more than one
// (", " and " & " between the names). So the album is the group between the two
// bullets -- unless that group is the length itself, which is the shape a
// playlist or album track list has. Read from the runs rather than guessed from
// the row's text, so an artist called "4:09" cannot be mistaken for one.
static void read_album(const json_doc_t *doc, int renderer, ytmusic_item_t *item) {
	int runs = json_path(doc, renderer, "flexColumns.1.musicResponsiveListItemFlexColumnRenderer.text.runs");
	char album[YTMUSIC_TITLE_MAX] = "";
	bool past_artist = false;
	for (int i = 0; i < json_len(doc, runs); i++) {
		char text[YTMUSIC_TITLE_MAX] = "";
		if (!json_obj_str(doc, json_at(doc, runs, i), "text", text, sizeof(text)) || !text[0]) continue;
		if (strstr(text, "\xE2\x80\xA2")) { // the bullet: " • "
			if (past_artist) break;            // the album group has ended
			past_artist = true;
			continue;
		}
		if (!past_artist) continue; // the artist, and the "&" / "," between names
		if (text_is_play_count(text)) continue; // a chart position, not this song's album
		size_t used = strlen(album);
		snprintf(album + used, sizeof(album) - used, "%s", text);
	}
	// A duration where the album should be: the page did not name one.
	size_t len = strlen(album);
	while (len > 0 && (album[len - 1] == ' ' || album[len - 1] == '\t')) album[--len] = '\0';
	char *start = album;
	while (*start == ' ' || *start == '\t') start++;
	if (!start[0] || (isdigit((unsigned char)start[0]) && strchr(start, ':'))) return;
	snprintf(item->album, sizeof(item->album), "%s", start);
}

static bool is_bare_year(const char *s) {
	if (!s) return false;
	while (*s == ' ' || *s == '\t') s++;
	if (strlen(s) != 4) return false;
	for (int i = 0; i < 4; i++) {
		if (!isdigit((unsigned char)s[i])) return false;
	}
	return true;
}

static bool is_release_type(const char *text) {
	if (!text || !text[0]) return false;
	static const char *const types[] = {
		"album", "single", "singiel", "ep", "song", "utwór", "utwor",
		"piosenka", "track", "video", "wideo", "playlist", "playlista",
	};
	for (size_t i = 0; i < sizeof(types) / sizeof(types[0]); i++) {
		if (strcasecmp(text, types[i]) == 0) return true;
	}
	return false;
}

static void read_subtitle_from_runs(const json_doc_t *doc, int runs_arr, char *out, size_t size) {
	if (out && size) out[0] = '\0';
	if (!doc || runs_arr < 0 || !out || size == 0) return;
	int len = json_len(doc, runs_arr);
	if (len <= 0) return;

	bool has_bullet = false;
	for (int i = 0; i < len; i++) {
		char t[YTMUSIC_NAME_MAX] = "";
		if (json_obj_str(doc, json_at(doc, runs_arr, i), "text", t, sizeof(t)) &&
			(strstr(t, "\xE2\x80\xA2") || strstr(t, "\xC2\xB7"))) {
			has_bullet = true;
			break;
		}
	}

	if (has_bullet) {
		char before_bullet[YTMUSIC_NAME_MAX] = "";
		char after_bullet[YTMUSIC_NAME_MAX] = "";
		bool past_bullet = false;
		for (int i = 0; i < len; i++) {
			char t[YTMUSIC_NAME_MAX] = "";
			if (!json_obj_str(doc, json_at(doc, runs_arr, i), "text", t, sizeof(t)) || !t[0]) continue;
			if (strstr(t, "\xE2\x80\xA2") || strstr(t, "\xC2\xB7")) {
				if (past_bullet) break;
				past_bullet = true;
				continue;
			}
			if (!past_bullet) {
				size_t cur = strlen(before_bullet);
				snprintf(before_bullet + cur, sizeof(before_bullet) - cur, "%s", t);
			} else {
				size_t cur = strlen(after_bullet);
				snprintf(after_bullet + cur, sizeof(after_bullet) - cur, "%s", t);
			}
		}

		char *b = before_bullet;
		while (*b == ' ' || *b == '\t') b++;
		size_t blen = strlen(b);
		while (blen > 0 && (b[blen - 1] == ' ' || b[blen - 1] == '\t')) b[--blen] = '\0';

		char *a = after_bullet;
		while (*a == ' ' || *a == '\t') a++;
		size_t alen = strlen(a);
		while (alen > 0 && (a[alen - 1] == ' ' || a[alen - 1] == '\t')) a[--alen] = '\0';

		if (is_release_type(b)) {
			if (a[0] && !text_is_play_count(a) && !is_bare_year(a)) {
				snprintf(out, size, "%s", a);
				return;
			}
		} else if (b[0] && !text_is_play_count(b) && !is_bare_year(b)) {
			snprintf(out, size, "%s", b);
			return;
		} else if (a[0] && !text_is_play_count(a) && !is_bare_year(a)) {
			snprintf(out, size, "%s", a);
			return;
		}
	}

	for (int i = 0; i < len; i++) {
		char t[YTMUSIC_NAME_MAX] = "";
		if (json_obj_str(doc, json_at(doc, runs_arr, i), "text", t, sizeof(t)) && t[0]) {
			char *trimmed = t;
			while (*trimmed == ' ' || *trimmed == '\t') trimmed++;
			size_t tlen = strlen(trimmed);
			while (tlen > 0 && (trimmed[tlen - 1] == ' ' || trimmed[tlen - 1] == '\t')) trimmed[--tlen] = '\0';
			if (trimmed[0] && !text_is_play_count(trimmed) && !is_release_type(trimmed) && !is_bare_year(trimmed)) {
				snprintf(out, size, "%s", trimmed);
				return;
			}
		}
	}
}

// One row of any of the three shapes the store uses for a list entry: a song
// row, a podcast episode (a multi-row item), or a card for a playlist, album or
// show. A row with a video id plays; one that only names a page opens it.
static bool read_item(const json_doc_t *doc, int renderer, ytmusic_item_t *item) {
	memset(item, 0, sizeof(*item));
	if (token_video_id(doc, renderer, item->id)) {
		item->kind = YTMUSIC_ITEM_TRACK;
	} else if (token_browse_id(doc, renderer, item->browse_id, sizeof(item->browse_id))) {
		item->kind = YTMUSIC_ITEM_CONTAINER;
	} else {
		return false;
	}
	json_str(doc, json_path(doc, renderer, "flexColumns.0.musicResponsiveListItemFlexColumnRenderer.text.runs.0.text"), item->title, sizeof(item->title));
	if (!item->title[0]) json_str(doc, json_path(doc, renderer, "title.runs.0.text"), item->title, sizeof(item->title));
	// The second column is the artist, or the show for an episode; the third
	// holds the album, or a play count on an album page. The first one that says
	// something is the subtitle -- unless what it says is a play count, which is
	// what an album's or a chart's track list puts where a song puts its artist.
	// The next column is tried instead; a row that only ever offers counts keeps
	// its name and its sleeve and says nothing else, which is how the Tidal rows
	// read.
	read_subtitle_from_runs(doc, json_path(doc, renderer, "subtitle.runs"), item->subtitle, sizeof(item->subtitle));
	if (!item->subtitle[0]) {
		static const char *const subtitle_paths[] = {
			"flexColumns.1.musicResponsiveListItemFlexColumnRenderer.text.runs.0.text",
			"flexColumns.2.musicResponsiveListItemFlexColumnRenderer.text.runs.0.text",
			"straplineTextOne.runs.0.text",
			"longBylineText.runs.0.text",
			"shortBylineText.runs.0.text",
			"ownerText.runs.0.text",
			"strapline.runs.0.text",
			"episodePublisherText.runs.0.text",
			"subtitle.runs.0.text",
		};
		for (size_t i = 0; i < sizeof(subtitle_paths) / sizeof(subtitle_paths[0]); i++) {
			char text[YTMUSIC_NAME_MAX] = "";
			if (!json_str(doc, json_path(doc, renderer, subtitle_paths[i]), text, sizeof(text)) || !text[0]) {
				continue;
			}
			if (text_is_play_count(text) || is_release_type(text) || is_bare_year(text)) {
				continue;
			}
			snprintf(item->subtitle, sizeof(item->subtitle), "%s", text);
			break;
		}
	}
	if (item->kind == YTMUSIC_ITEM_TRACK) {
		read_album(doc, renderer, item);
		if (json_path(doc, renderer, "episodePublisherText") >= 0) {
			item->podcast_episode = true;
		}
	}
	read_thumbnail(doc, renderer, item->thumbnail, sizeof(item->thumbnail));
	char duration[32] = "";
	json_str(doc, json_path(doc, renderer, "fixedColumns.0.musicResponsiveListItemFixedColumnRenderer.text.runs.0.text"), duration, sizeof(duration));
	item->duration = parse_duration(duration);
	return item->title[0] != '\0';
}

static const char *skip_json_space(const char *p) {
	while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
	return p;
}

// Return the byte after a JSON string. This small scanner lets search skip the
// large unrelated parts of an InnerTube response without allocating a token
// for every field in it.
static const char *json_string_end(const char *p) {
	if (*p != '"') return NULL;
	for (p++; *p; p++) {
		if (*p == '\\') {
			if (!p[1]) return NULL;
			p++;
		} else if (*p == '"') {
			return p + 1;
		}
	}
	return NULL;
}

// Return the byte after one complete object/array, respecting strings and
// escaped quotes. A response may be truncated at the HTTP cap; in that case
// complete track rows before the cut remain usable.
static const char *json_compound_end(const char *start) {
	char closers[JSON_MAX_DEPTH];
	int depth = 0;
	bool in_string = false;
	for (const char *p = start; *p; p++) {
		if (in_string) {
			if (*p == '\\') {
				if (!p[1]) return NULL;
				p++;
			} else if (*p == '"') {
				in_string = false;
			}
			continue;
		}
		if (*p == '"') {
			in_string = true;
		} else if (*p == '{' || *p == '[') {
			if (depth >= JSON_MAX_DEPTH) return NULL;
			closers[depth++] = *p == '{' ? '}' : ']';
		} else if (*p == '}' || *p == ']') {
			if (depth <= 0 || closers[depth - 1] != *p) return NULL;
			if (--depth == 0) return p + 1;
		}
	}
	return NULL;
}

static bool track_renderer_is_ad(const json_doc_t *doc, int renderer) {
	// Ad markers are keys of the renderer itself -- "adSlot", "promoted" and
	// their kin. They are never anything nested inside it, and reading that
	// deep once flagged every real row on the page: the action menu each row
	// carries holds "signInEndpoint":{"hack":true}, and a boolean called
	// "hack" read exactly like the ad this check went looking for. So the
	// scan stays on the renderer's own keys, and only the known names count.
	int end_char = doc->toks[renderer].end;
	for (int i = renderer + 1; i < doc->count && doc->toks[i].start < end_char; i++) {
		if (doc->toks[i].parent != renderer || doc->toks[i].type != JSON_STRING) {
			continue; // a value, or a key of something nested deeper in
		}
		char key[128];
		if (!json_str(doc, i, key, sizeof(key))) continue;
		if (strcasestr(key, "adSlot") || strcasestr(key, "adPlacement") ||
			strcasestr(key, "displayAd") || strcasestr(key, "promoted") ||
			strcasestr(key, "sponsored")) {
			return true;
		}
	}
	return false;
}

// The three renderers a list entry arrives in. A song and a podcast show are
// both "responsive list items", a playlist/album/show card is a "two row item",
// and a podcast episode is a "multi row item".
static bool item_renderer_key(const char *key, size_t len) {
	static const char *const keys[] = {
		"musicResponsiveListItemRenderer",
		"musicTwoRowItemRenderer",
		"musicMultiRowListItemRenderer",
	};
	for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
		if (len == strlen(keys[i]) && memcmp(key, keys[i], len) == 0) return true;
	}
	return false;
}

// A public page that came back with nothing is worth one line in the
// developer log -- the length alone tells a consent wall (a big HTML page)
// from a transfer that died (a stub) from a request that never went out. The
// body itself is not kept anywhere: a working player has no use for whole
// search JSONs on the card, and the developer log switch is the only way
// anything is collected.
static void ytmusic_log_empty_body(const char *what, const char *body) {
	if (!body) {
		fprintf(stderr, "ytmusic: %s page empty: no body at all\n", what);
		return;
	}
	fprintf(stderr, "ytmusic: %s page empty, %zu bytes\n", what, strlen(body));
}

// Parse each item renderer on its own instead of materializing the entire
// page as one token tree. This uses much less RAM and still returns complete
// early rows if YouTube sends a response larger than our bounded HTTP buffer.
static int collect_items_compact(const char *response, ytmusic_item_t *items, int limit) {
	const char *json = youtubei_json_start(response);
	if (!json || (*json != '{' && *json != '[')) return -1;
	int count = 0;
	bool saw_unreadable = false;
	const char *p = json;

	while (*p && count < limit) {
		if (*p != '"') {
			p++;
			continue;
		}
		const char *key_end = json_string_end(p);
		if (!key_end) break;
		bool is_item_key = item_renderer_key(p + 1, (size_t)(key_end - p - 2));
		const char *value = skip_json_space(key_end);
		if (!is_item_key || *value != ':') {
			p = key_end;
			continue;
		}
		value = skip_json_space(value + 1);
		if (*value != '{') {
			saw_unreadable = true;
			p = key_end;
			continue;
		}
		const char *end = json_compound_end(value);
		if (!end) {
			saw_unreadable = true;
			break;
		}
		size_t item_len = (size_t)(end - value);
		if (item_len <= YTMUSIC_RENDERER_SIZE_LIMIT) {
			char *item_json = malloc(item_len + 1);
			if (!item_json) return count ? count : -1;
			memcpy(item_json, value, item_len);
			item_json[item_len] = '\0';
			json_doc_t item_doc;
			if (json_parse_limited(item_json, &item_doc, YTMUSIC_RENDERER_TOKEN_LIMIT)) {
				int root = json_root(&item_doc);
				// An ad, sponsored or promoted card is skipped in silence: it is
				// not a broken row, it is a row this player does not offer.
				if (!track_renderer_is_ad(&item_doc, root)) {
					ytmusic_item_t candidate;
					if (read_item(&item_doc, root, &candidate)) {
						static const char PODCAST_RENDERER[] = "musicMultiRowListItemRenderer";
						if (is_item_key && (size_t)(key_end - p - 2) == sizeof(PODCAST_RENDERER) - 1 &&
							memcmp(p + 1, PODCAST_RENDERER, sizeof(PODCAST_RENDERER) - 1) == 0) {
							candidate.podcast_episode = true;
						}
						if (candidate.kind == YTMUSIC_ITEM_TRACK) {
							if (strstr(item_json, "\"MPSP") != NULL ||
							    strstr(item_json, "episodePublisherText") != NULL) {
								candidate.podcast_episode = true;
							}
						}
						bool duplicate = false;
						for (int i = 0; i < count; i++) {
							const char *mine = items[i].kind == YTMUSIC_ITEM_TRACK ? items[i].id : items[i].browse_id;
							const char *theirs = candidate.kind == YTMUSIC_ITEM_TRACK ? candidate.id : candidate.browse_id;
							if (mine[0] && strcmp(mine, theirs) == 0) {
								duplicate = true;
								break;
							}
						}
						if (!duplicate) items[count++] = candidate;
					} else {
						saw_unreadable = true;
					}
				}
				json_free(&item_doc);
			} else {
				saw_unreadable = true;
			}
			free(item_json);
		} else {
			saw_unreadable = true;
		}
		p = end;
	}
	if (!count && saw_unreadable) return -1;
	return count;
}

// The heading of one carousel of the Explore page.
//
// The heading lives in the carousel's header, a small object at the front of
// it -- but the carousel itself is the whole shelf, and on a real Explore page
// that is a couple of hundred kilobytes and far more tokens than a bounded
// parse will take. Parsing all of it to read one string used to run past that
// ceiling and answer "no heading" for every row on the page, which drew three
// rows all labelled Explore. So the header object is cut out first and parsed
// on its own, which costs one scan of the carousel's first few hundred bytes.
static void read_section_title(const char *object, char *out, size_t size) {
	static const char *const headers[] = {
		"\"musicCarouselShelfBasicHeaderRenderer\"",
		"\"musicCardShelfHeaderBasicRenderer\"",
	};
	out[0] = '\0';
	for (size_t i = 0; i < sizeof(headers) / sizeof(headers[0]); i++) {
		const char *key = strstr(object, headers[i]);
		if (!key) continue;
		const char *value = skip_json_space(key + strlen(headers[i]));
		if (*value != ':') continue;
		value = skip_json_space(value + 1);
		if (*value != '{') continue;
		const char *end = json_compound_end(value);
		if (!end) continue;
		size_t len = (size_t)(end - value);
		char *header = malloc(len + 1);
		if (!header) return;
		memcpy(header, value, len);
		header[len] = '\0';
		json_doc_t doc;
		if (json_parse_limited(header, &doc, 2000)) {
			int root = json_root(&doc);
			if (!json_str(&doc, json_path(&doc, root, "title.runs.0.text"), out, size) || !out[0]) {
				json_str(&doc, json_path(&doc, root, "title.simpleText"), out, size);
			}
			json_free(&doc);
		}
		free(header);
		if (out[0]) return;
	}
}

static void read_browse_header(const char *body, char *artist, size_t artist_size,
                               char *album, size_t album_size,
                               char *thumbnail, size_t thumb_size) {
	if (artist && artist_size) artist[0] = '\0';
	if (album && album_size) album[0] = '\0';
	if (thumbnail && thumb_size) thumbnail[0] = '\0';
	if (!body) return;

	static const char *const headers[] = {
		"\"musicResponsiveHeaderRenderer\"",
		"\"musicHeaderRenderer\"",
		"\"musicVisualHeaderRenderer\"",
		"\"musicDetailHeaderRenderer\"",
		"\"musicEditablePlaylistHeaderRenderer\"",
	};

	for (size_t i = 0; i < sizeof(headers) / sizeof(headers[0]); i++) {
		const char *key = strstr(body, headers[i]);
		if (!key) continue;
		const char *value = skip_json_space(key + strlen(headers[i]));
		if (*value != ':') continue;
		value = skip_json_space(value + 1);
		if (*value != '{') continue;
		const char *end = json_compound_end(value);
		if (!end) continue;
		size_t len = (size_t)(end - value);
		if (len > YTMUSIC_RENDERER_SIZE_LIMIT) continue;
		char *header = malloc(len + 1);
		if (!header) return;
		memcpy(header, value, len);
		header[len] = '\0';
		json_doc_t doc;
		if (json_parse_limited(header, &doc, YTMUSIC_RENDERER_TOKEN_LIMIT)) {
			int root = json_root(&doc);
			if (album && album_size && !album[0]) {
				if (!json_str(&doc, json_path(&doc, root, "title.runs.0.text"), album, album_size) || !album[0]) {
					json_str(&doc, json_path(&doc, root, "title.simpleText"), album, album_size);
				}
			}
			if (artist && artist_size && !artist[0]) {
				static const char *const artist_paths[] = {
					"straplineTextOne.runs.0.text",
					"straplineTextOne.simpleText",
					"strapline.runs.0.text",
					"bylineText.runs.0.text",
					"ownerText.runs.0.text",
				};
				for (size_t p = 0; p < sizeof(artist_paths) / sizeof(artist_paths[0]); p++) {
					if (json_str(&doc, json_path(&doc, root, artist_paths[p]), artist, artist_size) && artist[0]) {
						break;
					}
				}
				if (!artist[0]) {
					int sub_runs = json_path(&doc, root, "subtitle.runs");
					read_subtitle_from_runs(&doc, sub_runs, artist, artist_size);
				}
			}
			if (thumbnail && thumb_size && !thumbnail[0]) {
				read_thumbnail(&doc, root, thumbnail, thumb_size);
			}
			json_free(&doc);
		}
		free(header);
		if ((artist && artist[0]) || (album && album[0]) || (thumbnail && thumbnail[0])) {
			return;
		}
	}
}

// The cards under one carousel. The carousel is a slice of the whole page, so
// it is copied out (the scanner wants a terminated string) and the items are
// read from the copy.
static int collect_section_items(const char *object, size_t len, ytmusic_section_t *section) {
	char *copy = malloc(len + 1);
	if (!copy) return -1;
	memcpy(copy, object, len);
	copy[len] = '\0';
	read_section_title(copy, section->title, sizeof(section->title));
	int count = collect_items_compact(copy, section->items, YTMUSIC_SECTION_ITEMS_MAX);
	free(copy);
	return count;
}

static bool browse_id_valid(const char *id) {
	if (!id || !id[0] || strlen(id) >= YTMUSIC_BROWSE_ID_MAX) return false;
	for (const unsigned char *p = (const unsigned char *)id; *p; p++) {
		if (!isalnum(*p) && *p != '-' && *p != '_') return false;
	}
	return true;
}

static bool playable_track_response(const json_doc_t *doc, int root, const char *video_id,
									char *error, size_t error_size) {
	char response_id[YTMUSIC_ID_MAX] = "";
	char status[32] = "";
	if (!json_obj_str(doc, json_get(doc, root, "videoDetails"), "videoId",
					  response_id, sizeof(response_id)) ||
		strcmp(response_id, video_id) != 0) {
		set_error(error, error_size, "ytmusic_unexpected_video");
		return false;
	}
	if (!json_obj_str(doc, json_get(doc, root, "playabilityStatus"), "status",
					  status, sizeof(status)) ||
		strcmp(status, "OK") != 0) {
		set_error(error, error_size, "ytmusic_track_unavailable");
		return false;
	}
	return true;
}

int ytmusic_search_items(const char *query, ytmusic_filter_t filter,
						 ytmusic_item_t *items, int limit, char *error, size_t error_size) {
	if (error && error_size) error[0] = '\0';
	if (!query || !query[0] || !items || limit <= 0) {
		set_error(error, error_size, "ytmusic_invalid_search");
		return -1;
	}
	if (limit > YTMUSIC_ITEMS_MAX) limit = YTMUSIC_ITEMS_MAX;
	char escaped[512], request[1400], gl[3], hl[3];
	catalogue_bounds(gl, sizeof(gl), hl, sizeof(hl));
	if (!json_escape(query, escaped, sizeof(escaped))) {
		set_error(error, error_size, "ytmusic_search_too_long");
		return -1;
	}
	// The songs filter and the podcasts one. Both are the store's own encoded
	// filter values, the same ones its search chips send.
	const char *params = "EgWKAQIIAWoKEAkQBRAKEAMQBA==";
	if (filter == YTMUSIC_FILTER_ALBUMS) {
		params = "EgWKAQIYAWoKEAkQBRAKEAMQBA==";
	} else if (filter == YTMUSIC_FILTER_ARTISTS) {
		params = "EgWKAQIgAWoKEAkQBRAKEAMQBA==";
	} else if (filter == YTMUSIC_FILTER_PODCASTS) {
		params = "EgWKAQJQAWoMEA4QChADEAQQCRAF";
	}
	snprintf(request, sizeof(request),
		"{\"context\":{\"client\":{\"clientName\":\"WEB_REMIX\",\"clientVersion\":\"1.20250101.00.00\",\"hl\":\"%s\",\"gl\":\"%s\"}},\"query\":\"%s\",\"params\":\"%s\"}",
		hl, gl, escaped, params);
	char *body = NULL;
	// Search is public, so it is asked as a visitor: the catalogue can answer a
	// request carrying the imported session with a successful but empty page,
	// even though the same query works signed out.
	if (!youtubei_post_ex("search", request, false, true, &body, NULL, error, error_size)) return -1;
	int count = collect_items_compact(body, items, limit);
	if (count > 0 && filter == YTMUSIC_FILTER_PODCASTS) {
		for (int i = 0; i < count; i++) {
			if (items[i].kind == YTMUSIC_ITEM_TRACK) {
				items[i].podcast_episode = true;
			}
		}
	}
	if (count <= 0) {
		ytmusic_log_empty_body("search", body);
	}
	free(body);
	if (count < 0) set_error(error, error_size, "ytmusic_unreadable_reply");
	return count;
}

int ytmusic_browse_items(const char *browse_id, ytmusic_item_t *items, int limit,
						 char *error, size_t error_size) {
	if (error && error_size) error[0] = '\0';
	if (!items || limit <= 0 || !browse_id_valid(browse_id)) {
		set_error(error, error_size, "ytmusic_invalid_browse");
		return -1;
	}
	if (limit > YTMUSIC_ITEMS_MAX) limit = YTMUSIC_ITEMS_MAX;
	char request[512], gl[3], hl[3];
	catalogue_bounds(gl, sizeof(gl), hl, sizeof(hl));
	snprintf(request, sizeof(request),
		"{\"context\":{\"client\":{\"clientName\":\"WEB_REMIX\",\"clientVersion\":\"1.20250101.00.00\",\"hl\":\"%s\",\"gl\":\"%s\"}},\"browseId\":\"%s\"}",
		hl, gl, browse_id);
	char *body = NULL;
	bool used_account = false;
	// Public albums, charts and result cards load even when the imported
	// session is not accepted. Try them as a visitor first; an account-owned
	// playlist still falls through below.
	if (!youtubei_post_ex("browse", request, false, true, &body, NULL, error, error_size)) {
		if (!session_cookie_attached() || !youtubei_post("browse", request, false, &body, NULL, error, error_size)) return -1;
		used_account = true;
	}
	char header_artist[YTMUSIC_NAME_MAX] = "";
	char header_album[YTMUSIC_TITLE_MAX] = "";
	char header_thumbnail[YTMUSIC_URL_MAX] = "";
	read_browse_header(body, header_artist, sizeof(header_artist),
	                   header_album, sizeof(header_album),
	                   header_thumbnail, sizeof(header_thumbnail));
	int count = collect_items_compact(body, items, limit);
	if (count == 0 && !used_account) {
		ytmusic_log_empty_body(browse_id, body);
	}
	free(body);
	if (count == 0 && !used_account && session_cookie_attached()) {
		char *private_page = NULL;
		if (youtubei_post("browse", request, false, &private_page, NULL, error, error_size)) {
			if (!header_artist[0] && !header_album[0] && !header_thumbnail[0]) {
				read_browse_header(private_page, header_artist, sizeof(header_artist),
				                   header_album, sizeof(header_album),
				                   header_thumbnail, sizeof(header_thumbnail));
			}
			count = collect_items_compact(private_page, items, limit);
			free(private_page);
		}
		if (error && error_size) error[0] = '\0';
	}
	if (count > 0 && strncmp(browse_id, "MPSP", 4) == 0) {
		for (int i = 0; i < count; i++) {
			if (items[i].kind == YTMUSIC_ITEM_TRACK) {
				items[i].podcast_episode = true;
			}
		}
	}
	// The liked songs -- "VLLM" -- are the account's own list, and the only
	// credential the music endpoints accept for it is the browser session
	// imported from the card. Without one the page answers with no rows and no
	// error, so Favourites would sit there saying "no results" forever: say
	// what is missing instead of showing a list that can never fill.
	if (count == 0 && strcmp(browse_id, "VLLM") == 0 && !session_cookie_attached()) {
		set_error(error, error_size, "ytmusic_library_needs_session");
		return -1;
	}
	if (count < 0) {
		set_error(error, error_size, "ytmusic_unreadable_reply");
		return count;
	}
	if (count > 0 && strcmp(browse_id, "VLLM") != 0) {
		for (int i = 0; i < count; i++) {
			if (items[i].kind == YTMUSIC_ITEM_TRACK) {
				if (!items[i].subtitle[0] && header_artist[0]) {
					snprintf(items[i].subtitle, sizeof(items[i].subtitle), "%s", header_artist);
				}
				if (!items[i].album[0] && header_album[0]) {
					snprintf(items[i].album, sizeof(items[i].album), "%s", header_album);
				}
				if (!items[i].thumbnail[0] && header_thumbnail[0]) {
					snprintf(items[i].thumbnail, sizeof(items[i].thumbnail), "%s", header_thumbnail);
				}
			}
		}
	}
	if (strcmp(browse_id, "VLLM") == 0 && session_cookie_attached()) {
		ytlike_t local[YTMUSIC_ITEMS_MAX];
		ytmusic_item_t merged[YTMUSIC_ITEMS_MAX];
		int local_count = ytlikes_snapshot(local, limit);
		if (local_count > YTMUSIC_ITEMS_MAX) local_count = YTMUSIC_ITEMS_MAX;
		int merged_count = 0;
		for (int i = 0; i < local_count && merged_count < limit; i++) {
			ytmusic_item_t *item = &merged[merged_count++];
			memset(item, 0, sizeof(*item));
			item->kind = YTMUSIC_ITEM_TRACK;
			snprintf(item->id, sizeof(item->id), "%.*s", (int)sizeof(item->id) - 1, local[i].id);
			snprintf(item->title, sizeof(item->title), "%.*s", (int)sizeof(item->title) - 1,
					 local[i].title[0] ? local[i].title : local[i].id);
			snprintf(item->subtitle, sizeof(item->subtitle), "%.*s",
					 (int)sizeof(item->subtitle) - 1, local[i].artist);
			snprintf(item->album, sizeof(item->album), "%.*s", (int)sizeof(item->album) - 1,
					 local[i].album);
			snprintf(item->thumbnail, sizeof(item->thumbnail),
					 "https://i.ytimg.com/vi/%.*s/hqdefault.jpg", YTLIKES_ID_MAX - 1, local[i].id);
			item->duration = local[i].duration;
		}
		for (int i = 0; i < count && merged_count < limit; i++) {
			const char *id = items[i].kind == YTMUSIC_ITEM_TRACK ? items[i].id : items[i].browse_id;
			bool duplicate = false;
			for (int j = 0; j < merged_count; j++) {
				const char *existing = merged[j].kind == YTMUSIC_ITEM_TRACK
					? merged[j].id : merged[j].browse_id;
				if (id[0] && strcmp(id, existing) == 0) {
					duplicate = true;
					break;
				}
			}
			if (!duplicate) merged[merged_count++] = items[i];
		}
		memcpy(items, merged, sizeof(items[0]) * (size_t)merged_count);
		count = merged_count;
	}
	return count;
}

// The carousels of one Explore page, in the order the page lists them.
static int collect_sections(const char *body, ytmusic_section_t *sections, int limit) {
	int count = 0;
	static const char NEEDLE[] = "\"musicCarouselShelfRenderer\"";
	const char *p = body;
	while (count < limit && (p = strstr(p, NEEDLE)) != NULL) {
		p += sizeof(NEEDLE) - 1;
		const char *value = skip_json_space(p);
		if (*value != ':') continue;
		value = skip_json_space(value + 1);
		if (*value != '{') continue;
		const char *end = json_compound_end(value);
		if (!end) break;
		ytmusic_section_t *section = &sections[count];
		memset(section, 0, sizeof(*section));
		int items = collect_section_items(value, (size_t)(end - value), section);
		// A carousel whose cards are not playable or browsable -- "Moods &
		// genres" is a grid of filter buttons -- is not a row worth showing.
		if (items > 0) {
			section->count = items;
			count++;
		}
		p = end;
	}
	// A page without carousels still has rows on it: show them under one
	// untitled heading rather than an empty Explore page.
	if (!count) {
		memset(&sections[0], 0, sizeof(sections[0]));
		int items = collect_items_compact(body, sections[0].items, YTMUSIC_SECTION_ITEMS_MAX);
		if (items > 0) {
			sections[0].count = items;
			count = 1;
		}
	}
	return count;
}

int ytmusic_explore_sections(ytmusic_section_t *sections, int limit,
							 char *error, size_t error_size) {
	if (error && error_size) error[0] = '\0';
	if (!sections || limit <= 0) {
		set_error(error, error_size, "ytmusic_invalid_browse");
		return -1;
	}
	if (limit > YTMUSIC_SECTIONS_MAX) limit = YTMUSIC_SECTIONS_MAX;
	// Explore, the store's own front page of new releases and charts. It is a
	// public page, so it answers without an account.
	char request[512], gl[3], hl[3];
	catalogue_bounds(gl, sizeof(gl), hl, sizeof(hl));
	snprintf(request, sizeof(request),
		"{\"context\":{\"client\":{\"clientName\":\"WEB_REMIX\",\"clientVersion\":\"1.20250101.00.00\",\"hl\":\"%s\",\"gl\":\"%s\"}},\"browseId\":\"FEmusic_explore\"}",
		hl, gl);
	char *body = NULL;
	// Explore is public too; keep it independent of an attached account just
	// like search, so a rejected token cannot turn the front page empty.
	if (!youtubei_post_ex("browse", request, false, true, &body, NULL, error, error_size)) return -1;
	int count = collect_sections(body, sections, limit);
	if (!count) {
		ytmusic_log_empty_body("explore", body);
	}
	free(body);
	if (!count) set_error(error, error_size, "ytmusic_explore_empty");
	return count;
}

// The audio stream to download: the AAC track with the highest bitrate that
// the reply gives a plain URL for. Opus is skipped -- there is an Opus decoder
// but it reads Ogg, not the WebM container YouTube puts Opus in.
static bool pick_audio_format(const json_doc_t *doc, int root, char *url, size_t url_size) {
	int best = -1;
	long best_bitrate = -1;
	int formats = json_path(doc, root, "streamingData.adaptiveFormats");
	for (int i = 0; i < json_len(doc, formats); i++) {
		int format = json_at(doc, formats, i);
		char mime[96] = "", candidate[YTMUSIC_URL_MAX] = "";
		json_obj_str(doc, format, "mimeType", mime, sizeof(mime));
		if (strncasecmp(mime, "audio/mp4", 9) != 0) continue;
		if (!json_obj_str(doc, format, "url", candidate, sizeof(candidate)) || !candidate[0]) continue;
		long bitrate = json_obj_long(doc, format, "bitrate", 0);
		if (bitrate > best_bitrate) {
			best = format;
			best_bitrate = bitrate;
		}
	}
	if (best < 0) {
		// Nothing audio-only answered a plain URL. That is the end of it: the
		// muxed progressive file carries a video track this player must never
		// fetch, demux or decode, so a video stream is not a fallback but the
		// one answer that is refused. player_reply_media() names the error.
		return false;
	}
	return json_obj_str(doc, best, "url", url, url_size) && url[0];
}

// One player reply: the track that was asked for, playable, with its media URL
// and the facts the now-playing screen shows.
static bool player_reply_media(const char *body, const char *video_id, char *url, size_t url_size,
							   int *duration, char *title, size_t title_size,
							   char *artist, size_t artist_size,
							   char *error, size_t error_size) {
	json_doc_t doc;
	if (!body || !json_parse_limited(youtubei_json_start(body), &doc, YTMUSIC_PLAYER_TOKEN_LIMIT)) {
		set_error(error, error_size, "ytmusic_unreadable_reply");
		return false;
	}
	int root = json_root(&doc);
	if (!playable_track_response(&doc, root, video_id, error, error_size)) {
		json_free(&doc);
		return false;
	}
	int details = json_get(&doc, root, "videoDetails");
	if (duration) *duration = (int)json_obj_long(&doc, details, "lengthSeconds", 0);
	if (title && title_size) json_obj_str(&doc, details, "title", title, title_size);
	if (artist && artist_size) json_obj_str(&doc, details, "author", artist, artist_size);
	bool ok = pick_audio_format(&doc, root, url, url_size);
	json_free(&doc);
	if (!ok) set_error(error, error_size, "ytmusic_aac_stream_unavailable");
	return ok;
}

// One source's answer to "where are the bytes", named the way the log and the
// settings page speak about it: an instance base address, or one of these
// words.
#define YTMUSIC_VIA_WEB "web"
#define YTMUSIC_VIA_DIRECT "direct"
#define YTMUSIC_VIA_RELAY "relay"

// The imported browser session, through the web music player: it knows the
// account's region and, with Premium, its own stream. Only a session answers
// this client, so it is asked only when a cookie was imported.
static bool web_session_audio_url(const char *video_id, char *url, size_t url_size,
								  int *duration, char *title, size_t title_size,
								  char *artist, size_t artist_size, char *error,
								  size_t error_size) {
	char cookie[YTMUSIC_COOKIE_MAX];
	ytmusic_session_cookie(cookie, sizeof(cookie));
	bool have = cookie[0] != '\0';
	memset(cookie, 0, sizeof(cookie));
	if (!have) return false;
	char request[1024], gl[3], hl[3];
	catalogue_bounds(gl, sizeof(gl), hl, sizeof(hl));
	snprintf(request, sizeof(request),
			 "{\"context\":{\"client\":{\"clientName\":\"WEB_REMIX\",\"clientVersion\":\"%s\",\"hl\":\"%s\",\"gl\":\"%s\"}},\"videoId\":\"%s\",\"contentCheckOk\":true,\"racyCheckOk\":true}",
			 client_version[0] ? client_version : YTMUSIC_CLIENT_VERSION_FALLBACK, hl, gl, video_id);
	char *body = NULL;
	bool ok = youtubei_post("player", request, true, &body, NULL, error, error_size) &&
		player_reply_media(body, video_id, url, url_size, duration, title, title_size, artist,
						   artist_size, error, error_size);
	free(body);
	return ok;
}

// One source, asked. False when it has nothing, with `error` naming why when
// it said more than a shrug.
static bool resolve_via(const char *source, const char *video_id, char *url, size_t url_size,
						int *duration, char *title, size_t title_size, char *artist,
						size_t artist_size, bool *proxied, uint64_t *expected_bytes, char *error,
						size_t error_size) {
	if (!source || !source[0]) return false;
	if (proxied) *proxied = false;
	if (expected_bytes) *expected_bytes = 0;
	if (strcmp(source, YTMUSIC_VIA_WEB) == 0) {
		return web_session_audio_url(video_id, url, url_size, duration, title, title_size, artist,
									 artist_size, error, error_size);
	}
	if (strcmp(source, YTMUSIC_VIA_DIRECT) == 0) {
		char *body = NULL;
		bool asked = android_player(video_id, &body, error, error_size);
		bool ok = asked && player_reply_media(body, video_id, url, url_size, duration, title,
											  title_size, artist, artist_size, error, error_size);
		free(body);
		return ok;
	}
	if (strcmp(source, YTMUSIC_VIA_RELAY) == 0) {
		return relay_audio_url(video_id, url, url_size, duration, title, title_size, artist,
							   artist_size, proxied, error, error_size);
	}
	return instance_audio_url(source, video_id, url, url_size, duration, title, title_size,
							  artist, artist_size, proxied, expected_bytes, error, error_size);
}

// What a source is called in the log: a host reads better than an address, and
// "this device" better than "direct".
static const char *source_label(const char *source, char *out, size_t size) {
	if (strcmp(source, YTMUSIC_VIA_WEB) == 0) {
		snprintf(out, size, "%s", "the browser session");
	} else if (strcmp(source, YTMUSIC_VIA_DIRECT) == 0) {
		snprintf(out, size, "%s", "this device");
	} else if (strcmp(source, YTMUSIC_VIA_RELAY) == 0) {
		snprintf(out, size, "%s", "the relay");
	} else {
		const char *host = strstr(source, "://");
		snprintf(out, size, "%s", host ? host + 3 : source);
	}
	return out;
}

// The answers one play attempt walks, in order, and the only place that order
// is written down:
//
//   1. The imported browser session, when there is one: it knows the account's
//      region and, with Premium, its own stream.
//   2. The instances, the pinned one first. This is the path measured to work
//      on this device with no other machine involved: an Invidious instance
//      fetches the audio from Google and passes the bytes on through
//      /latest_version?local=true, so a track plays with nothing but the
//      player and its Wi-Fi.
//   3. The device itself, through the phone-app player. It resolves an address
//      Google then refuses to serve (403 on every media fetch in the field
//      log), so it is a hopeful extra here rather than the primary answer --
//      but the day that refusal stops, it plays with no instance at all.
//   4. The relay, which needs another machine and so comes last.
//
// A pin to "direct" or "relay" replaces the whole list with that one answer.
// An instance pin is tried first and the rest still follow it.
static int play_sources(char out[][YTMUSIC_SOURCE_MAX], int limit) {
	if (!out || limit <= 0) return 0;
	int count = 0;
	const char *source = source_value();
	bool relay_only = strcmp(source, "relay") == 0;
	bool direct_only = strcmp(source, "direct") == 0;
	if (relay_only) {
		snprintf(out[count++], YTMUSIC_SOURCE_MAX, "%s", YTMUSIC_VIA_RELAY);
		return count;
	}
	if (direct_only) {
		snprintf(out[count++], YTMUSIC_SOURCE_MAX, "%s", YTMUSIC_VIA_DIRECT);
		return count;
	}
	// Two slots are kept for the device and the relay: an instance list
	// somebody wrote must not push them out of the walk.
	int room = limit > 2 ? limit - 2 : 1;
	char cookie[YTMUSIC_COOKIE_MAX];
	ytmusic_session_cookie(cookie, sizeof(cookie));
	bool have_session = cookie[0] != '\0';
	memset(cookie, 0, sizeof(cookie));
	if (have_session && count < room) {
		snprintf(out[count++], YTMUSIC_SOURCE_MAX, "%s", YTMUSIC_VIA_WEB);
	}
	char bases[YTMUSIC_SOURCES_MAX][YTMUSIC_SOURCE_MAX];
	int instances = ytmusic_instance_bases(bases, YTMUSIC_SOURCES_MAX);
	// The built-in list is a guess at what might be up today; when it is the
	// list in use, a few tries is enough to know. A list somebody wrote
	// themselves is tried whole, because each entry there is a machine they
	// chose.
	if (!config_get("ytmusic", "instances", "")[0] && instances > YTMUSIC_INSTANCE_TRIES) {
		instances = YTMUSIC_INSTANCE_TRIES;
	}
	for (int i = 0; i < instances && count < room; i++) {
		snprintf(out[count++], YTMUSIC_SOURCE_MAX, "%.*s", YTMUSIC_SOURCE_MAX - 1, bases[i]);
	}
	if (count < limit) snprintf(out[count++], YTMUSIC_SOURCE_MAX, "%s", YTMUSIC_VIA_DIRECT);
	if (count < limit) snprintf(out[count++], YTMUSIC_SOURCE_MAX, "%s", YTMUSIC_VIA_RELAY);
	return count;
}

bool ytmusic_audio_url(const char *video_id, char *url, size_t url_size,
					  int *duration, char *title, size_t title_size,
					  char *artist, size_t artist_size, bool *relay_proxied,
					  char *error, size_t error_size) {
	if (error && error_size) error[0] = '\0';
	if (relay_proxied) *relay_proxied = false;
	if (!video_id || strlen(video_id) != 11 || !url || !url_size) {
		set_error(error, error_size, "ytmusic_invalid_track");
		return false;
	}
	for (const unsigned char *p = (const unsigned char *)video_id; *p; p++) {
		if (!isalnum(*p) && *p != '-' && *p != '_') {
			set_error(error, error_size, "ytmusic_invalid_track");
			return false;
		}
	}

	char sources[YTMUSIC_SOURCES_MAX][YTMUSIC_SOURCE_MAX];
	int count = play_sources(sources, YTMUSIC_SOURCES_MAX);
	// The error left standing is the last one a source had something to say
	// about: "the relay does not serve /streams" is actionable, and a generic
	// "no source" would throw it away.
	char last[192] = "";
	for (int i = 0; i < count; i++) {
		char label[YTMUSIC_SOURCE_MAX], reason[192] = "";
		source_label(sources[i], label, sizeof(label));
		if (resolve_via(sources[i], video_id, url, url_size, duration, title, title_size,
						artist, artist_size, relay_proxied, NULL, reason, sizeof(reason))) {
			printf("ytmusic: %.11s: %s served it\n", video_id, label);
			return true;
		}
		// A half-written address is worse than none: the caller fetches what
		// this buffer holds.
		url[0] = '\0';
		if (relay_proxied) *relay_proxied = false;
		if (reason[0]) snprintf(last, sizeof(last), "%s", reason);
		fprintf(stderr, "ytmusic: %.11s: %s had nothing (%s)\n", video_id, label,
				reason[0] ? reason : "no reason given");
	}
	set_error(error, error_size, last[0] ? last : "ytmusic_no_source");
	return false;
}

static void clean_tag_value(char *value) {
	for (; value && *value; value++) {
		if (*value == '\r' || *value == '\n') *value = ' ';
	}
}

static bool write_playback_tags(const char *path, const ytmusic_track_t *track) {
	char tags[640], tmp[680];
	if (snprintf(tags, sizeof(tags), "%s.tags", path) >= (int)sizeof(tags) ||
		snprintf(tmp, sizeof(tmp), "%s.tmp", tags) >= (int)sizeof(tmp)) return false;
	char title[YTMUSIC_TITLE_MAX], artist[YTMUSIC_NAME_MAX], album[YTMUSIC_TITLE_MAX];
	snprintf(title, sizeof(title), "%s", track->title);
	snprintf(artist, sizeof(artist), "%s", track->artist);
	snprintf(album, sizeof(album), "%s", track->album);
	clean_tag_value(title);
	clean_tag_value(artist);
	clean_tag_value(album);
	FILE *f = fopen(tmp, "w");
	if (!f) return false;
	// The id travels with the names: a copy kept on the card is still this
	// service's song, and the star on it has to reach YouTube Music's own
	// favourites rather than the card's. The file name there is the song's
	// name, so this sidecar is the only place the id survives.
	bool wrote = fprintf(f, "title=%s\nartist=%s\nalbum=%s\npodcast_episode=%d\nid=%s\n",
						 title, artist, album, track->podcast_episode ? 1 : 0, track->id) > 0;
	bool closed = fclose(f) == 0;
	bool ok = wrote && closed;
	if (!ok || rename(tmp, tags) != 0) {
		remove(tmp);
		return false;
	}
	return true;
}

// The record of a finished download.
//
// The tags sidecar used to be that record, and it is not one any more: the tags
// are written when playback starts, so that a file still arriving already has a
// title and an artist on screen, and a half-downloaded track carrying them
// would pass for a whole one. A marker of its own says only that: the audio is
// all here.
// The name a song's cache file has, whether or not it is there yet.
//
// The queue needs this before anything is downloaded. YouTube Music has no
// files of its own until the cache makes them, so the list the listener tapped
// in becomes a queue of the paths those songs will have -- exactly the shape
// Tidal's queue has, and the reason next and previous stay inside the service
// instead of walking whatever folder the last song happened to live in.
bool ytmusic_cache_path_for(const char *id, char *out, size_t out_size) {
	if (!id || strlen(id) != 11 || !out || out_size == 0) {
		return false;
	}
	const char *cache = playback_cache_dir();
	char name[512];
	if (snprintf(name, sizeof(name), "%s/%s.m4a", cache, id) >= (int)sizeof(name)) {
		return false;
	}
	if (strlen(name) >= out_size) {
		return false;
	}
	snprintf(out, out_size, "%s", name);
	return true;
}

// Whether that cache file is whole: its end was reached and the .done marker
// says so. A file that is still arriving answers false -- it is playable, just
// not finished, and the difference matters to whoever is deciding whether to
// start a transfer.
bool ytmusic_cache_whole(const char *path) {
	char id[16], name[512], done[520];
	if (!ytmusic_cache_track_id(path, id, sizeof(id))) {
		return false;
	}
	if (!ytmusic_cache_path_for(id, name, sizeof(name))) {
		return false;
	}
	if (snprintf(done, sizeof(done), "%s.done", name) >= (int)sizeof(done)) {
		return false;
	}
	return !growfile_is_growing(name) && access(name, R_OK) == 0 && access(done, R_OK) == 0;
}

bool ytmusic_cache_is_podcast_episode(const char *path) {
	if (!path || !path[0]) return false;
	char tag_path[544];
	FILE *file = NULL;

	// Check if <path>.tags exists directly beside the audio file
	if (snprintf(tag_path, sizeof(tag_path), "%s.tags", path) < (int)sizeof(tag_path)) {
		file = fopen(tag_path, "r");
	}

	// If not found beside path, try resolving via track ID and cache directory
	if (!file) {
		char id[YTMUSIC_ID_MAX], audio_path[512];
		if (ytmusic_cache_track_id(path, id, sizeof(id)) && id[0] &&
			ytmusic_cache_path_for(id, audio_path, sizeof(audio_path)) &&
			snprintf(tag_path, sizeof(tag_path), "%s.tags", audio_path) < (int)sizeof(tag_path)) {
			file = fopen(tag_path, "r");
		}
	}
	if (!file) return false;

	bool is_podcast = false;
	char line[128];
	while (fgets(line, sizeof(line), file)) {
		if (strncmp(line, "podcast_episode=1", 17) == 0) {
			is_podcast = true;
			break;
		}
	}
	fclose(file);
	return is_podcast;
}

// The title, artist and album of a queued song, written beside the file before
// any of it is downloaded. The queue holds the whole list, so playback can
// reach a song that has not arrived; without this its row and the player would
// show the video id until the transfer began. Text only: a sleeve is a
// download per track, and arrives with the audio.
bool ytmusic_cache_write_sidecars(const ytmusic_track_t *track, const char *path) {
	if (!track || !path || !path[0]) {
		return false;
	}
	return ensure_cache_dir() && write_playback_tags(path, track);
}

static void mark_complete(const char *path, bool done) {
	char marker[700];
	char marker_inc[700];
	if (snprintf(marker, sizeof(marker), "%s.done", path) >= (int)sizeof(marker) ||
		snprintf(marker_inc, sizeof(marker_inc), "%s.incompleto", path) >= (int)sizeof(marker_inc)) {
		return;
	}
	if (done) {
		FILE *f = fopen(marker, "w");
		if (f) fclose(f);
		remove(marker_inc);
	} else {
		FILE *f = fopen(marker_inc, "w");
		if (f) fclose(f);
		remove(marker);
	}
}

// Google throttles one long transfer of a media URL to about 32 KB/s -- a
// three-megabyte track then takes two minutes before a note comes out of the
// player -- while the same bytes asked for a few hundred kilobytes at a time
// arrive at the link's own speed. So the download is chunked, and one long
// transfer is kept as the fallback for a server that ignores Range.
#define YTMUSIC_CHUNK (256 * 1024)
// The first chunk of a stream, smaller than the rest: playback starts on the
// one that carries the index and the first frames, and at a public instance's
// pace the difference between this and a full chunk is the difference between
// a couple of seconds of card and five.
#define YTMUSIC_FIRST_CHUNK (96 * 1024)
// How long the transfer tolerates a server that answers but sends nothing.
// A 206 with an empty body is the instance's own upstream pausing, not the end
// of the file: a minute of it is a failure, anything shorter is buffering.
#define YTMUSIC_STALL_GIVEUP_MS (60 * 1000)

// Where the playback cache lives.
//
// On the memory card, beside Tidal's and Qobuz's scratch folders, and NOT in
// /tmp: /tmp is tmpfs on this device, which is to say RAM. A five minute track
// is four or five megabytes written there, and then they sit in the heap for as
// long as the song is loaded -- on a player where the decoder, the sleeve and a
// Bluetooth codec are all asking for the same megabytes. That is what made the
// figure on the processes page climb through a listening session, and what took
// the player down with LDAC on top of it.
//
// /tmp is still the answer when there is no card: the song is scratch, and a
// player with no card at all should still play. SONIX_YTMUSIC_CACHE moves it,
// which is what lets the host suite write it at all -- some Windows machines
// refuse every create under \tmp, and a suite that cannot make the folder would
// have to skip the download checks it exists to make.
#define YTMUSIC_CACHE_DIR ".local/ytmusic-cache"

static char cache_dir[320];

static const char *playback_cache_dir(void) {
	const char *override = getenv("SONIX_YTMUSIC_CACHE");
	if (override && override[0]) {
		return override;
	}
	return cache_dir[0] ? cache_dir : "/tmp/sonix-ytmusic";
}

// The card the cache lives on, told once the storage has looked for it.
// Without one the cache stays in /tmp, which is what it always was.
void ytmusic_set_cache_root(const char *sd_root) {
	if (!sd_root || !*sd_root) {
		cache_dir[0] = '\0';
		return;
	}
#ifdef HOST_BUILD
	sd_root = "/tmp"; // the simulator has no card; its /tmp stands in for one
#endif
	snprintf(cache_dir, sizeof(cache_dir), "%s/%s", sd_root, YTMUSIC_CACHE_DIR);
}

const char *ytmusic_cache_dir(void) { return playback_cache_dir(); }

// mkdir -p over the two levels the cache needs: .local, then the folder.
static bool ensure_cache_dir(void) {
	const char *cache = playback_cache_dir();
	char path[512];
	snprintf(path, sizeof(path), "%s", cache);
	for (char *p = path + 1; *p; p++) {
		if (*p != '/') {
			continue;
		}
		*p = '\0';
		mkdir(path, 0777);
		*p = '/';
	}
	if (mkdir(path, 0777) != 0 && errno != EEXIST) {
		fprintf(stderr, "ytmusic: cannot create %s: %s\n", path, strerror(errno));
		return false;
	}
	return true;
}

// What the download is doing, for the page's loading card. A percentage rather
// than a byte count: a 64-bit counter read from another thread on a 32-bit
// device can tear, and being one step out on a bar is not a problem.
static volatile int progress_percent = -1;
static volatile int progress_active;
static uint64_t progress_expected;

void ytmusic_download_progress(int *percent, bool *running) {
	if (percent) *percent = progress_percent;
	if (running) *running = progress_active != 0;
}

static void progress_note(uint64_t done) {
	if (progress_expected == 0) return;
	uint64_t percent = done * 100 / progress_expected;
	progress_percent = percent > 100 ? 100 : (int)percent;
}

// Starting the player before the file is whole.
//
// growfile.c gives the decoder a file that is still being written -- the same
// mechanism Qobuz plays through -- and this is the other end of that contract:
// announce the path, say what has arrived after every chunk, and say once, at
// the byte the track can be opened from, that playback may begin. NULL means an
// ordinary download, which is what "keep on card" wants: it has nothing to
// play while it waits, and a file that is only whole at the end.
typedef struct {
	const char *path;   // the file as the player will open it
	uint64_t ready_at;  // bytes that have to be on disk before it may
	ytmusic_ready_cb ready;
	void *user;
	bool called;
	bool have_moov;     // the decoder's index has landed, so opening will work
} stream_hook_t;

// Is the MP4 index there, and is it whole?
//
// An M4A is no use without its `moov` box: that is where the sample table
// lives, and the decoder cannot describe one frame without it. It sits at the
// front of the streams these instances serve, but "at the front" is a property
// of the container, not a promise -- and a `moov` that has begun but not
// finished is no index at all. So this walks the top-level box headers, which
// carry their own sizes, and says yes only when a whole `moov` is on disk.
//
// The reader walks the same boxes again when it opens the file (mp4.c) and
// waits there for a partial one; this copy exists so the player is not started
// -- sheet, artwork, transport -- into a file that cannot be opened yet.
static bool moov_arrived(const char *path) {
	FILE *f = fopen(path, "rb");
	if (!f) {
		return false;
	}
	bool ready = false;
	if (fseek(f, 0, SEEK_END) == 0) {
		long total = ftell(f);
		uint64_t pos = 0;
		for (int boxes = 0; total > 0 && boxes < 64 && pos + 8 <= (uint64_t)total; boxes++) {
			if (fseek(f, (long)pos, SEEK_SET) != 0) {
				break;
			}
			unsigned char head[16];
			if (fread(head, 1, 8, f) != 8) {
				break;
			}
			uint64_t size = ((uint64_t)head[0] << 24) | ((uint64_t)head[1] << 16) |
							((uint64_t)head[2] << 8) | (uint64_t)head[3];
			uint64_t header = 8;
			if (size == 1) {
				if (fread(head + 8, 1, 8, f) != 8) {
					break;
				}
				size = ((uint64_t)head[8] << 56) | ((uint64_t)head[9] << 48) |
					   ((uint64_t)head[10] << 40) | ((uint64_t)head[11] << 32) |
					   ((uint64_t)head[12] << 24) | ((uint64_t)head[13] << 16) |
					   ((uint64_t)head[14] << 8) | (uint64_t)head[15];
				header = 16;
			} else if (size == 0) {
				break; // to the end of the file: not an answer while it grows
			}
			if (size < header) {
				break;
			}
			if (memcmp(head + 4, "moov", 4) == 0) {
				ready = pos + size <= (uint64_t)total;
				break;
			}
			pos += size;
		}
	}
	fclose(f);
	return ready;
}

static void stream_note(stream_hook_t *hook, uint64_t done) {
	if (!hook) return;
	growfile_progress(hook->path, (long)done);
	if (hook->called || done < hook->ready_at) return;
	// Checked again on every chunk until it is there, and the player is not
	// started before it is. A stream that keeps its index at the very end
	// therefore starts when the download is over -- download_for_playback
	// plays it at its own end -- instead of starting into a decode failure.
	// The walk is a handful of seeks per chunk, not a parse of the file.
	if (!hook->have_moov) {
		hook->have_moov = moov_arrived(hook->path);
		if (!hook->have_moov) return;
	}
	hook->called = true;
	printf("ytmusic: %llu bytes are enough to start playing\n", (unsigned long long)done);
	if (hook->ready) hook->ready(hook->path, hook->user);
}

// How many bytes before the decoder is given the file: the first chunk, which
// is the smallest request whose answer carries the index and the first frames.
// Nothing here is sized by the length of the track -- that was the difference
// between "a song starts" and "a card sits there" -- because the rest of it
// arrives behind the player (see mp4.c's growth support).
//
// A track shorter than two first-chunks has no "behind the player": it starts
// halfway through instead, which is still earlier than its end.
static uint64_t stream_threshold(uint64_t expected) {
	uint64_t at = YTMUSIC_FIRST_CHUNK;
	if (expected && expected / 2 < at) {
		at = expected / 2;
	}
	return at;
}

// How many seconds of audio a finished file actually holds, or 0 when it says
// nothing (not an MP4 this reader understands, or a file whose index arrived
// without the track's own length).
//
// Asked of the media rather than trusted from the API reply, because the whole
// question after a transfer is whether the bytes are a song or a fragment of
// one -- and thirty seconds of a four-minute track is exactly the answer a
// half-finished download gives when the byte count is all it was checked
// against.
static int media_seconds(const char *path) {
	mp4_file_t *m = mp4_open(path);
	if (!m) {
		return 0;
	}
	double seconds = mp4_duration_seconds(m);
	mp4_close(m);
	if (!(seconds > 0)) {
		return 0;
	}
	return seconds > 86400.0 ? 86400 : (int)(seconds + 0.5);
}

// The file in ranged chunks. False when Range is not honoured (or nothing came
// back), which is the caller's cue to download it in one piece instead.
static bool download_ranged(const char *url, const char *part, uint64_t from, uint64_t *downloaded,
							bool *complete, uint64_t *total_out, stream_hook_t *hook) {
	// `from` is not always zero: a transfer that stopped early is continued by
	// another source at the byte it reached (see download_for_playback), and
	// that continuation appends to the file the decoder is already reading.
	*downloaded = from;
	*complete = false;
	// The whole size, as the server states it: read out of the Content-Range of
	// the replies, because an instance's JSON often does not name it -- and a
	// download that does not know the length has no way to tell a finished file
	// from one the server cut off.
	uint64_t total = total_out ? *total_out : 0;
	FILE *f = fopen(part, from > 0 ? "ab" : "wb");
	if (!f) {
		fprintf(stderr, "ytmusic: the cache file %.64s cannot be written (%s)\n", part,
				strerror(errno));
		return false;
	}
	bool ok = true;
	int empty_ms = 0;
	while (!*complete) {
		char range[96], *body = NULL;
		size_t len = 0;
		int status = 0;
		// Measured, not counted: how long the server has been answering with
		// nothing, so a slow instance is waited out and a dead one is given up.
		uint64_t ask = (hook && *downloaded == 0) ? YTMUSIC_FIRST_CHUNK : YTMUSIC_CHUNK;
		// A connection that drops mid-chunk is worth one more ask for the same
		// bytes: the offset has not moved, so nothing is written twice.
		bool got = false;
		for (int attempt = 0; attempt < 2 && !got; attempt++) {
			snprintf(range, sizeof(range), "Range: bytes=%llu-%llu\r\n",
					 (unsigned long long)*downloaded,
					 (unsigned long long)(*downloaded + ask - 1));
			http_req_t req = {.method = "GET",
							  .extra_headers = range,
							  .allow_empty_body = true, // a 206 is success, and so is a 204
							  .want_error_body = true,  // a refusal says why in its body
							  .no_https_downgrade = true};
			got = http_request(url, &req, &body, &len, (size_t)ask + 1, YTMUSIC_RELAY_TIMEOUT,
							   &status);
			if (!got) {
				fprintf(stderr, "http: media fetch failed at byte %llu: %s%s\n",
						(unsigned long long)*downloaded,
						http_last_error() ? http_last_error() : "no reason given",
						attempt == 0 ? ", asking again" : "");
				free(body);
				body = NULL;
				len = 0;
			}
		}
		if (!got) {
			ok = false;
			break;
		}
		if (status >= 400) {
			// A refused media fetch carries its reason in the body -- Google
			// says which rule was broken, and that sentence is the whole
			// diagnosis. It goes to the log, one line, with the newlines out.
			char why[161];
			size_t n = body && len < 160 ? len : (body ? 160 : 0);
			if (n) memcpy(why, body, n);
			why[n] = '\0';
			for (size_t k = 0; k < n; k++) {
				if (why[k] == '\r' || why[k] == '\n' || why[k] == '\t') why[k] = ' ';
			}
			fprintf(stderr, "http: media answered %d: %s\n", status, n ? why : "(no body)");
		}
		if (status == 200) {
			// The range was ignored and this is the whole file (or as much of
			// it as was kept): not a piece of anything.
			free(body);
			ok = false;
			break;
		}
		if (status == 416) {
			// Asked past the end -- of the file, or only of what the server is
			// willing to give. Those two are not the same thing, and taking the
			// second for the first is how a track came to play for thirty
			// seconds and then stop: the file was renamed into place as whole.
			// Known length and the byte count short of it means this source has
			// finished, not the file.
			free(body);
			*complete = *downloaded > 0 && (total == 0 || *downloaded >= total);
			break;
		}
		if (len == 0) {
			// A 206 carrying nothing is the instance's own upstream pausing, not
			// the end of the file -- treating it as an ending is the other way a
			// track used to finish after a minute. Only a 416 says the file is
			// over. Six empty replies in under two seconds (what this used to
			// allow) marked a half-downloaded track complete, which then played
			// as a short song for ever because it had tags beside it; the wait
			// is on the clock now.
			free(body);
			empty_ms += 500;
			if (empty_ms >= YTMUSIC_STALL_GIVEUP_MS) {
				ok = false; // a dead source, not the end of the track
				break;
			}
			usleep(500 * 1000);
			continue;
		}
		if (status != 206 || !body || fwrite(body, 1, len, f) != len) {
			free(body);
			ok = false;
			break;
		}
		free(body);
		*downloaded += len;
		// The total the server states for the whole resource, learned from the
		// first ranged reply that carries it -- which is usually the first one.
		uint64_t stated = http_last_content_range_total();
		if (stated > total) {
			total = stated;
			if (total_out) *total_out = total;
		}
		// Flushed before it is announced: a reader that is handed this path
		// reads the file, not the writer's stdio buffer, so bytes still sitting
		// in it are bytes the decoder waits for. Only the growing case pays for
		// this -- an ordinary download has no reader until it is over.
		if (hook) fflush(f);
		progress_note(*downloaded);
		stream_note(hook, *downloaded);
		empty_ms = 0;
		if (hook && hook->called) {
			// A short pause between chunk downloads gives the single-core CPU,
			// memory bus and SD card I/O headroom so the playback thread and Bluetooth
			// encoder never suffer buffer starvation or audio stuttering.
			usleep(8 * 1000);
		}
		// The last byte the server promised has arrived: nothing left to ask
		// for, and no 416 needed to say so.
		if (total > 0 && *downloaded >= total) {
			*complete = true;
			break;
		}
		// A short chunk is NOT the end of the file. Invidious proxies the bytes
		// and hands back less than was asked for whenever its own upstream
		// pauses, and taking that for an ending is what cut a three-minute
		// track off after a minute, at a file size the player then reported as
		// a 35 kbps stream. So the next request asks for what follows, and the
		// server says 416 -- or sends nothing at all -- only when there is
		// nothing left.
	}
	if (fclose(f) != 0) ok = false;
	if (total > 0 && *downloaded < total) {
		// A source that answered until it did not, with bytes still owed.
		printf("ytmusic: the source stopped at %llu of %llu bytes\n",
			   (unsigned long long)*downloaded, (unsigned long long)total);
		*complete = false;
	}
	return ok && *complete;
}

// The same file in one transfer, for a server that does not answer Range. This
// is what the download always did, and the reason for the chunked path above.
static bool download_streamed(const char *url, const char *part, uint64_t *downloaded, bool *complete,
							  stream_hook_t *hook) {
	*downloaded = 0;
	*complete = false;
	http_stream_t stream;
	http_stream_options_t options = {.no_icy = true, .no_https_downgrade = true};
	if (!http_stream_open_ex(&stream, url, YTMUSIC_RELAY_TIMEOUT, &options)) {
		return false;
	}
	if (stream.content_length64) progress_expected = stream.content_length64;
	FILE *f = fopen(part, "wb");
	if (!f) {
		http_stream_close(&stream);
		return false;
	}
	char *buffer = malloc(32768);
	bool ok = buffer != NULL;
	while (ok) {
		int n = http_stream_read(&stream, buffer, 32768);
		if (n < 0) {
			ok = false;
			break;
		}
		if (n == 0) break;
		if (fwrite(buffer, 1, (size_t)n, f) != (size_t)n) {
			ok = false;
			break;
		}
		*downloaded += (uint64_t)n;
		if (hook) fflush(f);
		progress_note(*downloaded);
		stream_note(hook, *downloaded);
	}
	free(buffer);
	if (fclose(f) != 0) ok = false;
	if (stream.content_length64 && *downloaded != stream.content_length64) ok = false;
	http_stream_close(&stream);
	*complete = ok;
	return ok;
}

static void prune_playback_cache(const char *keep_id) {
	const char *cache = playback_cache_dir();
	DIR *dir = opendir(cache);
	if (!dir) return;
	struct dirent *entry;
	while ((entry = readdir(dir)) != NULL) {
		if (entry->d_name[0] == '.') continue;
		char *dot = strrchr(entry->d_name, '.');
		if (!dot || (strcmp(dot, ".m4a") != 0 && strcmp(dot, ".tags") != 0 &&
					 strcmp(dot, ".part") != 0 && strcmp(dot, ".jpg") != 0 &&
					 strcmp(dot, ".done") != 0 && strcmp(dot, ".incompleto") != 0)) {
			continue;
		}
		if (strncmp(entry->d_name, keep_id, 11) == 0 && entry->d_name[11] == '.') continue;
		char path[640];
		if (snprintf(path, sizeof(path), "%s/%s", cache, entry->d_name) >= (int)sizeof(path)) continue;
		// The song that just stopped is still somebody's: "save to the card"
		// copies it out on a worker thread, and deleting it here would leave a
		// truncated track on the card. Anything touched in the last two minutes
		// is left alone; the sweep catches it on a later play.
		struct stat st;
		if (stat(path, &st) == 0 && st.st_mtime > time(NULL) - 120) continue;
		remove(path);
	}
	closedir(dir);
}

// Everything the playback cache holds, removed.
//
// The cache is scratch: it lives on the card now, and a card that quietly
// collects a gigabyte of songs nobody asked to keep is worse than one that does
// not. The one file a listener DID ask for is the one "save to the card" copied
// into Music, which is not here. Called on the way down, where Tidal's, Qobuz's
// and the podcasts' caches are cleared for the same reason.
void ytmusic_cache_clear(void) {
	const char *cache = playback_cache_dir();
	DIR *dir = opendir(cache);
	if (!dir) {
		return;
	}
	int removed = 0;
	struct dirent *entry;
	while ((entry = readdir(dir)) != NULL) {
		if (entry->d_name[0] == '.') {
			continue;
		}
		char *dot = strrchr(entry->d_name, '.');
		if (!dot || (strcmp(dot, ".m4a") != 0 && strcmp(dot, ".tags") != 0 && strcmp(dot, ".part") != 0 &&
					 strcmp(dot, ".jpg") != 0 && strcmp(dot, ".done") != 0 && strcmp(dot, ".incompleto") != 0)) {
			continue;
		}
		char path[640];
		if (snprintf(path, sizeof(path), "%s/%s", cache, entry->d_name) >= (int)sizeof(path)) {
			continue;
		}
		if (remove(path) == 0) {
			removed++;
		}
	}
	closedir(dir);
	if (removed > 0) {
		printf("ytmusic: playback cache cleared (%d files)\n", removed);
	}
}

// The cached copy of a song, when there is a whole one.
//
// A `.done` marker is written beside the audio only once the transfer
// finished, and it is what makes this test honest: the file exists is not the
// question, the file is whole is. The tags sidecar cannot answer it any more --
// it is written when playback begins, so that a title and an artist are on
// screen while the file is still arriving -- and a half-downloaded track
// carrying one would pass for a finished song.
bool ytmusic_cached_track(const char *id, char *out, size_t out_size) {
	if (!id || strlen(id) != 11 || !out || out_size == 0) {
		return false;
	}
	const char *cache = playback_cache_dir();
	char final[512], done[520];
	if (snprintf(final, sizeof(final), "%s/%s.m4a", cache, id) >= (int)sizeof(final) ||
		snprintf(done, sizeof(done), "%s.done", final) >= (int)sizeof(done) ||
		strlen(final) >= out_size) {
		return false;
	}
	if (growfile_is_growing(final) || access(final, R_OK) != 0 || access(done, R_OK) != 0) {
		return false;
	}
	snprintf(out, out_size, "%s", final);
	return true;
}

// Reads one key out of a sidecar's "key=value" lines. The sidecars are the
// player's own small files: a line is the whole record.
static bool sidecar_value(const char *tags_path, const char *key, char *out, size_t size) {
	if (!tags_path || !key || !out || size == 0) return false;
	out[0] = '\0';
	FILE *f = fopen(tags_path, "r");
	if (!f) return false;
	size_t key_len = strlen(key);
	char line[512];
	bool found = false;
	while (!found && fgets(line, sizeof(line), f)) {
		if (strncmp(line, key, key_len) != 0 || line[key_len] != '=') continue;
		char *value = line + key_len + 1;
		char *end = value + strlen(value);
		while (end > value && (end[-1] == '\n' || end[-1] == '\r')) *--end = '\0';
		snprintf(out, size, "%s", value);
		found = true;
	}
	fclose(f);
	return found;
}

// Whether a path is one of this module's own finished downloads -- the file the
// player was handed when a YouTube Music song started. That is what the
// now-playing menu's "save to the card" copies out, and nothing else may be
// treated as such: a file the user put on the card is already theirs.
bool ytmusic_cache_track_id(const char *path, char *out, size_t out_size) {
	if (out && out_size) out[0] = '\0';
	if (!path || !path[0]) return false;
	const char *cache = playback_cache_dir();
	size_t prefix_len = strlen(cache);
	if (strncmp(path, cache, prefix_len) == 0 && path[prefix_len] == '/') {
		const char *leaf = path + prefix_len + 1;
		// Eleven characters and the extension: "dQw4w9WgXcQ.m4a".
		if (strlen(leaf) == 11 + 4 && strcmp(leaf + 11, ".m4a") == 0) {
			for (int i = 0; i < 11; i++) {
				if (!isalnum((unsigned char)leaf[i]) && leaf[i] != '-' && leaf[i] != '_') return false;
			}
			if (out && out_size) snprintf(out, out_size, "%.11s", leaf);
			return true;
		}
	}
	if (strstr(path, "/Music/YouTube Music/") != NULL) {
		// A copy kept on the card: the file name is the song's name, so the id
		// is the one the tags beside it carry.
		char tags[600];
		if (out && out_size && snprintf(tags, sizeof(tags), "%s.tags", path) < (int)sizeof(tags) &&
			sidecar_value(tags, "id", out, out_size) && strlen(out) != 11) {
			out[0] = '\0';
		}
		return true;
	}
	return false;
}

bool ytmusic_cache_owns(const char *path) {
	if (!path || !path[0]) return false;
	const char *cache = playback_cache_dir();
	size_t prefix_len = strlen(cache);
	if (strncmp(path, cache, prefix_len) == 0 && path[prefix_len] == '/') {
		const char *leaf = path + prefix_len + 1;
		if (leaf[0] && !strchr(leaf, '/')) {
			size_t n = strlen(leaf);
			if (n > 4 && strcmp(leaf + n - 4, ".m4a") == 0) {
				return true;
			}
		}
	}
	if (strstr(path, "/Music/YouTube Music/") != NULL) {
		return true;
	}
	return false;
}

// Playback: every source the player has, in turn, until one hands over the
// whole track. Resolving is not the same as playing -- a source can name a
// stream that is then refused, which is what the device's own Google address
// does (403 on every media fetch in the field log) -- so a source that keeps no
// bytes is passed over and the next one is asked, each attempt in the log.
// The sleeve.
//
// YouTube's artwork is a URL on the row, not a picture inside the file, and the
// player reads a track's cover from the file it was handed -- or from an image
// beside it named after it (albumart's find_cover_file). So the row's picture is
// fetched and written as <id>.jpg next to <id>.m4a: the cover loader then finds
// it the same way it finds any folder cover, and now-playing has a sleeve.
//
// The URL carries its own sizing directive after the last '=' ("=w120-h120-"),
// so that is replaced rather than the picture being asked for at thumbnail size.
#define YTMUSIC_COVER_MAX (512 * 1024)
#define YTMUSIC_COVER_SIZE 320

static void cover_url_at_size(const char *in, char *out, size_t size) {
	if (!out || !size) return;
	out[0] = '\0';
	if (!in || !in[0]) return;
	const char *eq = strrchr(in, '=');
	size_t base = eq ? (size_t)(eq - in) : strlen(in);
	if (base == 0 || base + 40 >= size) {
		snprintf(out, size, "%.*s", (int)(size - 1), in);
		return;
	}
	snprintf(out, size, "%.*s=w%d-h%d-l90-rj", (int)base, in, YTMUSIC_COVER_SIZE,
			 YTMUSIC_COVER_SIZE);
}

static void save_cover(const char *audio_path, const char *thumb_url) {
	char url[YTMUSIC_URL_MAX];
	cover_url_at_size(thumb_url, url, sizeof(url));
	if (!url[0]) return;
	char cover[600];
	snprintf(cover, sizeof(cover), "%s", audio_path);
	char *dot = strrchr(cover, '.');
	if (dot) *dot = '\0';
	if (strlen(cover) + 5 >= sizeof(cover)) return;
	snprintf(cover + strlen(cover), sizeof(cover) - strlen(cover), ".jpg");
	char *body = NULL;
	size_t len = 0;
	pthread_mutex_lock(&request_lock);
	bool got = http_get_ex(url, NULL, false, &body, &len, YTMUSIC_COVER_MAX, YTMUSIC_TIMEOUT, NULL);
	pthread_mutex_unlock(&request_lock);
	if (!got || !body || len < 1024) {
		fprintf(stderr, "ytmusic: the cover was not fetched (%.96s)\n", url);
		free(body);
		return;
	}
	FILE *f = fopen(cover, "wb");
	bool wrote = f && fwrite(body, 1, len, f) == len;
	if (f) fclose(f);
	free(body);
	if (!wrote) {
		remove(cover);
		return;
	}
	printf("ytmusic: cover saved, %zu bytes\n", len);
}

// `hook` non-NULL means the player is waiting: the bytes go straight into the
// file it will open, the file is announced to growfile so the decoder can read
// it while it fills, and playback is started as soon as there is enough of it.
// NULL is the plain download -- a .part beside it, renamed only when whole.
static bool download_for_playback(const ytmusic_track_t *track, char *path, size_t path_size,
								  stream_hook_t *hook, char *error, size_t error_size) {
	bool streaming = hook != NULL;
	if (error && error_size) error[0] = '\0';
	if (!track || !path || !path_size || strlen(track->id) != 11) {
		set_error(error, error_size, "ytmusic_invalid_track");
		return false;
	}
	const char *cache = playback_cache_dir();
	if (!ensure_cache_dir()) {
		set_error(error, error_size, "ytmusic_cache_unavailable");
		return false;
	}
	char final[512], part[520];
	if (snprintf(final, sizeof(final), "%s/%s.m4a", cache, track->id) >= (int)sizeof(final) ||
		snprintf(part, sizeof(part), "%s.part", final) >= (int)sizeof(part) ||
		strlen(final) >= path_size) {
		set_error(error, error_size, "ytmusic_path_too_long");
		return false;
	}
	prune_playback_cache(track->id);

	if (ytmusic_cache_whole(final)) {
		printf("ytmusic: %.11s: already complete in cache\n", track->id);
		snprintf(path, path_size, "%s", final);
		if (streaming && hook) {
			hook->called = true;
			if (hook->ready) hook->ready(final, hook->user);
		}
		return true;
	}

	if (!streaming && growfile_is_growing(final)) {
		printf("ytmusic: %.11s: waiting for active stream to finish\n", track->id);
		for (int wait_tick = 0; wait_tick < 300 && growfile_is_growing(final); wait_tick++) {
			usleep(100 * 1000);
		}
		if (ytmusic_cache_whole(final)) {
			snprintf(path, path_size, "%s", final);
			return true;
		}
	}

	char sources[YTMUSIC_SOURCES_MAX][YTMUSIC_SOURCE_MAX];
	int count = play_sources(sources, YTMUSIC_SOURCES_MAX);
	char last[192] = "";
	bool kept_any = false;
	// A song that is already playing must not stop where its first instance
	// lost interest. `resume_from` is how many bytes are on disk when a source
	// stops early; the next one is asked for the rest, from that byte, and
	// appends to the same growing file. `resume_expected` is the length the
	// first source stated, because a second instance that picks a different
	// audio stream would splice two files together: the continuation is only
	// offered to one that says the stream is the same size.
	uint64_t resume_from = 0;
	uint64_t resume_expected = 0;
	for (int i = 0; i < count; i++) {
		char label[YTMUSIC_SOURCE_MAX], reason[192] = "", url[YTMUSIC_URL_MAX];
		bool proxied = false;
		uint64_t expected = 0;
		source_label(sources[i], label, sizeof(label));
		printf("ytmusic: %.11s: asking %s\n", track->id, label);
		int source_duration = 0;
		if (!resolve_via(sources[i], track->id, url, sizeof(url), &source_duration, NULL, 0, NULL, 0,
						 &proxied, &expected, reason, sizeof(reason))) {
			fprintf(stderr, "ytmusic: %.11s: %s had nothing (%s)\n", track->id, label,
					reason[0] ? reason : "no reason given");
			continue;
		}
		if (track->duration <= 0 && source_duration > 0) {
			((ytmusic_track_t *)track)->duration = source_duration;
		}
		printf("ytmusic: %.11s: fetching from %s: %s\n", track->id, label, url);
		// The loading card's bar: the total is what this source said the file
		// is, and is 0 when it never said.
		progress_expected = expected;
		progress_percent = -1;
		// A download for playback writes the file the player opens; a plain
		// download keeps its .part until it is whole.
		const char *target = streaming ? final : part;
		if (streaming) {
			// The same song, tapped again while it is still arriving: the
			// file is already being written for a player that is reading it.
			// Starting the transfer over would truncate it under the decoder,
			// so the same growing file is handed over instead.
			//
			// Only on the first pass: past `resume_from` the file is growing
			// because this loop is still writing it, and handing it back here
			// would be answering "it is arriving" without ever finishing it.
			if (resume_from == 0 && growfile_is_growing(final)) {
				hook->called = true;
				snprintf(path, path_size, "%s", final);
				if (hook->ready) hook->ready(final, hook->user);
				return true;
			}
			if (resume_from == 0) {
				hook->path = final;
				hook->ready_at = stream_threshold(expected);
				// Announced before the first byte, because the reader registers
				// itself from the player the moment playback starts, and an
				// unannounced path is an ordinary -- empty -- file to it.
				remove(final);
				// The title, the artist and the sleeve before the first byte. The
				// player is handed this file the moment the index has landed, and
				// whatever is not beside it by then is what it shows: a YouTube
				// Music song used to come up nameless, artistless and without a
				// picture, because both were written only once the transfer ended.
				// The metadata is not a claim that the file is whole any more --
				// that is what the .done marker is for.
				write_playback_tags(final, track);
				save_cover(final, track->thumbnail);
				mark_complete(final, false);
				growfile_announce(final, (long)expected);
			} else if (!hook->path) {
				hook->path = final;
			}
		} else {
			remove(part);
			mark_complete(final, false);
		}
		uint64_t downloaded = 0;
		bool complete = false;
		if (resume_from > 0) {
			if (expected && resume_expected && expected != resume_expected) {
				// A different audio stream: its bytes would not line up with what
				// is already on disk, and a file spliced from two streams is not
				// a song. Left alone rather than made into corruption.
				printf("ytmusic: %.11s: %s serves a %llu-byte stream, not the %llu-byte one being continued\n",
					   track->id, label, (unsigned long long)expected,
					   (unsigned long long)resume_expected);
				break;
			}
			printf("ytmusic: %.11s: %s carries on from byte %llu\n", track->id, label,
				   (unsigned long long)resume_from);
		}
		uint64_t from = streaming ? resume_from : 0;
		// The whole size this source states, learned from its first ranged reply
		// when its API did not name one.
		uint64_t stated_total = expected;
		bool ok = download_ranged(url, target, from, &downloaded, &complete, &stated_total, hook);
		if (!expected && stated_total) {
			// The length the JSON left out, taken from the transfer itself: from
			// here on a file that stops short is a failure with a number in it,
			// not a track that plays for half a minute and ends.
			expected = stated_total;
			progress_expected = expected;
			printf("ytmusic: %.11s: %s keeps %.11s at %llu bytes\n", track->id, label, track->id,
				   (unsigned long long)expected);
			if (streaming) growfile_set_total(final, (long)expected);
		}
		if (!ok && from == 0 && !(hook && hook->called)) {
			// A server that will not answer a Range keeps the old behaviour: one
			// transfer, started over, with the signature problem to live with.
			// Not once playback has begun: starting the file over would truncate
			// it under the decoder that is already reading it.
			printf("ytmusic: %.11s: %s refused ranged chunks, one transfer instead\n",
				   track->id, label);
			remove(target);
			downloaded = 0;
			complete = false;
			if (streaming) {
				// The file starts over, so the search for the index does too:
				// what the first attempt read is not on the disk any more.
				hook->have_moov = false;
				growfile_announce(final, (long)expected);
			}
			ok = download_streamed(url, target, &downloaded, &complete, hook);
		}
		memset(url, 0, sizeof(url));
		// The instance's reply says how many bytes the whole stream is, so a
		// transfer that stopped early is caught here rather than playing as a
		// track that ends after a minute.
		if (ok && complete && expected && downloaded != expected) {
			printf("ytmusic: %.11s: %s stopped at %llu of %llu bytes\n", track->id, label,
				   (unsigned long long)downloaded, (unsigned long long)expected);
			ok = false;
			complete = false;
		}
		// Bytes are not music. A source can hand over a file that is exactly as
		// long as it said it would be and still be a fragment of the track: a
		// preview, a cut-off transcode, an upstream that ended early and said
		// so. The file's own index knows how many seconds it holds, and the row
		// knows how many the track is -- so the two are compared, and a file
		// that is a few seconds short of the song is not the song.
		if (ok && complete && track->duration > 0) {
			int have = media_seconds(target);
			if (have > 0 && have + 2 < track->duration) {
				printf("ytmusic: %.11s: %s served %d s of %d -- not the whole track\n",
					   track->id, label, have, track->duration);
				ok = false;
				complete = false;
			}
		}
		bool landed = ok && complete && downloaded >= 1024;
		if (landed && !streaming) {
			// A whole file replaces whatever the cache held under that name. The
			// remove is for Windows’ rename(), which refuses an existing target
			// where POSIX replaces it; the device never needed it.
			remove(final);
			if (rename(part, final) != 0) landed = false;
		}
		if (landed) {
			write_playback_tags(final, track);
			save_cover(final, track->thumbnail);
			mark_complete(final, true);
			snprintf(path, path_size, "%s", final);
			if (streaming) {
				// A file that is whole is an ordinary file from here on. A track
				// short enough to have finished before the threshold was reached
				// starts the player here instead, at its end.
				if (!hook->called) {
					hook->called = true;
					if (hook->ready) hook->ready(final, hook->user);
				}
				growfile_finish(final, true);
			}
			printf("ytmusic: %.11s: played from %s, %llu bytes\n", track->id, label,
				   (unsigned long long)downloaded);
			return true;
		}
		if (streaming) {
			// Playback is running on what arrived and the transfer has stopped.
			// Another source is asked for the rest first, from the byte it
			// reached: that is the difference between a song that ends after a
			// minute and one that plays to its end when one instance of several
			// is having a bad day.
			// Only a source that stated the same length can continue it: two
			// instances can pick different audio streams for one video, and a file
			// spliced from two of them is not a song. A length nobody stated is
			// no promise at all, so that case is not continued either.
			bool same_stream = expected != 0 && (resume_expected == 0 || expected == resume_expected);
			if (hook->called && downloaded > from && same_stream) {
				if (resume_expected == 0) resume_expected = expected;
				resume_from = downloaded;
				kept_any = true;
				snprintf(last, sizeof(last), "%s", "ytmusic_download_incomplete");
				printf("ytmusic: %.11s: %s stopped at %llu bytes; continuing from there\n",
					   track->id, label, (unsigned long long)downloaded);
				continue;
			}
			// Nothing more is coming. The file has to be *finished*, not just
			// left: its reader is waiting on the download, and a path nobody
			// will write again that still says "growing" makes every read wait
			// out the whole timeout -- a track that stands still instead of
			// ending, which is what stopping after fifty seconds looked like.
			growfile_finish(final, false);
			if (hook->called) {
				// Playback is already running on what arrived. The file is left as
				// it is: removing it would take the decoder's file out from under
				// the listener, and the next play of this song truncates it anyway.
				printf("ytmusic: %.11s: %s stopped at %llu bytes after playback began\n",
					   track->id, label, (unsigned long long)downloaded);
				return true;
			}
			remove(final);
		} else {
			remove(part);
		}
		printf("ytmusic: %.11s: %s kept %llu bytes (%s, %s)\n", track->id, label,
			   (unsigned long long)downloaded, ok ? "complete" : "failed",
			   http_last_error() ? http_last_error() : "no reason given");
		if (downloaded == 0) {
			const char *why = http_last_error() ? http_last_error() : "ytmusic_stream_failed";
			if (strcmp(sources[i], YTMUSIC_VIA_RELAY) == 0) why = "ytmusic_relay_not_serving";
			snprintf(last, sizeof(last), "%s", why);
		} else {
			kept_any = true;
			snprintf(last, sizeof(last), "%s", "ytmusic_download_incomplete");
		}
	}
	// Reaching here with a file still being written means every source gave up
	// on it: the readers are told, so the decoder ends the track instead of
	// waiting on a download that is not there.
	if (streaming) {
		growfile_finish(final, false);
	}
	set_error(error, error_size,
			  last[0] ? last : (kept_any ? "ytmusic_download_incomplete" : "ytmusic_no_source"));
	return false;
}

// The entry the page calls. The work is download_for_playback above; this marks
// the span for the loading card, so a bar is shown from the first byte to the
// last -- a track takes long enough over a public instance that being told
// something is happening is the difference between waiting and thinking the
// player has hung.
bool ytmusic_download_for_playback(const ytmusic_track_t *track, char *path, size_t path_size,
								   char *error, size_t error_size) {
	progress_expected = 0;
	progress_percent = -1;
	progress_active = 1;
	bool ok = download_for_playback(track, path, path_size, NULL, error, error_size);
	progress_active = 0;
	progress_expected = 0;
	progress_percent = -1;
	return ok;
}

// The same download, started for a listener who is waiting to hear it: the
// loading card's span covers the whole transfer, and `ready` fires part-way
// through it so the page can hand the growing file to the player.
bool ytmusic_stream_for_playback(const ytmusic_track_t *track, char *path, size_t path_size,
								 ytmusic_ready_cb ready, void *user,
								 char *error, size_t error_size) {
	stream_hook_t hook = {.path = NULL, .ready_at = 0, .ready = ready, .user = user, .called = false};
	progress_expected = 0;
	progress_percent = -1;
	progress_active = 1;
	bool ok = download_for_playback(track, path, path_size, &hook, error, error_size);
	progress_active = 0;
	progress_expected = 0;
	progress_percent = -1;
	return ok;
}
