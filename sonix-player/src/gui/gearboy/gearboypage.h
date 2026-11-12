#ifndef GUI_GEARBOYPAGE_H
#define GUI_GEARBOYPAGE_H

#include "src/gui/shell/gui.h"

#include "lvgl/lvgl.h"

// The game shelf: ROMs from Games/GB and Games/GBC, titled from the cartridge
// database and optionally decorated from a matching Cover Art image.

extern lv_obj_t *gearboypage_screen;

void gearboypage_init(gui_config_t *cfg);

#endif /* GUI_GEARBOYPAGE_H */
