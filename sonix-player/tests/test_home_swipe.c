// Exercise switcher.c's swipe-up-to-home gesture and its on/off setting with
// LVGL's real pointer and scrolling dispatch.
#define _DEFAULT_SOURCE
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "lvgl/lvgl.h"
#include "src/gui/board_profile.h"
#include "src/gui/library/audiobooks.h"
#include "src/gui/library/browser.h"
#include "src/gui/library/libraryscan.h"
#include "src/gui/library/music.h"
#include "src/gui/nowplaying/coverflow.h"
#include "src/gui/nowplaying/player.h"
#include "src/gui/nowplaying/trackmenu.h"
#include "src/gui/shell/gestures.h"
#include "src/gui/shell/gui.h"
#include "src/gui/shell/icons.h"
#include "src/gui/shell/main_menu.h"
#include "src/gui/shell/switcher.h"
#include "src/gui/shell/theme.h"
#include "src/gui/shell/topbar.h"
#include "src/system/core/config.h"

lv_font_t font_ui_14;
lv_style_t theme_style_icon;
lv_image_dsc_t icon_chevron_left;

lv_obj_t *main_menu_screen;
lv_obj_t *player_screen;
lv_obj_t *browser_screen;
lv_obj_t *libraryscan_screen;
lv_obj_t *audiobookscan_screen;
lv_obj_t *trackmenu_queue_screen;
lv_obj_t *chapters_screen;

int bp_header_button_size(void) { return 36; }
bool bp_is_tempotec_v1(void) { return true; }
bool browser_can_go_up(void) { return false; }
bool browser_go_up(void) { return false; }
void browser_refresh(void) {}
bool coverflow_is_open(void) { return false; }
void coverflow_close(bool animate) { (void)animate; }
bool gui_popups_open(void) { return false; }
void libraryscan_begin(void) {}
void music_notify_main_menu_reset(void) {}
bool player_sheet_is_open(void) { return false; }
void player_sheet_open(bool animate) { (void)animate; }
void player_sheet_close(bool animate) { (void)animate; }
// switcher.c's back button steps the lyrics back first; no lyrics here.
bool player_lyrics_back(void) { return false; }
lv_obj_t *qobuz_list_screen(void) { return NULL; }
lv_obj_t *tidal_list_screen(void) { return NULL; }
void topbar_set_hidden(bool hidden) { (void)hidden; }
void topbar_move_to(lv_obj_t *screen) { (void)screen; }
const theme_palette_t *theme(void) {
	static theme_palette_t palette;
	return &palette;
}
void theme_notify_screen_shown(lv_obj_t *screen) { (void)screen; }
void theme_register_refresh(void (*fn)(void)) { (void)fn; }

static lv_point_t point;
static lv_indev_state_t state;

static void read_pointer(lv_indev_t *indev, lv_indev_data_t *data) {
	(void)indev;
	data->point = point;
	data->state = state;
}

static void read_at(lv_indev_t *indev, int x, int y, lv_indev_state_t s) {
	point.x = x;
	point.y = y;
	state = s;
	lv_tick_inc(20);
	lv_indev_read(indev);
}

static void swipe_up(lv_indev_t *indev, int x, int y_from, int y_to) {
	read_at(indev, x, y_from, LV_INDEV_STATE_PRESSED);
	read_at(indev, x, (y_from + y_to) / 2, LV_INDEV_STATE_PRESSED);
	read_at(indev, x, y_to, LV_INDEV_STATE_PRESSED);
	read_at(indev, x, y_to, LV_INDEV_STATE_RELEASED);
}

int main(void) {
	char dir[] = "/tmp/sonix-home-swipe-XXXXXX";
	assert(mkdtemp(dir));
	char cfg_path[256];
	snprintf(cfg_path, sizeof(cfg_path), "%s/device_config.ini", dir);
	assert(!config_init(cfg_path));

	lv_init();
	font_ui_14 = lv_font_montserrat_14;
	lv_style_init(&theme_style_icon);
	lv_display_create(240, 320);

	main_menu_screen = lv_obj_create(NULL);
	player_screen = lv_obj_create(NULL);
	lv_obj_t *subpage = lv_obj_create(NULL);
	lv_obj_set_scrollable(subpage, false);
	switcher_attach_back_gesture(subpage);

	lv_screen_load(main_menu_screen);

	lv_indev_t *indev = lv_indev_create();
	lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
	lv_indev_set_read_cb(indev, read_pointer);

	// Default is ON: an upward swipe on a non-scrollable page returns home.
	assert(gestures_swipe_up_home());
	switch_screen(subpage);
	assert(lv_screen_active() == subpage);
	swipe_up(indev, 120, 220, 100);
	assert(lv_screen_active() == main_menu_screen);

	// Switched OFF: the same upward swipe stays on the current page.
	gestures_set_swipe_up_home(false);
	assert(!gestures_swipe_up_home());
	assert(!config_get_bool("ui", "swipe_up_home", true));
	switch_screen(subpage);
	assert(lv_screen_active() == subpage);
	swipe_up(indev, 120, 220, 100);
	assert(lv_screen_active() == subpage);

	// Toggling OFF mid-press also cancels an in-progress swipe cleanly.
	gestures_set_swipe_up_home(true);
	read_at(indev, 120, 220, LV_INDEV_STATE_PRESSED);
	read_at(indev, 120, 160, LV_INDEV_STATE_PRESSED);
	gestures_set_swipe_up_home(false);
	read_at(indev, 120, 100, LV_INDEV_STATE_PRESSED);
	read_at(indev, 120, 100, LV_INDEV_STATE_RELEASED);
	assert(lv_screen_active() == subpage);

	// Switched back ON: the gesture works again immediately.
	gestures_set_swipe_up_home(true);
	assert(gestures_swipe_up_home());
	assert(config_get_bool("ui", "swipe_up_home", false));
	swipe_up(indev, 120, 220, 100);
	assert(lv_screen_active() == main_menu_screen);

	// Stock-gesture bottom edge stays reserved for the control centre.
	gestures_set_stock(true);
	switch_screen(subpage);
	assert(lv_screen_active() == subpage);
	swipe_up(indev, 120, 300, 180);
	assert(lv_screen_active() == subpage);
	swipe_up(indev, 120, 220, 100);
	assert(lv_screen_active() == main_menu_screen);

	lv_deinit();
	unlink(cfg_path);
	rmdir(dir);
	printf("PASS swipe-up-to-home gesture and toggle\n");
	return 0;
}
