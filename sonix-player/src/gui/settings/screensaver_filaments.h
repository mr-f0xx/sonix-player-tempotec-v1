#ifndef SCREENSAVER_FILAMENTS_H
#define SCREENSAVER_FILAMENTS_H

#include "lvgl/lvgl.h"

typedef enum {
	FILAMENTS_SHARP = 1 << 0,
	FILAMENTS_SOFT = 1 << 1,
	FILAMENTS_SPARKS = 1 << 2,
	FILAMENTS_TUNNEL = 1 << 3,
	FILAMENTS_ALL = FILAMENTS_SHARP | FILAMENTS_SOFT | FILAMENTS_SPARKS | FILAMENTS_TUNNEL,
} filaments_layer_t;

// Only called on the GUI thread while the screensaver is visible and playing.
void filaments_draw(lv_obj_t *parent, int area_w, int area_h, unsigned layers);
void filaments_release(void);

#endif
