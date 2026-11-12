#ifndef APPEARANCE_H
#define APPEARANCE_H

#include "src/gui/shell/gui.h"

#include "lvgl/lvgl.h"

// The Appearance page: theme (Dark, Light, Tokyo Night, Amber on Black or OLED)
// as immediate Adwaita-style segmented buttons, plus the boot screen the next
// power-on opens with, the accent colour, the clock position, icon-only
// main-menu tiles, the battery percentage, rounded screen corners, the text
// size and the face the whole interface is drawn with -- MiSans, the others
// the image carries, and the fonts in the card's Fonts folder. See fonts.h.

extern lv_obj_t *appearance_screen;

void appearance_init(gui_config_t *cfg);

// Also reachable by holding a home tile or empty grid, even when Settings is
// hidden. NULL until Appearance has been initialized.
lv_obj_t *appearance_main_menu_items_screen(void);

#endif /* APPEARANCE_H */
