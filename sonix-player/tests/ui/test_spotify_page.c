/*
 * Streaming -> Spotify, on the real renderer, at both panel sizes.
 *
 * The receiver itself is the card's business (docs/spotify.md) and the protocol
 * is checked in plain C elsewhere (tests/test_spotify.c). What is checked here
 * is the page: that the switch and every browser row are on the glass of a
 * panel, that a tap sends the request the row promises -- the op and every
 * argument, cursor included -- that a track plays through its list and a
 * queue row skips, that the settings page writes the player's own settings
 * and asks the receiver for the rest, and that Back walks up the lists it
 * descended instead of leaving the page.
 *
 * The service behind it is stubbed, the Q2SPOT/1 client along with it: the page
 * runs its worker thread against canned replies, which is the shape the real
 * one has (a reply object handed to the interface thread through a mailbox),
 * so what is exercised is the page's own plumbing and not a mock of itself.
 */
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "src/gui/board_profile.h"
#include "src/gui/fonts/fonts.h"
#include "src/gui/nowplaying/cover.h"
#include "src/gui/shell/gui.h"
#include "src/gui/shell/theme.h"
#include "src/gui/streaming/spotifypage.h"
#include "src/system/core/config.h"
#include "src/system/core/lang.h"
#include "src/system/net/wifi.h"
#include "src/system/streaming/spotify.h"

/* ---- the shell these pages lean on, substituted ------------------------- */

lv_obj_t *music_screen, *streaming_screen, *wireless_screen;
lv_obj_t *audiobooks_screen, *settings_screen, *player_screen;
lv_obj_t *browser_screen, *wifisettings_screen;

lv_font_t font_ui_14, font_ui_16, font_ui_18, font_ui_20, font_ui_20_bold;
lv_font_t font_ui_22, font_ui_22_bold, font_ui_24, font_ui_24_bold;
lv_font_t font_ui_26, font_ui_28, font_ui_32, font_ui_36_bold;

static void init_fonts(void) {
	lv_font_t *all[] = {
		&font_ui_14, &font_ui_16, &font_ui_18, &font_ui_20, &font_ui_20_bold, &font_ui_22, &font_ui_22_bold,
		&font_ui_24, &font_ui_24_bold, &font_ui_26, &font_ui_28, &font_ui_32, &font_ui_36_bold,
	};
	for (size_t i = 0; i < sizeof(all) / sizeof(all[0]); i++) {
		*all[i] = lv_font_montserrat_14;
	}
}

void fonts_register_change(void (*cb)(void)) { (void)cb; }
int fonts_text_size(void) { return FONTS_TEXT_NORMAL; }
bool fonts_set_text_size(int size) { (void)size; return true; }

void gui_notify_popup(const char *text) { (void)text; }
void switch_screen(lv_obj_t *screen) { lv_screen_load(screen); }
void switch_screen_return_to(lv_obj_t *screen) { lv_screen_load(screen); }
void switcher_attach_back_gesture(lv_obj_t *obj) { (void)obj; }
bool switcher_back_drag_active(void) { return false; }
void player_sheet_attach_drag(lv_obj_t *obj, bool opening) {
	(void)obj;
	(void)opening;
}

// The list screen's back guard: the page registers it, and Back is what the
// test presses. `peek` is what tells the shell there is somewhere to go.
static bool (*back_guard)(void);
static bool (*back_peek)(void);
void switcher_set_back_guard(lv_obj_t *screen, bool (*handler)(void)) {
	(void)screen;
	back_guard = handler;
}
void switcher_set_back_guard_peek(lv_obj_t *screen, bool (*peek)(void)) {
	(void)screen;
	back_peek = peek;
}

static const char *last_toast;
void toast_error(const char *text) { last_toast = text; }
void toast_plain(const char *text) { last_toast = text; }
void toast_success(const char *text) { last_toast = text; }
void toast_busy(const char *text) { last_toast = text; }
void toast_busy_end(void) { }

// The dialog is not what is under test; the action behind it is.
void confirm_show(const char *title, const char *message, const char *ok_label, void (*on_ok)(void *user), void *user) {
	(void)title;
	(void)message;
	(void)ok_label;
	on_ok(user);
}

// The keyboard is the real page's, minus its glass: it is built by two of the
// pages, and what the test needs of it is the accept callback, which it calls
// itself. `keyboard_accept()` is the enter key.
typedef struct keyboard_s {
	lv_obj_t *field;
	lv_event_cb_t on_accept;
	void *user;
} keyboard_t;

static keyboard_t keyboards[4];
static int keyboard_count;

keyboard_t *keyboard_create(lv_obj_t *parent, int width, int height, lv_obj_t *field, const lv_image_dsc_t *icon,
							const char *text, lv_event_cb_t on_accept, void *user) {
	(void)parent;
	(void)width;
	(void)height;
	(void)icon;
	(void)text;
	assert(keyboard_count < 4);
	keyboard_t *keyboard = &keyboards[keyboard_count++];
	keyboard->field = field;
	keyboard->on_accept = on_accept;
	keyboard->user = user;
	return keyboard;
}

// The enter key of whichever keyboard belongs to this field. The event is what
// the real keyboard would pass and every one of the page's callbacks ignores
// it, so the callback is asked directly.
static void keyboard_accept(lv_obj_t *field) {
	for (int i = 0; i < keyboard_count; i++) {
		if (keyboards[i].field == field) {
			assert(keyboards[i].on_accept);
			keyboards[i].on_accept(NULL);
			return;
		}
	}
	assert(0);
}
void keyboard_set_visible(keyboard_t *kb, bool visible) { (void)kb; (void)visible; }
void keyboard_reset(keyboard_t *kb) { (void)kb; }
void keyboard_style_caret(lv_obj_t *field) { (void)field; }

// The artwork loader: the page asks, and nothing ever comes back -- which is
// the same as a receiver whose cover file is not written yet.
void coverloader_request(int slot, const char *path, int size) {
	(void)slot;
	(void)path;
	(void)size;
}
bool coverloader_take(int slot, cover_image_t *out, bool *finished) {
	(void)slot;
	(void)out;
	*finished = false;
	return false;
}

void wifi_get_status(wifi_status_t *out) {
	memset(out, 0, sizeof(*out));
	out->state = WIFI_STATE_CONNECTED;
	snprintf(out->ip, sizeof(out->ip), "10.0.0.2");
}

void thread_be_low_priority(const char *name) { (void)name; }
void bootlogo_set(int choice, bool dark) { (void)choice; (void)dark; }
bool player_sheet_drag_active(void) { return false; }

/* ---- the service, substituted ------------------------------------------- */

static spotify_state_t state;
static bool available = true, running, enabled;
static pthread_mutex_t browse_lock = PTHREAD_MUTEX_INITIALIZER;
#define ANSWER_QUEUE 4
static spotify_reply_t answers[ANSWER_QUEUE];
static int answer_count;
static char browse_op[32];
static char browse_args[4][SPOTIFY_URI_MAX];
static int browse_nargs;
static int browse_calls;

void spotify_init(const char *sd_root) { (void)sd_root; }
bool spotify_available(void) { return available; }
const char *spotify_dir(void) { return "/tmp"; }
void spotify_set_enabled(bool on) { enabled = on; running = on; }
bool spotify_enabled(void) { return enabled; }
bool spotify_running(void) { return running; }
const char *spotify_status_text(void) { return enabled ? "running" : ""; }
void spotify_shutdown(void) { running = false; }
void spotify_get_state(spotify_state_t *out) { *out = state; }
void spotify_cover_path(char *out, size_t size) { snprintf(out, size, "/tmp/cover.jpg"); }
bool spotify_owns_playback(void) { return false; }
void spotify_forget_active(void) { }
int spotify_silent_ms(void) { return -1; }
static int last_bitrate, last_cache_mb, last_idle_secs;
static char last_name[64];
void spotify_control(spotify_command_t command) { (void)command; }
void spotify_seek_ms(int ms) { (void)ms; }
bool spotify_browse_busy(void) { return false; }
static bool page_open_state;
void spotify_page_open(bool open) { page_open_state = open; }
void spotify_activity(void) { }
void spotify_log_tail(char *out, size_t size) {
	if (size) {
		out[0] = '\0';
	}
}
int spotify_bitrate(void) { return 160; }
void spotify_set_bitrate(int kbps) { last_bitrate = kbps; }
const char *spotify_receiver_name(void) { return "Test V1"; }
void spotify_set_receiver_name(const char *name) { snprintf(last_name, sizeof(last_name), "%s", name); }
int spotify_cache_mb(void) { return 1024; }
void spotify_set_cache_mb(int mb) { last_cache_mb = mb; }
int spotify_idle_stop_secs(void) { return 0; }
void spotify_set_idle_stop_secs(int seconds) { last_idle_secs = seconds; }

bool spotify_browse(const char *op, const char *const *args, int nargs, spotify_reply_t *out) {
	pthread_mutex_lock(&browse_lock);
	snprintf(browse_op, sizeof(browse_op), "%s", op);
	browse_nargs = nargs;
	for (int i = 0; i < nargs && i < 4; i++) {
		snprintf(browse_args[i], sizeof(browse_args[i]), "%s", args[i]);
	}
	browse_calls++;
	if (answer_count > 0) {
		*out = answers[0];
		for (int i = 0; i + 1 < answer_count; i++) {
			answers[i] = answers[i + 1];
		}
		answer_count--;
	} else {
		memset(out, 0, sizeof(*out));
		out->status = SPOTIFY_REPLY_OK;
	}
	bool ok = out->status == SPOTIFY_REPLY_OK;
	pthread_mutex_unlock(&browse_lock);
	return ok;
}

/* ---- helpers ------------------------------------------------------------ */

static lv_obj_t *find_label(lv_obj_t *obj, const char *text) {
	uint32_t count = lv_obj_get_child_count(obj);
	for (uint32_t i = 0; i < count; i++) {
		lv_obj_t *child = lv_obj_get_child(obj, i);
		if (lv_obj_check_type(child, &lv_label_class) && strcmp(lv_label_get_text(child), text) == 0) {
			return child;
		}
		lv_obj_t *deeper = find_label(child, text);
		if (deeper) {
			return deeper;
		}
	}
	return NULL;
}

// The field a page's keyboard is pointed at: both typed pages have exactly one.
static lv_obj_t *find_textarea(lv_obj_t *obj) {
	uint32_t count = lv_obj_get_child_count(obj);
	for (uint32_t i = 0; i < count; i++) {
		lv_obj_t *child = lv_obj_get_child(obj, i);
		if (lv_obj_check_type(child, &lv_textarea_class)) {
			return child;
		}
		lv_obj_t *deeper = find_textarea(child);
		if (deeper) {
			return deeper;
		}
	}
	return NULL;
}

// A settings row is the parent of its name label; a page's own row is what a
// tap has to land on.
static lv_obj_t *row_with_label(lv_obj_t *container, const char *text) {
	lv_obj_t *label = find_label(container, text);
	bool inside_row = false;
	for (lv_obj_t *parent = label ? lv_obj_get_parent(label) : NULL; parent; parent = lv_obj_get_parent(parent)) {
		if (lv_obj_check_type(parent, &lv_button_class)) {
			inside_row = true;
			break;
		}
	}
	return inside_row ? lv_obj_get_parent(label) : NULL;
}

static void tap(lv_obj_t *row) {
	assert(row);
	lv_obj_send_event(row, LV_EVENT_CLICKED, NULL);
}

// Runs the page's poll timer and lets the worker's answer arrive. The clock
// step is longer than the page's 400 ms poll, so every pass fires it.
static void pump(int ticks) {
	for (int i = 0; i < ticks; i++) {
		lv_tick_inc(600);
		lv_timer_handler();
		usleep(30 * 1000);
	}
}

// One queued answer, ready for the next request the worker makes.
static spotify_reply_t *answer_push(void) {
	pthread_mutex_lock(&browse_lock);
	assert(answer_count < ANSWER_QUEUE);
	spotify_reply_t *reply = &answers[answer_count++];
	memset(reply, 0, sizeof(*reply));
	reply->status = SPOTIFY_REPLY_OK;
	pthread_mutex_unlock(&browse_lock);
	return reply;
}

// One canned page. The rows are the caller's shape: a list of playlists, a
// list of tracks inside an album, or the queue. Rows answer in the order they
// were queued, because the page asks one question at a time and the answers
// do not arrive together.
static void answer_page(const char *cursor, int n, spotify_row_kind_t kind, const char *title_prefix,
						const char *uri_prefix, const char *context) {
	spotify_reply_t *reply = answer_push();
	snprintf(reply->cursor, sizeof(reply->cursor), "%s", cursor);
	for (int i = 0; i < n; i++) {
		spotify_row_t *row = &reply->rows[i];
		row->kind = kind;
		snprintf(row->uri, sizeof(row->uri), "%s%d", uri_prefix, i);
		snprintf(row->title, sizeof(row->title), "%s %d", title_prefix, i);
		snprintf(row->subtitle, sizeof(row->subtitle), "second line %d", i);
		snprintf(row->context, sizeof(row->context), "%s", context);
		snprintf(row->position, sizeof(row->position), "%d", i);
	}
	reply->count = n;
}

// The `settings` and `account` answers, which are rows of the protocol's own
// two kinds and not a page of tracks.
static void answer_settings(const char *shuffle, const char *repeat) {
	spotify_reply_t *reply = answer_push();
	reply->count = 2;
	reply->rows[0].kind = SPOTIFY_ROW_SETTING;
	snprintf(reply->rows[0].uri, sizeof(reply->rows[0].uri), "shuffle");
	snprintf(reply->rows[0].title, sizeof(reply->rows[0].title), "%s", shuffle);
	reply->rows[1].kind = SPOTIFY_ROW_SETTING;
	snprintf(reply->rows[1].uri, sizeof(reply->rows[1].uri), "repeat");
	snprintf(reply->rows[1].title, sizeof(reply->rows[1].title), "%s", repeat);
}

static void answer_account(const char *who) {
	spotify_reply_t *reply = answer_push();
	reply->count = 1;
	reply->rows[0].kind = SPOTIFY_ROW_ACCOUNT;
	snprintf(reply->rows[0].title, sizeof(reply->rows[0].title), "%s", who);
}

// Nothing drawn, anywhere on a page that does not scroll, may leave the panel.
static void check_within_panel(lv_obj_t *obj, gui_config_t *cfg, const char *what) {
	if (lv_obj_is_hidden(obj)) {
		return;
	}
	lv_area_t area;
	lv_obj_get_coords(obj, &area);
	if (area.x1 < 0 || area.y1 < 0 || area.x2 >= (int32_t)cfg->screen_width ||
		area.y2 >= (int32_t)cfg->screen_height) {
		printf("FAIL %s: %d,%d..%d,%d is outside %dx%d\n", what, (int)area.x1, (int)area.y1, (int)area.x2,
			   (int)area.y2, cfg->screen_width, cfg->screen_height);
		fflush(stdout);
		assert(0);
	}
}

// The page's own children: the header, the switch, the rows. The scrolling
// list holds more than the panel can show and is checked row by row instead.
static void check_page_within(lv_obj_t *screen, gui_config_t *cfg, const char *what) {
	uint32_t count = lv_obj_get_child_count(screen);
	for (uint32_t i = 0; i < count; i++) {
		lv_obj_t *child = lv_obj_get_child(screen, i);
		if (lv_obj_is_scrollable(child)) {
			continue;
		}
		check_within_panel(child, cfg, what);
	}
}

int main(int argc, char **argv) {
	setvbuf(stdout, NULL, _IONBF, 0);
	assert(argc == 2);

	char card[] = "/tmp/sonix-spotify-page-XXXXXX";
	assert(mkdtemp(card));
	char config[512];
	snprintf(config, sizeof(config), "%s/device_config.ini", card);
	config_init(config);
	config_set("ui", "language", "English");
	lang_init(argv[1]);

	init_fonts();
	lv_init();
	gui_config_t cfg = {bp_screen_w(), bp_screen_h(), bp_padding(), bp_status_bar_h(), card};
	lv_display_create(cfg.screen_width, cfg.screen_height);
	theme_init();

	music_screen = lv_obj_create(NULL);
	streaming_screen = lv_obj_create(NULL);
	wireless_screen = lv_obj_create(NULL);
	audiobooks_screen = lv_obj_create(NULL);
	settings_screen = lv_obj_create(NULL);
	player_screen = lv_obj_create(NULL);
	browser_screen = lv_obj_create(NULL);
	wifisettings_screen = lv_obj_create(NULL);

	const char *board = bp_is_tempotec_v1() ? "v1" : "hiby";

	// No receiver on the card: the page says so instead of offering a switch
	// that cannot work.
	available = false;
	enabled = running = false;
	spotifypage_init(&cfg);
	assert(spotify_screen);
	// LVGL sends the page its own SCREEN_LOADED as the screen goes up, which is
	// what stops the polling; sending one here as well would count the page as
	// being up twice and it would never hand the receiver back.
	switch_screen(spotify_screen);
	pump(2);
	check_page_within(spotify_screen, &cfg, "spotify, no receiver");
	assert(find_label(spotify_screen, tr("spotify_no_receiver")));

	// With one, switched off: the switch is there, the rows say what is
	// missing rather than opening an empty list.
	available = true;
	lv_obj_send_event(spotify_screen, LV_EVENT_REFRESH, NULL);
	pump(2);
	check_page_within(spotify_screen, &cfg, "spotify, off");
	assert(find_label(spotify_screen, tr("spotify_off_note")));

	lv_obj_t *liked = row_with_label(spotify_screen, tr("spotify_liked"));
	assert(liked);
	browse_calls = 0;
	tap(liked);
	pump(2);
	assert(browse_calls == 0); // nothing was sent to a receiver that is off
	assert(last_toast == tr("spotify_receiver_off"));

	// Switched on and playing: the page follows the receiver's status file.
	enabled = running = true;
	lv_obj_send_event(spotify_screen, LV_EVENT_REFRESH, NULL);
	memset(&state, 0, sizeof(state));
	state.state = SPOTIFY_PLAY_PLAYING;
	snprintf(state.title, sizeof(state.title), "A track");
	snprintf(state.artist, sizeof(state.artist), "An artist");
	snprintf(state.album, sizeof(state.album), "An album");
	state.duration_ms = 215000;
	state.position_ms = 65000;
	pump(2);
	check_page_within(spotify_screen, &cfg, "spotify, playing");
	assert(find_label(spotify_screen, "A track"));
	assert(find_label(spotify_screen, "An artist"));
	assert(find_label(spotify_screen, "An album"));

	// The browser: a row asks for its list, the answer fills the list screen.
	browse_calls = 0;
	browse_nargs = -1;
	answer_page("", 6, SPOTIFY_ROW_PLAYLIST, "list", "spotify:playlist:", "");
	tap(liked);
	pump(4);
	assert(browse_calls == 1);
	assert(strcmp(browse_op, "liked") == 0);
	assert(browse_nargs == 0); // no cursor: the first page
	lv_obj_t *list_screen = lv_screen_active();
	assert(list_screen != spotify_screen);
	assert(find_label(list_screen, "list 0"));
	assert(find_label(list_screen, "second line 0"));
	check_page_within(list_screen, &cfg, "spotify, liked songs");

	// A row that is a list of its own: its URI goes out on its own, and the
	// answer brings a second page with it -- six rows at a time, forty-seven
	// more in the list.
	browse_calls = 0;
	answer_page("47", 3, SPOTIFY_ROW_TRACK, "track", "spotify:track:", "spotify:playlist:0");
	tap(row_with_label(list_screen, "list 0"));
	pump(4);
	assert(browse_calls == 1);
	assert(strcmp(browse_op, "playlist") == 0);
	assert(browse_nargs == 1);
	assert(strcmp(browse_args[0], "spotify:playlist:0") == 0);
	assert(find_label(lv_screen_active(), "track 0"));

	// A track row plays through its list: `play <context> <position> <uri>`,
	// so the next track is the next track of the album rather than of nothing.
	browse_calls = 0;
	tap(row_with_label(lv_screen_active(), "track 1"));
	pump(2);
	assert(browse_calls == 1);
	assert(strcmp(browse_op, "play") == 0);
	assert(browse_nargs == 3);
	assert(strcmp(browse_args[0], "spotify:playlist:0") == 0);
	assert(strcmp(browse_args[1], "1") == 0);
	assert(strcmp(browse_args[2], "spotify:track:1") == 0);

	// The next page: the cursor the receiver answered with is sent as the last
	// argument of the same op, and the row that goes back appears.
	browse_calls = 0;
	browse_nargs = -1;
	answer_page("", 2, SPOTIFY_ROW_TRACK, "later", "spotify:track:", "spotify:playlist:0");
	lv_obj_t *next = row_with_label(lv_screen_active(), tr("spotify_next_page"));
	assert(next);
	tap(next);
	pump(4);
	assert(browse_calls == 1);
	assert(strcmp(browse_op, "playlist") == 0);
	assert(browse_nargs == 2);
	assert(strcmp(browse_args[1], "47") == 0);
	assert(find_label(lv_screen_active(), "later 0"));

	// And back to the first page: the same op without a cursor.
	browse_calls = 0;
	browse_nargs = -1;
	answer_page("47", 3, SPOTIFY_ROW_TRACK, "track", "spotify:track:", "spotify:playlist:0");
	lv_obj_t *back = row_with_label(lv_screen_active(), tr("spotify_previous_page"));
	assert(back);
	tap(back);
	pump(4);
	assert(browse_calls == 1);
	assert(strcmp(browse_op, "playlist") == 0);
	assert(browse_nargs == 1);

	// Back is a guard rather than a page: it walks up the lists and says so,
	// and only at the top does it let the shell leave the page.
	assert(back_guard && back_peek);
	assert(back_peek());
	browse_calls = 0;
	answer_page("", 6, SPOTIFY_ROW_PLAYLIST, "list", "spotify:playlist:", "");
	assert(back_guard());
	pump(4);
	assert(browse_calls == 1);
	assert(strcmp(browse_op, "liked") == 0);

	// The queue is the phone's own list, so a tap there is a skip rather than
	// a play: the context is already the queue and its positions are its own.
	// LVGL sends the page its own SCREEN_LOADED as the screen goes up, which is
	// what stops the polling; sending one here as well would count the page as
	// being up twice and it would never hand the receiver back.
	switch_screen(spotify_screen);
	pump(2);
	answer_page("", 3, SPOTIFY_ROW_TRACK, "queued", "spotify:track:", "");
	browse_calls = 0;
	tap(row_with_label(spotify_screen, tr("spotify_queue")));
	pump(4);
	assert(browse_calls == 1);
	assert(strcmp(browse_op, "queue") == 0);
	assert(browse_nargs == 0);
	tap(row_with_label(lv_screen_active(), "queued 2"));
	pump(2);
	assert(browse_calls == 2);
	assert(strcmp(browse_op, "skip") == 0);
	assert(browse_nargs == 2);
	assert(strcmp(browse_args[0], "2") == 0);
	assert(strcmp(browse_args[1], "spotify:track:2") == 0);

	// The settings page: its own rows and pills, all of them on the glass.
	// The settings page: its own rows and pills, all of them on the glass,
	// and the receiver's shuffle, repeat and account asked for on the way in
	// -- one question after the other, because there is one mailbox.
	answer_settings("on", "track");
	answer_account("listener@example.com");
	lv_obj_t *settings = row_with_label(spotify_screen, tr("spotify_receiver_settings"));
	assert(settings);
	browse_calls = 0;
	tap(settings);
	pump(6);
	lv_obj_t *now = lv_screen_active();
	assert(find_label(now, tr("spotify_bitrate")));
	assert(find_label(now, tr("spotify_cache")));
	assert(find_label(now, tr("spotify_sign_out")));
	check_page_within(now, &cfg, "spotify, settings");
	assert(browse_calls == 2);
	assert(strcmp(browse_op, "account") == 0);
	assert(find_label(now, tr("on")));
	assert(find_label(now, tr("spotify_repeat_track")));
	assert(find_label(now, "listener@example.com"));

	// Shuffle and repeat are the receiver's, not the page's: the tap says what
	// it should be, and the label follows.
	browse_calls = 0;
	tap(row_with_label(now, tr("spotify_shuffle")));
	pump(2);
	assert(strcmp(browse_op, "shuffle") == 0);
	assert(browse_nargs == 1);
	assert(strcmp(browse_args[0], "off") == 0); // it was showing "on"
	assert(find_label(now, tr("off")));
	browse_calls = 0;
	tap(row_with_label(now, tr("spotify_repeat")));
	pump(2);
	assert(strcmp(browse_op, "repeat") == 0);
	assert(strcmp(browse_args[0], "off") == 0); // and this one "one track"

	// The three pills that are the player's own settings rather than the
	// receiver's: they are written to the config and nothing is sent.
	browse_calls = 0;
	last_bitrate = last_cache_mb = last_idle_secs = -1;
	tap(row_with_label(now, "320"));
	tap(row_with_label(now, "1 GB"));
	tap(row_with_label(now, "15 min"));
	pump(1);
	assert(last_bitrate == 320);
	assert(last_cache_mb == 1024);
	assert(last_idle_secs == 900);
	assert(browse_calls == 0);

	// The name the phone shows: the keyboard page, and what leaving it writes.
	tap(row_with_label(now, tr("spotify_receiver_name")));
	pump(2);
	assert(lv_screen_active() != now);
	lv_obj_t *name_field = find_textarea(lv_screen_active());
	assert(name_field);
	assert(strcmp(lv_textarea_get_text(name_field), "Test V1") == 0); // what it is now
	lv_textarea_set_text(name_field, "V1 in the kitchen");
	keyboard_accept(name_field);
	pump(2);
	assert(strcmp(last_name, "V1 in the kitchen") == 0);
	assert(lv_screen_active() == now);

	// Signing out is behind a confirmation, and it says what it did.
	last_toast = NULL;
	browse_calls = 0;
	tap(row_with_label(now, tr("spotify_sign_out")));
	pump(3);
	assert(browse_calls == 1);
	assert(strcmp(browse_op, "signout") == 0);
	assert(last_toast && strcmp(last_toast, "spotify_signed_out") == 0);

	// Search: the four categories the protocol accepts, and a query long
	// enough to be a query.
	// LVGL sends the page its own SCREEN_LOADED as the screen goes up, which is
	// what stops the polling; sending one here as well would count the page as
	// being up twice and it would never hand the receiver back.
	switch_screen(spotify_screen);
	pump(2);
	lv_obj_t *search = row_with_label(spotify_screen, tr("spotify_search"));
	assert(search);
	tap(search);
	pump(2);
	now = lv_screen_active();
	assert(find_label(now, tr("spotify_search_in")));
	assert(find_label(now, tr("spotify_tracks")));
	check_page_within(now, &cfg, "spotify, search");

	// The category is one of the four the protocol accepts, the query is the
	// keyboard's, and the op carries both.
	tap(row_with_label(now, tr("spotify_albums")));
	pump(1);
	lv_obj_t *term = find_textarea(now);
	assert(term);
	lv_textarea_set_text(term, "blue train");
	answer_page("", 2, SPOTIFY_ROW_ALBUM, "found", "spotify:album:", "");
	browse_calls = 0;
	keyboard_accept(term);
	pump(4);
	assert(browse_calls == 1);
	assert(strcmp(browse_op, "search") == 0);
	assert(browse_nargs == 2);
	assert(strcmp(browse_args[0], "albums") == 0);
	assert(strcmp(browse_args[1], "blue train") == 0);
	assert(find_label(lv_screen_active(), "found 0"));

	// Leaving the page entirely: nobody is looking any more, which is what
	// lets a receiver that has sat silent for long enough stop itself.
	switch_screen(music_screen);
	pump(1);
	assert(!page_open_state);

	lv_deinit();
	printf("PASS spotify page (%s)\n", board);
	return 0;
}
