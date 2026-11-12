#ifndef SUBSONICDL_H
#define SUBSONICDL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "subsonic.h"

// Music from a Subsonic / Gonic server kept on the card as ordinary files:
//
//     <card>/Music/Subsonic Downloads/<Artist>/<Album>/<Artist> - <Title>.<ext>
//
// with the album's artwork beside the tracks as cover.jpg (or cover.png) and a
// "<file>.tags" sidecar in the format metadata.c already reads, so the player,
// the browser, the library scan and the details page treat a download as the
// track it is. Nothing here is ever pruned: a download stays until its file is
// deleted from the downloads page.
//
// One download at a time, on a thread of its own. Every file is written under
// a hidden ".part" name and renamed when complete, so a cut transfer never
// shows up in the browser as a track, and a completed file is never replaced
// by a failed re-download.
//
// What is saved is what would have been streamed: the quality setting decides
// between the server's MP3 or Opus transcode and the original file (Raw).
// Downloads always use the account and quality in force when the job starts;
// a track already on the card is counted as done and not fetched again.

// Where downloads go: "<card>/Music/Subsonic Downloads". An empty string when
// there is no card. The folder itself is created by the worker, not by asking.
const char *subsonicdl_root(void);

// The file `track` is saved to. False when there is no card, it is not a
// track, or the Raw quality cannot carry its codec. Says nothing about whether
// the file exists.
//
// The name is decided from the track alone -- its artist, album and title --
// so the answer here is exactly the path the download worker writes, and a
// page can ask "is this one already on the card?" without keeping state.
bool subsonicdl_path(const subsonic_entry_t *track, subsonic_quality_t quality, char *out, size_t size);

// Whether that file is already on the card.
bool subsonicdl_exists(const subsonic_entry_t *track, subsonic_quality_t quality);

// A name the card accepts: the characters FAT forbids go, leading and trailing
// dots and spaces go, the length is capped on a UTF-8 boundary. `fallback`
// when nothing is left.
void subsonicdl_safe_name(const char *title, const char *fallback, char *out, size_t size);

// One download job: `count` tracks of the page the selection was made on,
// starting at `start` (the one that was selected); the ones before it are not
// downloaded. A single track is count 1. Non-track entries are skipped, so the
// same call works on a page of folders holding a few files.
typedef struct {
	const subsonic_entry_t *tracks;
	int count;
	int start;
} subsonicdl_request_t;

// Starts the background download. The track list and the strings it points at
// are copied before this returns. The format saved is the Subsonic quality
// setting at the moment the job starts (Raw keeps the original file).
// False, starting nothing, when: a download is already running, the account is
// not configured, there is no writable card, or no selected track's codec can
// be saved with the Raw setting.
bool subsonicdl_start(const subsonicdl_request_t *request);

// Stops the running download and deletes the file it was writing. Returns at
// once; the state becomes SUBSONICDL_CANCELLED when the thread has wound up.
// Files already completed stay on the card.
void subsonicdl_cancel(void);

typedef enum {
	SUBSONICDL_IDLE,
	SUBSONICDL_RUNNING,
	SUBSONICDL_DONE,
	SUBSONICDL_FAILED,
	SUBSONICDL_CANCELLED,
} subsonicdl_state_t;

// A snapshot, protected against concurrent updates by the worker. `bytes_done`
// and `bytes_total` are the current file's (total 0 when the server did not
// announce one); `completed` and `failed` count finished files, and
// `total_tracks` is the request's count.
typedef struct {
	subsonicdl_state_t state;
	bool running;
	int completed;
	int failed;
	int total_tracks;
	uint64_t bytes_done;
	uint64_t bytes_total;
	char title[200];
	char error[192]; // why a file failed, "" when the last one did not
} subsonicdl_progress_t;

void subsonicdl_progress(subsonicdl_progress_t *out);

// True while a download runs. power.c keeps Wi-Fi up on this, as it does for
// the Qobuz and podcast downloaders.
bool subsonicdl_busy(void);

// Back to SUBSONICDL_IDLE after a finished, failed or cancelled download has
// been reported.
void subsonicdl_acknowledge(void);

// What is already on the card under the download folder, newest first. An
// entry is one file: `artist` and `album` come from its sidecar, and from the
// album folder's own names when the sidecar is missing.
#define SUBSONICDL_HISTORY_MAX 200
typedef struct {
	char path[768];
	char title[200];
	char artist[128];
	char album[200];
	long long modified;
} subsonicdl_entry_t;
int subsonicdl_history(subsonicdl_entry_t *entries, int max);

// Deletes a downloaded file, its sidecar, and -- when it was the last track in
// its album folder -- the folder with its marker and cover. Only paths under
// subsonicdl_root() in the album layout this module writes are accepted. False
// when the file could not be removed.
bool subsonicdl_delete(const char *path);

// True when `path` is the download folder or inside it. Playback of a
// downloaded file is ordinary local playback and nothing in the player needs
// this; the downloads page uses it to decide what it may delete.
bool subsonicdl_owns(const char *path);

#endif /* SUBSONICDL_H */
