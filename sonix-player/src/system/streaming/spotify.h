#ifndef SPOTIFY_H
#define SPOTIFY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "lvgl/lvgl.h"

// Spotify on the V1: the client half of SpotUI's design.
//
// SpotUI (github.com/noisetta/spotui-hiby-r3proii) is two programs: a
// framebuffer UI and a daemon built around librespot, the open-source Spotify
// Connect client. The V1 carries one C binary and cannot link a Rust library,
// so the split is kept and only the halves are moved: this file is the client
// that plays the part of SpotUI's daemon for the player, and spotifyd.c (built
// separately, outside this repository's Makefile, exactly as librespot itself
// is) is the librespot process that answers it.
//
//     sonix_player  --command lines-->  spotifyd   (librespot: session, audio)
//          ^                                |
//          +----- PCM (46-byte header + S16_LE stereo 44.1 kHz) --+
//
// Over a UNIX stream socket, /var/run/sonix-spotify.sock by default. A line is
// one command with tab-separated arguments and ends with '\n'; the reply is
// one or more lines and ends with END or ERR, so a reader always knows where a
// reply stops. No JSON on the wire: the player already parses JSON for its
// other services and a JSON encoder on the daemon side would be a dependency
// for nothing.
//
// The PCM channel is a second connection to the same daemon, announced with
// AOPEN <16|24>. The daemon writes frames as they are decoded -- no temp file,
// no loopback HTTP, no per-frame allocation -- and this file hands them to
// audio_external_write(), the same door the internet radio, Bluetooth-in and
// AirPlay receiver come through. A track change is a new header and the data
// goes on, so the DAC is never closed and reopened between songs.
//
// Everything here blocks on its own thread; the pages never call it from the
// GUI thread. What the rest of the player needs to know -- the serial, the
// now-playing snapshot, the transport -- is lock-protected and cheap.

#define SPOTIFY_NAME_MAX 192
#define SPOTIFY_ID_MAX 64
#define SPOTIFY_SOCKET_PATH_MAX 108 // sun_path's own limit: one byte more is ENOTDIR
#define SPOTIFY_ERROR_MAX 192
#define SPOTIFY_TOKEN_MAX 4096

// Where the daemon listens when the configuration says nothing.
#ifdef HOST_BUILD
#define SPOTIFY_DEFAULT_SOCKET "usr/data/spotifyd.sock" // relative, like every HOST_BUILD path
#else
#define SPOTIFY_DEFAULT_SOCKET "/var/run/sonix-spotify.sock"
#endif

// The daemon's credential store: librespot's own cache directory, which is
// what a `librespot --cache ...` login and SpotUI's onboarding helper both
// write. Read only to say whether a login exists.
#define SPOTIFY_CACHE_DIR "/usr/data/librespot-cache"

typedef enum {
	SPOTIFY_STATE_UNAVAILABLE = 0, // no daemon, no login, or no Wi-Fi
	SPOTIFY_STATE_READY,           // logged in, nothing loaded
	SPOTIFY_STATE_LOADING,
	SPOTIFY_STATE_PLAYING,
	SPOTIFY_STATE_PAUSED,
	SPOTIFY_STATE_STOPPED,
} spotify_state_t;

typedef enum {
	SPOTIFY_BACKEND_AUTO = 0, // a socket or executable if there is one, else the mock
	SPOTIFY_BACKEND_DAEMON,
	SPOTIFY_BACKEND_MOCK, // the demo library in spotifymock.c
} spotify_backend_t;

typedef struct {
	char id[SPOTIFY_ID_MAX]; // base62 track id, "" for a local file
	char title[SPOTIFY_NAME_MAX];
	char artist[SPOTIFY_NAME_MAX];
	char album[SPOTIFY_NAME_MAX];
	char cover_url[256];
	int duration_ms;
	int index; // place in the queue, 1-based
	int count; // its length
} spotify_track_t;

typedef struct {
	bool connected;     // the status connection answered
	bool logged_in;     // ...and the daemon can play: credentials, Premium
	spotify_state_t state;
	bool shuffle;
	int repeat;         // 0 off, 1 the queue, 2 the one track
	int position_ms;
	int volume;         // 0..100 as the daemon reports it, -1 unknown
	spotify_track_t track; // zeroed when nothing is loaded
	unsigned serial;    // bumped on every change worth redrawing
	char error[SPOTIFY_ERROR_MAX]; // why it is not playing, when it is not
	char account[SPOTIFY_NAME_MAX]; // the Spotify display name, when the daemon says
	char backend[24];   // "daemon" or "mock"
} spotify_status_t;

// One list of tracks: Liked Songs, a playlist, a search's hits. The id is the
// Spotify URI's own (a base62 playlist id), or "liked" / "search".
typedef struct {
	char id[SPOTIFY_ID_MAX];
	char name[SPOTIFY_NAME_MAX];
	char owner[SPOTIFY_NAME_MAX];
	int total;
} spotify_list_t;

#define SPOTIFY_PAGE_MAX 100

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

// Reads the configuration, opens a status connection and starts the poll. Safe
// to call twice. Returns false only when no backend of any kind could be
// started (the mock is always available, so this is effectively never).
bool spotify_init(void);

// Closes the sockets and ends the playback thread. The daemon is left running:
// it is a service, not a child of this page.
void spotify_shutdown(void);

spotify_backend_t spotify_backend(void);

// The socket the player talks to, for the settings page to show and for tests
// to replace (before spotify_init()).
void spotify_set_socket(const char *path);

// When non-NULL, the daemon is started by this program: anything up to the
// first space is the executable, the rest its arguments, with {socket} replaced
// by the socket path. Off by default, because on a device the daemon is meant
// to be started by init, before the UI exists, so a reboot into the player is
// already logged in. HOST_WEB_EMU builds cannot fork.
void spotify_set_command(const char *command);
const char *spotify_get_command(void);

// A whole status snapshot on one call: the pages' timer asks this twice a
// second, and it only touches a mutex and copies strings.
void spotify_status(spotify_status_t *out);

// Bumped when anything in the snapshot changes. A page redraws only when the
// number it saw last is not the number now.
unsigned spotify_serial(void);

// ---------------------------------------------------------------------------
// Transport
// ---------------------------------------------------------------------------

// Sends a command and waits for its reply, for the calls that must know the
// answer before they draw (playlists are the long one). The page's workers use
// it; the transport below does not.
//
// `out` receives the reply's lines, tab-separated as the daemon sent them, and
// NULL is allowed when the caller only wants to know it worked.
bool spotify_command(const char *command, char *out, size_t out_size, int timeout_ms);

void spotify_play_track(const char *track_id);
void spotify_play_pause(void);
void spotify_next(void);
void spotify_prev(void);
void spotify_seek_ms(int position_ms);
void spotify_set_shuffle(bool on);
// 0 off, 1 the queue, 2 the one track.
void spotify_set_repeat(int mode);
void spotify_set_volume(int percent);

// Starts a context at a chosen track: Liked Songs, a playlist id, or the last
// search's results. `kind` is "liked", "playlist" or "search".
void spotify_play_context(const char *kind, const char *context_id, const char *track_id);

// ---------------------------------------------------------------------------
// Browse
//
// Every one of these blocks and belongs on a worker thread. A return of false
// means the request could not be made at all; a request the daemon refused
// comes back true with out->total 0 and spotify_status()->error set, because
// "this playlist is empty" and "Spotify refused" are different sentences and
// the page says one of them.
// ---------------------------------------------------------------------------

bool spotify_fetch_liked(int offset, int limit, spotify_track_t *out, int capacity, int *count);
bool spotify_fetch_playlists(spotify_list_t *out, int capacity, int *count);
bool spotify_fetch_playlist(const char *playlist_id, int offset, int limit, spotify_track_t *out, int capacity,
							int *count);
bool spotify_search(const char *query, int offset, int limit, spotify_track_t *out, int capacity, int *count);

// The queue as the daemon is playing it: current index (0-based) and length.
bool spotify_fetch_queue(int *index, int *count);

// ---------------------------------------------------------------------------
// Playback (the PCM channel and what it must not break)
// ---------------------------------------------------------------------------

// True from the moment a PCM connection is open until it is closed. The player
// asks this the way it asks radio_is_active(): while it is true the DAC is the
// daemon's and device_state_get() answers from here.
bool spotify_stream_active(void);

// Like spotify_stream_active(), but false unless the daemon says it is playing
// or loading: an open channel between two tracks is still the stream's, but a
// paused one is not "playing" in any sense a UI should draw.
bool spotify_is_active(void);

// Stops playback and drops the PCM channel, keeping the track loaded so the
// player can show it stopped. A live stream's play button means stop.
void spotify_stop(void);

// Reopens the PCM channel for the loaded, stopped track.
void spotify_resume(void);

// ---------------------------------------------------------------------------
// What device_state_get() needs
// ---------------------------------------------------------------------------

// The snapshot fields the now-playing page reads, filled without taking the
// status lock twice. `title`, `artist` and `cover_path` are what the page
// shows for a live source: the track's name, its artists, and the JPEG the
// page should draw when it exists.
typedef struct {
	bool active;
	bool connecting;
	spotify_state_t state;
	char title[SPOTIFY_NAME_MAX];
	char artist[SPOTIFY_NAME_MAX];
	char album[SPOTIFY_NAME_MAX];
	char cover_path[256];
	int position_ms;
	int duration_ms;
	int index;
	int count;
} spotify_now_t;

void spotify_get_now(spotify_now_t *out);

// Where the artwork for the current track is cached, "" when there is none.
// Downloads happen on the status thread; the page asks for this path and, when
// the file is not there yet, is drawn the Spotify mark until it is.
bool spotify_cover_path(char *out, size_t out_size);

#endif /* SPOTIFY_H */
