#include "jellyfinpage.h"

#include <ctype.h>
#include <dirent.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "jellyfin_icon.h"
#include "src/gui/board_profile.h"
#include "src/gui/fonts/fonts.h"
#include "src/gui/nowplaying/player.h"
#include "src/gui/shell/confirm.h"
#include "src/gui/shell/gridpage.h"
#include "src/gui/shell/icons.h"
#include "src/gui/shell/keyboard.h"
#include "src/gui/shell/popover.h"
#include "src/gui/shell/settingsrow.h"
#include "src/gui/shell/spinner.h"
#include "src/gui/shell/switcher.h"
#include "src/gui/shell/theme.h"
#include "src/gui/shell/toast.h"
#include "src/system/audio/audio.h"
#include "src/system/core/lang.h"
#include "src/system/core/utils.h"
#include "src/system/device/system.h"
#include "src/system/net/tls.h"
#include "src/system/playback/device_state.h"
#include "src/system/playback/playlist.h"
#include "src/system/streaming/jellyfin.h"
#include "src/system/streaming/jellyfincache.h"
#include "src/system/streaming/streamkeep.h"
#include "src/system/streaming/streamturn.h"

#define JF_UI_PAGE_SIZE 40
#define JF_HISTORY_DEPTH 16
#define JF_QUEUE_CAP 256
#define JF_SAVED_CAP 256
#define JF_CACHE_PRESET_COUNT 6
#define JF_QUALITY_COUNT 4
#define JF_HOME_TILES 8
#define JF_TILE_ART_PX 48
#define JF_TILE_GLYPH_PX 40
#define JF_WORKER_STACK (256 * 1024)
#define JF_QC_POLL_MS 2000

static const int JF_CACHE_PRESET_MB[JF_CACHE_PRESET_COUNT] = {512, 1024, 2048, 4096, 8192, 0};

lv_obj_t *jellyfin_screen;

static gui_config_t *page_cfg;
static lv_obj_t *home_grid;
static char home_server_caption[280];

static lv_obj_t *search_screen;
static lv_obj_t *search_field;
static keyboard_t *search_keyboard;

static lv_obj_t *list_screen;
static lv_obj_t *list_container;
static lv_obj_t *list_title;
static lv_obj_t *list_corner_btn;
static lv_obj_t *list_corner_icon;

static lv_obj_t *settings_screen;
static lv_obj_t *server_value;
static lv_obj_t *user_value;
static lv_obj_t *pass_value;
static lv_obj_t *library_value;
static lv_obj_t *cache_value;
static lv_obj_t *remember_switch;
static lv_obj_t *self_signed_switch;
static lv_obj_t *report_switch;
static lv_obj_t *quality_pills[JF_QUALITY_COUNT];
static lv_obj_t *cache_limit_pills[JF_CACHE_PRESET_COUNT];

static lv_obj_t *qc_screen;
static lv_obj_t *qc_code_label;
static lv_obj_t *qc_qr;
static lv_obj_t *qc_hint_label;
static lv_obj_t *qc_status_label;
static lv_obj_t *qc_retry_btn;

static lv_obj_t *edit_screen;
static lv_obj_t *edit_title;
static lv_obj_t *edit_field;
static keyboard_t *edit_keyboard;

typedef struct {
	lv_obj_t *row;
	lv_obj_t *text_col;
	lv_obj_t *title;
	lv_obj_t *value;
	lv_obj_t *chevron;
} jf_value_row_t;

static jf_value_row_t value_rows[6];
static int value_row_count;

// ---------------------------------------------------------------------------
// Local saved offline entries (<card>/Music/Jellyfin/...)
// ---------------------------------------------------------------------------

typedef struct {
	char path[512];
	char title[JELLYFIN_TITLE_MAX];
	char subtitle[JELLYFIN_NAME_MAX + JELLYFIN_TITLE_MAX];
} jf_saved_item_t;

// ---------------------------------------------------------------------------
// View / History / Worker Job types
// ---------------------------------------------------------------------------

typedef enum {
	JF_JOB_BROWSE = 0,
	JF_JOB_AUTH,
	JF_JOB_PING,
	JF_JOB_PLAY,
	JF_JOB_QC_INIT,
	JF_JOB_QC_POLL,
	JF_JOB_FORGET,
} jf_job_kind_t;

typedef struct {
	jf_job_kind_t kind;
	jellyfin_browse_t browse;
	bool is_saved_view;
	bool is_library_picker;
	char id_or_query[256];
	char title[JELLYFIN_TITLE_MAX];
	int offset;
	jellyfin_credentials_t creds;
	bool remember;
	jellyfin_entry_t play_track;
	jellyfin_qc_t qc;
} jf_job_t;

typedef struct {
	jf_job_t job;
	jellyfin_entry_t *entries;
	int count;
	bool more;
	char resolved_path[512];
	char error[160];
	jellyfin_qc_t qc_out;
	jellyfin_qc_state_t qc_state;
} jf_result_t;

static jf_job_t current_view;
static jf_job_t history_stack[JF_HISTORY_DEPTH];
static int history_depth;

static jellyfin_entry_t *shown_entries;
static int shown_count;
static bool shown_more;
static jf_saved_item_t *saved_items;
static int saved_count;

static bool waiting_for_track;
static char loading_item_id[JELLYFIN_ID_MAX];

// Track metadata table for queue transitions (`prepare_track`) and prefetch.
static pthread_mutex_t queue_meta_lock = PTHREAD_MUTEX_INITIALIZER;
static jellyfin_entry_t queued_tracks[JF_QUEUE_CAP];
static int queued_track_count;

// Main UI-triggered worker (single active job at a time).
static pthread_mutex_t job_lock = PTHREAD_MUTEX_INITIALIZER;
static bool worker_busy;
static jf_result_t *pending_result;
static uint32_t job_generation;

// Background prefetch worker (separate from foreground jobs).
static pthread_mutex_t prefetch_lock = PTHREAD_MUTEX_INITIALIZER;
static bool prefetch_running;
static char last_prefetched_id[JELLYFIN_ID_MAX];

// Offline bulk/single Keep-on-card download worker.
static pthread_mutex_t dl_lock = PTHREAD_MUTEX_INITIALIZER;
static bool dl_running;
static bool dl_cancel;
static int dl_done_count;
static int dl_total_count;
static int dl_failed_count;

// Quick Connect live state.
static bool qc_active;
static bool qc_poll_in_flight;
static jellyfin_qc_t qc_session;
static lv_timer_t *qc_poll_timer;

// Settings draft values (password lives ONLY in memory, cleared after login).
static jellyfin_credentials_t draft;
static bool draft_remember = true;

typedef enum {
	JF_EDIT_SERVER = 0,
	JF_EDIT_USERNAME,
	JF_EDIT_PASSWORD,
} jf_edit_field_t;
static jf_edit_field_t active_edit_field;

// Playback reporting state (watched on 1 Hz timer).
static char rep_item_id[JELLYFIN_ID_MAX];
static char rep_session_id[JELLYFIN_ID_MAX];
static bool rep_transcoded;
static bool rep_was_paused;
static double rep_last_pos;
static uint32_t rep_last_ping_ms;

// Popover context for long-pressed item.
static int popover_item_index = -1;
static bool popover_item_is_saved;

// ---------------------------------------------------------------------------
// Helpers & forward declarations
// ---------------------------------------------------------------------------

static void refresh_settings_ui(void);
static void refresh_home_server_tile(void);
static void rebuild_list_ui(void);
static void start_browse(const jf_job_t *view, bool push_history);
static void open_saved_view(bool push_history);
static void stop_quick_connect(void);

static bool drag_active(void) {
	return player_sheet_drag_active() || switcher_back_drag_active();
}

static const char *tr_or_raw(const char *key) {
	if (!key || !key[0])
		return "";
	const char *t = tr(key);
	return (t && t[0]) ? t : key;
}

static void jf_toast_error(const char *key) {
	toast_error((key && key[0]) ? key : "jellyfin_connection_failed");
}

static void free_result(jf_result_t *res) {
	if (!res)
		return;
	free(res->entries);
	free(res);
}

static void record_queued_tracks(const jellyfin_entry_t *tracks, int count) {
	pthread_mutex_lock(&queue_meta_lock);
	if (count > JF_QUEUE_CAP)
		count = JF_QUEUE_CAP;
	if (count > 0 && tracks)
		memcpy(queued_tracks, tracks, (size_t)count * sizeof(queued_tracks[0]));
	queued_track_count = count > 0 ? count : 0;
	pthread_mutex_unlock(&queue_meta_lock);
}

static bool lookup_queued_track(const char *item_id, jellyfin_entry_t *out) {
	if (!item_id || !item_id[0] || !out)
		return false;
	pthread_mutex_lock(&queue_meta_lock);
	for (int i = 0; i < queued_track_count; i++) {
		if (strcmp(queued_tracks[i].id, item_id) == 0) {
			*out = queued_tracks[i];
			pthread_mutex_unlock(&queue_meta_lock);
			return true;
		}
	}
	pthread_mutex_unlock(&queue_meta_lock);
	return false;
}

static void format_duration(int secs, char *out, size_t size) {
	if (secs <= 0) {
		out[0] = '\0';
		return;
	}
	int h = secs / 3600;
	int m = (secs % 3600) / 60;
	int s = secs % 60;
	if (h > 0)
		snprintf(out, size, "%d:%02d:%02d", h, m, s);
	else
		snprintf(out, size, "%d:%02d", m, s);
}

static void format_qc_code(const char *raw, char *out, size_t size) {
	if (!raw || !raw[0]) {
		out[0] = '\0';
		return;
	}
	size_t len = strlen(raw);
	if (len == 6) {
		snprintf(out, size, "%.3s %.3s", raw, raw + 3);
	} else {
		snprintf(out, size, "%s", raw);
	}
}

// ---------------------------------------------------------------------------
// Inline row loading spinner
// ---------------------------------------------------------------------------

static void refresh_track_loading_ui(void) {
	if (!list_container)
		return;
	uint32_t count = lv_obj_get_child_count(list_container);
	for (uint32_t i = 0; i < count; i++) {
		lv_obj_t *row = lv_obj_get_child(list_container, i);
		intptr_t idx = (intptr_t)lv_obj_get_user_data(row);
		if (idx < 0 || idx >= shown_count || current_view.is_saved_view)
			continue;
		if (shown_entries[idx].kind != JELLYFIN_ITEM_TRACK)
			continue;

		bool is_loading = waiting_for_track && loading_item_id[0] &&
						  strcmp(shown_entries[idx].id, loading_item_id) == 0;
		lv_obj_t *existing_spin = NULL;
		uint32_t cc = lv_obj_get_child_count(row);
		for (uint32_t c = 0; c < cc; c++) {
			lv_obj_t *child = lv_obj_get_child(row, c);
			if (lv_obj_get_user_data(child) == (void *)(uintptr_t)0x7F10u) {
				existing_spin = child;
				break;
			}
		}
		if (is_loading && !existing_spin) {
			lv_obj_t *sp = spinner_create(row, &icon_loader_small);
			lv_obj_set_user_data(sp, (void *)(uintptr_t)0x7F10u);
		} else if (!is_loading && existing_spin) {
			lv_obj_delete(existing_spin);
		}
	}
}

// ---------------------------------------------------------------------------
// Background worker (browse, auth, ping, play, Quick Connect, forget)
// ---------------------------------------------------------------------------

typedef struct {
	jf_job_t job;
	uint32_t gen;
} jf_worker_arg_t;

static void *jf_worker_thread(void *raw_arg) {
	jf_worker_arg_t *warg = (jf_worker_arg_t *)raw_arg;
	thread_be_background("jf-worker");

	jf_result_t *res = calloc(1, sizeof(*res));
	if (!res) {
		free(warg);
		pthread_mutex_lock(&job_lock);
		worker_busy = false;
		pthread_mutex_unlock(&job_lock);
		return NULL;
	}
	res->job = warg->job;

	switch (warg->job.kind) {
	case JF_JOB_BROWSE: {
		jellyfin_entry_t *entries = calloc(JF_UI_PAGE_SIZE, sizeof(*entries));
		if (!entries) {
			res->count = -1;
			snprintf(res->error, sizeof(res->error), "%s", "jellyfin_out_of_memory");
			break;
		}
		bool more = false;
		int n = jellyfin_browse(warg->job.browse, warg->job.id_or_query, warg->job.offset, entries, JF_UI_PAGE_SIZE,
								&more, res->error, sizeof(res->error));
		if (n < 0) {
			free(entries);
			res->count = -1;
		} else {
			res->entries = entries;
			res->count = n;
			res->more = more;
		}
		break;
	}

	case JF_JOB_AUTH: {
		if (!jellyfin_authenticate(&warg->job.creds, warg->job.remember, res->error, sizeof(res->error)))
			res->count = -1;
		else
			res->count = 0;
		memset(&warg->job.creds.password, 0, sizeof(warg->job.creds.password));
		break;
	}

	case JF_JOB_PING: {
		if (!jellyfin_ping(res->error, sizeof(res->error)))
			res->count = -1;
		else
			res->count = 0;
		break;
	}

	case JF_JOB_PLAY: {
		if (!jellyfincache_get(&warg->job.play_track, false, res->resolved_path, sizeof(res->resolved_path), res->error,
							   sizeof(res->error)))
			res->count = -1;
		else
			res->count = 0;
		break;
	}

	case JF_JOB_QC_INIT: {
		res->qc_state = jellyfin_qc_initiate(warg->job.creds.server, &res->qc_out, res->error, sizeof(res->error));
		res->count = (res->qc_state == JELLYFIN_QC_PENDING) ? 0 : -1;
		break;
	}

	case JF_JOB_QC_POLL: {
		res->qc_state = jellyfin_qc_poll(&warg->job.qc, warg->job.remember, res->error, sizeof(res->error));
		res->count = (res->qc_state == JELLYFIN_QC_OK || res->qc_state == JELLYFIN_QC_PENDING) ? 0 : -1;
		break;
	}

	case JF_JOB_FORGET: {
		jellyfincache_clear();
		jellyfin_logout_and_forget();
		res->count = 0;
		break;
	}
	}

	uint32_t gen = warg->gen;
	free(warg);

	pthread_mutex_lock(&job_lock);
	if (gen == job_generation) {
		free_result(pending_result);
		pending_result = res;
	} else {
		free_result(res);
	}
	worker_busy = false;
	pthread_mutex_unlock(&job_lock);
	return NULL;
}

static bool submit_job(const jf_job_t *job) {
	pthread_mutex_lock(&job_lock);
	if (worker_busy) {
		pthread_mutex_unlock(&job_lock);
		return false;
	}
	jf_worker_arg_t *arg = calloc(1, sizeof(*arg));
	if (!arg) {
		pthread_mutex_unlock(&job_lock);
		return false;
	}
	arg->job = *job;
	arg->gen = ++job_generation;
	worker_busy = true;
	pthread_mutex_unlock(&job_lock);

	pthread_attr_t attr;
	pthread_t tid;
	pthread_attr_init(&attr);
	pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
	pthread_attr_setstacksize(&attr, JF_WORKER_STACK);
	if (pthread_create(&tid, &attr, jf_worker_thread, arg) != 0) {
		pthread_attr_destroy(&attr);
		free(arg);
		pthread_mutex_lock(&job_lock);
		worker_busy = false;
		pthread_mutex_unlock(&job_lock);
		return false;
	}
	pthread_attr_destroy(&attr);
	return true;
}

// ---------------------------------------------------------------------------
// Async Favourite toggle (for player heart & quick panel)
// ---------------------------------------------------------------------------

typedef struct {
	char item_id[JELLYFIN_ID_MAX];
	bool next_fav;
	bool ok;
	char error[128];
	void (*done)(bool ok, const char *error, void *user);
	void *user;
} jf_fav_job_t;

static void fav_done_ui(void *arg) {
	jf_fav_job_t *job = (jf_fav_job_t *)arg;
	if (!job)
		return;
	if (job->ok && !current_view.is_saved_view && shown_entries) {
		for (int i = 0; i < shown_count; i++) {
			if (strcmp(shown_entries[i].id, job->item_id) == 0)
				shown_entries[i].is_favorite = job->next_fav;
		}
	}
	if (job->done)
		job->done(job->ok, job->ok ? NULL : job->error, job->user);
	free(job);
}

static void *fav_worker_thread(void *arg) {
	jf_fav_job_t *job = (jf_fav_job_t *)arg;
	thread_be_background("jf-fav");
	job->ok = jellyfin_set_favorite(job->item_id, job->next_fav, job->error, sizeof(job->error));
	if (!gui_post(fav_done_ui, job))
		free(job);
	return NULL;
}

void jellyfinpage_toggle_favorite_async(const char *item_id,
										void (*done)(bool ok, const char *error, void *user), void *user) {
	if (!item_id || !item_id[0]) {
		if (done)
			done(false, "jellyfin_invalid_id", user);
		return;
	}
	if (!jellyfin_logged_in()) {
		if (done)
			done(false, "jellyfin_not_configured", user);
		return;
	}

	jf_fav_job_t *job = calloc(1, sizeof(*job));
	if (!job) {
		if (done)
			done(false, "jellyfin_out_of_memory", user);
		return;
	}
	snprintf(job->item_id, sizeof(job->item_id), "%s", item_id);
	job->next_fav = !jellyfin_is_favorite(item_id);
	job->done = done;
	job->user = user;

	pthread_attr_t attr;
	pthread_t tid;
	pthread_attr_init(&attr);
	pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
	pthread_attr_setstacksize(&attr, JF_WORKER_STACK);
	if (pthread_create(&tid, &attr, fav_worker_thread, job) != 0) {
		pthread_attr_destroy(&attr);
		free(job);
		if (done)
			done(false, "jellyfin_connection_failed", user);
		return;
	}
	pthread_attr_destroy(&attr);
}

// ---------------------------------------------------------------------------
// Offline "Save to card" download worker (single track or whole view)
// ---------------------------------------------------------------------------

typedef struct {
	jellyfin_entry_t *tracks;
	int count;
} jf_dl_job_t;

typedef struct {
	int saved;
	int total;
	int failed;
	bool cancelled;
	char error[160];
} jf_dl_notice_t;

static void dl_done_ui(void *arg) {
	jf_dl_notice_t *n = (jf_dl_notice_t *)arg;
	if (!n)
		return;
	if (n->cancelled) {
		toast_plain("jellyfin_save_cancelled");
	} else if (n->saved > 0 && n->failed == 0) {
		if (n->total == 1) {
			toast_success("jellyfin_saved_one");
		} else {
			char msg[160];
			snprintf(msg, sizeof(msg), tr_or_raw("jellyfin_saved_many"), n->saved);
			toast_success(msg);
		}
	} else if (n->saved > 0) {
		char msg[160];
		snprintf(msg, sizeof(msg), tr_or_raw("jellyfin_saved_partial"), n->saved, n->total);
		toast_plain(msg);
	} else {
		jf_toast_error(n->error[0] ? n->error : "jellyfin_save_failed");
	}
	if (current_view.is_saved_view && lv_screen_active() == list_screen)
		open_saved_view(false);
	free(n);
}

static void *dl_worker_thread(void *arg) {
	jf_dl_job_t *job = (jf_dl_job_t *)arg;
	thread_be_background("jf-save");

	int saved = 0;
	int failed = 0;
	bool cancelled = false;
	char last_err[160] = "";

	for (int i = 0; i < job->count; i++) {
		pthread_mutex_lock(&dl_lock);
		cancelled = dl_cancel;
		pthread_mutex_unlock(&dl_lock);
		if (cancelled)
			break;

		char cached[512];
		char err[160] = "";
		if (!jellyfincache_get(&job->tracks[i], true, cached, sizeof(cached), err, sizeof(err))) {
			failed++;
			if (err[0])
				snprintf(last_err, sizeof(last_err), "%s", err);
			continue;
		}

		// Wait if another keep operation is copying on the card.
		while (streamkeep_busy())
			usleep(50 * 1000);

		if (!streamkeep_keep(cached, false)) {
			failed++;
			char kerr[160] = "";
			if (streamkeep_last_error(kerr, sizeof(kerr)) && kerr[0])
				snprintf(last_err, sizeof(last_err), "%s", kerr);
			continue;
		}
		while (streamkeep_busy())
			usleep(50 * 1000);

		char kerr[160] = "";
		if (streamkeep_last_error(kerr, sizeof(kerr)) && kerr[0]) {
			failed++;
			snprintf(last_err, sizeof(last_err), "%s", kerr);
		} else {
			saved++;
		}

		pthread_mutex_lock(&dl_lock);
		dl_done_count = saved;
		dl_failed_count = failed;
		pthread_mutex_unlock(&dl_lock);
	}

	int total = job->count;
	free(job->tracks);
	free(job);

	pthread_mutex_lock(&dl_lock);
	dl_running = false;
	dl_cancel = false;
	pthread_mutex_unlock(&dl_lock);

	jf_dl_notice_t *notice = calloc(1, sizeof(*notice));
	if (notice) {
		notice->saved = saved;
		notice->total = total;
		notice->failed = failed;
		notice->cancelled = cancelled;
		snprintf(notice->error, sizeof(notice->error), "%s", last_err);
		if (!gui_post(dl_done_ui, notice))
			free(notice);
	}
	return NULL;
}

static void start_offline_save(const jellyfin_entry_t *tracks, int count) {
	if (!tracks || count <= 0)
		return;
	const char *sd = storage_sd_root();
	if (!sd || !sd[0]) {
		toast_error("jellyfin_no_card");
		return;
	}

	pthread_mutex_lock(&dl_lock);
	if (dl_running) {
		dl_cancel = true;
		pthread_mutex_unlock(&dl_lock);
		toast_plain("jellyfin_save_cancelling");
		return;
	}
	jf_dl_job_t *job = calloc(1, sizeof(*job));
	if (!job) {
		pthread_mutex_unlock(&dl_lock);
		return;
	}
	job->tracks = calloc((size_t)count, sizeof(*job->tracks));
	if (!job->tracks) {
		free(job);
		pthread_mutex_unlock(&dl_lock);
		return;
	}
	memcpy(job->tracks, tracks, (size_t)count * sizeof(*job->tracks));
	job->count = count;
	dl_running = true;
	dl_cancel = false;
	dl_done_count = 0;
	dl_total_count = count;
	dl_failed_count = 0;
	pthread_mutex_unlock(&dl_lock);

	pthread_attr_t attr;
	pthread_t tid;
	pthread_attr_init(&attr);
	pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
	pthread_attr_setstacksize(&attr, JF_WORKER_STACK);
	if (pthread_create(&tid, &attr, dl_worker_thread, job) != 0) {
		pthread_attr_destroy(&attr);
		free(job->tracks);
		free(job);
		pthread_mutex_lock(&dl_lock);
		dl_running = false;
		pthread_mutex_unlock(&dl_lock);
		toast_error("jellyfin_save_failed");
		return;
	}
	pthread_attr_destroy(&attr);

	if (count == 1) {
		toast_plain("jellyfin_saving_one");
	} else {
		char msg[160];
		snprintf(msg, sizeof(msg), tr_or_raw("jellyfin_saving_many"), count);
		toast_plain(msg);
	}
}

bool jellyfinpage_download_busy(void) {
	pthread_mutex_lock(&dl_lock);
	bool busy = dl_running;
	pthread_mutex_unlock(&dl_lock);
	return busy;
}

// ---------------------------------------------------------------------------
// Background lookahead prefetch (80% progress)
// ---------------------------------------------------------------------------

static void *prefetch_worker_thread(void *arg) {
	jellyfin_entry_t *track = (jellyfin_entry_t *)arg;
	thread_be_background("jf-prefetch");
	jellyfincache_prefetch(track);
	free(track);
	pthread_mutex_lock(&prefetch_lock);
	prefetch_running = false;
	pthread_mutex_unlock(&prefetch_lock);
	return NULL;
}

static void request_prefetch(const char *item_id) {
	if (!item_id || !item_id[0])
		return;
	jellyfin_entry_t track;
	if (!lookup_queued_track(item_id, &track))
		return;

	pthread_mutex_lock(&prefetch_lock);
	if (prefetch_running || strcmp(last_prefetched_id, item_id) == 0) {
		pthread_mutex_unlock(&prefetch_lock);
		return;
	}
	jellyfin_entry_t *copy = calloc(1, sizeof(*copy));
	if (!copy) {
		pthread_mutex_unlock(&prefetch_lock);
		return;
	}
	*copy = track;
	snprintf(last_prefetched_id, sizeof(last_prefetched_id), "%s", item_id);
	prefetch_running = true;
	pthread_mutex_unlock(&prefetch_lock);

	pthread_attr_t attr;
	pthread_t tid;
	pthread_attr_init(&attr);
	pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
	pthread_attr_setstacksize(&attr, JF_WORKER_STACK);
	if (pthread_create(&tid, &attr, prefetch_worker_thread, copy) != 0) {
		pthread_attr_destroy(&attr);
		free(copy);
		pthread_mutex_lock(&prefetch_lock);
		prefetch_running = false;
		pthread_mutex_unlock(&prefetch_lock);
		return;
	}
	pthread_attr_destroy(&attr);
}

// ---------------------------------------------------------------------------
// Queue preparation hook (`device_state_add_prepare_cb`)
// ---------------------------------------------------------------------------

bool jellyfinpage_prepare_track(char *path, size_t path_size) {
	if (!jellyfincache_owns(path))
		return true;

	char item_id[JELLYFIN_ID_MAX];
	if (!jellyfincache_item_id_of(path, item_id, sizeof(item_id)))
		return false;

	char existing[512];
	if (jellyfincache_find_playable(item_id, existing, sizeof(existing))) {
		jellyfincache_protect(existing);
		snprintf(path, path_size, "%s", existing);
		return true;
	}

	jellyfin_entry_t track;
	if (!lookup_queued_track(item_id, &track)) {
		memset(&track, 0, sizeof(track));
		snprintf(track.id, sizeof(track.id), "%s", item_id);
		snprintf(track.title, sizeof(track.title), "%s", item_id);
	}

	char resolved[512];
	char err[160] = "";
	if (!jellyfincache_get(&track, false, resolved, sizeof(resolved), err, sizeof(err)))
		return false;

	jellyfincache_protect(resolved);
	snprintf(path, path_size, "%s", resolved);
	return true;
}

bool jellyfinpage_waiting_for_track(void) {
	return waiting_for_track;
}

// ---------------------------------------------------------------------------
// Saved offline scanner (<card>/Music/Jellyfin/...)
// ---------------------------------------------------------------------------

static bool is_saved_audio_name(const char *name) {
	const char *dot = strrchr(name, '.');
	if (!dot || !dot[1])
		return false;
	const char *ext = dot + 1;
	return strcasecmp(ext, "flac") == 0 || strcasecmp(ext, "mp3") == 0 || strcasecmp(ext, "opus") == 0 ||
		   strcasecmp(ext, "ogg") == 0 || strcasecmp(ext, "m4a") == 0 || strcasecmp(ext, "aac") == 0 ||
		   strcasecmp(ext, "wav") == 0 || strcasecmp(ext, "wv") == 0 || strcasecmp(ext, "ape") == 0 ||
		   strcasecmp(ext, "aiff") == 0 || strcasecmp(ext, "dsf") == 0 || strcasecmp(ext, "dff") == 0;
}

static void read_saved_tags(const char *audio_path, char *title_out, size_t title_size, char *sub_out,
							size_t sub_size) {
	title_out[0] = '\0';
	sub_out[0] = '\0';
	char tags_path[528];
	if ((size_t)snprintf(tags_path, sizeof(tags_path), "%s.tags", audio_path) < sizeof(tags_path)) {
		FILE *f = fopen(tags_path, "rb");
		if (f) {
			char artist[JELLYFIN_NAME_MAX] = "";
			char album[JELLYFIN_TITLE_MAX] = "";
			char line[384];
			while (fgets(line, sizeof(line), f)) {
				size_t len = strlen(line);
				while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r'))
					line[--len] = '\0';
				if (strncmp(line, "title=", 6) == 0)
					snprintf(title_out, title_size, "%.*s", (int)(title_size - 1), line + 6);
				else if (strncmp(line, "artist=", 7) == 0)
					snprintf(artist, sizeof(artist), "%.*s", (int)(sizeof(artist) - 1), line + 7);
				else if (strncmp(line, "album=", 6) == 0)
					snprintf(album, sizeof(album), "%.*s", (int)(sizeof(album) - 1), line + 6);
			}
			fclose(f);
			if (artist[0] && album[0])
				snprintf(sub_out, sub_size, "%s · %s", artist, album);
			else if (artist[0])
				snprintf(sub_out, sub_size, "%s", artist);
			else if (album[0])
				snprintf(sub_out, sub_size, "%s", album);
		}
	}
	if (!title_out[0]) {
		const char *base = strrchr(audio_path, '/');
		const char *name = base ? base + 1 : audio_path;
		snprintf(title_out, title_size, "%.*s", (int)(title_size - 1), name);
	}
}

static void scan_saved_recursive(const char *dir, int depth, jf_saved_item_t *items, int *count, int cap) {
	if (depth > 4 || *count >= cap)
		return;
	DIR *d = opendir(dir);
	if (!d)
		return;
	struct dirent *de;
	while ((de = readdir(d)) != NULL && *count < cap) {
		if (de->d_name[0] == '.')
			continue;
		char full[512];
		if ((size_t)snprintf(full, sizeof(full), "%s/%s", dir, de->d_name) >= sizeof(full))
			continue;
		struct stat st;
		if (stat(full, &st) != 0)
			continue;
		if (S_ISDIR(st.st_mode)) {
			scan_saved_recursive(full, depth + 1, items, count, cap);
		} else if (S_ISREG(st.st_mode) && is_saved_audio_name(de->d_name)) {
			jf_saved_item_t *slot = &items[*count];
			memset(slot, 0, sizeof(*slot));
			snprintf(slot->path, sizeof(slot->path), "%s", full);
			read_saved_tags(full, slot->title, sizeof(slot->title), slot->subtitle, sizeof(slot->subtitle));
			(*count)++;
		}
	}
	closedir(d);
}

static int cmp_saved_item(const void *a, const void *b) {
	const jf_saved_item_t *sa = (const jf_saved_item_t *)a;
	const jf_saved_item_t *sb = (const jf_saved_item_t *)b;
	return strcasecmp(sa->path, sb->path);
}

static void open_saved_view(bool push_history) {
	if (push_history && lv_screen_active() == list_screen && history_depth < JF_HISTORY_DEPTH)
		history_stack[history_depth++] = current_view;

	memset(&current_view, 0, sizeof(current_view));
	current_view.is_saved_view = true;
	snprintf(current_view.title, sizeof(current_view.title), "%s", tr_or_raw("jellyfin_saved"));

	free(saved_items);
	saved_items = NULL;
	saved_count = 0;

	const char *sd = storage_sd_root();
	if (sd && sd[0]) {
		char root[512];
		if ((size_t)snprintf(root, sizeof(root), "%s/Music/Jellyfin", sd) < sizeof(root)) {
			jf_saved_item_t *buf = calloc(JF_SAVED_CAP, sizeof(*buf));
			if (buf) {
				int n = 0;
				scan_saved_recursive(root, 0, buf, &n, JF_SAVED_CAP);
				if (n > 1)
					qsort(buf, (size_t)n, sizeof(*buf), cmp_saved_item);
				saved_items = buf;
				saved_count = n;
			}
		}
	}

	rebuild_list_ui();
	if (lv_screen_active() != list_screen)
		switch_screen(list_screen);
}

static void delete_saved_confirmed(void *user) {
	int idx = (int)(intptr_t)user;
	if (idx < 0 || idx >= saved_count || !saved_items)
		return;
	const char *path = saved_items[idx].path;

	// If deleting the track currently playing, stop playback first.
	char active[512];
	audio_get_current_file(active, sizeof(active));
	if (strcmp(active, path) == 0)
		device_state_stop();

	unlink(path);
	char side[528];
	if ((size_t)snprintf(side, sizeof(side), "%s.tags", path) < sizeof(side))
		unlink(side);

	// Clean up empty album/artist directories up to Music/Jellyfin.
	char dir[512];
	snprintf(dir, sizeof(dir), "%s", path);
	for (int up = 0; up < 2; up++) {
		char *slash = strrchr(dir, '/');
		if (!slash || slash <= dir)
			break;
		*slash = '\0';
		if (strstr(dir, "/Music/Jellyfin/") == NULL)
			break;
		if (rmdir(dir) != 0)
			break;
	}

	toast_success("jellyfin_saved_deleted");
	open_saved_view(false);
}

// ---------------------------------------------------------------------------
// List screen actions (tap, long-press popover, corner button)
// ---------------------------------------------------------------------------

static void play_track_at_index(int index) {
	if (index < 0 || index >= shown_count || !shown_entries)
		return;
	if (shown_entries[index].kind != JELLYFIN_ITEM_TRACK)
		return;
	const char *sd = storage_sd_root();
	if (!sd || !sd[0]) {
		toast_error("jellyfin_no_card");
		return;
	}

	jellyfin_entry_t *qtracks = calloc(JF_UI_PAGE_SIZE, sizeof(*qtracks));
	char (*qpaths)[512] = calloc(JF_UI_PAGE_SIZE, sizeof(*qpaths));
	const char **qptrs = calloc(JF_UI_PAGE_SIZE, sizeof(*qptrs));
	if (!qtracks || !qpaths || !qptrs) {
		free(qtracks);
		free(qpaths);
		free(qptrs);
		toast_error("jellyfin_out_of_memory");
		return;
	}

	int qcount = 0;
	int selected_slot = 0;
	for (int i = 0; i < shown_count && qcount < JF_UI_PAGE_SIZE; i++) {
		if (shown_entries[i].kind != JELLYFIN_ITEM_TRACK)
			continue;
		if (!jellyfincache_placeholder_path(shown_entries[i].id, shown_entries[i].container, qpaths[qcount],
											sizeof(qpaths[0])))
			continue;
		if (i == index)
			selected_slot = qcount;
		qtracks[qcount] = shown_entries[i];
		qptrs[qcount] = qpaths[qcount];
		qcount++;
	}

	if (qcount == 0) {
		free(qtracks);
		free(qpaths);
		free(qptrs);
		toast_error("jellyfin_stream_failed");
		return;
	}

	record_queued_tracks(qtracks, qcount);
	streamturn_abandon_all();

	const jellyfin_entry_t *chosen = &shown_entries[index];
	char cached[512];
	if (jellyfincache_find_playable(chosen->id, cached, sizeof(cached))) {
		waiting_for_track = false;
		loading_item_id[0] = '\0';
		refresh_track_loading_ui();
		jellyfincache_protect(cached);
		device_state_play_list(qptrs, qcount, selected_slot);
		player_refresh_now_playing();
		player_sheet_open(true);
		free(qtracks);
		free(qpaths);
		free(qptrs);
		return;
	}

	device_state_stop();
	playlist_load_paths(qptrs, qcount, selected_slot);
	free(qtracks);
	free(qpaths);
	free(qptrs);

	waiting_for_track = true;
	snprintf(loading_item_id, sizeof(loading_item_id), "%s", chosen->id);
	jf_job_t job;
	memset(&job, 0, sizeof(job));
	job.kind = JF_JOB_PLAY;
	job.play_track = *chosen;
	if (submit_job(&job)) {
		refresh_track_loading_ui();
	} else {
		waiting_for_track = false;
		loading_item_id[0] = '\0';
		refresh_track_loading_ui();
		toast_error("jellyfin_busy");
	}
}

static void play_saved_at_index(int index) {
	if (index < 0 || index >= saved_count || !saved_items)
		return;
	const char **ptrs = calloc((size_t)saved_count, sizeof(*ptrs));
	if (!ptrs)
		return;
	for (int i = 0; i < saved_count; i++)
		ptrs[i] = saved_items[i].path;
	device_state_play_list(ptrs, saved_count, index);
	player_refresh_now_playing();
	player_sheet_open(true);
	free(ptrs);
}

static void popover_play_cb(void *user) {
	(void)user;
	if (popover_item_is_saved)
		play_saved_at_index(popover_item_index);
	else
		play_track_at_index(popover_item_index);
}

static void popover_save_cb(void *user) {
	(void)user;
	if (popover_item_is_saved || popover_item_index < 0 || popover_item_index >= shown_count || !shown_entries)
		return;
	if (shown_entries[popover_item_index].kind == JELLYFIN_ITEM_TRACK)
		start_offline_save(&shown_entries[popover_item_index], 1);
}

static void popover_fav_done(bool ok, const char *err, void *user) {
	bool now_fav = (bool)(intptr_t)user;
	if (ok) {
		toast_success(now_fav ? "jellyfin_fav_added" : "jellyfin_fav_removed");
		player_refresh_now_playing();
	} else {
		toast_error(err ? err : "jellyfin_connection_failed");
	}
}

static void popover_fav_cb(void *user) {
	(void)user;
	if (popover_item_is_saved || popover_item_index < 0 || popover_item_index >= shown_count || !shown_entries)
		return;
	const jellyfin_entry_t *e = &shown_entries[popover_item_index];
	if (e->kind != JELLYFIN_ITEM_TRACK)
		return;
	bool next = !jellyfin_is_favorite(e->id);
	jellyfinpage_toggle_favorite_async(e->id, popover_fav_done, (void *)(intptr_t)next);
}

static void popover_delete_saved_cb(void *user) {
	(void)user;
	if (!popover_item_is_saved || popover_item_index < 0 || popover_item_index >= saved_count || !saved_items)
		return;
	confirm_show(tr_or_raw("jellyfin_delete_saved"), saved_items[popover_item_index].title, tr_or_raw("delete"),
				 delete_saved_confirmed, (void *)(intptr_t)popover_item_index);
}

static void list_entry_long_press_cb(lv_event_t *e) {
	if (drag_active())
		return;
	lv_obj_t *row = lv_event_get_target(e);
	int idx = (int)(intptr_t)lv_event_get_user_data(e);

	if (current_view.is_saved_view) {
		if (idx < 0 || idx >= saved_count || !saved_items)
			return;
		popover_item_index = idx;
		popover_item_is_saved = true;
		popover_item_t items[2] = {
			{.label = "jellyfin_play", .action = popover_play_cb},
			{.label = "delete", .action = popover_delete_saved_cb},
		};
		popover_show(row, items, 2);
		return;
	}

	if (current_view.is_library_picker || idx < 0 || idx >= shown_count || !shown_entries)
		return;
	if (shown_entries[idx].kind != JELLYFIN_ITEM_TRACK)
		return;

	popover_item_index = idx;
	popover_item_is_saved = false;
	bool fav = jellyfin_is_favorite(shown_entries[idx].id);
	popover_item_t items[3] = {
		{.label = "jellyfin_play", .action = popover_play_cb},
		{.label = "jellyfin_save_to_card", .action = popover_save_cb},
		{.label = fav ? "jellyfin_remove_favorite" : "jellyfin_add_favorite", .action = popover_fav_cb},
	};
	popover_show(row, items, 3);
}

static void list_entry_click_cb(lv_event_t *e) {
	if (drag_active())
		return;
	int idx = (int)(intptr_t)lv_event_get_user_data(e);

	if (current_view.is_saved_view) {
		play_saved_at_index(idx);
		return;
	}

	// Library picker: index -1 means "All libraries".
	if (current_view.is_library_picker) {
		if (idx < 0) {
			jellyfin_set_library("", "");
		} else if (idx < shown_count && shown_entries) {
			jellyfin_set_library(shown_entries[idx].id, shown_entries[idx].title);
		}
		refresh_settings_ui();
		toast_success("jellyfin_library_saved");
		switch_screen_return_to(settings_screen);
		return;
	}

	if (idx < 0 || idx >= shown_count || !shown_entries)
		return;

	const jellyfin_entry_t *item = &shown_entries[idx];
	if (item->kind == JELLYFIN_ITEM_TRACK) {
		play_track_at_index(idx);
		return;
	}

	jf_job_t next;
	memset(&next, 0, sizeof(next));
	next.kind = JF_JOB_BROWSE;
	snprintf(next.id_or_query, sizeof(next.id_or_query), "%s", item->id);
	snprintf(next.title, sizeof(next.title), "%s", item->title);
	next.offset = 0;

	switch (item->kind) {
	case JELLYFIN_ITEM_FOLDER:
		next.browse = JELLYFIN_BROWSE_FOLDER;
		break;
	case JELLYFIN_ITEM_ARTIST:
		next.browse = JELLYFIN_BROWSE_ARTIST_ALBUMS;
		break;
	case JELLYFIN_ITEM_ALBUM:
		next.browse = JELLYFIN_BROWSE_ALBUM_TRACKS;
		break;
	case JELLYFIN_ITEM_PLAYLIST:
		next.browse = JELLYFIN_BROWSE_PLAYLIST_ITEMS;
		break;
	case JELLYFIN_ITEM_GENRE:
		next.browse = JELLYFIN_BROWSE_GENRE_ALBUMS;
		break;
	default:
		return;
	}
	start_browse(&next, true);
}

static void page_prev_cb(lv_event_t *e) {
	(void)e;
	if (drag_active() || current_view.offset <= 0)
		return;
	jf_job_t prev = current_view;
	prev.offset = (prev.offset >= JF_UI_PAGE_SIZE) ? (prev.offset - JF_UI_PAGE_SIZE) : 0;
	start_browse(&prev, false);
}

static void page_next_cb(lv_event_t *e) {
	(void)e;
	if (drag_active() || !shown_more)
		return;
	jf_job_t next = current_view;
	next.offset += JF_UI_PAGE_SIZE;
	start_browse(&next, false);
}

static void list_corner_action_cb(lv_event_t *e) {
	(void)e;
	if (drag_active())
		return;

	if (!current_view.is_saved_view && !current_view.is_library_picker &&
		(current_view.browse == JELLYFIN_BROWSE_ALBUMS || current_view.browse == JELLYFIN_BROWSE_RECENT)) {
		jf_job_t toggled = current_view;
		toggled.offset = 0;
		if (current_view.browse == JELLYFIN_BROWSE_ALBUMS) {
			toggled.browse = JELLYFIN_BROWSE_RECENT;
			snprintf(toggled.title, sizeof(toggled.title), "%s", tr_or_raw("jellyfin_recent"));
		} else {
			toggled.browse = JELLYFIN_BROWSE_ALBUMS;
			snprintf(toggled.title, sizeof(toggled.title), "%s", tr_or_raw("jellyfin_albums"));
		}
		start_browse(&toggled, false);
		return;
	}

	// Collect playable tracks on the current view and save them offline.
	if (!current_view.is_saved_view && !current_view.is_library_picker && shown_entries && shown_count > 0) {
		jellyfin_entry_t *tracks = calloc((size_t)shown_count, sizeof(*tracks));
		if (!tracks)
			return;
		int tcount = 0;
		for (int i = 0; i < shown_count; i++) {
			if (shown_entries[i].kind == JELLYFIN_ITEM_TRACK)
				tracks[tcount++] = shown_entries[i];
		}
		if (tcount > 0)
			start_offline_save(tracks, tcount);
		free(tracks);
	}
}

// ---------------------------------------------------------------------------
// List screen rendering
// ---------------------------------------------------------------------------

static lv_obj_t *make_list_row(const char *title_txt, const char *sub_txt, const lv_image_dsc_t *icon_src,
							   theme_semantic_t tone, bool with_chevron, lv_event_cb_t click_cb,
							   lv_event_cb_t long_cb, void *user_data) {
	bool compact = bp_is_tempotec_v1();
	bool flat = compact && theme_uses_function_glyphs();

	lv_obj_t *row = lv_btn_create(list_container);
	lv_obj_set_width(row, lv_pct(100));
	lv_obj_set_height(row, LV_SIZE_CONTENT);
	lv_obj_set_style_min_height(row, (sub_txt && sub_txt[0]) ? (compact ? 54 : 84) : (compact ? 44 : 72), 0);
	lv_obj_add_style(row, &theme_style_card, 0);
	lv_obj_add_style(row, &theme_style_card_pressed, LV_STATE_PRESSED);
	if (flat) {
		lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
		lv_obj_set_style_border_width(row, 0, 0);
		lv_obj_set_style_radius(row, 8, 0);
	}
	lv_obj_set_style_pad_left(row, compact ? 10 : 18, 0);
	lv_obj_set_style_pad_right(row, compact ? 10 : 18, 0);
	lv_obj_set_style_pad_top(row, compact ? 6 : 12, 0);
	lv_obj_set_style_pad_bottom(row, compact ? 6 : 12, 0);
	lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_column(row, compact ? 8 : 14, 0);
	lv_obj_set_user_data(row, user_data);

	if (icon_src) {
		lv_obj_t *ic = lv_image_create(row);
		lv_image_set_src(ic, icon_src);
		int sz = compact ? 20 : 28;
		int max_side = LV_MAX((int)icon_src->header.w, (int)icon_src->header.h);
		if (max_side > 0) {
			lv_obj_set_size(ic, sz, sz);
			lv_image_set_inner_align(ic, LV_IMAGE_ALIGN_CENTER);
			lv_image_set_scale(ic, (uint32_t)(LV_SCALE_NONE * sz / max_side));
		}
		lv_obj_set_style_image_recolor(ic, theme_semantic_color(tone), 0);
		lv_obj_set_style_image_recolor_opa(ic, LV_OPA_COVER, 0);
	}

	lv_obj_t *col = lv_obj_create(row);
	lv_obj_remove_style_all(col);
	lv_obj_set_flex_grow(col, 1);
	lv_obj_set_width(col, 0);
	lv_obj_set_height(col, LV_SIZE_CONTENT);
	lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_style_pad_row(col, 2, 0);
	lv_obj_set_clickable(col, false);

	lv_obj_t *lbl = lv_label_create(col);
	lv_label_set_text(lbl, title_txt ? title_txt : "");
	lv_obj_set_width(lbl, lv_pct(100));
	lv_label_set_long_mode(lbl, LV_LABEL_LONG_DOT);
	lv_obj_set_style_max_height(lbl, lv_font_get_line_height(compact ? &font_ui_18 : &font_ui_24) * 2, 0);
	lv_obj_set_style_text_font(lbl, compact ? &font_ui_18 : &font_ui_24, 0);
	lv_obj_add_style(lbl, &theme_style_text, 0);

	if (sub_txt && sub_txt[0]) {
		lv_obj_t *sub = lv_label_create(col);
		lv_label_set_text(sub, sub_txt);
		lv_obj_set_width(sub, lv_pct(100));
		lv_label_set_long_mode(sub, LV_LABEL_LONG_DOT);
		lv_obj_set_style_text_font(sub, compact ? &font_ui_14 : &font_ui_18, 0);
		lv_obj_add_style(sub, &theme_style_text_dim, 0);
	}

	if (with_chevron) {
		lv_obj_t *chev = lv_image_create(row);
		lv_image_set_src(chev, &icon_chevron_right);
		lv_obj_set_style_image_recolor(chev, theme()->text_secondary, 0);
		lv_obj_set_style_image_recolor_opa(chev, LV_OPA_COVER, 0);
	}

	if (click_cb)
		lv_obj_add_event_cb(row, click_cb, LV_EVENT_CLICKED, user_data);
	if (long_cb)
		lv_obj_add_event_cb(row, long_cb, LV_EVENT_LONG_PRESSED, user_data);
	return row;
}

static void show_list_loading(const char *title) {
	if (list_title) {
		lv_label_set_text(list_title, (title && title[0]) ? title : tr_or_raw("jellyfin"));
		settingsrow_title_realign(list_title);
	}
	if (list_corner_btn)
		lv_obj_set_hidden(list_corner_btn, true);
	if (!list_container)
		return;
	lv_obj_clean(list_container);
	lv_obj_t *wrap = lv_obj_create(list_container);
	lv_obj_remove_style_all(wrap);
	lv_obj_set_width(wrap, lv_pct(100));
	lv_obj_set_height(wrap, bp_is_tempotec_v1() ? 120 : 180);
	lv_obj_set_flex_flow(wrap, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(wrap, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	spinner_create(wrap, &icon_loader_big);
}

static void update_list_corner_button(void) {
	if (!list_corner_btn || !list_corner_icon)
		return;
	bool show_sort = !current_view.is_saved_view && !current_view.is_library_picker &&
					 (current_view.browse == JELLYFIN_BROWSE_ALBUMS || current_view.browse == JELLYFIN_BROWSE_RECENT);
	bool has_tracks = false;
	if (!current_view.is_saved_view && !current_view.is_library_picker && shown_entries) {
		for (int i = 0; i < shown_count; i++) {
			if (shown_entries[i].kind == JELLYFIN_ITEM_TRACK) {
				has_tracks = true;
				break;
			}
		}
	}

	if (show_sort) {
		lv_image_set_src(list_corner_icon,
						 (current_view.browse == JELLYFIN_BROWSE_ALBUMS) ? &icon_sort_date_new : &icon_sort_az);
		settingsrow_scale_corner_icon(list_corner_icon, page_cfg);
		lv_obj_set_hidden(list_corner_btn, false);
		settingsrow_title_corner_slots(list_title, page_cfg, 1);
	} else if (has_tracks) {
		lv_image_set_src(list_corner_icon, &icon_download);
		settingsrow_scale_corner_icon(list_corner_icon, page_cfg);
		lv_obj_set_hidden(list_corner_btn, false);
		settingsrow_title_corner_slots(list_title, page_cfg, 1);
	} else {
		lv_obj_set_hidden(list_corner_btn, true);
		settingsrow_title_corner_slots(list_title, page_cfg, 0);
	}
	settingsrow_title_realign(list_title);
}

static void rebuild_list_ui(void) {
	if (!list_container)
		return;
	lv_obj_clean(list_container);
	lv_obj_scroll_to_y(list_container, 0, LV_ANIM_OFF);

	if (list_title) {
		lv_label_set_text(list_title, current_view.title[0] ? current_view.title : tr_or_raw("jellyfin"));
		update_list_corner_button();
	}

	// Saved offline view
	if (current_view.is_saved_view) {
		if (saved_count <= 0) {
			lv_obj_t *empty = lv_label_create(list_container);
			lv_label_set_text(empty, tr_or_raw("jellyfin_saved_empty"));
			lv_obj_set_width(empty, lv_pct(100));
			lv_label_set_long_mode(empty, LV_LABEL_LONG_WRAP);
			lv_obj_set_style_text_align(empty, LV_TEXT_ALIGN_CENTER, 0);
			lv_obj_set_style_pad_top(empty, 24, 0);
			lv_obj_set_style_text_font(empty, bp_is_tempotec_v1() ? &font_ui_16 : &font_ui_20, 0);
			lv_obj_add_style(empty, &theme_style_text_dim, 0);
			return;
		}
		for (int i = 0; i < saved_count; i++) {
			make_list_row(saved_items[i].title, saved_items[i].subtitle, &icon_music_note, THEME_SEMANTIC_GREEN, false,
						  list_entry_click_cb, list_entry_long_press_cb, (void *)(intptr_t)i);
		}
		return;
	}

	// Library picker: prepend "All libraries".
	if (current_view.is_library_picker) {
		jellyfin_account_info_t info;
		jellyfin_account_info(&info);
		make_list_row(tr_or_raw("jellyfin_all_libraries"), !info.library_id[0] ? tr_or_raw("jellyfin_selected") : NULL,
					  &icon_folder_root, THEME_SEMANTIC_CYAN, false, list_entry_click_cb, NULL, (void *)(intptr_t)-1);
		for (int i = 0; i < shown_count; i++) {
			bool active = info.library_id[0] && strcmp(info.library_id, shown_entries[i].id) == 0;
			make_list_row(shown_entries[i].title, active ? tr_or_raw("jellyfin_selected") : NULL, &icon_folder,
						  THEME_SEMANTIC_PURPLE, false, list_entry_click_cb, NULL, (void *)(intptr_t)i);
		}
		return;
	}

	if (current_view.offset > 0) {
		make_list_row(tr_or_raw("jellyfin_previous_page"), NULL, &icon_chevron_left, THEME_SEMANTIC_MUTED, false,
					  page_prev_cb, NULL, NULL);
	}

	if (shown_count <= 0) {
		lv_obj_t *empty = lv_label_create(list_container);
		lv_label_set_text(empty, tr_or_raw("jellyfin_empty"));
		lv_obj_set_width(empty, lv_pct(100));
		lv_label_set_long_mode(empty, LV_LABEL_LONG_WRAP);
		lv_obj_set_style_text_align(empty, LV_TEXT_ALIGN_CENTER, 0);
		lv_obj_set_style_pad_top(empty, 24, 0);
		lv_obj_set_style_text_font(empty, bp_is_tempotec_v1() ? &font_ui_16 : &font_ui_20, 0);
		lv_obj_add_style(empty, &theme_style_text_dim, 0);
		return;
	}

	for (int i = 0; i < shown_count; i++) {
		const jellyfin_entry_t *item = &shown_entries[i];
		char sub[256] = "";
		const lv_image_dsc_t *ic = &icon_music_note;
		theme_semantic_t tone = THEME_SEMANTIC_CYAN;
		bool chev = true;

		switch (item->kind) {
		case JELLYFIN_ITEM_FOLDER:
			ic = &icon_folder;
			tone = THEME_SEMANTIC_CYAN;
			if (item->child_count > 0)
				snprintf(sub, sizeof(sub), "%d", item->child_count);
			break;
		case JELLYFIN_ITEM_ARTIST:
			ic = &icon_artist;
			tone = THEME_SEMANTIC_GREEN;
			break;
		case JELLYFIN_ITEM_ALBUM:
			ic = &icon_album;
			tone = THEME_SEMANTIC_BLUE;
			if (item->artist[0] && item->year > 0)
				snprintf(sub, sizeof(sub), "%s · %d", item->artist, item->year);
			else if (item->artist[0])
				snprintf(sub, sizeof(sub), "%s", item->artist);
			else if (item->year > 0)
				snprintf(sub, sizeof(sub), "%d", item->year);
			break;
		case JELLYFIN_ITEM_PLAYLIST:
			ic = &icon_list_music;
			tone = THEME_SEMANTIC_PURPLE;
			if (item->child_count > 0)
				snprintf(sub, sizeof(sub), "%d", item->child_count);
			break;
		case JELLYFIN_ITEM_GENRE:
			ic = &icon_genre;
			tone = THEME_SEMANTIC_AMBER;
			break;
		case JELLYFIN_ITEM_TRACK: {
			ic = &icon_music_note;
			tone = THEME_SEMANTIC_PURPLE;
			chev = false;
			char dur[24] = "";
			format_duration(item->duration, dur, sizeof(dur));
			if (item->artist[0] && dur[0])
				snprintf(sub, sizeof(sub), "%s · %s", item->artist, dur);
			else if (item->artist[0])
				snprintf(sub, sizeof(sub), "%s", item->artist);
			else if (dur[0])
				snprintf(sub, sizeof(sub), "%s", dur);
			break;
		}
		}

		make_list_row(item->title, sub[0] ? sub : NULL, ic, tone, chev, list_entry_click_cb,
					  (item->kind == JELLYFIN_ITEM_TRACK) ? list_entry_long_press_cb : NULL,
					  (void *)(intptr_t)i);
	}

	if (shown_more) {
		make_list_row(tr_or_raw("jellyfin_next_page"), NULL, &icon_chevron_right, THEME_SEMANTIC_MUTED, false,
					  page_next_cb, NULL, NULL);
	}

	refresh_track_loading_ui();
}

static void start_browse(const jf_job_t *view, bool push_history) {
	if (!jellyfin_logged_in()) {
		toast_error("jellyfin_not_configured");
		refresh_settings_ui();
		switch_screen(settings_screen);
		return;
	}

	if (push_history && lv_screen_active() == list_screen && history_depth < JF_HISTORY_DEPTH)
		history_stack[history_depth++] = current_view;

	current_view = *view;
	show_list_loading(view->title);
	if (lv_screen_active() != list_screen)
		switch_screen(list_screen);

	if (!submit_job(view)) {
		toast_error("jellyfin_busy");
		rebuild_list_ui();
	}
}

static bool list_back_guard_peek(void) {
	return history_depth > 0;
}

static bool list_back_guard(void) {
	if (history_depth <= 0)
		return false;
	jf_job_t prev = history_stack[--history_depth];
	if (prev.is_saved_view)
		open_saved_view(false);
	else
		start_browse(&prev, false);
	return true;
}

// ---------------------------------------------------------------------------
// Home screen tile actions
// ---------------------------------------------------------------------------

static void open_category(jellyfin_browse_t browse, const char *title_key) {
	history_depth = 0;
	jf_job_t job;
	memset(&job, 0, sizeof(job));
	job.kind = JF_JOB_BROWSE;
	job.browse = browse;
	snprintf(job.title, sizeof(job.title), "%s", tr_or_raw(title_key));
	start_browse(&job, false);
}

static void home_folders_cb(void) {
	open_category(JELLYFIN_BROWSE_FOLDERS, "jellyfin_folders");
}

static void home_playlists_cb(void) {
	open_category(JELLYFIN_BROWSE_PLAYLISTS, "jellyfin_playlists");
}

static void home_artists_cb(void) {
	open_category(JELLYFIN_BROWSE_ARTISTS, "jellyfin_artists");
}

static void home_albums_cb(void) {
	open_category(JELLYFIN_BROWSE_ALBUMS, "jellyfin_albums");
}

static void home_genres_cb(void) {
	open_category(JELLYFIN_BROWSE_GENRES, "jellyfin_genres");
}

static void home_favorites_cb(void) {
	open_category(JELLYFIN_BROWSE_FAVORITES, "jellyfin_favorites");
}

static void home_saved_cb(void) {
	history_depth = 0;
	open_saved_view(false);
}

static void home_settings_cb(void) {
	jellyfin_account_info_t info;
	jellyfin_account_info(&info);
	if (info.server[0])
		snprintf(draft.server, sizeof(draft.server), "%s", info.server);
	if (info.username[0])
		snprintf(draft.username, sizeof(draft.username), "%s", info.username);
	draft_remember = info.logged_in ? info.remembered : true;
	refresh_settings_ui();
	switch_screen(settings_screen);
}

static void open_search_cb(lv_event_t *e) {
	(void)e;
	if (drag_active())
		return;
	if (!jellyfin_logged_in()) {
		home_settings_cb();
		return;
	}
	if (search_field)
		lv_textarea_set_text(search_field, "");
	if (search_keyboard)
		keyboard_reset(search_keyboard);
	switch_screen(search_screen);
}

static void search_accept_cb(lv_event_t *e) {
	(void)e;
	const char *q = search_field ? lv_textarea_get_text(search_field) : "";
	while (*q && isspace((unsigned char)*q))
		q++;
	if (!q[0]) {
		toast_error("jellyfin_invalid_query");
		return;
	}
	history_depth = 0;
	jf_job_t job;
	memset(&job, 0, sizeof(job));
	job.kind = JF_JOB_BROWSE;
	job.browse = JELLYFIN_BROWSE_SEARCH;
	snprintf(job.id_or_query, sizeof(job.id_or_query), "%s", q);
	snprintf(job.title, sizeof(job.title), "%s", q);
	start_browse(&job, false);
}

// ---------------------------------------------------------------------------
// Quick Connect screen & polling
// ---------------------------------------------------------------------------

static void stop_quick_connect(void) {
	qc_active = false;
	qc_poll_in_flight = false;
	if (qc_poll_timer) {
		lv_timer_delete(qc_poll_timer);
		qc_poll_timer = NULL;
	}
}

static bool qc_back_guard(void) {
	stop_quick_connect();
	return false;
}

static void qc_poll_timer_cb(lv_timer_t *t) {
	(void)t;
	if (!qc_active || qc_poll_in_flight || lv_screen_active() != qc_screen)
		return;
	jf_job_t job;
	memset(&job, 0, sizeof(job));
	job.kind = JF_JOB_QC_POLL;
	job.qc = qc_session;
	job.remember = draft_remember;
	if (submit_job(&job))
		qc_poll_in_flight = true;
}

static void start_quick_connect_flow(void) {
	char norm[JELLYFIN_SERVER_MAX];
	char err[160] = "";
	if (!jellyfin_normalize_server(draft.server, norm, sizeof(norm), err, sizeof(err))) {
		toast_error(err);
		return;
	}
	snprintf(draft.server, sizeof(draft.server), "%s", norm);

	stop_quick_connect();
	if (qc_code_label)
		lv_label_set_text(qc_code_label, "...");
	if (qc_qr)
		lv_obj_set_hidden(qc_qr, true);
	if (qc_hint_label)
		lv_label_set_text(qc_hint_label, tr_or_raw("jellyfin_qc_starting"));
	if (qc_status_label)
		lv_label_set_text(qc_status_label, "");
	if (qc_retry_btn)
		lv_obj_set_hidden(qc_retry_btn, true);

	if (lv_screen_active() != qc_screen)
		switch_screen(qc_screen);

	jf_job_t job;
	memset(&job, 0, sizeof(job));
	job.kind = JF_JOB_QC_INIT;
	snprintf(job.creds.server, sizeof(job.creds.server), "%s", norm);
	if (!submit_job(&job))
		toast_error("jellyfin_busy");
}

static void qc_retry_cb(lv_event_t *e) {
	(void)e;
	if (drag_active())
		return;
	start_quick_connect_flow();
}

// ---------------------------------------------------------------------------
// Settings screen callbacks & refresh
// ---------------------------------------------------------------------------

static void fit_stacked_title(jf_value_row_t *vr) {
	if (!vr || !vr->title || !vr->text_col)
		return;
	int32_t width = lv_obj_get_content_width(vr->text_col);
	const char *text = lv_label_get_text(vr->title);
	if (!text || !text[0] || width <= 0)
		return;

	bool compact = bp_is_tempotec_v1();
	static lv_font_t *const COMPACT_STEPS[] = {&font_ui_20, &font_ui_18, &font_ui_16, &font_ui_14};
	static lv_font_t *const REGULAR_STEPS[] = {&font_ui_24, &font_ui_22, &font_ui_20};
	lv_font_t *const *steps = compact ? COMPACT_STEPS : REGULAR_STEPS;
	int count = compact ? 4 : 3;

	lv_font_t *chosen = steps[count - 1];
	for (int i = 0; i < count; i++) {
		lv_point_t sz;
		lv_text_get_size(&sz, text, steps[i], 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
		if (sz.x <= width) {
			chosen = steps[i];
			break;
		}
	}
	lv_obj_set_style_text_font(vr->title, chosen, 0);
}

static lv_obj_t *add_stacked_value_row(lv_obj_t *parent, const char *title_key, lv_obj_t **value_out,
									   lv_event_cb_t cb) {
	bool compact = bp_is_tempotec_v1();
	lv_obj_t *row = lv_btn_create(parent);
	lv_obj_set_width(row, lv_pct(100));
	lv_obj_set_height(row, LV_SIZE_CONTENT);
	lv_obj_set_style_min_height(row, compact ? 54 : 84, 0);
	lv_obj_add_style(row, &theme_style_card, 0);
	lv_obj_add_style(row, &theme_style_card_pressed, LV_STATE_PRESSED);
	lv_obj_set_style_pad_left(row, compact ? 12 : 18, 0);
	lv_obj_set_style_pad_right(row, compact ? 10 : 16, 0);
	lv_obj_set_style_pad_top(row, compact ? 8 : 12, 0);
	lv_obj_set_style_pad_bottom(row, compact ? 8 : 12, 0);
	lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_column(row, compact ? 8 : 12, 0);

	lv_obj_t *col = lv_obj_create(row);
	lv_obj_remove_style_all(col);
	lv_obj_set_flex_grow(col, 1);
	lv_obj_set_width(col, 0);
	lv_obj_set_height(col, LV_SIZE_CONTENT);
	lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_style_pad_row(col, 2, 0);
	lv_obj_set_clickable(col, false);

	lv_obj_t *title = lv_label_create(col);
	lv_label_set_text(title, tr_or_raw(title_key));
	lv_obj_set_width(title, lv_pct(100));
	lv_label_set_long_mode(title, LV_LABEL_LONG_DOT);
	lv_obj_set_style_text_font(title, compact ? &font_ui_18 : &font_ui_24, 0);
	lv_obj_add_style(title, &theme_style_text, 0);

	lv_obj_t *val = lv_label_create(col);
	lv_label_set_text(val, "");
	lv_obj_set_width(val, lv_pct(100));
	lv_label_set_long_mode(val, LV_LABEL_LONG_DOT);
	lv_obj_set_style_text_font(val, compact ? &font_ui_14 : &font_ui_18, 0);
	lv_obj_add_style(val, &theme_style_text_dim, 0);

	lv_obj_t *chev = lv_image_create(row);
	lv_image_set_src(chev, &icon_chevron_right);
	lv_obj_set_style_image_recolor(chev, theme()->text_secondary, 0);
	lv_obj_set_style_image_recolor_opa(chev, LV_OPA_COVER, 0);

	if (cb)
		lv_obj_add_event_cb(row, cb, LV_EVENT_CLICKED, NULL);
	if (value_out)
		*value_out = val;
	if (value_row_count < (int)(sizeof(value_rows) / sizeof(value_rows[0]))) {
		value_rows[value_row_count++] = (jf_value_row_t){
			.row = row,
			.text_col = col,
			.title = title,
			.value = val,
			.chevron = chev,
		};
	}
	return row;
}

static void open_edit(jf_edit_field_t field, const char *title_key, const char *initial) {
	active_edit_field = field;
	if (edit_title) {
		lv_label_set_text(edit_title, tr_or_raw(title_key));
		settingsrow_title_realign(edit_title);
	}
	if (edit_field) {
		lv_textarea_set_password_mode(edit_field, field == JF_EDIT_PASSWORD);
		keyboard_refresh_password(edit_field);
		lv_textarea_set_text(edit_field, initial ? initial : "");
	}
	if (edit_keyboard)
		keyboard_reset(edit_keyboard);
	switch_screen(edit_screen);
}

static void edit_server_cb(lv_event_t *e) {
	(void)e;
	if (!drag_active())
		open_edit(JF_EDIT_SERVER, "jellyfin_server_url", draft.server);
}

static void edit_user_cb(lv_event_t *e) {
	(void)e;
	if (!drag_active())
		open_edit(JF_EDIT_USERNAME, "jellyfin_username", draft.username);
}

static void edit_pass_cb(lv_event_t *e) {
	(void)e;
	if (!drag_active())
		open_edit(JF_EDIT_PASSWORD, "password", draft.password);
}

static void edit_accept_cb(lv_event_t *e) {
	(void)e;
	const char *txt = edit_field ? lv_textarea_get_text(edit_field) : "";
	switch (active_edit_field) {
	case JF_EDIT_SERVER:
		snprintf(draft.server, sizeof(draft.server), "%s", txt);
		break;
	case JF_EDIT_USERNAME:
		snprintf(draft.username, sizeof(draft.username), "%s", txt);
		break;
	case JF_EDIT_PASSWORD:
		snprintf(draft.password, sizeof(draft.password), "%s", txt);
		break;
	}
	refresh_settings_ui();
	switch_screen_return_to(settings_screen);
}

static void quick_connect_row_cb(lv_event_t *e) {
	(void)e;
	if (drag_active())
		return;
	start_quick_connect_flow();
}

static void remember_switch_cb(lv_event_t *e) {
	lv_obj_t *sw = lv_event_get_target(e);
	draft_remember = lv_obj_has_state(sw, LV_STATE_CHECKED);
	if (jellyfin_logged_in()) {
		char err[128] = "";
		if (!jellyfin_set_remember(draft_remember, err, sizeof(err)))
			toast_error(err);
	}
}

static void self_signed_switch_cb(lv_event_t *e) {
	lv_obj_t *sw = lv_event_get_target(e);
	bool relaxed = lv_obj_has_state(sw, LV_STATE_CHECKED);
	tls_set_verify_mode(relaxed ? TLS_VERIFY_RELAXED : TLS_VERIFY_STRICT);
}

static void report_switch_cb(lv_event_t *e) {
	lv_obj_t *sw = lv_event_get_target(e);
	jellyfin_set_report_playback(lv_obj_has_state(sw, LV_STATE_CHECKED));
}

static void connect_save_cb(lv_event_t *e) {
	(void)e;
	if (drag_active())
		return;

	char norm[JELLYFIN_SERVER_MAX];
	char err[160] = "";
	if (!jellyfin_normalize_server(draft.server, norm, sizeof(norm), err, sizeof(err))) {
		toast_error(err);
		return;
	}
	snprintf(draft.server, sizeof(draft.server), "%s", norm);

	if (!draft.username[0]) {
		toast_error("jellyfin_invalid_credentials");
		return;
	}

	// If already signed in to this exact server + username and password box is
	// blank, simply update remember preference and ping!
	jellyfin_account_info_t info;
	jellyfin_account_info(&info);
	if (info.logged_in && !draft.password[0] && strcmp(info.server, norm) == 0 &&
		strcasecmp(info.username, draft.username) == 0) {
		if (!jellyfin_set_remember(draft_remember, err, sizeof(err))) {
			toast_error(err);
			return;
		}
		jf_job_t ping_job;
		memset(&ping_job, 0, sizeof(ping_job));
		ping_job.kind = JF_JOB_PING;
		if (!submit_job(&ping_job))
			toast_error("jellyfin_busy");
		return;
	}

	jf_job_t job;
	memset(&job, 0, sizeof(job));
	job.kind = JF_JOB_AUTH;
	job.creds = draft;
	job.remember = draft_remember;
	if (!submit_job(&job))
		toast_error("jellyfin_busy");
}

static void pick_library_cb(lv_event_t *e) {
	(void)e;
	if (drag_active())
		return;
	if (!jellyfin_logged_in()) {
		toast_error("jellyfin_not_configured");
		return;
	}
	history_depth = 0;
	jf_job_t job;
	memset(&job, 0, sizeof(job));
	job.kind = JF_JOB_BROWSE;
	job.browse = JELLYFIN_BROWSE_LIBRARIES;
	job.is_library_picker = true;
	snprintf(job.title, sizeof(job.title), "%s", tr_or_raw("jellyfin_library"));
	start_browse(&job, false);
}

static void quality_pill_cb(lv_event_t *e) {
	if (drag_active())
		return;
	int val = (int)(intptr_t)lv_event_get_user_data(e);
	jellyfin_set_quality((jellyfin_quality_t)val);
	refresh_settings_ui();
}

static void cache_limit_pill_cb(lv_event_t *e) {
	if (drag_active())
		return;
	int mb = (int)(intptr_t)lv_event_get_user_data(e);
	jellyfincache_set_max_mb(mb);
	refresh_settings_ui();
}

static void clear_cache_confirmed(void *user) {
	(void)user;
	char active[512];
	audio_get_current_file(active, sizeof(active));
	if (jellyfincache_owns(active))
		device_state_stop();
	jellyfincache_protect(NULL);
	jellyfincache_clear();
	refresh_settings_ui();
	toast_success("jellyfin_cache_cleared");
}

static void clear_cache_cb(lv_event_t *e) {
	(void)e;
	if (drag_active())
		return;
	confirm_show(tr_or_raw("jellyfin_clear_cache"), tr_or_raw("jellyfin_clear_cache_note"),
				 tr_or_raw("jellyfin_clear"), clear_cache_confirmed, NULL);
}

static void test_connection_cb(lv_event_t *e) {
	(void)e;
	if (drag_active())
		return;
	if (!jellyfin_logged_in()) {
		toast_error("jellyfin_not_configured");
		return;
	}
	jf_job_t job;
	memset(&job, 0, sizeof(job));
	job.kind = JF_JOB_PING;
	if (!submit_job(&job))
		toast_error("jellyfin_busy");
}

static void forget_confirmed(void *user) {
	(void)user;
	char active[512];
	audio_get_current_file(active, sizeof(active));
	if (jellyfincache_owns(active))
		device_state_stop();
	jellyfincache_protect(NULL);

	jf_job_t job;
	memset(&job, 0, sizeof(job));
	job.kind = JF_JOB_FORGET;
	if (!submit_job(&job)) {
		// Fallback locally if worker is busy.
		jellyfincache_clear();
		jellyfin_forget();
		memset(&draft, 0, sizeof(draft));
		refresh_settings_ui();
		refresh_home_server_tile();
		toast_success("jellyfin_forgotten");
	}
}

static void forget_server_cb(lv_event_t *e) {
	(void)e;
	if (drag_active())
		return;
	confirm_show(tr_or_raw("jellyfin_forget_server"), tr_or_raw("jellyfin_forget_note"), tr_or_raw("delete"),
				 forget_confirmed, NULL);
}

static void refresh_home_server_tile(void) {
	if (!home_grid)
		return;
	jellyfin_account_info_t info;
	jellyfin_account_info(&info);
	if (info.logged_in && info.username[0]) {
		if (info.server_name[0])
			snprintf(home_server_caption, sizeof(home_server_caption), "%s · %s", info.username, info.server_name);
		else
			snprintf(home_server_caption, sizeof(home_server_caption), "%s", info.username);
		gridpage_set_tile_caption(home_grid, JF_HOME_TILES - 1, home_server_caption);
	} else {
		gridpage_set_tile(home_grid, JF_HOME_TILES - 1, NULL, "jellyfin_server_settings");
	}
}

static void refresh_settings_ui(void) {
	jellyfin_account_info_t info;
	jellyfin_account_info(&info);

	if (server_value)
		lv_label_set_text(server_value, draft.server[0] ? draft.server : tr_or_raw("jellyfin_not_set"));
	if (user_value)
		lv_label_set_text(user_value, draft.username[0] ? draft.username : tr_or_raw("jellyfin_not_set"));
	if (pass_value) {
		if (draft.password[0])
			lv_label_set_text(pass_value, "••••••••");
		else if (info.logged_in)
			lv_label_set_text(pass_value, tr_or_raw("jellyfin_signed_in"));
		else
			lv_label_set_text(pass_value, tr_or_raw("jellyfin_not_set"));
	}
	if (library_value) {
		lv_label_set_text(library_value,
						  info.library_name[0] ? info.library_name : tr_or_raw("jellyfin_all_libraries"));
	}
	if (cache_value) {
		int tracks = 0;
		uint64_t bytes = 0;
		jellyfincache_stats(&tracks, &bytes);
		char buf[64];
		uint64_t mb = bytes / (1024ULL * 1024ULL);
		snprintf(buf, sizeof(buf), "%d · %llu MB", tracks, (unsigned long long)mb);
		lv_label_set_text(cache_value, buf);
	}

	if (remember_switch) {
		if (draft_remember)
			lv_obj_add_state(remember_switch, LV_STATE_CHECKED);
		else
			lv_obj_clear_state(remember_switch, LV_STATE_CHECKED);
	}
	if (self_signed_switch) {
		if (tls_verify_mode() == TLS_VERIFY_RELAXED)
			lv_obj_add_state(self_signed_switch, LV_STATE_CHECKED);
		else
			lv_obj_clear_state(self_signed_switch, LV_STATE_CHECKED);
	}
	if (report_switch) {
		if (jellyfin_report_playback_enabled())
			lv_obj_add_state(report_switch, LV_STATE_CHECKED);
		else
			lv_obj_clear_state(report_switch, LV_STATE_CHECKED);
	}

	jellyfin_quality_t q = jellyfin_get_quality();
	for (int i = 0; i < JF_QUALITY_COUNT; i++) {
		if (quality_pills[i])
			settingsrow_pill_active(quality_pills[i], i == (int)q);
	}
	int max_mb = jellyfincache_get_max_mb();
	for (int i = 0; i < JF_CACHE_PRESET_COUNT; i++) {
		if (cache_limit_pills[i])
			settingsrow_pill_active(cache_limit_pills[i], JF_CACHE_PRESET_MB[i] == max_mb);
	}

	for (int i = 0; i < value_row_count; i++)
		fit_stacked_title(&value_rows[i]);

	refresh_home_server_tile();
}

static void theme_refresh_cb(void) {
	refresh_settings_ui();
	if (list_corner_icon) {
		lv_obj_set_style_image_recolor(list_corner_icon, theme()->text_primary, 0);
		lv_obj_set_style_image_recolor_opa(list_corner_icon, LV_OPA_COVER, 0);
	}
	if (qc_code_label)
		lv_obj_set_style_text_color(qc_code_label, theme()->accent, 0);
	for (int i = 0; i < value_row_count; i++) {
		if (value_rows[i].chevron) {
			lv_obj_set_style_image_recolor(value_rows[i].chevron, theme()->text_secondary, 0);
			lv_obj_set_style_image_recolor_opa(value_rows[i].chevron, LV_OPA_COVER, 0);
		}
	}
	if (home_grid) {
		bool glyph = theme_uses_function_glyphs();
		int sz = glyph ? JF_TILE_GLYPH_PX : JF_TILE_ART_PX;
		for (int i = 0; i < JF_HOME_TILES; i++)
			gridpage_set_tile_icon_size(home_grid, i, sz);
	}
}

// ---------------------------------------------------------------------------
// 1 Hz watcher: job completion, 80% lookahead prefetch, playback reporting
// ---------------------------------------------------------------------------

static void watch_playback_reporting(void) {
	audio_status_t st = audio_get_status();
	char current[512] = "";
	audio_get_current_file(current, sizeof(current));
	bool is_jf = (st == AUDIO_STATUS_PLAYING || st == AUDIO_STATUS_PAUSED) && jellyfincache_owns(current);

	char cur_id[JELLYFIN_ID_MAX] = "";
	if (is_jf && !jellyfincache_item_id_of(current, cur_id, sizeof(cur_id)))
		is_jf = false;

	double pos = 0.0, dur = 0.0;
	if (is_jf)
		audio_get_progress(&pos, &dur);
	long long ticks = (pos > 0.0) ? (long long)(pos * 10000000.0) : 0LL;

	// If a previously reported Jellyfin track ended or switched away, send Stopped.
	if (rep_item_id[0] && (!is_jf || strcmp(rep_item_id, cur_id) != 0)) {
		long long prev_ticks = (rep_last_pos > 0.0) ? (long long)(rep_last_pos * 10000000.0) : 0LL;
		jellyfin_report_stopped_async(rep_item_id, rep_session_id, prev_ticks);
		rep_item_id[0] = '\0';
		rep_session_id[0] = '\0';
		rep_last_pos = 0.0;
		rep_was_paused = false;
	}

	if (!is_jf)
		return;

	bool paused = (st == AUDIO_STATUS_PAUSED);
	uint32_t now = lv_tick_get();

	if (!rep_item_id[0]) {
		snprintf(rep_item_id, sizeof(rep_item_id), "%s", cur_id);
		snprintf(rep_session_id, sizeof(rep_session_id), "sonix-%lu-%u", (unsigned long)time(NULL), (unsigned int)now);
		rep_transcoded = (strstr(current, "__m") != NULL || strstr(current, "__o") != NULL);
		rep_was_paused = paused;
		rep_last_pos = pos;
		rep_last_ping_ms = now;
		jellyfin_report_start_async(rep_item_id, rep_session_id, ticks, rep_transcoded);
		return;
	}

	bool pause_changed = (paused != rep_was_paused);
	double delta = pos - rep_last_pos;
	if (delta < 0.0)
		delta = -delta;
	bool seeked = (delta > 5.0);
	bool periodic = (!paused && (now - rep_last_ping_ms >= 15000u));

	rep_last_pos = pos;
	if (pause_changed || seeked || periodic) {
		rep_was_paused = paused;
		rep_last_ping_ms = now;
		jellyfin_report_progress_async(rep_item_id, rep_session_id, ticks, paused, rep_transcoded);
	}
}

static void handle_result(jf_result_t *res) {
	switch (res->job.kind) {
	case JF_JOB_BROWSE:
		if (res->count < 0) {
			toast_error(res->error[0] ? res->error : "jellyfin_connection_failed");
			rebuild_list_ui();
		} else {
			free(shown_entries);
			shown_entries = res->entries;
			res->entries = NULL;
			shown_count = res->count;
			shown_more = res->more;
			current_view = res->job;
			rebuild_list_ui();
		}
		break;

	case JF_JOB_AUTH:
		memset(draft.password, 0, sizeof(draft.password));
		if (res->count < 0) {
			toast_error(res->error[0] ? res->error : "jellyfin_auth_failed");
			refresh_settings_ui();
		} else {
			jellyfin_account_info_t info;
			jellyfin_account_info(&info);
			snprintf(draft.server, sizeof(draft.server), "%s", info.server);
			snprintf(draft.username, sizeof(draft.username), "%s", info.username);
			draft_remember = info.remembered;
			refresh_settings_ui();
			toast_success("jellyfin_connected");
			if (lv_screen_active() == settings_screen)
				switch_screen_return_to(jellyfin_screen);
		}
		break;

	case JF_JOB_PING:
		if (res->count < 0) {
			toast_error(res->error[0] ? res->error : "jellyfin_connection_failed");
		} else {
			refresh_settings_ui();
			toast_success("jellyfin_connected");
		}
		break;

	case JF_JOB_PLAY:
		waiting_for_track = false;
		loading_item_id[0] = '\0';
		refresh_track_loading_ui();
		if (res->count < 0) {
			toast_error(res->error[0] ? res->error : "jellyfin_stream_failed");
		} else {
			jellyfincache_protect(res->resolved_path);
			int cur_idx = playlist_current_index();
			if (cur_idx >= 0)
				(void)device_state_play_queue_index(cur_idx);
			else
				device_state_play_file(res->resolved_path);
			player_refresh_now_playing();
			player_sheet_open(true);
		}
		break;

	case JF_JOB_QC_INIT:
		if (res->qc_state == JELLYFIN_QC_PENDING) {
			qc_session = res->qc_out;
			qc_active = true;
			qc_poll_in_flight = false;

			char pretty[32];
			format_qc_code(qc_session.code, pretty, sizeof(pretty));
			if (qc_code_label)
				lv_label_set_text(qc_code_label, pretty);
			if (qc_qr && qc_session.web_url[0]) {
				if (lv_qrcode_update(qc_qr, qc_session.web_url, (uint32_t)strlen(qc_session.web_url)) == LV_RESULT_OK)
					lv_obj_set_hidden(qc_qr, false);
				else
					lv_obj_set_hidden(qc_qr, true);
			}
			if (qc_hint_label)
				lv_label_set_text(qc_hint_label, tr_or_raw("jellyfin_qc_note"));
			if (qc_status_label)
				lv_label_set_text(qc_status_label, tr_or_raw("jellyfin_qc_waiting"));
			if (qc_retry_btn)
				lv_obj_set_hidden(qc_retry_btn, true);

			if (!qc_poll_timer)
				qc_poll_timer = lv_timer_create(qc_poll_timer_cb, JF_QC_POLL_MS, NULL);
		} else {
			stop_quick_connect();
			if (qc_code_label)
				lv_label_set_text(qc_code_label, "—");
			if (qc_status_label)
				lv_label_set_text(qc_status_label, tr_or_raw(res->error[0] ? res->error : "jellyfin_qc_disabled"));
			if (qc_retry_btn)
				lv_obj_set_hidden(qc_retry_btn, false);
			toast_error(res->error[0] ? res->error : "jellyfin_qc_disabled");
		}
		break;

	case JF_JOB_QC_POLL:
		qc_poll_in_flight = false;
		if (!qc_active)
			break;
		if (res->qc_state == JELLYFIN_QC_OK) {
			stop_quick_connect();
			jellyfin_account_info_t info;
			jellyfin_account_info(&info);
			snprintf(draft.server, sizeof(draft.server), "%s", info.server);
			snprintf(draft.username, sizeof(draft.username), "%s", info.username);
			memset(draft.password, 0, sizeof(draft.password));
			draft_remember = info.remembered;
			refresh_settings_ui();
			toast_success("jellyfin_connected");
			switch_screen_return_to(jellyfin_screen);
		} else if (res->qc_state == JELLYFIN_QC_EXPIRED || res->qc_state == JELLYFIN_QC_DISABLED) {
			stop_quick_connect();
			if (qc_status_label)
				lv_label_set_text(qc_status_label, tr_or_raw(res->error[0] ? res->error : "jellyfin_qc_expired"));
			if (qc_retry_btn)
				lv_obj_set_hidden(qc_retry_btn, false);
		} else if (res->qc_state == JELLYFIN_QC_ERROR) {
			// Keep polling on transient Wi-Fi glitches, update subtle status.
			if (qc_status_label)
				lv_label_set_text(qc_status_label, tr_or_raw("jellyfin_qc_waiting"));
		}
		break;

	case JF_JOB_FORGET:
		memset(&draft, 0, sizeof(draft));
		draft_remember = true;
		refresh_settings_ui();
		toast_success("jellyfin_forgotten");
		if (lv_screen_active() == settings_screen)
			switch_screen_return_to(jellyfin_screen);
		break;
	}
}

static void watch_timer_cb(lv_timer_t *t) {
	(void)t;

	jf_result_t *res = NULL;
	pthread_mutex_lock(&job_lock);
	if (pending_result) {
		res = pending_result;
		pending_result = NULL;
	}
	pthread_mutex_unlock(&job_lock);

	if (res) {
		handle_result(res);
		free_result(res);
	}

	if (waiting_for_track)
		refresh_track_loading_ui();

	watch_playback_reporting();

	// Lookahead prefetch next Jellyfin track once playback passes 80%.
	if (audio_get_status() == AUDIO_STATUS_PLAYING && !waiting_for_track) {
		char current[512], next[512];
		audio_get_current_file(current, sizeof(current));
		if (jellyfincache_owns(current)) {
			double pos = 0.0, dur = 0.0;
			audio_get_progress(&pos, &dur);
			if (dur > 0.0 && pos >= dur * 0.80 && playlist_peek_auto(next, sizeof(next)) &&
				jellyfincache_owns(next)) {
				char next_id[JELLYFIN_ID_MAX];
				if (jellyfincache_item_id_of(next, next_id, sizeof(next_id)))
					request_prefetch(next_id);
			}
		}
	}
}

// ---------------------------------------------------------------------------
// Screen builders
// ---------------------------------------------------------------------------

static void build_search_screen(gui_config_t *cfg) {
	bool compact = bp_is_tempotec_v1();
	search_screen = lv_obj_create(NULL);
	lv_obj_add_style(search_screen, &theme_style_screen, 0);
	switcher_attach_back_gesture(search_screen);

	lv_obj_t *title = settingsrow_title(search_screen, cfg, "jellyfin_search");
	settingsrow_title_corner_slots(title, cfg, 0);

	int top = settingsrow_content_top(cfg);
	int pad = compact ? 10 : 18;
	search_field = lv_textarea_create(search_screen);
	lv_obj_set_pos(search_field, pad, top);
	lv_obj_set_size(search_field, cfg->screen_width - pad * 2, compact ? 38 : 54);
	lv_textarea_set_one_line(search_field, true);
	lv_textarea_set_placeholder_text(search_field, tr_or_raw("jellyfin_search_placeholder"));
	lv_obj_set_style_text_font(search_field, compact ? &font_ui_18 : &font_ui_24, 0);
	lv_obj_add_style(search_field, &theme_style_card, 0);
	lv_obj_add_style(search_field, &theme_style_text, 0);
	keyboard_style_caret(search_field);

	int kb_h = compact ? 176 : 260;
	search_keyboard = keyboard_create(search_screen, cfg->screen_width, kb_h, search_field, &icon_search, NULL,
									  search_accept_cb, NULL);
}

static void build_edit_screen(gui_config_t *cfg) {
	bool compact = bp_is_tempotec_v1();
	edit_screen = lv_obj_create(NULL);
	lv_obj_add_style(edit_screen, &theme_style_screen, 0);
	switcher_attach_back_gesture(edit_screen);

	edit_title = settingsrow_title(edit_screen, cfg, tr_or_raw("jellyfin_server_url"));
	settingsrow_title_corner_slots(edit_title, cfg, 0);

	int top = settingsrow_content_top(cfg);
	int pad = compact ? 10 : 18;
	edit_field = lv_textarea_create(edit_screen);
	lv_obj_set_pos(edit_field, pad, top);
	lv_obj_set_size(edit_field, cfg->screen_width - pad * 2, compact ? 38 : 54);
	lv_textarea_set_one_line(edit_field, true);
	lv_obj_set_style_text_font(edit_field, compact ? &font_ui_18 : &font_ui_24, 0);
	lv_obj_add_style(edit_field, &theme_style_card, 0);
	lv_obj_add_style(edit_field, &theme_style_text, 0);
	keyboard_style_caret(edit_field);

	int kb_h = compact ? 176 : 260;
	edit_keyboard =
		keyboard_create(edit_screen, cfg->screen_width, kb_h, edit_field, NULL, "OK", edit_accept_cb, NULL);
}

static void build_list_screen(gui_config_t *cfg) {
	list_screen = lv_obj_create(NULL);
	lv_obj_add_style(list_screen, &theme_style_screen, 0);
	switcher_attach_back_gesture(list_screen);

	list_container = settingsrow_page(list_screen, cfg, "jellyfin");
	list_title = settingsrow_page_title(list_screen);
	switcher_attach_back_gesture(list_container);
	player_sheet_attach_drag(list_screen, true);
	player_sheet_attach_drag(list_container, true);

	list_corner_btn = lv_btn_create(list_screen);
	lv_obj_add_style(list_corner_btn, &theme_style_card, 0);
	lv_obj_add_style(list_corner_btn, &theme_style_card_pressed, LV_STATE_PRESSED);
	lv_obj_set_style_radius(list_corner_btn, LV_RADIUS_CIRCLE, 0);
	lv_obj_set_style_pad_all(list_corner_btn, 0, 0);
	settingsrow_place_corner_button(list_corner_btn, cfg, 0);
	lv_obj_add_event_cb(list_corner_btn, list_corner_action_cb, LV_EVENT_CLICKED, NULL);

	list_corner_icon = lv_image_create(list_corner_btn);
	lv_image_set_src(list_corner_icon, &icon_download);
	settingsrow_scale_corner_icon(list_corner_icon, cfg);
	lv_obj_add_style(list_corner_icon, &theme_style_icon, 0);
	lv_obj_center(list_corner_icon);
	lv_obj_set_hidden(list_corner_btn, true);

	switcher_set_back_guard(list_screen, list_back_guard);
	switcher_set_back_guard_peek(list_screen, list_back_guard_peek);
}

static void build_qc_screen(gui_config_t *cfg) {
	bool compact = bp_is_tempotec_v1();
	qc_screen = lv_obj_create(NULL);
	lv_obj_add_style(qc_screen, &theme_style_screen, 0);
	switcher_attach_back_gesture(qc_screen);

	lv_obj_t *box = settingsrow_page(qc_screen, cfg, "jellyfin_quick_connect");
	lv_obj_t *title = settingsrow_page_title(qc_screen);
	settingsrow_title_corner_slots(title, cfg, 0);
	lv_obj_set_flex_align(box, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_row(box, compact ? 8 : 14, 0);
	switcher_attach_back_gesture(box);

	qc_code_label = lv_label_create(box);
	lv_label_set_text(qc_code_label, "...");
	lv_obj_set_style_text_font(qc_code_label, compact ? &font_ui_32 : &font_ui_36_bold, 0);
	lv_obj_set_style_text_color(qc_code_label, theme()->accent, 0);
	lv_obj_set_style_text_align(qc_code_label, LV_TEXT_ALIGN_CENTER, 0);

	// High-contrast black-on-white QR code with white quiet-zone border.
	int qr_size = compact ? 108 : 164;
	qc_qr = lv_qrcode_create(box);
	lv_qrcode_set_size(qc_qr, qr_size);
	lv_qrcode_set_dark_color(qc_qr, lv_color_black());
	lv_qrcode_set_light_color(qc_qr, lv_color_white());
	lv_obj_set_style_border_color(qc_qr, lv_color_white(), 0);
	lv_obj_set_style_border_width(qc_qr, compact ? 5 : 8, 0);
	lv_obj_set_style_bg_color(qc_qr, lv_color_white(), 0);
	lv_obj_set_style_bg_opa(qc_qr, LV_OPA_COVER, 0);
	lv_obj_set_hidden(qc_qr, true);

	qc_hint_label = lv_label_create(box);
	lv_label_set_text(qc_hint_label, tr_or_raw("jellyfin_qc_note"));
	lv_obj_set_width(qc_hint_label, lv_pct(100));
	lv_label_set_long_mode(qc_hint_label, LV_LABEL_LONG_WRAP);
	lv_obj_set_style_text_align(qc_hint_label, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_set_style_text_font(qc_hint_label, compact ? &font_ui_14 : &font_ui_18, 0);
	lv_obj_add_style(qc_hint_label, &theme_style_text_dim, 0);

	qc_status_label = lv_label_create(box);
	lv_label_set_text(qc_status_label, "");
	lv_obj_set_width(qc_status_label, lv_pct(100));
	lv_label_set_long_mode(qc_status_label, LV_LABEL_LONG_WRAP);
	lv_obj_set_style_text_align(qc_status_label, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_set_style_text_font(qc_status_label, compact ? &font_ui_14 : &font_ui_18, 0);
	lv_obj_add_style(qc_status_label, &theme_style_text, 0);

	qc_retry_btn = settingsrow_action(box, "jellyfin_retry", qc_retry_cb, NULL);
	lv_obj_set_hidden(qc_retry_btn, true);

	switcher_set_back_guard(qc_screen, qc_back_guard);
}

static void build_settings_screen(gui_config_t *cfg) {
	bool compact = bp_is_tempotec_v1();
	settings_screen = lv_obj_create(NULL);
	lv_obj_add_style(settings_screen, &theme_style_screen, 0);
	switcher_attach_back_gesture(settings_screen);

	lv_obj_t *list = settingsrow_page(settings_screen, cfg, "jellyfin_server_settings");
	lv_obj_t *title = settingsrow_page_title(settings_screen);
	settingsrow_title_corner_slots(title, cfg, 0);
	switcher_attach_back_gesture(list);

	add_stacked_value_row(list, "jellyfin_server_url", &server_value, edit_server_cb);
	settingsrow_action(list, "jellyfin_quick_connect", quick_connect_row_cb, NULL);
	add_stacked_value_row(list, "jellyfin_username", &user_value, edit_user_cb);
	add_stacked_value_row(list, "password", &pass_value, edit_pass_cb);

	settingsrow_toggle(list, "jellyfin_remember", &remember_switch, remember_switch_cb);
	settingsrow_toggle(list, "jellyfin_allow_self_signed", &self_signed_switch, self_signed_switch_cb);

	settingsrow_action(list, "jellyfin_connect_save", connect_save_cb, NULL);
	add_stacked_value_row(list, "jellyfin_library", &library_value, pick_library_cb);

	// Streaming quality pills
	lv_obj_t *q_pills = NULL;
	settingsrow_pills(list, "jellyfin_quality", &q_pills);
	quality_pills[0] = settingsrow_pill(q_pills, "jellyfin_quality_original", JELLYFIN_QUALITY_ORIGINAL,
										quality_pill_cb);
	quality_pills[1] = settingsrow_pill_text(q_pills, "MP3 320", JELLYFIN_QUALITY_MP3_320, quality_pill_cb);
	quality_pills[2] = settingsrow_pill_text(q_pills, "MP3 192", JELLYFIN_QUALITY_MP3_192, quality_pill_cb);
	quality_pills[3] = settingsrow_pill_text(q_pills, "Opus 128", JELLYFIN_QUALITY_OPUS_128, quality_pill_cb);

	// SD cache limit pills
	lv_obj_t *c_pills = NULL;
	settingsrow_pills(list, "jellyfin_cache_limit", &c_pills);
	cache_limit_pills[0] = settingsrow_pill_text(c_pills, "512 MB", 512, cache_limit_pill_cb);
	cache_limit_pills[1] = settingsrow_pill_text(c_pills, "1 GB", 1024, cache_limit_pill_cb);
	cache_limit_pills[2] = settingsrow_pill_text(c_pills, "2 GB", 2048, cache_limit_pill_cb);
	cache_limit_pills[3] = settingsrow_pill_text(c_pills, "4 GB", 4096, cache_limit_pill_cb);
	cache_limit_pills[4] = settingsrow_pill_text(c_pills, "8 GB", 8192, cache_limit_pill_cb);
	cache_limit_pills[5] = settingsrow_pill(c_pills, "jellyfin_cache_unlimited", 0, cache_limit_pill_cb);

	add_stacked_value_row(list, "jellyfin_clear_cache", &cache_value, clear_cache_cb);
	settingsrow_toggle(list, "jellyfin_report_playback", &report_switch, report_switch_cb);
	settingsrow_action(list, "jellyfin_test_connection", test_connection_cb, NULL);
	settingsrow_action(list, "jellyfin_forget_server", forget_server_cb, NULL);

	lv_obj_t *note = lv_label_create(list);
	lv_label_set_text(note, tr_or_raw("jellyfin_settings_note"));
	lv_obj_set_width(note, lv_pct(100));
	lv_label_set_long_mode(note, LV_LABEL_LONG_WRAP);
	lv_obj_set_style_pad_top(note, compact ? 4 : 8, 0);
	lv_obj_set_style_pad_bottom(note, compact ? 12 : 20, 0);
	lv_obj_set_style_text_font(note, compact ? &font_ui_14 : &font_ui_18, 0);
	lv_obj_add_style(note, &theme_style_text_dim, 0);
}

// ---------------------------------------------------------------------------
// Public entry point
// ---------------------------------------------------------------------------

void jellyfinpage_init(gui_config_t *cfg) {
	page_cfg = cfg;
	value_row_count = 0;
	jellyfin_init();
	jellyfincache_set_root(storage_sd_root());

	jellyfin_account_info_t info;
	jellyfin_account_info(&info);
	memset(&draft, 0, sizeof(draft));
	if (info.server[0])
		snprintf(draft.server, sizeof(draft.server), "%s", info.server);
	if (info.username[0])
		snprintf(draft.username, sizeof(draft.username), "%s", info.username);
	draft_remember = info.logged_in ? info.remembered : true;

	jellyfin_screen = lv_obj_create(NULL);
	lv_obj_add_style(jellyfin_screen, &theme_style_screen, 0);
	switcher_attach_back_gesture(jellyfin_screen);
	player_sheet_attach_drag(jellyfin_screen, true);

	lv_obj_t *title = settingsrow_title(jellyfin_screen, cfg, "jellyfin");
	settingsrow_title_corner_slots(title, cfg, 1);

	lv_obj_t *search_btn = lv_btn_create(jellyfin_screen);
	lv_obj_add_style(search_btn, &theme_style_card, 0);
	lv_obj_add_style(search_btn, &theme_style_card_pressed, LV_STATE_PRESSED);
	lv_obj_set_style_radius(search_btn, LV_RADIUS_CIRCLE, 0);
	lv_obj_set_style_pad_all(search_btn, 0, 0);
	settingsrow_place_corner_button(search_btn, cfg, 0);
	lv_obj_add_event_cb(search_btn, open_search_cb, LV_EVENT_CLICKED, NULL);

	lv_obj_t *search_ic = lv_image_create(search_btn);
	lv_image_set_src(search_ic, &icon_search);
	settingsrow_scale_corner_icon(search_ic, cfg);
	lv_obj_add_style(search_ic, &theme_style_icon, 0);
	lv_obj_center(search_ic);

	const grid_entry_t home_entries[JF_HOME_TILES] = {
		{"jellyfin_folders", &icon_menu_sub_folders, NULL, home_folders_cb},
		{"jellyfin_playlists", &icon_menu_qobuz_playlists, NULL, home_playlists_cb},
		{"jellyfin_artists", &icon_menu_sub_artists, NULL, home_artists_cb},
		{"jellyfin_albums", &icon_menu_qobuz_albums, NULL, home_albums_cb},
		{"jellyfin_genres", &icon_menu_qobuz_genres, NULL, home_genres_cb},
		{"jellyfin_favorites", &icon_menu_qobuz_favourites, NULL, home_favorites_cb},
		{"jellyfin_saved", &icon_jellyfin_saved, NULL, home_saved_cb},
		{"jellyfin_server_settings", &icon_menu_sub_server, NULL, home_settings_cb},
	};
	home_grid = gridpage_build(jellyfin_screen, cfg, home_entries, JF_HOME_TILES, 2, 3, true);
	for (int i = 0; i < JF_HOME_TILES; i++)
		gridpage_set_tile_label_lines(home_grid, i, 2);
	switcher_attach_back_gesture(home_grid);

	build_search_screen(cfg);
	build_edit_screen(cfg);
	build_list_screen(cfg);
	build_qc_screen(cfg);
	build_settings_screen(cfg);

	theme_register_refresh(theme_refresh_cb);
	theme_refresh_cb();

	device_state_add_prepare_cb(jellyfinpage_prepare_track);
	lv_timer_create(watch_timer_cb, 1000, NULL);
}
