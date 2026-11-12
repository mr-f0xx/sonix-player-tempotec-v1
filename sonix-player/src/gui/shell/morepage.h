#ifndef GUI_MOREPAGE_H
#define GUI_MOREPAGE_H

#include "src/gui/shell/gui.h"

#include "lvgl/lvgl.h"

// The More page is reached through its home-menu shortcut. It holds the DAC
// and destinations that do not fit among the six home slots. Hiding the
// shortcut does not disable the page or the features it contains.

extern lv_obj_t *morepage_screen;

void morepage_init(gui_config_t *cfg);

#endif /* GUI_MOREPAGE_H */
