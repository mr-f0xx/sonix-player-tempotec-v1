#ifndef MUSIC_H
#define MUSIC_H

#include "src/gui/shell/gui.h"

#include "lvgl/lvgl.h"

// The tiles themselves live in music_tiles.h, which the tile arrangement rule
// (settings/tilelayout.c) can include without pulling the page in.
#include "src/gui/library/music_tiles.h"

// The Music page: the library's index tiles (all tracks, albums, artists,
// album artists, genres, years, playlists, most played) plus the file browser,
// with library settings, playlists, favourites and search on the corner buttons.
extern lv_obj_t *music_screen;

void music_init(gui_config_t *cfg);

// Rebuilds the Music page tile grid from the current tile arrangement and
// updates the corner button icon. Called at startup and whenever the user
// rearranges tiles or toggles "playlists first".
void music_refresh_layout(void);

#endif /* MUSIC_H */
