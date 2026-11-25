#ifndef TILELAYOUT_H
#define TILELAYOUT_H

#include <stdbool.h>
#include <stddef.h>

#include "src/gui/library/music_tiles.h"

// Which tiles the Music page shows, in what order, and which wait in "Not in
// use" -- the rule behind Music > Settings > Library > Rearrange main page.
//
// Here rather than in the page that draws the rows because it is a rule and not
// a drawing: what a saved arrangement means, which tiles a build offers, and
// what happens to a tile it no longer does. It reads and writes the config
// itself, so the page, the rearrange screen and the tests all agree.
//
// The names are what the setting holds ("albums", "most_played"), and they
// outlive the tiles: "Most played" left the page in favour of Music > Stats,
// and an arrangement written before that still has to be read and written back
// as it was. See tilelayout.c.

// The tiles a build offers on the page, in the order an arrangement that says
// nothing is read in. "Most played" is not among them.
int tilelayout_offered(music_tile_id_t *out, int max);

// Whether this build draws `id` on the Music page.
bool tilelayout_available(music_tile_id_t id);

// The name an arrangement saves `id` under, and the tile a name means. False
// from the second for a name nothing knows.
const char *tilelayout_slug(music_tile_id_t id);
bool tilelayout_parse(const char *slug, size_t len, music_tile_id_t *out);

// Reads the saved arrangement: the tiles on the page, in order, then the ones
// waiting. Both arrays hold MUSIC_TILE_COUNT; the counts are what was written.
//
// A tile the build no longer offers is never among the active ones, wherever
// the config named it and whatever order it was in -- and it is kept in the
// waiting list, so arranging the page again writes it back rather than losing
// what the user chose.
void tilelayout_load(music_tile_id_t *active, int *active_count, music_tile_id_t *hidden, int *hidden_count);

// The page's tiles and the waiting ones, for a caller that only wants them in
// order.
int tilelayout_active(music_tile_id_t *out, int max);
int tilelayout_hidden(music_tile_id_t *out, int max);

// Stores an arrangement. Anything of MUSIC_TILE_COUNT that neither list names
// goes where this build wants it: an offered tile onto the page, a retired one
// among the waiting.
void tilelayout_save(const music_tile_id_t *active, int active_count, const music_tile_id_t *hidden,
					 int hidden_count);

#endif /* TILELAYOUT_H */
