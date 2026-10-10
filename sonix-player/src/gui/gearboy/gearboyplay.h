#ifndef GUI_GEARBOYPLAY_H
#define GUI_GEARBOYPLAY_H

#include "src/gui/shell/gui.h"

#include "lvgl/lvgl.h"

// The play screen: the picture at the top, the touch controls beneath it.
//
// HiBy R3 Pro II and R1: 160x144 at three times is 480x432, exactly the panel
// width. The V1 uses a nearest-neighbour 240x216 fit to give the gameplay image
// more room on its 240x320 display, with a compact D-pad and A/B in the 104 rows
// below it. The physical Play/Pause key acts as Start and the skip keys act as
// Select, so those two buttons do not consume touchscreen space.

extern lv_obj_t *gearboyplay_screen;

void gearboyplay_init(gui_config_t *cfg);

// Starts a game and opens the screen. The title comes from the database.
void gearboyplay_open(const char *rom_path, const char *title);

// Offers a physical/media transport-key action to the active game. Returns
// true when the game screen owns the key and the music transport must not see it.
bool gearboyplay_handle_key(gui_key_t key);

#endif /* GUI_GEARBOYPLAY_H */
