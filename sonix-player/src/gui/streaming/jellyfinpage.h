#ifndef JELLYFINPAGE_H
#define JELLYFINPAGE_H

#include <stdbool.h>
#include <stddef.h>

#include "src/gui/shell/gui.h"

extern lv_obj_t *jellyfin_screen;

void jellyfinpage_init(gui_config_t *cfg);
bool jellyfinpage_prepare_track(char *path, size_t path_size);
bool jellyfinpage_waiting_for_track(void);
bool jellyfinpage_download_busy(void);

// Toggles a Jellyfin track's Favourite status asynchronously on a worker thread.
void jellyfinpage_toggle_favorite_async(const char *item_id,
										void (*done)(bool ok, const char *error, void *user), void *user);

#endif /* JELLYFINPAGE_H */
