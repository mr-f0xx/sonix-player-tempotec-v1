/* Empty library and artwork services for the real Cover Flow gesture handler.
 * No audio, storage worker or actual covers are needed to open its empty sheet. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "src/gui/nowplaying/cover.h"
#include "src/gui/nowplaying/coverloader.h"
#include "src/gui/nowplaying/player.h"
#include "src/gui/shell/quickpanel.h"
#include "src/gui/shell/scrolltext.h"
#include "src/gui/shell/topbar.h"
#include "src/system/audio/usbdac.h"
#include "src/system/core/utils.h"
#include "src/system/library/library.h"
#include "src/system/playback/device_state.h"
#include "src/system/playback/playlist.h"

void *big_alloc(size_t size) { return malloc(size); }
void big_free(void *ptr, size_t size) { (void)size; free(ptr); }
uint32_t utils_isqrt32(uint32_t value) { return (uint32_t)sqrt((double)value); }
bool quickpanel_is_open(void) { return false; }
bool quickpanel_blocked(void) { return false; }
bool usbdac_is_active(void) { return false; }
void topbar_bring_to_front(void) {}
void scrolltext_apply(lv_obj_t *label) { (void)label; }
void scrolltext_set(lv_obj_t *label, const char *text) { lv_label_set_text(label, text); }
void player_refresh_now_playing(void) {}

bool library_is_open(void) { return true; }
library_index_t *library_index_open(library_list_t kind, library_filter_t filter, const char *value,
		library_order_t order, bool desc) {
	(void)kind; (void)filter; (void)value; (void)order; (void)desc;
	return NULL;
}
void library_index_close(library_index_t *index) { (void)index; }
int library_index_count(const library_index_t *index) { (void)index; return 0; }
int library_index_find_name(const library_index_t *index, const char *name) { (void)index; (void)name; return -1; }
int library_index_window(const library_index_t *index, int offset, int count, library_row_cb cb, void *user) {
	(void)index; (void)offset; (void)count; (void)cb; (void)user;
	return 0;
}
library_index_t *library_index_clone(const library_index_t *index) { (void)index; return NULL; }
void library_album_title(const char *value, char *out, size_t size) { snprintf(out, size, "%s", value); }
bool device_state_play_index(library_index_t *index, int start) { (void)index; (void)start; return false; }
void playlist_set_album(const char *album) { (void)album; }

uint32_t cover_dominant_tone(const cover_image_t *cover) { (void)cover; return 0; }
void cover_free(cover_image_t *cover) { if (cover) memset(cover, 0, sizeof(*cover)); }
bool cover_thumb_cached(const char *path, int size, cover_image_t *out) {
	(void)path; (void)size; (void)out;
	return false;
}
void coverloader_start(void) {}
void coverloader_request(int slot, const char *path, int size) { (void)slot; (void)path; (void)size; }
void coverloader_release(int slot) { (void)slot; }
bool coverloader_take(int slot, cover_image_t *out, bool *finished) {
	(void)slot; (void)out;
	if (finished) *finished = true;
	return false;
}
