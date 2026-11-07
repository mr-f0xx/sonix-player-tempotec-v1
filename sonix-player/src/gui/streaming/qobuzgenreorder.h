#ifndef QOBUZGENREORDER_H
#define QOBUZGENREORDER_H
#include "src/system/streaming/qobuz.h"
// Fetch root genres and the Reggae child, then rank the entire set before
// handing slices to the UI. Returns -1 on a root request failure.
int qobuzgenre_load_ordered(qobuz_genre_t *out, int max, long *reggae_id);
#endif
