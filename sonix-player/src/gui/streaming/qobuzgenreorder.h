#ifndef QOBUZGENREORDER_H
#define QOBUZGENREORDER_H
#include "src/system/streaming/qobuz.h"
#include <string.h>
#include <strings.h>

// The parent ID comes from the API, not a guessed child ID. Slugs are stable
// across locales; the name fallback covers responses that omit the slug.
static inline bool qobuzgenre_is_blues_parent(const char *name, const char *slug) {
    const char *leaf = slug ? strrchr(slug, '/') : NULL;
    leaf = leaf ? leaf + 1 : (slug ? slug : "");
    return !strcasecmp(leaf, "blues-country-folk") ||
           (name && !strcasecmp(name, "Blues/Country/Folk"));
}

// Fetch root genres and the Reggae child, then rank the entire set before
// handing slices to the UI. Returns -1 on a root request failure.
int qobuzgenre_load_ordered(qobuz_genre_t *out, int max, long *reggae_id);
#endif
