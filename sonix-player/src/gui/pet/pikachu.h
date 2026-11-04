#ifndef GUI_PET_PIKACHU_H
#define GUI_PET_PIKACHU_H

#include "lvgl/lvgl.h"

// The pet's picture: one Pikachu on a grid of PIKACHU_COLS x PIKACHU_ROWS
// cells, see pikachu.c. It is handed out already enlarged by whole cells, so
// that the page can draw it 1:1 -- LVGL's own scaler is not exact enough for
// pixel art.
#define PIKACHU_COLS 37
#define PIKACHU_ROWS 40

extern const lv_image_dsc_t pikachu_2x; // 74 x 80: 2 px a cell
extern const lv_image_dsc_t pikachu_3x; // 111 x 120: 3 px a cell

// The picture's six colours, as 0xRRGGBB.
#define PIKACHU_INK 0x252923
#define PIKACHU_YELLOW 0xF4D601
#define PIKACHU_ORANGE 0xF5901B
#define PIKACHU_RED 0xF40202
#define PIKACHU_BROWN 0x82440A
#define PIKACHU_WHITE 0xF4FBF3

#endif /* GUI_PET_PIKACHU_H */
