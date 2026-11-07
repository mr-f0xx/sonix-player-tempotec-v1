#include "stockkeys_ui.h"

#include <stdio.h>
#include <string.h>

#include "src/gui/board_profile.h"
#include "src/gui/fonts/fonts.h"
#include "src/gui/nowplaying/player.h"
#include "src/gui/shell/gui.h"
#include "src/gui/shell/settingsrow.h"
#include "src/gui/shell/switcher.h"
#include "src/gui/shell/theme.h"
#include "src/gui/shell/toast.h"
#include "src/gui/wireless/wifisettings.h"
#include "src/system/addons/addons.h"
#include "src/system/core/lang.h"
#include "src/system/device/power.h"
#include "src/system/device/system.h"
#include "src/system/net/wifi.h"
#include "src/system/streaming/qobuz.h"
#include "src/system/streaming/stockkeys.h"
#include "src/system/streaming/streamkeys.h"
#include "src/system/streaming/tidal.h"

#define CARD_W 420
#define BUTTON_H 64
#define COMPACT_BUTTON_H 40
#define POLL_MS 150

static lv_obj_t *veil;
static lv_obj_t *card;
static lv_obj_t *title_label;
static lv_obj_t *message_label;
static lv_obj_t *bar;
static lv_obj_t *cancel_btn;
static lv_obj_t *cancel_label;
static lv_timer_t *poll_timer;
static bool holding_screen;
static void (*ready_cb)(void);

static bool wifi_ready(void) {
#ifdef HOST_BUILD
	return true;
#else
	wifi_status_t st;
	wifi_get_status(&st);
	return st.state == WIFI_STATE_CONNECTED && st.ip[0] != '\0';
#endif
}

static void veil_cb(lv_event_t *e) { (void)e; }

static void cancel_cb(lv_event_t *e) {
	(void)e;
	stockkeys_cancel();
}

static void ensure_modal(void) {
	if (veil) {
		return;
	}

	int32_t screen_w = lv_display_get_horizontal_resolution(lv_display_get_default());
	int32_t margin = bp_pick(28, 40);
	int32_t target_w = CARD_W < screen_w - margin ? CARD_W : screen_w - margin;

	veil = lv_obj_create(lv_layer_top());
	lv_obj_set_size(veil, lv_pct(100), lv_pct(100));
	lv_obj_set_pos(veil, 0, 0);
	lv_obj_set_style_bg_color(veil, lv_color_black(), 0);
	lv_obj_set_style_bg_opa(veil, LV_OPA_60, 0);
	lv_obj_set_style_border_width(veil, 0, 0);
	lv_obj_set_style_radius(veil, 0, 0);
	lv_obj_set_style_pad_all(veil, 0, 0);
	lv_obj_set_scrollable(veil, false);
	lv_obj_set_hidden(veil, true);
	lv_obj_add_event_cb(veil, veil_cb, LV_EVENT_CLICKED, NULL);

	card = lv_obj_create(veil);
	lv_obj_set_width(card, target_w);
	lv_obj_set_height(card, LV_SIZE_CONTENT);
	lv_obj_center(card);
	lv_obj_add_style(card, &theme_style_card, 0);
	lv_obj_set_style_radius(card, bp_pick(14, 18), 0);
	lv_obj_set_style_border_width(card, 0, 0);
	lv_obj_set_style_shadow_width(card, 0, 0);
	lv_obj_set_style_pad_all(card, bp_pick(12, 22), 0);
	lv_obj_set_style_pad_gap(card, bp_pick(8, 14), 0);
	lv_obj_set_scrollable(card, false);
	lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(card, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

	title_label = lv_label_create(card);
	lv_label_set_long_mode(title_label, LV_LABEL_LONG_WRAP);
	lv_obj_set_width(title_label, lv_pct(100));
	lv_obj_add_style(title_label, &theme_style_text, 0);
	lv_obj_set_style_text_font(title_label, bp_is_tempotec_v1() ? &font_ui_22 : &font_ui_26, 0);
	lv_obj_set_style_text_align(title_label, LV_TEXT_ALIGN_CENTER, 0);

	message_label = lv_label_create(card);
	lv_label_set_long_mode(message_label, LV_LABEL_LONG_WRAP);
	lv_obj_set_width(message_label, lv_pct(100));
	lv_obj_add_style(message_label, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(message_label, bp_is_tempotec_v1() ? &font_ui_18 : &font_ui_22, 0);
	lv_obj_set_style_text_align(message_label, LV_TEXT_ALIGN_CENTER, 0);

	bar = lv_bar_create(card);
	lv_obj_set_width(bar, lv_pct(100));
	lv_obj_set_height(bar, bp_pick(8, 12));
	lv_bar_set_range(bar, 0, 1000);
	lv_obj_set_style_radius(bar, LV_RADIUS_CIRCLE, LV_PART_MAIN);
	lv_obj_set_style_bg_opa(bar, LV_OPA_40, LV_PART_MAIN);
	lv_obj_set_style_radius(bar, LV_RADIUS_CIRCLE, LV_PART_INDICATOR);

	cancel_btn = lv_btn_create(card);
	lv_obj_set_width(cancel_btn, lv_pct(100));
	lv_obj_set_height(cancel_btn, bp_pick(COMPACT_BUTTON_H, BUTTON_H));
	lv_obj_set_style_radius(cancel_btn, bp_pick(bp_tile_radius(), 12), 0);
	lv_obj_set_style_shadow_width(cancel_btn, 0, 0);
	lv_obj_add_style(cancel_btn, &theme_style_switch, 0);
	lv_obj_add_event_cb(cancel_btn, cancel_cb, LV_EVENT_CLICKED, NULL);

	cancel_label = lv_label_create(cancel_btn);
	lv_obj_set_style_text_font(cancel_label, bp_is_tempotec_v1() ? &font_ui_20 : &font_ui_24, 0);
	lv_obj_center(cancel_label);
}

static void refresh_modal_theme_and_text(void) {
	lv_label_set_text(title_label, tr("setup_from_stock_firmware"));
	lv_label_set_text(cancel_label, tr("cancel"));
	lv_obj_set_style_text_color(cancel_label, theme()->text_primary, 0);
	lv_obj_set_style_bg_color(bar, theme()->text_secondary, LV_PART_MAIN);
	lv_obj_set_style_bg_color(bar, theme()->accent, LV_PART_INDICATOR);
}

static void update_running_status(void) {
	stockkeys_phase_t ph = stockkeys_phase();
	long done = 0, total = 0;
	stockkeys_progress(&done, &total);

	char line[192];
	switch (ph) {
	case STOCKKEYS_PHASE_SCANNING:
		snprintf(line, sizeof(line), "%s", tr("stock_keys_scanning_sd"));
		lv_obj_set_hidden(bar, true);
		break;
	case STOCKKEYS_PHASE_DOWNLOADING:
		if (total > 0) {
			int permil = (int)((long long)done * 1000LL / (long long)total);
			if (permil < 0) {
				permil = 0;
			}
			if (permil > 1000) {
				permil = 1000;
			}
			lv_bar_set_value(bar, permil, LV_ANIM_OFF);
			lv_obj_set_hidden(bar, false);
			snprintf(line, sizeof(line), "%s\n%.1f / %.1f MB", tr("stock_keys_downloading"),
					 (double)done / (1024.0 * 1024.0), (double)total / (1024.0 * 1024.0));
		} else if (done > 0) {
			lv_obj_set_hidden(bar, true);
			snprintf(line, sizeof(line), "%s\n%.1f MB", tr("stock_keys_downloading"),
					 (double)done / (1024.0 * 1024.0));
		} else {
			lv_obj_set_hidden(bar, true);
			snprintf(line, sizeof(line), "%s", tr("stock_keys_downloading"));
		}
		break;
	case STOCKKEYS_PHASE_EXTRACTING:
	default:
		lv_bar_set_value(bar, 1000, LV_ANIM_OFF);
		lv_obj_set_hidden(bar, false);
		snprintf(line, sizeof(line), "%s", tr("stock_keys_extracting"));
		break;
	}
	lv_label_set_text(message_label, line);
}

static void close_modal(void) {
	if (poll_timer) {
		lv_timer_pause(poll_timer);
	}
	if (veil) {
		lv_obj_set_hidden(veil, true);
	}
	if (holding_screen) {
		holding_screen = false;
		power_hold_screen_on(false);
	}
}

static void report_failure(stockkeys_result_t res, const char *detail) {
	char msg[256];
	switch (res) {
	case STOCKKEYS_NO_SOURCE_OFFLINE:
		snprintf(msg, sizeof(msg), "%s", tr("stock_keys_need_wifi_or_sd"));
		break;
	case STOCKKEYS_NO_SPACE:
		snprintf(msg, sizeof(msg), "%s", tr("stock_keys_no_space"));
		break;
	case STOCKKEYS_DOWNLOAD_FAILED:
		if (detail && detail[0]) {
			snprintf(msg, sizeof(msg), "%s (%s)", tr("stock_keys_download_failed"), detail);
		} else {
			snprintf(msg, sizeof(msg), "%s", tr("stock_keys_download_failed"));
		}
		break;
	case STOCKKEYS_SAVE_FAILED:
		snprintf(msg, sizeof(msg), "%s", tr("stock_keys_save_failed"));
		break;
	case STOCKKEYS_EXTRACT_FAILED:
	default:
		snprintf(msg, sizeof(msg), "%s", tr("stock_keys_extract_failed"));
		break;
	}
	gui_notify_popup(msg);
}

static void poll_cb(lv_timer_t *t) {
	(void)t;
	stockkeys_state_t st = stockkeys_state();
	if (st == STOCKKEYS_RUNNING) {
		update_running_status();
		return;
	}
	if (st != STOCKKEYS_FINISHED) {
		close_modal();
		return;
	}

	stockkeys_result_t res = stockkeys_result();
	char detail[160];
	snprintf(detail, sizeof(detail), "%s", stockkeys_error_detail());
	stockkeys_acknowledge();
	close_modal();

	if (res == STOCKKEYS_CANCELLED) {
		return;
	}
	if (res == STOCKKEYS_OK) {
		(void)streamkeys_reload(storage_sd_root());
		if (addons_enabled("qobuz")) {
			qobuz_init();
		}
		if (addons_enabled("tidal")) {
			tidal_init();
		}
		if (ready_cb) {
			ready_cb();
		}
		toast_success("stock_keys_ready");
		return;
	}
	report_failure(res, detail);
}

static void open_wifi_cb(lv_event_t *e) {
	(void)e;
	if (switcher_back_drag_active() || player_sheet_drag_active()) {
		return;
	}
	switch_screen(wifisettings_screen);
}

static void start_setup_cb(lv_event_t *e) {
	(void)e;
	if (switcher_back_drag_active() || player_sheet_drag_active()) {
		return;
	}

	if (!wifi_ready() && !stockkeys_has_sd_candidate(storage_sd_root())) {
		gui_notify_popup(tr("stock_keys_need_wifi_or_sd"));
		return;
	}

	ensure_modal();
	refresh_modal_theme_and_text();

	if (stockkeys_state() != STOCKKEYS_RUNNING) {
		if (!stockkeys_start()) {
			return;
		}
	}

	update_running_status();
	lv_obj_set_hidden(veil, false);
	lv_obj_move_foreground(veil);
	if (!holding_screen) {
		holding_screen = true;
		power_hold_screen_on(true);
	}
	if (!poll_timer) {
		poll_timer = lv_timer_create(poll_cb, POLL_MS, NULL);
	} else {
		lv_timer_resume(poll_timer);
	}
}

void stockkeys_ui_populate_missing(lv_obj_t *home_container, void (*on_ready)(void)) {
	ready_cb = on_ready;

	lv_obj_t *note = lv_label_create(home_container);
	lv_label_set_long_mode(note, LV_LABEL_LONG_WRAP);
	lv_obj_set_width(note, lv_pct(100));
	lv_obj_add_style(note, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(note, bp_is_tempotec_v1() ? &font_ui_18 : &font_ui_20, 0);
	lv_label_set_text(note, tr("stock_keys_hint"));

	lv_obj_t *setup_row = settingsrow_action(home_container, "setup_from_stock_firmware", start_setup_cb, NULL);
	lv_obj_set_style_margin_top(setup_row, bp_pick(10, 14), 0);

	if (!wifi_ready() && !stockkeys_has_sd_candidate(storage_sd_root())) {
		settingsrow_add(home_container, "wi_fi_settings", NULL, open_wifi_cb, NULL);
	}
}
