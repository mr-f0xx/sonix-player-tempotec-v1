// Exercise the production Control Centre layout parser without the radios,
// streaming services and display behind the sheet. Section GC removes those
// unused handlers; the list migration and accessors are the real ones.
#define _DEFAULT_SOURCE
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../src/gui/shell/quickpanel.c"

// Mode answers used by quickpanel_blocked(). DAC mode deliberately does not
// block the Control Centre any more; the other device-owning modes still do.
static bool dac_active;
static bool transfer_enabled;
static bool receiver_active;

bool usbdac_is_active(void) { return dac_active; }
bool wifitransfer_get_enabled(void) { return transfer_enabled; }
bool btreceiver_is_active(void) { return receiver_active; }

// The palette the page dots are coloured from. Nothing is drawn here.
const theme_palette_t *theme(void) {
	static theme_palette_t palette;
	return &palette;
}

// The real slider callbacks, with device IO replaced by observable state.
static int test_volume = 35, volume_writes, volume_saves, bar_volume;
static long test_brightness = 60;
static int brightness_writes;
int get_volume_percent(void) { return test_volume; }
void set_volume_percent(int v) { test_volume = v > 80 ? 80 : v; volume_writes++; }
void volume_profile_persist(void) { volume_saves++; }
void topbar_refresh_volume(int v) { bar_volume = v; }
long power_get_min_brightness(void) { return 5; }
long power_get_max_brightness(void) { return 100; }
long power_get_brightness(void) { return test_brightness; }
void power_set_brightness(long v) { test_brightness = v; brightness_writes++; }
void theme_apply_slider_knob(lv_obj_t *slider) { (void)slider; }
void usbdac_get_state(usbdac_state_t *out) { memset(out, 0, sizeof(*out)); out->active = dac_active; }
lv_style_t theme_style_icon;
static const uint8_t icon_pixels[32 * 32 * 4];
#define TEST_ICON { .header = { .magic = LV_IMAGE_HEADER_MAGIC, .cf = LV_COLOR_FORMAT_ARGB8888, \
	.w = 32, .h = 32, .stride = 32 * 4 }, .data_size = sizeof(icon_pixels), .data = icon_pixels }
const lv_image_dsc_t icon_sun = TEST_ICON;
const lv_image_dsc_t icon_volume_mute = TEST_ICON;
const lv_image_dsc_t icon_volume_low = TEST_ICON;
const lv_image_dsc_t icon_volume_high = TEST_ICON;

// LVGL's configured default font; no widgets are created in this suite.
lv_font_t font_ui_14;

static void reset_layout(void) {
	order_loaded = false;
	hidden_n = 0;
	memset(slots, QP_BTN_NONE, sizeof(slots));
	memset(hidden, QP_BTN_NONE, sizeof(hidden));
}
static bool contains(const uint8_t *list, int n, quickpanel_button_t button) {
	for (int i = 0; i < n; i++) {
		if (list[i] == button) { return true; }
	}
	return false;
}
static void check_partition(void) {
	bool seen[QP_BTN_COUNT] = {false};
	int in_n = quickpanel_in_use_count();
	int out_n = quickpanel_hidden_count();
	assert(in_n <= QP_SLOT_COUNT && in_n + out_n == QP_BTN_COUNT);
	for (int i = 0; i < in_n; i++) {
		int b = quickpanel_in_use_at(i);
		assert(b >= 0 && b < QP_BTN_COUNT && !seen[b]);
		seen[b] = true;
	}
	for (int i = 0; i < out_n; i++) {
		int b = quickpanel_hidden_at(i);
		assert(b >= 0 && b < QP_BTN_COUNT && !seen[b]);
		seen[b] = true;
	}
}

static void test_quickpanel_mode_gating(void) {
	dac_active = true;
	transfer_enabled = false;
	receiver_active = false;
	assert(usbdac_is_active());
	assert(!quickpanel_blocked());

	dac_active = false;
	transfer_enabled = true;
	assert(quickpanel_blocked());

	transfer_enabled = false;
	receiver_active = true;
	assert(quickpanel_blocked());

	receiver_active = false;
	assert(!quickpanel_blocked());
}

// Exercise production layout and real LVGL hit-testing on the 240x320 panel.
static void test_level_rows(const char *path) {
	lv_display_t *display = lv_display_create(240, 320);
	lv_obj_t *screen = lv_display_get_screen_active(display);
	lv_obj_set_style_pad_all(screen, 0, 0);
	qp_compact = true;
	circle_button_size = 44; circle_button_gap = 4;
	card_padding = 6; card_row_gap = 4; card_gap = 6;
	level_row_h = 24; dots_room = 6; normal_page_h = 114;
	card_h_rest = 154; np_card_h_rest = 104; card_top = 28;
	pages_used = 2; edit_mode = false;
	controls_card = lv_obj_create(screen);
	lv_obj_set_size(controls_card, 228, card_h_rest);
	lv_obj_set_pos(controls_card, 6, card_top);
	lv_obj_set_style_pad_all(controls_card, card_padding, 0);
	lv_obj_set_style_pad_gap(controls_card, card_row_gap, 0);
	lv_obj_set_style_border_width(controls_card, 0, 0);
	lv_obj_set_flex_flow(controls_card, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(controls_card, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	pages_view = lv_obj_create(controls_card);
	lv_obj_set_size(pages_view, 216, normal_page_h);
	lv_obj_set_style_pad_all(pages_view, 0, 0);
	lv_obj_set_style_border_width(pages_view, 0, 0);
	for (int i = 0; i < QP_PAGE_COUNT; i++) {
		page_row[i] = lv_obj_create(pages_view);
		lv_obj_set_size(page_row[i], 216, normal_page_h);
	}
	level_row = make_level_row(&level_icon, &level_slider);
	volume_row = make_level_row(&volume_icon, &volume_slider);
	page_dots = lv_obj_create(controls_card);
	lv_obj_set_ignore_layout(page_dots, true);
	lv_obj_set_size(page_dots, 120, dots_room + card_padding);
	lv_obj_align(page_dots, LV_ALIGN_BOTTOM_MID, 0, card_padding);
	np_card = lv_obj_create(screen);
	lv_obj_set_style_pad_all(np_card, card_padding, 0);
	lv_obj_set_style_border_width(np_card, 0, 0);
	lv_obj_set_size(np_card, 228, np_card_h_rest);
	np_title = lv_obj_create(np_card); np_artist = lv_obj_create(np_card);
	np_transport = lv_obj_create(np_card);
	lv_obj_set_size(np_transport, 200, 52);

	config_set_bool("quickpanel", "volume_slider", true);
	assert(!quickpanel_both_sliders());
	quickpanel_set_both_sliders(true);
	lv_obj_update_layout(screen);
	assert(!level_is_volume && level_both && !lv_obj_is_hidden(volume_row));
	assert(lv_obj_is_hidden(np_title) && lv_obj_is_hidden(np_artist));
	assert(lv_obj_get_height(level_row) == 40 && lv_obj_get_height(volume_row) == 40);
	assert(lv_obj_get_height(pages_view) >= 2 * circle_button_size + circle_button_gap);
	assert(lv_obj_get_height(np_card) >= 44 + 2 * card_padding && lv_obj_get_height(np_transport) == 44);
	lv_area_t a, b, card, np;
	lv_obj_get_coords(level_row, &a); lv_obj_get_coords(volume_row, &b);
	lv_obj_get_coords(controls_card, &card); lv_obj_get_coords(np_card, &np);
	assert(a.y2 < b.y1 && b.y2 <= card.y2 && card.y2 < np.y1 && np.y2 < 320);
	assert(lv_obj_get_width(level_slider) >= 120 && lv_obj_get_width(volume_slider) >= 120);
	// The centre of each 40-pixel row must actually hit that rail, including
	// points well above and below the visible 10-pixel track.
	for (int i = 0; i < 2; i++) {
		lv_obj_t *slider = i ? volume_slider : level_slider;
		lv_area_t row = i ? b : a, rail;
		lv_obj_get_coords(slider, &rail);
		for (int y = row.y1 + 2; y <= row.y2 - 2; y++) {
			lv_point_t pt = {.x = (rail.x1 + rail.x2) / 2, .y = y};
			assert(lv_indev_search_obj(screen, &pt) == slider);
		}
	}
	lv_slider_set_value(level_slider, 42, LV_ANIM_OFF);
	lv_obj_send_event(level_slider, LV_EVENT_VALUE_CHANGED, NULL);
	assert(test_brightness == 42 && brightness_writes == 1 && volume_writes == 0);
	lv_obj_send_event(level_slider, LV_EVENT_RELEASED, NULL);
	assert(config_get_int("screen", "brightness", 0) == 42);
	lv_slider_set_value(volume_slider, 95, LV_ANIM_OFF);
	lv_obj_send_event(volume_slider, LV_EVENT_VALUE_CHANGED, NULL);
	assert(test_volume == 80 && lv_slider_get_value(volume_slider) == 80 && bar_volume == 80);
	lv_obj_send_event(volume_slider, LV_EVENT_RELEASED, NULL);
	assert(volume_saves == 1);
	test_volume = 25; level_follow();
	assert(lv_slider_get_value(volume_slider) == 25);
	// Polls and reopen syncs never pull either knob away during a drag.
	lv_obj_add_state(volume_slider, LV_STATE_PRESSED);
	lv_slider_set_value(volume_slider, 55, LV_ANIM_OFF);
	level_follow(); level_sync();
	assert(lv_slider_get_value(volume_slider) == 55);
	lv_obj_remove_state(volume_slider, LV_STATE_PRESSED);
	lv_obj_add_state(level_slider, LV_STATE_PRESSED);
	lv_slider_set_value(level_slider, 73, LV_ANIM_OFF);
	level_sync(); assert(lv_slider_get_value(level_slider) == 73);
	lv_obj_remove_state(level_slider, LV_STATE_PRESSED);
	assert(config_save()); config_init(path);
	assert(quickpanel_both_sliders() && quickpanel_volume_slider());
	// One and two pages, and each gesture scheme, retain the same two rails.
	for (int pages = 1; pages <= 2; pages++) {
		for (int top = 20; top <= 44; top += 24) {
			pages_used = pages; card_top = top; level_layout();
			lv_obj_set_y(controls_card, top);
			lv_obj_update_layout(screen);
			lv_obj_get_coords(volume_row, &b); lv_obj_get_coords(np_card, &np);
			assert(b.y2 < np.y1 && np.y2 < 320 && lv_obj_get_height(np_card) >= 56);
		}
	}
	pages_used = 2; card_top = 28; lv_obj_set_y(controls_card, card_top);
	edit_mode = true; level_layout(); assert(lv_obj_is_hidden(volume_row));
	edit_mode = false; level_layout(); assert(!lv_obj_is_hidden(volume_row));
	quickpanel_set_both_sliders(false);
	lv_obj_update_layout(screen);
	assert(level_is_volume && lv_obj_is_hidden(volume_row));
	assert(!lv_obj_is_hidden(np_title) && !lv_obj_is_hidden(np_artist));
	assert(lv_obj_get_height(pages_view) == normal_page_h);
	assert(lv_obj_get_height(np_card) == np_card_h_rest - dots_room);
	assert(lv_obj_get_height(np_transport) == 52);
	// DAC mode needs its status text even when the compact transport is gone.
	dac_active = true; quickpanel_set_both_sliders(true);
	assert(!lv_obj_is_hidden(np_title) && !lv_obj_is_hidden(np_artist));
	dac_active = false; quickpanel_set_both_sliders(false);
	lv_display_delete(display);
	controls_card = np_card = pages_view = level_row = volume_row = NULL;
	level_slider = volume_slider = level_icon = volume_icon = NULL;
	np_title = np_artist = np_transport = page_dots = NULL;
	for (int i = 0; i < QP_PAGE_COUNT; i++) page_row[i] = NULL;
	config_set_bool("quickpanel", "volume_slider", false);
}

int main(void) {
	lv_init();
	char dir[] = "/tmp/sonix-qp-layout-XXXXXX";
	assert(mkdtemp(dir));
	char path[512];
	snprintf(path, sizeof(path), "%s/config.ini", dir);
	config_init(path);
	test_quickpanel_mode_gating();
	test_level_rows(path);

	// A config that has never carried a layout gets the panel as it ships: the
	// eight buttons the first page has always had, in their old order, and a
	// second page of the new toggles. Everything else waits in Settings.
	static const uint8_t page_one[QP_SLOT_VISIBLE] = {QP_BTN_WIFI, QP_BTN_BLUETOOTH, QP_BTN_AIRPLAY, QP_BTN_MSEB,
													  QP_BTN_EQ,   QP_BTN_GAIN,	  QP_BTN_LINEOUT, QP_BTN_SONIXLINK};
	static const uint8_t page_two[QP_SLOT_VISIBLE] = {QP_BTN_PEQ,		QP_BTN_AIRPLANE, QP_BTN_CROSSFEED, QP_BTN_REPLAYGAIN,
													  QP_BTN_MONO, QP_BTN_KEYLOCK,  QP_BTN_SCREEN_OFF, QP_BTN_POWER};
	assert(QP_PAGE_COUNT == 2 && QP_SLOT_COUNT == 2 * QP_SLOT_VISIBLE);
	assert(quickpanel_in_use_count() == QP_SLOT_COUNT);
	for (int i = 0; i < QP_SLOT_VISIBLE; i++) {
		assert(quickpanel_in_use_at(i) == page_one[i]);
		assert(quickpanel_in_use_at(QP_SLOT_VISIBLE + i) == page_two[i]);
	}
	assert(quickpanel_hidden_count() == QP_BTN_COUNT - QP_SLOT_COUNT);
	assert(contains(hidden, hidden_n, QP_BTN_PEQ_PROFILE));
	assert(contains(hidden, hidden_n, QP_BTN_SOUNDFIELD));
	assert(contains(hidden, hidden_n, QP_BTN_LED));
	assert(contains(hidden, hidden_n, QP_BTN_ROTATE));
	assert(strcmp(button_key[QP_BTN_PEQ_PROFILE], "peq_profile") == 0);
	assert(strcmp(quickpanel_button_tag(QP_BTN_PEQ_PROFILE), "peq_profile") == 0);
	check_partition();

	// Every button has a key and a tag, and the keys are all different: they
	// are what the config file stores.
	for (int i = 0; i < QP_BTN_COUNT; i++) {
		assert(button_key[i] && button_key[i][0] && button_tag[i] && button_tag[i][0]);
		assert(strlen(button_key[i]) <= 15);
		for (int j = 0; j < i; j++) {
			assert(strcmp(button_key[i], button_key[j]) != 0);
		}
	}
	assert(strcmp(button_key[QP_BTN_AIRPLANE], "airplane") == 0);
	assert(strcmp(button_key[QP_BTN_SCREEN_OFF], "screen_off") == 0);

	// The setting that is not a layout does not make one: a player whose owner
	// only moved the slider to the volume is still on the shipped panel.
	config_set_bool("quickpanel", "volume_slider", true);
	reset_layout();
	assert(quickpanel_in_use_at(QP_SLOT_VISIBLE + 1) == QP_BTN_AIRPLANE);
	check_partition();
	config_set_bool("quickpanel", "volume_slider", false);

	// A layout saved from the shipped one reads back the same.
	order_save();
	reset_layout();
	assert(quickpanel_in_use_count() == QP_SLOT_COUNT);
	assert(quickpanel_in_use_at(QP_SLOT_VISIBLE + 7) == QP_BTN_POWER);
	assert(quickpanel_hidden_count() == QP_BTN_COUNT - QP_SLOT_COUNT);
	check_partition();

	// An existing v4 layout keeps every chosen position. The new button takes
	// an empty place; existing empty places remain empty.
	config_set("quickpanel", "order", "peq,eq,wifi,bluetooth,-,-,-,-");
	config_set("quickpanel", "order_more", "");
	config_set("quickpanel", "hidden", "mseb,airplay,gain,lineout,sonixlink,dlna,sleep_music,sleep_audiobook");
	config_set("quickpanel", "hidden_more", "sleep_podcast,wifi_transfer,gapless,fade");
	config_set_int("quickpanel", "layout", 4);
	reset_layout();
	assert(quickpanel_in_use_count() == 5);
	assert(quickpanel_in_use_at(0) == QP_BTN_PEQ);
	assert(quickpanel_in_use_at(3) == QP_BTN_BLUETOOTH);
	assert(quickpanel_in_use_at(4) == QP_BTN_PEQ_PROFILE);
	assert(slots[5] == QP_BTN_NONE);
	check_partition();

	// A full saved grid is not rearranged: the new control waits in the
	// settings list, where the user can explicitly make room for it.
	config_set("quickpanel", "order", "wifi,bluetooth,airplay,mseb,eq,gain,lineout,peq");
	config_set("quickpanel", "order_more", "sonixlink,dlna,sleep_music,sleep_audiobook,sleep_podcast,wifi_transfer,gapless,fade");
	config_set("quickpanel", "hidden", "");
	config_set("quickpanel", "hidden_more", "");
	reset_layout();
	assert(quickpanel_in_use_count() == QP_SLOT_COUNT);
	assert(quickpanel_in_use_at(15) == QP_BTN_FADE);
	assert(quickpanel_hidden_count() == QP_BTN_COUNT - QP_SLOT_COUNT);
	assert(contains(hidden, hidden_n, QP_BTN_PEQ_PROFILE));
	assert(contains(hidden, hidden_n, QP_BTN_AIRPLANE) && contains(hidden, hidden_n, QP_BTN_POWER));
	check_partition();

	// Moving it into the first eight makes it visible without expanding the
	// sheet. PEQ shifts into the extension, the last control waits in Settings,
	// and no chosen control is lost. The choice survives boot.
	quickpanel_move_in_use(QP_BTN_PEQ_PROFILE, 7);
	assert(quickpanel_in_use_at(7) == QP_BTN_PEQ_PROFILE);
	assert(quickpanel_in_use_at(8) == QP_BTN_PEQ);
	assert(contains(hidden, hidden_n, QP_BTN_FADE));
	check_partition();
	assert(config_save());
	config_init(path);
	reset_layout();
	assert(quickpanel_in_use_at(7) == QP_BTN_PEQ_PROFILE);
	assert(quickpanel_in_use_count() == QP_SLOT_COUNT);
	check_partition();

	// Edit mode: a tap picks a tile up, a tap on another swaps the two (here
	// across the pages), a tap on the picked-up one or on nothing puts it down,
	// and a swap is saved. The first run of this section starts from the shipped
	// panel again.
	remove(path);
	config_init(path);
	reset_layout();
	assert(quickpanel_in_use_at(0) == QP_BTN_WIFI && quickpanel_in_use_at(15) == QP_BTN_POWER);
	edit_mode = true;
	edit_sel = QP_BTN_NONE;
	edit_tap(QP_BTN_WIFI);
	assert(edit_sel == QP_BTN_WIFI);
	edit_tap(QP_BTN_WIFI);
	assert(edit_sel == QP_BTN_NONE);
	edit_tap(QP_BTN_WIFI);
	edit_tap(QP_BTN_NONE);
	assert(edit_sel == QP_BTN_NONE && quickpanel_in_use_at(0) == QP_BTN_WIFI);
	edit_tap(QP_BTN_WIFI);
	edit_tap(QP_BTN_POWER);
	assert(edit_sel == QP_BTN_NONE);
	assert(quickpanel_in_use_at(0) == QP_BTN_POWER && quickpanel_in_use_at(15) == QP_BTN_WIFI);
	assert(quickpanel_in_use_count() == QP_SLOT_COUNT);
	check_partition();
	assert(config_save());
	config_init(path);
	reset_layout();
	assert(quickpanel_in_use_at(0) == QP_BTN_POWER && quickpanel_in_use_at(15) == QP_BTN_WIFI);
	check_partition();
	edit_mode = false;

	// More buttons left out than two keys can hold: they are carried by the
	// further hidden keys and none is lost over a save and a boot.
	config_set("quickpanel", "order", "wifi,-,-,-,-,-,-,-");
	config_set("quickpanel", "order_more", "-,-,-,-,-,-,-,-");
	config_set("quickpanel", "hidden", "");
	config_set("quickpanel", "hidden_more", "");
	config_set("quickpanel", "hidden_more2", "");
	config_set("quickpanel", "hidden_more3", "");
	config_set_int("quickpanel", "layout", 4);
	reset_layout();
	assert(quickpanel_in_use_count() == 1);
	assert(quickpanel_hidden_count() == QP_BTN_COUNT - 1);
	check_partition();
	order_save();
	assert(config_get("quickpanel", "hidden_more2", NULL) && config_get("quickpanel", "hidden_more2", NULL)[0]);
	assert(config_save());
	config_init(path);
	reset_layout();
	assert(quickpanel_in_use_count() == 1);
	assert(quickpanel_hidden_count() == QP_BTN_COUNT - 1);
	check_partition();

	// Every key written stays inside what a setting can hold.
	for (int k = 0; k < QP_HIDDEN_KEYS; k++) {
		const char *v = config_get("quickpanel", hidden_key_name[k], "");
		assert(strlen(v) <= 127);
	}

	printf("quickpanel: DAC-mode access, shipped two-page panel, saved layouts and hidden-list capacity passed\n");
	return 0;
}
