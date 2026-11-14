#ifndef JELLYFIN_H
#define JELLYFIN_H

#include <stdbool.h>
#include <stddef.h>

// Jellyfin self-hosted music client.
//
// Every network call blocks and belongs on a worker thread, never on the GUI or
// audio thread. No third-party application API key is required.
//
// Two authentication methods are supported:
//
//   1. Username + password (`POST /Users/AuthenticateByName`): exchanges the
//      password once for a revocable Jellyfin AccessToken. The password is
//      never written to disk.
//   2. Quick Connect (`POST /QuickConnect/Initiate` -> `GET /QuickConnect/Connect`
//      -> `POST /Users/AuthenticateWithQuickConnect`): shows a 6-digit code
//      (and QR link) to authorize from a phone or browser without typing a
//      password on the 2-inch touchscreen.
//
// Authentication uses the standard `Authorization: MediaBrowser ...` header,
// strictly restricted to the configured server origin (`scheme://host:port`).

#define JELLYFIN_SERVER_MAX 512
#define JELLYFIN_USER_MAX 128
#define JELLYFIN_PASSWORD_MAX 256
#define JELLYFIN_TOKEN_MAX 256
#define JELLYFIN_DEVICE_ID_MAX 64
#define JELLYFIN_ID_MAX 64
#define JELLYFIN_TITLE_MAX 200
#define JELLYFIN_NAME_MAX 128
#define JELLYFIN_CONTAINER_MAX 16
#define JELLYFIN_URL_MAX 2048
#define JELLYFIN_PAGE_SIZE 50

#define JELLYFIN_CLIENT_NAME "SonixPlayer"
#define JELLYFIN_DEVICE_NAME "TempoTec V1"
#define JELLYFIN_CLIENT_VERSION "1.0"

typedef enum {
	JELLYFIN_QUALITY_ORIGINAL = 0, // Direct play original file (static=true)
	JELLYFIN_QUALITY_MP3_320 = 1,  // Server-side MP3 320 kbps transcode
	JELLYFIN_QUALITY_MP3_192 = 2,  // Server-side MP3 192 kbps transcode
	JELLYFIN_QUALITY_OPUS_128 = 3, // Server-side Opus 128 kbps (Ogg) transcode
} jellyfin_quality_t;

typedef enum {
	JELLYFIN_ITEM_FOLDER = 0,
	JELLYFIN_ITEM_ARTIST = 1,
	JELLYFIN_ITEM_ALBUM = 2,
	JELLYFIN_ITEM_PLAYLIST = 3,
	JELLYFIN_ITEM_GENRE = 4,
	JELLYFIN_ITEM_TRACK = 5,
} jellyfin_item_kind_t;

typedef struct {
	jellyfin_item_kind_t kind;
	char id[JELLYFIN_ID_MAX];
	char title[JELLYFIN_TITLE_MAX];
	char artist[JELLYFIN_NAME_MAX];
	char album[JELLYFIN_TITLE_MAX];
	char album_id[JELLYFIN_ID_MAX];
	char cover_id[JELLYFIN_ID_MAX];
	char container[JELLYFIN_CONTAINER_MAX]; // lowercase extension/container, e.g. "flac", "mp3"
	int duration;							// seconds (from 64-bit RunTimeTicks)
	int track_number;
	int disc_number;
	int year;
	int child_count;
	bool is_favorite;
} jellyfin_entry_t;

typedef enum {
	JELLYFIN_BROWSE_LIBRARIES = 0,
	JELLYFIN_BROWSE_FOLDERS = 1,
	JELLYFIN_BROWSE_FOLDER = 2,
	JELLYFIN_BROWSE_PLAYLISTS = 3,
	JELLYFIN_BROWSE_PLAYLIST_ITEMS = 4,
	JELLYFIN_BROWSE_ARTISTS = 5,
	JELLYFIN_BROWSE_ARTIST_ALBUMS = 6,
	JELLYFIN_BROWSE_ALBUMS = 7,
	JELLYFIN_BROWSE_RECENT = 8,
	JELLYFIN_BROWSE_ALBUM_TRACKS = 9,
	JELLYFIN_BROWSE_GENRES = 10,
	JELLYFIN_BROWSE_GENRE_ALBUMS = 11,
	JELLYFIN_BROWSE_FAVORITES = 12,
	JELLYFIN_BROWSE_SEARCH = 13,
} jellyfin_browse_t;

typedef struct {
	char server[JELLYFIN_SERVER_MAX]; // base URL, including any reverse-proxy prefix
	char username[JELLYFIN_USER_MAX];
	char password[JELLYFIN_PASSWORD_MAX]; // transient only, never persisted to disk
} jellyfin_credentials_t;

typedef struct {
	char server[JELLYFIN_SERVER_MAX];
	char server_name[JELLYFIN_NAME_MAX];
	char user_id[JELLYFIN_ID_MAX];
	char username[JELLYFIN_USER_MAX];
	char library_id[JELLYFIN_ID_MAX];
	char library_name[JELLYFIN_TITLE_MAX];
	bool logged_in;
	bool remembered;
} jellyfin_account_info_t;

typedef struct {
	char server[JELLYFIN_SERVER_MAX];
	char code[32];
	char secret[128];
	char web_url[JELLYFIN_SERVER_MAX + 32];
} jellyfin_qc_t;

typedef enum {
	JELLYFIN_QC_PENDING = 0,
	JELLYFIN_QC_OK = 1,
	JELLYFIN_QC_EXPIRED = 2,
	JELLYFIN_QC_DISABLED = 3,
	JELLYFIN_QC_ERROR = 4,
} jellyfin_qc_state_t;

// Startup initialisation (call once after config_init()). Loads optional
// 0600 `jellyfin-credentials.json` (token + user id, never a password).
void jellyfin_init(void);

// Normalizes and validates a Jellyfin server base URL (http:// or https://,
// IPv4/DNS host, optional port and reverse-proxy path prefix; strips trailing
// slashes).
bool jellyfin_normalize_server(const char *raw, char *out, size_t out_size, char *error, size_t error_size);

// Authenticates with username + password against `credentials->server`.
// If `remember` is true, persists the revocable AccessToken (mode 0600) but
// NEVER writes the password to disk.
bool jellyfin_authenticate(const jellyfin_credentials_t *credentials, bool remember, char *error, size_t error_size);

// Updates the remember-on-disk preference for the currently signed-in session
// (writes or deletes `jellyfin-credentials.json`).
bool jellyfin_set_remember(bool remember, char *error, size_t error_size);

// Quick Connect flow. `jellyfin_qc_initiate()` contacts `server_url` and fills
// `out->code`, `out->secret` and `out->web_url`. `jellyfin_qc_poll()` checks
// authorization status and, once authorized, exchanges the secret for an
// AccessToken and saves the session (honoring `remember`).
jellyfin_qc_state_t jellyfin_qc_initiate(const char *server_url, jellyfin_qc_t *out, char *error, size_t error_size);
jellyfin_qc_state_t jellyfin_qc_poll(const jellyfin_qc_t *qc, bool remember, char *error, size_t error_size);

// Active account snapshot & configuration helpers.
void jellyfin_account_info(jellyfin_account_info_t *out);
bool jellyfin_logged_in(void);
bool jellyfin_set_library(const char *library_id, const char *library_name);
bool jellyfin_forget(void);
// Best-effort network logout + local forget (call from worker thread).
bool jellyfin_logout_and_forget(void);

// Validates the current session against `/Users/Me` and refreshes server name.
bool jellyfin_ping(char *error, size_t error_size);

jellyfin_quality_t jellyfin_get_quality(void);
void jellyfin_set_quality(jellyfin_quality_t quality);

bool jellyfin_report_playback_enabled(void);
void jellyfin_set_report_playback(bool enabled);

bool jellyfin_api_busy(void);
const char *jellyfin_last_error(void);

// Builds `Authorization: MediaBrowser ...` header ONLY when `for_url` matches
// the configured Jellyfin server's scheme, host and port. Writes an empty
// string otherwise (strict host isolation).
void jellyfin_headers_for_url(const char *for_url, char *out, size_t out_size);

// Builds an unauthenticated MediaBrowser header for `for_url` (used before
// login, or for explicit server checks).
void jellyfin_device_id(char *out, size_t out_size);

// Server-paginated browsing and search (`StartIndex` + `Limit`).
// Returns entry count >= 0, or -1 on error (`error` receives a translation key
// or server message). `more` is set when additional pages exist.
int jellyfin_browse(jellyfin_browse_t browse, const char *id_or_query, int offset, jellyfin_entry_t *entries, int limit,
					bool *more, char *error, size_t error_size);

// URL construction (no network). `container_hint` may be empty.
// Sets `*will_transcode` if non-NULL to indicate DirectStream vs Transcode.
bool jellyfin_stream_url(const char *item_id, jellyfin_quality_t quality, const char *container_hint, char *out,
						 size_t size, char *ext_out, size_t ext_size, bool *will_transcode, char *error,
						 size_t error_size);

// Resized album/item Primary cover URL (`maxWidth=300&quality=80`).
bool jellyfin_cover_url(const char *cover_id, char *out, size_t size);

// Session-mirrored favourites (populated from API responses and live toggles).
bool jellyfin_is_favorite(const char *item_id);
void jellyfin_note_favorite(const char *item_id, bool favorite);
bool jellyfin_set_favorite(const char *item_id, bool favorite, char *error, size_t error_size);

// Playback reporting (`/Sessions/Playing`, `/Progress`, `/Stopped`).
// Synchronous calls block and belong on a worker thread (also used by tests).
bool jellyfin_report_playback_start(const char *item_id, const char *play_session_id, long long position_ticks,
									bool transcoded);
bool jellyfin_report_playback_progress(const char *item_id, const char *play_session_id, long long position_ticks,
									   bool is_paused, bool transcoded);
bool jellyfin_report_playback_stopped(const char *item_id, const char *play_session_id, long long position_ticks);

// Non-blocking async reporting queue safe to call from the GUI watchdog.
void jellyfin_report_start_async(const char *item_id, const char *play_session_id, long long position_ticks,
								 bool transcoded);
void jellyfin_report_progress_async(const char *item_id, const char *play_session_id, long long position_ticks,
									bool is_paused, bool transcoded);
void jellyfin_report_stopped_async(const char *item_id, const char *play_session_id, long long position_ticks);

// Returns true if the given lowercase container/extension can be direct-played
// natively by Sonix Player without server-side transcoding.
bool jellyfin_container_playable(const char *container);

#endif /* JELLYFIN_H */
