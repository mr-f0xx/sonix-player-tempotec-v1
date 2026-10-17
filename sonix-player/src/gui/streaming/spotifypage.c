#include "spotifypage.h"

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lvgl/lvgl.h"

#include "src/gui/fonts/fonts.h"
#include "src/gui/nowplaying/coverloader.h"
#include "src/gui/nowplaying/player.h"
#include "src/gui/shell/confirm.h"
#include "src/gui/shell/icons.h"
#include "src/gui/shell/keyboard.h"
#include "src/gui/shell/scrolltext.h"
#include "src/gui/shell/settingsrow.h"
#include "src/gui/shell/switcher.h"
#include "src/gui/shell/theme.h"
#include "src/gui/shell/toast.h"
#include "src/gui/wireless/wifisettings.h"
#include "src/system/core/lang.h"
#include "src/system/core/utils.h"
#include "src/system/net/wifi.h"
#include "src/system/streaming/spotify.h"

lv_obj_t *spotify_screen;

static gui_config_t *config;
static lv_timer_t *poll_timer;

// The switch, and what is playing under it -- the AirPlay page's shape, for
// the same reason: the receiver is not tied to this page, so the page reports
// rather than owns.
static lv_obj_t *toggle;
static lv_obj_t *glyph;
static lv_obj_t *cover_img;
static lv_obj_t *title_label;
static lv_obj_t *artist_label;
static lv_obj_t *album_label;
static lv_obj_t *status_label;
static lv_obj_t *wifi_row;

static lv_obj_t *list_screen, *list_container, *list_title;

static lv_obj_t *settings_screen, *name_screen, *search_screen;
static lv_obj_t *account_value, *shuffle_value, *repeat_value, *name_value;
static lv_obj_t *bitrate_pills[3], *cache_pills[3], *idle_pills[4];
static lv_obj_t *name_field, *search_field, *search_pills[4];
static keyboard_t *name_keyboard, *search_keyboard;
static int search_category;

#define SP_POLL_MS 400
#define SP_COVER_PX 96

// ---------------------------------------------------------------------------
// the worker: one job in flight, one result box
//
// A request on this protocol may take up to fifteen seconds to answer, so none
// of it can happen on the interface thread -- LVGL would be inside a socket
// call with the page frozen. One job at a time and one mailbox each way, the
// same shape the Subsonic page settled on: a newer request abandons the older
// one, and a late answer is dropped rather than drawn under the wrong heading.
// ---------------------------------------------------------------------------

typedef enum {
	SP_JOB_LIST,	// a page of rows, into the list screen
	SP_JOB_CONTROL, // the settings page's own state: shuffle, repeat, account
	SP_JOB_ACTION,	// play, enqueue, sign out: only a failure is worth saying
} sp_job_kind_t;

// The home page's browser rows.
typedef enum {
	SP_BROWSE_LIKED,
	SP_BROWSE_PLAYLISTS,
	SP_BROWSE_ALBUMS,
	SP_BROWSE_ARTISTS,
	SP_BROWSE_QUEUE,
	SP_BROWSE_DOWNLOADED,
} sp_browse_t;

typedef struct {
	unsigned serial;
	sp_job_kind_t kind;
	char op[16];
	char args[3][SPOTIFY_URI_MAX];
	int nargs;
	// The protocol's cursor: empty for the first page of a list, and the
	// offset the receiver answered with for the pages after it. It is sent as
	// the last argument of the paged ops.
	char cursor[SPOTIFY_CURSOR_MAX];
	char title[96]; // the list screen's heading, a translation key
	char note[64];	// the toast an action that worked is worth, "" for none
} sp_job_t;

typedef struct {
	sp_job_t job;
	spotify_reply_t *reply; // owned by the interface thread once posted
} sp_result_t;

static pthread_mutex_t job_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t job_ready = PTHREAD_COND_INITIALIZER;
static bool worker_started, have_job;
static unsigned serial;
static sp_job_t pending;
static sp_result_t *mail;

// One page of the protocol's rows, allocated when the browser is first opened
// and kept: the receiver had to build the same page to answer, and a malloc of
// this size on every scroll would be the largest allocation in the player.
static spotify_reply_t *shown;
static sp_job_t current_list;

// Going back up the browser: the lists walked to get here, so Back leaves the
// page rather than the list, and the pages already read, so a list longer than
// one page can be walked both ways.
#define HISTORY_DEPTH 12
static sp_job_t history[HISTORY_DEPTH];
static int history_count;
#define CURSOR_DEPTH 8
static char cursors[CURSOR_DEPTH][SPOTIFY_CURSOR_MAX];
static int cursor_count;

static void free_result(sp_result_t *result) {
	if (result) {
		free(result->reply);
		free(result);
	}
}

static void *worker_main(void *arg) {
	(void)arg;
	thread_be_low_priority("spotify browse");

	spotify_reply_t *reply = calloc(1, sizeof(*reply));
	if (!reply) {
		return NULL;
	}

	for (;;) {
		pthread_mutex_lock(&job_lock);
		while (!have_job) {
			pthread_cond_wait(&job_ready, &job_lock);
		}
		sp_job_t job = pending;
		memset(&pending, 0, sizeof(pending));
		have_job = false;
		pthread_mutex_unlock(&job_lock);

		memset(reply, 0, sizeof(*reply));

		const char *argv[4]; // three arguments and the cursor
		int argc = 0;
		for (int i = 0; i < job.nargs; i++) {
			argv[argc++] = job.args[i];
		}
		if (job.cursor[0]) {
			argv[argc++] = job.cursor;
		}
		spotify_browse(job.op, argv, argc, reply);

		sp_result_t *result = calloc(1, sizeof(*result));
		if (!result) {
			continue;
		}
		result->job = job;
		result->reply = reply;
		pthread_mutex_lock(&job_lock);
		free_result(mail);
		mail = result;
		pthread_mutex_unlock(&job_lock);

		// The interface thread owns that buffer now: this thread takes a new
		// one so the two never read and write the same page.
		reply = calloc(1, sizeof(*reply));
		if (!reply) {
			return NULL;
		}
	}
	return NULL;
}

static bool worker_start(void) {
	if (worker_started) {
		return true;
	}
	pthread_t worker;
	pthread_attr_t attr;
	int err = pthread_attr_init(&attr);
	if (!err) {
		err = pthread_attr_setstacksize(&attr, 256 * 1024);
		if (!err) {
			err = pthread_create(&worker, &attr, worker_main, NULL);
		}
		pthread_attr_destroy(&attr);
	}
	if (err) {
		return false;
	}
	pthread_detach(worker);
	worker_started = true;
	return true;
}

// One short line for a toast, from whatever the service can say about itself.
// The status text is already translated or, when the receiver is simply off,
// empty -- and an empty toast is a card with nothing on it.
static const char *status_message(void) {
	const char *text = spotify_status_text();
	return text[0] ? text : tr("spotify_receiver_off");
}

// Posts a job. False when there was nothing to send to: the receiver is down,
// or the worker could not be started. A caller that already told the user what
// it was doing does nothing else with the answer.
static bool submit(sp_job_t *job) {
	if (!spotify_running()) {
		toast_error(status_message());
		return false;
	}
	if (!worker_start()) {
		toast_error("spotify_out_of_memory");
		return false;
	}

	pthread_mutex_lock(&job_lock);
	job->serial = ++serial;
	pending = *job;
	have_job = true;
	pthread_cond_signal(&job_ready);
	pthread_mutex_unlock(&job_lock);
	return true;
}

// Forgets whatever is in flight: an answer to it must not rebuild a list the
// user has left or put a toast over another page.
static void drop_job(void) {
	pthread_mutex_lock(&job_lock);
	serial++;
	have_job = false;
	memset(&pending, 0, sizeof(pending));
	free_result(mail);
	mail = NULL;
	pthread_mutex_unlock(&job_lock);
	toast_busy_end();
}

// The name of a status, as a language key. The receiver's own English line
// would be the wrong thing to put in a German interface, and every one of
// these has a sentence of ours behind it.
static const char *status_key(spotify_reply_status_t status) {
	switch (status) {
	case SPOTIFY_REPLY_OK:
		return "";
	case SPOTIFY_REPLY_LOGIN:
		return "spotify_status_login";
	case SPOTIFY_REPLY_OFFLINE:
		return "spotify_status_offline";
	case SPOTIFY_REPLY_PREMIUM:
		return "spotify_status_premium";
	case SPOTIFY_REPLY_REFUSED:
		return "spotify_status_refused";
	case SPOTIFY_REPLY_UNAVAILABLE:
		return "spotify_status_unavailable";
	case SPOTIFY_REPLY_STALE:
		return "spotify_status_stale";
	case SPOTIFY_REPLY_THROTTLED:
		return "spotify_status_throttled";
	case SPOTIFY_REPLY_UNSUPPORTED:
		return "spotify_status_unsupported";
	case SPOTIFY_REPLY_BAD:
		return "spotify_status_bad";
	case SPOTIFY_REPLY_MALFORMED:
	case SPOTIFY_REPLY_NOSOCKET:
	case SPOTIFY_REPLY_TIMEOUT:
		return "spotify_status_no_answer";
	default:
		return "spotify_status_error";
	}
}

// ---------------------------------------------------------------------------
// the status line
// ---------------------------------------------------------------------------

static bool wifi_is_connected(void) {
	wifi_status_t status;
	wifi_get_status(&status);
	return status.state == WIFI_STATE_CONNECTED && status.ip[0] != '\0';
}

static void status_refresh(const spotify_state_t *state) {
	if (!spotify_available()) {
		lv_label_set_text(status_label, tr("spotify_no_receiver"));
		return;
	}
	if (!spotify_running()) {
		lv_label_set_text(status_label, spotify_enabled() ? tr("turning_on") : tr("spotify_off_note"));
		return;
	}

	switch (state->state) {
	case SPOTIFY_PLAY_PLAYING: {
		int at = state->position_ms / 1000;
		int of = state->duration_ms / 1000;
		if (of > 0) {
			lv_label_set_text_fmt(status_label, "%s  %d:%02d / %d:%02d", tr("spotify_playing"), at / 60, at % 60, of / 60,
								  of % 60);
		} else {
			lv_label_set_text(status_label, tr("spotify_playing"));
		}
		break;
	}
	case SPOTIFY_PLAY_PAUSED:
		lv_label_set_text(status_label, tr("spotify_paused"));
		break;
	case SPOTIFY_PLAY_STOPPED:
		lv_label_set_text(status_label, tr("spotify_stopped"));
		break;
	default:
		// No Connect session: nothing has been cast here yet. What to do about
		// it is to pick the device on the phone, so say which device.
		lv_label_set_text_fmt(status_label, "%s\n\xC2\xAB%s\xC2\xBB", tr("spotify_waiting"), spotify_receiver_name());
		break;
	}
}

// A metadata line: set with its text, or taken out of the layout entirely.
static void set_line(lv_obj_t *label, const char *text) {
	if (text[0] == '\0') {
		lv_obj_set_hidden(label, true);
		return;
	}
	scrolltext_set(label, text);
	lv_obj_set_hidden(label, false);
}

// ---------------------------------------------------------------------------
// the list screen
// ---------------------------------------------------------------------------

static void run_list(sp_job_t job, bool push_history);
static void search_open_cb(lv_event_t *e);
static void settings_open_cb(lv_event_t *e);

// The home page's browser rows: the op each one sends, its label, its glyph.
// One table, read by both the builder and the tap handler, so a row and the
// request it makes cannot drift apart.
typedef struct {
	const char *label;
	const char *op;
	const lv_image_dsc_t *icon;
	theme_semantic_t tone;
} sp_row_t;

static const sp_row_t BROWSER[] = {
	{"spotify_liked", "liked", &icon_heart_filled, THEME_SEMANTIC_RED},
	{"spotify_playlists", "playlists", &icon_menu_playlist, THEME_SEMANTIC_PURPLE},
	{"spotify_albums", "albums", &icon_menu_album, THEME_SEMANTIC_CYAN},
	{"spotify_artists", "artists", &icon_menu_artist, THEME_SEMANTIC_GREEN},
	{"spotify_queue", "queue", &icon_list_queue, THEME_SEMANTIC_AMBER},
	{"spotify_downloaded", "downloaded", &icon_podcast_downloaded, THEME_SEMANTIC_MUTED},
};

static const sp_row_t ACTIONS[] = {
	{"spotify_receiver_settings", "settings", &icon_music_settings, THEME_SEMANTIC_MUTED},
};

static void rebuild_list(void);
static void list_show(const sp_job_t *job);

// A tap on one of the two tables above. The indices are the tables laid end to
// end -- the browser rows first, then the actions -- so one number says which
// row of which table was pressed and the two cannot drift apart.
static void browse_cb(lv_event_t *e) {
	if (switcher_back_drag_active()) {
		return;
	}
	unsigned int index = (unsigned int)(uintptr_t)lv_event_get_user_data(e);
	unsigned int browsers = sizeof(BROWSER) / sizeof(BROWSER[0]);

	if (index >= browsers) {
		settings_open_cb(NULL);
		return;
	}
	sp_job_t job = {.kind = SP_JOB_LIST};
	snprintf(job.op, sizeof(job.op), "%s", BROWSER[index].op);
	snprintf(job.title, sizeof(job.title), "%s", BROWSER[index].label);
	run_list(job, false);
}

static bool list_back_has_step(void) { return history_count > 0; }

static bool list_back(void) {
	if (history_count > 0) {
		sp_job_t job = history[--history_count];
		list_show(&job);
		drop_job();
		if (submit(&job)) {
			toast_busy("loading");
		}
		return true;
	}
	drop_job();
	return false;
}

// Puts the list screen up on `job` and asks for its page. The heading is set
// before the answer arrives, and the rows of the previous list are cleared:
// showing them under the new heading reads as the wrong page.
static void list_show(const sp_job_t *job) {
	current_list = *job;
	cursor_count = 0;
	lv_obj_clean(list_container);
	lv_label_set_text(list_title, tr(job->title));
	if (lv_screen_active() != list_screen) {
		switch_screen(list_screen);
	}
}

static void run_list(sp_job_t job, bool push_history) {
	if (!spotify_running()) {
		toast_error(status_message());
		return;
	}
	if (push_history && lv_screen_active() == list_screen && history_count < HISTORY_DEPTH) {
		history[history_count++] = current_list;
	} else if (lv_screen_active() != list_screen) {
		history_count = 0;
	}

	list_show(&job);
	if (submit(&job)) {
		toast_busy("loading");
	}
}

static void page_cb(lv_event_t *e) {
	int step = (int)(intptr_t)lv_event_get_user_data(e);
	sp_job_t job = current_list;

	if (step > 0) {
		if (!shown || !shown->cursor[0]) {
			return;
		}
		// Where to come back to, and where to go.
		if (cursor_count < CURSOR_DEPTH) {
			snprintf(cursors[cursor_count++], sizeof(cursors[0]), "%s", job.cursor);
		}
		snprintf(job.cursor, sizeof(job.cursor), "%s", shown->cursor);
	} else {
		if (cursor_count == 0) {
			return;
		}
		snprintf(job.cursor, sizeof(job.cursor), "%s", cursors[--cursor_count]);
	}

	current_list = job;
	lv_obj_clean(list_container);
	if (submit(&job)) {
		toast_busy("loading");
	}
}

static const lv_image_dsc_t *row_icon(spotify_row_kind_t kind) {
	switch (kind) {
	case SPOTIFY_ROW_ALBUM:
		return &icon_album;
	case SPOTIFY_ROW_ARTIST:
		return &icon_artist;
	case SPOTIFY_ROW_PLAYLIST:
		return &icon_menu_playlist;
	case SPOTIFY_ROW_ACCOUNT:
		return &icon_menu_artist;
	case SPOTIFY_ROW_SETTING:
		return &icon_music_settings;
	default:
		return &icon_music_note;
	}
}

// A tap on a track. `play <context> <position> <uri>` starts it inside the
// list it came from -- so the next track is the next track of that album, and
// not of nothing -- and a row standing on its own is played by URI.
// A tap on a queue row is a `skip` instead: the phone's queue has positions,
// not a context, and the row carries its place in it.
static void row_cb(lv_event_t *e) {
	if (switcher_back_drag_active()) {
		return;
	}
	int index = (int)(intptr_t)lv_event_get_user_data(e);
	if (!shown || index < 0 || index >= shown->count) {
		return;
	}
	const spotify_row_t *row = &shown->rows[index];

	if (row->kind == SPOTIFY_ROW_TRACK) {
		sp_job_t job = {.kind = SP_JOB_ACTION, .note = "spotify_playing"};
		if (strcmp(current_list.op, "queue") == 0) {
			snprintf(job.op, sizeof(job.op), "skip");
			snprintf(job.args[0], sizeof(job.args[0]), "%s", row->position);
			snprintf(job.args[1], sizeof(job.args[1]), "%s", row->uri);
			job.nargs = 2;
		} else {
			snprintf(job.op, sizeof(job.op), "play");
			if (row->context[0] && row->position[0]) {
				// The row's place in its list, and the track that place
				// holds: the URI is what makes the answer unambiguous when
				// the list happens to carry the same track twice.
				snprintf(job.args[0], sizeof(job.args[0]), "%s", row->context);
				snprintf(job.args[1], sizeof(job.args[1]), "%s", row->position);
				snprintf(job.args[2], sizeof(job.args[2]), "%s", row->uri);
				job.nargs = 3;
			} else if (row->context[0]) {
				snprintf(job.args[0], sizeof(job.args[0]), "%s", row->context);
				job.nargs = 1;
			} else {
				snprintf(job.args[0], sizeof(job.args[0]), "%s", row->uri);
				job.nargs = 1;
			}
		}
		if (submit(&job)) {
			toast_plain("spotify_playing");
		}
		return;
	}

	// A row that is a list of its own.
	if (row->kind != SPOTIFY_ROW_ALBUM && row->kind != SPOTIFY_ROW_ARTIST && row->kind != SPOTIFY_ROW_PLAYLIST) {
		return;
	}
	sp_job_t job = {.kind = SP_JOB_LIST};
	snprintf(job.op, sizeof(job.op), "%s", row->kind == SPOTIFY_ROW_ALBUM	? "album"
										: row->kind == SPOTIFY_ROW_ARTIST ? "artist"
																		  : "playlist");
	snprintf(job.args[0], sizeof(job.args[0]), "%s", row->uri);
	job.nargs = 1;
	snprintf(job.title, sizeof(job.title), "%s", row->title);
	run_list(job, true);
}

static void rebuild_list(void) {
	lv_obj_clean(list_container);

	if (!shown || !shown->count) {
		lv_obj_t *empty = lv_label_create(list_container);
		lv_obj_add_style(empty, &theme_style_text_dim, 0);
		lv_obj_set_width(empty, lv_pct(100));
		lv_label_set_long_mode(empty, LV_LABEL_LONG_WRAP);
		lv_label_set_text(empty, tr("spotify_empty"));
		return;
	}

	for (int i = 0; i < shown->count; i++) {
		const spotify_row_t *row = &shown->rows[i];
		lv_obj_t *value = NULL;
		lv_obj_t *obj = settingsrow_add(list_container, "", &value, row_cb, (void *)(intptr_t)i);
		lv_label_set_text(settingsrow_name_label(obj), row->title); // data, not a translation key
		settingsrow_name_lines(obj, 2);
		if (row->subtitle[0] && value) {
			lv_label_set_text(value, row->subtitle);
		}
		settingsrow_add_icon(obj, row_icon(row->kind),
							 row->kind == SPOTIFY_ROW_TRACK ? THEME_SEMANTIC_GREEN : THEME_SEMANTIC_CYAN);
	}

	if (cursor_count > 0) {
		settingsrow_action(list_container, "spotify_previous_page", page_cb, (void *)(intptr_t)-1);
	}
	if (shown->cursor[0]) {
		settingsrow_action(list_container, "spotify_next_page", page_cb, (void *)(intptr_t)1);
	}
}

// ---------------------------------------------------------------------------
// the settings page
// ---------------------------------------------------------------------------

static void settings_refresh(void) {
	lv_label_set_text(name_value, spotify_receiver_name());
	for (int i = 0; i < 3; i++) {
		settingsrow_pill_active(bitrate_pills[i], spotify_bitrate() == (i == 0 ? 96 : i == 1 ? 160 : 320));
		settingsrow_pill_active(cache_pills[i], spotify_cache_mb() == (i == 0 ? 0 : i == 1 ? 1024 : 8192));
	}
	for (int i = 0; i < 4; i++) {
		settingsrow_pill_active(idle_pills[i], spotify_idle_stop_secs() == (i == 0 ? 0 : i == 1 ? 300 : i == 2 ? 900 : 3600));
	}
}

// Shuffle and repeat are the receiver's own state rather than ours: asked of
// it when the page opens, and sent back to it when they change. Two ops, one
// after the other rather than together -- there is one mailbox, and the second
// question would throw away the first answer.
static void query_control(void) {
	if (!spotify_running()) {
		return;
	}
	sp_job_t job = {.kind = SP_JOB_CONTROL};
	snprintf(job.op, sizeof(job.op), "settings");
	submit(&job);
}

static void query_account(void) {
	sp_job_t job = {.kind = SP_JOB_CONTROL};
	snprintf(job.op, sizeof(job.op), "account");
	submit(&job);
}

static void shuffle_cb(lv_event_t *e) {
	(void)e;
	if (switcher_back_drag_active()) {
		return;
	}
	bool on = strcmp(lv_label_get_text(shuffle_value), tr("on")) != 0;
	sp_job_t job = {.kind = SP_JOB_ACTION};
	snprintf(job.op, sizeof(job.op), "shuffle");
	snprintf(job.args[0], sizeof(job.args[0]), "%s", on ? "on" : "off");
	job.nargs = 1;
	if (submit(&job)) {
		lv_label_set_text(shuffle_value, on ? tr("on") : tr("off"));
	}
}

static void repeat_cb(lv_event_t *e) {
	(void)e;
	if (switcher_back_drag_active()) {
		return;
	}
	// The protocol's three answers, cycled: off, the list, one track.
	const char *now = lv_label_get_text(repeat_value);
	const char *next = strcmp(now, tr("spotify_repeat_context")) == 0 ? "track"
					   : strcmp(now, tr("spotify_repeat_track")) == 0 ? "off"
																	 : "context";
	sp_job_t job = {.kind = SP_JOB_ACTION};
	snprintf(job.op, sizeof(job.op), "repeat");
	snprintf(job.args[0], sizeof(job.args[0]), "%s", next);
	job.nargs = 1;
	if (submit(&job)) {
		lv_label_set_text(repeat_value, strcmp(next, "track") == 0	 ? tr("spotify_repeat_track")
										: strcmp(next, "context") == 0 ? tr("spotify_repeat_context")
																	   : tr("off"));
	}
}

static void bitrate_cb(lv_event_t *e) {
	spotify_set_bitrate((int)(intptr_t)lv_event_get_user_data(e));
	settings_refresh();
}

static void cache_cb(lv_event_t *e) {
	spotify_set_cache_mb((int)(intptr_t)lv_event_get_user_data(e));
	settings_refresh();
}

static void idle_cb(lv_event_t *e) {
	spotify_set_idle_stop_secs((int)(intptr_t)lv_event_get_user_data(e));
	settings_refresh();
}

static void sign_out_ok(void *user) {
	(void)user;
	sp_job_t job = {.kind = SP_JOB_ACTION, .note = "spotify_signed_out"};
	snprintf(job.op, sizeof(job.op), "signout");
	submit(&job);
}

static void sign_out_cb(lv_event_t *e) {
	(void)e;
	if (switcher_back_drag_active()) {
		return;
	}
	if (!spotify_running()) {
		toast_error(status_message());
		return;
	}
	confirm_show("spotify_sign_out", "spotify_sign_out_confirm", NULL, sign_out_ok, NULL);
}

static void name_accept_cb(lv_event_t *e) {
	(void)e;
	spotify_set_receiver_name(lv_textarea_get_text(name_field));
	lv_textarea_set_text(name_field, "");
	settings_refresh();
	switch_screen_return_to(settings_screen);
}

static void name_open_cb(lv_event_t *e) {
	(void)e;
	if (switcher_back_drag_active()) {
		return;
	}
	lv_textarea_set_text(name_field, spotify_receiver_name());
	keyboard_reset(name_keyboard);
	keyboard_set_visible(name_keyboard, true);
	switch_screen(name_screen);
}

static void settings_open_cb(lv_event_t *e) {
	(void)e;
	if (switcher_back_drag_active()) {
		return;
	}
	settings_refresh();
	query_control();
	switch_screen(settings_screen);
}

// ---------------------------------------------------------------------------
// search
// ---------------------------------------------------------------------------

static void search_category_cb(lv_event_t *e) {
	search_category = (int)(intptr_t)lv_event_get_user_data(e);
	for (int i = 0; i < 4; i++) {
		settingsrow_pill_active(search_pills[i], i == search_category);
	}
}

static void search_open_cb(lv_event_t *e) {
	(void)e;
	if (switcher_back_drag_active()) {
		return;
	}
	lv_textarea_set_text(search_field, "");
	keyboard_reset(search_keyboard);
	keyboard_set_visible(search_keyboard, true);
	switch_screen(search_screen);
}

static void search_accept_cb(lv_event_t *e) {
	(void)e;
	const char *term = lv_textarea_get_text(search_field);
	if (term[0] == '\0') {
		return;
	}
	// `search <category> <term>`: the four categories the receiver accepts,
	// and one query.
	static const char *const CATEGORIES[] = {"tracks", "albums", "artists", "playlists"};
	sp_job_t job = {.kind = SP_JOB_LIST};
	snprintf(job.op, sizeof(job.op), "search");
	snprintf(job.args[0], sizeof(job.args[0]), "%s", CATEGORIES[search_category]);
	snprintf(job.args[1], sizeof(job.args[1]), "%s", term);
	job.nargs = 2;
	snprintf(job.title, sizeof(job.title), "%s", tr("spotify_search"));
	run_list(job, false);
}

// ---------------------------------------------------------------------------
// the switch
// ---------------------------------------------------------------------------

static void cover_request(const char *path) {
	if (!path[0]) {
		lv_obj_set_hidden(cover_img, true);
		return;
	}
	lv_obj_set_hidden(cover_img, false);
	coverloader_request(COVERLOADER_SPOTIFY_SLOT, path, SP_COVER_PX);
}

static void refresh(void) {
	spotify_state_t state;
	spotify_get_state(&state);

	if (!spotify_available()) {
		lv_label_set_text(status_label, tr("spotify_no_receiver"));
		lv_obj_add_state(toggle, LV_STATE_DISABLED);
		lv_obj_set_hidden(wifi_row, true);
		return;
	}
	lv_obj_remove_state(toggle, LV_STATE_DISABLED);

	status_refresh(&state);

	// The artwork is the receiver's own file, rewritten as tracks change; the
	// loader is asked again on every poll and answers from what it already
	// decoded when nothing moved.
	if (state.cover && state.state != SPOTIFY_PLAY_NONE) {
		char path[512];
		spotify_cover_path(path, sizeof(path));
		cover_request(path);
	} else {
		lv_obj_set_hidden(cover_img, true);
	}

	set_line(title_label, state.title);
	set_line(artist_label, state.artist);
	set_line(album_label, state.album);
	lv_obj_set_hidden(glyph, state.title[0] != '\0' || !lv_obj_is_hidden(cover_img));

	// The Wi-Fi row is the way to the settings that fix a missing network, so
	// it is shown only for that.
	lv_obj_set_hidden(wifi_row, wifi_is_connected());
}

static void poll_cb(lv_timer_t *timer) {
	(void)timer;

	refresh();

	// The artwork arrives from the loader's worker: collected here rather than
	// in refresh() so a busy loader costs one check per tick.
	if (!lv_obj_is_hidden(cover_img)) {
		cover_image_t image;
		bool finished = false;
		bool found = coverloader_take(COVERLOADER_SPOTIFY_SLOT, &image, &finished);
		if (finished) {
			if (found) {
				lv_image_set_src(cover_img, &image.dsc);
			} else {
				lv_obj_set_hidden(cover_img, true); // this track has none
			}
		}
	}

	pthread_mutex_lock(&job_lock);
	sp_result_t *result = mail;
	mail = NULL;
	unsigned latest = serial;
	pthread_mutex_unlock(&job_lock);

	if (result) {
		if (result->job.serial == latest) {
			toast_busy_end();
			const spotify_reply_t *reply = result->reply;
			if (!reply) {
				toast_error("spotify_out_of_memory");
			} else if (reply->status != SPOTIFY_REPLY_OK) {
				toast_error(status_key(reply->status));
			} else if (result->job.kind == SP_JOB_LIST) {
				if (!shown) {
					shown = calloc(1, sizeof(*shown));
				}
				if (shown) {
					*shown = *reply;
					rebuild_list();
				} else {
					toast_error("spotify_out_of_memory");
				}
			} else if (result->job.kind == SP_JOB_CONTROL) {
				// The second half of the settings query, asked now that the
				// first answer is in: the mailbox holds one at a time.
				if (strcmp(result->job.op, "settings") == 0 && lv_screen_active() == settings_screen) {
					query_account();
				}
				for (int i = 0; i < reply->count; i++) {
					const spotify_row_t *row = &reply->rows[i];
					if (row->kind == SPOTIFY_ROW_SETTING && strcmp(row->uri, "shuffle") == 0) {
						lv_label_set_text(shuffle_value, strcmp(row->title, "on") == 0 ? tr("on") : tr("off"));
					} else if (row->kind == SPOTIFY_ROW_SETTING && strcmp(row->uri, "repeat") == 0) {
						lv_label_set_text(repeat_value, strcmp(row->title, "track") == 0		? tr("spotify_repeat_track")
															: strcmp(row->title, "context") == 0 ? tr("spotify_repeat_context")
																								: tr("off"));
					} else if (row->kind == SPOTIFY_ROW_ACCOUNT) {
						lv_label_set_text(account_value, row->title[0] ? row->title : tr("spotify_not_signed_in"));
					}
				}
			} else if (result->job.note[0]) {
				toast_success(result->job.note);
			}
		}
		free_result(result);
	}
}

static void toggle_changed_cb(lv_event_t *e) {
	(void)e;
	bool on = lv_obj_has_state(toggle, LV_STATE_CHECKED);

	if (on && !spotify_available()) {
		lv_obj_remove_state(toggle, LV_STATE_CHECKED);
		toast_error("spotify_no_receiver");
		return;
	}
	if (on && !wifi_is_connected()) {
		lv_obj_remove_state(toggle, LV_STATE_CHECKED);
		toast_error("enable_wi_fi_first");
		return;
	}

	spotify_set_enabled(on);
	refresh();
}

static void switched_elsewhere_cb(lv_event_t *e) {
	(void)e;
	if (spotify_enabled() && spotify_available()) {
		lv_obj_add_state(toggle, LV_STATE_CHECKED);
	} else {
		lv_obj_remove_state(toggle, LV_STATE_CHECKED);
	}
	refresh();
}

// How many of this page's own screens are on the glass: one, or none. The
// browser's list is a screen of its own, so a single "the page is loaded" flag
// would stop the polling the moment the user opened a list -- and the answer
// that would have filled it would be collected by nobody.
static int screens_up;

static void screen_loaded_cb(lv_event_t *e) {
	if (screens_up++ == 0) {
		// The first one up clears the lines the last visit left behind, and
		// the switch says what the receiver is doing now rather than what it
		// was doing when this page was last open.
		set_line(title_label, "");
		set_line(artist_label, "");
		set_line(album_label, "");
		if (spotify_enabled() && spotify_available()) {
			lv_obj_add_state(toggle, LV_STATE_CHECKED);
		} else {
			lv_obj_remove_state(toggle, LV_STATE_CHECKED);
		}
		refresh();
		// Someone is looking: the receiver stays up while they are.
		spotify_page_open(true);
	}
	(void)e;
	lv_timer_resume(poll_timer);
	lv_timer_ready(poll_timer);
}

// Leaving does not stop the receiver -- the switch is the switch, and the phone
// keeps playing to it -- but it does stop the polling, and it tells the service
// that nobody is looking any more, which is what lets a receiver that has been
// sitting silent for long enough stop itself. Something waiting on an answer
// is dropped rather than drawn over the page that took this one's place.
static void screen_unloaded_cb(lv_event_t *e) {
	(void)e;
	if (screens_up > 0 && --screens_up == 0) {
		lv_timer_pause(poll_timer);
		spotify_page_open(false);
		drop_job();
	}
}

static void wifi_row_cb(lv_event_t *e) {
	(void)e;
	switch_screen(wifisettings_screen);
}

// ---------------------------------------------------------------------------
// building the pages
// ---------------------------------------------------------------------------

static lv_obj_t *screen(void) {
	lv_obj_t *obj = lv_obj_create(NULL);
	lv_obj_add_style(obj, &theme_style_screen, 0);
	switcher_attach_back_gesture(obj);
	return obj;
}

static void add_note(lv_obj_t *container, const char *key) {
	lv_obj_t *note = lv_label_create(container);
	lv_obj_add_style(note, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(note, &font_ui_18, 0);
	lv_obj_set_width(note, lv_pct(100));
	lv_label_set_long_mode(note, LV_LABEL_LONG_WRAP);
	lv_label_set_text(note, tr(key));
}

static void build_name(void) {
	name_screen = screen();
	lv_obj_t *container = settingsrow_page(name_screen, config, "spotify_receiver_name");

	name_field = lv_textarea_create(name_screen);
	lv_textarea_set_one_line(name_field, true);
	lv_textarea_set_max_length(name_field, 63);
	lv_obj_add_style(name_field, &theme_style_card, 0);
	lv_obj_set_style_text_font(name_field, &font_ui_24, 0);
	lv_obj_set_style_pad_all(name_field, 8, 0);
	lv_obj_set_size(name_field, config->screen_width - 2 * config->padding, config->screen_width < 320 ? 44 : 64);
	lv_obj_align(name_field, LV_ALIGN_TOP_LEFT, config->padding, settingsrow_content_top(config));
	keyboard_style_caret(name_field);

	name_keyboard = keyboard_create(name_screen, config->screen_width, config->screen_width < 320 ? 144 : 316,
									name_field, NULL, "ok", name_accept_cb, NULL);
	(void)container;
}

static void build_search(void) {
	search_screen = screen();
	lv_obj_t *container = settingsrow_page(search_screen, config, "spotify_search");

	lv_obj_t *pills;
	settingsrow_pills(container, "spotify_search_in", &pills);
	static const char *const NAMES[] = {"spotify_tracks", "spotify_albums", "spotify_artists", "spotify_playlists"};
	for (int i = 0; i < 4; i++) {
		search_pills[i] = settingsrow_pill(pills, NAMES[i], i, search_category_cb);
	}

	search_field = lv_textarea_create(search_screen);
	lv_textarea_set_one_line(search_field, true);
	lv_textarea_set_max_length(search_field, 79);
	lv_obj_add_style(search_field, &theme_style_card, 0);
	lv_obj_set_style_text_font(search_field, &font_ui_24, 0);
	lv_obj_set_style_pad_all(search_field, 8, 0);
	lv_obj_set_size(search_field, config->screen_width - 2 * config->padding, config->screen_width < 320 ? 44 : 64);
	lv_obj_align(search_field, LV_ALIGN_TOP_LEFT, config->padding, settingsrow_content_top(config));
	keyboard_style_caret(search_field);
	lv_textarea_set_placeholder_text(search_field, tr("spotify_search"));

	search_keyboard = keyboard_create(search_screen, config->screen_width, config->screen_width < 320 ? 144 : 316,
									  search_field, &icon_search, NULL, search_accept_cb, NULL);
	for (int i = 0; i < 4; i++) {
		settingsrow_pill_active(search_pills[i], i == search_category);
	}
}

static void build_settings(void) {
	settings_screen = screen();
	lv_obj_t *container = settingsrow_page(settings_screen, config, "spotify_settings");

	lv_obj_t *row = settingsrow_add(container, "spotify_account", &account_value, NULL, NULL);
	settingsrow_add_icon(row, &icon_menu_artist, THEME_SEMANTIC_BLUE);
	lv_label_set_text(account_value, tr("spotify_not_signed_in"));

	row = settingsrow_add(container, "spotify_shuffle", &shuffle_value, shuffle_cb, NULL);
	settingsrow_add_icon(row, &icon_shuffle, THEME_SEMANTIC_PURPLE);
	lv_label_set_text(shuffle_value, tr("off"));
	row = settingsrow_add(container, "spotify_repeat", &repeat_value, repeat_cb, NULL);
	settingsrow_add_icon(row, &icon_repeat_all, THEME_SEMANTIC_PURPLE);
	lv_label_set_text(repeat_value, tr("off"));

	lv_obj_t *pills;
	settingsrow_pills(container, "spotify_bitrate", &pills);
	bitrate_pills[0] = settingsrow_pill_text(pills, "96", 96, bitrate_cb);
	bitrate_pills[1] = settingsrow_pill_text(pills, "160", 160, bitrate_cb);
	bitrate_pills[2] = settingsrow_pill_text(pills, "320", 320, bitrate_cb);
	add_note(container, "spotify_bitrate_note");

	settingsrow_pills(container, "spotify_cache", &pills);
	cache_pills[0] = settingsrow_pill(pills, "off", 0, cache_cb);
	cache_pills[1] = settingsrow_pill_text(pills, "1 GB", 1024, cache_cb);
	cache_pills[2] = settingsrow_pill_text(pills, "8 GB", 8192, cache_cb);
	add_note(container, "spotify_cache_note");

	row = settingsrow_add(container, "spotify_receiver_name", &name_value, name_open_cb, NULL);
	settingsrow_add_icon(row, &icon_spotify_mark, THEME_SEMANTIC_GREEN);
	add_note(container, "spotify_receiver_name_note");

	settingsrow_pills(container, "spotify_idle_stop", &pills);
	idle_pills[0] = settingsrow_pill(pills, "never", 0, idle_cb);
	idle_pills[1] = settingsrow_pill_text(pills, "5 min", 300, idle_cb);
	idle_pills[2] = settingsrow_pill_text(pills, "15 min", 900, idle_cb);
	idle_pills[3] = settingsrow_pill_text(pills, "1 h", 3600, idle_cb);
	add_note(container, "spotify_idle_stop_note");

	row = settingsrow_action(container, "spotify_sign_out", sign_out_cb, NULL);
	settingsrow_add_icon(row, &icon_log_out, THEME_SEMANTIC_RED);
	add_note(container, "spotify_sign_out_note");
	add_note(container, "spotify_card_note");
	player_sheet_attach_drag(container, true);
	settings_refresh();
}

void spotifypage_init(gui_config_t *cfg) {
	config = cfg;

	spotify_screen = screen();
	lv_obj_t *container = settingsrow_page(spotify_screen, cfg, "spotify");

	settingsrow_toggle(container, "spotify", &toggle, toggle_changed_cb);

	glyph = lv_image_create(container);
	lv_image_set_src(glyph, &icon_spotify_mark);
	lv_obj_add_style(glyph, &theme_style_icon, 0);
	lv_obj_set_style_image_opa(glyph, LV_OPA_40, 0);
	lv_obj_set_style_margin_top(glyph, 24, 0);
	lv_obj_set_style_margin_bottom(glyph, 12, 0);
	lv_obj_set_style_align(glyph, LV_ALIGN_CENTER, 0);

	cover_img = lv_image_create(container);
	lv_obj_set_size(cover_img, SP_COVER_PX, SP_COVER_PX);
	lv_obj_set_style_radius(cover_img, 6, 0);
	lv_obj_set_style_clip_corner(cover_img, true, 0);
	lv_obj_set_style_align(cover_img, LV_ALIGN_CENTER, 0);
	lv_obj_set_hidden(cover_img, true);

	title_label = lv_label_create(container);
	lv_obj_set_width(title_label, lv_pct(100));
	lv_obj_add_style(title_label, &theme_style_text, 0);
	lv_obj_set_style_text_font(title_label, &font_ui_26, 0);
	lv_obj_set_style_text_align(title_label, LV_TEXT_ALIGN_CENTER, 0);
	scrolltext_apply(title_label);

	artist_label = lv_label_create(container);
	lv_obj_set_width(artist_label, lv_pct(100));
	lv_obj_add_style(artist_label, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(artist_label, &font_ui_22, 0);
	lv_obj_set_style_text_align(artist_label, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_set_style_pad_top(artist_label, 6, 0);
	scrolltext_apply(artist_label);

	album_label = lv_label_create(container);
	lv_obj_set_width(album_label, lv_pct(100));
	lv_obj_add_style(album_label, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(album_label, &font_ui_20, 0);
	lv_obj_set_style_text_align(album_label, LV_TEXT_ALIGN_CENTER, 0);
	scrolltext_apply(album_label);

	status_label = lv_label_create(container);
	lv_obj_set_width(status_label, lv_pct(100));
	lv_label_set_long_mode(status_label, LV_LABEL_LONG_WRAP);
	lv_obj_add_style(status_label, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(status_label, &font_ui_20, 0);
	lv_obj_set_style_text_align(status_label, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_set_style_pad_hor(status_label, 4, 0);
	lv_obj_set_style_pad_top(status_label, 14, 0);

	wifi_row = settingsrow_add(container, "wi_fi_settings", NULL, wifi_row_cb, NULL);
	lv_obj_set_style_margin_top(wifi_row, 16, 0);
	lv_obj_set_hidden(wifi_row, true);

	// The browser. Every row needs the receiver up, so a tap before that says
	// why instead of showing an empty list.
	for (unsigned int i = 0; i < sizeof(BROWSER) / sizeof(BROWSER[0]); i++) {
		lv_obj_t *browse = settingsrow_add(container, BROWSER[i].label, NULL, browse_cb, (void *)(intptr_t)i);
		settingsrow_add_icon(browse, BROWSER[i].icon, BROWSER[i].tone);
	}

	lv_obj_t *row = settingsrow_add(container, "spotify_search", NULL, search_open_cb, NULL);
	settingsrow_add_icon(row, &icon_search, THEME_SEMANTIC_BLUE);
	// The rows of ACTIONS are numbered on from the browser's, so the one tap
	// callback can tell a list from a page.
	for (unsigned int i = 0; i < sizeof(ACTIONS) / sizeof(ACTIONS[0]); i++) {
		row = settingsrow_add(container, ACTIONS[i].label, NULL, browse_cb,
							  (void *)(intptr_t)(sizeof(BROWSER) / sizeof(BROWSER[0]) + i));
		settingsrow_add_icon(row, ACTIONS[i].icon, ACTIONS[i].tone);
	}

	player_sheet_attach_drag(container, true);

	list_screen = screen();
	list_container = settingsrow_page(list_screen, cfg, "spotify");
	list_title = settingsrow_page_title(list_screen);
	player_sheet_attach_drag(list_container, true);
	switcher_set_back_guard(list_screen, list_back);
	switcher_set_back_guard_peek(list_screen, list_back_has_step);

	build_search();
	build_name();
	build_settings();
	settings_refresh();

	poll_timer = lv_timer_create(poll_cb, SP_POLL_MS, NULL);
	lv_timer_pause(poll_timer);

	lv_obj_add_event_cb(spotify_screen, screen_loaded_cb, LV_EVENT_SCREEN_LOADED, NULL);
	lv_obj_add_event_cb(spotify_screen, switched_elsewhere_cb, LV_EVENT_REFRESH, NULL);
	lv_obj_add_event_cb(spotify_screen, screen_unloaded_cb, LV_EVENT_SCREEN_UNLOADED, NULL);
	// The browser's list and the three pages reached from the switch are this
	// page too: while one of them is up, the polling and the idle stop follow
	// from the same rules as the switch's own screen.
	lv_obj_t *const OURS[] = {list_screen, settings_screen, name_screen, search_screen};
	for (size_t i = 0; i < sizeof(OURS) / sizeof(OURS[0]); i++) {
		lv_obj_add_event_cb(OURS[i], screen_loaded_cb, LV_EVENT_SCREEN_LOADED, NULL);
		lv_obj_add_event_cb(OURS[i], screen_unloaded_cb, LV_EVENT_SCREEN_UNLOADED, NULL);
	}

	refresh();
}
