// snake.h -- Snake, from the More page.
//
// The snake is let out of a card, steered with taps and swipes on a lattice of
// its own, and eats to grow. The wall is optional: the setting that decides
// whether it kills is on the card, because a board you can leave the far side of
// is a different game and the player should choose it before the first step
// rather than find out on the first corner.

#ifndef GUI_SNAKE_H
#define GUI_SNAKE_H

#include "src/gui/shell/gui.h"

#include "lvgl/lvgl.h"

extern lv_obj_t *snake_screen;

void snake_init(gui_config_t *cfg);

// Opens the page on the card it starts every run from.
void snake_open(void);

#endif /* GUI_SNAKE_H */
