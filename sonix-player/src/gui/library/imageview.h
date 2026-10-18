#ifndef IMAGEVIEW_H
#define IMAGEVIEW_H

#include "src/gui/shell/gui.h"

#include "lvgl/lvgl.h"

#include "src/gui/nowplaying/cover.h"

// An image file on a page of its own, read-only: the file manager opens
// JPEG, PNG, BMP, GIF and WebP files here.
extern lv_obj_t *imageview_screen;

void imageview_init(gui_config_t *cfg);

// Shows `path` on the image viewer page. False, staying where it is, when the
// file cannot be read or decoded.
bool imageview_open(const char *path);

// Shows an image the caller already decoded and packed into `img`. The caller
// keeps ownership of `img` and must cover_free() it when the viewer is closed.
bool imageview_show_image(const char *title, cover_image_t *img);

#endif /* IMAGEVIEW_H */
