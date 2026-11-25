#ifndef STATS_H
#define STATS_H

#include "src/gui/shell/gui.h"

// The listening statistics page: how much the player has been played, and to
// what, over the last week, the current month, the current year or all time.
//
// The shape follows what the player's own screens do -- a title row with the
// range pills across the top, the two summary cards, the bar chart of the last
// seven days, current-month days, current-year months or all-time years, and the leaders by
// track, artist and record. Every number comes from listeningstats.h, which
// counts it while the music plays.
//
// The three "Top" cards are the whole of what the Music page's old "Most
// played" tile said: the plays come from the same LISTEN_TRACK rows, and a row
// is a way in. Tapping a track plays it, tapping a record opens its tracks and
// tapping an artist opens their page, so nothing had to be kept separate and a
// tile could go (issue #494).
//
// Three "See all" pages hang off it: the full top-track list, the full
// top-artist list and the full top-album list, with their covers.

extern lv_obj_t *stats_screen;
extern lv_obj_t *stats_artists_screen;
extern lv_obj_t *stats_albums_screen;
extern lv_obj_t *stats_tracks_screen;

void stats_init(gui_config_t *cfg);

// Repainted from the database. Called when the page is shown rather than once
// at build time, so a day's listening that happened after boot is on the page
// the first time it is opened.
void stats_refresh(void);

#endif /* STATS_H */
