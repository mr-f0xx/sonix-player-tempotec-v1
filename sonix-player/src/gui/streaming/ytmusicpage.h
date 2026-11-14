#ifndef YTMUSICPAGE_H
#define YTMUSICPAGE_H

#include <stdbool.h>
#include <stddef.h>

#include "src/gui/shell/gui.h"
#include "src/system/streaming/ytmusic.h"
#include "lvgl/lvgl.h"

extern lv_obj_t *ytmusic_screen;
void ytmusicpage_init(gui_config_t *cfg);

// "Can this queue entry be played right now?", asked by device_state before it
// opens one. A song the page queued but nobody has played has no cache file
// yet: this answers no, fetches it, and the queue is restarted on it when it is
// there. Registered by ytmusicpage_init(); declared because it is defined
// elsewhere in the file than where it is registered.
bool ytmusicpage_prepare_track(char *path, size_t path_size);

// Gives the YouTube queue created by SonixLink the same preparation behavior
// as a queue built from this page, so next/previous can fetch unheard tracks.
bool ytmusicpage_set_external_queue(const ytmusic_track_t *tracks, const char *const *paths, int count,
								   int start_slot);

#endif /* YTMUSICPAGE_H */
