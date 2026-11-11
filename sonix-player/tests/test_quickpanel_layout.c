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

// Mode answers used by quickpanel_blocked(). DAC mode and the Bluetooth receiver
// deliberately do not block the Control Centre; a Wi-Fi transfer still does.
static bool dac_active;
static bool transfer_enabled;
static bool receiver_active;

bool usbdac_is_active(void) { return dac_active; }
bool wifitransfer_get_enabled(void) { return transfer_enabled; }
bool btreceiver_is_active(void) { return receiver_active; }

// What the receiver says about itself, and the track the phone names.
static btreceiver_state_t receiver_state;
static bool receiver_has_track;
static bt_track_t receiver_track;
void btreceiver_get_state(btreceiver_state_t *out) {
	*out = receiver_state;
	out->active = receiver_active;
}
bool bluetooth_receiver_track(bt_track_t *out) {
	if (receiver_has_track) {
		*out = receiver_track;
	}
	return receiver_has_track;
}

// The receiver's session, as the control centre sees it: whether the user is in
// it, which page it was asked to wait behind, and how often it was ended.
static bool session_in_use;
static lv_obj_t *session_held_for;
static int session_ends;
bool btreceiversession_in_use(void) { return session_in_use; }
void btreceiversession_hold_for(lv_obj_t *screen) { session_held_for = screen; }
void btreceiversession_end(void) {
	session_ends++;
	receiver_active = false;
	session_in_use = false;
}

// The pages the receiver can wait behind, and one it cannot.
static lv_obj_t *peq_stub, *eq_stub, *mseb_stub;
lv_obj_t *peqpage_screen(void) { return peq_stub; }
lv_obj_t *musicsettings_eq_screen(void) { return eq_stub; }
lv_obj_t *musicsettings_mseb_screen(void) { return mseb_stub; }
const char *tr(const char *key) { return key; }

// The card's two lines, as the card was last told to show them.
static char card_title[512], card_line[512];
void scrolltext_set(lv_obj_t *label, const char *text) {
	char *dest = label == np_title ? card_title : card_line;
	snprintf(dest, sizeof(card_line), "%s", text); // both are the same size
}

// The question the control centre asks before leaving the receiver, and the
// page it was asked for. The dialog is recorded, not drawn; the test answers it.
static int confirm_calls;
static const char *confirm_title_seen;
static void (*confirm_ok)(void *);
static void *confirm_ok_user;
static void (*confirm_cancel)(void *);
static void *confirm_cancel_user;
void confirm_show_with_cancel(const char *title, const char *message, const char *ok_label, void (*on_ok)(void *user),
							  void *user, const char *cancel_label, void (*on_cancel)(void *user), void *cancel_user) {
	(void)message;
	(void)ok_label;
	(void)cancel_label;
	confirm_calls++;
	confirm_title_seen = title;
	confirm_ok = on_ok;
	confirm_ok_user = user;
	confirm_cancel = on_cancel;
	confirm_cancel_user = cancel_user;
}
static int popups;
static const char *popup_seen;
void gui_notify_popup(const char *tag) {
	popups++;
	popup_seen = tag;
}
static int page_switches;
static lv_obj_t *page_switched_to;
void switch_screen(lv_obj_t *target) {
	page_switches++;
	page_switched_to = target;
}
bool player_sheet_is_open(void) { return false; }
bool gestures_stock(void) { return false; } // the classic scheme: the sheet parks above
void player_sheet_close(bool animate) { (void)animate; }
void switcher_set_player_return(lv_obj_t *screen) { (void)screen; }

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

	// The receiver lets the sheet open over it: PEQ and the rest are the point.
	// What it must not allow is handled by the guards below, not by a locked
	// door. A file transfer still owns the screen, receiver or not.
	transfer_enabled = false;
	receiver_active = true;
	assert(!quickpanel_blocked());
	transfer_enabled = true;
	assert(quickpanel_blocked());

	transfer_enabled = false;
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
	// ...and so does a Bluetooth receiver, whose card is the same two lines.
	receiver_active = true; quickpanel_set_both_sliders(true);
	assert(!lv_obj_is_hidden(np_title) && !lv_obj_is_hidden(np_artist));
	receiver_active = false; quickpanel_set_both_sliders(false);
	lv_display_delete(display);
	controls_card = np_card = pages_view = level_row = volume_row = NULL;
	level_slider = volume_slider = level_icon = volume_icon = NULL;
	np_title = np_artist = np_transport = page_dots = NULL;
	for (int i = 0; i < QP_PAGE_COUNT; i++) page_row[i] = NULL;
	config_set_bool("quickpanel", "volume_slider", false);
}


// What the receiver's card says: the track the phone names, or the mode's own
// name; who it is by or what the stream is doing; the codec and rate at the end.
static void test_receiver_card_text(void) {
	btreceiver_state_t st = {0};
	bt_track_t track = {0};
	char title[200], line[300];

	st.active = true;
	receiver_card_text(&st, false, NULL, title, sizeof(title), line, sizeof(line));
	assert(!strcmp(title, "btreceiver_title") && !strcmp(line, "btreceiver_waiting"));

	snprintf(st.codec, sizeof(st.codec), "aptX");
	st.sample_rate = 44100;
	receiver_card_text(&st, false, NULL, title, sizeof(title), line, sizeof(line));
	assert(!strcmp(line, "btreceiver_waiting · aptX 44.1 kHz"));

	st.streaming = true;
	receiver_card_text(&st, false, NULL, title, sizeof(title), line, sizeof(line));
	assert(!strcmp(title, "btreceiver_title") && !strcmp(line, "btreceiver_playing · aptX 44.1 kHz"));

	st.sample_rate = 96000;
	snprintf(st.codec, sizeof(st.codec), "LDAC");
	receiver_card_text(&st, false, NULL, title, sizeof(title), line, sizeof(line));
	assert(!strcmp(line, "btreceiver_playing · LDAC 96 kHz"));

	// A track the phone names: its title on the first line, its artist on the second.
	snprintf(track.title, sizeof(track.title), "Song");
	snprintf(track.artist, sizeof(track.artist), "Artist");
	snprintf(track.album, sizeof(track.album), "Album");
	receiver_card_text(&st, true, &track, title, sizeof(title), line, sizeof(line));
	assert(!strcmp(title, "Song") && !strcmp(line, "Artist · LDAC 96 kHz"));
	track.artist[0] = '\0';
	receiver_card_text(&st, true, &track, title, sizeof(title), line, sizeof(line));
	assert(!strcmp(line, "Album · LDAC 96 kHz"));
	track.album[0] = '\0';
	receiver_card_text(&st, true, &track, title, sizeof(title), line, sizeof(line));
	assert(!strcmp(title, "Song") && !strcmp(line, "btreceiver_playing · LDAC 96 kHz"));

	// A track that is not being heard is not what the card is about.
	st.streaming = false;
	receiver_card_text(&st, true, &track, title, sizeof(title), line, sizeof(line));
	assert(!strcmp(title, "btreceiver_title") && !strcmp(line, "btreceiver_waiting · LDAC 96 kHz"));

	// Only one half of the format known.
	st.streaming = true;
	st.sample_rate = 0;
	receiver_card_text(&st, false, NULL, title, sizeof(title), line, sizeof(line));
	assert(!strcmp(line, "btreceiver_playing · LDAC"));
	st.codec[0] = '\0';
	st.sample_rate = 48000;
	receiver_card_text(&st, false, NULL, title, sizeof(title), line, sizeof(line));
	assert(!strcmp(line, "btreceiver_playing · 48 kHz"));
	st.sample_rate = 0;
	receiver_card_text(&st, false, NULL, title, sizeof(title), line, sizeof(line));
	assert(!strcmp(line, "btreceiver_playing"));

	// A problem replaces the second line, whatever else is known.
	snprintf(st.error, sizeof(st.error), "The stream could not be opened.");
	receiver_card_text(&st, true, &track, title, sizeof(title), line, sizeof(line));
	assert(!strcmp(title, "btreceiver_title") && !strcmp(line, "The stream could not be opened."));

	// Never past the end of what it was given.
	char tiny_title[8], tiny_line[8];
	st.error[0] = '\0';
	snprintf(track.title, sizeof(track.title), "A very long title indeed");
	snprintf(track.artist, sizeof(track.artist), "An equally long artist name");
	st.streaming = true;
	receiver_card_text(&st, true, &track, tiny_title, sizeof(tiny_title), tiny_line, sizeof(tiny_line));
	assert(strlen(tiny_title) == 7 && strlen(tiny_line) == 7);
}

// The card itself over a receiver: two lines, no transport, nothing to tap.
static void test_receiver_card(void) {
	lv_display_t *display = lv_display_create(240, 320);
	lv_obj_t *screen = lv_display_get_screen_active(display);
	np_card = lv_obj_create(screen);
	np_title = lv_obj_create(np_card);
	np_artist = lv_obj_create(np_card);
	np_transport = lv_obj_create(np_card);

	receiver_active = true;
	memset(&receiver_state, 0, sizeof(receiver_state));
	snprintf(receiver_state.codec, sizeof(receiver_state.codec), "AAC");
	receiver_state.sample_rate = 44100;
	receiver_state.streaming = true;
	receiver_has_track = true;
	memset(&receiver_track, 0, sizeof(receiver_track));
	snprintf(receiver_track.title, sizeof(receiver_track.title), "Song");
	snprintf(receiver_track.artist, sizeof(receiver_track.artist), "Artist");

	// Hidden and tappable as a local card would leave them: the receiver's card
	// has to put them right itself.
	lv_obj_set_hidden(np_title, true);
	lv_obj_set_hidden(np_artist, true);
	lv_obj_set_hidden(np_transport, false);
	lv_obj_set_clickable(np_title, true);
	lv_obj_set_clickable(np_artist, true);
	assert(refresh_mode_card());
	assert(lv_obj_is_hidden(np_transport));
	assert(!lv_obj_is_hidden(np_title) && !lv_obj_is_hidden(np_artist));
	assert(!lv_obj_is_clickable(np_title) && !lv_obj_is_clickable(np_artist));
	assert(!strcmp(card_title, "Song") && !strcmp(card_line, "Artist · AAC 44.1 kHz"));

	// The transport's buttons are local: they do nothing while the receiver is on.
	assert(mode_owns_output() && receiver_owns_output() && !dac_mode_owns_output());

	// Nothing the phone names: the mode's own name, and what it is doing.
	receiver_has_track = false;
	assert(refresh_mode_card());
	assert(!strcmp(card_title, "btreceiver_title") && !strcmp(card_line, "btreceiver_playing · AAC 44.1 kHz"));

	// The DAC wins when both are somehow claimed: its card is the older one.
	receiver_has_track = false;
	dac_active = true;
	assert(refresh_mode_card());
	assert(!strcmp(card_title, "dac"));
	dac_active = false;

	// Neither: the local player's card is left alone for the caller to fill.
	receiver_active = false;
	assert(!mode_owns_output());
	card_title[0] = card_line[0] = '\0';
	assert(!refresh_mode_card() && card_title[0] == '\0' && card_line[0] == '\0');
	lv_display_delete(display);
	np_card = np_title = np_artist = np_transport = NULL;
}

// Bluetooth and airplane mode end the stream, so over the receiver they say why
// instead of doing it -- playing, or with the page open waiting for a phone.
static void test_radios_held_by_receiver(void) {
	popups = 0;
	receiver_active = false;
	session_in_use = false;
	assert(!radios_held_by_receiver() && popups == 0);

	receiver_active = true;
	assert(radios_held_by_receiver() && popups == 1);
	assert(!strcmp(popup_seen, "btreceiver_radios_held"));
	receiver_active = false;

	// The page open and nothing connected yet: still the receiver's radio.
	session_in_use = true;
	assert(radios_held_by_receiver() && popups == 2);
	session_in_use = false;
}

static void answer_and_run_async(bool yes) {
	if (yes) {
		confirm_ok(confirm_ok_user);
	} else {
		confirm_cancel(confirm_cancel_user);
	}
	lv_timer_handler(); // the one turn of the event loop the move waits for
}

static void reset_navigation(void) {
	page_switches = confirm_calls = session_ends = 0;
	page_switched_to = NULL;
	session_held_for = NULL;
	receiver_active = false;
	session_in_use = false;
}

// The equaliser's pages are what the sheet is allowed over the receiver for, so
// they open without ending it and without asking: the receiver is held for the
// page, which is how it is still there when the user backs out.
static void test_open_equaliser_pages_over_receiver(void) {
	lv_display_t *display = lv_display_create(240, 320);
	peq_stub = lv_obj_create(NULL);
	eq_stub = lv_obj_create(NULL);
	mseb_stub = lv_obj_create(NULL);
	lv_obj_t *const pages[] = {peq_stub, eq_stub, mseb_stub};

	for (size_t i = 0; i < sizeof(pages) / sizeof(pages[0]); i++) {
		// Playing.
		reset_navigation();
		receiver_active = true;
		session_in_use = true;
		open_page(pages[i]);
		assert(confirm_calls == 0 && session_ends == 0);
		assert(session_held_for == pages[i]);
		assert(page_switches == 1 && page_switched_to == pages[i]);

		// Parked already behind another equaliser page: the same again.
		reset_navigation();
		receiver_active = true;
		session_in_use = true;
		open_page(pages[i]);
		assert(session_held_for == pages[i] && page_switches == 1 && session_ends == 0);

		// The page open and waiting for a phone: held for too, so the A2DP sink
		// is still registered when the phone connects while the user is in PEQ.
		reset_navigation();
		session_in_use = true;
		open_page(pages[i]);
		assert(confirm_calls == 0 && session_ends == 0);
		assert(session_held_for == pages[i] && page_switches == 1);
	}

	// No receiver at all: nothing is held, nothing asked, the page opens.
	reset_navigation();
	open_page(peq_stub);
	assert(session_held_for == NULL && confirm_calls == 0 && session_ends == 0);
	assert(page_switches == 1 && page_switched_to == peq_stub);

	// Already on the page: nothing to hold for and nothing to switch.
	reset_navigation();
	receiver_active = true;
	session_in_use = true;
	lv_screen_load(peq_stub);
	open_page(peq_stub);
	assert(session_held_for == NULL && page_switches == 0 && confirm_calls == 0);

	reset_navigation();
	lv_display_delete(display);
	peq_stub = eq_stub = mseb_stub = NULL;
}

// Every other page is somewhere the receiver cannot follow: playing, that is a
// question first and the page opens only on yes; with nothing playing the
// session is ended quietly and the page opens at once.
static void test_open_other_pages_over_receiver(void) {
	lv_display_t *display = lv_display_create(240, 320);
	peq_stub = lv_obj_create(NULL);
	eq_stub = lv_obj_create(NULL);
	mseb_stub = lv_obj_create(NULL);
	lv_obj_t *target = lv_obj_create(NULL);

	// No receiver: straight there, nothing asked, nothing held, nothing ended.
	reset_navigation();
	open_page(target);
	assert(page_switches == 1 && page_switched_to == target);
	assert(confirm_calls == 0 && session_ends == 0 && session_held_for == NULL);

	// Playing: nothing moves until the user has answered.
	reset_navigation();
	receiver_active = true;
	session_in_use = true;
	open_page(target);
	assert(confirm_calls == 1 && !strcmp(confirm_title_seen, "btreceiver_leave_confirm"));
	assert(page_switches == 0 && session_ends == 0 && receiver_active && session_held_for == NULL);
	lv_timer_handler();
	assert(page_switches == 0);

	// Yes: the session ends, then -- a turn of the loop later -- the page opens.
	answer_and_run_async(true);
	assert(session_ends == 1 && !receiver_active);
	assert(page_switches == 1 && page_switched_to == target);

	// No: nothing ends, nothing opens, now or later.
	reset_navigation();
	receiver_active = true;
	session_in_use = true;
	open_page(target);
	assert(confirm_calls == 1);
	answer_and_run_async(false);
	lv_timer_handler();
	assert(session_ends == 0 && page_switches == 0 && receiver_active);

	// Asked again after a refusal: the old answer is not left lying about, and a
	// "yes" can only ever run what it was asked for.
	assert(after_receiver_fn == NULL);
	confirm_calls = 0;
	open_page(target);
	assert(confirm_calls == 1 && after_receiver_fn != NULL);
	answer_and_run_async(true);
	assert(page_switches == 1 && page_switched_to == target && after_receiver_fn == NULL);

	// A "yes" given after the receiver ended on its own still opens the page.
	reset_navigation();
	receiver_active = true;
	session_in_use = true;
	open_page(target);
	receiver_active = false;
	answer_and_run_async(true);
	assert(page_switches == 1 && page_switched_to == target);

	// Nothing playing but the page open (or parked) waiting for a phone: leaving
	// is just leaving. The session is ended -- parked, that is what gives the
	// sink back -- and the page opens, with no question for a silence.
	reset_navigation();
	session_in_use = true;
	open_page(target);
	assert(confirm_calls == 0 && session_ends == 1);
	assert(page_switches == 1 && page_switched_to == target && session_held_for == NULL);

	reset_navigation();
	lv_display_delete(display);
	peq_stub = eq_stub = mseb_stub = NULL;
}

// What the file-transfer button does through the same two doors.
static int then_runs;
static void then_cb(void *user) {
	then_runs += user ? *(int *)user : 1;
}

static void test_leave_receiver_then(void) {
	lv_display_t *display = lv_display_create(240, 320);
	int by_two = 2;

	// Playing: held for the answer.
	reset_navigation();
	then_runs = 0;
	receiver_active = true;
	session_in_use = true;
	leave_receiver_then(then_cb, &by_two);
	assert(confirm_calls == 1 && then_runs == 0 && session_ends == 0);
	answer_and_run_async(true);
	assert(then_runs == 2 && session_ends == 1);

	// Declined: it never runs.
	reset_navigation();
	then_runs = 0;
	receiver_active = true;
	session_in_use = true;
	leave_receiver_then(then_cb, &by_two);
	answer_and_run_async(false);
	assert(then_runs == 0 && session_ends == 0);

	// Not playing: ended quietly and run at once.
	reset_navigation();
	then_runs = 0;
	session_in_use = true;
	leave_receiver_then(then_cb, NULL);
	assert(confirm_calls == 0 && then_runs == 1 && session_ends == 1);

	reset_navigation();
	lv_display_delete(display);
}

// The receiver is a session for the control centre's purposes whenever the user
// is in it, and only that makes its card, its radios and its pages what they are.
static void test_receiver_session(void) {
	receiver_active = false;
	session_in_use = false;
	assert(!receiver_session());
	receiver_active = true;
	assert(receiver_session());
	receiver_active = false;
	session_in_use = true;
	assert(receiver_session());
	session_in_use = false;

	// Only three pages are the equaliser's.
	lv_display_t *display = lv_display_create(240, 320);
	peq_stub = lv_obj_create(NULL);
	eq_stub = lv_obj_create(NULL);
	mseb_stub = lv_obj_create(NULL);
	lv_obj_t *other = lv_obj_create(NULL);
	assert(page_serves_receiver(peq_stub) && page_serves_receiver(eq_stub) && page_serves_receiver(mseb_stub));
	assert(!page_serves_receiver(other) && !page_serves_receiver(NULL));
	lv_display_delete(display);
	peq_stub = eq_stub = mseb_stub = NULL;
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
	test_receiver_card_text();
	test_receiver_card();
	test_radios_held_by_receiver();
	test_receiver_session();
	test_open_equaliser_pages_over_receiver();
	test_open_other_pages_over_receiver();
	test_leave_receiver_then();

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
