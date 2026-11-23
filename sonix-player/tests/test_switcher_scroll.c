// Issue #398: a list keeps its scroll offset only on the way back. Going
// forward into a page -- from a parent menu, a row, a corner button -- always
// starts it at the top; returning to it with the chevron, the swipe-back's
// commit or switch_screen_return_to() lands on the row that was left.
//
// Runs the real switcher.c and its history against LVGL screens whose bodies
// are marked exactly as settingsrow_page(), the ebook settings and the GearBoy
// list mark theirs.
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

#define SCREEN_H 320
#define ROW_H 56
#define ROWS 20

// The LV_EVENT_SCREEN_LOADED handler a page like Language or Time zone has:
// it scrolls to the row it wants shown. The reset on a forward step must run
// before it, so the page's own choice is what ends up on screen.
static int loaded_scroll_target = -1;
static void scroll_to_row_on_load(lv_event_t *e) {
	lv_obj_t *list = (lv_obj_t *)lv_event_get_user_data(e);
	if (loaded_scroll_target >= 0) {
		lv_obj_scroll_to_y(list, loaded_scroll_target, LV_ANIM_OFF);
	}
}

// A page the way settingsrow_page() lays one out: a screen, a marked scrolling
// column of rows, enough of them to scroll on a 320 px panel.
static lv_obj_t *make_page(lv_obj_t **list_out, bool nested) {
	lv_obj_t *screen = lv_obj_create(NULL);
	lv_obj_set_scrollable(screen, false);

	lv_obj_t *parent = screen;
	if (nested) {
		// GearBoy keeps its list one level down, inside a non-scrolling
		// column with the GB/GBC chooser above it.
		parent = lv_obj_create(screen);
		lv_obj_set_size(parent, lv_pct(100), SCREEN_H);
		lv_obj_set_scrollable(parent, false);
	}

	lv_obj_t *list = lv_obj_create(parent);
	lv_obj_set_size(list, lv_pct(100), SCREEN_H);
	lv_obj_set_style_pad_all(list, 0, 0);
	lv_obj_set_style_pad_gap(list, 0, 0);
	lv_obj_set_scroll_dir(list, LV_DIR_VER);
	lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
	for (int i = 0; i < ROWS; i++) {
		lv_obj_t *row = lv_obj_create(list);
		lv_obj_set_size(row, lv_pct(100), ROW_H);
	}
	switcher_mark_fresh_start(list);
	switcher_attach_back_gesture(list);
	lv_obj_update_layout(screen);
	assert(lv_obj_get_scroll_bottom(list) > 0);

	*list_out = list;
	return screen;
}

static int scroll_y(lv_obj_t *list) { return (int)lv_obj_get_scroll_y(list); }

int main(void) {
	char dir[] = "/tmp/sonix-switcher-scroll-XXXXXX";
	assert(mkdtemp(dir));
	char cfg_path[256];
	snprintf(cfg_path, sizeof(cfg_path), "%s/device_config.ini", dir);
	assert(!config_init(cfg_path));

	lv_init();
	font_ui_14 = lv_font_montserrat_14;
	lv_style_init(&theme_style_icon);
	lv_display_create(240, SCREEN_H);

	main_menu_screen = lv_obj_create(NULL);
	player_screen = lv_obj_create(NULL);

	lv_obj_t *settings_list, *sub_list, *leaf_list, *gearboy_list;
	lv_obj_t *settings = make_page(&settings_list, false);  // Settings
	lv_obj_t *sub = make_page(&sub_list, false);            // a submenu of it
	lv_obj_t *leaf = make_page(&leaf_list, false);          // a page under that
	lv_obj_t *gearboy = make_page(&gearboy_list, true);     // body one level down

	// The mark is readable back and is not on an unmarked object.
	assert(switcher_is_fresh_start(settings_list));
	assert(!switcher_is_fresh_start(settings));
	assert(!switcher_is_fresh_start(NULL));

	lv_screen_load(main_menu_screen);

	// --- Forward into Settings for the first time: at the top.
	switch_screen(settings);
	assert(lv_screen_active() == settings);
	assert(scroll_y(settings_list) == 0);

	// Scroll down, step into the submenu: the submenu opens at the top...
	lv_obj_scroll_to_y(settings_list, 3 * ROW_H, LV_ANIM_OFF);
	assert(scroll_y(settings_list) == 3 * ROW_H);
	lv_obj_scroll_to_y(sub_list, 5 * ROW_H, LV_ANIM_OFF); // left over from an earlier visit
	switch_screen(sub);
	assert(lv_screen_active() == sub);
	assert(scroll_y(sub_list) == 0);
	// ...and Settings, parked in the history, keeps its offset meanwhile.
	assert(scroll_y(settings_list) == 3 * ROW_H);

	// --- Back with the chevron: Settings is where it was left.
	lv_obj_scroll_to_y(sub_list, 4 * ROW_H, LV_ANIM_OFF);
	back_btn_cb(NULL);
	assert(lv_screen_active() == settings);
	assert(scroll_y(settings_list) == 3 * ROW_H);

	// --- Forward into the same submenu again: top again, not row 4.
	switch_screen(sub);
	assert(lv_screen_active() == sub);
	assert(scroll_y(sub_list) == 0);

	// --- Two levels: Settings (row 3) -> sub (row 2) -> leaf, back, back.
	lv_obj_scroll_to_y(sub_list, 2 * ROW_H, LV_ANIM_OFF);
	switch_screen(leaf);
	assert(scroll_y(leaf_list) == 0);
	lv_obj_scroll_to_y(leaf_list, 6 * ROW_H, LV_ANIM_OFF);
	back_btn_cb(NULL);
	assert(lv_screen_active() == sub);
	assert(scroll_y(sub_list) == 2 * ROW_H);
	back_btn_cb(NULL);
	assert(lv_screen_active() == settings);
	assert(scroll_y(settings_list) == 3 * ROW_H);

	// --- Returning to the page just arrived from through switch_screen()
	// (the Wi-Fi password page's way out) is a step back, not forward.
	switch_screen(sub);
	assert(scroll_y(sub_list) == 0);
	lv_obj_scroll_to_y(sub_list, 7 * ROW_H, LV_ANIM_OFF);
	switch_screen(leaf);
	switch_screen(sub); // top of the history: popped, offset kept
	assert(lv_screen_active() == sub);
	assert(scroll_y(sub_list) == 7 * ROW_H);

	// --- switch_screen_return_to() from two pages deep is a step back too.
	switch_screen(leaf);
	switch_screen_return_to(settings);
	assert(lv_screen_active() == settings);
	assert(scroll_y(settings_list) == 3 * ROW_H);
	// ...and the pages it skipped are entered afresh next time.
	switch_screen(sub);
	assert(scroll_y(sub_list) == 0);

	// --- Home, then back into Settings: a fresh entry, so the top.
	switch_screen(main_menu_screen);
	assert(lv_screen_active() == main_menu_screen);
	assert(scroll_y(settings_list) == 3 * ROW_H); // untouched until it is entered
	switch_screen(settings);
	assert(scroll_y(settings_list) == 0);

	// --- A body nested below the screen (GearBoy) is found and reset.
	lv_obj_scroll_to_y(gearboy_list, 5 * ROW_H, LV_ANIM_OFF);
	switch_screen(gearboy);
	assert(lv_screen_active() == gearboy);
	assert(scroll_y(gearboy_list) == 0);
	lv_obj_scroll_to_y(gearboy_list, 5 * ROW_H, LV_ANIM_OFF);
	switch_screen(leaf);
	back_btn_cb(NULL);
	assert(lv_screen_active() == gearboy);
	assert(scroll_y(gearboy_list) == 5 * ROW_H);

	// --- A page that scrolls to its selected row on LV_EVENT_SCREEN_LOADED
	// still gets its way: the reset runs before the load, not after.
	lv_obj_add_event_cb(leaf, scroll_to_row_on_load, LV_EVENT_SCREEN_LOADED, leaf_list);
	loaded_scroll_target = 8 * ROW_H;
	switch_screen(leaf);
	assert(lv_screen_active() == leaf);
	assert(scroll_y(leaf_list) == 8 * ROW_H);
	loaded_scroll_target = -1;

	// --- An unmarked body is left alone in both directions.
	lv_obj_t *plain_list;
	lv_obj_t *plain = make_page(&plain_list, false);
	lv_obj_set_user_flag(plain_list, 0, false);
	assert(!switcher_is_fresh_start(plain_list));
	lv_obj_scroll_to_y(plain_list, 2 * ROW_H, LV_ANIM_OFF);
	switch_screen(plain);
	assert(scroll_y(plain_list) == 2 * ROW_H);

	lv_deinit();
	unlink(cfg_path);
	rmdir(dir);
	printf("PASS lists start at the top going forward and keep their place going back\n");
	return 0;
}
