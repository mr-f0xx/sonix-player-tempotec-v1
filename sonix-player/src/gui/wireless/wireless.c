#include "wireless.h"

#include <string.h>

#include "lvgl/lvgl.h"

#include "src/gui/audio/dacpage.h"
#include "src/gui/board_profile.h"
#include "src/gui/bluetooth/btsettings.h"
#include "src/gui/fonts/fonts.h"
#include "src/gui/nowplaying/player.h"
#include "src/gui/shell/gridpage.h"
#include "src/gui/shell/icons.h"
#include "src/gui/shell/settingsrow.h"
#include "src/gui/shell/switcher.h"
#include "src/gui/shell/theme.h"
#include "src/gui/wireless/airplay.h"
#include "src/gui/wireless/dlna.h"
#include "src/gui/wireless/sonixlink.h"
#include "src/gui/wireless/wifisettings.h"
#include "src/gui/wireless/wifitransfer.h"
#include "src/system/bluetooth/bluetooth.h"
#include "src/system/core/lang.h"
#include "src/system/net/wifi.h"

lv_obj_t *wireless_screen;

#define WIRELESS_ACTION_COUNT 4

typedef struct {
	bool wifi;
	lv_obj_t *toggle;
	lv_obj_t *detail;
} radio_row_t;

typedef struct {
	lv_obj_t *icon;
	theme_semantic_t tone;
} wireless_icon_t;

static gui_config_t *wireless_cfg;
static lv_obj_t *wireless_grid;
static lv_obj_t *tokyo_wireless_panel;
static lv_obj_t *output_icon;
static lv_timer_t *wireless_timer;
static radio_row_t wifi_row;
static radio_row_t bluetooth_row;
static wireless_icon_t action_icons[WIRELESS_ACTION_COUNT];

static void radio_detail_refresh(radio_row_t *row) {
	if (!row || !row->detail || !row->toggle) {
		return;
	}

	bool available = row->wifi ? wifi_available() : bluetooth_available();
	bool enabled = row->wifi ? wifi_get_enabled() : bluetooth_get_enabled();
	if (enabled) {
		lv_obj_add_state(row->toggle, LV_STATE_CHECKED);
	} else {
		lv_obj_remove_state(row->toggle, LV_STATE_CHECKED);
	}
	if (available) {
		lv_obj_remove_state(row->toggle, LV_STATE_DISABLED);
	} else {
		lv_obj_add_state(row->toggle, LV_STATE_DISABLED);
	}

	const char *text;
	if (!available) {
		text = tr(row->wifi ? "wifi_no_hardware" : "bt_no_hardware");
	} else if (row->wifi && wifi_busy()) {
		text = tr(enabled ? "turning_on" : "wifi_turning_off_2");
	} else if (!row->wifi && bluetooth_busy()) {
		text = tr(enabled ? "turning_on" : "bt_turning_off");
	} else if (row->wifi && enabled) {
		wifi_status_t status;
		wifi_get_status(&status);
		text = status.state == WIFI_STATE_CONNECTED && status.ssid[0] ? status.ssid : tr("on");
	} else if (!row->wifi && enabled) {
		bt_device_t device;
		text = bluetooth_connected_device(&device) && device.name[0] ? device.name : tr("on");
	} else {
		text = tr("off");
	}
	lv_label_set_text(row->detail, text);
}

static void radios_refresh(void) {
	radio_detail_refresh(&wifi_row);
	radio_detail_refresh(&bluetooth_row);
}

static void radio_changed_cb(lv_event_t *e) {
	radio_row_t *row = lv_event_get_user_data(e);
	if (!row || !row->toggle) {
		return;
	}
	bool enabled = lv_obj_has_state(row->toggle, LV_STATE_CHECKED);
	if (row->wifi) {
		wifi_set_enabled(enabled);
	} else {
		bluetooth_set_enabled(enabled);
	}
	radios_refresh();
}

static lv_obj_t *wireless_card(lv_obj_t *parent, int height) {
	lv_obj_t *card = lv_obj_create(parent);
	lv_obj_set_width(card, lv_pct(100));
	lv_obj_set_height(card, height);
	lv_obj_add_style(card, &theme_style_card, 0);
	lv_obj_set_style_radius(card, bp_tile_radius(), 0);
	lv_obj_set_style_border_width(card, 0, 0);
	lv_obj_set_style_shadow_width(card, 0, 0);
	lv_obj_set_style_pad_all(card, 8, 0);
	lv_obj_set_scrollable(card, false);
	lv_obj_set_event_bubble(card, true);
	return card;
}

static lv_obj_t *radio_card(lv_obj_t *parent, const char *label, const lv_image_dsc_t *source,
							theme_semantic_t tone, lv_obj_t *target, radio_row_t *row) {
	lv_obj_t *card = wireless_card(parent, 62);
	lv_obj_t *open = lv_btn_create(card);
	lv_obj_remove_style_all(open);
	lv_obj_set_size(open, wireless_cfg->screen_width - 2 * wireless_cfg->padding - 16 - 48 - 8, 46);
	lv_obj_align(open, LV_ALIGN_LEFT_MID, 0, 0);
	lv_obj_set_style_bg_opa(open, LV_OPA_TRANSP, 0);
	lv_obj_set_style_border_width(open, 0, 0);
	lv_obj_set_style_shadow_width(open, 0, 0);
	lv_obj_set_style_pad_all(open, 0, 0);
	lv_obj_set_style_pad_column(open, 7, 0);
	lv_obj_set_flex_flow(open, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(open, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_event_bubble(open, true);
	lv_obj_add_event_cb(open, switch_screen_cb, LV_EVENT_CLICKED, target);

	lv_obj_t *icon = lv_image_create(open);
	lv_image_set_src(icon, source);
	lv_obj_add_style(icon, &theme_style_icon, 0);
	lv_obj_set_size(icon, 18, 18);
	lv_image_set_inner_align(icon, LV_IMAGE_ALIGN_CENTER);
	lv_image_set_scale(icon, (uint32_t)(LV_SCALE_NONE * 18 / LV_MAX((int)source->header.w, (int)source->header.h)));
	lv_obj_set_style_image_recolor(icon, theme_semantic_color(tone), 0);
	lv_obj_set_style_image_recolor_opa(icon, LV_OPA_COVER, 0);

	lv_obj_t *copy = lv_obj_create(open);
	lv_obj_remove_style_all(copy);
	lv_obj_set_size(copy, LV_MAX(1, wireless_cfg->screen_width - 2 * wireless_cfg->padding - 16 - 48 - 8 - 25),
					LV_SIZE_CONTENT);
	lv_obj_set_style_pad_all(copy, 0, 0);
	lv_obj_set_style_pad_row(copy, 1, 0);
	lv_obj_set_flex_flow(copy, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(copy, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
	lv_obj_set_scrollable(copy, false);

	lv_obj_t *title = lv_label_create(copy);
	lv_label_set_text(title, tr(label));
	lv_label_set_long_mode(title, LV_LABEL_LONG_DOT);
	lv_obj_set_width(title, lv_pct(100));
	lv_obj_set_height(title, lv_font_get_line_height(&font_ui_16));
	lv_obj_add_style(title, &theme_style_text, 0);
	lv_obj_set_style_text_font(title, &font_ui_16, 0);

	row->wifi = strcmp(label, "wi_fi") == 0;
	row->detail = lv_label_create(copy);
	lv_label_set_long_mode(row->detail, LV_LABEL_LONG_DOT);
	lv_obj_set_width(row->detail, lv_pct(100));
	lv_obj_set_height(row->detail, lv_font_get_line_height(&font_ui_14));
	lv_obj_add_style(row->detail, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(row->detail, &font_ui_14, 0);

	row->toggle = lv_switch_create(card);
	lv_obj_set_size(row->toggle, 48, 28);
	lv_obj_align(row->toggle, LV_ALIGN_RIGHT_MID, 0, 0);
	lv_obj_add_style(row->toggle, &theme_style_switch, LV_PART_MAIN);
	lv_obj_add_style(row->toggle, &theme_style_switch_checked, LV_PART_INDICATOR | LV_STATE_CHECKED);
	lv_obj_add_event_cb(row->toggle, radio_changed_cb, LV_EVENT_VALUE_CHANGED, row);

	radios_refresh();
	return card;
}

static lv_obj_t *output_card(lv_obj_t *parent) {
	lv_obj_t *card = wireless_card(parent, 59);
	lv_obj_set_clickable(card, true);
	lv_obj_add_style(card, &theme_style_card_pressed, LV_STATE_PRESSED);
	lv_obj_add_event_cb(card, switch_screen_cb, LV_EVENT_CLICKED, dacpage_screen);
	lv_obj_t *icon = lv_image_create(card);
	output_icon = icon;
	lv_image_set_src(icon, &icon_headphones);
	lv_obj_add_style(icon, &theme_style_icon, 0);
	lv_obj_set_size(icon, 19, 19);
	lv_image_set_scale(icon, (uint32_t)(LV_SCALE_NONE * 19 / 26));
	lv_image_set_inner_align(icon, LV_IMAGE_ALIGN_CENTER);
	lv_obj_set_style_image_recolor(icon, theme_semantic_color(THEME_SEMANTIC_PURPLE), 0);
	lv_obj_set_style_image_recolor_opa(icon, LV_OPA_COVER, 0);
	lv_obj_align(icon, LV_ALIGN_LEFT_MID, 0, 0);

	lv_obj_t *title = lv_label_create(card);
	lv_label_set_text(title, tr("wireless_audio_output"));
	lv_obj_add_style(title, &theme_style_text, 0);
	lv_obj_set_style_text_font(title, &font_ui_16, 0);
	lv_obj_align(title, LV_ALIGN_TOP_LEFT, 28, 3);

	lv_obj_t *detail = lv_label_create(card);
	lv_label_set_text(detail, tr("wireless_headphone_output"));
	lv_label_set_long_mode(detail, LV_LABEL_LONG_DOT);
	lv_obj_set_width(detail, wireless_cfg->screen_width - 2 * wireless_cfg->padding - 16 - 28 - 14 - 8);
	lv_obj_add_style(detail, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(detail, &font_ui_14, 0);
	lv_obj_align(detail, LV_ALIGN_BOTTOM_LEFT, 28, -3);

	lv_obj_t *chevron = lv_image_create(card);
	lv_image_set_src(chevron, &icon_chevron_right);
	lv_obj_add_style(chevron, &theme_style_icon, 0);
	lv_obj_set_size(chevron, 14, 14);
	lv_image_set_scale(chevron, (uint32_t)(LV_SCALE_NONE * 14 / 36));
	lv_image_set_inner_align(chevron, LV_IMAGE_ALIGN_CENTER);
	lv_obj_set_style_image_opa(chevron, LV_OPA_60, 0);
	lv_obj_align(chevron, LV_ALIGN_RIGHT_MID, 0, 0);
	return card;
}

static lv_obj_t *wireless_action_list(lv_obj_t *parent, const grid_entry_t *entries) {
	lv_obj_t *card = wireless_card(parent, 4 * 40 + 8);
	lv_obj_set_style_pad_all(card, 4, 0);
	lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(card, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
	for (int i = 0; i < WIRELESS_ACTION_COUNT; i++) {
		const grid_entry_t *entry = &entries[i + 2];
		lv_obj_t *button = lv_btn_create(card);
		lv_obj_set_size(button, lv_pct(100), 40);
		lv_obj_set_style_bg_opa(button, LV_OPA_TRANSP, 0);
		lv_obj_add_style(button, &theme_style_card_pressed, LV_STATE_PRESSED);
		lv_obj_set_style_border_width(button, 0, 0);
		if (i + 1 < WIRELESS_ACTION_COUNT) {
			lv_obj_set_style_border_side(button, LV_BORDER_SIDE_BOTTOM, 0);
			lv_obj_set_style_border_width(button, 1, 0);
			lv_obj_set_style_border_color(button, theme()->text_secondary, 0);
			lv_obj_set_style_border_opa(button, LV_OPA_20, 0);
		}
		lv_obj_set_style_radius(button, 0, 0);
		lv_obj_set_style_pad_hor(button, 5, 0);
		lv_obj_set_style_pad_all(button, 5, 0);
		lv_obj_set_style_pad_column(button, 7, 0);
		lv_obj_set_event_bubble(button, true);
		lv_obj_set_flex_flow(button, LV_FLEX_FLOW_ROW);
		lv_obj_set_flex_align(button, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
		lv_obj_add_event_cb(button, switch_screen_cb, LV_EVENT_CLICKED, *entry->target);

		lv_obj_t *icon = lv_image_create(button);
		lv_image_set_src(icon, entry->icon);
		lv_obj_add_style(icon, &theme_style_icon, 0);
		lv_obj_set_size(icon, 18, 18);
		lv_image_set_inner_align(icon, LV_IMAGE_ALIGN_CENTER);
		int side = LV_MAX((int)entry->icon->header.w, (int)entry->icon->header.h);
		lv_image_set_scale(icon, side ? (uint32_t)(LV_SCALE_NONE * 18 / side) : LV_SCALE_NONE);
		lv_obj_set_style_image_recolor(icon, theme_semantic_color((theme_semantic_t)(i == 0 ? THEME_SEMANTIC_CYAN :
													 i == 1 ? THEME_SEMANTIC_GREEN : i == 2 ? THEME_SEMANTIC_AMBER : THEME_SEMANTIC_BLUE)), 0);
		lv_obj_set_style_image_recolor_opa(icon, LV_OPA_COVER, 0);
		action_icons[i] = (wireless_icon_t){.icon = icon,
										 .tone = i == 0 ? THEME_SEMANTIC_CYAN : i == 1 ? THEME_SEMANTIC_GREEN :
												 i == 2 ? THEME_SEMANTIC_AMBER : THEME_SEMANTIC_BLUE};

		lv_obj_t *label = lv_label_create(button);
		lv_label_set_text(label, tr(entry->label));
		lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
		lv_obj_set_flex_grow(label, 1);
		lv_obj_add_style(label, &theme_style_text, 0);
		lv_obj_set_style_text_font(label, &font_ui_16, 0);

		lv_obj_t *chevron = lv_image_create(button);
		lv_image_set_src(chevron, &icon_chevron_right);
		lv_obj_add_style(chevron, &theme_style_icon, 0);
		lv_obj_set_size(chevron, 14, 14);
		lv_image_set_scale(chevron, (uint32_t)(LV_SCALE_NONE * 14 / 36));
		lv_image_set_inner_align(chevron, LV_IMAGE_ALIGN_CENTER);
		lv_obj_set_style_image_opa(chevron, LV_OPA_60, 0);
	}
	return card;
}

static void tokyo_panel_build(gui_config_t *cfg, const grid_entry_t *entries) {
	if (!bp_is_tempotec_v1()) {
		return;
	}

	int top = settingsrow_content_top(cfg);
	tokyo_wireless_panel = lv_obj_create(wireless_screen);
	lv_obj_set_size(tokyo_wireless_panel, lv_pct(100), cfg->screen_height - top);
	lv_obj_align(tokyo_wireless_panel, LV_ALIGN_TOP_LEFT, 0, top);
	lv_obj_set_style_bg_opa(tokyo_wireless_panel, LV_OPA_TRANSP, 0);
	lv_obj_set_style_border_width(tokyo_wireless_panel, 0, 0);
	lv_obj_set_style_radius(tokyo_wireless_panel, 0, 0);
	lv_obj_set_style_pad_hor(tokyo_wireless_panel, cfg->padding, 0);
	lv_obj_set_style_pad_ver(tokyo_wireless_panel, cfg->padding, 0);
	lv_obj_set_style_pad_gap(tokyo_wireless_panel, 5, 0);
	lv_obj_set_flex_flow(tokyo_wireless_panel, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(tokyo_wireless_panel, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_scroll_dir(tokyo_wireless_panel, LV_DIR_VER);
	lv_obj_set_scrollbar_mode(tokyo_wireless_panel, LV_SCROLLBAR_MODE_AUTO);
	lv_obj_set_hidden(tokyo_wireless_panel, true);

	radio_card(tokyo_wireless_panel, "wi_fi", &icon_wifi, THEME_SEMANTIC_BLUE, wifisettings_screen, &wifi_row);
	radio_card(tokyo_wireless_panel, "bluetooth", &icon_bluetooth, THEME_SEMANTIC_PURPLE, btsettings_screen,
			   &bluetooth_row);
	output_card(tokyo_wireless_panel);
	wireless_action_list(tokyo_wireless_panel, entries);
	player_sheet_attach_drag(tokyo_wireless_panel, true);
	switcher_attach_back_gesture(tokyo_wireless_panel);
}

static void wireless_refresh_theme(void) {
	bool tokyo_v1 = bp_is_tempotec_v1() && theme_is_tokyo_night();
	if (wireless_grid) {
		lv_obj_set_hidden(wireless_grid, tokyo_v1);
	}
	if (tokyo_wireless_panel) {
		lv_obj_set_hidden(tokyo_wireless_panel, !tokyo_v1);
	}
	for (int i = 0; i < WIRELESS_ACTION_COUNT; i++) {
		if (action_icons[i].icon) {
			lv_obj_set_style_image_recolor(action_icons[i].icon, theme_semantic_color(action_icons[i].tone), 0);
			lv_obj_set_style_image_recolor_opa(action_icons[i].icon, LV_OPA_COVER, 0);
		}
	}
	if (output_icon) {
		lv_obj_set_style_image_recolor(output_icon, theme_semantic_color(THEME_SEMANTIC_PURPLE), 0);
		lv_obj_set_style_image_recolor_opa(output_icon, LV_OPA_COVER, 0);
	}
	radios_refresh();
}

static void wireless_screen_loaded_cb(lv_event_t *e) {
	(void)e;
	wireless_refresh_theme();
	if (tokyo_wireless_panel) {
		radios_refresh();
	}
}

static void wireless_timer_cb(lv_timer_t *timer) {
	(void)timer;
	if (lv_screen_active() == wireless_screen) {
		radios_refresh();
	}
}

void wireless_init(gui_config_t *cfg) {
	wireless_cfg = cfg;
	const grid_entry_t entries[] = {
		{"wi_fi", &icon_menu_wifi_settings, &wifisettings_screen, NULL},
		{"bluetooth", &icon_menu_bluetooth, &btsettings_screen, NULL},
		{"airplay", &icon_menu_airplay, &airplay_screen, NULL},
		{"transfer", &icon_menu_wifi_transfer, &wifitransfer_screen, NULL},
		{"sonixlink", &icon_menu_sonixlink, &sonixlink_screen, NULL},
		{"dlna", &icon_menu_dlna, &dlna_screen, NULL},
	};

	// Dark and Light keep their established six-tile page. Tokyo Night on the
	// V1 adds live Wi-Fi/Bluetooth state and keeps every service destination.
	wireless_grid = gridpage_build(wireless_screen, cfg, entries, (int)(sizeof(entries) / sizeof(entries[0])), 2, 3, true);
	settingsrow_title(wireless_screen, cfg, "wireless");
	tokyo_panel_build(cfg, entries);
	wireless_refresh_theme();
	theme_register_refresh(wireless_refresh_theme);
	lv_obj_add_event_cb(wireless_screen, wireless_screen_loaded_cb, LV_EVENT_SCREEN_LOADED, NULL);
	wireless_timer = lv_timer_create(wireless_timer_cb, 1000, NULL);
}
