#ifndef QOBUZDL_H
#define QOBUZDL_H

#include <stdbool.h>
#include <stddef.h>
#include "qobuz.h"

// One background download at a time. Album requests fetch all pages, not just
// the tracks currently visible in the UI. Files are never part of the cache.
bool qobuzdl_track(const qobuz_track_t *track, int format);
bool qobuzdl_album(const char *album_id, int format);
bool qobuzdl_busy(void);

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
