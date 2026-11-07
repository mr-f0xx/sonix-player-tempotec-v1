#ifndef QOBUZGENREGRID_H
#define QOBUZGENREGRID_H

#include <stdbool.h>
#include "lvgl/lvgl.h"

// The list container is shared with album and track lists. Switch its layout
// only when a fresh result replaces the list, not while appending a page.
void qobuzgenre_grid_layout(lv_obj_t *container, bool grid);
lv_obj_t *qobuzgenre_grid_add(lv_obj_t *container, int content_width, long genre_id,
                              const char *name, const char *slug, int index, long reggae_id, lv_event_cb_t clicked);
void qobuzgenre_grid_refresh(lv_obj_t *container);
const lv_image_dsc_t *qobuzgenre_icon(long genre_id, long reggae_id);

#endif
