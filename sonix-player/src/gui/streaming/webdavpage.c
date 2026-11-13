#include "webdavpage.h"
#include "webdavdlpage.h"

#include <pthread.h>
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
#include "src/system/streaming/streamturn.h"
#include "src/system/streaming/webdav.h"
#include "src/system/streaming/webdavcache.h"
#include "src/system/streaming/webdavdl.h"
#include "src/system/streaming/webdavstream.h"

#define WEBDAV_HOME_TILES 4
#define WEBDAV_TILE_ART_PX 54
#define WEBDAV_TILE_GLYPH_PX 46
#define WEBDAV_HISTORY_MAX 32
#define WEBDAV_CACHE_PILLS 6
#define WEBDAV_MAX_VALUE_ROWS 12

typedef enum {
	JOB_NONE = 0,
	JOB_PING,
	JOB_CONNECT,
	JOB_BROWSE,
	JOB_PLAY,
} job_kind_t;

typedef struct {
	job_kind_t kind;
	unsigned serial;
	int offset;
	char rel_path[WEBDAV_PATH_MAX];
	char title[WEBDAV_TITLE_MAX];
	webdav_credentials_t credentials;
	bool remember;
	webdav_entry_t play_track;
	bool start_queue_after_play;
} job_t;

typedef struct {
	job_t job;
	webdav_entry_t *entries;
	int count;
	bool more;
	char resolved_path[560];
	char error[192];
} result_t;

typedef struct {
	lv_obj_t *row;
	lv_obj_t *icon;
	lv_obj_t *text_col;
	lv_obj_t *title;
	lv_obj_t *value;
	theme_semantic_t tone;
} wd_value_row_t;

lv_obj_t *webdav_screen;

static gui_config_t *config;
static lv_obj_t *home_grid;
static lv_obj_t *list_screen, *list_container, *list_title;
static lv_obj_t *settings_screen, *server_value, *user_value, *pass_value, *cache_value;
static lv_obj_t *remember_switch, *relaxed_switch, *sd_cache_switch;
static lv_obj_t *cache_limit_pills[WEBDAV_CACHE_PILLS];
static lv_obj_t *edit_screen, *edit_title, *edit_field;
static keyboard_t *keyboard;
static lv_obj_t *track_load_badge, *track_load_spin;

static wd_value_row_t value_rows[WEBDAV_MAX_VALUE_ROWS];
static int value_row_count;

static webdav_credentials_t draft;
static bool draft_remember = true;
static int editing_field;

static job_t current_list;
static job_t history[WEBDAV_HISTORY_MAX];
static int history_count;
static webdav_entry_t *shown;
static int shown_count;
static bool shown_more;
static int menu_index;

// Queued tracks metadata for SD-Cache-On placeholder resolution & prefetch.
static pthread_mutex_t queue_meta_lock = PTHREAD_MUTEX_INITIALIZER;
static webdav_entry_t queue_tracks[WEBDAV_QUEUE_MAX];
static char queue_placeholders[WEBDAV_QUEUE_MAX][560];
static int queue_track_count;
static volatile bool waiting_for_track;

// Worker mailbox for PROPFIND / ping / connect / SD-cache prebuffer.
static pthread_mutex_t job_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t job_ready = PTHREAD_COND_INITIALIZER;
static bool worker_started;
static bool have_job;
static bool configuring;
static unsigned serial = 1;
static unsigned failed_serial;
static job_t pending;
static result_t *mail;
static bool busy;
static lv_obj_t *job_owner;

// Low-priority SD-cache prefetch worker for gapless next-track lookahead.
static pthread_mutex_t prefetch_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t prefetch_cond = PTHREAD_COND_INITIALIZER;
static bool prefetch_started;
static bool have_prefetch;
static webdav_entry_t pending_prefetch;

static const int CACHE_MB_VALUES[WEBDAV_CACHE_PILLS] = {256, 512, 1024, 2048, 4096, 0};

static void wipe_bytes(void *buf, size_t len) {
	volatile unsigned char *p = (volatile unsigned char *)buf;
	while (len--) {
		*p++ = 0;
	}
}

bool webdavpage_waiting_for_track(void) {
	return waiting_for_track;
}

lv_obj_t *webdavpage_settings_screen(void) { return settings_screen; }
lv_obj_t *webdavpage_list_screen(void) { return list_screen; }
lv_obj_t *webdavpage_edit_screen(void) { return edit_screen; }

static void record_queued_tracks(const webdav_entry_t *tracks, char placeholders[][560], int count) {
	pthread_mutex_lock(&queue_meta_lock);
	queue_track_count = count < WEBDAV_QUEUE_MAX ? count : WEBDAV_QUEUE_MAX;
	for (int i = 0; i < queue_track_count; i++) {
		queue_tracks[i] = tracks[i];
		snprintf(queue_placeholders[i], sizeof(queue_placeholders[i]), "%s", placeholders[i]);
	}
	pthread_mutex_unlock(&queue_meta_lock);
}

static bool lookup_queued_placeholder(const char *placeholder, webdav_entry_t *out) {
	if (!placeholder || !placeholder[0]) {
		return false;
	}
	pthread_mutex_lock(&queue_meta_lock);
	for (int i = 0; i < queue_track_count; i++) {
		if (strcmp(queue_placeholders[i], placeholder) == 0) {
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

static void refresh_track_loading_badge(void) {
	if (!track_load_badge) {
		return;
	}
	if (track_load_spin) {
		lv_obj_set_style_image_recolor(track_load_spin, theme()->accent, 0);
	}
	if (waiting_for_track) {
		lv_obj_set_hidden(track_load_badge, false);
		lv_obj_move_foreground(track_load_badge);
	} else {
		lv_obj_set_hidden(track_load_badge, true);
	}
}

static void track_loading_async(void *user) {
	(void)user;
	refresh_track_loading_badge();
}

static void free_result(result_t *r) {
	if (r) {
		wipe_bytes(&r->job.credentials, sizeof(r->job.credentials));
		free(r->entries);
		free(r);
	}
}

static void *prefetch_worker_main(void *arg) {
	(void)arg;
	thread_be_low_priority("webdav prefetch");
	for (;;) {
		pthread_mutex_lock(&prefetch_lock);
		while (!have_prefetch) {
			pthread_cond_wait(&prefetch_cond, &prefetch_lock);
		}
		webdav_entry_t trk = pending_prefetch;
		have_prefetch = false;
		pthread_mutex_unlock(&prefetch_lock);

		if (!waiting_for_track && trk.path[0] && !webdavcache_cached(&trk, NULL, 0)) {
			char out[560] = "", err[96] = "";
			(void)webdavcache_get(&trk, out, sizeof(out), err, sizeof(err));
		}
	}
	return NULL;
}

static void request_sd_prefetch(const char *placeholder_path) {
	webdav_entry_t trk;
	if (!lookup_queued_placeholder(placeholder_path, &trk) || webdavcache_cached(&trk, NULL, 0)) {
		return;
	}
	pthread_mutex_lock(&prefetch_lock);
	if (!prefetch_started) {
		pthread_t tid;
		if (pthread_create(&tid, NULL, prefetch_worker_main, NULL) == 0) {
			pthread_detach(tid);
			prefetch_started = true;
		}
	}
	if (prefetch_started) {
		pending_prefetch = trk;
		have_prefetch = true;
		pthread_cond_signal(&prefetch_cond);
	}
	pthread_mutex_unlock(&prefetch_lock);
}

static void *worker_main(void *arg) {
	(void)arg;
	thread_be_low_priority("webdav");
	for (;;) {
		pthread_mutex_lock(&job_lock);
		while (!have_job) {
			pthread_cond_wait(&job_ready, &job_lock);
		}
		job_t job = pending;
		wipe_bytes(&pending, sizeof(pending));
		have_job = false;
		configuring = (job.kind == JOB_CONNECT);
		pthread_mutex_unlock(&job_lock);

		result_t *res = calloc(1, sizeof(*res));
		if (!res) {
			wipe_bytes(&job.credentials, sizeof(job.credentials));
			pthread_mutex_lock(&job_lock);
			configuring = false;
			if (job.serial == serial) {
				failed_serial = job.serial;
			}
			pthread_mutex_unlock(&job_lock);
			continue;
		}
		res->job = job;

		if (job.kind == JOB_BROWSE) {
			res->entries = calloc(WEBDAV_PAGE_SIZE, sizeof(*res->entries));
			if (!res->entries) {
				res->count = -1;
				snprintf(res->error, sizeof(res->error), "%s", "webdav_out_of_memory");
			} else {
				res->count = webdav_browse(job.rel_path, job.offset, res->entries, WEBDAV_PAGE_SIZE,
										   &res->more, res->error, sizeof(res->error));
			}
		} else if (job.kind == JOB_CONNECT) {
			webdav_credentials_t previous;
			bool prev_remember = false;
			bool had_prev = webdav_configured();
			webdav_credentials(&previous, &prev_remember);

			bool ok = webdav_configure(&job.credentials, job.remember, res->error, sizeof(res->error));
			if (ok) {
				ok = webdav_ping(res->error, sizeof(res->error));
				if (!ok) {
					if (had_prev) {
						char ignored[64];
						(void)webdav_configure(&previous, prev_remember, ignored, sizeof(ignored));
					} else {
						(void)webdav_forget();
					}
				}
			}
			wipe_bytes(&previous, sizeof(previous));
			res->count = ok ? 0 : -1;
		} else if (job.kind == JOB_PING) {
			res->count = webdav_ping(res->error, sizeof(res->error)) ? 0 : -1;
		} else if (job.kind == JOB_PLAY) {
			bool ok = webdavcache_get(&job.play_track, res->resolved_path, sizeof(res->resolved_path),
									  res->error, sizeof(res->error));
			res->count = ok ? 0 : -1;
		}

		wipe_bytes(&job.credentials, sizeof(job.credentials));
		wipe_bytes(&res->job.credentials, sizeof(res->job.credentials));

		pthread_mutex_lock(&job_lock);
		configuring = false;
		if (job.serial != serial) {
			pthread_mutex_unlock(&job_lock);
			free_result(res);
			continue;
		}
		free_result(mail);
		mail = res;
		pthread_mutex_unlock(&job_lock);
	}
	return NULL;
}

static bool ensure_worker(void) {
	pthread_mutex_lock(&job_lock);
	if (!worker_started) {
		pthread_t tid;
		if (pthread_create(&tid, NULL, worker_main, NULL) == 0) {
			pthread_detach(tid);
			worker_started = true;
		}
	}
	bool ok = worker_started;
	pthread_mutex_unlock(&job_lock);
	return ok;
}

static bool submit_job(job_t *job, lv_obj_t *owner, const char *busy_key) {
	if (!ensure_worker()) {
		toast_error("webdav_out_of_memory");
		return false;
	}
	pthread_mutex_lock(&job_lock);
	job->serial = ++serial;
	pending = *job;
	have_job = true;
	free_result(mail);
	mail = NULL;
	pthread_cond_signal(&job_ready);
	pthread_mutex_unlock(&job_lock);

	busy = true;
	job_owner = owner;
	if (busy_key) {
		toast_busy(busy_key);
	}
	return true;
}

static void invalidate_job(void) {
	bool cancel_cache = waiting_for_track;
	pthread_mutex_lock(&job_lock);
	serial++;
	have_job = false;
	wipe_bytes(&pending, sizeof(pending));
	free_result(mail);
	mail = NULL;
	pthread_mutex_unlock(&job_lock);

	busy = false;
	waiting_for_track = false;
	job_owner = NULL;
	toast_busy_end();
	refresh_track_loading_badge();
	if (cancel_cache) {
		webdavcache_cancel();
	}
}

static void page_left_cb(lv_event_t *e) {
	if (busy && job_owner && lv_event_get_target(e) == job_owner) {
		invalidate_job();
	}
}

// ---------------------------------------------------------------------------
// SD-Cache placeholder resolution callback for device_state
// ---------------------------------------------------------------------------

bool webdavpage_prepare_track(char *path, size_t path_size) {
	if (!webdavcache_is_placeholder(path)) {
		if (waiting_for_track && !webdavcache_owns(path)) {
			waiting_for_track = false;
			gui_post(track_loading_async, NULL);
		}
		return true;
	}

	webdav_entry_t trk;
	if (!lookup_queued_placeholder(path, &trk)) {
		return false;
	}

	char real[560] = "";
	if (webdavcache_cached(&trk, real, sizeof(real))) {
		if (strlen(real) >= path_size) {
			return false;
		}
		snprintf(path, path_size, "%s", real);
		waiting_for_track = false;
		gui_post(track_loading_async, NULL);
		return true;
	}

	waiting_for_track = true;
	device_state_stop();

	if (ensure_worker()) {
		job_t job = {
			.kind = JOB_PLAY,
			.play_track = trk,
			.start_queue_after_play = true,
		};
		pthread_mutex_lock(&job_lock);
		job.serial = ++serial;
		pending = job;
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
// Responsive 240x320 stacked settings rows & cache pills
// ---------------------------------------------------------------------------

static void fit_stacked_title(wd_value_row_t *vr) {
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

static void value_row_resized_cb(lv_event_t *e) {
	fit_stacked_title((wd_value_row_t *)lv_event_get_user_data(e));
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

	if (value_row_count < WEBDAV_MAX_VALUE_ROWS) {
		wd_value_row_t *slot = &value_rows[value_row_count++];
		slot->row = row;
		slot->icon = icon;
		slot->text_col = col;
		slot->title = title;
		slot->value = val;
		slot->tone = tone;
		lv_obj_add_event_cb(col, value_row_resized_cb, LV_EVENT_SIZE_CHANGED, slot);
		lv_obj_add_event_cb(title, value_row_resized_cb, LV_EVENT_REFR_EXT_DRAW_SIZE, slot);
	}
	return row;
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

static void add_note(lv_obj_t *container, const char *key) {
	lv_obj_t *note = lv_label_create(container);
	lv_obj_add_style(note, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(note, &font_ui_18, 0);
	lv_obj_set_width(note, lv_pct(100));
	lv_label_set_long_mode(note, LV_LABEL_LONG_WRAP);
	lv_label_set_text(note, tr(key));
}

static void refresh_cache_pills(void) {
	int cur_mb = webdavcache_max_mb();
	bool sd_on = webdav_sd_cache();
	for (int i = 0; i < WEBDAV_CACHE_PILLS; i++) {
		if (!cache_limit_pills[i]) {
			continue;
		}
		settingsrow_pill_active(cache_limit_pills[i], cur_mb == CACHE_MB_VALUES[i]);
		if (sd_on) {
			lv_obj_remove_state(cache_limit_pills[i], LV_STATE_DISABLED);
		} else {
			lv_obj_add_state(cache_limit_pills[i], LV_STATE_DISABLED);
		}
	}
}

static void refresh_cache_usage_label(void) {
	if (!cache_value) {
		return;
	}
	int files = 0;
	uint64_t bytes = 0;
	webdavcache_stats(&files, &bytes);
	if (files <= 0) {
		lv_label_set_text(cache_value, "0 MB");
	} else {
		char buf[96];
		snprintf(buf, sizeof(buf), "%d \xC2\xB7 %.1f MB", files, (double)bytes / (1024.0 * 1024.0));
		lv_label_set_text(cache_value, buf);
	}
}

static void settings_refresh(void) {
	if (!settings_screen) {
		return;
	}
	lv_label_set_text(server_value, draft.server[0] ? draft.server : tr("webdav_not_set"));
	lv_label_set_text(user_value, draft.username[0] ? draft.username : tr("webdav_anonymous"));
	lv_label_set_text(pass_value, draft.password[0] ? "\xE2\x80\xA2\xE2\x80\xA2\xE2\x80\xA2\xE2\x80\xA2\xE2\x80\xA2\xE2\x80\xA2"
													: tr("webdav_not_set"));
	if (draft_remember) {
		lv_obj_add_state(remember_switch, LV_STATE_CHECKED);
	} else {
		lv_obj_remove_state(remember_switch, LV_STATE_CHECKED);
	}
	if (tls_verify_mode() == TLS_VERIFY_RELAXED) {
		lv_obj_add_state(relaxed_switch, LV_STATE_CHECKED);
	} else {
		lv_obj_remove_state(relaxed_switch, LV_STATE_CHECKED);
	}
	if (webdav_sd_cache()) {
		lv_obj_add_state(sd_cache_switch, LV_STATE_CHECKED);
	} else {
		lv_obj_remove_state(sd_cache_switch, LV_STATE_CHECKED);
	}
	refresh_cache_pills();
	refresh_cache_usage_label();
}

static void home_style_tiles(void) {
	if (!home_grid) {
		return;
	}
	static const lv_image_dsc_t *const art[WEBDAV_HOME_TILES] = {
		&icon_menu_sub_folders,
		&icon_menu_sc_playlists,
		&icon_menu_sub_test,
		&icon_menu_sub_server,
	};
	static const lv_image_dsc_t *const glyphs[WEBDAV_HOME_TILES] = {
		&icon_sub_folders_glyph,
		&icon_sc_playlists_glyph,
		&icon_sub_test_glyph,
		&icon_sub_server_glyph,
	};
	static const theme_semantic_t tones[WEBDAV_HOME_TILES] = {
		THEME_SEMANTIC_CYAN,
		THEME_SEMANTIC_PURPLE,
		THEME_SEMANTIC_GREEN,
		THEME_SEMANTIC_BLUE,
	};
	bool glyph = theme_uses_function_glyphs();
	for (int i = 0; i < WEBDAV_HOME_TILES; i++) {
		gridpage_set_tile_icon_style(home_grid, i, glyph ? glyphs[i] : art[i],
									 glyph ? WEBDAV_TILE_GLYPH_PX : WEBDAV_TILE_ART_PX, glyph,
									 theme_semantic_color(tones[i]), false);
		gridpage_set_tile_orientation(home_grid, i, false);
	}
}

static void refresh_webdav_theme(void) {
	home_style_tiles();
	refresh_cache_pills();
	bool compact = bp_is_tempotec_v1();
	bool fn_glyphs = theme_uses_function_glyphs();
	bool flat = compact && fn_glyphs;

	for (int i = 0; i < value_row_count; i++) {
		wd_value_row_t *vr = &value_rows[i];
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
}

// ---------------------------------------------------------------------------
// Settings & Edit handlers
// ---------------------------------------------------------------------------

static void open_settings_cb(lv_event_t *e) {
	(void)e;
	if ((busy && !waiting_for_track) || switcher_back_drag_active()) {
		return;
	}
	webdav_credentials(&draft, &draft_remember);
	settings_refresh();
	switch_screen(settings_screen);
}

static void open_edit_field(int field) {
	if (field < 0 || field > 2) {
		return;
	}
	editing_field = field;
	const char *keys[] = {"webdav_server_url", "webdav_username", "password"};
	const char *values[] = {draft.server, draft.username, draft.password};
	const size_t sizes[] = {sizeof(draft.server), sizeof(draft.username), sizeof(draft.password)};

	lv_label_set_text(edit_title, tr(keys[editing_field]));
	lv_textarea_set_max_length(edit_field, (uint32_t)sizes[editing_field] - 1);
	lv_textarea_set_password_mode(edit_field, false);
	lv_textarea_set_text(edit_field, values[editing_field]);
	lv_textarea_set_placeholder_text(edit_field,
									 editing_field == 0 ? "https://nas.local/webdav" : tr(keys[editing_field]));
	keyboard_reset(keyboard);
	keyboard_set_visible(keyboard, true);
	switch_screen(edit_screen);
}

static void edit_cb(lv_event_t *e) {
	if ((busy && !waiting_for_track) || switcher_back_drag_active()) {
		return;
	}
	open_edit_field((int)(intptr_t)lv_event_get_user_data(e));
}

static void accept_edit_cb(lv_event_t *e) {
	(void)e;
	char *destinations[] = {draft.server, draft.username, draft.password};
	const size_t sizes[] = {sizeof(draft.server), sizeof(draft.username), sizeof(draft.password)};
	const char *text = lv_textarea_get_text(edit_field);
	if (strlen(text) >= sizes[editing_field]) {
		toast_error("webdav_invalid_credentials");
		return;
	}
	snprintf(destinations[editing_field], sizes[editing_field], "%s", text);
	lv_textarea_set_text(edit_field, "");
	settings_refresh();
	switch_screen_return_to(settings_screen);
}

static void edit_left_cb(lv_event_t *e) {
	(void)e;
	lv_textarea_set_text(edit_field, "");
}

static void remember_cb(lv_event_t *e) {
	draft_remember = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
}

static void certificates_cb(lv_event_t *e) {
	(void)e;
	bool relaxed = lv_obj_has_state(relaxed_switch, LV_STATE_CHECKED);
	tls_set_verify_mode(relaxed ? TLS_VERIFY_RELAXED : TLS_VERIFY_STRICT);
}

static void sd_cache_toggle_cb(lv_event_t *e) {
	bool enabled = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
	webdav_set_sd_cache(enabled);
	refresh_cache_pills();
}

static void cache_limit_pill_cb(lv_event_t *e) {
	int mb = (int)(intptr_t)lv_event_get_user_data(e);
	webdavcache_set_max_mb(mb);
	refresh_cache_pills();
	refresh_cache_usage_label();
}

static void clear_cache_confirmed(void *user) {
	(void)user;
	if (waiting_for_track) {
		invalidate_job();
	}
	char cur[512];
	audio_get_current_file(cur, sizeof(cur));
	if (webdavcache_owns(cur)) {
		device_state_stop();
	}
	int removed = 0;
	if (webdavcache_clear(&removed)) {
		refresh_cache_usage_label();
		toast_success("webdav_cache_cleared");
	} else {
		toast_error("webdav_no_card");
	}
}

static void clear_cache_cb(lv_event_t *e) {
	(void)e;
	if ((!busy || waiting_for_track) && !switcher_back_drag_active() && !player_sheet_drag_active()) {
		confirm_show("webdav_clear_cache", NULL, NULL, clear_cache_confirmed, NULL);
	}
}

static void reset_library_state(void) {
	webdavstream_reset();
	webdavcache_cancel();
	pthread_mutex_lock(&queue_meta_lock);
	queue_track_count = 0;
	pthread_mutex_unlock(&queue_meta_lock);

	free(shown);
	shown = NULL;
	shown_count = 0;
	shown_more = false;
	history_count = 0;
	memset(&current_list, 0, sizeof(current_list));
	if (list_container) {
		lv_obj_clean(list_container);
	}
	if (list_title) {
		lv_label_set_text(list_title, tr("webdav_folders"));
	}
}

static void connect_cb(lv_event_t *e) {
	(void)e;
	if (busy && !waiting_for_track) {
		return;
	}
	if (waiting_for_track) {
		invalidate_job();
	}
	char current[512];
	audio_get_current_file(current, sizeof(current));
	if (webdavstream_owns(current) || webdavcache_owns(current)) {
		device_state_stop();
	}
	reset_library_state();
	job_t job = {
		.kind = JOB_CONNECT,
		.credentials = draft,
		.remember = draft_remember,
	};
	submit_job(&job, settings_screen, "loading");
	wipe_bytes(&job.credentials, sizeof(job.credentials));
}

static void forget_confirmed(void *user) {
	(void)user;
	invalidate_job();
	pthread_mutex_lock(&job_lock);
	bool saving = configuring;
	pthread_mutex_unlock(&job_lock);
	if (saving) {
		toast_error("webdav_settings_busy");
		return;
	}
	char current[512];
	audio_get_current_file(current, sizeof(current));
	if (webdavstream_owns(current) || webdavcache_owns(current)) {
		device_state_stop();
	}
	reset_library_state();
	bool removed = webdav_forget();
	wipe_bytes(&draft, sizeof(draft));
	draft_remember = true;
	settings_refresh();
	if (removed) {
		toast_success("webdav_forgotten");
	} else {
		toast_error("webdav_cannot_save_credentials");
	}
}

static void forget_cb(lv_event_t *e) {
	(void)e;
	if ((!busy || waiting_for_track) && !switcher_back_drag_active() && !player_sheet_drag_active()) {
		confirm_show("webdav_forget_server", "webdav_forget_note", "delete", forget_confirmed, NULL);
	}
}

static void ping_cb(lv_event_t *e) {
	(void)e;
	if (busy && !waiting_for_track) {
		return;
	}
	if (!webdav_configured()) {
		toast_error("webdav_not_configured");
		open_settings_cb(NULL);
		return;
	}
	if (waiting_for_track) {
		invalidate_job();
	}
	job_t job = {.kind = JOB_PING};
	submit_job(&job, webdav_screen, "loading");
}

// ---------------------------------------------------------------------------
// Folder Browser & Playback / Save-to-SD
// ---------------------------------------------------------------------------

static void run_list(job_t job, bool push_history) {
	if (!webdav_configured()) {
		toast_error("webdav_not_configured");
		open_settings_cb(NULL);
		return;
	}
	if (waiting_for_track) {
		invalidate_job();
	}
	if (push_history && current_list.kind == JOB_BROWSE && history_count < WEBDAV_HISTORY_MAX) {
		history[history_count++] = current_list;
	}
	lv_label_set_text(list_title, job.title[0] ? job.title : tr("webdav_folders"));
	if (lv_screen_active() != list_screen) {
		switch_screen(list_screen);
	}
	submit_job(&job, list_screen, "loading");
}

static bool list_back_has_step(void) {
	return history_count > 0;
}

static bool list_back(void) {
	if (history_count > 0) {
		run_list(history[--history_count], false);
		return true;
	}
	invalidate_job();
	return false;
}

static void page_cb(lv_event_t *e) {
	if (busy && !waiting_for_track) {
		return;
	}
	job_t job = current_list;
	job.offset += (int)(intptr_t)lv_event_get_user_data(e) * WEBDAV_PAGE_SIZE;
	if (job.offset >= 0) {
		run_list(job, false);
	}
}

static void menu_open_downloads(void *user) {
	(void)user;
	webdavdlpage_open();
}

static void menu_save_track(void *user) {
	(void)user;
	webdavdlpage_download_tracks(shown, shown_count, menu_index);
}

static void menu_save_current_folder(void *user) {
	(void)user;
	webdavdlpage_download_tracks(shown, shown_count, -1);
}

static void menu_save_subfolder(void *user) {
	(void)user;
	if (menu_index >= 0 && menu_index < shown_count && shown[menu_index].kind == WEBDAV_DIR) {
		webdavdlpage_download_folder(shown[menu_index].path, shown[menu_index].title);
	}
}

static void row_long_pressed_cb(lv_event_t *e) {
	if ((busy && !waiting_for_track) || switcher_back_drag_active() || player_sheet_drag_active()) {
		return;
	}
	int index = (int)(intptr_t)lv_event_get_user_data(e);
	if (index < 0 || index >= shown_count) {
		return;
	}
	menu_index = index;
	lv_indev_t *indev = lv_indev_active();
	if (indev) {
		lv_indev_wait_release(indev);
	}

	if (shown[index].kind == WEBDAV_DIR) {
		popover_item_t items[] = {
			{.label = "webdav_save_folder", .action = menu_save_subfolder},
			{.label = "webdav_downloads", .action = menu_open_downloads},
		};
		popover_show(lv_event_get_current_target(e), items, webdavdl_busy() ? 1 : 2);
		return;
	}

	popover_item_t items[] = {
		{.label = "webdav_save_track", .action = menu_save_track},
		{.label = "webdav_save_folder", .action = menu_save_current_folder},
		{.label = "webdav_downloads", .action = menu_open_downloads},
	};
	popover_show(lv_event_get_current_target(e), items, webdavdl_busy() ? 2 : 3);
}

static void start_sd_cached_playback(int index) {
	if (!storage_card_attached() || !storage_sd_writable()) {
		toast_error("webdav_no_card");
		return;
	}
	if (waiting_for_track) {
		invalidate_job();
	}

	webdav_entry_t *qtracks = calloc((size_t)shown_count, sizeof(*qtracks));
	char (*qpaths)[560] = calloc((size_t)shown_count, sizeof(*qpaths));
	const char **qptrs = calloc((size_t)shown_count, sizeof(*qptrs));
	if (!qtracks || !qpaths || !qptrs) {
		free(qtracks);
		free(qpaths);
		free(qptrs);
		toast_error("webdav_out_of_memory");
		return;
	}

	int qcount = 0;
	int selected_slot = 0;
	for (int i = 0; i < shown_count && qcount < WEBDAV_QUEUE_MAX; i++) {
		if (shown[i].kind != WEBDAV_TRACK) {
			continue;
		}
		if (!webdavcache_placeholder(&shown[i], qpaths[qcount], sizeof(qpaths[0]))) {
			continue;
		}
		if (i == index) {
			selected_slot = qcount;
		}
		qtracks[qcount] = shown[i];
		qptrs[qcount] = qpaths[qcount];
		qcount++;
	}

	if (qcount == 0) {
		free(qtracks);
		free(qpaths);
		free(qptrs);
		toast_error("webdav_empty");
		return;
	}

	record_queued_tracks(qtracks, qpaths, qcount);
	streamturn_abandon_all();

	const webdav_entry_t *tapped = &shown[index];
	if (webdavcache_cached(tapped, NULL, 0)) {
		waiting_for_track = false;
		refresh_track_loading_badge();
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
	job_t job = {
		.kind = JOB_PLAY,
		.play_track = *tapped,
		.start_queue_after_play = false,
	};
	if (submit_job(&job, NULL, NULL)) {
		refresh_track_loading_badge();
	} else {
		waiting_for_track = false;
		refresh_track_loading_badge();
	}
}

static void start_ram_stream_playback(int index) {
	if (waiting_for_track) {
		invalidate_job();
	}
	char (*paths)[96] = calloc(WEBDAV_PAGE_SIZE, sizeof(*paths));
	const char **pointers = calloc(WEBDAV_PAGE_SIZE, sizeof(*pointers));
	int count = 0, selected = 0;
	char error[192] = "";
	bool ok = paths && pointers &&
			  webdavstream_queue(shown, shown_count, index, paths, pointers, &count, &selected, error, sizeof(error));
	if (ok) {
		streamturn_abandon_all();
		device_state_play_list(pointers, count, selected);
		player_refresh_now_playing();
		player_sheet_open(true);
	} else {
		toast_error(error[0] ? error : "webdav_out_of_memory");
	}
	free(pointers);
	free(paths);
}

static void row_cb(lv_event_t *e) {
	if ((busy && !waiting_for_track) || switcher_back_drag_active() || player_sheet_drag_active()) {
		return;
	}
	int index = (int)(intptr_t)lv_event_get_user_data(e);
	if (index < 0 || index >= shown_count) {
		return;
	}
	const webdav_entry_t *item = &shown[index];
	if (item->kind == WEBDAV_DIR) {
		job_t job = {.kind = JOB_BROWSE, .offset = 0};
		snprintf(job.rel_path, sizeof(job.rel_path), "%s", item->path);
		snprintf(job.title, sizeof(job.title), "%s", item->title);
		run_list(job, true);
		return;
	}

	if (webdav_sd_cache()) {
		start_sd_cached_playback(index);
	} else {
		start_ram_stream_playback(index);
	}
}

static void format_track_caption(const webdav_entry_t *item, char *out, size_t size) {
	char upper[16] = "";
	for (size_t i = 0; item->suffix[i] && i + 1 < sizeof(upper); i++) {
		char c = item->suffix[i];
		upper[i] = (c >= 'a' && c <= 'z') ? (char)(c - 'a' + 'A') : c;
		upper[i + 1] = '\0';
	}
	if (item->size >= 1024 * 1024) {
		snprintf(out, size, "%s \xC2\xB7 %.1f MB", upper, (double)item->size / (1024.0 * 1024.0));
	} else if (item->size > 0) {
		snprintf(out, size, "%s \xC2\xB7 %llu KB", upper, (unsigned long long)(item->size / 1024));
	} else {
		snprintf(out, size, "%s", upper);
	}
}

static void rebuild_list(void) {
	lv_obj_clean(list_container);
	lv_obj_scroll_to_y(list_container, 0, LV_ANIM_OFF);
	if (current_list.offset > 0) {
		settingsrow_action(list_container, "webdav_previous_page", page_cb, (void *)(intptr_t)-1);
	}
	if (!shown_count) {
		lv_obj_t *empty = lv_label_create(list_container);
		lv_obj_add_style(empty, &theme_style_text_dim, 0);
		lv_obj_set_width(empty, lv_pct(100));
		lv_label_set_long_mode(empty, LV_LABEL_LONG_WRAP);
		lv_label_set_text(empty, tr("webdav_empty"));
	}
	for (int i = 0; i < shown_count; i++) {
		const webdav_entry_t *item = &shown[i];
		lv_obj_t *row = (item->kind == WEBDAV_DIR)
							? settingsrow_add(list_container, "", NULL, row_cb, (void *)(intptr_t)i)
							: settingsrow_action(list_container, "", row_cb, (void *)(intptr_t)i);
		lv_label_set_text(settingsrow_name_label(row), item->title);
		settingsrow_name_lines(row, 2);

		if (item->kind == WEBDAV_TRACK && webdavdlpage_present(item)) {
			settingsrow_add_accent_icon(row, &icon_check);
		} else {
			const lv_image_dsc_t *icon = (item->kind == WEBDAV_DIR) ? &icon_folder : &icon_music_note;
			settingsrow_add_icon(row, icon, THEME_SEMANTIC_CYAN);
		}
		if (item->kind == WEBDAV_TRACK) {
			char caption[64];
			format_track_caption(item, caption, sizeof(caption));
			if (caption[0]) {
				settingsrow_add_caption(row, caption);
			}
		}
		lv_obj_add_event_cb(row, row_long_pressed_cb, LV_EVENT_LONG_PRESSED, (void *)(intptr_t)i);
	}
	if (shown_more) {
		settingsrow_action(list_container, "webdav_next_page", page_cb, (void *)(intptr_t)1);
	}
}

static void poll_cb(lv_timer_t *timer) {
	(void)timer;
	webdavdlpage_watch();

	pthread_mutex_lock(&job_lock);
	result_t *res = mail;
	mail = NULL;
	unsigned latest = serial;
	unsigned failed = failed_serial;
	failed_serial = 0;
	pthread_mutex_unlock(&job_lock);

	if (failed && failed == latest) {
		busy = false;
		waiting_for_track = false;
		toast_busy_end();
		refresh_track_loading_badge();
		toast_error("webdav_out_of_memory");
	}

	if (res) {
		if (res->job.serial == latest) {
			busy = false;
			toast_busy_end();
			if (res->count < 0) {
				waiting_for_track = false;
				refresh_track_loading_badge();
				if (strcmp(res->error, "cancelled") != 0) {
					toast_error(res->error[0] ? res->error : "webdav_connection_failed");
				}
			} else if (res->job.kind == JOB_BROWSE) {
				free(shown);
				shown = res->entries;
				res->entries = NULL;
				shown_count = res->count;
				shown_more = res->more;
				current_list = res->job;
				rebuild_list();
			} else if (res->job.kind == JOB_CONNECT || res->job.kind == JOB_PING) {
				toast_success("webdav_connected");
				if (res->job.kind == JOB_CONNECT && lv_screen_active() == settings_screen) {
					wipe_bytes(draft.password, sizeof(draft.password));
					switch_screen_return_to(webdav_screen);
				}
			} else if (res->job.kind == JOB_PLAY) {
				waiting_for_track = false;
				refresh_track_loading_badge();
				refresh_cache_usage_label();
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

	char stream_err[192];
	if (webdavstream_take_error(stream_err, sizeof(stream_err))) {
		toast_error(stream_err);
	}

	if (audio_get_status() == AUDIO_STATUS_PLAYING && !waiting_for_track) {
		char current[512], next[512];
		audio_get_current_file(current, sizeof(current));
		if (webdavstream_owns(current)) {
			double pos = 0, dur = 0;
			audio_get_progress(&pos, &dur);
			if (dur > 0 && pos >= dur * 0.90) {
				if (playlist_peek_auto(next, sizeof(next)) && webdavstream_owns(next)) {
					webdavstream_prefetch(next);
				} else {
					webdavstream_prefetch(NULL);
				}
			}
		} else if (webdavcache_owns(current)) {
			double pos = 0, dur = 0;
			audio_get_progress(&pos, &dur);
			if (dur > 0 && pos >= dur * 0.80 &&
				playlist_peek_auto(next, sizeof(next)) && webdavcache_is_placeholder(next)) {
				request_sd_prefetch(next);
			}
		}
	}
}

// ---------------------------------------------------------------------------
// Screen builders & initialization
// ---------------------------------------------------------------------------

static lv_obj_t *make_screen(void) {
	lv_obj_t *obj = lv_obj_create(NULL);
	lv_obj_add_style(obj, &theme_style_screen, 0);
	lv_obj_add_event_cb(obj, page_left_cb, LV_EVENT_SCREEN_UNLOADED, NULL);
	switcher_attach_back_gesture(obj);
	return obj;
}

static void settings_loaded_cb(lv_event_t *e) {
	(void)e;
	settings_refresh();
	fit_all_value_row_titles();
}

static void build_track_load_badge(gui_config_t *cfg) {
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

static void build_settings(gui_config_t *cfg) {
	settings_screen = make_screen();
	lv_obj_t *c = settingsrow_page(settings_screen, cfg, "webdav_server_settings");
	settingsrow_title_corner_slots(settingsrow_page_title(settings_screen), cfg, 0);

	add_stacked_value_row(c, "webdav_server_url", &server_value, &icon_wifi_high, THEME_SEMANTIC_BLUE, true,
						  edit_cb, (void *)(intptr_t)0);
	add_stacked_value_row(c, "webdav_username", &user_value, &icon_artist, THEME_SEMANTIC_CYAN, true,
						  edit_cb, (void *)(intptr_t)1);
	add_stacked_value_row(c, "password", &pass_value, &icon_keylock_quick, THEME_SEMANTIC_PURPLE, true,
						  edit_cb, (void *)(intptr_t)2);

	int toggle_min_h = cfg->screen_width < 320 ? 55 : 84;
	lv_obj_t *rem_card = settingsrow_toggle(c, "webdav_remember", &remember_switch, remember_cb);
	lv_obj_set_height(rem_card, LV_SIZE_CONTENT);
	lv_obj_set_style_min_height(rem_card, toggle_min_h, 0);

	lv_obj_t *rel_card = settingsrow_toggle(c, "webdav_allow_self_signed", &relaxed_switch, certificates_cb);
	lv_obj_set_height(rel_card, LV_SIZE_CONTENT);
	lv_obj_set_style_min_height(rel_card, toggle_min_h, 0);
	add_note(c, "webdav_allow_self_signed_note");

	add_stacked_value_row(c, "webdav_connect_save", NULL, &icon_check, THEME_SEMANTIC_GREEN, false,
						  connect_cb, NULL);

	lv_obj_t *sdc_card = settingsrow_toggle(c, "webdav_sd_cache", &sd_cache_switch, sd_cache_toggle_cb);
	lv_obj_set_height(sdc_card, LV_SIZE_CONTENT);
	lv_obj_set_style_min_height(sdc_card, toggle_min_h, 0);
	add_note(c, "webdav_sd_cache_note");

	lv_obj_t *cache_pills = NULL;
	wrap_pills_card_heading(settingsrow_pills(c, "webdav_cache_limit", &cache_pills));
	cache_limit_pills[0] = settingsrow_pill_text(cache_pills, "256 MB", 256, cache_limit_pill_cb);
	cache_limit_pills[1] = settingsrow_pill_text(cache_pills, "512 MB", 512, cache_limit_pill_cb);
	cache_limit_pills[2] = settingsrow_pill_text(cache_pills, "1 GB", 1024, cache_limit_pill_cb);
	cache_limit_pills[3] = settingsrow_pill_text(cache_pills, "2 GB", 2048, cache_limit_pill_cb);
	cache_limit_pills[4] = settingsrow_pill_text(cache_pills, "4 GB", 4096, cache_limit_pill_cb);
	cache_limit_pills[5] = settingsrow_pill(cache_pills, "webdav_cache_unlimited", 0, cache_limit_pill_cb);

	add_stacked_value_row(c, "webdav_clear_cache", &cache_value, &icon_trash, THEME_SEMANTIC_AMBER, false,
						  clear_cache_cb, NULL);

	add_stacked_value_row(c, "webdav_forget_server", NULL, &icon_log_out, THEME_SEMANTIC_RED, false,
						  forget_cb, NULL);

	add_note(c, "webdav_settings_note");
	player_sheet_attach_drag(c, true);
	lv_obj_add_event_cb(settings_screen, settings_loaded_cb, LV_EVENT_SCREEN_LOADED, NULL);

	edit_screen = make_screen();
	edit_title = settingsrow_title(edit_screen, cfg, "webdav_server_url");
	settingsrow_title_corner_slots(edit_title, cfg, 0);
	edit_field = lv_textarea_create(edit_screen);
	lv_textarea_set_one_line(edit_field, true);
	lv_obj_add_style(edit_field, &theme_style_card, 0);
	lv_obj_set_style_text_font(edit_field, &font_ui_24, 0);
	lv_obj_set_style_pad_all(edit_field, 8, 0);
	lv_obj_set_size(edit_field, cfg->screen_width - 2 * cfg->padding, cfg->screen_width < 320 ? 44 : 64);
	lv_obj_align(edit_field, LV_ALIGN_TOP_LEFT, cfg->padding, settingsrow_content_top(cfg));
	keyboard_style_caret(edit_field);
	keyboard = keyboard_create(edit_screen, cfg->screen_width, cfg->screen_width < 320 ? 144 : 316,
							   edit_field, NULL, "ok", accept_edit_cb, NULL);
	lv_obj_add_event_cb(edit_screen, edit_left_cb, LV_EVENT_SCREEN_UNLOADED, NULL);

	webdav_credentials(&draft, &draft_remember);
	settings_refresh();
}

static void home_folders_action(void) {
	if ((busy && !waiting_for_track) || switcher_back_drag_active()) {
		return;
	}
	history_count = 0;
	job_t job = {.kind = JOB_BROWSE, .offset = 0};
	snprintf(job.title, sizeof(job.title), "%s", tr("webdav_folders"));
	run_list(job, false);
}

static void home_downloads_action(void) {
	if (switcher_back_drag_active()) {
		return;
	}
	webdavdlpage_open();
}

static void home_test_action(void) {
	ping_cb(NULL);
}

static void home_settings_action(void) {
	open_settings_cb(NULL);
}

static void corner_downloads_cb(lv_event_t *e) {
	(void)e;
	if (!switcher_back_drag_active()) {
		webdavdlpage_open();
	}
}

void webdavpage_open_settings_for_test(void) {
	webdav_credentials(&draft, &draft_remember);
	settings_refresh();
	switch_screen(settings_screen);
}

void webdavpage_open_edit_for_test(int field) {
	open_edit_field(field);
}

void webdavpage_init(gui_config_t *cfg) {
	config = cfg;
	value_row_count = 0;
	webdav_init();
	device_state_add_prepare_cb(webdavpage_prepare_track);
	device_state_add_prepare_cb(webdavstream_prepare);
	streamturn_register_abandon(webdavstream_cancel);
	streamturn_register_abandon(webdavcache_cancel);

	webdav_screen = make_screen();
	settingsrow_title(webdav_screen, cfg, "webdav");

	const grid_entry_t home_entries[WEBDAV_HOME_TILES] = {
		{"webdav_folders", &icon_menu_sub_folders, NULL, home_folders_action},
		{"webdav_downloads", &icon_menu_sc_playlists, NULL, home_downloads_action},
		{"webdav_test_connection", &icon_menu_sub_test, NULL, home_test_action},
		{"webdav_server_settings", &icon_menu_sub_server, NULL, home_settings_action},
	};
	home_grid = gridpage_build(webdav_screen, cfg, home_entries, WEBDAV_HOME_TILES, 2, 2, true);
	for (int i = 0; i < WEBDAV_HOME_TILES; i++) {
		gridpage_set_tile_label_lines(home_grid, i, 2);
	}
	home_style_tiles();
	player_sheet_attach_drag(webdav_screen, true);

	lv_obj_t *downloads_btn = lv_btn_create(webdav_screen);
	settingsrow_place_corner_button(downloads_btn, cfg, 0);
	lv_obj_set_style_bg_opa(downloads_btn, LV_OPA_TRANSP, 0);
	lv_obj_set_style_border_width(downloads_btn, 0, 0);
	lv_obj_set_style_shadow_width(downloads_btn, 0, 0);
	lv_obj_set_style_pad_all(downloads_btn, 0, 0);
	lv_obj_t *downloads_icon = lv_image_create(downloads_btn);
	lv_image_set_src(downloads_icon, &icon_download);
	lv_obj_add_style(downloads_icon, &theme_style_icon, 0);
	settingsrow_scale_corner_icon(downloads_icon, cfg);
	lv_obj_center(downloads_icon);
	lv_obj_add_event_cb(downloads_btn, corner_downloads_cb, LV_EVENT_CLICKED, NULL);
	settingsrow_title_corner_slots(settingsrow_page_title(webdav_screen), cfg, 1);

	webdavdlpage_init(cfg);

	list_screen = make_screen();
	list_container = settingsrow_page(list_screen, cfg, "webdav_folders");
	list_title = settingsrow_page_title(list_screen);
	player_sheet_attach_drag(list_container, true);
	switcher_set_back_guard(list_screen, list_back);
	switcher_set_back_guard_peek(list_screen, list_back_has_step);

	build_track_load_badge(cfg);
	build_settings(cfg);

	refresh_webdav_theme();
	theme_register_refresh(refresh_webdav_theme);
	fonts_register_change(fit_all_value_row_titles);

	lv_timer_create(poll_cb, 500, NULL);
}
