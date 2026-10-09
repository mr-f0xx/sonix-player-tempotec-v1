#ifndef APPEARANCE_H
#define APPEARANCE_H

#include "src/gui/shell/gui.h"

#include "lvgl/lvgl.h"

// The Appearance page: theme (dark, the default, or light) as two
// Adwaita-style segmented buttons rather than a row that silently flips, plus
// the boot screen the next power-on opens with, the accent colour, the clock
// position, the battery percentage, the text size and the face the whole
// interface is drawn with -- MiSans, the others the image carries, and the
// fonts in the card's Fonts folder. See fonts.h.

extern lv_obj_t *appearance_screen;

void appearance_init(gui_config_t *cfg);

#endif /* APPEARANCE_H */
