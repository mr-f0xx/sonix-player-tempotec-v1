#include "btreceiverpage.h"

#include <stdio.h>
#include <string.h>

#include "src/gui/shell/confirm.h"
#include "src/gui/fonts/fonts.h"
#include "src/gui/shell/icons.h"
#include "src/gui/shell/gui.h"
#include "src/gui/shell/popover.h"
#include "src/gui/shell/settingsrow.h"
#include "src/gui/shell/switcher.h"
#include "src/gui/shell/theme.h"
#include "src/system/bluetooth/bluetooth.h"
#include "src/system/bluetooth/btreceiver.h"
#include "src/system/core/lang.h"

lv_obj_t *btreceiverpage_screen;

#define POLL_MS 500

static lv_obj_t *big_icon;
static lv_obj_t *format_label;
static lv_obj_t *device_label;
static lv_obj_t *status_label;
static lv_obj_t *title_label;
static lv_timer_t *poll_timer;
typedef struct {
	unsigned receiver, devices, stream, track;
	bool streaming, busy, enabled;
} receiver_snapshot_t;
static receiver_snapshot_t last_snapshot;
static bool have_snapshot;
static lv_color_t status_normal_color;

// ---------------------------------------------------------------------------

// "44100" -> "44,1 kHz". The rates that divide evenly lose the decimal: 48 kHz
// written as 48,0 reads like a measurement rather than a number everyone knows.
static void format_rate(unsigned rate, char *out, size_t size) {
	if (rate == 0) {
		snprintf(out, size, "--");
	} else if (rate % 1000 == 0) {
		snprintf(out, size, "%u kHz", rate / 1000);
	} else {
		snprintf(out, size, "%.1f kHz", rate / 1000.0);
	}
}

static void refresh(void) {
	btreceiver_state_t st;
	btreceiver_get_state(&st);
	char current_name[BT_NAME_MAX] = "";
	bool has_current_name = bluetooth_receiver_device(NULL, 0, current_name, sizeof(current_name));
	if (!has_current_name) {
		snprintf(current_name, sizeof(current_name), "%s", st.device);
	}

	lv_obj_set_style_text_color(status_label, status_normal_color, 0);

	lv_obj_set_hidden(title_label, true);

	if (st.error[0]) {
		lv_label_set_text(device_label, current_name);
		lv_label_set_text(format_label, "");
		lv_label_set_text(status_label, st.error);
		lv_obj_set_style_text_color(status_label, lv_color_make(224, 27, 36), 0);
		return;
	}

	if (!st.active) {
		// Asked now, not remembered. Show the RX mode badge and the adapter's
		// visible name explicitly so users can tell at a glance that the player
		// is in RX mode and ready to be found from a phone.
		lv_label_set_text(format_label, tr("bt_mode_rx_badge"));
		if (!bluetooth_get_enabled() || bluetooth_busy()) {
			lv_label_set_text(device_label, "");
			lv_label_set_text(status_label, bluetooth_get_enabled() ? tr("turning_on") : tr("bt_bluetooth_off"));
		} else if (has_current_name) {
			lv_label_set_text(device_label, current_name);
			lv_label_set_text(status_label, tr("btreceiver_connecting"));
		} else {
			lv_label_set_text_fmt(device_label, tr("bt_visible_as"), bluetooth_local_name());
			lv_label_set_text(status_label, tr("btreceiver_no_source_note"));
		}
		return;
	}

	lv_label_set_text(device_label, current_name);

	// The codec and the rate, which is the whole of what this page is for: the
	// sender chose both and this is the only place the choice is visible.
	char rate[24];
	format_rate(st.sample_rate, rate, sizeof(rate));
	char text[64];
	if (st.codec[0]) {
		snprintf(text, sizeof(text), "%s  %s", st.codec, rate);
	} else {
		snprintf(text, sizeof(text), "%s", rate);
	}
	lv_label_set_text(format_label, text);

	// What the sender says it is playing, when it says anything. A phone
	// streaming from a music app names the track over AVRCP; a laptop sending
	// system audio usually names nothing, and then the line goes back to saying
	// what the stream is doing.
	bt_track_t track;
	if (st.streaming && bluetooth_receiver_track(&track) && track.title[0]) {
		lv_label_set_text(title_label, track.title);
		lv_obj_set_hidden(title_label, false);

		// Artist and album on one line, with whichever of them there is.
		char line[sizeof(track.artist) + sizeof(track.album) + 8];
		if (track.artist[0] && track.album[0]) {
			snprintf(line, sizeof(line), "%s  --  %s", track.artist, track.album);
		} else {
			snprintf(line, sizeof(line), "%s", track.artist[0] ? track.artist : track.album);
		}
		lv_label_set_text(status_label, line);
		return;
	}

	lv_obj_set_hidden(title_label, true);
	lv_label_set_text(status_label, st.streaming ? tr("btreceiver_playing") : tr("btreceiver_waiting"));
}

static bool leaving;

// The device the mode was last started for from here. A start that ended in
// an error is not tried again for the same device until it has gone away:
// otherwise the page would restart a failing stream every half second.
static char started_for[24];

// Starts the mode when a sender is connected and the mode is not running --
// on arrival, and for a phone or a computer that connects while the page is
// open, which is when a sender usually turns up.
static void start_if_sender(void) {
	if (leaving || btreceiver_is_active()) {
		return;
	}
	char mac[sizeof(started_for)];
	if (!bluetooth_receiver_device(mac, sizeof(mac), NULL, 0)) {
		started_for[0] = '\0';
		return;
	}
	btreceiver_state_t st;
	btreceiver_get_state(&st);
	if (st.error[0] && strcmp(mac, started_for) == 0) {
		return;
	}
	snprintf(started_for, sizeof(started_for), "%s", mac);
	btreceiver_start();
}

static void poll_cb(lv_timer_t *timer) {
	(void)timer;
	if (lv_screen_active() != btreceiverpage_screen) {
		return;
	}
	start_if_sender();
	// Compare fields separately: adding counters and flags permits changes in
	// opposite directions to cancel, leaving old metadata or status on screen.
	btreceiver_state_t st;
	btreceiver_get_state(&st);
	receiver_snapshot_t now = {
		.receiver = btreceiver_serial(), .devices = bluetooth_devices_serial(),
		.stream = bluetooth_receiver_stream_serial(), .track = bluetooth_receiver_track_serial(),
		.streaming = st.streaming, .busy = bluetooth_busy(), .enabled = bluetooth_get_enabled(),
	};
	if (have_snapshot && now.receiver == last_snapshot.receiver && now.devices == last_snapshot.devices &&
	    now.stream == last_snapshot.stream && now.track == last_snapshot.track &&
	    now.streaming == last_snapshot.streaming && now.busy == last_snapshot.busy &&
	    now.enabled == last_snapshot.enabled) return;
	last_snapshot = now;
	have_snapshot = true;
	refresh();
}

// ---------------------------------------------------------------------------
// Choosing the codec
//
// The sending device picks the codec when the link comes up, and on a phone
// that choice is buried in the developer options or not offered at all. What
// bluealsa exposes on the sink PCM is the same SelectCodec the headphone side
// uses, so the player can move the link itself.
//
// The list is whatever the two ends have in common on the link that is up.
// Nothing is remembered: the next connection negotiates again.
// ---------------------------------------------------------------------------

#define CODEC_MENU_MAX 6

static popover_item_t codec_items[CODEC_MENU_MAX];
static char codec_labels[CODEC_MENU_MAX][BT_CODEC_MAX];

static void codec_chosen(void *user) {
	const char *codec = user;
	if (codec && codec[0]) {
		bluetooth_receiver_set_codec(codec);
	}
}

static void codec_btn_cb(lv_event_t *e) {
	if (switcher_back_drag_active()) {
		return;
	}

	char offered[BT_MAX_CODECS][BT_CODEC_MAX];
	char selected[BT_CODEC_MAX] = "";
	int count = bluetooth_receiver_codecs(offered, BT_MAX_CODECS, selected, sizeof(selected));
	if (count <= 0) {
		gui_notify_popup("btreceiver_no_codecs");
		return;
	}

	if (count > CODEC_MENU_MAX) {
		count = CODEC_MENU_MAX;
	}
	for (int i = 0; i < count; i++) {
		// Copied: the popover calls back after it has closed, so the strings it
		// was given have to still be there.
		memcpy(codec_labels[i], offered[i], sizeof(codec_labels[i]));
		codec_labels[i][sizeof(codec_labels[i]) - 1] = '\0';
		codec_items[i].label = codec_labels[i];
		codec_items[i].action = codec_chosen;
		codec_items[i].user = codec_labels[i];
		codec_items[i].checked = selected[0] && strcasecmp(selected, codec_labels[i]) == 0;
		codec_items[i].verbatim = true;
	}

	popover_show(lv_event_get_current_target(e), codec_items, count);
}

// ---------------------------------------------------------------------------

static void leave_async(void *user) {
	(void)user;
	back_btn_cb(NULL);
	leaving = false;
}

// Deferred by one turn of the event loop, as on the DAC page: this runs from
// inside the confirmation's own handler, and navigating out from under a dialog
// that is still closing leaves the screen change undone.
static void really_leave(void *user) {
	(void)user;
	btreceiver_stop();
	have_snapshot = false;
	refresh();
	leaving = true;
	lv_async_call(leave_async, NULL);
}

// The back chevron and the swipe both come through here. True means "handled,
// stay put": leaving stops the audio, and that is worth one question.
static bool back_guard(void) {
	if (leaving || !btreceiver_is_active()) {
		return false;
	}
	confirm_show("btreceiver_leave_confirm", "btreceiver_leave_confirm_note", "leave", really_leave, NULL);
	return true;
}

// ---------------------------------------------------------------------------

static void loaded_cb(lv_event_t *e) {
	(void)e;
	// The A2DP sink exists only while this page is open (bluetooth.h): a phone
	// connects to it from here on, and bring-up starts straight into sink mode
	// when entered while the radio was off.
	bluetooth_set_receiver_profile(true);
	if (!bluetooth_get_enabled() && bluetooth_available()) {
		bluetooth_set_enabled(true);
	}
	have_snapshot = false;
	// Arriving is the switch: there is nothing else this page does, so a toggle
	// on it would only repeat what opening it already said.
	started_for[0] = '\0';
	start_if_sender();
	refresh();
}

static void unloaded_cb(lv_event_t *e) {
	(void)e;
	// Every other way out -- the power menu, a notification, a screen change
	// this page did not ask for -- ends the mode too. The guard above catches
	// the deliberate ones; this catches the rest, so the device is never left
	// holding a stream nobody can see.
	btreceiver_stop();
	bluetooth_set_receiver_profile(false);
}

void btreceiverpage_init(gui_config_t *cfg) {
	bool compact = cfg->screen_width < 320;
	lv_obj_t *container = settingsrow_page(btreceiverpage_screen, cfg, "btreceiver_title");

	// The codec button, in the corner every other page puts its options in.
	lv_obj_t *codec_btn = lv_btn_create(btreceiverpage_screen);
	settingsrow_place_corner_button(codec_btn, cfg, 0);
	lv_obj_set_style_bg_opa(codec_btn, LV_OPA_TRANSP, 0);
	lv_obj_set_style_border_width(codec_btn, 0, 0);
	lv_obj_set_style_shadow_width(codec_btn, 0, 0);
	lv_obj_set_style_pad_all(codec_btn, 0, 0);
	lv_obj_add_event_cb(codec_btn, codec_btn_cb, LV_EVENT_CLICKED, NULL);

	lv_obj_t *codec_glyph = lv_image_create(codec_btn);
	lv_image_set_src(codec_glyph, &icon_change_codec);
	lv_obj_add_style(codec_glyph, &theme_style_icon, 0);
	settingsrow_scale_corner_icon(codec_glyph, cfg);
	lv_obj_center(codec_glyph);

	// Straight into the flex flow with no alignment of its own: the container
	// settingsrow_page() builds already centres its children, and an object
	// positioned by alignment leaves the flow and is measured as taking no
	// space. On the V1 the 200 px source nearly fills the display and crowds
	// the receiver status below it, so scale both its drawing and layout box.
	big_icon = lv_image_create(container);
	lv_image_set_src(big_icon, &icon_bluetooth_receiver_page);
	int icon_size = compact ? 96 : 200;
	int source_side = LV_MAX((int)icon_bluetooth_receiver_page.header.w,
							 (int)icon_bluetooth_receiver_page.header.h);
	lv_obj_set_size(big_icon, icon_size, icon_size);
	lv_image_set_inner_align(big_icon, LV_IMAGE_ALIGN_CENTER);
	if (source_side > 0) {
		lv_image_set_scale(big_icon, (uint32_t)(LV_SCALE_NONE * icon_size / source_side));
	}
	lv_obj_set_style_margin_top(big_icon, compact ? 8 : 40, 0);

	format_label = lv_label_create(container);
	lv_obj_set_width(format_label, lv_pct(100));
	lv_label_set_long_mode(format_label, LV_LABEL_LONG_WRAP);
	lv_obj_add_style(format_label, &theme_style_text, 0);
	lv_obj_set_style_text_align(format_label, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_set_style_text_font(format_label, compact ? &font_ui_20_bold : &font_ui_24_bold, 0);
	lv_obj_set_style_margin_top(format_label, compact ? 6 : 20, 0);
	lv_label_set_text(format_label, "");

	device_label = lv_label_create(container);
	lv_obj_set_width(device_label, lv_pct(100));
	lv_label_set_long_mode(device_label, LV_LABEL_LONG_DOT);
	lv_obj_set_style_text_align(device_label, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_add_style(device_label, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(device_label, compact ? &font_ui_18 : &font_ui_22, 0);
	lv_obj_set_style_margin_top(device_label, compact ? 2 : 10, 0);
	lv_label_set_text(device_label, "");

	// The track the sender names, under the device it comes from. Hidden when
	// there is nothing to name, so a laptop's system audio does not leave an
	// empty row on the page.
	title_label = lv_label_create(container);
	lv_obj_set_width(title_label, lv_pct(100));
	lv_label_set_long_mode(title_label, LV_LABEL_LONG_DOT);
	lv_obj_set_style_text_align(title_label, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_add_style(title_label, &theme_style_text, 0);
	lv_obj_set_style_text_font(title_label, compact ? &font_ui_18 : &font_ui_24, 0);
	lv_obj_set_style_margin_top(title_label, compact ? 6 : 18, 0);
	lv_label_set_text(title_label, "");
	lv_obj_set_hidden(title_label, true);

	status_label = lv_label_create(container);
	lv_obj_set_width(status_label, lv_pct(100));
	lv_label_set_long_mode(status_label, LV_LABEL_LONG_WRAP);
	lv_obj_set_style_text_align(status_label, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_add_style(status_label, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(status_label, compact ? &font_ui_16 : &font_ui_22, 0);
	lv_obj_set_style_margin_top(status_label, compact ? 2 : 10, 0);
	lv_label_set_text(status_label, "");
	status_normal_color = lv_obj_get_style_text_color(status_label, LV_PART_MAIN);

	lv_obj_add_event_cb(btreceiverpage_screen, loaded_cb, LV_EVENT_SCREEN_LOADED, NULL);
	lv_obj_add_event_cb(btreceiverpage_screen, unloaded_cb, LV_EVENT_SCREEN_UNLOADED, NULL);
	switcher_set_back_guard(btreceiverpage_screen, back_guard);

	poll_timer = lv_timer_create(poll_cb, POLL_MS, NULL);
	(void)poll_timer;
}
