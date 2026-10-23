#ifndef SONIX_VISUALIZATION_H
#define SONIX_VISUALIZATION_H
#include "lvgl/lvgl.h"
#include "src/system/audio/visualizer.h"

// Lightweight presentation widget: caller owns samples, refresh timing and visibility.
// NULL frames reset smoothing/peak history immediately (pause, Off, stale PCM).
lv_obj_t *visualization_create(lv_obj_t *parent);
void visualization_update(lv_obj_t *obj, visualizer_mode_t mode,
                          const visualizer_frame_t *frame, lv_color_t accent);
#endif
