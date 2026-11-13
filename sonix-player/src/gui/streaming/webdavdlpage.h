#ifndef WEBDAVDLPAGE_H
#define WEBDAVDLPAGE_H

#include <stdbool.h>

#include "src/gui/shell/gui.h"
#include "src/system/streaming/webdav.h"

#define WEBDAVDL_HISTORY_MAX 200

extern lv_obj_t *webdavdl_screen;

void webdavdlpage_init(gui_config_t *cfg);
void webdavdlpage_open(void);
void webdavdlpage_repaint(void);
void webdavdlpage_watch(void);

bool webdavdlpage_present(const webdav_entry_t *entry);
void webdavdlpage_download_tracks(const webdav_entry_t *tracks, int count, int index);
void webdavdlpage_download_folder(const char *folder_rel_path, const char *folder_title);

#endif
