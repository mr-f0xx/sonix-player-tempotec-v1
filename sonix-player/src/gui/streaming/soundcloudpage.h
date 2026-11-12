#ifndef SOUNDCLOUDPAGE_H
#define SOUNDCLOUDPAGE_H

#include <stdbool.h>
#include <stddef.h>

#include "src/gui/shell/gui.h"

extern lv_obj_t *soundcloud_screen;

void soundcloudpage_init(gui_config_t *cfg);
bool soundcloudpage_prepare_track(char *path, size_t path_size);
bool soundcloudpage_waiting_for_track(void);

// Toggles a SoundCloud track's Like status asynchronously on a worker thread.
void soundcloudpage_toggle_favorite_async(long long track_id,
										  void (*done)(bool ok, const char *error, void *user), void *user);

#endif /* SOUNDCLOUDPAGE_H */
