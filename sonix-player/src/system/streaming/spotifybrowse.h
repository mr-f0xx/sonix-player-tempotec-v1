#ifndef SPOTIFYBROWSE_H
#define SPOTIFYBROWSE_H

#include <stdbool.h>
#include <stddef.h>

// Q2SPOT/1: the browsing protocol that q2-librespot's --browse-socket answers.
//
// Written from the specification in q2-pod's docs/librespot.md, and spoken to
// the same fork Q2 Pod talks to. Nothing about Spotify itself is here: a
// request is an operation name and some opaque arguments, and a response is a
// status, a cursor and rows of text. The receiver does the talking to Spotify,
// which is the whole point -- see docs/spotify.md.
//
// This file is deliberately free of the player: no LVGL, no sockets in the
// parser, no configuration. tests/test_spotify.c drives one end of the
// protocol against this code and the other end against a fake backend, on the
// host, with no card and no librespot.

// Limits, all from the protocol document. A response is at most 64 KiB and a
// request 8 KiB; a page is at most 50 rows.
#define SPOTIFY_REQ_MAX 8192
#define SPOTIFY_REPLY_MAX (64 * 1024)
#define SPOTIFY_ROWS_MAX 50

// What a field holds. Titles and subtitles are display text and are cut to fit
// (the protocol itself cuts them at 255 bytes on a character boundary); URIs,
// contexts and cursors are opaque and are NEVER cut -- an over-long one makes
// the whole reply malformed rather than quietly starting a different track.
#define SPOTIFY_TEXT_MAX 200
#define SPOTIFY_URI_MAX 384
#define SPOTIFY_POSITION_MAX 24
#define SPOTIFY_MESSAGE_MAX 160
#define SPOTIFY_CURSOR_MAX 1024

typedef enum {
	SPOTIFY_ROW_TRACK,
	SPOTIFY_ROW_ALBUM,
	SPOTIFY_ROW_ARTIST,
	SPOTIFY_ROW_PLAYLIST,
	SPOTIFY_ROW_ACCOUNT,
	SPOTIFY_ROW_SETTING,
	SPOTIFY_ROW_OP, // a capability, from `caps`
	SPOTIFY_ROW_UNKNOWN,
} spotify_row_kind_t;

typedef struct {
	spotify_row_kind_t kind;
	char uri[SPOTIFY_URI_MAX]; // a Spotify URI; for `setting` its name, for `op` an op name
	char title[SPOTIFY_TEXT_MAX];
	char subtitle[SPOTIFY_TEXT_MAX];
	char context[SPOTIFY_URI_MAX]; // `play <context> <position> <uri>` starts the row in its list
	char position[SPOTIFY_POSITION_MAX];
} spotify_row_t;

// Every status the backend can answer, plus the four ways the conversation
// itself can fail. The page shows one message per value, so the two kinds live
// in one enum rather than forcing every caller to handle both separately.
typedef enum {
	SPOTIFY_REPLY_OK,
	SPOTIFY_REPLY_LOGIN,	   // no saved login: pair from the phone
	SPOTIFY_REPLY_OFFLINE,	   // Spotify cannot be reached
	SPOTIFY_REPLY_PREMIUM,	   // the account is not Premium
	SPOTIFY_REPLY_REFUSED,	   // Spotify refused the login (401/403)
	SPOTIFY_REPLY_UNAVAILABLE, // the item is gone, or cannot play here
	SPOTIFY_REPLY_STALE,	   // search's query hash changed (412): update the add-on
	SPOTIFY_REPLY_THROTTLED,   // 429; the message says when to try again
	SPOTIFY_REPLY_UNSUPPORTED, // the backend does not know the op (too old)
	SPOTIFY_REPLY_BAD,		   // malformed or oversized request
	SPOTIFY_REPLY_ERROR,	   // anything else the backend reported
	SPOTIFY_REPLY_MALFORMED,   // the reply did not parse
	SPOTIFY_REPLY_NOSOCKET,	   // no browse socket: the receiver is not running
	SPOTIFY_REPLY_TIMEOUT,	   // the answer did not arrive inside the deadline
} spotify_reply_status_t;

typedef struct {
	spotify_reply_status_t status;
	char message[SPOTIFY_MESSAGE_MAX]; // the backend's one line, or ours
	char cursor[SPOTIFY_CURSOR_MAX];   // empty at the end of a list
	int count;
	spotify_row_t rows[SPOTIFY_ROWS_MAX];
} spotify_reply_t;

// ---------------------------------------------------------------------------
// fields
// ---------------------------------------------------------------------------

// `in` into `out` with the four escaped bytes as %25/%09/%0A/%0D. Returns the
// number of bytes written (not counting the terminator), or -1 when it does not
// fit -- an escaped request must never be silently cut, because the field it
// carries is an identifier.
int spotify_escape(const char *in, char *out, size_t size);

// The other way round. A '%' not followed by two hex digits, a decoded NUL (it
// would end the field early), or something that does not fit returns -1.
int spotify_unescape(const char *in, size_t in_len, char *out, size_t size);

// ---------------------------------------------------------------------------
// the two directions
// ---------------------------------------------------------------------------

// Builds the request: the version line, then `<id>\t<op>\t<arg>...`, each field
// escaped. Returns the length, or -1 when it does not fit in SPOTIFY_REQ_MAX.
int spotify_request_build(char *out, size_t size, unsigned id, const char *op, const char *const *args, int nargs);

// Parses a whole reply. `want_id` is the request's number: a reply carrying
// another one is a late answer to an abandoned request and is dropped, with
// false. Returns true when the header and every row were understood.
bool spotify_reply_parse(const char *buf, size_t len, unsigned want_id, spotify_reply_t *out);

// The name of a status, for a log line.
const char *spotify_reply_status_name(spotify_reply_status_t status);

// ---------------------------------------------------------------------------
// the conversation
// ---------------------------------------------------------------------------

// One request on `socket_path` (a Unix stream socket), one reply, one
// connection -- the protocol's transport. Writes the request, shuts the write
// side down, reads until the peer closes, and parses what came back into `out`.
//
// Blocking, with a deadline; it belongs on a worker thread, never on the
// interface thread. Returns true only for SPOTIFY_REPLY_OK.
bool spotify_browse_call(const char *socket_path, unsigned id, const char *op, const char *const *args, int nargs,
						 spotify_reply_t *out, int timeout_ms);

#endif /* SPOTIFYBROWSE_H */
