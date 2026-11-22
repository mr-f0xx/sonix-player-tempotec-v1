#ifndef THEMEFILEPAGE_H
#define THEMEFILEPAGE_H

#include "src/gui/shell/gui.h"

// The "Custom theme" page under Settings > Appearance: every .ini in Themes/
// on the card, and which one the Custom theme wears.
//
// A page rather than a row of pills in Appearance because the files are card
// content read on the way in, and because a pill per file crowded the theme
// row the moment there was more than one -- the row in Appearance names the
// file in force and opens this list, the way Boot picture does.
extern lv_obj_t *themefilepage_screen;

void themefilepage_init(gui_config_t *cfg);

#endif // THEMEFILEPAGE_H
