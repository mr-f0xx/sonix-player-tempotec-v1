#include "dlna.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "lvgl/lvgl.h"

#include "src/gui/fonts/fonts.h"
#include "src/gui/shell/gui.h"
#include "src/gui/shell/icons.h"
#include "src/gui/nowplaying/player.h"
#include "src/gui/shell/scrolltext.h"
#include "src/gui/shell/settingsrow.h"
#include "src/gui/shell/switcher.h"
#include "src/gui/shell/theme.h"
#include "src/gui/wireless/wifisettings.h"
#include "src/system/audio/alsa-controls.h"
#include "src/system/audio/audio.h"
#include "src/system/playback/device_state.h"
#include "src/system/remote/dlna.h"
#include "src/system/remote/dlna_cast.h"
#include "src/system/core/lang.h"
#include "src/system/device/system.h"
#include "src/system/net/wifi.h"

lv_obj_t *dlna_screen;

// The command pump does not tick at the page's rate: commands arrive while the
// page is closed too, and that is exactly when they have to work.
#define PUMP_MS 120
#define PAGE_POLL_MS 400

#define DL_UNAVAILABLE "dlna_unavailable"
#define DL_OFF_HELP "dlna_off_note"
#define DL_STARTING "turning_on"
#define DL_WAIT "dlna_waiting_for_a_device"
#define DL_RECEIVING "dlna_playing_from_dlna"
#define DL_FETCHING "dlna_downloading_the_track"
#define DL_NO_CARD "dlna_no_card_note"

static lv_obj_t *toggle;
static lv_obj_t *glyph;
static lv_obj_t *title_label;
static lv_obj_t *artist_label;
static lv_obj_t *album_label;
static lv_obj_t *status_label;
static lv_obj_t *wifi_row;
static lv_obj_t *cast_btn;
static lv_obj_t *cast_layer;
static lv_obj_t *cast_status_label;
static lv_obj_t *cast_rows[DLNA_CAST_MAX_TARGETS];
static lv_obj_t *cast_row_labels[DLNA_CAST_MAX_TARGETS];
static lv_obj_t *cast_play_row;
static lv_obj_t *cast_stop_row;
static lv_timer_t *page_timer;

static void refresh_cast_controls(void);
static void cast_layer_hide(void);

// ---------------------------------------------------------------------------

static void hide(lv_obj_t *obj) { lv_obj_set_hidden(obj, true); }
static void show(lv_obj_t *obj) { lv_obj_set_hidden(obj, false); }

static bool switch_is_on(void) { return lv_obj_has_state(toggle, LV_STATE_CHECKED); }

static bool wifi_is_connected(void) {
	wifi_status_t status;
	wifi_get_status(&status);
	return status.state == WIFI_STATE_CONNECTED && status.ip[0] != '\0';
}

static void set_line(lv_obj_t *label, const char *text) {
	if (!text || text[0] == '\0') {
		hide(label);
		return;
	}
	scrolltext_set(label, text);
	show(label);
}

static void hide_receiver(void) {
	hide(glyph);
	hide(title_label);
	hide(artist_label);
	hide(album_label);
}

// ---------------------------------------------------------------------------
// the pump: the phone's commands, carried out here
// ---------------------------------------------------------------------------
//
// system/dlna.c collects them on a network thread and leaves them in a queue;
// this is the only place playback is touched from.

static void apply_command(const dlna_command_t *cmd) {
	switch (cmd->kind) {
	case DLNA_CMD_PLAY: {
		// A queue of exactly one track: the real queue lives on the phone,
		// which sends the next track when this one ends. With one entry,
		// auto-advance stops on its own instead of picking something at random
		// from the user's library.
		const char *one[1] = {cmd->path};
		device_state_play_list(one, 1, 0);
		if (cmd->position > 0.5) {
			device_state_seek(cmd->position);
		}
		dlna_report_playing(cmd->path);
		player_refresh_now_playing();
		// The track comes from outside and was not asked for here, so showing
		// the player is the only way to signal that something is happening.
		if (!player_sheet_is_open()) {
			player_sheet_open(true);
		}
		break;
	}
	case DLNA_CMD_PAUSE:
		if (audio_get_status() == AUDIO_STATUS_PLAYING) {
			player_key_play_pause();
		}
		break;
	case DLNA_CMD_RESUME:
		if (audio_get_status() != AUDIO_STATUS_PLAYING) {
			player_key_play_pause();
		}
		break;
	case DLNA_CMD_STOP:
		device_state_stop();
		dlna_report_stopped();
		player_refresh_now_playing();
		break;
	case DLNA_CMD_SEEK:
		device_state_seek(cmd->position);
		break;
	case DLNA_CMD_VOLUME:
		set_volume_percent(cmd->volume);
		gui_notify_volume(get_volume_percent());
		break;
	case DLNA_CMD_NONE:
	default:
		break;
	}
}

// How slowly the pump ticks while DLNA is off. It cannot stop entirely, since
// it is also what notices DLNA being switched on, but with the service down no
// command can arrive, and two seconds is the worst-case delay between the
// switch being touched and the first useful tick. (Touching the switch also
// restores the fast period directly, below.)
#define PUMP_IDLE_MS 2000

static lv_timer_t *pump_timer;

// Puts the pump back at the rate the current state calls for.
static void pump_pace(void) {
	if (pump_timer) {
		lv_timer_set_period(pump_timer, dlna_get_enabled() ? PUMP_MS : PUMP_IDLE_MS);
	}
}

static void pump_cb(lv_timer_t *timer) {
	// Eight wakeups a second for a service that is off is a cost paid all night
	// in standby. While it is on the rate is unchanged.
	lv_timer_set_period(timer, dlna_get_enabled() ? PUMP_MS : PUMP_IDLE_MS);
	if (!dlna_get_enabled()) {
		return;
	}

	dlna_command_t cmd;
	while (dlna_take_command(&cmd)) {
		apply_command(&cmd);
	}

	if (!dlna_owns_playback()) {
		return;
	}

	// Something else has taken the player: a local track, the radio, an album
	// opened from the library. From here DLNA has nothing left to report, and
	// the phone learns it from its next request.
	char current[512];
	audio_get_current_file(current, sizeof(current));
	if (!dlna_owns_path(current)) {
		dlna_report_stopped();
		return;
	}

	double position = 0, duration = 0;
	audio_get_progress(&position, &duration);
	dlna_report_progress((int)position, (int)duration, get_volume_percent());
}

// ---------------------------------------------------------------------------
// the page
// ---------------------------------------------------------------------------

static void refresh(void) {
	if (!dlna_available()) {
		lv_label_set_text(status_label, tr(DL_UNAVAILABLE));
		lv_obj_add_state(toggle, LV_STATE_DISABLED);
		hide_receiver();
		hide(wifi_row);
		return;
	}

	if (!wifi_is_connected()) {
		// The same line every network-backed page uses, in the normal text
		// colour: it is an instruction, not a footnote.
		lv_label_set_text(status_label, tr("enable_wi_fi_first"));
		lv_obj_set_style_text_color(status_label, theme()->text_primary, 0);
		lv_obj_add_state(toggle, LV_STATE_DISABLED);
		hide_receiver();
		show(wifi_row);
		return;
	}
	lv_obj_remove_local_style_prop(status_label, LV_STYLE_TEXT_COLOR, 0);

	lv_obj_remove_state(toggle, LV_STATE_DISABLED);
	hide(wifi_row);

	if (!switch_is_on()) {
		const char *root = storage_sd_root();
		lv_label_set_text(status_label, root && root[0] ? tr(DL_OFF_HELP) : tr(DL_NO_CARD));
		hide_receiver();
		return;
	}

	dlna_state_t s;
	dlna_get_state(&s);

	// The error is checked before the "starting" state: when startup fails,
	// dlna_running() stays false forever, and in the other order the page would
	// say "starting" indefinitely instead of what went wrong.
	if (s.error[0]) {
		lv_label_set_text(status_label, tr(s.error));
		hide_receiver();
		return;
	}

	// On, but dmrd takes a moment: sys_server queues the request and waits for
	// the renderer to report that it has started.
	if (!dlna_running()) {
		lv_label_set_text(status_label, tr(DL_STARTING));
		hide_receiver();
		return;
	}

	show(glyph);
	set_line(title_label, s.title);
	set_line(artist_label, s.artist);
	set_line(album_label, s.album);

	if (s.fetching) {
		lv_label_set_text(status_label, tr(DL_FETCHING));
		return;
	}
	if (dlna_owns_playback()) {
		lv_label_set_text(status_label, tr(DL_RECEIVING));
		return;
	}

	// Nothing has claimed the renderer yet, so show the name to look for on the
	// phone: a music app's device list never says which of the names belongs to
	// this player.
	lv_label_set_text_fmt(status_label, "%s\n\xC2\xAB%s\xC2\xBB", tr(DL_WAIT), dlna_name());
}

static void page_poll_cb(lv_timer_t *timer) {
	(void)timer;

	refresh();
	refresh_cast_controls();

	if (!switch_is_on()) {
		return;
	}

	// The network went away under a running receiver: there is nothing left to
	// be discovered on.
	if (!wifi_is_connected()) {
		lv_obj_remove_state(toggle, LV_STATE_CHECKED);
		dlna_set_enabled(false);
		refresh();
	}
}

static void toggle_changed_cb(lv_event_t *e) {
	(void)e;

	bool on = switch_is_on();

	if (on && !dlna_available()) {
		lv_obj_remove_state(toggle, LV_STATE_CHECKED);
		gui_notify_popup(DL_UNAVAILABLE);
		return;
	}
	if (on && !wifi_is_connected()) {
		lv_obj_remove_state(toggle, LV_STATE_CHECKED);
		gui_notify_popup("enable_wi_fi_first");
		return;
	}

	if (on) {
		// Re-check the scratch directory now: the card may have been inserted
		// after boot. Only when one is really there, though -- main.c has
		// already set a fallback root, and overwriting it with NULL would leave
		// DLNA nowhere to write.
		const char *root = storage_sd_root();
		if (root && root[0]) {
			dlna_set_root(root);
		} else {
			gui_notify_popup(DL_NO_CARD);
		}
	}

	dlna_set_enabled(on);
	pump_pace(); // no wait between the switch and the first command served
	refresh();
}

// The control centre switched the service with the page open under it: the
// switch and the lines under it follow, and nothing else is touched.
static void switched_elsewhere_cb(lv_event_t *e) {
	(void)e;
	if (dlna_get_enabled() && dlna_available()) {
		lv_obj_add_state(toggle, LV_STATE_CHECKED);
	} else {
		lv_obj_remove_state(toggle, LV_STATE_CHECKED);
	}
	refresh();
}

static void screen_loaded_cb(lv_event_t *e) {
	(void)e;

	set_line(title_label, "");
	set_line(artist_label, "");
	set_line(album_label, "");

	// The switch reflects what the receiver is doing now, not what it was doing
	// the last time this page was open.
	if (dlna_get_enabled() && dlna_available()) {
		lv_obj_add_state(toggle, LV_STATE_CHECKED);
	} else {
		lv_obj_remove_state(toggle, LV_STATE_CHECKED);
	}

	refresh();
	refresh_cast_controls();
	lv_timer_resume(page_timer);
	lv_timer_ready(page_timer);
}

// Like AirPlay, leaving the page does not stop the receiver: the switch owns
// that state, the page is only where it lives. All that stops here is the
// redraw.
static void screen_unloaded_cb(lv_event_t *e) {
	(void)e;
	lv_timer_pause(page_timer);
	cast_layer_hide();
}

static void wifi_row_cb(lv_event_t *e) {
	(void)e;
	switch_screen(wifisettings_screen);
}

// ---------------------------------------------------------------------------
// DLNA controller (cast-to-renderer) controls. These are independent of the
// receiver switch above: both roles can be used, and switching either role
// never disables the other.

static void cast_layer_hide(void) {
	if (cast_layer) {
		lv_obj_set_hidden(cast_layer, true);
	}
	back_btn_force_hidden(false);
}

static void cast_layer_show(void) {
	if (!cast_layer) {
		return;
	}
	refresh_cast_controls();
	lv_obj_set_hidden(cast_layer, false);
	lv_obj_move_foreground(cast_layer);
	back_btn_force_hidden(true);
}

static void cast_cancel_cb(lv_event_t *e) {
	(void)e;
	cast_layer_hide();
}

static void cast_btn_cb(lv_event_t *e) {
	(void)e;
	cast_layer_show();
}

static bool dlna_back_guard(void) {
	if (cast_layer && !lv_obj_is_hidden(cast_layer)) {
		cast_layer_hide();
		return true;
	}
	return false;
}

static bool dlna_back_peek(void) {
	return cast_layer && !lv_obj_is_hidden(cast_layer);
}

static void cast_renderer_cb(lv_event_t *e) {
	int index = (int)(intptr_t)lv_event_get_user_data(e);
	dlna_cast_select(index);
	refresh_cast_controls();
}

static void cast_find_cb(lv_event_t *e) {
	(void)e;
	if (!wifi_is_connected()) {
		gui_notify_popup("enable_wi_fi_first");
		return;
	}
	dlna_cast_scan();
	refresh_cast_controls();
}

static void cast_play_cb(lv_event_t *e) {
	(void)e;
	char path[1024] = "";
	audio_get_current_file(path, sizeof(path));
	if (!path[0]) {
		gui_notify_popup("Nothing is playing");
		return;
	}
	dlna_cast_play(path);
	refresh_cast_controls();
}

static void cast_stop_cb(lv_event_t *e) {
	(void)e;
	dlna_cast_stop();
	refresh_cast_controls();
}

static void refresh_cast_controls(void) {
	if (!cast_status_label) return;
	char status[160] = "";
	dlna_cast_status(status, sizeof(status));
	if (!wifi_is_connected()) snprintf(status, sizeof(status), "%s", tr("Enable Wi-Fi to cast to a DLNA renderer"));
	lv_label_set_text(cast_status_label, status);

	dlna_cast_target_t targets[DLNA_CAST_MAX_TARGETS];
	int count = dlna_cast_targets(targets, DLNA_CAST_MAX_TARGETS);
	int selected = dlna_cast_selected();
	for (int i = 0; i < DLNA_CAST_MAX_TARGETS; i++) {
		if (i < count) {
			char name[DLNA_CAST_NAME_MAX + 4];
			snprintf(name, sizeof(name), "%s%s", i == selected ? "✓ " : "", targets[i].name);
			lv_label_set_text(cast_row_labels[i], name);
			show(cast_rows[i]);
		} else {
			hide(cast_rows[i]);
		}
	}
	if (selected >= 0 && selected < count && wifi_is_connected()) show(cast_play_row);
	else hide(cast_play_row);
	if (selected >= 0 && selected < count && status[0] && strncmp(status, "Playing on ", 11) == 0) show(cast_stop_row);
	else hide(cast_stop_row);
}

static void build_cast_layer(gui_config_t *cfg) {
	cast_layer = lv_obj_create(dlna_screen);
	lv_obj_set_size(cast_layer, cfg->screen_width, cfg->screen_height);
	lv_obj_align(cast_layer, LV_ALIGN_TOP_LEFT, 0, 0);
	lv_obj_add_style(cast_layer, &theme_style_screen, 0);
	lv_obj_set_style_bg_opa(cast_layer, LV_OPA_COVER, 0);
	lv_obj_set_style_border_width(cast_layer, 0, 0);
	lv_obj_set_style_radius(cast_layer, 0, 0);
	lv_obj_set_style_pad_all(cast_layer, 0, 0);
	lv_obj_set_scrollable(cast_layer, false);
	lv_obj_set_hidden(cast_layer, true);

	lv_obj_t *heading = lv_label_create(cast_layer);
	lv_label_set_text(heading, tr("Cast to a DLNA renderer"));
	lv_obj_add_style(heading, &theme_style_text, 0);
	lv_obj_set_style_text_font(heading, &font_ui_24, 0);
	lv_obj_align(heading, LV_ALIGN_TOP_LEFT, cfg->padding, cfg->padding + cfg->top_bar_height + 10);

	lv_obj_t *cancel = lv_btn_create(cast_layer);
	settingsrow_place_corner_button(cancel, cfg, 0);
	lv_obj_set_style_bg_opa(cancel, LV_OPA_TRANSP, 0);
	lv_obj_set_style_border_width(cancel, 0, 0);
	lv_obj_set_style_shadow_width(cancel, 0, 0);
	lv_obj_set_style_pad_all(cancel, 0, 0);
	lv_obj_add_event_cb(cancel, cast_cancel_cb, LV_EVENT_CLICKED, NULL);

	lv_obj_t *cancel_icon = lv_image_create(cancel);
	lv_image_set_src(cancel_icon, &icon_close);
	lv_obj_add_style(cancel_icon, &theme_style_icon, 0);
	settingsrow_scale_corner_icon(cancel_icon, cfg);
	lv_obj_center(cancel_icon);

	int top = settingsrow_content_top(cfg);
	lv_obj_t *box = lv_obj_create(cast_layer);
	lv_obj_set_size(box, lv_pct(100), cfg->screen_height - top);
	lv_obj_align(box, LV_ALIGN_TOP_LEFT, 0, top);
	lv_obj_set_style_bg_opa(box, 0, 0);
	lv_obj_set_style_border_width(box, 0, 0);
	lv_obj_set_style_radius(box, 0, 0);
	lv_obj_set_style_pad_hor(box, cfg->padding, 0);
	lv_obj_set_style_pad_bottom(box, cfg->padding, 0);
	lv_obj_set_style_pad_gap(box, 8, 0);
	lv_obj_set_scroll_dir(box, LV_DIR_VER);
	lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);

	settingsrow_action(box, "Search for DLNA renderers", cast_find_cb, NULL);
	for (int i = 0; i < DLNA_CAST_MAX_TARGETS; i++) {
		cast_rows[i] = settingsrow_action(box, "DLNA renderer", cast_renderer_cb, (void *)(intptr_t)i);
		cast_row_labels[i] = settingsrow_name_label(cast_rows[i]);
		hide(cast_rows[i]);
	}
	cast_play_row = settingsrow_action(box, "Cast current track", cast_play_cb, NULL);
	cast_stop_row = settingsrow_action(box, "Stop casting", cast_stop_cb, NULL);
	hide(cast_play_row);
	hide(cast_stop_row);

	cast_status_label = lv_label_create(box);
	lv_obj_set_width(cast_status_label, lv_pct(100));
	lv_label_set_long_mode(cast_status_label, LV_LABEL_LONG_WRAP);
	lv_obj_add_style(cast_status_label, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(cast_status_label, &font_ui_18, 0);
	lv_obj_set_style_pad_hor(cast_status_label, 8, 0);
	lv_obj_set_style_pad_ver(cast_status_label, 8, 0);
}

// ---------------------------------------------------------------------------

void dlna_page_init(gui_config_t *cfg) {
	const bool compact = cfg->screen_width < 320;

	lv_obj_t *container = settingsrow_page(dlna_screen, cfg, "dlna");
	settingsrow_title_corner_slots(settingsrow_page_title(dlna_screen), cfg, 1);

	cast_btn = lv_btn_create(dlna_screen);
	settingsrow_place_corner_button(cast_btn, cfg, 0);
	lv_obj_set_style_bg_opa(cast_btn, LV_OPA_TRANSP, 0);
	lv_obj_set_style_border_width(cast_btn, 0, 0);
	lv_obj_set_style_shadow_width(cast_btn, 0, 0);
	lv_obj_set_style_pad_all(cast_btn, 0, 0);
	lv_obj_add_event_cb(cast_btn, cast_btn_cb, LV_EVENT_CLICKED, NULL);

	lv_obj_t *cast_btn_icon = lv_image_create(cast_btn);
	lv_image_set_src(cast_btn_icon, &icon_dlna_page);
	lv_obj_add_style(cast_btn_icon, &theme_style_icon, 0);
	int corner_icon_side = compact ? 22 : 28;
	lv_image_set_scale(cast_btn_icon, (uint32_t)(LV_SCALE_NONE * corner_icon_side / 128));
	lv_obj_center(cast_btn_icon);

	settingsrow_toggle(container, "dlna", &toggle, toggle_changed_cb);

	glyph = lv_image_create(container);
	lv_image_set_src(glyph, &icon_dlna_page);
	lv_obj_add_style(glyph, &theme_style_icon, 0);
	lv_obj_set_style_image_opa(glyph, LV_OPA_40, 0);
	int icon_size = compact ? 72 : 96;
	lv_obj_set_size(glyph, icon_size, icon_size);
	lv_image_set_inner_align(glyph, LV_IMAGE_ALIGN_CENTER);
	lv_image_set_scale(glyph, (uint32_t)(LV_SCALE_NONE * icon_size / 128));
	lv_obj_set_style_margin_top(glyph, compact ? 8 : 16, 0);
	lv_obj_set_style_margin_bottom(glyph, compact ? 8 : 16, 0);
	lv_obj_set_style_align(glyph, LV_ALIGN_CENTER, 0);

	title_label = lv_label_create(container);
	lv_obj_set_width(title_label, lv_pct(100));
	lv_obj_add_style(title_label, &theme_style_text, 0);
	lv_obj_set_style_text_font(title_label, compact ? &font_ui_22 : &font_ui_26, 0);
	lv_obj_set_style_text_align(title_label, LV_TEXT_ALIGN_CENTER, 0);
	scrolltext_apply(title_label);

	artist_label = lv_label_create(container);
	lv_obj_set_width(artist_label, lv_pct(100));
	lv_obj_add_style(artist_label, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(artist_label, compact ? &font_ui_18 : &font_ui_22, 0);
	lv_obj_set_style_text_align(artist_label, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_set_style_pad_top(artist_label, compact ? 2 : 6, 0);
	scrolltext_apply(artist_label);

	album_label = lv_label_create(container);
	lv_obj_set_width(album_label, lv_pct(100));
	lv_obj_add_style(album_label, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(album_label, compact ? &font_ui_18 : &font_ui_20, 0);
	lv_obj_set_style_text_align(album_label, LV_TEXT_ALIGN_CENTER, 0);
	scrolltext_apply(album_label);

	status_label = lv_label_create(container);
	lv_obj_set_width(status_label, lv_pct(100));
	lv_label_set_long_mode(status_label, LV_LABEL_LONG_WRAP);
	lv_obj_add_style(status_label, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(status_label, compact ? &font_ui_18 : &font_ui_20, 0);
	lv_obj_set_style_text_align(status_label, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_set_style_pad_hor(status_label, 4, 0);
	lv_obj_set_style_pad_top(status_label, compact ? 8 : 14, 0);

	// The same row as the Wi-Fi transfer page: the notice says what is missing,
	// the Wi-Fi settings row leads to where it is fixed.
	wifi_row = settingsrow_add(container, "wi_fi_settings", NULL, wifi_row_cb, NULL);
	lv_obj_set_style_margin_top(wifi_row, 16, 0);
	hide(wifi_row);

	build_cast_layer(cfg);

	page_timer = lv_timer_create(page_poll_cb, PAGE_POLL_MS, NULL);
	lv_timer_pause(page_timer);

	// This timer never stops, not even with the screen off: it is the bridge
	// between the network thread and playback, and a phone pushing a track
	// while another page is open still has to be served. It only slows down
	// while DLNA is off, which is nearly always.
	pump_timer = lv_timer_create(pump_cb, PUMP_IDLE_MS, NULL);
	pump_pace();

	lv_obj_add_event_cb(dlna_screen, screen_loaded_cb, LV_EVENT_SCREEN_LOADED, NULL);
	lv_obj_add_event_cb(dlna_screen, switched_elsewhere_cb, LV_EVENT_REFRESH, NULL);
	lv_obj_add_event_cb(dlna_screen, screen_unloaded_cb, LV_EVENT_SCREEN_UNLOADED, NULL);
	switcher_set_back_guard(dlna_screen, dlna_back_guard);
	switcher_set_back_guard_peek(dlna_screen, dlna_back_peek);

	// No dlna_set_root() here: main.c has already called it with the real card
	// or the fallback root, and this runs later, so repeating it with whatever
	// storage_sd_root() answers now (NULL without a card) would undo it.
	refresh();
}
