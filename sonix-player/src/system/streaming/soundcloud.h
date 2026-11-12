#ifndef SOUNDCLOUD_H
#define SOUNDCLOUD_H

#include <stdbool.h>
#include <stddef.h>

// SoundCloud (web API v2, compatible with both Free and Go / Go+ accounts).
//
// Every network call blocks and belongs on a worker thread, never on the GUI or
// audio thread.
//
// Two ways to connect an account, and Search needs neither:
//
//   1. Username or profile URL only (no token, no API keys): resolves the
//      public profile id and reads public Liked tracks, Reposts and Playlists.
//   2. OAuth token (optional, from a browser session or streaming-keys.ini):
//      authenticates via `Authorization: OAuth <token>`, resolves `/me`
//      automatically, unlocks private likes/playlists, Likes writes from the
//      player, and full-length / high-quality streams on SoundCloud Go / Go+.
//
// `client_id` is discovered automatically from SoundCloud's web bundle and
// cached in device_config.ini, rotating automatically on 401/403; an explicit
// override in Account settings or streaming-keys.ini takes precedence.

#define SOUNDCLOUD_TITLE_MAX 200
#define SOUNDCLOUD_NAME_MAX 128
#define SOUNDCLOUD_USER_MAX 128
#define SOUNDCLOUD_TOKEN_MAX 256
#define SOUNDCLOUD_CLIENT_ID_MAX 64
#define SOUNDCLOUD_SECRET_MAX 64
#define SOUNDCLOUD_URL_MAX 1536
#define SOUNDCLOUD_CURSOR_MAX 1536
#define SOUNDCLOUD_PAGE_LIMIT 50

#define SOUNDCLOUD_USER_AGENT \
	"Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/131.0.0.0 Safari/537.36"

typedef enum {
	SOUNDCLOUD_QUALITY_LOW = 0,		 // Opus ~64 kbps (HLS), fallback MP3 128 kbps
	SOUNDCLOUD_QUALITY_STANDARD = 1, // MP3 128 kbps (progressive or HLS)
	SOUNDCLOUD_QUALITY_HIGH = 2,	 // AAC 256 kbps (Go+ HLS) when available, fallback MP3 128 kbps
} soundcloud_quality_t;

typedef struct {
	char username[SOUNDCLOUD_USER_MAX];
	char oauth_token[SOUNDCLOUD_TOKEN_MAX];
	char client_id[SOUNDCLOUD_CLIENT_ID_MAX];
} soundcloud_credentials_t;

typedef struct {
	long long user_id;
	char display_name[SOUNDCLOUD_NAME_MAX];
	char permalink[SOUNDCLOUD_USER_MAX];
	char tier[32]; // "Free", "Go", "Go+", or ""
	bool has_token;
	bool remembered;
} soundcloud_account_info_t;

typedef struct {
	long long id;
	char title[SOUNDCLOUD_TITLE_MAX];
	char artist[SOUNDCLOUD_NAME_MAX];
	char album[SOUNDCLOUD_TITLE_MAX];
	char cover[SOUNDCLOUD_URL_MAX];
	char secret_token[SOUNDCLOUD_SECRET_MAX];
	int duration; // seconds (for 30s Go+ previews on Free, the playable snippet length)
	int full_duration;
	bool streamable;
	bool preview; // true when restricted to a 30s Go+ preview on the current tier
} soundcloud_track_t;

typedef struct {
	long long id;
	char title[SOUNDCLOUD_TITLE_MAX];
	char owner[SOUNDCLOUD_NAME_MAX];
	char cover[SOUNDCLOUD_URL_MAX];
	char secret_token[SOUNDCLOUD_SECRET_MAX];
	int track_count;
	bool is_liked_playlist;
} soundcloud_playlist_t;

typedef struct {
	bool is_playlist;
	soundcloud_track_t track;
	soundcloud_playlist_t playlist;
} soundcloud_repost_t;

typedef enum {
	SOUNDCLOUD_STREAM_PROGRESSIVE = 0,
	SOUNDCLOUD_STREAM_HLS = 1,
} soundcloud_stream_kind_t;

typedef enum {
	SOUNDCLOUD_CODEC_MP3 = 0,
	SOUNDCLOUD_CODEC_OPUS = 1,
	SOUNDCLOUD_CODEC_AAC = 2, // fMP4 or ADTS AAC unwrapped to .aac
} soundcloud_codec_t;

typedef struct {
	soundcloud_stream_kind_t kind;
	soundcloud_codec_t codec;
	char url[SOUNDCLOUD_URL_MAX]; // direct audio URL or .m3u8 manifest URL
	int duration_secs;
	bool snipped;
} soundcloud_stream_t;

// Startup initialisation (call after config_init() and streamkeys_init()).
void soundcloud_init(void);
void soundcloud_reload_streamkeys(void);

// Account configuration & state. `soundcloud_configure` validates against the
// API and blocks; call it from a worker thread.
bool soundcloud_configure(const soundcloud_credentials_t *credentials, bool remember, char *error, size_t error_size);
void soundcloud_credentials(soundcloud_credentials_t *out, bool *remembered);
void soundcloud_account_info(soundcloud_account_info_t *out);
bool soundcloud_has_account(void);
bool soundcloud_can_like(void);
bool soundcloud_forget(void);

int soundcloud_get_quality(void);
void soundcloud_set_quality(int quality);
const char *soundcloud_expected_mime(void);

bool soundcloud_api_busy(void);
const char *soundcloud_last_error(void);

// Upgrades a SoundCloud `-large.jpg` (100x100) URL to `-t500x500.jpg` or `-t300x300.jpg`.
void soundcloud_artwork_url(const char *raw, int size_px, char *out, size_t out_size);

// Extra HTTP headers (`Origin` / `Referer`, plus `Authorization: OAuth ...` only
// when `for_url` targets the SoundCloud API host).
void soundcloud_headers_for_url(const char *for_url, char *out, size_t out_size);

// Browsing & search (all block; return item count >= 0, or -1 on error with
// `soundcloud_last_error()` set to a translation key or message).
int soundcloud_search_tracks(const char *query, int offset, soundcloud_track_t *out, int max, bool *more);
int soundcloud_liked_tracks(const char *cursor, soundcloud_track_t *out, int max, char *next_cursor,
							size_t next_cursor_size);
int soundcloud_reposts(const char *cursor, soundcloud_repost_t *out, int max, char *next_cursor,
					   size_t next_cursor_size);
int soundcloud_playlists(const char *cursor, soundcloud_playlist_t *out, int max, char *next_cursor,
						 size_t next_cursor_size);
int soundcloud_playlist_tracks(long long playlist_id, const char *secret_token, int offset, soundcloud_track_t *out,
							   int max, bool *more);

// Resolves a fresh playable stream URL / HLS manifest URL for `track_id`.
bool soundcloud_get_stream(long long track_id, const char *secret_token, bool allow_aac, soundcloud_stream_t *out);

//Session-mirrored likes for the Now Playing favourite button.
bool soundcloud_is_liked(long long track_id);
void soundcloud_note_liked(long long track_id, bool liked);
bool soundcloud_like_track(long long track_id, bool like);

// Test hook: redirect API v2 and web HTML bundle discovery to a local mock server.
void soundcloud_set_endpoints_for_test(const char *api_url, const char *web_url);

#endif /* SOUNDCLOUD_H */
