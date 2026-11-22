#ifndef SPOTIFYPAGE_H
#define SPOTIFYPAGE_H

#include <stdbool.h>

#include "src/gui/shell/gui.h"

#include "lvgl/lvgl.h"

// The Spotify pages: the home grid, the track lists (Liked Songs, a playlist,
// a search's hits -- one list page, reused), search, and the account page.
//
// The shape is SpotUI's: a tile per place to start from, the lists as rows with
// the playing track marked, a Now Playing that is the player's own (the daemon
// streams into it), and transport that is the page's and the physical keys'.
//
// Everything here talks to src/system/streaming/spotify.c; nothing in this
// file knows how the daemon is reached or what a librespot is.
extern lv_obj_t *spotify_screen;

void spotifypage_init(gui_config_t *cfg);

// The list page, for back navigation and for opening a context from elsewhere.
extern lv_obj_t *spotify_list_screen;

// Opens Liked Songs, a playlist or a search's results from outside the pages
// (the Now Playing sheet's "show the queue" lands here).
bool spotifypage_open_liked(void);
bool spotifypage_open_playlist(const char *playlist_id, const char *title);

// The home page's geometry, for the headless page test. The photo of a real V1
// showed the demo note printed across the bottom tiles and the badge sitting on
// top of a tile -- states the test never looked at, because it only ever asked
// whether the captions existed. These hand it the objects to measure.
lv_obj_t *spotify_home_grid_for_test(void);
// One of the list page's rows. The rows are content-sized flex containers, and
// the list scrolls by a fixed pitch, so the test measures a row against that.
lv_obj_t *spotify_list_row_for_test(int index);
lv_obj_t *spotify_home_demo_for_test(void);
lv_obj_t *spotify_home_note_for_test(void);

#endif /* SPOTIFYPAGE_H */
