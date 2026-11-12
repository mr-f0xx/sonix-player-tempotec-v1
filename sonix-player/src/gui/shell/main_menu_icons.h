#ifndef MAIN_MENU_ICONS_H
#define MAIN_MENU_ICONS_H

#include "lvgl/lvgl.h"

// Exact-size art for the six home tiles (row-major order), and the alternate
// Browse/Books destinations. Glyphs are tintable A8 masks; Dark/Light retain
// their colored illustrations. Unknown sizes return NULL; callers can
// downsample the 173 px variant for a reflowed tile.
#define MAIN_MENU_ICON_BROWSE 6
#define MAIN_MENU_ICON_BOOKS 7
const lv_image_dsc_t *main_menu_icon_native(int tile, bool glyph, int size);

#endif /* MAIN_MENU_ICONS_H */
