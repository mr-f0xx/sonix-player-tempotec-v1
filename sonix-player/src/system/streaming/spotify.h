#ifndef SPOTIFY_H
#define SPOTIFY_H

#include <stdbool.h>
#include <stddef.h>

#include "src/system/streaming/spotifybrowse.h"

// Spotify on the V1: a receiver on the card, driven from here.
//
// The heavy half of Spotify -- the login, the Connect session, the audio keys,
// the Ogg Vorbis stream -- is q2-librespot's, a librespot fork that runs as a
// Spotify Connect receiver. This is the other end of its three interfaces, the
// same ones Q2 Pod drives it through:
//
//   --status-file      what it is playing, into a file we poll
//   --control-socket   one-byte datagrams for play/pause/next/previous/seek
//   --browse-socket    Q2SPOT/1: the browser's requests (spotifybrowse.c)
//
// and a fourth that is ours alone:
//
//   --backend pipe     its PCM into a FIFO that the player reads and writes to
//                      the DAC through audio_external_*(), exactly as the
//                      AirPlay receiver's socket does (spotifysink.c)
//
// Nothing of Spotify's protocol is implemented here, and nothing of the player
// is in the receiver: the two meet at four files in /tmp and one process. That
// is what makes this half small enough to be worth doing.
//
// The receiver lives on the microSD card, in <card>/.spotify, and is started
// and stopped from Streaming -> Spotify. Nothing is flashed for it. See
// docs/spotify.md for the card layout and for the build it needs.

// ---------------------------------------------------------------------------
// where it is, and whether it is there
// ---------------------------------------------------------------------------

// Reads the settings and works out the paths. Call once, after config_init()
// and after the storage layer knows which directory the card landed on; the
// receiver itself is not started here.
void spotify_init(const char *sd_root);

// True when the card carries an executable receiver. The page says so instead
// of offering a switch that cannot work.
bool spotify_available(void);

// <card>/.spotify, or "" when there is no card. What the page shows and what
// the documentation talks about.
const char *spotify_dir(void);

// ---------------------------------------------------------------------------
// the switch
// ---------------------------------------------------------------------------

// Starts or stops the receiver. The work happens on a control thread -- a
// librespot takes a moment to come up and to go away -- so this returns at
// once and the page follows spotify_running().
void spotify_set_enabled(bool on);
bool spotify_enabled(void);

// True once the receiver is up. Which is not the same as enabled: there is a
// gap while it starts, and it can be enabled but stopped because it died.
bool spotify_running(void);

// One short line for the page's status row: what is running, or why not.
const char *spotify_status_text(void);

// Stops the receiver synchronously, for the paths that are leaving: power off,
// reboot, factory reset. Waiting is the point -- a librespot still holding the
// card or the DAC while the storage layer unmounts is a filesystem that is
// dirty on the next boot.
void spotify_shutdown(void);

// ---------------------------------------------------------------------------
// what it is doing
// ---------------------------------------------------------------------------

// The state the receiver itself writes down. `none` is the documented "no
// Connect session"; the three others are the player's own states.
typedef enum {
	SPOTIFY_PLAY_NONE,
	SPOTIFY_PLAY_STOPPED,
	SPOTIFY_PLAY_PAUSED,
	SPOTIFY_PLAY_PLAYING,
} spotify_play_state_t;

typedef struct {
	spotify_play_state_t state;
	bool session;	// a Connect session exists: a phone has paired and can cast
	bool playing;	// the sink has audio flowing, not merely the receiver saying so
	bool cover;		// the cover file holds this track's artwork
	char track[80]; // Spotify's id for it, useful only for spotting a change
	char title[SPOTIFY_TEXT_MAX];
	char artist[SPOTIFY_TEXT_MAX];
	char album[SPOTIFY_TEXT_MAX];
	int duration_ms;
	int position_ms; // advanced from the file's `at` while it plays
} spotify_state_t;

// A copy under a lock: safe from the interface thread while the poll thread
// writes.
void spotify_get_state(spotify_state_t *out);

// Where the receiver writes the current track's artwork: a JPEG beside its
// status file, rewritten as tracks change. Decode it off the interface thread
// (coverloader) and re-ask whenever spotify_serial() moves.
void spotify_cover_path(char *out, size_t size);

// Bumped whenever anything above changes, so a page redraws only then.
unsigned spotify_serial(void);

// Whether the device's transport keys belong to Spotify right now: it is
// playing, or it is paused and it was the last thing that played. Exclusive
// with local playback by construction -- local playback asks for the DAC
// through spotifysink_release_output(), which is what clears the latch.
bool spotify_owns_playback(void);

// Hands the keys back to local playback. The sink calls it when local playback
// takes the DAC; the Sink settings page calls it when the user says so.
void spotify_forget_active(void);

// Milliseconds since audio last arrived, or -1 if none ever has. A receiver
// that is paused, or whose phone stopped sending, reads as a long silence
// rather than as an error.
int spotify_silent_ms(void);

// ---------------------------------------------------------------------------
// transport
// ---------------------------------------------------------------------------

typedef enum {
	SPOTIFY_CMD_PLAY_PAUSE,
	SPOTIFY_CMD_PLAY,
	SPOTIFY_CMD_PAUSE,
	SPOTIFY_CMD_NEXT,
	SPOTIFY_CMD_PREV,
} spotify_command_t;

// One datagram to the receiver. Never blocks for long: a missing socket is
// simply sent to nowhere, and the key falls through to local playback.
void spotify_control(spotify_command_t command);

// A seek, in milliseconds from the start of the track.
void spotify_seek_ms(int ms);

// ---------------------------------------------------------------------------
// the browser
// ---------------------------------------------------------------------------

// One Q2SPOT/1 request. Blocking, up to the protocol's 15 s, so it belongs on
// the page's worker and never on the interface thread. `out` describes what
// happened either way -- including when the receiver is not running at all.
bool spotify_browse(const char *op, const char *const *args, int nargs, spotify_reply_t *out);

// True while a request is in flight. The power layer uses it to keep Wi-Fi up:
// parking the radio between a press and its answer is a page that never loads.
bool spotify_browse_busy(void);

// What the Android-sized question "is this add-on new enough?" needs: the
// version the receiver reports for `caps`, e.g. "q2-v1.4", or "".
bool spotify_backend_version(char *out, size_t size);

// Writes a file the browser can show instead of a blank screen: the receiver's
// own log tail, for when it will not start. "" when there is nothing to say.
void spotify_log_tail(char *out, size_t size);

// ---------------------------------------------------------------------------
// what the page tells the service
// ---------------------------------------------------------------------------

// The browser is on screen (or left it). Keeps the receiver awake while
// someone is looking at it, and lets the idle stop below apply when not.
void spotify_page_open(bool open);

// Any user interaction: a key, a tap, a request. Counts as being in use.
void spotify_activity(void);

// ---------------------------------------------------------------------------
// settings, all persisted in device_config.ini
// ---------------------------------------------------------------------------

// 96, 160 or 320 kbit/s. The default is 160, which is what the Q2 plays and
// what its author found this class of hardware keeps up with in soft float.
int spotify_bitrate(void);
void spotify_set_bitrate(int kbps);

// The name the phone shows in Spotify's device list. Empty means the player's
// own name from system-info.json.
const char *spotify_receiver_name(void);
void spotify_set_receiver_name(const char *name);

// How much of the card the receiver may fill with cached audio, in MiB
// (complete tracks are what its `downloaded` list replays offline). 0 turns
// that cache off entirely.
int spotify_cache_mb(void);
void spotify_set_cache_mb(int mb);

// Seconds of nothing playing after it has played, with the browser shut, that
// stop the receiver. 0 keeps it running until it is switched off.
int spotify_idle_stop_secs(void);
void spotify_set_idle_stop_secs(int seconds);

#endif /* SPOTIFY_H */
