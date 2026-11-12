#include "soundcloudpage.h"

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "src/gui/board_profile.h"
#include "src/gui/fonts/fonts.h"
#include "src/gui/nowplaying/player.h"
#include "src/gui/shell/confirm.h"
#include "src/gui/shell/gridpage.h"
#include "src/gui/shell/icons.h"
#include "src/gui/shell/keyboard.h"
#include "src/gui/shell/settingsrow.h"
#include "src/gui/shell/spinner.h"
#include "src/gui/shell/switcher.h"
#include "src/gui/shell/theme.h"
#include "src/gui/shell/toast.h"
#include "src/system/audio/audio.h"
#include "src/system/core/lang.h"
#include "src/system/core/utils.h"
#include "src/system/device/system.h"
#include "src/system/playback/device_state.h"
#include "src/system/playback/playlist.h"
#include "src/system/streaming/soundcloud.h"
#include "src/system/streaming/soundcloudcache.h"
#include "src/system/streaming/streamturn.h"

#define SC_UI_PAGE_SIZE 30
#define SC_HISTORY_DEPTH 10
#define SC_CURSOR_STACK_MAX 16
#define SC_QUEUE_CAP 256
#define SC_CACHE_PRESET_COUNT 6
// Liked tracks, Reposts, Playlists and the account: a 2x2 grid that fills the
// panel under the heading.
#define SC_HOME_TILES 4
// The size each card's picture is drawn at, matching the size tools/svg_to_lvgl.py
// emits it: SOUNDCLOUD_TILE_SIZE and SOUNDCLOUD_GLYPH_SIZE there.
#define SOUNDCLOUD_TILE_ART_PX 58
#define SOUNDCLOUD_TILE_GLYPH_PX 50

static const int SC_CACHE_PRESET_MB[SC_CACHE_PRESET_COUNT] = {512, 1024, 2048, 4096, 8192, 0};

lv_obj_t *soundcloud_screen;

static gui_config_t *page_cfg;
static lv_obj_t *home_grid;

// The account tile names the account instead of repeating its own destination,
// the way the row it replaces carried the name in its value line.
static char home_account_caption[192];

static lv_obj_t *search_screen;
static lv_obj_t *search_field;
static keyboard_t *search_keyboard;

static lv_obj_t *list_screen;
static lv_obj_t *list_container;
static lv_obj_t *list_title;

static lv_obj_t *settings_screen;
static lv_obj_t *user_value;
static lv_obj_t *token_value;
static lv_obj_t *cid_value;
static lv_obj_t *cache_value;
static lv_obj_t *remember_switch;
static lv_obj_t *quality_pills[3];
static lv_obj_t *cache_limit_pills[SC_CACHE_PRESET_COUNT];

static lv_obj_t *edit_screen;
static lv_obj_t *edit_title;
static lv_obj_t *edit_field;
static keyboard_t *edit_keyboard;

static soundcloud_credentials_t draft;
static bool draft_remember;
static int editing_field;
static bool busy;
static lv_obj_t *job_owner;

typedef enum {
	VIEW_LIKES = 0,
	VIEW_REPOSTS = 1,
	VIEW_PLAYLISTS = 2,
	VIEW_PLAYLIST_TRACKS = 3,
	VIEW_SEARCH = 4,
} view_kind_t;

typedef struct {
	bool is_playlist;
	soundcloud_track_t track;
	soundcloud_playlist_t playlist;
} ui_entry_t;

typedef enum {
	JOB_BROWSE = 0,
	JOB_CONNECT = 1,
	JOB_PLAY = 2,
} job_kind_t;

typedef struct {
	unsigned serial;
	job_kind_t kind;
	view_kind_t view;
	long long playlist_id;
	char secret_token[SOUNDCLOUD_SECRET_MAX];
	char query[SOUNDCLOUD_TITLE_MAX];
	char title[SOUNDCLOUD_TITLE_MAX];
	int offset;
	char cursor[SOUNDCLOUD_CURSOR_MAX];
	int cursor_depth;
	char cursor_prev[SC_CURSOR_STACK_MAX][SOUNDCLOUD_CURSOR_MAX];

	soundcloud_credentials_t credentials;
	bool remember;

	soundcloud_track_t play_track;
	bool start_queue_after_play;
} job_t;

typedef struct {
	job_t job;
	ui_entry_t *entries;
	int count;
	bool more;
	char next_cursor[SOUNDCLOUD_CURSOR_MAX];
	char resolved_path[576];
	char error[192];
} result_t;

static pthread_mutex_t job_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t job_ready = PTHREAD_COND_INITIALIZER;
static bool worker_started;
static bool have_job;
static bool configuring;
static unsigned job_serial;
static unsigned failed_serial;
static job_t pending_job;
static result_t *completed_mail;

static ui_entry_t *shown_entries;
static int shown_count;
static bool shown_more;
static char shown_next_cursor[SOUNDCLOUD_CURSOR_MAX];
static lv_obj_t *shown_rows[SC_UI_PAGE_SIZE];
static job_t current_view;
static job_t nav_history[SC_HISTORY_DEPTH];
static int nav_history_count;

// Track metadata table for the active SoundCloud playback queue so Next/Prev
// and background prefetch can resolve any queued <id>.mp3 placeholder.
static pthread_mutex_t queue_meta_lock = PTHREAD_MUTEX_INITIALIZER;
static soundcloud_track_t queue_tracks[SC_QUEUE_CAP];
static int queue_track_count;
static volatile bool waiting_for_track;
static volatile long long loading_track_id;

// Discreet non-blocking track-loading indicators (replaces the modal Downloading popup).
static lv_obj_t *row_spinner;
static lv_obj_t *row_spinner_parent;
static lv_obj_t *list_corner_spin_wrap;
static lv_obj_t *list_corner_spin;
static lv_obj_t *track_load_badge;
static lv_obj_t *track_load_spin;

// Flex card rows (1-line or stacked 2-line title + value) so text never overlaps on 240x320.
#define SC_MAX_VALUE_ROWS 16
typedef struct {
	lv_obj_t *row;
	lv_obj_t *icon;
	lv_obj_t *text_col;
	lv_obj_t *title;
	lv_obj_t *value;
	theme_semantic_t tone;
} sc_value_row_t;

static sc_value_row_t value_rows[SC_MAX_VALUE_ROWS];
static int value_row_count;

// Separate low-priority prefetch worker so background lookahead never delays UI taps.
static pthread_mutex_t prefetch_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t prefetch_cond = PTHREAD_COND_INITIALIZER;
static bool prefetch_started;
static bool have_prefetch;
static soundcloud_track_t pending_prefetch;

// Async Like worker slot for Now Playing heart button.
typedef struct {
	long long track_id;
	bool target_like;
	void (*done)(bool ok, const char *error, void *user);
	void *user;
} fav_req_t;

static void wipe_mem(void *p, size_t n) {
	volatile unsigned char *v = p;
	while (n--) {
		*v++ = 0;
	}
}

static bool lookup_queued_track(long long id, soundcloud_track_t *out) {
	if (id <= 0) {
		return false;
	}
	pthread_mutex_lock(&queue_meta_lock);
	for (int i = 0; i < queue_track_count; i++) {
		if (queue_tracks[i].id == id) {
			if (out) {
				*out = queue_tracks[i];
			}
			pthread_mutex_unlock(&queue_meta_lock);
			return true;
		}
	}
	pthread_mutex_unlock(&queue_meta_lock);
	return false;
}

static void record_queued_tracks(const soundcloud_track_t *tracks, int count) {
	pthread_mutex_lock(&queue_meta_lock);
	queue_track_count = count < SC_QUEUE_CAP ? count : SC_QUEUE_CAP;
	if (queue_track_count > 0) {
		memcpy(queue_tracks, tracks, (size_t)queue_track_count * sizeof(tracks[0]));
	}
	pthread_mutex_unlock(&queue_meta_lock);
}

bool soundcloudpage_waiting_for_track(void) { return waiting_for_track; }

// ---------------------------------------------------------------------------
// Background prefetch worker & Now Playing async Like worker
// ---------------------------------------------------------------------------

static void *prefetch_worker_main(void *arg) {
	(void)arg;
	thread_be_low_priority("soundcloud prefetch");
	for (;;) {
		pthread_mutex_lock(&prefetch_lock);
		while (!have_prefetch) {
			pthread_cond_wait(&prefetch_cond, &prefetch_lock);
		}
		soundcloud_track_t trk = pending_prefetch;
		have_prefetch = false;
		pthread_mutex_unlock(&prefetch_lock);
		if (trk.id > 0 && !waiting_for_track) {
			(void)soundcloudcache_prefetch(&trk);
		}
	}
	return NULL;
}

static void request_prefetch(long long track_id) {
	soundcloud_track_t trk;
	if (!lookup_queued_track(track_id, &trk)) {
		return;
	}
	if (soundcloudcache_find_playable(track_id, NULL, 0)) {
		return;
	}
	if (!prefetch_started) {
		pthread_t tid;
		pthread_attr_t attr;
		if (pthread_attr_init(&attr) == 0) {
			pthread_attr_setstacksize(&attr, 256 * 1024);
			pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
			if (pthread_create(&tid, &attr, prefetch_worker_main, NULL) == 0) {
				prefetch_started = true;
			}
			pthread_attr_destroy(&attr);
		}
	}
	if (!prefetch_started) {
		return;
	}
	pthread_mutex_lock(&prefetch_lock);
	pending_prefetch = trk;
	have_prefetch = true;
	pthread_cond_signal(&prefetch_cond);
	pthread_mutex_unlock(&prefetch_lock);
}

static void *fav_worker_main(void *arg) {
	fav_req_t *req = arg;
	bool ok = soundcloud_like_track(req->track_id, req->target_like);
	const char *err = ok ? "" : soundcloud_last_error();
	if (req->done) {
		req->done(ok, err && err[0] ? tr(err) : "", req->user);
	}
	free(req);
	return NULL;
}

void soundcloudpage_toggle_favorite_async(long long track_id,
										  void (*done)(bool ok, const char *error, void *user), void *user) {
	if (!soundcloud_can_like()) {
		if (done) {
			done(false, tr("soundcloud_token_needed_for_likes"), user);
		}
		return;
	}
	bool next_state = !soundcloud_is_liked(track_id);
	soundcloud_note_liked(track_id, next_state);

	fav_req_t *req = calloc(1, sizeof(*req));
	if (!req) {
		soundcloud_note_liked(track_id, !next_state);
		if (done) done(false, tr("soundcloud_out_of_memory"), user);
		return;
	}
	req->track_id = track_id;
	req->target_like = next_state;
	req->done = done;
	req->user = user;

	pthread_t tid;
	pthread_attr_t attr;
	pthread_attr_init(&attr);
	pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
	pthread_attr_setstacksize(&attr, 192 * 1024);
	if (pthread_create(&tid, &attr, fav_worker_main, req) != 0) {
		soundcloud_note_liked(track_id, !next_state);
		free(req);
		if (done) done(false, tr("soundcloud_connection_failed"), user);
	}
	pthread_attr_destroy(&attr);
}

// ---------------------------------------------------------------------------
// Main SoundCloud API / playback worker
// ---------------------------------------------------------------------------

static void free_result(result_t *r) {
	if (!r) return;
	free(r->entries);
	wipe_mem(&r->job.credentials, sizeof(r->job.credentials));
	free(r);
}

static void execute_browse_job(const job_t *job, result_t *res) {
	res->entries = calloc(SC_UI_PAGE_SIZE, sizeof(*res->entries));
	if (!res->entries) {
		res->count = -1;
		snprintf(res->error, sizeof(res->error), "soundcloud_out_of_memory");
		return;
	}

	switch (job->view) {
	case VIEW_SEARCH: {
		soundcloud_track_t *tracks = calloc(SC_UI_PAGE_SIZE, sizeof(*tracks));
		if (!tracks) {
			res->count = -1;
			snprintf(res->error, sizeof(res->error), "soundcloud_out_of_memory");
			return;
		}
		int n = soundcloud_search_tracks(job->query, job->offset, tracks, SC_UI_PAGE_SIZE, &res->more);
		if (n < 0) {
			res->count = -1;
			snprintf(res->error, sizeof(res->error), "%s", soundcloud_last_error());
		} else {
			res->count = n;
			for (int i = 0; i < n; i++) {
				res->entries[i].is_playlist = false;
				res->entries[i].track = tracks[i];
			}
		}
		free(tracks);
		break;
	}
	case VIEW_LIKES: {
		soundcloud_track_t *tracks = calloc(SC_UI_PAGE_SIZE, sizeof(*tracks));
		if (!tracks) {
			res->count = -1;
			snprintf(res->error, sizeof(res->error), "soundcloud_out_of_memory");
			return;
		}
		int n = soundcloud_liked_tracks(job->cursor, tracks, SC_UI_PAGE_SIZE, res->next_cursor,
										sizeof(res->next_cursor));
		if (n < 0) {
			res->count = -1;
			snprintf(res->error, sizeof(res->error), "%s", soundcloud_last_error());
		} else {
			res->count = n;
			res->more = res->next_cursor[0] != '\0';
			for (int i = 0; i < n; i++) {
				res->entries[i].is_playlist = false;
				res->entries[i].track = tracks[i];
			}
		}
		free(tracks);
		break;
	}
	case VIEW_REPOSTS: {
		soundcloud_repost_t *reps = calloc(SC_UI_PAGE_SIZE, sizeof(*reps));
		if (!reps) {
			res->count = -1;
			snprintf(res->error, sizeof(res->error), "soundcloud_out_of_memory");
			return;
		}
		int n = soundcloud_reposts(job->cursor, reps, SC_UI_PAGE_SIZE, res->next_cursor, sizeof(res->next_cursor));
		if (n < 0) {
			res->count = -1;
			snprintf(res->error, sizeof(res->error), "%s", soundcloud_last_error());
		} else {
			res->count = n;
			res->more = res->next_cursor[0] != '\0';
			for (int i = 0; i < n; i++) {
				res->entries[i].is_playlist = reps[i].is_playlist;
				res->entries[i].track = reps[i].track;
				res->entries[i].playlist = reps[i].playlist;
			}
		}
		free(reps);
		break;
	}
	case VIEW_PLAYLISTS: {
		soundcloud_playlist_t *pls = calloc(SC_UI_PAGE_SIZE, sizeof(*pls));
		if (!pls) {
			res->count = -1;
			snprintf(res->error, sizeof(res->error), "soundcloud_out_of_memory");
			return;
		}
		int n = soundcloud_playlists(job->cursor, pls, SC_UI_PAGE_SIZE, res->next_cursor, sizeof(res->next_cursor));
		if (n < 0) {
			res->count = -1;
			snprintf(res->error, sizeof(res->error), "%s", soundcloud_last_error());
		} else {
			res->count = n;
			res->more = res->next_cursor[0] != '\0';
			for (int i = 0; i < n; i++) {
				res->entries[i].is_playlist = true;
				res->entries[i].playlist = pls[i];
			}
		}
		free(pls);
		break;
	}
	case VIEW_PLAYLIST_TRACKS: {
		soundcloud_track_t *tracks = calloc(SC_UI_PAGE_SIZE, sizeof(*tracks));
		if (!tracks) {
			res->count = -1;
			snprintf(res->error, sizeof(res->error), "soundcloud_out_of_memory");
			return;
		}
		int n = soundcloud_playlist_tracks(job->playlist_id, job->secret_token, job->offset, tracks, SC_UI_PAGE_SIZE,
										   &res->more);
		if (n < 0) {
			res->count = -1;
			snprintf(res->error, sizeof(res->error), "%s", soundcloud_last_error());
		} else {
			res->count = n;
			for (int i = 0; i < n; i++) {
				res->entries[i].is_playlist = false;
				res->entries[i].track = tracks[i];
			}
		}
		free(tracks);
		break;
	}
	}
}

static void *worker_main(void *arg) {
	(void)arg;
	thread_be_low_priority("soundcloud API");
	for (;;) {
		pthread_mutex_lock(&job_lock);
		while (!have_job) {
			pthread_cond_wait(&job_ready, &job_lock);
		}
		job_t job = pending_job;
		memset(&pending_job, 0, sizeof(pending_job));
		have_job = false;
		if (job.serial != job_serial) {
			wipe_mem(&job.credentials, sizeof(job.credentials));
			pthread_mutex_unlock(&job_lock);
			continue;
		}
		configuring = (job.kind == JOB_CONNECT);
		pthread_mutex_unlock(&job_lock);

		result_t *res = calloc(1, sizeof(*res));
		if (!res) {
			pthread_mutex_lock(&job_lock);
			free_result(completed_mail);
			completed_mail = NULL;
			pthread_mutex_unlock(&job_lock);
			res = calloc(1, sizeof(*res));
		}

		if (res) {
			res->job = job;
			wipe_mem(&res->job.credentials, sizeof(res->job.credentials));
			if (job.kind == JOB_BROWSE) {
				execute_browse_job(&job, res);
			} else if (job.kind == JOB_CONNECT) {
				bool ok = soundcloud_configure(&job.credentials, job.remember, res->error, sizeof(res->error));
				res->count = ok ? 0 : -1;
			} else if (job.kind == JOB_PLAY) {
				bool ok = soundcloudcache_get(&job.play_track, res->resolved_path, sizeof(res->resolved_path));
				res->count = ok ? 0 : -1;
				if (!ok) {
					snprintf(res->error, sizeof(res->error), "%s", soundcloudcache_last_error());
				}
			}
		}

		wipe_mem(&job.credentials, sizeof(job.credentials));
		pthread_mutex_lock(&job_lock);
		configuring = false;
		free_result(completed_mail);
		completed_mail = res;
		if (!res) {
			failed_serial = job.serial;
		}
		pthread_mutex_unlock(&job_lock);
	}
	return NULL;
}

static bool ensure_worker(void) {
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
		toast_error("soundcloud_out_of_memory");
		return false;
	}
	pthread_detach(worker);
	worker_started = true;
	return true;
}

static bool submit_job(job_t *job, lv_obj_t *owner, const char *busy_key) {
	if (!ensure_worker()) {
		return false;
	}
	pthread_mutex_lock(&job_lock);
	job->serial = ++job_serial;
	pending_job = *job;
	have_job = true;
	pthread_cond_signal(&job_ready);
	pthread_mutex_unlock(&job_lock);

	busy = true;
	job_owner = owner;
	if (busy_key) {
		toast_busy(busy_key);
	}
	return true;
}

static void set_row_chevron_hidden(lv_obj_t *row, bool hidden) {
	if (!row) {
		return;
	}
	uint32_t count = lv_obj_get_child_count(row);
	for (uint32_t i = 0; i < count; i++) {
		lv_obj_t *child = lv_obj_get_child(row, (int32_t)i);
		if (lv_obj_check_type(child, &lv_image_class) && lv_image_get_src(child) == &icon_chevron_right) {
			lv_obj_set_hidden(child, hidden);
			return;
		}
	}
}

static void clear_list_container(void) {
	memset(shown_rows, 0, sizeof(shown_rows));
	row_spinner = NULL;
	row_spinner_parent = NULL;
	if (list_container) {
		lv_obj_clean(list_container);
	}
}

static void refresh_track_loading_ui(void) {
	bool active = waiting_for_track && loading_track_id > 0;
	lv_obj_t *target_row = NULL;

	if (active && shown_entries) {
		int lim = shown_count < SC_UI_PAGE_SIZE ? shown_count : SC_UI_PAGE_SIZE;
		for (int i = 0; i < lim; i++) {
			if (!shown_entries[i].is_playlist && shown_entries[i].track.id == loading_track_id && shown_rows[i]) {
				target_row = shown_rows[i];
				break;
			}
		}
	}

	if (row_spinner_parent && (!active || row_spinner_parent != target_row)) {
		if (row_spinner) {
			lv_obj_delete(row_spinner);
			row_spinner = NULL;
		}
		set_row_chevron_hidden(row_spinner_parent, false);
		row_spinner_parent = NULL;
	}

	if (active && target_row && !row_spinner) {
		set_row_chevron_hidden(target_row, true);
		row_spinner = spinner_create(target_row, &icon_loader_small);
		lv_obj_set_clickable(row_spinner, false);
		if (bp_is_tempotec_v1()) {
			lv_image_set_scale(row_spinner, 192);
		}
		lv_obj_set_style_image_recolor(row_spinner, theme()->accent, 0);
		lv_obj_set_style_image_recolor_opa(row_spinner, LV_OPA_COVER, 0);
		row_spinner_parent = target_row;
	}

	if (list_corner_spin_wrap) {
		if (list_corner_spin) {
			lv_obj_set_style_image_recolor(list_corner_spin, theme()->accent, 0);
		}
		lv_obj_set_hidden(list_corner_spin_wrap, !(active && target_row == NULL));
	}

	if (track_load_badge) {
		if (track_load_spin) {
			lv_obj_set_style_image_recolor(track_load_spin, theme()->accent, 0);
		}
		bool list_visible = (lv_screen_active() == list_screen) && !player_sheet_is_open();
		if (active && !list_visible) {
			lv_obj_set_hidden(track_load_badge, false);
			lv_obj_move_foreground(track_load_badge);
		} else {
			lv_obj_set_hidden(track_load_badge, true);
		}
	}
}

static void track_loading_async(void *user) {
	(void)user;
	refresh_track_loading_ui();
}

static void invalidate_job(void) {
	pthread_mutex_lock(&job_lock);
	job_serial++;
	have_job = false;
	wipe_mem(&pending_job, sizeof(pending_job));
	free_result(completed_mail);
	completed_mail = NULL;
	pthread_mutex_unlock(&job_lock);
	busy = false;
	waiting_for_track = false;
	loading_track_id = 0;
	job_owner = NULL;
	toast_busy_end();
	refresh_track_loading_ui();
}

static void page_left_cb(lv_event_t *e) {
	if (busy && job_owner && lv_event_get_target(e) == job_owner) {
		invalidate_job();
	}
}

// ---------------------------------------------------------------------------
// Queue preparation callback for device_state (Next / Prev / Auto-advance)
// ---------------------------------------------------------------------------

bool soundcloudpage_prepare_track(char *path, size_t path_size) {
	if (!soundcloudcache_is_soundcloud_path(path)) {
		waiting_for_track = false;
		loading_track_id = 0;
		gui_post(track_loading_async, NULL);
		return true;
	}

	long long id = soundcloudcache_track_id_of(path);
	char real[576] = "";
	if (id > 0 && soundcloudcache_find_playable(id, real, sizeof(real))) {
		if (real[0] && strcmp(real, path) != 0) {
			if (strlen(real) >= path_size) {
				return false;
			}
			snprintf(path, path_size, "%s", real);
		}
		soundcloudcache_protect(path);
		waiting_for_track = false;
		loading_track_id = 0;
		gui_post(track_loading_async, NULL);
		return true;
	}

	soundcloud_track_t trk = {0};
	if (!lookup_queued_track(id, &trk)) {
		trk.id = id;
		snprintf(trk.title, sizeof(trk.title), "SoundCloud #%lld", id);
		snprintf(trk.album, sizeof(trk.album), "SoundCloud");
	}

	waiting_for_track = true;
	loading_track_id = id;
	device_state_stop();

	if (ensure_worker()) {
		job_t job = {
			.kind = JOB_PLAY,
			.play_track = trk,
			.start_queue_after_play = true,
		};
		pthread_mutex_lock(&job_lock);
		job.serial = ++job_serial;
		pending_job = job;
		have_job = true;
		pthread_cond_signal(&job_ready);
		pthread_mutex_unlock(&job_lock);
		busy = true;
		job_owner = NULL;
	}
	gui_post(track_loading_async, NULL);
	return false;
}

// ---------------------------------------------------------------------------
// UI state refreshes & navigation
// ---------------------------------------------------------------------------

// The signed-in name when there is one, the translated destination otherwise:
// the tile says which account its settings belong to.
static void refresh_home_summary(void) {
	if (!home_grid) {
		return;
	}
	soundcloud_account_info_t info;
	soundcloud_account_info(&info);
	char summary[192];
	if (info.display_name[0]) {
		if (info.tier[0] && strcmp(info.tier, "Public") != 0) {
			snprintf(summary, sizeof(summary), "%s (%s)", info.display_name, info.tier);
		} else {
			snprintf(summary, sizeof(summary), "%s", info.display_name);
		}
	} else if (info.has_token) {
		snprintf(summary, sizeof(summary), "%s", tr("soundcloud_connected"));
	} else {
		snprintf(summary, sizeof(summary), "%s", tr("soundcloud_account_settings"));
	}
	if (strcmp(summary, home_account_caption) != 0) {
		snprintf(home_account_caption, sizeof(home_account_caption), "%s", summary);
		gridpage_set_tile_caption(home_grid, SC_HOME_TILES - 1, home_account_caption);
	}
}

static void refresh_quality_pills(void) {
	int q = soundcloud_get_quality();
	for (int i = 0; i < 3; i++) {
		if (quality_pills[i]) {
			settingsrow_pill_active(quality_pills[i], i == q);
		}
	}
}

static void refresh_cache_limit_pills(void) {
	int cur_mb = soundcloudcache_get_max_mb();
	for (int i = 0; i < SC_CACHE_PRESET_COUNT; i++) {
		if (cache_limit_pills[i]) {
			settingsrow_pill_active(cache_limit_pills[i], SC_CACHE_PRESET_MB[i] == cur_mb);
		}
	}
}

static void refresh_cache_label(void) {
	if (!cache_value) {
		return;
	}
	int files = 0;
	long long bytes = 0;
	soundcloudcache_stats(&files, &bytes);
	int max_mb = soundcloudcache_get_max_mb();

	char used_str[32];
	if (bytes >= 1024LL * 1024LL * 1024LL) {
		snprintf(used_str, sizeof(used_str), "%.2f GB", (double)bytes / (1024.0 * 1024.0 * 1024.0));
	} else {
		snprintf(used_str, sizeof(used_str), "%.1f MB", (double)bytes / (1024.0 * 1024.0));
	}

	char cap_str[64];
	if (max_mb <= 0) {
		snprintf(cap_str, sizeof(cap_str), "%s", tr("soundcloud_cache_unlimited"));
	} else if (max_mb >= 1024 && (max_mb % 1024) == 0) {
		snprintf(cap_str, sizeof(cap_str), "%d GB", max_mb / 1024);
	} else {
		snprintf(cap_str, sizeof(cap_str), "%d MB", max_mb);
	}

	char buf[128];
	snprintf(buf, sizeof(buf), "%d · %s / %s", files, used_str, cap_str);
	lv_label_set_text(cache_value, buf);
}

static void refresh_settings_ui(void) {
	if (!settings_screen) {
		return;
	}
	lv_label_set_text(user_value, draft.username[0] ? draft.username : tr("soundcloud_not_set"));
	lv_label_set_text(token_value, draft.oauth_token[0] ? "••••••••" : tr("soundcloud_not_set"));
	lv_label_set_text(cid_value, draft.client_id[0] ? draft.client_id : tr("soundcloud_auto"));
	if (draft_remember) {
		lv_obj_add_state(remember_switch, LV_STATE_CHECKED);
	} else {
		lv_obj_remove_state(remember_switch, LV_STATE_CHECKED);
	}
	refresh_quality_pills();
	refresh_cache_limit_pills();
	refresh_cache_label();
	refresh_home_summary();
}

static void cancel_track_wait_if_active(void) {
	if (waiting_for_track) {
		invalidate_job();
		streamturn_abandon_all();
	}
}

static void open_settings_cb(lv_event_t *e) {
	(void)e;
	if ((busy && !waiting_for_track) || switcher_back_drag_active() || player_sheet_drag_active()) {
		return;
	}
	cancel_track_wait_if_active();
	soundcloud_credentials(&draft, &draft_remember);
	refresh_settings_ui();
	switch_screen(settings_screen);
}

static void open_search_cb(lv_event_t *e) {
	(void)e;
	if ((busy && !waiting_for_track) || switcher_back_drag_active() || player_sheet_drag_active()) {
		return;
	}
	cancel_track_wait_if_active();
	keyboard_reset(search_keyboard);
	keyboard_set_visible(search_keyboard, true);
	switch_screen(search_screen);
}

static void run_view(job_t job, bool push_history) {
	if (job.view != VIEW_SEARCH && job.view != VIEW_PLAYLIST_TRACKS && !soundcloud_has_account()) {
		open_settings_cb(NULL);
		return;
	}
	if (push_history && lv_screen_active() == list_screen && nav_history_count < SC_HISTORY_DEPTH) {
		nav_history[nav_history_count++] = current_view;
	} else if (lv_screen_active() != list_screen) {
		nav_history_count = 0;
	}

	current_view = job;
	free(shown_entries);
	shown_entries = NULL;
	shown_count = 0;
	shown_more = false;
	shown_next_cursor[0] = '\0';
	clear_list_container();
	lv_label_set_text(list_title, job.title);
	settingsrow_title_realign(list_title);

	if (lv_screen_active() != list_screen) {
		switch_screen(list_screen);
	}
	submit_job(&job, list_screen, "loading");
}

static bool list_back_has_step(void) { return nav_history_count > 0; }

static bool list_back_guard(void) {
	if (nav_history_count > 0) {
		run_view(nav_history[--nav_history_count], false);
		return true;
	}
	invalidate_job();
	return false;
}

static void browse_section(view_kind_t view) {
	if ((busy && !waiting_for_track) || switcher_back_drag_active() || player_sheet_drag_active()) {
		return;
	}
	cancel_track_wait_if_active();
	const char *title_key = "soundcloud_likes";
	if (view == VIEW_REPOSTS) title_key = "soundcloud_reposts";
	else if (view == VIEW_PLAYLISTS) title_key = "soundcloud_playlists";

	job_t job = {
		.kind = JOB_BROWSE,
		.view = view,
	};
	snprintf(job.title, sizeof(job.title), "%s", tr(title_key));
	run_view(job, false);
}

// Tiles call a plain action: the grid already drops a tap that was really a
// swipe, and nothing here needs the event.
static void home_likes_action(void) { browse_section(VIEW_LIKES); }
static void home_reposts_action(void) { browse_section(VIEW_REPOSTS); }
static void home_playlists_action(void) { browse_section(VIEW_PLAYLISTS); }
static void home_settings_action(void) { open_settings_cb(NULL); }

static void accept_search_cb(lv_event_t *e) {
	(void)e;
	if (busy && !waiting_for_track) {
		return;
	}
	const char *q = lv_textarea_get_text(search_field);
	if (!q || !q[0]) {
		return;
	}
	cancel_track_wait_if_active();
	job_t job = {
		.kind = JOB_BROWSE,
		.view = VIEW_SEARCH,
		.offset = 0,
	};
	snprintf(job.query, sizeof(job.query), "%s", q);
	snprintf(job.title, sizeof(job.title), "%s", q);
	nav_history_count = 0;
	current_view = job;
	free(shown_entries);
	shown_entries = NULL;
	shown_count = 0;
	shown_more = false;
	clear_list_container();
	lv_label_set_text(list_title, job.title);
	settingsrow_title_realign(list_title);
	switch_screen(list_screen);
	submit_job(&job, list_screen, "loading");
}

static void page_step_cb(lv_event_t *e) {
	if (busy && !waiting_for_track) {
		return;
	}
	cancel_track_wait_if_active();
	int dir = (int)(intptr_t)lv_event_get_user_data(e);
	job_t job = current_view;

	if (job.view == VIEW_SEARCH || job.view == VIEW_PLAYLIST_TRACKS) {
		int next_off = job.offset + dir * SC_UI_PAGE_SIZE;
		if (next_off < 0) return;
		job.offset = next_off;
		run_view(job, false);
		return;
	}

	if (dir > 0 && shown_next_cursor[0]) {
		if (job.cursor_depth < SC_CURSOR_STACK_MAX) {
			snprintf(job.cursor_prev[job.cursor_depth++], sizeof(job.cursor_prev[0]), "%s", job.cursor);
		}
		snprintf(job.cursor, sizeof(job.cursor), "%s", shown_next_cursor);
		run_view(job, false);
	} else if (dir < 0 && job.cursor_depth > 0) {
		job.cursor_depth--;
		snprintf(job.cursor, sizeof(job.cursor), "%s", job.cursor_prev[job.cursor_depth]);
		run_view(job, false);
	}
}

static void format_duration_caption(const soundcloud_track_t *t, char *out, size_t size) {
	int mins = t->duration / 60;
	int secs = t->duration % 60;
	if (t->preview) {
		snprintf(out, size, "%s · %d:%02d (%s)", t->artist[0] ? t->artist : "SoundCloud", mins, secs,
				 tr("soundcloud_preview_tag"));
	} else if (t->duration > 0) {
		snprintf(out, size, "%s · %d:%02d", t->artist[0] ? t->artist : "SoundCloud", mins, secs);
	} else {
		snprintf(out, size, "%s", t->artist[0] ? t->artist : "SoundCloud");
	}
}

static void entry_tap_cb(lv_event_t *e) {
	if ((busy && !waiting_for_track) || switcher_back_drag_active() || player_sheet_drag_active()) {
		return;
	}
	int index = (int)(intptr_t)lv_event_get_user_data(e);
	if (index < 0 || index >= shown_count) {
		return;
	}

	const ui_entry_t *item = &shown_entries[index];
	if (item->is_playlist) {
		cancel_track_wait_if_active();
		job_t job = {
			.kind = JOB_BROWSE,
			.view = VIEW_PLAYLIST_TRACKS,
			.playlist_id = item->playlist.id,
			.offset = 0,
		};
		snprintf(job.secret_token, sizeof(job.secret_token), "%s", item->playlist.secret_token);
		snprintf(job.title, sizeof(job.title), "%s", item->playlist.title);
		run_view(job, true);
		return;
	}

	if (!item->track.streamable) {
		toast_error("soundcloud_track_unavailable");
		return;
	}
	if (!soundcloudcache_available()) {
		soundcloudcache_set_root(storage_sd_root());
		if (!soundcloudcache_available()) {
			toast_error("soundcloud_no_card");
			return;
		}
	}

	if (waiting_for_track) {
		invalidate_job();
	}

	// Build a clean playback queue containing every playable track in the list.
	soundcloud_track_t *qtracks = calloc((size_t)shown_count, sizeof(*qtracks));
	char (*qpaths)[576] = calloc((size_t)shown_count, sizeof(*qpaths));
	const char **qptrs = calloc((size_t)shown_count, sizeof(*qptrs));
	if (!qtracks || !qpaths || !qptrs) {
		free(qtracks);
		free(qpaths);
		free(qptrs);
		toast_error("soundcloud_out_of_memory");
		return;
	}

	int qcount = 0;
	int selected_slot = 0;
	for (int i = 0; i < shown_count && qcount < SC_QUEUE_CAP; i++) {
		if (shown_entries[i].is_playlist || !shown_entries[i].track.streamable) {
			continue;
		}
		if (!soundcloudcache_placeholder_path(shown_entries[i].track.id, qpaths[qcount], sizeof(qpaths[0]))) {
			continue;
		}
		if (i == index) {
			selected_slot = qcount;
		}
		qtracks[qcount] = shown_entries[i].track;
		qptrs[qcount] = qpaths[qcount];
		qcount++;
	}

	if (qcount == 0) {
		free(qtracks);
		free(qpaths);
		free(qptrs);
		toast_error("soundcloud_track_unavailable");
		return;
	}

	record_queued_tracks(qtracks, qcount);
	streamturn_abandon_all();

	// If already cached on card, play immediately without spinner!
	if (soundcloudcache_find_playable(item->track.id, NULL, 0)) {
		waiting_for_track = false;
		loading_track_id = 0;
		refresh_track_loading_ui();
		device_state_play_list(qptrs, qcount, selected_slot);
		player_refresh_now_playing();
		player_sheet_open(true);
		free(qtracks);
		free(qpaths);
		free(qptrs);
		return;
	}

	// Otherwise stop current playback, load the queue at `selected_slot` once
	// the prebuffer arrives on our worker thread — showing a discreet inline
	// spinner on the tapped row instead of a blocking "Downloading" popup.
	device_state_stop();
	playlist_load_paths(qptrs, qcount, selected_slot);
	free(qtracks);
	free(qpaths);
	free(qptrs);

	waiting_for_track = true;
	loading_track_id = item->track.id;
	job_t job = {
		.kind = JOB_PLAY,
		.play_track = item->track,
		.start_queue_after_play = false,
	};
	if (submit_job(&job, NULL, NULL)) {
		refresh_track_loading_ui();
	} else {
		waiting_for_track = false;
		loading_track_id = 0;
		refresh_track_loading_ui();
	}
}

static lv_obj_t *make_list_entry_row(const char *title_txt, const char *sub_txt, const lv_image_dsc_t *icon_src,
									 theme_semantic_t tone, bool dimmed, bool with_chevron, lv_event_cb_t cb,
									 void *user_data) {
	bool compact = bp_is_tempotec_v1();
	bool fn_glyphs = theme_uses_function_glyphs();
	bool flat = compact && fn_glyphs;

	lv_obj_t *row = lv_btn_create(list_container);
	lv_obj_set_width(row, lv_pct(100));
	lv_obj_set_height(row, LV_SIZE_CONTENT);
	lv_obj_set_style_min_height(row, sub_txt ? (compact ? 56 : 88) : (compact ? 44 : 76), 0);
	lv_obj_add_style(row, &theme_style_card, 0);
	lv_obj_add_style(row, &theme_style_card_pressed, LV_STATE_PRESSED);
	lv_obj_set_style_radius(row, compact ? bp_tile_radius() : 12, 0);
	if (flat) {
		lv_obj_set_style_border_width(row, 0, 0);
		lv_obj_set_style_outline_width(row, 0, 0);
	}
	lv_obj_set_style_shadow_width(row, 0, 0);
	lv_obj_set_style_pad_hor(row, compact ? 8 : 16, 0);
	lv_obj_set_style_pad_ver(row, compact ? 7 : 12, 0);
	lv_obj_set_style_pad_column(row, compact ? 8 : 12, 0);
	lv_obj_set_scrollable(row, false);
	lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_event_bubble(row, true);
	if (cb) {
		lv_obj_add_event_cb(row, cb, LV_EVENT_CLICKED, user_data);
	}

	if (icon_src && fn_glyphs) {
		lv_obj_t *icon = lv_image_create(row);
		lv_image_set_src(icon, icon_src);
		lv_obj_add_style(icon, &theme_style_icon, 0);
		lv_obj_set_style_image_recolor(icon, theme_semantic_color(tone), 0);
		lv_obj_set_style_image_recolor_opa(icon, LV_OPA_COVER, 0);
		lv_obj_set_clickable(icon, false);
		lv_obj_set_scrollable(icon, false);
		int sw = (int)icon_src->header.w;
		int sh = (int)icon_src->header.h;
		int max_side = LV_MAX(sw, sh);
		int target = compact ? 18 : 24;
		if (max_side > 0) {
			lv_obj_set_size(icon, LV_MAX(1, sw * target / max_side), LV_MAX(1, sh * target / max_side));
			lv_image_set_inner_align(icon, LV_IMAGE_ALIGN_CENTER);
			lv_image_set_scale(icon, (uint32_t)(LV_SCALE_NONE * target / max_side));
		}
	}

	lv_obj_t *col = lv_obj_create(row);
	lv_obj_remove_style_all(col);
	lv_obj_set_flex_grow(col, 1);
	lv_obj_set_height(col, LV_SIZE_CONTENT);
	lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_style_pad_row(col, 2, 0);
	lv_obj_set_clickable(col, false);
	lv_obj_set_scrollable(col, false);
	lv_obj_set_event_bubble(col, true);

	const lv_font_t *tf = compact ? &font_ui_18 : &font_ui_22;
	lv_obj_t *title = lv_label_create(col);
	lv_label_set_text(title, title_txt ? title_txt : "");
	lv_obj_add_style(title, &theme_style_text, 0);
	lv_obj_set_style_text_font(title, tf, 0);
	if (dimmed) {
		lv_obj_set_style_text_color(title, theme()->text_secondary, 0);
	}
	lv_obj_set_width(title, lv_pct(100));
	lv_obj_set_height(title, lv_font_get_line_height(tf));
	lv_label_set_long_mode(title, LV_LABEL_LONG_DOT);
	lv_obj_set_clickable(title, false);

	if (sub_txt && sub_txt[0]) {
		const lv_font_t *sf = compact ? &font_ui_14 : &font_ui_18;
		lv_obj_t *sub = lv_label_create(col);
		lv_label_set_text(sub, sub_txt);
		lv_obj_add_style(sub, &theme_style_text_dim, 0);
		lv_obj_set_style_text_font(sub, sf, 0);
		lv_obj_set_width(sub, lv_pct(100));
		lv_obj_set_height(sub, lv_font_get_line_height(sf));
		lv_label_set_long_mode(sub, LV_LABEL_LONG_DOT);
		lv_obj_set_clickable(sub, false);
	}

	if (with_chevron) {
		lv_obj_t *chev = lv_image_create(row);
		lv_image_set_src(chev, &icon_chevron_right);
		lv_obj_add_style(chev, &theme_style_icon, 0);
		lv_obj_set_style_image_opa(chev, LV_OPA_60, 0);
		lv_obj_set_clickable(chev, false);
		lv_obj_set_scrollable(chev, false);
		if (compact) {
			int cw = (int)icon_chevron_right.header.w;
			int ch = (int)icon_chevron_right.header.h;
			int cmax = LV_MAX(cw, ch);
			int ctarget = 20;
			if (cmax > 0) {
				lv_obj_set_size(chev, LV_MAX(1, cw * ctarget / cmax), LV_MAX(1, ch * ctarget / cmax));
				lv_image_set_inner_align(chev, LV_IMAGE_ALIGN_CENTER);
				lv_image_set_scale(chev, (uint32_t)(LV_SCALE_NONE * ctarget / cmax));
			}
		}
	}

	return row;
}

static void rebuild_list_ui(void) {
	clear_list_container();
	lv_obj_scroll_to_y(list_container, 0, LV_ANIM_OFF);
	settingsrow_title_corner_slots(list_title, page_cfg, 1);

	bool has_prev = (current_view.view == VIEW_SEARCH || current_view.view == VIEW_PLAYLIST_TRACKS)
						? (current_view.offset > 0)
						: (current_view.cursor_depth > 0);
	if (has_prev) {
		make_list_entry_row(tr("soundcloud_previous_page"), NULL, NULL, THEME_SEMANTIC_BLUE, false, false,
							page_step_cb, (void *)(intptr_t)-1);
	}

	if (shown_count == 0) {
		lv_obj_t *empty = lv_label_create(list_container);
		lv_obj_add_style(empty, &theme_style_text_dim, 0);
		lv_obj_set_width(empty, lv_pct(100));
		lv_label_set_long_mode(empty, LV_LABEL_LONG_WRAP);
		lv_label_set_text(empty, tr("soundcloud_empty"));
	}

	for (int i = 0; i < shown_count; i++) {
		const ui_entry_t *item = &shown_entries[i];
		lv_obj_t *row = NULL;
		if (item->is_playlist) {
			char cap[192];
			snprintf(cap, sizeof(cap), "%s · %d", item->playlist.owner[0] ? item->playlist.owner : "SoundCloud",
					 item->playlist.track_count);
			row = make_list_entry_row(item->playlist.title, cap, &icon_files_playlist, THEME_SEMANTIC_PURPLE, false,
									  true, entry_tap_cb, (void *)(intptr_t)i);
		} else {
			char cap[220];
			format_duration_caption(&item->track, cap, sizeof(cap));
			row = make_list_entry_row(item->track.title, cap, &icon_music_note,
									  item->track.streamable ? THEME_SEMANTIC_CYAN : THEME_SEMANTIC_RED,
									  !item->track.streamable, true, entry_tap_cb, (void *)(intptr_t)i);
		}
		if (i < SC_UI_PAGE_SIZE) {
			shown_rows[i] = row;
		}
	}

	if (shown_more) {
		make_list_entry_row(tr("soundcloud_next_page"), NULL, NULL, THEME_SEMANTIC_BLUE, false, false, page_step_cb,
							(void *)(intptr_t)1);
	}
	refresh_track_loading_ui();
}

// ---------------------------------------------------------------------------
// Account Settings callbacks
// ---------------------------------------------------------------------------

static void edit_field_cb(lv_event_t *e) {
	if ((busy && !waiting_for_track) || switcher_back_drag_active() || player_sheet_drag_active()) {
		return;
	}
	cancel_track_wait_if_active();
	editing_field = (int)(intptr_t)lv_event_get_user_data(e);
	const char *titles[3] = {"soundcloud_username", "soundcloud_oauth_token", "soundcloud_client_id"};
	const char *placeholders[3] = {"soundcloud.com/your-name", "2-123456-...", "Auto"};
	const char *vals[3] = {draft.username, draft.oauth_token, draft.client_id};
	const size_t caps[3] = {sizeof(draft.username), sizeof(draft.oauth_token), sizeof(draft.client_id)};

	lv_label_set_text(edit_title, tr(titles[editing_field]));
	settingsrow_title_realign(edit_title);
	lv_textarea_set_max_length(edit_field, (uint32_t)caps[editing_field] - 1);
	lv_textarea_set_password_mode(edit_field, false);
	lv_textarea_set_placeholder_text(edit_field, placeholders[editing_field]);
	lv_textarea_set_text(edit_field, vals[editing_field]);
	keyboard_reset(edit_keyboard);
	keyboard_set_visible(edit_keyboard, true);
	switch_screen(edit_screen);
}

static void accept_edit_cb(lv_event_t *e) {
	(void)e;
	soundcloud_credentials_t candidate = draft;
	char *dests[3] = {candidate.username, candidate.oauth_token, candidate.client_id};
	const size_t caps[3] = {sizeof(candidate.username), sizeof(candidate.oauth_token), sizeof(candidate.client_id)};
	const char *txt = lv_textarea_get_text(edit_field);
	if (strlen(txt) >= caps[editing_field]) {
		wipe_mem(&candidate, sizeof(candidate));
		toast_error("soundcloud_invalid_credentials");
		return;
	}
	snprintf(dests[editing_field], caps[editing_field], "%s", txt);

	char err[192] = "";
	if (!soundcloud_save_draft(&candidate, draft_remember, err, sizeof(err))) {
		wipe_mem(&candidate, sizeof(candidate));
		toast_error(err[0] ? err : "soundcloud_invalid_credentials");
		return;
	}
	wipe_mem(&candidate, sizeof(candidate));

	soundcloud_credentials(&draft, &draft_remember);
	lv_textarea_set_text(edit_field, "");
	refresh_settings_ui();
	switch_screen_return_to(settings_screen);
}

static void edit_unloaded_cb(lv_event_t *e) {
	(void)e;
	lv_textarea_set_text(edit_field, "");
}

static void remember_toggle_cb(lv_event_t *e) {
	draft_remember = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
	char err[192] = "";
	if (!soundcloud_save_draft(&draft, draft_remember, err, sizeof(err))) {
		soundcloud_credentials(&draft, &draft_remember);
		refresh_settings_ui();
		toast_error(err[0] ? err : "soundcloud_cannot_save_credentials");
	}
}

static void quality_pill_cb(lv_event_t *e) {
	int q = (int)(intptr_t)lv_event_get_user_data(e);
	soundcloud_set_quality(q);
	refresh_quality_pills();
}

static void cache_limit_pill_cb(lv_event_t *e) {
	int mb = (int)(intptr_t)lv_event_get_user_data(e);
	soundcloudcache_set_max_mb(mb);
	refresh_cache_limit_pills();
	refresh_cache_label();
}

static void connect_save_cb(lv_event_t *e) {
	(void)e;
	if ((busy && !waiting_for_track) || switcher_back_drag_active() || player_sheet_drag_active()) {
		return;
	}
	cancel_track_wait_if_active();
	job_t job = {
		.kind = JOB_CONNECT,
		.credentials = draft,
		.remember = draft_remember,
	};
	submit_job(&job, settings_screen, "loading");
	wipe_mem(&job.credentials, sizeof(job.credentials));
}

static void clear_cache_confirm(void *user) {
	(void)user;
	cancel_track_wait_if_active();
	char cur[512];
	audio_get_current_file(cur, sizeof(cur));
	if (soundcloudcache_is_soundcloud_path(cur)) {
		device_state_stop();
	}
	soundcloudcache_clear();
	refresh_cache_label();
	toast_success("soundcloud_cache_cleared");
}

static void clear_cache_cb(lv_event_t *e) {
	(void)e;
	if ((!busy || waiting_for_track) && !switcher_back_drag_active() && !player_sheet_drag_active()) {
		confirm_show("soundcloud_clear_cache", NULL, NULL, clear_cache_confirm, NULL);
	}
}

static void forget_confirm(void *user) {
	(void)user;
	invalidate_job();
	pthread_mutex_lock(&job_lock);
	bool saving = configuring;
	pthread_mutex_unlock(&job_lock);
	if (saving) {
		toast_error("soundcloud_settings_busy");
		return;
	}
	char cur[512];
	audio_get_current_file(cur, sizeof(cur));
	if (soundcloudcache_is_soundcloud_path(cur)) {
		device_state_stop();
	}
	bool removed = soundcloud_forget();
	wipe_mem(&draft, sizeof(draft));
	draft_remember = true;
	free(shown_entries);
	shown_entries = NULL;
	shown_count = 0;
	nav_history_count = 0;
	clear_list_container();
	refresh_settings_ui();
	if (removed) {
		toast_success("soundcloud_forgotten");
	} else {
		toast_error("soundcloud_cannot_save_credentials");
	}
}

static void forget_account_cb(lv_event_t *e) {
	(void)e;
	if ((!busy || waiting_for_track) && !switcher_back_drag_active() && !player_sheet_drag_active()) {
		confirm_show("soundcloud_forget_account", "soundcloud_forget_note", NULL, forget_confirm, NULL);
	}
}

// ---------------------------------------------------------------------------
// LVGL periodic poll (processes worker completions & triggers lookahead prefetch)
// ---------------------------------------------------------------------------

static void poll_cb(lv_timer_t *timer) {
	(void)timer;

	pthread_mutex_lock(&job_lock);
	result_t *res = completed_mail;
	completed_mail = NULL;
	unsigned latest = job_serial;
	unsigned failed = failed_serial;
	failed_serial = 0;
	pthread_mutex_unlock(&job_lock);

	if (failed && failed == latest) {
		busy = false;
		waiting_for_track = false;
		loading_track_id = 0;
		toast_busy_end();
		refresh_track_loading_ui();
		toast_error("soundcloud_out_of_memory");
	}

	if (res) {
		if (res->job.serial == latest) {
			busy = false;
			toast_busy_end();
			if (res->count < 0) {
				waiting_for_track = false;
				loading_track_id = 0;
				refresh_track_loading_ui();
				if (res->job.kind == JOB_CONNECT) {
					soundcloud_credentials(&draft, &draft_remember);
					refresh_settings_ui();
				}
				toast_error(res->error[0] ? res->error : "soundcloud_connection_failed");
			} else if (res->job.kind == JOB_BROWSE) {
				free(shown_entries);
				shown_entries = res->entries;
				res->entries = NULL;
				shown_count = res->count;
				shown_more = res->more;
				snprintf(shown_next_cursor, sizeof(shown_next_cursor), "%s", res->next_cursor);
				current_view = res->job;
				rebuild_list_ui();
			} else if (res->job.kind == JOB_CONNECT) {
				soundcloud_credentials(&draft, &draft_remember);
				refresh_settings_ui();
				toast_success("soundcloud_connected");
				if (lv_screen_active() == settings_screen) {
					switch_screen_return_to(soundcloud_screen);
				}
			} else if (res->job.kind == JOB_PLAY) {
				waiting_for_track = false;
				loading_track_id = 0;
				refresh_track_loading_ui();
				soundcloudcache_protect(res->resolved_path);
				int cur_idx = playlist_current_index();
				if (cur_idx >= 0) {
					(void)device_state_play_queue_index(cur_idx);
				} else {
					device_state_play_file(res->resolved_path);
				}
				player_refresh_now_playing();
				if (!res->job.start_queue_after_play) {
					player_sheet_open(true);
				}
			}
		}
		free_result(res);
	}

	if (waiting_for_track) {
		refresh_track_loading_ui();
	}

	// Trigger background lookahead prefetch once the active SoundCloud track passes 80%.
	if (audio_get_status() == AUDIO_STATUS_PLAYING && !waiting_for_track) {
		char current[512], next[512];
		audio_get_current_file(current, sizeof(current));
		if (soundcloudcache_is_soundcloud_path(current)) {
			double pos = 0, dur = 0;
			audio_get_progress(&pos, &dur);
			if (dur > 0 && pos >= dur * 0.80 && playlist_peek_auto(next, sizeof(next)) &&
				soundcloudcache_is_soundcloud_path(next)) {
				request_prefetch(soundcloudcache_track_id_of(next));
			}
		}
	}
}

// ---------------------------------------------------------------------------
// Screen builders
// ---------------------------------------------------------------------------

static void fit_stacked_title(sc_value_row_t *vr) {
	if (!vr || !vr->title || !vr->text_col) {
		return;
	}
	int32_t width = lv_obj_get_content_width(vr->text_col);
	const char *text = lv_label_get_text(vr->title);
	if (!text || !text[0] || width <= 0) {
		return;
	}

	bool compact = bp_is_tempotec_v1();
	if (vr->value) {
		const lv_font_t *vf = lv_obj_get_style_text_font(vr->value, LV_PART_MAIN);
		if (vf) {
			int32_t lh = lv_font_get_line_height(vf);
			if (lv_obj_get_height(vr->value) != lh) {
				lv_obj_set_height(vr->value, lh);
			}
		}
	}
	static lv_font_t *const COMPACT_STEPS[] = {&font_ui_24, &font_ui_22, &font_ui_20, &font_ui_18, &font_ui_16};
	static lv_font_t *const REGULAR_STEPS[] = {&font_ui_24, &font_ui_22, &font_ui_20};
	lv_font_t *const *steps = compact ? COMPACT_STEPS : REGULAR_STEPS;
	size_t count = compact ? sizeof(COMPACT_STEPS) / sizeof(COMPACT_STEPS[0])
						   : sizeof(REGULAR_STEPS) / sizeof(REGULAR_STEPS[0]);

	for (size_t i = 0; i < count; i++) {
		lv_point_t sz;
		lv_text_get_size(&sz, text, steps[i], 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
		if (sz.x <= width || i + 1 == count) {
			if (lv_obj_get_style_text_font(vr->title, LV_PART_MAIN) != steps[i]) {
				lv_obj_set_style_text_font(vr->title, steps[i], 0);
			}
			return;
		}
	}
}

static void fit_all_value_row_titles(void) {
	for (int i = 0; i < value_row_count; i++) {
		fit_stacked_title(&value_rows[i]);
	}
}

static void value_row_col_resized_cb(lv_event_t *e) {
	fit_stacked_title((sc_value_row_t *)lv_event_get_user_data(e));
}

// These cards are twice a menu row's height, and they are the whole page, so
// the picture is drawn large and centred with the caption underneath in every
// theme -- including the flat ones, whose menu rows would otherwise put the
// glyph beside the text and leave the card mostly empty.
static void home_fit_tile_icons(void) {
	if (!home_grid) {
		return;
	}
	bool fn = theme_uses_function_glyphs();
	// Both pictures are rendered at exactly the size they are drawn at, so the
	// card shows the artwork's own pixels and never a resampled copy of them.
	int size = fn ? SOUNDCLOUD_TILE_GLYPH_PX : SOUNDCLOUD_TILE_ART_PX;
	for (int i = 0; i < SC_HOME_TILES; i++) {
		gridpage_set_tile_icon_size(home_grid, i, size);
		// Picture above, caption underneath, in every theme: the flat themes
		// put the glyph beside the text on their menu rows, which leaves most
		// of these taller cards empty. Re-applying the layout also measures the
		// caption against the picture's new width.
		gridpage_set_tile_orientation(home_grid, i, false);
	}
}

static void refresh_soundcloud_theme(void) {
	home_fit_tile_icons();
	refresh_quality_pills();
	refresh_cache_limit_pills();
	bool compact = bp_is_tempotec_v1();
	bool fn_glyphs = theme_uses_function_glyphs();
	bool flat = compact && fn_glyphs;

	for (int i = 0; i < value_row_count; i++) {
		sc_value_row_t *vr = &value_rows[i];
		if (!vr->row) {
			continue;
		}
		if (flat) {
			lv_obj_set_style_border_width(vr->row, 0, 0);
			lv_obj_set_style_outline_width(vr->row, 0, 0);
		} else {
			lv_obj_remove_local_style_prop(vr->row, LV_STYLE_BORDER_WIDTH, 0);
			lv_obj_remove_local_style_prop(vr->row, LV_STYLE_OUTLINE_WIDTH, 0);
		}
		if (vr->icon) {
			lv_obj_set_hidden(vr->icon, !fn_glyphs);
			if (fn_glyphs) {
				lv_obj_set_style_image_recolor(vr->icon, theme_semantic_color(vr->tone), 0);
				lv_obj_set_style_image_recolor_opa(vr->icon, LV_OPA_COVER, 0);
			}
		}
		fit_stacked_title(vr);
	}
	if (shown_count > 0 && list_container) {
		rebuild_list_ui();
	} else {
		refresh_track_loading_ui();
	}
}

static lv_obj_t *add_stacked_value_row(lv_obj_t *parent, const char *name, lv_obj_t **value_out,
									   const lv_image_dsc_t *icon_src, theme_semantic_t tone, bool with_chevron,
									   lv_event_cb_t cb, void *user_data) {
	bool compact = bp_is_tempotec_v1();
	bool two_line = (value_out != NULL);
	lv_obj_t *row = lv_btn_create(parent);
	lv_obj_set_width(row, lv_pct(100));
	lv_obj_set_height(row, LV_SIZE_CONTENT);
	lv_obj_set_style_min_height(row, two_line ? (compact ? 58 : 100) : (compact ? 44 : 84), 0);
	lv_obj_add_style(row, &theme_style_card, 0);
	lv_obj_add_style(row, &theme_style_card_pressed, LV_STATE_PRESSED);
	lv_obj_set_style_radius(row, compact ? bp_tile_radius() : 12, 0);
	lv_obj_set_style_shadow_width(row, 0, 0);
	lv_obj_set_style_pad_hor(row, compact ? 8 : 20, 0);
	lv_obj_set_style_pad_ver(row, compact ? 7 : 14, 0);
	lv_obj_set_style_pad_column(row, compact ? 10 : 12, 0);
	lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_event_bubble(row, true);
	if (cb) {
		lv_obj_add_event_cb(row, cb, LV_EVENT_CLICKED, user_data);
	}

	lv_obj_t *icon = NULL;
	if (icon_src) {
		icon = lv_image_create(row);
		lv_image_set_src(icon, icon_src);
		lv_obj_add_style(icon, &theme_style_icon, 0);
		lv_obj_set_clickable(icon, false);
		lv_obj_set_scrollable(icon, false);
		int sw = (int)icon_src->header.w;
		int sh = (int)icon_src->header.h;
		int max_side = LV_MAX(sw, sh);
		int target = compact ? 18 : 24;
		if (max_side > 0) {
			lv_obj_set_size(icon, LV_MAX(1, sw * target / max_side), LV_MAX(1, sh * target / max_side));
			lv_image_set_inner_align(icon, LV_IMAGE_ALIGN_CENTER);
			lv_image_set_scale(icon, (uint32_t)(LV_SCALE_NONE * target / max_side));
		}
	}

	lv_obj_t *col = lv_obj_create(row);
	lv_obj_remove_style_all(col);
	lv_obj_set_flex_grow(col, 1);
	lv_obj_set_height(col, LV_SIZE_CONTENT);
	lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_style_pad_row(col, 2, 0);
	lv_obj_set_clickable(col, false);
	lv_obj_set_scrollable(col, false);
	lv_obj_set_event_bubble(col, true);

	lv_obj_t *title = lv_label_create(col);
	lv_label_set_text(title, tr(name));
	lv_obj_add_style(title, &theme_style_text, 0);
	lv_obj_set_style_text_font(title, &font_ui_24, 0);
	lv_obj_set_style_text_line_space(title, 0, 0);
	lv_obj_set_width(title, lv_pct(100));
	lv_label_set_long_mode(title, LV_LABEL_LONG_WRAP);
	lv_obj_set_clickable(title, false);

	lv_obj_t *val = NULL;
	if (two_line) {
		const lv_font_t *val_font = compact ? &font_ui_14 : &font_ui_20;
		val = lv_label_create(col);
		lv_obj_add_style(val, &theme_style_text_dim, 0);
		lv_obj_set_style_text_font(val, val_font, 0);
		lv_obj_set_width(val, lv_pct(100));
		lv_obj_set_height(val, lv_font_get_line_height(val_font));
		lv_label_set_long_mode(val, LV_LABEL_LONG_DOT);
		lv_obj_set_clickable(val, false);
		*value_out = val;
	}

	if (with_chevron) {
		lv_obj_t *chev = lv_image_create(row);
		lv_image_set_src(chev, &icon_chevron_right);
		lv_obj_add_style(chev, &theme_style_icon, 0);
		lv_obj_set_style_image_opa(chev, LV_OPA_60, 0);
		lv_obj_set_clickable(chev, false);
		lv_obj_set_scrollable(chev, false);
		if (compact) {
			int cw = (int)icon_chevron_right.header.w;
			int ch = (int)icon_chevron_right.header.h;
			int cmax = LV_MAX(cw, ch);
			int ctarget = 20;
			if (cmax > 0) {
				lv_obj_set_size(chev, LV_MAX(1, cw * ctarget / cmax), LV_MAX(1, ch * ctarget / cmax));
				lv_image_set_inner_align(chev, LV_IMAGE_ALIGN_CENTER);
				lv_image_set_scale(chev, (uint32_t)(LV_SCALE_NONE * ctarget / cmax));
			}
		}
	}

	if (value_row_count < SC_MAX_VALUE_ROWS) {
		sc_value_row_t *slot = &value_rows[value_row_count++];
		slot->row = row;
		slot->icon = icon;
		slot->text_col = col;
		slot->title = title;
		slot->value = val;
		slot->tone = tone;
		lv_obj_add_event_cb(col, value_row_col_resized_cb, LV_EVENT_SIZE_CHANGED, slot);
		lv_obj_add_event_cb(title, value_row_col_resized_cb, LV_EVENT_REFR_EXT_DRAW_SIZE, slot);
	}

	return row;
}

static void build_track_load_indicators(gui_config_t *cfg) {
	list_corner_spin_wrap = lv_obj_create(list_screen);
	lv_obj_remove_style_all(list_corner_spin_wrap);
	settingsrow_place_corner_button(list_corner_spin_wrap, cfg, 0);
	lv_obj_set_clickable(list_corner_spin_wrap, false);
	lv_obj_set_scrollable(list_corner_spin_wrap, false);
	lv_obj_set_hidden(list_corner_spin_wrap, true);
	list_corner_spin = spinner_create(list_corner_spin_wrap, &icon_loader_small);
	lv_obj_set_style_image_recolor(list_corner_spin, theme()->accent, 0);
	lv_obj_set_style_image_recolor_opa(list_corner_spin, LV_OPA_COVER, 0);
	lv_obj_center(list_corner_spin);

	int badge_sz = cfg->screen_width < 320 ? 26 : 36;
	track_load_badge = lv_obj_create(lv_layer_top());
	lv_obj_set_size(track_load_badge, badge_sz, badge_sz);
	lv_obj_align(track_load_badge, LV_ALIGN_TOP_MID, 0, cfg->top_bar_height + 4);
	lv_obj_add_style(track_load_badge, &theme_style_card, 0);
	lv_obj_set_style_bg_opa(track_load_badge, LV_OPA_80, 0);
	lv_obj_set_style_radius(track_load_badge, LV_RADIUS_CIRCLE, 0);
	lv_obj_set_style_border_width(track_load_badge, 0, 0);
	lv_obj_set_style_shadow_width(track_load_badge, 0, 0);
	lv_obj_set_style_pad_all(track_load_badge, 0, 0);
	lv_obj_set_clickable(track_load_badge, false);
	lv_obj_set_scrollable(track_load_badge, false);
	lv_obj_set_hidden(track_load_badge, true);

	track_load_spin = spinner_create(track_load_badge, &icon_loader_small);
	if (cfg->screen_width < 320) {
		lv_image_set_scale(track_load_spin, 192);
	}
	lv_obj_set_style_image_recolor(track_load_spin, theme()->accent, 0);
	lv_obj_set_style_image_recolor_opa(track_load_spin, LV_OPA_COVER, 0);
	lv_obj_center(track_load_spin);
}

static lv_obj_t *make_screen(void) {
	lv_obj_t *obj = lv_obj_create(NULL);
	lv_obj_add_style(obj, &theme_style_screen, 0);
	lv_obj_add_event_cb(obj, page_left_cb, LV_EVENT_SCREEN_UNLOADED, NULL);
	switcher_attach_back_gesture(obj);
	return obj;
}

static lv_obj_t *add_corner_btn(lv_obj_t *parent, gui_config_t *cfg, int slot, const lv_image_dsc_t *glyph,
								lv_event_cb_t cb) {
	lv_obj_t *btn = lv_btn_create(parent);
	settingsrow_place_corner_button(btn, cfg, slot);
	lv_obj_set_style_bg_opa(btn, LV_OPA_TRANSP, 0);
	lv_obj_set_style_border_width(btn, 0, 0);
	lv_obj_set_style_shadow_width(btn, 0, 0);
	lv_obj_set_style_pad_all(btn, 0, 0);

	lv_obj_t *ic = lv_image_create(btn);
	lv_image_set_src(ic, glyph);
	lv_obj_add_style(ic, &theme_style_icon, 0);
	settingsrow_scale_corner_icon(ic, cfg);
	lv_obj_center(ic);
	lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);
	return btn;
}

static void add_note_label(lv_obj_t *container, const char *key) {
	lv_obj_t *note = lv_label_create(container);
	lv_obj_add_style(note, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(note, &font_ui_18, 0);
	lv_obj_set_width(note, lv_pct(100));
	lv_label_set_long_mode(note, LV_LABEL_LONG_WRAP);
	lv_label_set_text(note, tr(key));
}

static void build_search_screen(gui_config_t *cfg) {
	search_screen = make_screen();
	lv_obj_t *stitle = settingsrow_title(search_screen, cfg, "search");
	settingsrow_title_corner_slots(stitle, cfg, 0);
	search_field = lv_textarea_create(search_screen);
	lv_textarea_set_one_line(search_field, true);
	lv_textarea_set_max_length(search_field, SOUNDCLOUD_TITLE_MAX - 1);
	lv_textarea_set_placeholder_text(search_field, tr("soundcloud_search_tracks"));
	lv_obj_add_style(search_field, &theme_style_card, 0);
	lv_obj_set_style_text_font(search_field, &font_ui_24, 0);
	lv_obj_set_style_pad_all(search_field, 8, 0);
	lv_obj_set_size(search_field, cfg->screen_width - 2 * cfg->padding, cfg->screen_width < 320 ? 44 : 64);
	lv_obj_align(search_field, LV_ALIGN_TOP_LEFT, cfg->padding, settingsrow_content_top(cfg));
	keyboard_style_caret(search_field);
	search_keyboard = keyboard_create(search_screen, cfg->screen_width, cfg->screen_width < 320 ? 144 : 316,
									  search_field, NULL, "ok", accept_search_cb, NULL);
}

static void wrap_pills_card_heading(lv_obj_t *card) {
	if (!card || lv_obj_get_child_count(card) == 0) {
		return;
	}
	lv_obj_t *heading = lv_obj_get_child(card, 0);
	if (heading && lv_obj_check_type(heading, &lv_label_class)) {
		lv_obj_set_width(heading, lv_pct(100));
		lv_label_set_long_mode(heading, LV_LABEL_LONG_WRAP);
	}
}

static void build_settings_screens(gui_config_t *cfg) {
	settings_screen = make_screen();
	lv_obj_t *c = settingsrow_page(settings_screen, cfg, "soundcloud_account_settings");
	settingsrow_title_corner_slots(settingsrow_page_title(settings_screen), cfg, 0);

	add_stacked_value_row(c, "soundcloud_username", &user_value, &icon_artist, THEME_SEMANTIC_CYAN, true,
						  edit_field_cb, (void *)(intptr_t)0);

	add_stacked_value_row(c, "soundcloud_oauth_token", &token_value, &icon_keylock_quick, THEME_SEMANTIC_BLUE, true,
						  edit_field_cb, (void *)(intptr_t)1);

	settingsrow_toggle(c, "soundcloud_remember", &remember_switch, remember_toggle_cb);

	add_stacked_value_row(c, "soundcloud_connect_save", NULL, &icon_check, THEME_SEMANTIC_GREEN, false,
						  connect_save_cb, NULL);

	add_stacked_value_row(c, "soundcloud_client_id", &cid_value, &icon_wifi_high, THEME_SEMANTIC_PURPLE, true,
						  edit_field_cb, (void *)(intptr_t)2);

	lv_obj_t *pills = NULL;
	wrap_pills_card_heading(settingsrow_pills(c, "soundcloud_quality", &pills));
	quality_pills[0] = settingsrow_pill_text(pills, "Opus 64k", SOUNDCLOUD_QUALITY_LOW, quality_pill_cb);
	quality_pills[1] = settingsrow_pill_text(pills, "MP3 128k", SOUNDCLOUD_QUALITY_STANDARD, quality_pill_cb);
	quality_pills[2] = settingsrow_pill_text(pills, "AAC 256k", SOUNDCLOUD_QUALITY_HIGH, quality_pill_cb);

	lv_obj_t *cache_pills = NULL;
	wrap_pills_card_heading(settingsrow_pills(c, "soundcloud_cache_limit", &cache_pills));
	cache_limit_pills[0] = settingsrow_pill_text(cache_pills, "512 MB", 512, cache_limit_pill_cb);
	cache_limit_pills[1] = settingsrow_pill_text(cache_pills, "1 GB", 1024, cache_limit_pill_cb);
	cache_limit_pills[2] = settingsrow_pill_text(cache_pills, "2 GB", 2048, cache_limit_pill_cb);
	cache_limit_pills[3] = settingsrow_pill_text(cache_pills, "4 GB", 4096, cache_limit_pill_cb);
	cache_limit_pills[4] = settingsrow_pill_text(cache_pills, "8 GB", 8192, cache_limit_pill_cb);
	cache_limit_pills[5] = settingsrow_pill(cache_pills, "soundcloud_cache_unlimited", 0, cache_limit_pill_cb);

	add_stacked_value_row(c, "soundcloud_clear_cache", &cache_value, &icon_trash, THEME_SEMANTIC_AMBER, false,
						  clear_cache_cb, NULL);

	add_stacked_value_row(c, "soundcloud_forget_account", NULL, &icon_log_out, THEME_SEMANTIC_RED, false,
						  forget_account_cb, NULL);

	add_note_label(c, "soundcloud_settings_note");
	player_sheet_attach_drag(c, true);

	edit_screen = make_screen();
	edit_title = settingsrow_title(edit_screen, cfg, "soundcloud_username");
	settingsrow_title_corner_slots(edit_title, cfg, 0);
	edit_field = lv_textarea_create(edit_screen);
	lv_textarea_set_one_line(edit_field, true);
	lv_obj_add_style(edit_field, &theme_style_card, 0);
	lv_obj_set_style_text_font(edit_field, &font_ui_24, 0);
	lv_obj_set_style_pad_all(edit_field, 8, 0);
	lv_obj_set_size(edit_field, cfg->screen_width - 2 * cfg->padding, cfg->screen_width < 320 ? 44 : 64);
	lv_obj_align(edit_field, LV_ALIGN_TOP_LEFT, cfg->padding, settingsrow_content_top(cfg));
	keyboard_style_caret(edit_field);
	edit_keyboard = keyboard_create(edit_screen, cfg->screen_width, cfg->screen_width < 320 ? 144 : 316, edit_field,
									NULL, "ok", accept_edit_cb, NULL);
	lv_obj_add_event_cb(edit_screen, edit_unloaded_cb, LV_EVENT_SCREEN_UNLOADED, NULL);
}

static void home_loaded_cb(lv_event_t *e) {
	(void)e;
	refresh_home_summary();
	fit_all_value_row_titles();
}

static void settings_loaded_cb(lv_event_t *e) {
	(void)e;
	refresh_cache_label();
	fit_all_value_row_titles();
}

void soundcloudpage_init(gui_config_t *cfg) {
	page_cfg = cfg;
	value_row_count = 0;
	soundcloud_init();
	soundcloudcache_set_root(storage_sd_root());
	device_state_add_prepare_cb(soundcloudpage_prepare_track);

	soundcloud_screen = make_screen();
	settingsrow_title(soundcloud_screen, cfg, "soundcloud");
	settingsrow_title_corner_slots(settingsrow_page_title(soundcloud_screen), cfg, 2);
	add_corner_btn(soundcloud_screen, cfg, 0, &icon_music_settings, open_settings_cb);
	add_corner_btn(soundcloud_screen, cfg, 1, &icon_search, open_search_cb);

	// Four destinations, so two square tiles fill the panel under the heading
	// exactly: the grid is sized from the space that is left, and there is
	// never a fifth row to scroll to. Search stays the corner button.
	const grid_entry_t home_entries[] = {
		{"soundcloud_likes", &icon_menu_sc_likes, NULL, home_likes_action},
		{"soundcloud_reposts", &icon_menu_sc_reposts, NULL, home_reposts_action},
		{"soundcloud_playlists", &icon_menu_sc_playlists, NULL, home_playlists_action},
		{"soundcloud_account_settings", &icon_menu_sc_account, NULL, home_settings_action},
	};
	home_grid = gridpage_build(soundcloud_screen, cfg, home_entries, SC_HOME_TILES, 2, 2, true);
	// These tiles are taller than a menu row's, and the captions are longer
	// than one line in most languages ("Account settings", "Paramètres du
	// compte"), so let them use two lines instead of an ellipsis.
	for (int i = 0; i < SC_HOME_TILES; i++) {
		gridpage_set_tile_label_lines(home_grid, i, 2);
	}

	lv_obj_add_event_cb(soundcloud_screen, home_loaded_cb, LV_EVENT_SCREEN_LOADED, NULL);
	player_sheet_attach_drag(soundcloud_screen, true);

	list_screen = make_screen();
	list_container = settingsrow_page(list_screen, cfg, "soundcloud");
	list_title = settingsrow_page_title(list_screen);
	switcher_set_back_guard(list_screen, list_back_guard);
	switcher_set_back_guard_peek(list_screen, list_back_has_step);
	player_sheet_attach_drag(list_screen, true);
	player_sheet_attach_drag(list_container, true);

	build_track_load_indicators(cfg);
	build_search_screen(cfg);
	build_settings_screens(cfg);
	lv_obj_add_event_cb(settings_screen, settings_loaded_cb, LV_EVENT_SCREEN_LOADED, NULL);

	soundcloud_credentials(&draft, &draft_remember);
	refresh_settings_ui();
	refresh_soundcloud_theme();
	theme_register_refresh(refresh_soundcloud_theme);
	fonts_register_change(fit_all_value_row_titles);

	lv_timer_create(poll_cb, 250, NULL);
}
