#ifndef SONIX_VISUALIZATION_H
#define SONIX_VISUALIZATION_H
#include "lvgl/lvgl.h"
#include "src/system/audio/visualizer.h"

// Stateless drawing widget: caller owns samples, timing and visibility.
lv_obj_t *visualization_create(lv_obj_t *parent);
void visualization_update(lv_obj_t *obj, visualizer_mode_t mode,
                          const visualizer_frame_t *frame, lv_color_t accent);
#endif
