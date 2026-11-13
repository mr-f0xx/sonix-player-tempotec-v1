#ifndef WEBDAVPAGE_H
#define WEBDAVPAGE_H

#include <stdbool.h>
#include <stddef.h>

#include "src/gui/shell/gui.h"

extern lv_obj_t *webdav_screen;

void webdavpage_init(gui_config_t *cfg);
bool webdavpage_prepare_track(char *path, size_t path_size);
bool webdavpage_waiting_for_track(void);

// Inspection helpers for 240x320 UI layout tests.
lv_obj_t *webdavpage_settings_screen(void);
lv_obj_t *webdavpage_list_screen(void);
lv_obj_t *webdavpage_edit_screen(void);
void webdavpage_open_settings_for_test(void);
void webdavpage_open_edit_for_test(int field);

#endif
