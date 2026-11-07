#ifndef QOBUZDL_H
#define QOBUZDL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "qobuz.h"

// One background download at a time. Album requests fetch all pages, not just
// the tracks currently visible in the UI. Files are never part of the cache.
bool qobuzdl_track(const qobuz_track_t *track, int format);
bool qobuzdl_album(const char *album_id, int format);
bool qobuzdl_busy(void);

// Snapshot, protected against concurrent updates by the download worker.
// The byte count refers to the current track; total == 0 means unknown.
typedef struct {
    bool running;
    bool album;
    int completed_tracks;
    int total_tracks; // 0 if Qobuz did not provide a count
    int failed_tracks;
    uint64_t bytes_done;
    uint64_t bytes_total;
    char title[QOBUZ_TITLE_MAX];
} qobuzdl_progress_t;
void qobuzdl_progress(qobuzdl_progress_t *out);

#define QOBUZDL_HISTORY_MAX 200
typedef struct {
    char path[768];
    char title[QOBUZ_TITLE_MAX];
    char artist[QOBUZ_NAME_MAX];
    long long modified;
} qobuzdl_entry_t;
// Completed downloads currently on the card, newest first.
int qobuzdl_history(qobuzdl_entry_t *entries, int max);
#endif
