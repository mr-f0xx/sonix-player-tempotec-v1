#include "qobuzgenreorder.h"
#include <strings.h>

// Stable IDs for Qobuz's root genres. Unknown genres retain their original
// API order after the curated ones, regardless of the language of their names.
static int rank_genre(long id, long reggae_id) {
    static const long order[] = {
        133, // Hip-Hop/Rap
        127, // Soul/Funk/R&B
        80,  // Jazz
        64,  // Electronic
        3,   // Blues
        0,   // Reggae (resolved from Qobuz's World children)
        112, // Pop/Rock
        116, // Metal
        5,   // Folk/Americana
        4,   // Country
        149, // Latin
        94,  // World
        10,  // Classical
        91,  // Soundtracks
        59,  // Humour/Spoken Word
    };
    for (unsigned i = 0; i < sizeof(order) / sizeof(order[0]); i++) {
        long candidate = order[i] ? order[i] : reggae_id;
        if (candidate > 0 && candidate == id) return (int)i;
    }
    return (int)(sizeof(order) / sizeof(order[0]));
}

int qobuzgenre_load_ordered(qobuz_genre_t *out, int max, long *reggae_id) {
    if (!out || max <= 0 || !reggae_id) return 0;
    *reggae_id = 0;
    int count = 0;
    while (count < max) {
        int want = max - count < 50 ? max - count : 50;
        int n = qobuz_genres(count, out + count, want);
        if (n < 0) return -1;
        count += n;
        if (n < want) break;
    }
    qobuz_genre_t reggae;
    // An unavailable/absent subgenre should not hide the rest of the list.
    if (qobuz_reggae_genre(&reggae) == 1 && reggae.id > 0) {
        *reggae_id = reggae.id;
        int found = 0;
        for (int i = 0; i < count; i++) if (out[i].id == reggae.id) found = 1;
        if (!found) {
            if (count < max) out[count++] = reggae;
            else {
                // Make room at the cap without dropping a higher-priority
                // genre that happened to occur at the end of the API list.
                int drop = 0;
                for (int i = 1; i < count; i++) {
                    if (rank_genre(out[i].id, *reggae_id) >= rank_genre(out[drop].id, *reggae_id)) drop = i;
                }
                for (int i = drop; i + 1 < count; i++) out[i] = out[i + 1];
                out[count - 1] = reggae;
            }
        }
    } else {
        // Some catalogues may already return Reggae as a root genre. If the
        // child lookup is unavailable, use it when its name is unambiguous.
        for (int i = 0; i < count; i++) {
            if (out[i].id > 0 && !strcasecmp(out[i].name, "reggae")) {
                *reggae_id = out[i].id;
                break;
            }
        }
    }
    // Stable insertion sort: small root list, no allocation, equal-ranked
    // entries (including unknown ones) stay in Qobuz's order.
    for (int i = 1; i < count; i++) {
        qobuz_genre_t item = out[i];
        int rank = rank_genre(item.id, *reggae_id), j = i;
        while (j > 0 && rank_genre(out[j - 1].id, *reggae_id) > rank) {
            out[j] = out[j - 1];
            j--;
        }
        out[j] = item;
    }
    return count;
}
