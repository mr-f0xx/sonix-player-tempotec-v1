#include "src/gui/wireless/listentogether.h"
#include "src/system/net/listentogether.h"
#include "src/gui/streaming/streaming.h"
#include "src/system/core/lang.h"

#include <stdio.h>
#include <string.h>

lv_obj_t *listentogether_screen = NULL;

static lv_obj_t *status_lbl = NULL;
static lv_obj_t *stats_lbl = NULL;
static lv_obj_t *host_btn_lbl = NULL;
static lv_obj_t *join_btn_lbl = NULL;
static lv_timer_t *lt_ui_timer = NULL;

static void update_ui(void) {
	if (!status_lbl || !stats_lbl) return;

	lt_stats_t st;
	lt_get_stats(&st);

	if (st.mode == LT_MODE_HOST) {
		lv_label_set_text_fmt(status_lbl, "Hosting (%d Hz) - %d peer(s)", st.active_rate, st.peer_count);
		lv_label_set_text_fmt(stats_lbl, "TX: %llu pkts | Multicast + Unicast",
							  (unsigned long long)st.packets_tx);
		if (host_btn_lbl) lv_label_set_text(host_btn_lbl, "Stop Hosting");
		if (join_btn_lbl) lv_label_set_text(join_btn_lbl, "Join Stream");
	} else if (st.mode == LT_MODE_CLIENT) {
		lv_label_set_text_fmt(status_lbl, "Listening (%s)",
							  st.peer_ip[0] ? st.peer_ip : "Multicast");
		lv_label_set_text_fmt(stats_lbl, "Buf: %d/%d ms | Drift: %+d ppm | Lost: %llu",
							  st.buffer_ms, st.target_ms, st.drift_ppm,
							  (unsigned long long)st.packets_lost);
		if (host_btn_lbl) lv_label_set_text(host_btn_lbl, "Start Hosting");
		if (join_btn_lbl) lv_label_set_text(join_btn_lbl, "Leave Stream");
	} else {
		lv_label_set_text(status_lbl, "Listen Together: Off");
		lv_label_set_text(stats_lbl, "Zero-drift P2P Wi-Fi audio sync");
		if (host_btn_lbl) lv_label_set_text(host_btn_lbl, "Start Hosting");
		if (join_btn_lbl) lv_label_set_text(join_btn_lbl, "Join Stream");
	}
}

static void lt_timer_cb(lv_timer_t *t) {
	(void)t;
	update_ui();
}

static void host_btn_cb(lv_event_t *e) {
	(void)e;
	if (lt_get_mode() == LT_MODE_HOST) {
		lt_stop();
	} else {
		lt_start_host();
	}
	update_ui();
}

static void join_btn_cb(lv_event_t *e) {
	(void)e;
	if (lt_get_mode() == LT_MODE_CLIENT) {
		lt_stop();
	} else {
		lt_host_info_t hosts[LT_MAX_HOSTS];
		int n = lt_discover_hosts(hosts, LT_MAX_HOSTS, 300);
		lt_start_client(n > 0 ? hosts[0].ip : NULL);
	}
	update_ui();
}

void listentogether_page_init(gui_config_t *cfg) {
	(void)cfg;
	if (listentogether_screen) return;

	listentogether_screen = lv_obj_create(NULL);
	lv_obj_set_style_bg_color(listentogether_screen, lv_color_black(), 0);

	lv_obj_t *cont = lv_obj_create(listentogether_screen);
	lv_obj_set_size(cont, LV_PCT(92), LV_PCT(88));
	lv_obj_center(cont);
	lv_obj_set_flex_flow(cont, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(cont, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_row(cont, 10, 0);
	lv_obj_set_style_bg_opa(cont, LV_OPA_TRANSP, 0);
	lv_obj_set_style_border_width(cont, 0, 0);

	status_lbl = lv_label_create(cont);
	lv_obj_set_style_text_color(status_lbl, lv_color_white(), 0);

	stats_lbl = lv_label_create(cont);
	lv_obj_set_style_text_color(stats_lbl, lv_color_hex(0x9BA3AE), 0);

	lv_obj_t *host_btn = lv_button_create(cont);
	lv_obj_set_width(host_btn, LV_PCT(80));
	host_btn_lbl = lv_label_create(host_btn);
	lv_obj_center(host_btn_lbl);

	lv_obj_t *join_btn = lv_button_create(cont);
	lv_obj_set_width(join_btn, LV_PCT(80));
	join_btn_lbl = lv_label_create(join_btn);
	lv_obj_center(join_btn_lbl);

	if (STREAMING_LAN_PREVIEW_ONLY) {
		lv_label_set_text(status_lbl, tr("Listen Together"));
		lv_obj_set_width(stats_lbl, LV_PCT(100));
		lv_label_set_long_mode(stats_lbl, LV_LABEL_LONG_WRAP);
		lv_obj_set_style_text_align(stats_lbl, LV_TEXT_ALIGN_CENTER, 0);
		lv_label_set_text(stats_lbl, tr("Hosting and joining are unavailable in the browser."));
		lv_label_set_text(host_btn_lbl, "Start Hosting");
		lv_label_set_text(join_btn_lbl, "Join Stream");
		streaming_disable_preview_action(host_btn, host_btn_lbl);
		streaming_disable_preview_action(join_btn, join_btn_lbl);
		return; // No discovery, socket callbacks, or stats timer in the browser.
	}

	lv_obj_add_event_cb(host_btn, host_btn_cb, LV_EVENT_CLICKED, NULL);
	lv_obj_add_event_cb(join_btn, join_btn_cb, LV_EVENT_CLICKED, NULL);
	update_ui();
	if (!lt_ui_timer) {
		lt_ui_timer = lv_timer_create(lt_timer_cb, 500, NULL);
	}
}
