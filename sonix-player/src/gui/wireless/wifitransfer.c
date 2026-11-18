#include "wifitransfer.h"

#include <stdio.h>
#include <string.h>

#include "src/gui/fonts/fonts.h"
#include "src/gui/nowplaying/player.h"
#include "src/gui/shell/confirm.h"
#include "src/gui/shell/settingsrow.h"
#include "src/gui/shell/switcher.h"
#include "src/gui/shell/theme.h"
#include "src/gui/wireless/wifisettings.h"
#include "src/system/audio/audio.h"
#include "src/system/core/lang.h"
#include "src/system/device/led.h"
#include "src/system/device/system.h"
#include "src/system/library/library.h"
#include "src/system/net/wifi.h"
#include "src/system/net/wifitransfer.h"
#include "src/system/streaming/radio.h"
#include "src/system/streaming/streamturn.h"

lv_obj_t *wifitransfer_screen;

#define WT_POLL_MS 500
#define WT_LINK_GRACE_TICKS 6 // tolerate a brief scan, but stop at once when switched off
#define QR_SIZE 88  // the numeric URL fits a 29-module QR at 3 px per module
#define QR_QUIET 12 // four modules of white, plus the bitmap's own margin

static lv_obj_t *toggle, *status_label, *url_label, *name_label, *code_label;
static lv_obj_t *qr_box, *qr, *trusted_label, *keep_open_label, *wifi_row;
static lv_timer_t *poll_timer;
static bool page_active, start_on_load, leaving;
static int link_missing_ticks;
static char qr_payload[WIFITRANSFER_URL_SIZE + 20];
static const char *last_error;

static void set_text(lv_obj_t *label, const char *text) {
	if (strcmp(lv_label_get_text(label), text) != 0) lv_label_set_text(label, text);
}

static void set_labels_hidden(bool hide) {
	lv_obj_set_hidden(url_label, hide);
	lv_obj_set_hidden(code_label, hide);
	lv_obj_set_hidden(trusted_label, hide);
	lv_obj_set_hidden(keep_open_label, hide);
	if (hide) {
		lv_obj_set_hidden(qr_box, true);
		lv_obj_set_hidden(name_label, true);
		qr_payload[0] = '\0';
	}
}

static void show_session(const wifitransfer_info_t *info) {
	// Encode the *numeric* URL, not .local, which not every phone resolves.
	// The fragment pairs the browser but is never sent as an HTTP URL.
	char payload[sizeof(qr_payload)];
	snprintf(payload, sizeof(payload), "%s/#%s", info->address, info->code);
	if (strcmp(payload, qr_payload) != 0) {
		bool valid = lv_qrcode_update(qr, payload, (uint32_t)strlen(payload)) == LV_RESULT_OK;
		lv_obj_set_hidden(qr_box, !valid);
		snprintf(qr_payload, sizeof(qr_payload), "%s", payload);
	}
	set_text(status_label, tr(lv_obj_is_hidden(qr_box) ? "wifitransfer_address_note" : "wifitransfer_scan_note"));
	set_text(url_label, info->address);
	char code[96], name[WIFITRANSFER_URL_SIZE + 32];
	const char *format = tr("wifitransfer_pairing_code");
	snprintf(code, sizeof(code), strcmp(format, "wifitransfer_pairing_code") ? format : "Code: %s", info->code);
	set_text(code_label, code);
	if (info->name[0]) {
		snprintf(name, sizeof(name), tr("wifitransfer_or"), info->name);
		set_text(name_label, name);
	}
	lv_obj_set_hidden(name_label, !info->name[0]);
	set_text(trusted_label, tr("wifitransfer_trusted_note"));
	set_text(keep_open_label, tr("wifitransfer_keep_open_note"));
	set_labels_hidden(false);
}

static bool read_link(char ip[16]) {
	wifi_status_t status;
	wifi_get_status(&status);
	bool have = status.state != WIFI_STATE_OFF && wifi_interface_address(ip, 16);
	if (have) link_missing_ticks = 0;
	else if (status.state == WIFI_STATE_OFF || !page_active) link_missing_ticks = WT_LINK_GRACE_TICKS;
	else if (link_missing_ticks < WT_LINK_GRACE_TICKS) link_missing_ticks++;
	return have;
}

static void stop_transfer(void) {
	lv_obj_remove_state(toggle, LV_STATE_CHECKED);
	if (!page_active) return;
	page_active = false;
	// Join the worker before rechecking databases or scanning the card. In
	// particular, an in-flight upload needs to remove its part file first.
	wifitransfer_release_card();
	led_set_wifi_transfer(false);
	wifi_set_status_poll_slow(false);
	if (gui_card_available()) {
		storage_card_files_recheck();
		library_card_returned(storage_sd_root());
	}
}

static void refresh(void) {
	char ip[16];
	bool have = read_link(ip);
	bool connected = link_missing_ticks < WT_LINK_GRACE_TICKS;
	bool available = wifitransfer_available();
	wifitransfer_info_t session;
	if (page_active) {
		bool live = wifitransfer_get_info(&session);
		bool moved = live && have && strcmp(ip, session.ip) != 0;
		if (!live || !connected || moved) {
			stop_transfer();
			last_error = moved ? "wifitransfer_link_changed" : NULL;
		}
	}

	lv_obj_set_hidden(wifi_row, !available || connected);
	if (available && connected) lv_obj_remove_state(toggle, LV_STATE_DISABLED);
	else lv_obj_add_state(toggle, LV_STATE_DISABLED);
	if (page_active) {
		if (wifitransfer_take_upload_complete()) led_notify_upload_complete();
		show_session(&session);
		return;
	}
	set_labels_hidden(true);
	const char *message = !available ? "wifitransfer_unavailable" : !connected ? "enable_wi_fi_first" :
		 last_error ? last_error : "wifitransfer_off_note";
	set_text(status_label, tr(message));
}

// Keep the existing playback and streaming behavior of the transfer switch:
// pause local playback so it can resume; a live station has no position to save.
static void pause_playback(void) {
	if (radio_is_active()) {
		if (!radio_is_playing()) return;
		radio_stop();
		player_refresh_now_playing();
	} else if (audio_get_status() == AUDIO_STATUS_PLAYING) {
		player_key_play_pause();
	} else {
		return;
	}
	gui_notify_popup("wifitransfer_stops_playback");
}

static void start_transfer(void) {
	if (page_active) return;
	lv_color_t accent = theme()->accent;
	uint32_t rgb = ((uint32_t)accent.red << 16) | ((uint32_t)accent.green << 8) | accent.blue;
	wifitransfer_result_t result = wifitransfer_start(rgb);
	last_error = NULL;
	if (result != WIFITRANSFER_OK) {
		lv_obj_remove_state(toggle, LV_STATE_CHECKED);
		last_error = result == WIFITRANSFER_NO_WIFI ? "enable_wi_fi_first" :
			result == WIFITRANSFER_NO_CARD ? "wifitransfer_card_note" : "wifitransfer_start_failed";
		return;
	}
	page_active = true;
	lv_obj_add_state(toggle, LV_STATE_CHECKED);
	led_set_wifi_transfer(true);
	wifi_set_status_poll_slow(true);
	pause_playback();
	streamturn_abandon_all();
}

static void toggle_changed_cb(lv_event_t *e) {
	(void)e;
	if (lv_obj_has_state(toggle, LV_STATE_CHECKED)) start_transfer();
	else stop_transfer();
	refresh();
}

static void leave_async(void *user) {
	(void)user;
	back_btn_cb(NULL);
	leaving = false;
}

static void confirm_leave_cb(void *user) {
	(void)user;
	leaving = true;
	wifitransfer_page_stop();
	// Navigate outside the confirmation dialog's close handler.
	lv_async_call(leave_async, NULL);
}

static bool back_guard(void) {
	if (leaving || !page_active) return false;
	confirm_show("wifitransfer_leave_transfer", "wifitransfer_off_confirm_note", "wifitransfer_turn_off",
				 confirm_leave_cb, NULL);
	return true;
}

static void poll_cb(lv_timer_t *timer) {
	(void)timer;
	refresh();
}

static void screen_loaded_cb(lv_event_t *e) {
	(void)e;
	leaving = false;
	last_error = NULL;
	if (start_on_load) {
		start_on_load = false;
		start_transfer();
	}
	refresh();
	lv_timer_resume(poll_timer);
}

static void screen_unload_cb(lv_event_t *e) {
	(void)e;
	lv_timer_pause(poll_timer);
	stop_transfer();
	set_labels_hidden(true);
}

void wifitransfer_page_open_started(void) {
	if (!wifitransfer_screen) return;
	start_on_load = true;
	if (lv_screen_active() == wifitransfer_screen) {
		lv_obj_send_event(wifitransfer_screen, LV_EVENT_SCREEN_LOADED, NULL);
	} else {
		switch_screen(wifitransfer_screen);
	}
}

void wifitransfer_page_stop(void) {
	if (!toggle) return;
	stop_transfer();
	refresh();
}

static void wifi_row_cb(lv_event_t *e) {
	(void)e;
	switch_screen(wifisettings_screen);
}

static void refresh_accent(void) {
	lv_obj_set_style_text_color(url_label, theme()->accent, 0);
}

static lv_obj_t *label(lv_obj_t *container, lv_style_t *style, const lv_font_t *font) {
	lv_obj_t *obj = lv_label_create(container);
	lv_obj_set_width(obj, lv_pct(100));
	lv_label_set_long_mode(obj, LV_LABEL_LONG_WRAP);
	lv_obj_add_style(obj, style, 0);
	lv_obj_set_style_text_font(obj, font, 0);
	lv_obj_set_style_text_align(obj, LV_TEXT_ALIGN_CENTER, 0);
	return obj;
}

void wifitransfer_page_init(gui_config_t *cfg) {
	lv_obj_t *container = settingsrow_page(wifitransfer_screen, cfg, "transfer");
	// Keep both the QR and the manual pairing details above the fold.
	lv_obj_set_style_pad_row(container, 2, 0);
	settingsrow_toggle(container, "wifitransfer_title", &toggle, toggle_changed_cb);
	status_label = label(container, &theme_style_text, &font_ui_14);

	qr_box = lv_obj_create(container);
	lv_obj_remove_style_all(qr_box);
	lv_obj_set_size(qr_box, QR_SIZE + 2 * QR_QUIET, QR_SIZE + 2 * QR_QUIET);
	lv_obj_set_scrollable(qr_box, false);
	lv_obj_set_style_bg_color(qr_box, lv_color_white(), 0);
	lv_obj_set_style_bg_opa(qr_box, LV_OPA_COVER, 0);
	qr = lv_qrcode_create(qr_box);
	lv_qrcode_set_size(qr, QR_SIZE);
	lv_qrcode_set_dark_color(qr, lv_color_black());
	lv_qrcode_set_light_color(qr, lv_color_white());
	lv_obj_center(qr);

	url_label = label(container, &theme_style_text, &font_ui_16);
	code_label = label(container, &theme_style_text, &font_ui_16);
	name_label = label(container, &theme_style_text_dim, &font_ui_14);
	trusted_label = label(container, &theme_style_text_dim, &font_ui_14);
	keep_open_label = label(container, &theme_style_text_dim, &font_ui_14);
	wifi_row = settingsrow_add(container, "wi_fi_settings", NULL, wifi_row_cb, NULL);

	poll_timer = lv_timer_create(poll_cb, WT_POLL_MS, NULL);
	lv_timer_pause(poll_timer);
	lv_obj_add_event_cb(wifitransfer_screen, screen_loaded_cb, LV_EVENT_SCREEN_LOADED, NULL);
	lv_obj_add_event_cb(wifitransfer_screen, screen_unload_cb, LV_EVENT_SCREEN_UNLOAD_START, NULL);
	switcher_set_back_guard(wifitransfer_screen, back_guard);
	theme_register_refresh(refresh_accent);
	refresh_accent();
	refresh();
}
