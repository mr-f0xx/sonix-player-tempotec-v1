#ifndef GLASSWALLPAPER_H
#define GLASSWALLPAPER_H

#include "lvgl/lvgl.h"

// The Liquid Glass default wallpaper: the abstract blue/purple glass artwork
// supplied for the theme, pre-rasterized to the V1 panel (240x320, RGB565).
// Used as the always-on background and as the Now Playing ground so the view
// is never see-through. Scaled to cover larger panels.
extern const lv_image_dsc_t glass_wallpaper_dsc;

#endif // GLASSWALLPAPER_H
