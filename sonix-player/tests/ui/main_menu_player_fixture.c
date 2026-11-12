/* Use the production player-sheet gesture handler without constructing the
 * entire playback UI. player_init() normally performs these initial sheet
 * steps; the rest of player.c is removed by linker section garbage collection.
 * Including it here lets the fixture set its private sheet width rather than
 * changing the production interface solely for a test. */
#include <assert.h>

/* Cover Flow's playback action is not used in this fixture. Keep its refresh
 * stub separate so it does not pull the entire now-playing UI into the test. */
#define player_refresh_now_playing main_menu_unused_player_refresh
#include "src/gui/nowplaying/player.c"
#undef player_refresh_now_playing

void main_menu_test_player_sheet_init(gui_config_t *cfg) {
	player_screen = lv_obj_create(lv_layer_top());
	sheet_width = (int)cfg->screen_width;
	lv_obj_set_size(player_screen, cfg->screen_width, cfg->screen_height);
	lv_obj_set_style_pad_all(player_screen, 0, 0);
	lv_obj_set_style_border_width(player_screen, 0, 0);
	lv_obj_set_style_radius(player_screen, 0, 0);
	lv_obj_set_scrollable(player_screen, false);
	lv_obj_set_pos(player_screen, sheet_width, 0);
	lv_obj_set_hidden(player_screen, true);
}

/* Sanitizer instrumentation can retain the unused audiobook action table even
 * though this fixture never constructs the player controls. */
void chapters_open(void) { assert(!"unexpected playback action"); }
void audiobookextras_add_bookmark(void) { assert(!"unexpected playback action"); }
void audiobookextras_open_current_bookmarks(void) { assert(!"unexpected playback action"); }
void audiobookextras_open_current_summary(void) { assert(!"unexpected playback action"); }
