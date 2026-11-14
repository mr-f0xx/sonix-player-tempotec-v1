#ifndef YTLIKES_H
#define YTLIKES_H

#include <stdbool.h>
#include <stddef.h>

// Songs and podcast episodes hearted on YouTube Music, kept on the player.
//
// The heart on a song used to write the cache file's path into the music
// database, which was the wrong list twice over: the path points into the
// playback cache, and the cache is swept on the next play, so the like
// evaporated. There is also no way to write a like to a Google account without
// the imported browser session -- OAuth tokens are refused by the music
// endpoints -- so a player signed in with nothing but a Google token could
// never keep a favourite at all.
//
// So a like is a record of the item, not of a file: the video id, the names the
// row carried, and when it was hearted. It lives in a sidecar beside
// device_config.ini, the way the session and the OAuth tokens do, and the
// Favourites row includes it with or without an account session. Nothing here
// touches the network.
//
// The list is capped, newest first: a hundred items is a few tens of kilobytes,
// and a card that has collected thousands would be a second of work per track
// change.

#define YTLIKES_MAX 100
#define YTLIKES_ID_MAX 16
#define YTLIKES_TITLE_MAX 128
#define YTLIKES_NAME_MAX 96

typedef struct {
	char id[YTLIKES_ID_MAX];          // the eleven-character video id
	char title[YTLIKES_TITLE_MAX];
	char artist[YTLIKES_NAME_MAX];
	char album[YTLIKES_TITLE_MAX];
	int duration;                     // seconds, 0 when the row did not say
	long liked_at;                    // unix seconds, newest first
} ytlike_t;

// Reads the sidecar. Called once from ytmusic_init(), after config_init(): the
// file sits in the config's own directory, so it is on the internal storage
// and survives updates.
void ytlikes_init(void);

int ytlikes_count(void);
// The entry at `index`, newest first, or NULL past the end. The pointer stays
// valid until the next add or remove.
const ytlike_t *ytlikes_at(int index);
// Copies up to `limit` entries, newest first. Safe to use from a worker while
// the GUI may add or remove a like.
int ytlikes_snapshot(ytlike_t *out, int limit);
const ytlike_t *ytlikes_find(const char *id);
bool ytlikes_contains(const char *id);

// Adds one, or moves an existing like of the same id to the front with the
// fresh names. The oldest entry makes room when the list is full.
bool ytlikes_add(const ytlike_t *like);
bool ytlikes_remove(const char *id);
// What the heart does: adds when the song is not in the list, removes it when
// it is. Returns true when the song is liked afterwards.
bool ytlikes_toggle(const ytlike_t *like);

#endif /* YTLIKES_H */
