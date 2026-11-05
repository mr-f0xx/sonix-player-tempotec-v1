#ifndef PODCASTPLAYED_H
#define PODCASTPLAYED_H

#include <stdbool.h>

// Which podcast episodes have been heard.
//
// One row per episode in <card>/.local/podcast.db, beside the followed
// podcasts: the row IS the fact, so there is no flag that can disagree with it
// and nothing to update in place. The key is the catalogue's episode id, not a
// file: an episode that has never been downloaded has no file, one in the cache
// is deleted after a while, and one kept in the Podcast folder is the same
// episode as the one that was streamed -- the id is the only thing all three
// share.
//
// The whole set is held in memory, sorted, because the episode list asks for it
// on every row it binds and a scroll binds a dozen at a time: a query per row
// would put SQLite in the middle of a finger's movement. The database is the
// on-disk copy of that set, written the moment the set changes -- powering the
// player off right after the last episode ended must not lose the mark.
//
// Episodes are marked from two places: playback to the end (see player.c's
// completion hook) and the long press on a row. Nothing ever unmarks them
// except the user.

// Sets the card root and loads the set. NULL or empty clears it.
void podcastplayed_set_root(const char *sd_root);

// Reads the set again when podcast.db was deleted or replaced since it was last
// read or written -- a card swapped under the player. Safe to call often.
void podcastplayed_reload_if_replaced(void);

// Whether that episode has been heard.
bool podcastplayed_get(long long episode_id);

// Marks it heard, or heard-and-not. Saves immediately, and does nothing for an
// id that is not an episode id (0 or negative).
void podcastplayed_set(long long episode_id, bool played);

#endif /* PODCASTPLAYED_H */
