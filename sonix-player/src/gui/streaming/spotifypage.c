#include "spotifypage.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lvgl/lvgl.h"

#include "src/gui/board_profile.h"
#include "src/gui/fonts/fonts.h"
#include "src/gui/nowplaying/player.h"
#include "src/gui/shell/gridpage.h"
#include "src/gui/shell/gui.h"
#include "src/gui/shell/icons.h"
#include "src/gui/shell/keyboard.h"
#include "src/gui/shell/settingsrow.h"
#include "src/gui/shell/switcher.h"
#include "src/gui/shell/theme.h"
#include "src/gui/shell/toast.h"
#include "src/gui/wireless/wifisettings.h"
#include "src/system/audio/audio.h"
#include "src/system/core/lang.h"
#include "src/system/net/wifi.h"
#include "src/system/streaming/spotify.h"

// The Spotify pages -- SpotUI's shape, drawn with this player's widgets.
//
//   home       four tiles: Liked Songs, Playlists, Search, Account
//   list       one page, reused for Liked Songs, a playlist and a search
//   playlists  every playlist in the account
//   search     the keyboard, and the last query
//   account    who is signed in, the daemon, the modes, the demo notice
//
// Two rules shape all of them. The GUI thread never waits for Spotify: the
// lists arrive on a worker (Spotify's own requests take seconds, a search can
// take ten), the transport keys post and forget, and what a page draws is the
// client's last snapshot. And the pages look like the rest of the player --
// the same settings rows, the same tiled grid, the same headings, the same
// offline card -- because a service that looks foreign is a service told
// apart from the music, and this one is not.

#define POLL_MS 300

// A list is one page of up to 64 tracks, drawn through a pool of rows that
// ride the scroll: a widget per track is felt on this hardware at a hundred,
// and the lists here reach that.
#define ROW_POOL 8
#define ROW_HEIGHT bp_pick(56, 100)
#define ROW_GAP bp_pick(4, 8)
#define ROW_PITCH (ROW_HEIGHT + ROW_GAP)
#define PAGE_TRACKS 64

typedef struct {
	lv_obj_t *row;
	lv_obj_t *num;
	lv_obj_t *mark;
	lv_obj_t *title;
	lv_obj_t *sub;
	int index;
} list_row_t;

typedef enum {
	SOURCE_LIKED = 0,
	SOURCE_PLAYLIST,
	SOURCE_SEARCH,
} source_t;

static gui_config_t *g_cfg;

static void start_list(source_t src, const char *playlist_id, const char *name, const char *query);
static void draw_window(int first);
static void refresh_chrome(void);
static void search_submit_cb(lv_event_t *e);
static void playlists_unloaded_cb(lv_event_t *e);
static void free_rowdata_cb(lv_event_t *e);

// Home.
static lv_obj_t *home_grid;
static lv_obj_t *home_offline;
static lv_obj_t *home_demo;
static lv_obj_t *home_account_line;
static lv_timer_t *poll_timer;
static unsigned last_serial;

// The home grid, and the list page, which the header hands to the rest of the
// player: the Streaming page's tile needs the first, and the player's queue
// shortcut the second.
lv_obj_t *spotify_screen;
lv_obj_t *spotify_list_screen;

// The list page's own name inside this file.
#define list_screen spotify_list_screen
static lv_obj_t *list_body;
static lv_obj_t *list_spinner;
static lv_obj_t *list_message;
static lv_obj_t *list_count;
static list_row_t rows[ROW_POOL];
static spotify_track_t tracks[PAGE_TRACKS];
static int drawn;
static int total;
static source_t source;
static char context_playlist[SPOTIFY_ID_MAX];
static char context_name[SPOTIFY_NAME_MAX];
static char context_query[SPOTIFY_NAME_MAX];
static bool list_open;
static int list_serial_wanted;

// Playlists page.
static lv_obj_t *playlists_screen;
static lv_obj_t *playlists_body;
static lv_obj_t *playlists_message;
static bool playlists_open;
static int playlists_serial_wanted;

// Search page.
static lv_obj_t *search_screen;
static lv_obj_t *search_field;
static keyboard_t *search_keyboard;

// Account page.
static lv_obj_t *account_screen;
static lv_obj_t *account_name;
static lv_obj_t *account_backend;
static lv_obj_t *account_quality;
static lv_obj_t *account_notice;
static lv_obj_t *account_shuffle;
static lv_obj_t *account_repeat;

// ---------------------------------------------------------------------------
// The worker
// ---------------------------------------------------------------------------

typedef enum {
	JOB_NONE = 0,
	JOB_LIKED,
	JOB_PLAYLIST,
	JOB_SEARCH,
	JOB_PLAYLISTS,
} job_kind_t;

typedef struct {
	bool ready;
	bool ok;
	job_kind_t kind;
	int serial;
	int count;
	int total;
	char playlist_id[SPOTIFY_ID_MAX];
	char playlist_name[SPOTIFY_NAME_MAX];
	char query[SPOTIFY_NAME_MAX];
	spotify_track_t tracks[PAGE_TRACKS];
	spotify_list_t lists[32];
	int list_count;
} result_t;

typedef struct {
	job_kind_t kind;
	int serial;
	char playlist_id[SPOTIFY_ID_MAX];
	char playlist_name[SPOTIFY_NAME_MAX];
	char query[SPOTIFY_NAME_MAX];
} job_t;

static pthread_mutex_t job_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t job_ready = PTHREAD_COND_INITIALIZER;
static job_t pending;
static bool have_job;
static int job_serial;
static pthread_t worker;
static bool worker_started;

// One result slot, newest wins. On the heap: a result is ~30 KB and the GUI
// thread copies it as a local when it draws, which is not a stack this device
// has to spare.
static pthread_mutex_t result_lock = PTHREAD_MUTEX_INITIALIZER;
static result_t *shared;

static void job_done_async(void *user);

static void *worker_main(void *arg) {
	(void)arg;
	for (;;) {
		job_t job;
		pthread_mutex_lock(&job_lock);
		while (!have_job) {
			pthread_cond_wait(&job_ready, &job_lock);
		}
		job = pending;
		have_job = false;
		pthread_mutex_unlock(&job_lock);

		result_t *fresh = calloc(1, sizeof(result_t));
		if (!fresh) {
			continue;
		}
		fresh->kind = job.kind;
		fresh->serial = job.serial;
		snprintf(fresh->playlist_id, sizeof(fresh->playlist_id), "%s", job.playlist_id);
		snprintf(fresh->playlist_name, sizeof(fresh->playlist_name), "%s", job.playlist_name);
		snprintf(fresh->query, sizeof(fresh->query), "%s", job.query);

		switch (job.kind) {
		case JOB_LIKED:
			fresh->ok = spotify_fetch_liked(0, PAGE_TRACKS, fresh->tracks, PAGE_TRACKS, &fresh->total);
			break;
		case JOB_PLAYLIST:
			fresh->ok = spotify_fetch_playlist(job.playlist_id, 0, PAGE_TRACKS, fresh->tracks, PAGE_TRACKS,
											   &fresh->total);
			break;
		case JOB_SEARCH:
			fresh->ok = spotify_search(job.query, 0, PAGE_TRACKS, fresh->tracks, PAGE_TRACKS, &fresh->total);
			break;
		case JOB_PLAYLISTS:
			fresh->ok = spotify_fetch_playlists(fresh->lists, 32, &fresh->list_count);
			break;
		default:
			break;
		}
		fresh->count = fresh->total > PAGE_TRACKS ? PAGE_TRACKS : (fresh->total > 0 ? fresh->total : 0);
		fresh->ready = true;

		pthread_mutex_lock(&result_lock);
		*shared = *fresh;
		pthread_mutex_unlock(&result_lock);
		free(fresh);

		gui_post(job_done_async, NULL);
	}
	return NULL;
}

static bool ensure_worker(void) {
	if (worker_started) {
		return true;
	}
	// 192 KB: the worker's real work (an HTTP request, a 24 KB parse) happens
	// on the heap; the frames it needs are the parser's, nothing like the 8 MB
	// a target pthread would default to.
	pthread_attr_t attr;
	pthread_attr_init(&attr);
	pthread_attr_setstacksize(&attr, 192 * 1024);
	int err = pthread_create(&worker, &attr, worker_main, NULL);
	pthread_attr_destroy(&attr);
	if (err != 0) {
		fprintf(stderr, "spotify: the page worker will not start (%d)\n", err);
		return false;
	}
	worker_started = true;
	return true;
}

// Hands the worker one request. `wanted` is the page-side "which answer am I
// still waiting for" counter, and it is set HERE, under the same lock the
// signal is sent under: set afterwards, a worker fast enough to finish and post
// its result first would find the counter still holding the previous request's
// number, and the answer would be dropped as stale -- an empty list on a fast
// machine, and nothing at all on a slow one to explain it.
static void submit(job_kind_t kind, int *wanted, const char *playlist_id, const char *playlist_name,
				   const char *query) {
	if (!ensure_worker()) {
		if (wanted) {
			*wanted = -1;
		}
		return;
	}
	pthread_mutex_lock(&job_lock);
	pending.kind = kind;
	pending.serial = ++job_serial;
	snprintf(pending.playlist_id, sizeof(pending.playlist_id), "%s", playlist_id ? playlist_id : "");
	snprintf(pending.playlist_name, sizeof(pending.playlist_name), "%s", playlist_name ? playlist_name : "");
	snprintf(pending.query, sizeof(pending.query), "%s", query ? query : "");
	if (wanted) {
		*wanted = pending.serial;
	}
	have_job = true;
	pthread_cond_signal(&job_ready);
	pthread_mutex_unlock(&job_lock);
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static bool compact(void) { return bp_is_tempotec_v1(); }

static bool online(void) {
#ifdef __EMSCRIPTEN__
	return true; // the emulator has no radio to ask
#else
	wifi_status_t st;
	wifi_get_status(&st);
	return st.state == WIFI_STATE_CONNECTED;
#endif
}

// A daemon that answered, or the demo library: either can list something.
static bool service_live(void) {
	spotify_status_t st;
	spotify_status(&st);
	return st.connected && (st.logged_in || strcmp(st.backend, "mock") == 0);
}

static void set_visible(lv_obj_t *obj, bool visible) {
	if (obj) {
		lv_obj_set_hidden(obj, !visible);
	}
}

static void free_rowdata_cb(lv_event_t *e) { free(lv_event_get_user_data(e)); }

// The spinner is a plain rotating label: LV_USE_SPINNER is off in this
// configuration, and a second widget type would be a second thing to theme.
static lv_obj_t *spinner_create(lv_obj_t *parent) {
	lv_obj_t *label = lv_label_create(parent);
	lv_label_set_text(label, LV_SYMBOL_REFRESH);
	lv_obj_set_style_text_font(label, &font_ui_20, 0);
	lv_obj_add_style(label, &theme_style_text_dim, 0);
	return label;
}

// ---------------------------------------------------------------------------
// The list page
// ---------------------------------------------------------------------------

static void list_show_loading(void) {
	set_visible(list_spinner, true);
	set_visible(list_message, false);
	set_visible(list_body, false);
	set_visible(list_count, false);
}

static void list_show_message(const char *text) {
	set_visible(list_spinner, false);
	set_visible(list_body, false);
	set_visible(list_count, false);
	if (list_message) {
		lv_label_set_text(list_message, text ? text : "");
		set_visible(list_message, text && text[0]);
	}
}

static void list_show_rows(void) {
	set_visible(list_spinner, false);
	set_visible(list_message, false);
	set_visible(list_body, true);
	set_visible(list_count, true);
}

static void bind_row(list_row_t *r, const spotify_track_t *t, int index, bool playing) {
	r->index = index;
	lv_label_set_text_fmt(r->num, "%d", index + 1);
	set_visible(r->num, !playing);
	set_visible(r->mark, playing);
	lv_label_set_text(r->title, t->title[0] ? t->title : t->id);
	if (t->artist[0] && t->album[0]) {
		lv_label_set_text_fmt(r->sub, "%s - %s", t->artist, t->album);
	} else {
		lv_label_set_text(r->sub, t->artist[0] ? t->artist : t->album);
	}
	set_visible(r->row, true);
}

// Window `first` shows tracks [first, first + ROW_POOL); the rows and the
// scroll agree because a row's place is first * ROW_PITCH.
static void draw_window(int first) {
	spotify_status_t st;
	spotify_status(&st);
	bool playing_state = st.state == SPOTIFY_STATE_PLAYING || st.state == SPOTIFY_STATE_LOADING;
	for (int i = 0; i < ROW_POOL; i++) {
		list_row_t *r = &rows[i];
		int index = first + i;
		if (index < 0 || index >= drawn) {
			set_visible(r->row, false);
			continue;
		}
		bool playing = playing_state && st.track.id[0] && strcmp(tracks[index].id, st.track.id) == 0;
		bind_row(r, &tracks[index], index, playing);
	}
}

static void list_scrolled_cb(lv_event_t *e) {
	lv_obj_t *body = lv_event_get_target_obj(e);
	int first = (int)lv_obj_get_scroll_y(body) / ROW_PITCH;
	int max_first = drawn > ROW_POOL ? drawn - ROW_POOL : 0;
	if (first > max_first) {
		first = max_first;
	}
	if (first < 0) {
		first = 0;
	}
	draw_window(first);
}

static void play_index(int index) {
	if (index < 0 || index >= drawn) {
		return;
	}
	const char *id = tracks[index].id;
	if (!id[0]) {
		return; // an unreadable row: its title is the reason, and nothing plays
	}

	// One PCM takes one writer, so whatever was playing stops first -- the
	// same order the radio keeps. A paused local track must not be holding
	// the DAC the daemon is about to write into.
	player_pause_for_loading();

	const char *kind = source == SOURCE_PLAYLIST ? "playlist" : (source == SOURCE_SEARCH ? "search" : "liked");
	const char *context = source == SOURCE_PLAYLIST ? context_playlist
						  : source == SOURCE_SEARCH  ? context_query
													 : "";
	spotify_play_context(kind, context, id);

	// The player's own sheet, which is already bound to this source: title,
	// artists, sleeve, the seek bar and the transport all read the snapshot.
	player_sheet_open(true);
}

static void row_click_cb(lv_event_t *e) {
	list_row_t *r = (list_row_t *)lv_event_get_user_data(e);
	if (!r || switcher_back_drag_active()) {
		return;
	}
	play_index(r->index);
}

static void start_list(source_t src, const char *playlist_id, const char *name, const char *query) {
	source = src;
	drawn = 0;
	total = 0;
	snprintf(context_playlist, sizeof(context_playlist), "%s", playlist_id ? playlist_id : "");
	snprintf(context_name, sizeof(context_name), "%s", name ? name : "");
	snprintf(context_query, sizeof(context_query), "%s", query ? query : "");

	// The heading is the page's title, and a playlist's own name is the honest
	// one: settingsrow_title_realign() re-centres it after the change.
	const char *heading = "spotify";
	if (src == SOURCE_PLAYLIST && name && name[0]) {
		heading = name;
	} else if (src == SOURCE_SEARCH) {
		heading = "spotify_search";
	} else if (src == SOURCE_LIKED) {
		heading = "spotify_liked";
	}
	lv_obj_t *title = settingsrow_page_title(list_screen);
	if (title) {
		lv_label_set_text(title, src == SOURCE_PLAYLIST && name ? name : tr(heading));
		settingsrow_title_realign(title);
	}

	list_open = true;
	list_show_loading();
	submit(src == SOURCE_LIKED   ? JOB_LIKED
		   : src == SOURCE_SEARCH ? JOB_SEARCH
								  : JOB_PLAYLIST,
		   &list_serial_wanted, context_playlist, name ? name : "", context_query);
	switch_screen(list_screen);
}

static void list_unloaded_cb(lv_event_t *e) {
	(void)e;
	list_open = false;
}

// ---------------------------------------------------------------------------
// The playlists page
// ---------------------------------------------------------------------------

static void playlist_row_click_cb(lv_event_t *e) {
	const char *id = (const char *)lv_event_get_user_data(e);
	if (!id || !id[0] || switcher_back_drag_active()) {
		return;
	}
	spotifypage_open_playlist(id, NULL);
}

static void draw_playlists(const result_t *r) {
	if (!playlists_body || !playlists_open) {
		return;
	}
	lv_obj_clean(playlists_body);
	if (!r->ok) {
		spotify_status_t st;
		spotify_status(&st);
		lv_label_set_text(playlists_message, st.error[0] ? st.error : tr("spotify_request_failed"));
		set_visible(playlists_message, true);
		return;
	}
	if (r->list_count == 0) {
		lv_label_set_text(playlists_message, tr("spotify_no_playlists"));
		set_visible(playlists_message, true);
		return;
	}
	set_visible(playlists_message, false);

	for (int i = 0; i < r->list_count; i++) {
		const spotify_list_t *pl = &r->lists[i];
		lv_obj_t *row = lv_obj_create(playlists_body);
		lv_obj_add_style(row, &theme_style_card, 0);
		lv_obj_set_size(row, lv_pct(100), ROW_HEIGHT);
		lv_obj_set_style_pad_all(row, 0, 0);
		lv_obj_set_style_pad_hor(row, compact() ? 10 : 16, 0);
		lv_obj_set_style_radius(row, bp_tile_radius(), 0);
		lv_obj_set_scrollable(row, false);
		// lv_obj_create() makes a container, and a container is not clickable.
		// A row that answers no tap is a row that cannot be played.
		lv_obj_set_clickable(row, true);

		lv_obj_t *name = lv_label_create(row);
		lv_obj_add_style(name, &theme_style_text, 0);
		lv_obj_set_style_text_font(name, &font_ui_16, 0);
		lv_obj_set_width(name, lv_pct(100));
		lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
		lv_label_set_text(name, pl->name[0] ? pl->name : pl->id);
		lv_obj_align(name, LV_ALIGN_TOP_LEFT, 0, compact() ? 7 : 14);

		lv_obj_t *sub = lv_label_create(row);
		lv_obj_add_style(sub, &theme_style_text_dim, 0);
		lv_obj_set_style_text_font(sub, &font_ui_14, 0);
		lv_obj_set_width(sub, lv_pct(100));
		lv_label_set_long_mode(sub, LV_LABEL_LONG_DOT);
		lv_label_set_text_fmt(sub, "%s%s%d %s", pl->owner[0] ? pl->owner : "", pl->owner[0] ? " - " : "", pl->total,
							  tr("spotify_tracks"));
		lv_obj_align(sub, LV_ALIGN_TOP_LEFT, 0, compact() ? 27 : 44);

		// The id has to outlive this function: one small copy per row, freed
		// with the row.
		char *id = malloc(SPOTIFY_ID_MAX);
		if (!id) {
			continue;
		}
		snprintf(id, SPOTIFY_ID_MAX, "%s", pl->id);
		lv_obj_add_event_cb(row, playlist_row_click_cb, LV_EVENT_CLICKED, id);
		lv_obj_add_event_cb(row, free_rowdata_cb, LV_EVENT_DELETE, id);
	}
}

// ---------------------------------------------------------------------------
// A finished job, on the GUI thread
// ---------------------------------------------------------------------------

static void job_done_async(void *user) {
	(void)user;
	result_t *done = malloc(sizeof(result_t));
	if (!done) {
		return;
	}
	pthread_mutex_lock(&result_lock);
	*done = *shared;
	shared->ready = false;
	pthread_mutex_unlock(&result_lock);

	if (!done->ready) {
		free(done);
		return;
	}

	switch (done->kind) {
	case JOB_LIKED:
	case JOB_PLAYLIST:
	case JOB_SEARCH:
		if (!list_open || done->serial != list_serial_wanted) {
			break;
		}
		if (!done->ok) {
			spotify_status_t st;
			spotify_status(&st);
			list_show_message(st.error[0] ? st.error : tr("spotify_request_failed"));
			break;
		}
		drawn = done->count;
		total = done->total;
		memcpy(tracks, done->tracks, sizeof(spotify_track_t) * (size_t)drawn);
		if (drawn == 0) {
			list_show_message(tr("spotify_no_tracks"));
			break;
		}
		if (list_count) {
			lv_label_set_text_fmt(list_count, "%d %s", total, tr("spotify_tracks"));
		}
		list_show_rows();
		draw_window(0);
		if (list_body) {
			lv_obj_scroll_to_y(list_body, 0, LV_ANIM_OFF);
		}
		break;
	case JOB_PLAYLISTS:
		if (done->serial == playlists_serial_wanted) {
			draw_playlists(done);
		}
		break;
	default:
		break;
	}
	free(done);
}

// ---------------------------------------------------------------------------
// The poll: the one timer all the pages share
// ---------------------------------------------------------------------------

static void refresh_chrome(void) {
	spotify_status_t st;
	spotify_status(&st);
	bool demo = st.connected && strcmp(st.backend, "mock") == 0;
	bool live = st.connected && (st.logged_in || demo);

	// Home: the badge, the offline card, the account line.
	set_visible(home_demo, demo);
	set_visible(home_offline, !live);
	if (home_account_line) {
		if (demo) {
			lv_label_set_text(home_account_line, tr("spotify_demo_note"));
		} else if (!st.connected) {
			lv_label_set_text(home_account_line, tr("spotify_no_daemon"));
		} else if (st.logged_in && st.account[0]) {
			lv_label_set_text(home_account_line, st.account);
		} else if (st.logged_in) {
			lv_label_set_text(home_account_line, tr("spotify_connected"));
		} else {
			lv_label_set_text(home_account_line, tr("spotify_sign_in_note"));
		}
	}

	// Account.
	if (account_name) {
		if (st.account[0]) {
			lv_label_set_text(account_name, st.account);
		} else if (demo) {
			lv_label_set_text(account_name, tr("spotify_demo_title"));
		} else {
			lv_label_set_text(account_name, tr("spotify_not_connected"));
		}
	}
	if (account_backend) {
		lv_label_set_text(account_backend,
						  strcmp(st.backend, "mock") == 0 ? tr("spotify_backend_mock") : tr("spotify_backend_daemon"));
	}
	if (account_quality) {
		lv_label_set_text(account_quality, "320 kbps");
	}
	if (account_notice) {
		const char *notice = "";
		if (demo) {
			notice = tr("spotify_demo_note");
		} else if (!st.connected) {
			notice = tr("spotify_no_daemon");
		} else if (!st.logged_in) {
			notice = tr("spotify_sign_in_note");
		} else if (st.error[0]) {
			notice = st.error;
		}
		lv_label_set_text(account_notice, notice);
		set_visible(account_notice, notice[0] != '\0');
	}
	if (account_shuffle) {
		if (st.shuffle) {
			lv_obj_add_state(account_shuffle, LV_STATE_CHECKED);
		} else {
			lv_obj_remove_state(account_shuffle, LV_STATE_CHECKED);
		}
	}
	if (account_repeat) {
		lv_label_set_text(account_repeat, tr(st.repeat == 1 ? "spotify_repeat_queue"
										   : st.repeat >= 2 ? "spotify_repeat_track"
															: "spotify_repeat_off"));
	}
}

static void poll_cb(lv_timer_t *t) {
	(void)t;
	unsigned serial = spotify_serial();
	if (serial == last_serial) {
		return;
	}
	last_serial = serial;
	refresh_chrome();
	// The playing mark moves with the track without reloading the list.
	if (list_open && drawn > 0 && list_body) {
		int first = (int)lv_obj_get_scroll_y(list_body) / ROW_PITCH;
		int max_first = drawn > ROW_POOL ? drawn - ROW_POOL : 0;
		if (first > max_first) {
			first = max_first;
		}
		if (first < 0) {
			first = 0;
		}
		draw_window(first);
	}
}

// ---------------------------------------------------------------------------
// Navigation
// ---------------------------------------------------------------------------

static void open_liked_cb(void) { start_list(SOURCE_LIKED, "", "", ""); }

bool spotifypage_open_liked(void) {
	start_list(SOURCE_LIKED, "", "", "");
	return true;
}

bool spotifypage_open_playlist(const char *playlist_id, const char *title) {
	if (!playlist_id || !playlist_id[0]) {
		return false;
	}
	start_list(SOURCE_PLAYLIST, playlist_id, title ? title : "", "");
	return true;
}

static void open_playlists_cb(void) {
	playlists_open = true;
	lv_obj_clean(playlists_body);
	lv_label_set_text(playlists_message, tr("spotify_loading"));
	set_visible(playlists_message, true);
	switch_screen(playlists_screen);
	submit(JOB_PLAYLISTS, &playlists_serial_wanted, "", "", "");
}

static void open_search_cb(void) {
	// The heading and the field take the top of the page; the keyboard the
	// bottom half, as on every other search page here.
	if (search_field) {
		lv_textarea_set_text(search_field, "");
	}
	if (search_keyboard) {
		keyboard_set_visible(search_keyboard, true);
		keyboard_reset(search_keyboard);
	}
	switch_screen(search_screen);
}

static void open_account_cb(void) {
	last_serial = 0; // which forces refresh_chrome() on the way in
	refresh_chrome();
	switch_screen(account_screen);
}

static void offline_wifi_cb(lv_event_t *e) {
	(void)e;
	switch_screen(wifisettings_screen);
}

static void search_submit_cb(lv_event_t *e) {
	(void)e;
	if (!search_field) {
		return;
	}
	const char *q = lv_textarea_get_text(search_field);
	if (!q || !q[0]) {
		return;
	}
	if (search_keyboard) {
		keyboard_set_visible(search_keyboard, false);
	}
	start_list(SOURCE_SEARCH, "", "", q);
}

static void playlists_unloaded_cb(lv_event_t *e) {
	(void)e;
	playlists_open = false;
}

// ---------------------------------------------------------------------------
// Building the pages
// ---------------------------------------------------------------------------

static void build_home(gui_config_t *cfg) {
	const grid_entry_t entries[] = {
		{"spotify_liked", &icon_menu_qobuz_favourites, NULL, open_liked_cb},
		{"playlists", &icon_menu_qobuz_playlists, NULL, open_playlists_cb},
		{"spotify_search", &icon_search, NULL, open_search_cb},
		{"spotify_account", &icon_settings_more, NULL, open_account_cb},
	};
	home_grid = gridpage_build(spotify_screen, cfg, entries, 4, 2, 2, true);
	for (int i = 0; i < 4; i++) {
		gridpage_set_tile_label_lines(home_grid, i, 2);
	}
	switcher_attach_back_gesture(home_grid);

	// The DEMO badge, under the corner buttons: the pages work and the music
	// does not, and a silent demo track must not leave anybody guessing why.
	home_demo = lv_label_create(spotify_screen);
	lv_label_set_text(home_demo, tr("spotify_demo"));
	lv_obj_set_style_text_font(home_demo, &font_ui_14, 0);
	lv_obj_set_style_text_color(home_demo, lv_color_white(), 0);
	lv_obj_set_style_bg_color(home_demo, lv_color_make(29, 185, 84), 0); // the Spotify green
	lv_obj_set_style_bg_opa(home_demo, LV_OPA_COVER, 0);
	lv_obj_set_style_radius(home_demo, 6, 0);
	lv_obj_set_style_pad_hor(home_demo, 8, 0);
	lv_obj_set_style_pad_ver(home_demo, 3, 0);
	lv_obj_align(home_demo, LV_ALIGN_TOP_RIGHT, -cfg->padding,
				 cfg->top_bar_height + cfg->padding + settingsrow_corner_button_size(cfg) + 6);
	lv_obj_set_hidden(home_demo, true);

	// The offline card. The grid stays behind it -- the pages exist, they
	// simply cannot list anything -- so it is a sentence, not a second page.
	home_offline = lv_obj_create(spotify_screen);
	lv_obj_add_style(home_offline, &theme_style_card, 0);
	lv_obj_set_size(home_offline, lv_pct(86), LV_SIZE_CONTENT);
	lv_obj_align(home_offline, LV_ALIGN_BOTTOM_MID, 0, -(cfg->padding + 22));
	lv_obj_set_flex_flow(home_offline, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(home_offline, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_all(home_offline, 10, 0);
	lv_obj_set_style_pad_gap(home_offline, 8, 0);
	lv_obj_set_hidden(home_offline, true);

	lv_obj_t *text = lv_label_create(home_offline);
	lv_label_set_text(text, tr("spotify_needs_wifi"));
	lv_obj_add_style(text, &theme_style_text, 0);
	lv_obj_set_style_text_font(text, &font_ui_16, 0);
	lv_obj_set_width(text, lv_pct(100));
	lv_label_set_long_mode(text, LV_LABEL_LONG_WRAP);
	lv_obj_set_style_text_align(text, LV_TEXT_ALIGN_CENTER, 0);

	lv_obj_t *btn = lv_button_create(home_offline);
	lv_obj_add_style(btn, &theme_style_card, 0);
	lv_obj_add_style(btn, &theme_style_card_pressed, LV_STATE_PRESSED);
	lv_obj_set_height(btn, compact() ? 36 : 44);
	lv_obj_set_width(btn, lv_pct(100));
	lv_obj_add_event_cb(btn, offline_wifi_cb, LV_EVENT_CLICKED, NULL);
	lv_obj_t *btn_label = lv_label_create(btn);
	lv_label_set_text(btn_label, tr("wifisettings"));
	lv_obj_add_style(btn_label, &theme_style_text, 0);
	lv_obj_center(btn_label);

	// Who is signed in, or why nobody is: under the grid, in one line.
	home_account_line = lv_label_create(spotify_screen);
	lv_label_set_text(home_account_line, "");
	lv_obj_add_style(home_account_line, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(home_account_line, &font_ui_14, 0);
	lv_obj_set_width(home_account_line, cfg->screen_width - 2 * cfg->padding);
	lv_label_set_long_mode(home_account_line, LV_LABEL_LONG_DOT);
	lv_obj_align(home_account_line, LV_ALIGN_BOTTOM_MID, 0, -6);
}

static void build_list(gui_config_t *cfg) {
	list_screen = lv_obj_create(NULL);
	list_body = settingsrow_page(list_screen, cfg, "spotify");
	switcher_attach_back_gesture(list_screen);
	lv_obj_add_event_cb(list_screen, list_unloaded_cb, LV_EVENT_SCREEN_UNLOADED, NULL);

	// The count line and the pool of rows, inside the scrolling body.
	lv_obj_t *holder = lv_obj_create(list_body);
	lv_obj_set_size(holder, lv_pct(100), LV_SIZE_CONTENT);
	lv_obj_set_style_bg_opa(holder, 0, 0);
	lv_obj_set_style_border_width(holder, 0, 0);
	lv_obj_set_style_pad_all(holder, 0, 0);
	lv_obj_set_flex_flow(holder, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_style_pad_gap(holder, ROW_GAP, 0);
	lv_obj_set_scrollable(holder, false);
	list_body = holder;
	lv_obj_add_event_cb(list_body, list_scrolled_cb, LV_EVENT_SCROLL, NULL);
	player_sheet_attach_drag(list_body, true);

	list_count = lv_label_create(list_body);
	lv_obj_add_style(list_count, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(list_count, &font_ui_14, 0);
	lv_obj_set_width(list_count, lv_pct(100));
	lv_obj_set_style_pad_bottom(list_count, 2, 0);
	lv_label_set_text(list_count, "");
	lv_obj_set_hidden(list_count, true);

	for (int i = 0; i < ROW_POOL; i++) {
		list_row_t *r = &rows[i];
		r->row = lv_obj_create(list_body);
		lv_obj_add_style(r->row, &theme_style_card, 0);
		lv_obj_set_size(r->row, lv_pct(100), ROW_HEIGHT);
		lv_obj_set_style_pad_all(r->row, 0, 0);
		lv_obj_set_style_pad_hor(r->row, compact() ? 10 : 16, 0);
		lv_obj_set_style_radius(r->row, bp_tile_radius(), 0);
		lv_obj_set_scrollable(r->row, false);
		lv_obj_set_clickable(r->row, true);

		r->num = lv_label_create(r->row);
		lv_obj_add_style(r->num, &theme_style_text_dim, 0);
		lv_obj_set_style_text_font(r->num, &font_ui_14, 0);
		lv_obj_align(r->num, LV_ALIGN_LEFT_MID, 0, 0);

		r->mark = lv_image_create(r->row);
		lv_image_set_src(r->mark, &icon_play);
		lv_obj_align(r->mark, LV_ALIGN_LEFT_MID, 0, 0);
		lv_obj_set_style_image_recolor(r->mark, theme()->accent, 0);
		lv_obj_set_style_image_recolor_opa(r->mark, LV_OPA_COVER, 0);
		lv_obj_set_hidden(r->mark, true);

		r->title = lv_label_create(r->row);
		lv_obj_add_style(r->title, &theme_style_text, 0);
		lv_obj_set_style_text_font(r->title, &font_ui_16, 0);
		lv_obj_set_width(r->title, lv_pct(100) - (compact() ? 26 : 36));
		lv_label_set_long_mode(r->title, LV_LABEL_LONG_DOT);
		lv_obj_align(r->title, LV_ALIGN_TOP_LEFT, compact() ? 22 : 30, compact() ? 7 : 14);

		r->sub = lv_label_create(r->row);
		lv_obj_add_style(r->sub, &theme_style_text_dim, 0);
		lv_obj_set_style_text_font(r->sub, &font_ui_14, 0);
		lv_obj_set_width(r->sub, lv_pct(100) - (compact() ? 26 : 36));
		lv_label_set_long_mode(r->sub, LV_LABEL_LONG_DOT);
		lv_obj_align(r->sub, LV_ALIGN_TOP_LEFT, compact() ? 22 : 30, compact() ? 27 : 44);

		lv_obj_add_event_cb(r->row, row_click_cb, LV_EVENT_CLICKED, r);
		lv_obj_set_hidden(r->row, true);
	}

	// The spinner and the message card sit over the body, at its top.
	list_spinner = spinner_create(list_screen);
	lv_obj_align(list_spinner, LV_ALIGN_TOP_MID, 0, settingsrow_content_top(cfg) + 24);
	lv_obj_set_hidden(list_spinner, true);

	list_message = lv_label_create(list_screen);
	lv_obj_add_style(list_message, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(list_message, &font_ui_16, 0);
	lv_obj_set_width(list_message, cfg->screen_width - 4 * cfg->padding);
	lv_obj_set_style_text_align(list_message, LV_TEXT_ALIGN_CENTER, 0);
	lv_label_set_long_mode(list_message, LV_LABEL_LONG_WRAP);
	lv_obj_align(list_message, LV_ALIGN_TOP_MID, 0, settingsrow_content_top(cfg) + 16);
	lv_obj_set_hidden(list_message, true);
}

static void build_playlists(gui_config_t *cfg) {
	playlists_screen = lv_obj_create(NULL);
	playlists_body = settingsrow_page(playlists_screen, cfg, "playlists");
	switcher_attach_back_gesture(playlists_screen);
	lv_obj_add_event_cb(playlists_screen, playlists_unloaded_cb, LV_EVENT_SCREEN_UNLOADED, NULL);

	playlists_message = lv_label_create(playlists_screen);
	lv_obj_add_style(playlists_message, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(playlists_message, &font_ui_16, 0);
	lv_obj_set_width(playlists_message, cfg->screen_width - 4 * cfg->padding);
	lv_obj_set_style_text_align(playlists_message, LV_TEXT_ALIGN_CENTER, 0);
	lv_label_set_long_mode(playlists_message, LV_LABEL_LONG_WRAP);
	lv_obj_align(playlists_message, LV_ALIGN_TOP_MID, 0, settingsrow_content_top(cfg) + 16);
	lv_obj_set_hidden(playlists_message, true);
}

static void build_search(gui_config_t *cfg) {
	search_screen = lv_obj_create(NULL);
	lv_obj_add_style(search_screen, &theme_style_screen, 0);
	settingsrow_title(search_screen, cfg, "spotify_search");
	switcher_attach_back_gesture(search_screen);

	search_field = lv_textarea_create(search_screen);
	lv_obj_set_width(search_field, cfg->screen_width - 2 * cfg->padding);
	lv_obj_set_height(search_field, compact() ? 38 : 46);
	lv_obj_align(search_field, LV_ALIGN_TOP_MID, 0, settingsrow_content_top(cfg));
	lv_textarea_set_one_line(search_field, true);
	lv_textarea_set_max_length(search_field, 120);
	lv_textarea_set_placeholder_text(search_field, tr("spotify_search_hint"));
	keyboard_style_caret(search_field);

	lv_obj_t *note = lv_label_create(search_screen);
	lv_obj_add_style(note, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(note, &font_ui_14, 0);
	lv_obj_set_width(note, cfg->screen_width - 2 * cfg->padding);
	lv_label_set_long_mode(note, LV_LABEL_LONG_WRAP);
	lv_obj_align(note, LV_ALIGN_TOP_MID, 0, settingsrow_content_top(cfg) + (compact() ? 44 : 52));
	lv_label_set_text(note, tr("spotify_search_note"));

	search_keyboard = keyboard_create(search_screen, cfg->screen_width - 2 * cfg->padding,
									  compact() ? (cfg->screen_height / 2) : 210, search_field, &icon_search, NULL,
									  search_submit_cb, NULL);
	keyboard_set_visible(search_keyboard, false);
}

static void account_shuffle_cb(lv_event_t *e) {
	lv_obj_t *sw = lv_event_get_target_obj(e);
	spotify_set_shuffle(lv_obj_has_state(sw, LV_STATE_CHECKED));
}

static void account_repeat_cb(lv_event_t *e) {
	(void)e;
	spotify_status_t st;
	spotify_status(&st);
	spotify_set_repeat(st.repeat >= 2 ? 0 : st.repeat + 1);
}

static void build_account(gui_config_t *cfg) {
	account_screen = lv_obj_create(NULL);
	lv_obj_t *body = settingsrow_page(account_screen, cfg, "spotify_account");
	switcher_attach_back_gesture(account_screen);

	// The account the session belongs to, in the daemon's own words -- or, with
	// no daemon, the sentence that says so. Polled, so a login that goes in
	// while the page is open shows up without leaving it, which is exactly the
	// order the one-time setup on a desktop happens in.
	lv_obj_t *card = lv_obj_create(body);
	lv_obj_add_style(card, &theme_style_card, 0);
	lv_obj_set_width(card, cfg->screen_width - 2 * cfg->padding);
	lv_obj_set_height(card, compact() ? 62 : 76);
	lv_obj_set_style_pad_all(card, 10, 0);
	lv_obj_set_scrollable(card, false);

	account_name = lv_label_create(card);
	lv_obj_add_style(account_name, &theme_style_text, 0);
	lv_obj_set_style_text_font(account_name, &font_ui_16, 0);
	lv_obj_set_width(account_name, lv_pct(100));
	lv_label_set_long_mode(account_name, LV_LABEL_LONG_DOT);
	lv_obj_align(account_name, LV_ALIGN_TOP_LEFT, 0, 0);
	lv_label_set_text(account_name, "");

	account_backend = lv_label_create(card);
	lv_obj_add_style(account_backend, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(account_backend, &font_ui_14, 0);
	lv_obj_set_width(account_backend, lv_pct(100));
	lv_label_set_long_mode(account_backend, LV_LABEL_LONG_WRAP);
	lv_obj_align(account_backend, LV_ALIGN_TOP_LEFT, 0, compact() ? 24 : 28);
	lv_label_set_text(account_backend, "");

	// The quality is the daemon's, not a setting here: librespot's bitrate
	// lives in its own configuration file (see tools/spotifyd/PROTOCOL.md), and
	// a player that pretended to set it would be lying.
	lv_obj_t *quality = settingsrow_add(body, "spotify_quality", &account_quality, NULL, NULL);
	(void)quality;

	// How to sign out, as a note rather than a row: it is a sentence, not
	// something to press, and a settings row's single line would run off the
	// card in German and Russian, where the sentence is a line longer.
	lv_obj_t *logout_note = lv_label_create(body);
	lv_obj_add_style(logout_note, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(logout_note, &font_ui_14, 0);
	lv_obj_set_width(logout_note, lv_pct(100));
	lv_label_set_long_mode(logout_note, LV_LABEL_LONG_WRAP);
	lv_label_set_text(logout_note, tr("spotify_logout_note"));

	// The session's two modes: the Now Playing page draws them as glyphs, this
	// page as names, and both read the same snapshot.
	lv_obj_t *shuffle_card = lv_obj_create(body);
	lv_obj_add_style(shuffle_card, &theme_style_card, 0);
	lv_obj_set_width(shuffle_card, cfg->screen_width - 2 * cfg->padding);
	lv_obj_set_height(shuffle_card, compact() ? 44 : 54);
	lv_obj_set_style_pad_hor(shuffle_card, 10, 0);
	lv_obj_set_scrollable(shuffle_card, false);
	lv_obj_t *sh_label = lv_label_create(shuffle_card);
	lv_label_set_text(sh_label, tr("spotify_shuffle"));
	lv_obj_add_style(sh_label, &theme_style_text, 0);
	lv_obj_set_style_text_font(sh_label, &font_ui_16, 0);
	// Room for the switch on the right; a long translation is elided rather
	// than run under it.
	lv_obj_set_width(sh_label, lv_pct(66));
	lv_label_set_long_mode(sh_label, LV_LABEL_LONG_DOT);
	lv_obj_align(sh_label, LV_ALIGN_LEFT_MID, 0, 0);
	account_shuffle = lv_switch_create(shuffle_card);
	lv_obj_align(account_shuffle, LV_ALIGN_RIGHT_MID, 0, 0);
	lv_obj_add_event_cb(account_shuffle, account_shuffle_cb, LV_EVENT_VALUE_CHANGED, NULL);

	lv_obj_t *repeat_card = lv_obj_create(body);
	lv_obj_add_style(repeat_card, &theme_style_card, 0);
	lv_obj_set_width(repeat_card, cfg->screen_width - 2 * cfg->padding);
	lv_obj_set_height(repeat_card, compact() ? 44 : 54);
	lv_obj_set_style_pad_hor(repeat_card, 10, 0);
	lv_obj_set_scrollable(repeat_card, false);
	lv_obj_set_clickable(repeat_card, true);
	lv_obj_add_event_cb(repeat_card, account_repeat_cb, LV_EVENT_CLICKED, NULL);
	lv_obj_t *rp_label = lv_label_create(repeat_card);
	lv_label_set_text(rp_label, tr("spotify_repeat_mode"));
	lv_obj_add_style(rp_label, &theme_style_text, 0);
	lv_obj_set_style_text_font(rp_label, &font_ui_16, 0);
	lv_obj_set_width(rp_label, lv_pct(62));
	lv_label_set_long_mode(rp_label, LV_LABEL_LONG_DOT);
	lv_obj_align(rp_label, LV_ALIGN_LEFT_MID, 0, 0);
	account_repeat = lv_label_create(repeat_card);
	lv_obj_add_style(account_repeat, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(account_repeat, &font_ui_14, 0);
	// The value is right-aligned in the remaining third, and stays inside it:
	// "Everything" in a longer language must not push past the card.
	lv_obj_set_width(account_repeat, lv_pct(32));
	lv_obj_set_style_text_align(account_repeat, LV_TEXT_ALIGN_RIGHT, 0);
	lv_label_set_long_mode(account_repeat, LV_LABEL_LONG_DOT);
	lv_obj_align(account_repeat, LV_ALIGN_RIGHT_MID, 0, 0);
	lv_label_set_text(account_repeat, "");

	// The notice: the demo sentence, how a login is done, or what to do about
	// no daemon. Last on the page, because it is the one read only when
	// something is missing.
	account_notice = lv_label_create(body);
	lv_obj_add_style(account_notice, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(account_notice, &font_ui_14, 0);
	lv_obj_set_width(account_notice, lv_pct(100));
	lv_label_set_long_mode(account_notice, LV_LABEL_LONG_WRAP);
	lv_label_set_text(account_notice, "");
	lv_obj_set_hidden(account_notice, true);
}

// ---------------------------------------------------------------------------
// init
// ---------------------------------------------------------------------------

void spotifypage_init(gui_config_t *cfg) {
	g_cfg = cfg;

	spotify_init();
	if (!shared) {
		shared = calloc(1, sizeof(result_t));
	}
	if (!shared) {
		return;
	}

	spotify_screen = lv_obj_create(NULL);
	build_home(cfg);
	build_list(cfg);
	build_playlists(cfg);
	build_search(cfg);
	build_account(cfg);

	refresh_chrome();
	poll_timer = lv_timer_create(poll_cb, POLL_MS, NULL);

	// The grid's tiles are captions from the language file, and the two that
	// name a place rather than a service say what they open.
	(void)home_grid;
	(void)context_name;
	(void)online;
	(void)service_live;
}
