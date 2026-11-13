#ifndef WEBDAV_H
#define WEBDAV_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Lightweight WebDAV folder browser and account configuration (RFC 4918).
// All network calls are blocking and belong on a background worker thread,
// never on the LVGL GUI thread or ALSA playback thread.

#define WEBDAV_SERVER_MAX 512
#define WEBDAV_USER_MAX 128
#define WEBDAV_PASSWORD_MAX 256
#define WEBDAV_PATH_MAX 512
#define WEBDAV_TITLE_MAX 200
#define WEBDAV_URL_MAX 2048
#define WEBDAV_AUTH_HEADER_MAX 640
#define WEBDAV_PAGE_SIZE 200
#define WEBDAV_QUEUE_MAX 500

typedef struct {
	char server[WEBDAV_SERVER_MAX]; // normalized base URL (no trailing slash)
	char username[WEBDAV_USER_MAX]; // empty for anonymous WebDAV servers
	char password[WEBDAV_PASSWORD_MAX];
} webdav_credentials_t;

typedef enum {
	WEBDAV_DIR,
	WEBDAV_TRACK,
} webdav_entry_kind_t;

typedef struct {
	webdav_entry_kind_t kind;
	char path[WEBDAV_PATH_MAX];   // decoded path relative to server root, e.g. "Jazz/01 - So What.flac"
	char title[WEBDAV_TITLE_MAX]; // display label (filename or folder name)
	char album[WEBDAV_TITLE_MAX]; // immediate parent folder name, "" at root
	char suffix[16];              // lowercase extension without dot, e.g. "flac"
	uint64_t size;                // bytes reported by getcontentlength, 0 if omitted
} webdav_entry_t;

void webdav_init(void);
bool webdav_configure(const webdav_credentials_t *credentials, bool remember, char *error, size_t error_size);
void webdav_credentials(webdav_credentials_t *out, bool *remembered);
bool webdav_configured(void);
bool webdav_forget(void);
bool webdav_api_busy(void);

// Playback mode setting: false = lightweight 2 MiB RAM ring stream (zero SD writes);
// true = stream through bounded SD card cache (.local/webdav-cache) with seeking and all local codecs.
bool webdav_sd_cache(void);
void webdav_set_sd_cache(bool enabled);

// Verifies server reachability, authentication, and WebDAV PROPFIND (Depth: 0).
bool webdav_ping(char *error, size_t error_size);

// Lists direct children of `rel_path` ("" = server root), folders first then
// recognized audio tracks, paginated by `offset` and `limit` (up to WEBDAV_PAGE_SIZE).
// Returns entry count (>= 0) on success, -1 on error.
int webdav_browse(const char *rel_path, int offset, webdav_entry_t *entries, int limit, bool *more,
				  char *error, size_t error_size);

// Builds the percent-encoded GET URL and optional "Authorization: Basic ...\r\n"
// header for `rel_path` without embedding credentials in the URL.
bool webdav_file_request_info(const char *rel_path, char *url_out, size_t url_size,
							  char *auth_header_out, size_t auth_size,
							  char *error, size_t error_size);

// Canonical account identity ("<server>\n<username>") for cache keying.
bool webdav_server_key(char *out, size_t size);

// True if `ext` (without leading dot, case-insensitive) is a supported audio file.
bool webdav_is_audio_suffix(const char *ext);

// True if `ext` can be decoded directly from the forward-only RAM streambridge.
bool webdav_is_ram_streamable_suffix(const char *ext);

#endif /* WEBDAV_H */
