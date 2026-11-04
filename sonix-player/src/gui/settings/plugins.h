#ifndef PLUGINS_H
#define PLUGINS_H

#include "lvgl/lvgl.h"

#include "src/gui/shell/gui.h"

// Settings -> Plugins: the optional modules of this build, each with a switch.
//
// The page is the whole of the addon interface. A module that is off is not
// built at startup (see addons.h), so its switch is a memory setting as much as
// a menu one; the page says so and asks for the restart that makes it true.
extern lv_obj_t *plugins_screen;

// Called from gui_init before settings_init: the settings row opens this page.
void plugins_init(gui_config_t *cfg);

#endif /* PLUGINS_H */
