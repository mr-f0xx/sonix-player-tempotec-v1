#ifndef JELLYFIN_ICON_H
#define JELLYFIN_ICON_H

#include "lvgl/lvgl.h"

// Monochrome white-alpha mark (32x32) for theme-tinted grids and list rows.
extern const lv_image_dsc_t icon_jellyfin_mark;

// Coloured artwork-corner badge (52x52) for Now Playing.
extern const lv_image_dsc_t icon_jellyfin_badge;

// Full streaming-grid tile icon (112x112).
extern const lv_image_dsc_t icon_jellyfin_menu;

// Saved-offline home tile illustration (48x48).
extern const lv_image_dsc_t icon_jellyfin_saved;

// Stacked-card artwork (tools/tile_art.py recipe) that Dark and Light show in
// place of the flat icons above; every other theme keeps those.
extern const lv_image_dsc_t icon_jellyfin_menu_card;
extern const lv_image_dsc_t icon_jellyfin_saved_card;

#endif /* JELLYFIN_ICON_H */
