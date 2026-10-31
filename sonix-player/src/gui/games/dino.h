// dino.h -- Dino, from the More page.
//
// One button, one obstacle in front of you, and a landscape that gets faster.
// The dino jumps on a tap, ducks while the finger is held down, and the night
// falls every hundred points, which is the whole game.
//
// The dino, the cacti and the birds are drawn from tables of rectangles rather
// than from pictures: the shapes are the game's own, they scale with the panel
// for free, and a build never has to hope an asset pack arrived. See the comment
// at the top of dino.c for how the pieces are put together.

#ifndef GUI_DINO_H
#define GUI_DINO_H

#include "src/gui/shell/gui.h"

#include "lvgl/lvgl.h"

extern lv_obj_t *dino_screen;

void dino_init(gui_config_t *cfg);

// Opens the page. The run starts on the card, not before it.
void dino_open(void);

#endif /* GUI_DINO_H */
