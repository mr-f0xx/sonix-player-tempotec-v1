#ifndef YTMUSIC_H
#define YTMUSIC_H

#include <stdbool.h>
#include <stddef.h>

#define YTMUSIC_TRACKS_MAX 50
#define YTMUSIC_ID_MAX 16
#define YTMUSIC_TITLE_MAX 200
#define YTMUSIC_NAME_MAX 128
#define YTMUSIC_URL_MAX 2048
#define YTMUSIC_QUEUE_MAX 60
#define YTMUSIC_BROWSE_ID_MAX 128
#define YTMUSIC_ITEMS_MAX 24
// A row's artwork URL: the largest size the page offers, which for the WEB_REMIX
// client is the short yt3.googleusercontent.com form, a little over 130
// characters. Rows are held in arrays of a hundred-odd entries, so the buffer is
// kept to the shape that is actually served rather than the 2 KB a stream URL
// needs; a URL longer than this leaves the row with its placeholder glyph.
#define YTMUSIC_ART_URL_MAX 160
#define YTMUSIC_SECTIONS_MAX 6
#define YTMUSIC_SECTION_ITEMS_MAX 12

typedef struct {
	char id[YTMUSIC_ID_MAX];
	char title[YTMUSIC_TITLE_MAX];
	char artist[YTMUSIC_NAME_MAX];
	char album[YTMUSIC_TITLE_MAX];
	char thumbnail[YTMUSIC_URL_MAX];
	int duration;
	bool podcast_episode;
} ytmusic_track_t;

// One row of a list: either something that plays (a song, a podcast episode),
// or something that opens another list (a playlist, an album, a podcast show).
//
// YouTube Music answers every one of those from the same page shape, so the
// page keeps one kind of row and lets the id decide what a tap does. The two
// ids are mutually exclusive: a track has the eleven-character video id, a
// container the browse id ("VL..." playlist, "MPRE..." album, "MPSP..." show).
typedef enum {
	YTMUSIC_ITEM_TRACK = 0,
	YTMUSIC_ITEM_CONTAINER,
} ytmusic_item_kind_t;

typedef struct {
	ytmusic_item_kind_t kind;
	char id[YTMUSIC_ID_MAX];
	char browse_id[YTMUSIC_BROWSE_ID_MAX];
	char title[YTMUSIC_TITLE_MAX];
	char subtitle[YTMUSIC_NAME_MAX];
	// The album the row named, when the page carried one: a song row's second
	// column is "artist • album • duration". Empty on pages that do not name
	// it (an album's own track list), where the list's title stands in for it.
	char album[YTMUSIC_TITLE_MAX];
	// The artwork the list draws on the row, and empty when the page carried
	// none: the row then keeps the glyph it was built with.
	char thumbnail[YTMUSIC_ART_URL_MAX];
	int duration;
	bool podcast_episode;
	bool played;
} ytmusic_item_t;

// A titled row of the Explore page -- "New albums &amp; singles", "Trending" --
// with the handful of cards under it. The cards are loaded with the page rather
// than on the tap, so opening a row costs no network round trip.
typedef struct {
	char title[64];
	int count;
	ytmusic_item_t items[YTMUSIC_SECTION_ITEMS_MAX];
} ytmusic_section_t;

// Which part of the catalogue a search asks for. Songs is the default; Podcasts
// returns shows, which open like any other container.
typedef enum {
	YTMUSIC_FILTER_SONGS = 0,
	YTMUSIC_FILTER_ALBUMS,
	YTMUSIC_FILTER_ARTISTS,
	YTMUSIC_FILTER_PODCASTS,
} ytmusic_filter_t;

// Call after config_init(). The imported cookie is stored in a private sidecar
// beside device_config.ini, outside config.c's small per-value limit, and
// survives player reboots.
void ytmusic_init(void);

// True when an API key or relay is configured.
bool ytmusic_configured(void);

bool ytmusic_set_session_cookie(const char *cookie, char *error, size_t error_size);
// One-time import from <card_root>/ytmusic_session.txt. The file contains a
// single Cookie header value exported by a trusted YouTube Music sign-in
// helper. Successful import removes the card copy after saving it internally.
bool ytmusic_import_session_file(const char *card_root, char *error, size_t error_size);
void ytmusic_session_cookie(char *out, size_t size);
// Whether an account is attached at all, which is whether the imported browser
// session is: it is the only kind there is, and the honest answer to "will the
// Favourites page show the account's list".
bool ytmusic_session_cookie_attached(void);
bool ytmusic_logout(void);

// Blocking network operations. Call only from a service worker, never from
// the LVGL or audio thread.
//
// All three fill `items` and return how many there are, 0 for an honest empty
// page, or -1 with a tag in `error`. Public pages -- search, charts, Explore --
// still answer when nobody is signed in, and an account whose session Google
// refuses falls back to that anonymous answer instead of failing.
int ytmusic_search_items(const char *query, ytmusic_filter_t filter,
						 ytmusic_item_t *items, int limit, char *error, size_t error_size);
// A playlist ("VL..."), an album ("MPRE..."), a podcast show ("MPSP..."), the
// charts ("FEmusic_charts"), or the liked songs ("VLLM", including on-player
// likes when a session is attached).
int ytmusic_browse_items(const char *browse_id, ytmusic_item_t *items, int limit,
						 char *error, size_t error_size);
// Explore: the carousels of the store's front page, each with a few cards.
int ytmusic_explore_sections(ytmusic_section_t *sections, int limit,
							 char *error, size_t error_size);

// One track's address, and what the relay said about it. `relay_proxied`
// (optional) is set when that address is the relay's own /audio endpoint rather
// than one the relay merely named; see ytmusic_relay_stream_url().
bool ytmusic_audio_url(const char *video_id, char *url, size_t url_size,
					   int *duration, char *title, size_t title_size,
					   char *artist, size_t artist_size, bool *relay_proxied,
					   char *error, size_t error_size);

// Where the bytes of one track are fetched from, given the address the relay
// answered with: that address when it is the relay's own, and the relay's
// /audio/<video-id> -- the endpoint tools/ytmusic-relay.py serves -- when it is
// somebody else's, which the player cannot fetch. `proxied` says which of the
// two was chosen. False when there is nothing to build from.
// `itag` is carried into the proxied address, so the relay serves the format
// this end chose; 0 leaves it out.
bool ytmusic_relay_stream_url(const char *base, const char *video_id, long itag, const char *reported,
							  char *out, size_t out_size, bool *proxied);

// ---------------------------------------------------------------------------
// Where playback comes from
//
// Three answers, and the player chooses without being told: the device itself
// (the player request it makes on its own -- no account, no helper machine), a
// Piped or Invidious instance, and the LAN relay in [ytmusic] relay_base.
//
// "auto" is the default and tries them in that order. The other values pin one
// first -- "direct", "relay", or an instance's https:// address -- and the
// rest are still tried after it, because a pinned host being down should not
// mean silence. The settings page lists exactly what these return.
#define YTMUSIC_SOURCE_MAX 96
#define YTMUSIC_SOURCES_MAX 12

typedef struct {
	// What the settings store keeps, and what ytmusic_source_select() takes:
	// "auto", "direct", "relay", or the instance's base address.
	char value[YTMUSIC_SOURCE_MAX];
	// What the row shows: a translated tag (for the three built-in answers) or
	// the instance's host name.
	char label[YTMUSIC_SOURCE_MAX];
} ytmusic_source_t;

// The choices, the default first. Returns how many were written.
int ytmusic_sources(ytmusic_source_t *out, int limit);
// The value in force, "auto" when nothing was ever chosen.
void ytmusic_source_active(char *out, size_t size);
// Stores a choice from ytmusic_sources(). False when the value is not one of
// them -- an address that is not http(s), or one too long to send.
bool ytmusic_source_select(const char *value);
// The instance addresses tried, in order: the configured [ytmusic] instances
// list when there is one, the built-in public list otherwise, and the pinned
// instance first when one is pinned. Fills `out` with the addresses and
// returns how many: each is a base like "https://pipedapi.kavin.rocks", with
// no trailing slash.
int ytmusic_instance_bases(char out[][YTMUSIC_SOURCE_MAX], int limit);

// Fetches one AAC/MP4 audio stream to the temporary playback cache and returns
// only when the file is whole. This is a blocking operation; call from a worker,
// not from the UI or audio thread. Used by the explicit "keep on card"
// download, which has nothing to play while it runs.
bool ytmusic_download_for_playback(const ytmusic_track_t *track, char *path, size_t path_size,
								   char *error, size_t error_size);

// The file of a song that is already in the playback cache, whole. A second play
// of a song -- or a play of one that finished downloading a moment ago -- goes
// straight to the player: there is nothing left to fetch, and a card saying so
// would be a prompt about work that is not happening. False when the cache holds
// no whole copy of that id.
bool ytmusic_cached_track(const char *id, char *out, size_t out_size);

// The same download, for a song the listener is waiting to hear. `ready` is
// called once, from this worker thread, as soon as enough of the file is on the
// disk for the decoder to open it -- `path` being the file to hand to the
// player. The transfer then carries on behind playback, so a song starts in
// seconds instead of after the whole thing has arrived, and `growfile` is what
// lets the decoder wait for the rest of each frame. Blocking; call from a
// worker. Returns false when nothing could be played at all: a failure after
// `ready` was called still returns true, because the listener already has
// audio and the file simply ends where the transfer did.
typedef void (*ytmusic_ready_cb)(const char *path, void *user);
bool ytmusic_stream_for_playback(const ytmusic_track_t *track, char *path, size_t path_size,
								 ytmusic_ready_cb ready, void *user,
								 char *error, size_t error_size);

// The name that song's cache file has, whether or not anything has been
// downloaded yet. This is what the page builds its queue from: YouTube Music
// has no files of its own until the cache makes them, so the list the listener
// tapped in becomes a queue of the paths those songs will have. False when the
// id is not eleven characters or the path does not fit.
bool ytmusic_cache_path_for(const char *id, char *out, size_t out_size);

// Whether that cache file is whole -- downloaded to its end, with the marker
// that says so. A file still arriving is playable and answers false here.
bool ytmusic_cache_whole(const char *path);
// True only for an episode opened from the YouTube Music Podcasts tab.
bool ytmusic_cache_is_podcast_episode(const char *path);

// Writes the track's title, artist and album beside its cache file before any
// of it is downloaded, so a queued song that has not arrived yet still has a
// name in the player and in the queue. Text only; the sleeve comes with the
// audio.
bool ytmusic_cache_write_sidecars(const ytmusic_track_t *track, const char *path);

// Whether `path` is a finished download of that cache -- what the player was
// handed for a YouTube Music song, and the only kind of file the "save to the
// card" action copies out of it.
bool ytmusic_cache_owns(const char *path);

// The video id of one of those cache files ("<id>.m4a"), which is what a like
// is kept under. False when `path` is not one.
bool ytmusic_cache_track_id(const char *path, char *out, size_t out_size);

// Where the playback cache lives: the card's own .local folder, so a song being
// streamed is never held in RAM. Told once the storage has looked for the card;
// without one the cache falls back to /tmp, which is memory on this device, so
// a player with a card never pays for a song twice.
void ytmusic_set_cache_root(const char *sd_root);
// Where that cache is; the boot sweep drops favourites that pointed into it.
const char *ytmusic_cache_dir(void);

// Empties the playback cache. Called on the way down with the other streaming
// caches: it is scratch that now lives on the card, and so has to be swept.
void ytmusic_cache_clear(void);

// How far the download in flight has got, for the page's loading card. Percent
// is -1 until the instance has said how big the file is, and `running` is false
// when nothing is downloading.
void ytmusic_download_progress(int *percent, bool *running);

// The bitrate of the stream the last resolution chose, in kbit/s, or 0 when the
// source never said. A cache file that is still being written has no honest
// average to compute -- its size over its length read as "3 kbps" on screen --
// so the now-playing page shows what the instance said instead.
int ytmusic_last_bitrate_kbps(void);

// The country whose store the catalogue answers from, and the language its
// labels come back in ("PL"/"en"): what the region row on the settings page
// stores, and what turns Explore into Polish recommendations while the rows stay
// in English. Both are two letters, and the page offers only real pairs.
#define YTMUSIC_REGION_MAX 8
typedef struct {
	char region[YTMUSIC_REGION_MAX];   // "US", "PL", a country's store
	char language[YTMUSIC_REGION_MAX]; // "en", "pl", the row language
	char label[48];                    // "Poland, English rows"
} ytmusic_region_t;

// The choices, the default first.
int ytmusic_regions(ytmusic_region_t *out, int limit);
// The pair in force.
void ytmusic_region_active(char *region, size_t region_size, char *language, size_t language_size);
// Stores a pair. False when either code is not two letters.
bool ytmusic_region_select(const char *region, const char *language);

// The audio quality row: high takes 256 kbit/s AAC where an instance offers it
// (itag 141) and the 128 kbit/s one everywhere else; standard always takes 128.
bool ytmusic_quality_high(void);
void ytmusic_quality_set(bool high);

#endif /* YTMUSIC_H */
