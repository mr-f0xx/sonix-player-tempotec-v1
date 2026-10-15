#ifndef MAIN_MENU_ICONS_H
#define MAIN_MENU_ICONS_H

#include "lvgl/lvgl.h"

// Exact-size art keyed by main_menu_button_t identity, not tile position.
// Glyphs are A8 masks, tinted with the same semantic colors as the originals;
// Dark/Light retain their colored illustrations. NULL means an unknown size,
// for which the caller can fall back to the original scalable image.
const lv_image_dsc_t *main_menu_icon_native(int button, bool glyph, int size);

#endif /* MAIN_MENU_ICONS_H */
