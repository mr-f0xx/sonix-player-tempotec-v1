#ifndef VIDEOPLAYER_UI_H
#define VIDEOPLAYER_UI_H

#include "lvgl/lvgl.h"
#include "src/gui/shell/gui.h"

extern lv_obj_t *videoplayer_screen;

void videoplayer_ui_init(gui_config_t *cfg);
bool videoplayer_ui_open(const char *path);

#endif /* VIDEOPLAYER_UI_H */
