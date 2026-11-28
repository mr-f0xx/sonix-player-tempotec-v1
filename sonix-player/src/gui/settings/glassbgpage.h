#ifndef GLASSBGPAGE_H
#define GLASSBGPAGE_H

#include "src/gui/shell/gui.h"

#include "lvgl/lvgl.h"

// Settings > Appearance > Background (Liquid Glass only): the list of pictures
// in the card's Wallpaper folder, and a way back to the supplied art. Picking
// one decodes it, fits it to the panel and makes it the background every glass
// surface shows. See glassbg.h for where the choice is kept.

extern lv_obj_t *glassbgpage_screen;

void glassbgpage_init(gui_config_t *cfg);

#endif /* GLASSBGPAGE_H */
