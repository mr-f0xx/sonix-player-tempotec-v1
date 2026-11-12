/* Navigation integration for the home-menu editor: real LVGL, screens,
 * persistence, switcher and both overlay gestures; unrelated device, library,
 * artwork and font services are stand-ins. */
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "lvgl/lvgl.h"
#include "src/gui/board_profile.h"
#include "src/gui/ebook/ebookpage.h"
#include "src/gui/fonts/fonts.h"
#include "src/gui/library/audiobooks.h"
#include "src/gui/library/browser.h"
#include "src/gui/library/libraryscan.h"
#include "src/gui/library/medialist.h"
#include "src/gui/library/music.h"
#include "src/gui/nowplaying/coverflow.h"
#include "src/gui/nowplaying/player.h"
#include "src/gui/settings/appearance.h"
#include "src/gui/settings/bootpicpage.h"
#include "src/gui/shell/corners.h"
#include "src/gui/shell/main_menu.h"
#include "src/gui/shell/morepage.h"
#include "src/gui/shell/settingsrow.h"
#include "src/gui/shell/switcher.h"
#include "src/gui/shell/theme.h"
#include "src/gui/shell/toast.h"
#include "src/gui/shell/topbar.h"
#include "src/gui/streaming/streaming.h"
#include "src/gui/wireless/wireless.h"
#include "src/system/addons/addons.h"
#include "src/system/bluetooth/btreceiver.h"
#include "src/system/core/config.h"
#include "src/system/core/lang.h"
#include "src/system/device/bootlogo.h"
#include "src/system/device/bootpic.h"
#include "src/system/playback/device_state.h"
#include "src/system/streaming/podcastcache.h"

lv_obj_t *music_screen, *streaming_screen, *wireless_screen, *audiobooks_screen;
lv_obj_t *morepage_screen, *settings_screen, *browser_screen, *ebookpage_screen;
lv_obj_t *libraryscan_screen, *audiobookscan_screen, *bootpicpage_screen;
void main_menu_test_player_sheet_init(gui_config_t *cfg);

lv_font_t font_ui_14, font_ui_16, font_ui_18, font_ui_20, font_ui_20_bold;
lv_font_t font_ui_22, font_ui_22_bold, font_ui_24, font_ui_24_bold;
lv_font_t font_ui_26, font_ui_28, font_ui_32;

static void init_fonts(void) {
	lv_font_t *fonts[] = {&font_ui_14, &font_ui_16, &font_ui_18, &font_ui_20, &font_ui_20_bold,
			&font_ui_22, &font_ui_22_bold, &font_ui_24, &font_ui_24_bold, &font_ui_26, &font_ui_28, &font_ui_32};
	for (size_t i = 0; i < sizeof(fonts) / sizeof(fonts[0]); i++) *fonts[i] = lv_font_montserrat_14;
}

void fonts_register_change(void (*cb)(void)) { (void)cb; }
int fonts_text_size(void) { return FONTS_TEXT_NORMAL; }
bool fonts_set_text_size(int size) { (void)size; return true; }
const char *fonts_choice(void) { return "default"; }
bool fonts_set_face(const char *id) { (void)id; return true; }
int fonts_choices(const char **ids, const char **labels, int max) {
	assert(max > 0);
	ids[0] = "default";
	labels[0] = "Test font";
	return 1;
}

const char *storage_sd_root(void) { return NULL; }
void bootlogo_set(int choice, bool dark) { (void)choice; (void)dark; }
bool bootpic_present(void) { return false; }
bool corners_enabled(void) { return false; }
void corners_set_enabled(bool enabled) { (void)enabled; }
int corners_radius(void) { return 16; }
void corners_set_radius(int radius) { (void)radius; }
static bool topbar_hidden;
void topbar_set_hidden(bool hidden) { topbar_hidden = hidden; }
bool topbar_is_hidden(void) { return topbar_hidden; }
void topbar_set_battery_percent(bool shown) { (void)shown; }
void topbar_set_clock_position(int pos) { (void)pos; }
void topbar_set_player_color(bool enabled, lv_color_t color) { (void)enabled; (void)color; }
void topbar_set_player_frost(const lv_image_dsc_t *img) { (void)img; }
void topbar_set_player_ground(bool enabled, lv_color_t color) { (void)enabled; (void)color; }
void topbar_set_player_palette(bool enabled, lv_color_t color) { (void)enabled; (void)color; }
bool btreceiver_is_active(void) { return false; }
void medialist_refresh_visible(void) {}
void toast_error(const char *text) { (void)text; assert(!"unexpected save error"); }
void gui_notify_popup(const char *text) { (void)text; assert(!"unexpected popup"); }
bool gui_card_available(void) { return true; }
void gui_notify_no_card(void) { assert(!"unexpected missing card"); }
void browser_refresh(void) {}
bool browser_can_go_up(void) { return false; }
bool browser_go_up(void) { return false; }
void libraryscan_begin(void) {}
void audiobooks_open(void) { switch_screen(audiobooks_screen); }
void ebookpage_open(void) { switch_screen(ebookpage_screen); }

void device_state_get(device_state_t *out) { memset(out, 0, sizeof(*out)); }
bool podcastcache_feed_id(const char *path, long long *out) { (void)path; (void)out; return false; }
bool podcastcache_is_episode(const char *path) { (void)path; return false; }
bool podcastcache_tag(const char *path, const char *key, char *out, size_t size) {
	(void)path; (void)key;
	if (out && size) out[0] = '\0';
	return false;
}

static void flush(lv_display_t *display, const lv_area_t *area, uint8_t *pixels) {
	(void)area; (void)pixels;
	lv_display_flush_ready(display);
}

static int home_holds;
static void count_home_hold(lv_event_t *e) { (void)e; home_holds++; }

static lv_point_t point;
static lv_indev_state_t state;
static void read_pointer(lv_indev_t *indev, lv_indev_data_t *data) {
	(void)indev;
	data->point = point;
	data->state = state;
}

static void read_at(lv_indev_t *indev, int x, int y, lv_indev_state_t next, int ms) {
	point.x = x;
	point.y = y;
	state = next;
	lv_tick_inc((uint32_t)ms);
	lv_indev_read(indev);
	/* Production refreshes layout between input samples. In particular, the
	 * overlay's release handler measures the last position set during a drag. */
	lv_obj_update_layout(lv_screen_active());
	lv_obj_update_layout(lv_layer_top());
}

/* Run deferred click-guard clearing and let the sheet animations finish. */
static void settle(void) {
	for (int i = 0; i < 20; i++) {
		lv_tick_inc(20);
		lv_timer_handler();
	}
}

static lv_area_t area_of(lv_obj_t *obj) {
	lv_area_t area;
	lv_obj_update_layout(lv_screen_active());
	lv_obj_update_layout(lv_layer_top());
	lv_obj_get_coords(obj, &area);
	return area;
}

static void tap(lv_indev_t *indev, lv_obj_t *obj) {
	lv_area_t area = area_of(obj);
	int x = (area.x1 + area.x2) / 2;
	int y = (area.y1 + area.y2) / 2;
	read_at(indev, x, y, LV_INDEV_STATE_PRESSED, 20);
	read_at(indev, x, y, LV_INDEV_STATE_RELEASED, 20);
	settle();
}

static lv_obj_t *find_label(lv_obj_t *obj, const char *text) {
	if (lv_obj_check_type(obj, &lv_label_class) && strcmp(lv_label_get_text(obj), text) == 0) return obj;
	for (uint32_t i = 0; i < lv_obj_get_child_count(obj); i++) {
		lv_obj_t *match = find_label(lv_obj_get_child(obj, i), text);
		if (match) return match;
	}
	return NULL;
}

static lv_obj_t *switch_for(lv_obj_t *items, main_menu_slot_t slot) {
	lv_obj_t *label = find_label(items, tr(main_menu_slot_label(slot)));
	assert(label);
	lv_obj_t *card = lv_obj_get_parent(label);
	for (uint32_t i = 0; i < lv_obj_get_child_count(card); i++) {
		lv_obj_t *child = lv_obj_get_child(card, i);
		if (lv_obj_check_type(child, &lv_switch_class)) return child;
	}
	assert(!"missing visibility switch");
	return NULL;
}

static void tap_switch(lv_indev_t *indev, lv_obj_t *items, main_menu_slot_t slot, bool shown) {
	lv_obj_t *sw = switch_for(items, slot);
	lv_obj_scroll_to_view_recursive(sw, LV_ANIM_OFF);
	tap(indev, sw);
	assert(main_menu_slot_visible(slot) == shown);
}

static void hold(lv_indev_t *indev, lv_obj_t *obj, lv_obj_t *items) {
	lv_area_t area = area_of(obj);
	int x = (area.x1 + area.x2) / 2;
	int y = (area.y1 + area.y2) / 2;
	read_at(indev, x, y, LV_INDEV_STATE_PRESSED, 20);
	read_at(indev, x, y, LV_INDEV_STATE_PRESSED, 600);
	assert(lv_screen_active() == items);
	read_at(indev, x, y, LV_INDEV_STATE_RELEASED, 20);
	settle();
	assert(lv_screen_active() == items); /* no click-through on release */
}

static void swipe(lv_indev_t *indev, int x, int y, int dx) {
	read_at(indev, x, y, LV_INDEV_STATE_PRESSED, 20);
	read_at(indev, x + 20, y, LV_INDEV_STATE_PRESSED, 20);
	read_at(indev, x + dx, y, LV_INDEV_STATE_PRESSED, 20);
	if (!switcher_back_drag_active()) fprintf(stderr, "back swipe not engaged at %d,%d (+%d)\n", x, y, dx);
	assert(switcher_back_drag_active());
	read_at(indev, x + dx, y, LV_INDEV_STATE_RELEASED, 20);
	settle();
}

int main(int argc, char **argv) {
	assert(argc == 2);
	char dir[] = "/tmp/sonix-menu-nav-XXXXXX";
	assert(mkdtemp(dir));
	char config[256];
	snprintf(config, sizeof(config), "%s/device_config.ini", dir);
	config_init(config);
	config_set("ui", "language", "English");
	lang_init(argv[1]);
	lv_init();
	init_fonts();
	gui_config_t cfg = {bp_screen_w(), bp_screen_h(), bp_padding(), bp_status_bar_h(), "/tmp"};
	lv_display_t *display = lv_display_create(cfg.screen_width, cfg.screen_height);
	static uint16_t pixels[240 * 32];
	lv_display_set_buffers(display, pixels, NULL, sizeof(pixels), LV_DISPLAY_RENDER_MODE_PARTIAL);
	lv_display_set_flush_cb(display, flush);
	lv_obj_set_scrollable(lv_layer_top(), false);
	theme_init();
	addons_seed(false);

	main_menu_screen = lv_obj_create(NULL);
	music_screen = lv_obj_create(NULL);
	streaming_screen = lv_obj_create(NULL);
	wireless_screen = lv_obj_create(NULL);
	audiobooks_screen = lv_obj_create(NULL);
	morepage_screen = lv_obj_create(NULL);
	settings_screen = lv_obj_create(NULL);
	browser_screen = lv_obj_create(NULL);
	ebookpage_screen = lv_obj_create(NULL);
	appearance_screen = lv_obj_create(NULL);
	bootpicpage_screen = lv_obj_create(NULL);
	main_menu_test_player_sheet_init(&cfg);
	back_btn_init(&cfg);
	lv_obj_t *back = lv_obj_get_child(lv_layer_top(), -1);
	main_menu_init(&cfg);
	coverflow_init(&cfg);
	lv_obj_t *coverflow_sheet = lv_obj_get_child(lv_layer_top(), -1);
	appearance_init(&cfg);
	/* gui_init also attaches handlers to the home screen itself. The grid and
	 * the editor attach their own handlers during construction. */
	player_sheet_attach_drag(main_menu_screen, true);
	switcher_attach_back_gesture(main_menu_screen);
	switch_screen(main_menu_screen);
	lv_obj_t *grid = lv_obj_get_child(main_menu_screen, 0);
	lv_obj_t *items = appearance_main_menu_items_screen();
	lv_obj_add_event_cb(grid, count_home_hold, LV_EVENT_LONG_PRESSED, NULL);
	assert(items && lv_obj_is_hidden(back));

	lv_indev_t *indev = lv_indev_create();
	lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
	lv_indev_set_display(indev, lv_display_get_default());
	lv_indev_set_read_cb(indev, read_pointer);
	lv_indev_set_long_press_time(indev, 500);
	read_at(indev, 120, 120, LV_INDEV_STATE_RELEASED, 20);

	/* A hold at the end of a real player-sheet drag must not open the editor
	 * or click the tile. The canceled sheet and its click guard must reset. */
	lv_area_t home_tile = area_of(lv_obj_get_child(grid, MAIN_MENU_SETTINGS));
	int x = (home_tile.x1 + home_tile.x2) / 2;
	int y = (home_tile.y1 + home_tile.y2) / 2;
	int previous_holds = home_holds;
	read_at(indev, x, y, LV_INDEV_STATE_PRESSED, 20);
	read_at(indev, x - 30, y, LV_INDEV_STATE_PRESSED, 20);
	assert(player_sheet_drag_active());
	read_at(indev, x - 30, y, LV_INDEV_STATE_PRESSED, 600);
	assert(home_holds == previous_holds + 1 && lv_screen_active() == main_menu_screen);
	read_at(indev, x - 30, y, LV_INDEV_STATE_RELEASED, 20);
	settle();
	assert(lv_screen_active() == main_menu_screen && !player_sheet_is_open());
	assert(lv_obj_is_hidden(player_screen) && lv_obj_is_hidden(back));

	/* A longer swipe still opens the player instead of the tile or editor. */
	read_at(indev, x, y, LV_INDEV_STATE_PRESSED, 20);
	read_at(indev, x - 20, y, LV_INDEV_STATE_PRESSED, 20);
	read_at(indev, x - 120, y, LV_INDEV_STATE_PRESSED, 20);
	assert(player_sheet_drag_active());
	read_at(indev, x - 120, y, LV_INDEV_STATE_RELEASED, 20);
	settle();
	assert(player_sheet_is_open() && lv_screen_active() == main_menu_screen);
	tap(indev, back);
	assert(!player_sheet_is_open() && lv_obj_is_hidden(player_screen));
	assert(lv_screen_active() == main_menu_screen && lv_obj_is_hidden(back));

	/* Cover Flow's bottom-edge drag also wins over the recovery hold and tile
	 * click, even when the drag stops short and the sheet snaps back. */
	assert(addons_enabled("coverflow"));
	coverflow_set_enabled(true);
	assert(coverflow_enabled() && y >= (int)cfg.screen_height - 80);
	previous_holds = home_holds;
	read_at(indev, x, y, LV_INDEV_STATE_PRESSED, 20);
	read_at(indev, x, y - 20, LV_INDEV_STATE_PRESSED, 20);
	assert(coverflow_drag_active() && coverflow_is_open());
	read_at(indev, x, y - 20, LV_INDEV_STATE_PRESSED, 600);
	assert(home_holds == previous_holds + 1 && lv_screen_active() == main_menu_screen);
	read_at(indev, x, y - 20, LV_INDEV_STATE_RELEASED, 20);
	settle();
	assert(!coverflow_is_open() && !coverflow_drag_active() && lv_obj_is_hidden(coverflow_sheet));
	assert(!player_sheet_drag_active() && !player_sheet_is_open());
	assert(lv_screen_active() == main_menu_screen && lv_obj_is_hidden(back));

	/* A committed pull opens the real sheet and its grip closes it again. */
	read_at(indev, x, y, LV_INDEV_STATE_PRESSED, 20);
	read_at(indev, x, y - 20, LV_INDEV_STATE_PRESSED, 20);
	read_at(indev, x, y - 120, LV_INDEV_STATE_PRESSED, 20);
	assert(coverflow_drag_active());
	read_at(indev, x, y - 120, LV_INDEV_STATE_RELEASED, 20);
	settle();
	assert(coverflow_is_open() && lv_screen_active() == main_menu_screen);
	lv_area_t grip = area_of(lv_obj_get_child(coverflow_sheet, 0));
	int grip_x = (grip.x1 + grip.x2) / 2;
	int grip_y = (grip.y1 + grip.y2) / 2;
	read_at(indev, grip_x, grip_y, LV_INDEV_STATE_PRESSED, 20);
	read_at(indev, grip_x, grip_y + 20, LV_INDEV_STATE_PRESSED, 20);
	read_at(indev, grip_x, grip_y + 120, LV_INDEV_STATE_PRESSED, 20);
	read_at(indev, grip_x, grip_y + 120, LV_INDEV_STATE_RELEASED, 20);
	settle();
	assert(!coverflow_is_open() && lv_obj_is_hidden(coverflow_sheet));
	assert(!player_sheet_is_open());
	assert(lv_screen_active() == main_menu_screen && lv_obj_is_hidden(back));

	/* Enter through the real home tile and the actual Appearance row. */
	tap(indev, lv_obj_get_child(grid, MAIN_MENU_SETTINGS));
	assert(lv_screen_active() == settings_screen && !lv_obj_is_hidden(back));
	switch_screen(appearance_screen); /* the Settings page is outside this fixture */
	lv_obj_t *row_label = find_label(appearance_screen, tr("appearance_main_menu_items"));
	assert(row_label);
	lv_obj_t *row = lv_obj_get_parent(row_label);
	lv_obj_scroll_to_view_recursive(row, LV_ANIM_OFF);
	tap(indev, row);
	assert(lv_screen_active() == items);
	assert(switcher_history_contains(main_menu_screen));
	assert(switcher_history_contains(settings_screen));
	assert(switcher_history_contains(appearance_screen));
	tap_switch(indev, items, MAIN_MENU_SETTINGS, false);
	tap(indev, back);
	assert(lv_screen_active() == appearance_screen);
	tap(indev, back);
	assert(lv_screen_active() == settings_screen);
	tap(indev, back);
	assert(lv_screen_active() == main_menu_screen && lv_obj_is_hidden(back));
	assert(lv_obj_is_hidden(lv_obj_get_child(grid, MAIN_MENU_SETTINGS)));

	/* The escape hatch returns directly to the menu, not the Appearance path. */
	hold(indev, lv_obj_get_child(grid, MAIN_MENU_MUSIC), items);
	assert(switcher_history_contains(main_menu_screen));
	assert(!switcher_history_contains(appearance_screen));
	lv_obj_t *music_switch = switch_for(items, MAIN_MENU_MUSIC);
	lv_obj_t *music_card = lv_obj_get_parent(music_switch);
	lv_obj_t *list = lv_obj_get_parent(music_card);

	/* A real vertical drag scrolls the long page rather than navigating. */
	lv_obj_scroll_to_y(list, 0, LV_ANIM_OFF);
	read_at(indev, 20, 275, LV_INDEV_STATE_PRESSED, 20);
	read_at(indev, 20, 190, LV_INDEV_STATE_PRESSED, 20);
	read_at(indev, 20, 140, LV_INDEV_STATE_PRESSED, 20);
	read_at(indev, 20, 140, LV_INDEV_STATE_RELEASED, 20);
	settle();
	assert(lv_screen_active() == items && lv_obj_get_scroll_y(list) > 0);
	assert(main_menu_slot_visible(MAIN_MENU_MUSIC));

	/* A short swipe on a card snaps back without activating the switch. */
	lv_obj_scroll_to_view_recursive(music_card, LV_ANIM_OFF);
	lv_area_t card = area_of(music_card);
	swipe(indev, card.x1 + 12, (card.y1 + card.y2) / 2, 50);
	assert(lv_screen_active() == items && lv_obj_get_x(items) == 0);
	assert(main_menu_slot_visible(MAIN_MENU_MUSIC));

	/* A swipe starting on any switch must not accidentally change it, whether
	 * on or off. Ordinary taps after the canceled swipes must still work. */
	for (int i = 0; i < MAIN_MENU_COUNT; i++) {
		main_menu_slot_t slot = (main_menu_slot_t)i;
		bool original = main_menu_slot_visible(slot);
		for (int pass = 0; pass < 2; pass++) {
			lv_obj_t *control = switch_for(items, slot);
			lv_obj_scroll_to_view_recursive(control, LV_ANIM_OFF);
			lv_area_t sw = area_of(control);
			bool shown = main_menu_slot_visible(slot);
			swipe(indev, sw.x1 + 4, (sw.y1 + sw.y2) / 2, 45);
			assert(lv_screen_active() == items && lv_obj_get_x(items) == 0);
			assert(main_menu_slot_visible(slot) == shown);
			assert(lv_obj_has_state(control, LV_STATE_CHECKED) == shown);
			tap_switch(indev, items, slot, !shown);
		}
		assert(main_menu_slot_visible(slot) == original);
	}

	/* A committed swipe from the body uses settingsrow_page's handler. */
	lv_obj_scroll_to_view_recursive(music_card, LV_ANIM_OFF);
	card = area_of(music_card);
	swipe(indev, card.x1 + 12, (card.y1 + card.y2) / 2, 110);
	assert(lv_screen_active() == main_menu_screen && lv_obj_is_hidden(back));

	/* The header is outside the list and uses the editor screen's handler. */
	hold(indev, lv_obj_get_child(grid, MAIN_MENU_MUSIC), items);
	swipe(indev, 60, cfg.top_bar_height + cfg.padding + bp_header_button_size() / 2, 110);
	assert(lv_screen_active() == main_menu_screen && lv_obj_is_hidden(back));

	/* Even the empty grid retains Cover Flow's edge gesture independently of
	 * visibility; a hold during its drag must not enter the editor. */
	for (int i = 0; i < MAIN_MENU_COUNT; i++) assert(main_menu_set_slot_visible((main_menu_slot_t)i, false));
	assert(addons_enabled("coverflow") && coverflow_enabled());
	lv_area_t empty_grid = area_of(grid);
	x = (empty_grid.x1 + empty_grid.x2) / 2;
	y = empty_grid.y2 - 5;
	assert(y >= (int)cfg.screen_height - 80);
	previous_holds = home_holds;
	read_at(indev, x, y, LV_INDEV_STATE_PRESSED, 20);
	read_at(indev, x, y - 20, LV_INDEV_STATE_PRESSED, 20);
	assert(coverflow_drag_active());
	read_at(indev, x, y - 20, LV_INDEV_STATE_PRESSED, 600);
	assert(home_holds == previous_holds + 1 && lv_screen_active() == main_menu_screen);
	read_at(indev, x, y - 20, LV_INDEV_STATE_RELEASED, 20);
	settle();
	assert(!coverflow_is_open() && lv_obj_is_hidden(coverflow_sheet));
	assert(!player_sheet_drag_active() && !player_sheet_is_open());

	/* With all shortcuts hidden, the grid remains usable after a real return. */
	hold(indev, grid, items);
	tap_switch(indev, items, MAIN_MENU_MUSIC, true);
	tap(indev, back);
	assert(lv_screen_active() == main_menu_screen && lv_obj_is_hidden(back));
	assert(!lv_obj_is_hidden(lv_obj_get_child(grid, MAIN_MENU_MUSIC)));
	tap(indev, lv_obj_get_child(grid, MAIN_MENU_MUSIC));
	assert(lv_screen_active() == music_screen);
	tap(indev, back);
	assert(lv_screen_active() == main_menu_screen);

	lv_indev_delete(indev);
	assert(unlink(config) == 0);
	assert(rmdir(dir) == 0);
	puts("PASS main-menu editor navigation, sheet gestures and recovery (240x320)");
	return 0;
}
