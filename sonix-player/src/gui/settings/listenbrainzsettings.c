#include "listenbrainzsettings.h"

#include <stdio.h>
#include <string.h>

#include "src/gui/fonts/fonts.h"
#include "src/gui/shell/confirm.h"
#include "src/gui/shell/keyboard.h"
#include "src/gui/shell/settingsrow.h"
#include "src/gui/shell/switcher.h"
#include "src/gui/shell/theme.h"
#include "src/gui/shell/toast.h"
#include "src/system/audio/audio.h"
#include "src/system/core/lang.h"
#include "src/system/listenbrainz/listenbrainz.h"
#include "src/system/playback/audiobook.h"
#include "src/system/playback/device_state.h"
#include "src/system/streaming/podcastcache.h"
#include "src/system/streaming/ytmusic.h"
#include "src/system/streaming/radio.h"

#define TICK_MS 1000

static lv_obj_t *page_screen;
static lv_obj_t *enabled_switch;
static lv_obj_t *account_value;
static lv_obj_t *status_value;
static lv_obj_t *queued_value;
static lv_obj_t *note_label;

static lv_obj_t *login_screen;
static lv_obj_t *token_field;
static keyboard_t *login_keyboard;

lv_obj_t *listenbrainzsettings_screen(void) { return page_screen; }

// ---------------------------------------------------------------------------
// the page
// ---------------------------------------------------------------------------

// Whether a token is held, for the account row. The status struct carries no
// token, and the state machine is the only other way to ask.
static bool token_held(void) {
	listenbrainz_status_t st;
	listenbrainz_get_status(&st);
	return st.state == LISTENBRAINZ_STATE_CONNECTED || st.state == LISTENBRAINZ_STATE_REFUSED;
}

static void refresh(void) {
	listenbrainz_status_t st;
	listenbrainz_get_status(&st);

	if (st.enabled) {
		lv_obj_add_state(enabled_switch, LV_STATE_CHECKED);
	} else {
		lv_obj_remove_state(enabled_switch, LV_STATE_CHECKED);
	}

	lv_label_set_text(account_value, token_held() ? tr("listenbrainz_signed_in") : tr("listenbrainz_not_signed_in"));

	const char *status = "";
	switch (st.state) {
	case LISTENBRAINZ_STATE_OFF:
		status = tr("listenbrainz_state_off");
		break;
	case LISTENBRAINZ_STATE_SIGNED_OUT:
		status = tr("listenbrainz_not_signed_in");
		break;
	case LISTENBRAINZ_STATE_CONNECTED:
		status = tr("connected");
		break;
	case LISTENBRAINZ_STATE_TOKEN_EXPIRED:
		status = tr("listenbrainz_token_expired");
		break;
	case LISTENBRAINZ_STATE_REFUSED:
		status = st.detail;
		break;
	}
	lv_label_set_text(status_value, status);
	lv_label_set_text(note_label, tr("listenbrainz_note"));
	lv_label_set_text_fmt(queued_value, "%d", st.queued);
}

static void open_login(void) {
	lv_textarea_set_text(token_field, "");
	keyboard_reset(login_keyboard);
	keyboard_set_field(login_keyboard, token_field);
	lv_obj_add_state(token_field, LV_STATE_FOCUSED);
	keyboard_show_caret(token_field, true);
	switch_screen(login_screen);
}

static void enabled_cb(lv_event_t *e) {
	(void)e;
	bool on = lv_obj_has_state(enabled_switch, LV_STATE_CHECKED);
	listenbrainz_set_enabled(on);
	listenbrainz_status_t st;
	listenbrainz_get_status(&st);
	// Switched on with no usable account behind it: the token is the next
	// step, whether there never was one or ListenBrainz refused the last.
	if (on && !token_held() && st.state != LISTENBRAINZ_STATE_OFF) {
		open_login();
	}
	refresh();
}

static void do_signout(void *user) {
	(void)user;
	listenbrainz_signout();
	toast_success("signed_out");
	refresh();
}

static void account_cb(lv_event_t *e) {
	(void)e;
	if (token_held()) {
		char message[160];
		snprintf(message, sizeof(message), tr("sign_out_confirm_note"), "ListenBrainz");
		confirm_show("listenbrainz_sign_out", message, "leave", do_signout, NULL);
		return;
	}
	open_login();
}

static void loaded_cb(lv_event_t *e) {
	(void)e;
	refresh();
}

static void build_page(gui_config_t *cfg) {
	page_screen = lv_obj_create(NULL);
	lv_obj_t *container = settingsrow_page(page_screen, cfg, "listenbrainz");

	settingsrow_toggle(container, "listenbrainz_scrobble", &enabled_switch, enabled_cb);
	settingsrow_add(container, "listenbrainz_account", &account_value, account_cb, NULL);
	settingsrow_add(container, "listenbrainz_status", &status_value, NULL, NULL);
	settingsrow_add(container, "listenbrainz_queued", &queued_value, NULL, NULL);

	// A message from ListenBrainz can be longer than the room beside the row's
	// name: one line, cut with an ellipsis.
	lv_obj_set_style_max_width(status_value, lv_pct(60), 0);
	lv_obj_set_height(status_value, lv_font_get_line_height(&font_ui_24));
	lv_label_set_long_mode(status_value, LV_LABEL_LONG_DOT);

	note_label = lv_label_create(container);
	lv_label_set_long_mode(note_label, LV_LABEL_LONG_WRAP);
	lv_obj_set_width(note_label, lv_pct(100));
	lv_obj_add_style(note_label, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(note_label, &font_ui_22, 0);
	lv_label_set_text(note_label, tr("listenbrainz_note"));

	lv_obj_add_event_cb(page_screen, loaded_cb, LV_EVENT_SCREEN_LOADED, NULL);
	switcher_attach_back_gesture(page_screen);
}

// ---------------------------------------------------------------------------
// the token
// ---------------------------------------------------------------------------

static void token_accept_cb(lv_event_t *e) {
	(void)e;
	const char *tok = lv_textarea_get_text(token_field);
	if (!tok || !tok[0]) {
		toast_error("listenbrainz_token_required");
		return;
	}
	// Signing in is local: the token is stored and every request from now on
	// carries it. A token ListenBrainz will not take is found by the first
	// listen sent with it, not here.
	listenbrainz_signin(tok);
	lv_textarea_set_text(token_field, "");
	if (lv_screen_active() == login_screen) {
		back_btn_cb(NULL);
	}
	toast_success("signed_in");
	refresh();
}

static lv_obj_t *make_field(lv_obj_t *parent, gui_config_t *cfg, const char *placeholder, int y) {
	lv_obj_t *field = lv_textarea_create(parent);
	lv_textarea_set_one_line(field, true);
	lv_textarea_set_placeholder_text(field, tr(placeholder));
	lv_obj_set_size(field, cfg->screen_width - 2 * cfg->padding, cfg->screen_width < 320 ? 44 : 62);
	lv_obj_set_scrollbar_mode(field, LV_SCROLLBAR_MODE_OFF);
	lv_obj_align(field, LV_ALIGN_TOP_LEFT, cfg->padding, y);
	lv_obj_add_style(field, &theme_style_card, 0);
	lv_obj_set_style_radius(field, 12, 0);
	lv_obj_set_style_border_width(field, 0, 0);
	lv_obj_set_style_shadow_width(field, 0, 0);
	lv_obj_set_style_pad_all(field, cfg->screen_width < 320 ? 8 : 14, 0);
	lv_obj_set_style_text_font(field, &font_ui_24, 0);
	keyboard_style_caret(field);
	return field;
}

static void build_login(gui_config_t *cfg) {
	login_screen = lv_obj_create(NULL);
	lv_obj_add_style(login_screen, &theme_style_screen, 0);

	settingsrow_title(login_screen, cfg, "listenbrainz_sign_in");
	int top = settingsrow_content_top(cfg);

	token_field = make_field(login_screen, cfg, "listenbrainz_token", top);

	lv_obj_t *note = lv_label_create(login_screen);
	lv_label_set_long_mode(note, LV_LABEL_LONG_WRAP);
	lv_obj_set_width(note, cfg->screen_width - 2 * cfg->padding);
	lv_obj_align(note, LV_ALIGN_TOP_LEFT, cfg->padding, top + (cfg->screen_width < 320 ? 50 : 78));
	lv_obj_add_style(note, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(note, &font_ui_18, 0);
	lv_label_set_text(note, tr("listenbrainz_token_note"));
	lv_obj_set_hidden(note, cfg->screen_width < 320);

	login_keyboard = keyboard_create(login_screen, cfg->screen_width, cfg->screen_width < 320 ? 144 : 316, token_field, NULL, "ok", token_accept_cb, NULL);
	switcher_attach_back_gesture(login_screen);
}

// ---------------------------------------------------------------------------
// the timer: what is playing, and the page while it is on screen
// ---------------------------------------------------------------------------

static void tick_cb(lv_timer_t *t) {
	(void)t;
	if (listenbrainz_active()) {
		char file[512];
		audio_get_current_file(file, sizeof(file));

		listenbrainz_playback_t now = {0};
		now.file = file;
		now.playing = audio_get_status() == AUDIO_STATUS_PLAYING;
		audio_get_progress(&now.position, &now.duration);

		// Tags only once device_state has read them for this very file.
		const char *tags_file = NULL;
		const song_metadata_t *m = device_state_loaded_metadata(&tags_file);
		bool tags = file[0] && tags_file && strcmp(tags_file, file) == 0;
		now.artist = tags ? (m->artist[0] ? m->artist : m->album_artist) : "";
		now.title = tags ? m->title : "";
		now.album = tags ? m->album : "";

		now.skip = radio_is_active() || audiobook_is_playing() ||
				   (podcastcache_is_episode(file) || ytmusic_cache_is_podcast_episode(file));
		listenbrainz_note_playback(&now);
	}

	if (lv_screen_active() == page_screen) {
		refresh();
	}
}

void listenbrainzsettings_init(gui_config_t *cfg) {
	build_page(cfg);
	build_login(cfg);

	refresh();

	lv_timer_create(tick_cb, TICK_MS, NULL);
}
