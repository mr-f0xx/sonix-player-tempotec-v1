// The real transfer page and LVGL QR canvas on a 240x320 display. The
// service is stubbed here; test_wifitransfer_server exercises real sockets.
#include <assert.h>
#include <stdlib.h>
#include <unistd.h>

#include "src/gui/board_profile.h"
#include "src/system/core/config.h"
#include "../../src/gui/wireless/wifitransfer.c"

lv_font_t font_ui_14, font_ui_16, font_ui_18, font_ui_20, font_ui_20_bold;
lv_font_t font_ui_22, font_ui_22_bold, font_ui_24, font_ui_24_bold;
lv_font_t font_ui_26, font_ui_28, font_ui_32, font_ui_36_bold;
lv_obj_t *wifisettings_screen;
static bool hardware = true, mounted = true, listener, led, slow_poll;
static wifi_state_t wifi = WIFI_STATE_OFF;
static const char *ip_address = "192.168.100.250";
static unsigned refreshes, abandon_calls, upload_notices;
static bool upload_complete;
static bool (*guard)(void);
static void (*confirmed)(void *);

void fonts_register_change(void (*cb)(void)) { (void)cb; }
void bootlogo_set(int choice, bool dark) { (void)choice; (void)dark; }
bool bootpic_present(void) { return false; }
void switcher_attach_back_gesture(lv_obj_t *obj) { (void)obj; }
bool switcher_back_drag_active(void) { return false; }
bool player_sheet_drag_active(void) { return false; }
void switcher_set_back_guard(lv_obj_t *screen, bool (*handler)(void)) {
	assert(screen == wifitransfer_screen);
	guard = handler;
}
void switch_screen(lv_obj_t *screen) { lv_screen_load(screen); }
void back_btn_cb(lv_event_t *e) { (void)e; switch_screen(wifisettings_screen); }
void confirm_show(const char *title, const char *message, const char *ok, void (*callback)(void *), void *data) {
	assert(title && message && ok && !data);
	confirmed = callback;
}

bool wifitransfer_available(void) { return hardware; }
bool wifitransfer_get_enabled(void) { return listener; }
wifitransfer_result_t wifitransfer_start(uint32_t accent) {
	assert(accent && hardware);
	if (wifi == WIFI_STATE_OFF || !ip_address) return WIFITRANSFER_NO_WIFI;
	if (!mounted) return WIFITRANSFER_NO_CARD;
	listener = true;
	return WIFITRANSFER_OK;
}
bool wifitransfer_get_info(wifitransfer_info_t *out) {
	if (!listener) return false;
	*out = (wifitransfer_info_t){.ip = "192.168.100.250", .address = "http://192.168.100.250:4399",
								.name = "http://sonix-transfer.local:4399", .code = "W7X9ABCD"};
	return true;
}
bool wifitransfer_take_upload_complete(void) {
	assert(listener);
	bool ready = upload_complete;
	upload_complete = false;
	return ready;
}
void wifitransfer_release_card(void) { listener = upload_complete = false; }
void wifi_get_status(wifi_status_t *out) { *out = (wifi_status_t){.state = wifi}; }
bool wifi_interface_address(char *out, size_t size) {
	if (!ip_address) return false;
	snprintf(out, size, "%s", ip_address);
	return true;
}
void wifi_set_status_poll_slow(bool slow) { slow_poll = slow; }
void led_set_wifi_transfer(bool on) { led = on; }
void led_notify_upload_complete(void) { upload_notices++; }
bool gui_card_available(void) { return mounted; }
const char *storage_sd_root(void) { return "/test-card"; }
void storage_card_files_recheck(void) { assert(!listener); }
bool library_card_returned(const char *root) {
	assert(!listener && root == storage_sd_root());
	refreshes++;
	return true;
}
void streamturn_abandon_all(void) { abandon_calls++; }
audio_status_t audio_get_status(void) { return AUDIO_STATUS_STOPPED; }
bool radio_is_active(void) { return false; }
bool radio_is_playing(void) { return false; }
void radio_stop(void) { assert(!"no radio in this test"); }
void player_refresh_now_playing(void) {}
void player_key_play_pause(void) { assert(!"no playback in this test"); }
void gui_notify_popup(const char *text) { (void)text; assert(!"unexpected popup"); }

static void flip(bool on) {
	if (on) lv_obj_add_state(toggle, LV_STATE_CHECKED);
	else lv_obj_remove_state(toggle, LV_STATE_CHECKED);
	lv_obj_send_event(toggle, LV_EVENT_VALUE_CHANGED, NULL);
}

static void flush(lv_display_t *disp, const lv_area_t *area, uint8_t *pixels) {
	(void)area;
	(void)pixels;
	lv_display_flush_ready(disp);
}

static void on_glass(lv_obj_t *obj, const gui_config_t *cfg) {
	assert(!lv_obj_is_hidden(obj));
	lv_area_t area;
	lv_obj_get_coords(obj, &area);
	if (area.y2 >= (int)cfg->screen_height) fprintf(stderr, "Object runs below the panel: %d..%d\n", area.y1, area.y2);
	assert(area.x1 >= 0 && area.x2 < (int)cfg->screen_width);
	assert(area.y1 >= 0 && area.y2 < (int)cfg->screen_height);
}

int main(int argc, char **argv) {
	assert(argc == 2);
	char dir[] = "/tmp/sonix-transfer-ui-XXXXXX", config[128];
	assert(mkdtemp(dir));
	snprintf(config, sizeof(config), "%s/device.ini", dir);
	config_init(config);
	lang_init(argv[1]);

	font_ui_14 = lv_font_montserrat_14;
	font_ui_16 = lv_font_montserrat_16;
	font_ui_18 = font_ui_20 = font_ui_20_bold = lv_font_montserrat_20;
	font_ui_22 = font_ui_22_bold = font_ui_24 = font_ui_24_bold = lv_font_montserrat_22;
	font_ui_26 = font_ui_28 = lv_font_montserrat_28;
	font_ui_32 = font_ui_36_bold = lv_font_montserrat_36;
	lv_init();
	gui_config_t cfg = {bp_screen_w(), bp_screen_h(), bp_padding(), bp_status_bar_h(), dir};
	lv_display_t *disp = lv_display_create(cfg.screen_width, cfg.screen_height);
	static uint16_t buffer[240 * 16];
	lv_display_set_color_format(disp, LV_COLOR_FORMAT_RGB565);
	lv_display_set_buffers(disp, buffer, NULL, sizeof(buffer), LV_DISPLAY_RENDER_MODE_PARTIAL);
	lv_display_set_flush_cb(disp, flush);
	theme_init();
	wifisettings_screen = lv_obj_create(NULL);
	wifitransfer_screen = lv_obj_create(NULL);
	wifitransfer_page_init(&cfg);
	lv_screen_load(wifitransfer_screen);
	assert(lv_obj_has_state(toggle, LV_STATE_DISABLED) && !lv_obj_is_hidden(wifi_row));
	assert(lv_obj_is_hidden(qr_box));

	wifi = WIFI_STATE_CONNECTED;
	poll_cb(NULL);
	assert(!lv_obj_has_state(toggle, LV_STATE_DISABLED) && lv_obj_is_hidden(wifi_row));
	mounted = false;
	flip(true);
	assert(!lv_obj_has_state(toggle, LV_STATE_CHECKED) && !listener);
	assert(strcmp(lv_label_get_text(status_label), tr("wifitransfer_card_note")) == 0);
	assert(lv_obj_is_hidden(qr_box) && lv_obj_is_hidden(code_label));

	mounted = true;
	flip(true);
	lv_obj_update_layout(wifitransfer_screen);
	assert(listener && led && slow_poll && abandon_calls == 1);
	assert(lv_qrcode_is_render_valid(qr));
	assert(strcmp(qr_payload, "http://192.168.100.250:4399/#W7X9ABCD") == 0);
	assert(strcmp(lv_label_get_text(url_label), "http://192.168.100.250:4399") == 0);
	assert(strcmp(lv_label_get_text(code_label), "Code: W7X9ABCD") == 0);
	on_glass(qr_box, &cfg);
	on_glass(qr, &cfg);
	on_glass(url_label, &cfg);
	on_glass(code_label, &cfg);
	assert(lv_obj_get_width(qr_box) - lv_obj_get_width(qr) == 2 * QR_QUIET);

	assert(upload_notices == 0);
	upload_complete = true;
	poll_cb(NULL);
	poll_cb(NULL);
	assert(upload_notices == 1 && listener && led);

	// A transient association poll does not unpair an otherwise live session.
	wifi = WIFI_STATE_ON;
	ip_address = NULL;
	for (int i = 1; i < WT_LINK_GRACE_TICKS; i++) poll_cb(NULL);
	assert(listener && !lv_obj_is_hidden(qr_box));
	poll_cb(NULL);
	assert(!listener && !led && !slow_poll && lv_obj_is_hidden(qr_box) && !qr_payload[0]);
	assert(refreshes == 1);

	wifi = WIFI_STATE_CONNECTED;
	ip_address = "192.168.100.250";
	wifitransfer_page_open_started();
	assert(listener);
	ip_address = "192.168.100.251";
	poll_cb(NULL);
	assert(!listener && lv_obj_is_hidden(code_label));
	assert(strcmp(lv_label_get_text(status_label), tr("wifitransfer_link_changed")) == 0);
	ip_address = "192.168.100.250";

	wifitransfer_page_open_started();
	assert(listener && guard && guard() && confirmed);
	assert(listener); // the prompt itself does not abort a transfer
	confirmed(NULL);
	assert(!listener && lv_obj_is_hidden(qr_box));
	lv_timer_handler();
	assert(lv_screen_active() == wifisettings_screen);

	wifitransfer_page_open_started();
	assert(listener && lv_screen_active() == wifitransfer_screen);
	switch_screen(wifisettings_screen); // unguarded screen changes stop it too
	assert(!listener && lv_obj_is_hidden(qr_box));
	assert(refreshes == 4);

	wifitransfer_page_open_started();
	wifi = WIFI_STATE_OFF;
	poll_cb(NULL);
	assert(!listener && lv_obj_is_hidden(qr_box)); // radio off beats the grace period
	assert(refreshes == 5);

	wifi = WIFI_STATE_CONNECTED;
	wifitransfer_page_open_started();
	assert(listener);
	mounted = false; // USB/hotplug owns the release, not the UI
	wifitransfer_release_card();
	poll_cb(NULL);
	assert(!lv_obj_has_state(toggle, LV_STATE_CHECKED) && lv_obj_is_hidden(qr_box) && !led && !slow_poll);
	assert(refreshes == 5); // don't reopen the library on an unavailable card
	lv_deinit();
	unlink(config);
	assert(rmdir(dir) == 0);
	puts("PASS Wi-Fi transfer page (240x320)");
	return 0;
}
