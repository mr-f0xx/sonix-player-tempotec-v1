// game2048.h -- 2048, from the More page.
//
// The rules are the ones Gabriele Cirulli published: four rows of four, a swipe
// sends every tile to that wall, two equal tiles that meet make the next power
// of two, and a fresh 2 or 4 turns up after each move. The game is turn-based,
// so it has no clock of its own and costs the player nothing while it is being
// thought about.
//
// The board is kept in the config, in [game2048], together with the score: a
// puzzle left half-finished because the player switched the music on is a
// puzzle they may not come back to.

#ifndef GUI_GAME2048_H
#define GUI_GAME2048_H

#include "src/gui/shell/gui.h"

#include "lvgl/lvgl.h"

extern lv_obj_t *game2048_screen;

void game2048_init(gui_config_t *cfg);

// Reads the saved board -- a new one when there is nothing to carry on with --
// and opens the page.
void game2048_open(void);

#endif /* GUI_GAME2048_H */
