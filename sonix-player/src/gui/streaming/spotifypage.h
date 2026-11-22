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

#endif /* SPOTIFYPAGE_H */
