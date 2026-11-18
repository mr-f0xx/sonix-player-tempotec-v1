#ifndef TAPESAMPLER_H
#define TAPESAMPLER_H

#include "lvgl/lvgl.h"

void tapesampler_build(lv_obj_t *parent, lv_obj_t **pads, lv_obj_t **kits);
void tapesampler_open_pads(void);
void tapesampler_open_kits(void);
void tapesampler_poll(void);
void tapesampler_poll_kits(void);
void tapesampler_refresh_theme(void);

#endif
