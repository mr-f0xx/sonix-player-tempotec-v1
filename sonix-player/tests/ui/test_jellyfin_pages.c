/*
 * Headless LVGL integration test for Jellyfin pages (`jellyfinpage.c` +
 * `jellyfin_icon.c`) at V1 dimensions (240x320) across all 11 shipped
 * languages and all 5 built-in themes.
 */
#define _GNU_SOURCE
#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <ftw.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "src/gui/board_profile.h"
#include "src/gui/fonts/fonts.h"
#include "src/gui/nowplaying/player.h"
#include "src/gui/shell/confirm.h"
#include "src/gui/shell/gridpage.h"
#include "src/gui/shell/gui.h"
#include "src/gui/shell/icons.h"
#include "src/gui/shell/keyboard.h"
#include "src/gui/shell/popover.h"
#include "src/gui/shell/settingsrow.h"
#include "src/gui/shell/switcher.h"
#include "src/gui/shell/theme.h"
#include "src/gui/shell/toast.h"
#include "src/gui/streaming/jellyfin_icon.h"
#include "src/gui/streaming/jellyfinpage.h"
#include "src/system/audio/audio.h"
#include "src/system/core/config.h"
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

/* ---- fonts -------------------------------------------------------------- */

lv_font_t font_ui_14, font_ui_16, font_ui_18, font_ui_20, font_ui_20_bold;
lv_font_t font_ui_22, font_ui_22_bold, font_ui_24, font_ui_24_bold;
lv_font_t font_ui_26, font_ui_28, font_ui_32;
lv_font_t font_ui_36_bold, font_ui_64_bold, font_ui_72;

static void init_fonts(void) {
	font_ui_14 = lv_font_montserrat_14;
	font_ui_16 = lv_font_montserrat_16;
	font_ui_18 = lv_font_montserrat_16;
	font_ui_20 = lv_font_montserrat_20;
	font_ui_20_bold = lv_font_montserrat_20;
	font_ui_22 = lv_font_montserrat_20;
	font_ui_22_bold = lv_font_montserrat_20;
	font_ui_24 = lv_font_montserrat_20;
	font_ui_24_bold = lv_font_montserrat_20;
	font_ui_26 = lv_font_montserrat_22;
	font_ui_28 = lv_font_montserrat_22;
	font_ui_32 = lv_font_montserrat_22;
	font_ui_36_bold = lv_font_montserrat_28;
	font_ui_64_bold = lv_font_montserrat_36;
	font_ui_72 = lv_font_montserrat_36;
}

/* ---- shell / playback stubs --------------------------------------------- */

static char card_root_buf[512];
const char *storage_sd_root(void) { return card_root_buf; }

lv_obj_t *player_screen;

static lv_obj_t *screen_stack[16];
static int screen_depth;
static struct {
	lv_obj_t *screen;
	bool (*handler)(void);
} back_guards[4];
static int back_guard_count;

void switch_screen(lv_obj_t *target_screen) {
	if (!target_screen || target_screen == lv_screen_active())
		return;
	assert(screen_depth < 16);
	screen_stack[screen_depth++] = lv_screen_active();
	lv_screen_load(target_screen);
	lv_obj_update_layout(target_screen);
}

void switch_screen_return_to(lv_obj_t *target_screen) {
	while (screen_depth > 0 && screen_stack[screen_depth - 1] != target_screen)
		screen_depth--;
	if (screen_depth > 0 && screen_stack[screen_depth - 1] == target_screen)
		screen_depth--;
	lv_screen_load(target_screen);
	lv_obj_update_layout(target_screen);
}

void switch_screen_cb(lv_event_t *e) {
	if (lv_event_get_code(e) != LV_EVENT_CLICKED)
		return;
	lv_obj_t *target = lv_event_get_user_data(e);
	switch_screen(target);
}

void back_btn_cb(lv_event_t *e) {
	if (lv_event_get_code(e) != LV_EVENT_CLICKED)
		return;
	for (int i = 0; i < back_guard_count; i++) {
		if (back_guards[i].screen == lv_screen_active() && back_guards[i].handler())
			return;
	}
	if (screen_depth > 0) {
		lv_screen_load(screen_stack[--screen_depth]);
		lv_obj_update_layout(lv_screen_active());
	}
}

void switcher_set_back_guard(lv_obj_t *screen, bool (*handler)(void)) {
	for (int i = 0; i < back_guard_count; i++) {
		if (back_guards[i].screen == screen) {
			back_guards[i].handler = handler;
			return;
		}
	}
	assert(back_guard_count < 4);
	back_guards[back_guard_count].screen = screen;
	back_guards[back_guard_count].handler = handler;
	back_guard_count++;
}

static struct {
	bool open;
	void (*on_ok)(void *);
	void *user;
} confirm_state;

void confirm_show(const char *title, const char *message, const char *ok_label, void (*on_ok)(void *user), void *user) {
	(void)title;
	(void)message;
	(void)ok_label;
	confirm_state.open = true;
	confirm_state.on_ok = on_ok;
	confirm_state.user = user;
}

typedef struct {
	void (*fn)(void *);
	void *arg;
} async_item_t;
static pthread_mutex_t async_mu = PTHREAD_MUTEX_INITIALIZER;
static async_item_t async_q[64];
static int async_head, async_tail;

bool gui_post(void (*cb)(void *), void *user) {
	pthread_mutex_lock(&async_mu);
	int next = (async_tail + 1) % 64;
	assert(next != async_head);
	async_q[async_tail].fn = cb;
	async_q[async_tail].arg = user;
	async_tail = next;
	pthread_mutex_unlock(&async_mu);
	return true;
}

static int drain_async(void) {
	int ran = 0;
	for (;;) {
		pthread_mutex_lock(&async_mu);
		if (async_head == async_tail) {
			pthread_mutex_unlock(&async_mu);
			break;
		}
		async_item_t it = async_q[async_head];
		async_head = (async_head + 1) % 64;
		pthread_mutex_unlock(&async_mu);
		if (it.fn)
			it.fn(it.arg);
		ran++;
	}
	return ran;
}

static int busy_toasts;
void toast_busy(const char *key) {
	(void)key;
	busy_toasts++;
}
void toast_busy_end(void) {
	if (busy_toasts > 0)
		busy_toasts--;
}

static int queued_count;
static char queued_paths[16][576];
static int played_queue_idx = -1;
static char last_started_path[576];
static device_state_prepare_cb registered_prep_cb;

void device_state_add_prepare_cb(device_state_prepare_cb cb) { registered_prep_cb = cb; }

void playlist_load_paths(const char *const *list, int count, int start_index) {
	queued_count = count < 16 ? count : 16;
	for (int i = 0; i < queued_count; i++)
		snprintf(queued_paths[i], sizeof(queued_paths[i]), "%s", list[i]);
	played_queue_idx = start_index;
}
int playlist_count(void) { return queued_count; }
int playlist_current_index(void) { return played_queue_idx; }
bool playlist_path_at(int index, char *out, size_t out_size) {
	if (index < 0 || index >= queued_count || !out || out_size == 0)
		return false;
	snprintf(out, out_size, "%s", queued_paths[index]);
	return true;
}
bool playlist_peek_auto(char *out, size_t size) {
	(void)out;
	(void)size;
	return false;
}
bool device_state_play_queue_index(int index) {
	played_queue_idx = index;
	if (index >= 0 && index < queued_count) {
		char buf[576];
		snprintf(buf, sizeof(buf), "%s", queued_paths[index]);
		if (registered_prep_cb)
			registered_prep_cb(buf, sizeof(buf));
		snprintf(last_started_path, sizeof(last_started_path), "%s", buf);
	}
	return true;
}
void device_state_play_list(const char *const *list, int count, int start_index) {
	playlist_load_paths(list, count, start_index);
	device_state_play_queue_index(start_index);
}
void device_state_play_file(const char *path) {
	if (path)
		snprintf(last_started_path, sizeof(last_started_path), "%s", path);
}

static tls_verify_t stub_tls_mode = TLS_VERIFY_STRICT;
tls_verify_t tls_verify_mode(void) { return stub_tls_mode; }
void tls_set_verify_mode(tls_verify_t mode) { stub_tls_mode = mode; }

bool streamkeep_is_stream_track(const char *path) { return path && strstr(path, "jellyfin-cache/") != NULL; }
bool streamkeep_keep(const char *path, bool notify_result) {
	(void)path;
	(void)notify_result;
	return true;
}
bool streamkeep_busy(void) { return false; }
bool streamkeep_last_error(char *out, size_t out_size) {
	if (out && out_size)
		out[0] = '\0';
	return false;
}

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
void fonts_register_change(void (*cb)(void)) {}
void bootlogo_set(int choice, bool dark) {}
bool bootpic_present(void) { return false; }
void gui_notify_popup(const char *text) {}
void toast_plain(const char *text) {}
void toast_success(const char *text) {}
void toast_error(const char *text) {}
void toast_plain_text(const char *text) {}
void toast_success_text(const char *text) {}
void toast_error_text(const char *text) {}
void switcher_attach_back_gesture(lv_obj_t *screen) {}
bool switcher_back_drag_active(void) { return false; }
void switcher_set_back_guard_peek(lv_obj_t *screen, bool (*peek)(void)) {}
bool coverflow_drag_active(void) { return false; }
const lv_image_dsc_t icon_pet = {0};
void player_sheet_attach_drag(lv_obj_t *obj, bool opening) {}
bool player_sheet_drag_active(void) { return false; }
bool player_sheet_is_open(void) { return false; }
void player_sheet_open(bool animate) {}
void player_refresh_now_playing(void) {}
void device_state_stop(void) {}
void device_state_get(device_state_t *out) { memset(out, 0, sizeof(*out)); }
audio_status_t audio_get_status(void) { return AUDIO_STATUS_STOPPED; }
void audio_get_current_file(char *out, size_t size) {
	if (out && size)
		out[0] = '\0';
}
void audio_get_progress(double *pos, double *dur) {
	if (pos)
		*pos = 0;
	if (dur)
		*dur = 0;
}
void streamturn_abandon_all(void) {}
#pragma GCC diagnostic pop

/* ---- Jellyfin client + cache stubs -------------------------------------- */

static bool stub_logged_in;
static bool stub_remembered = true;
static bool stub_report = true;
static char stub_server[JELLYFIN_SERVER_MAX] = "http://192.168.1.50:8096";
static char stub_user[JELLYFIN_USER_MAX] = "alice";
static char stub_lib_id[JELLYFIN_ID_MAX] = "";
static char stub_lib_name[JELLYFIN_TITLE_MAX] = "";
static jellyfin_quality_t stub_quality = JELLYFIN_QUALITY_ORIGINAL;
static int stub_cache_max_mb = 512;
static int stub_cache_clears;
static jellyfin_browse_t stub_last_browse = JELLYFIN_BROWSE_ALBUMS;

void jellyfin_init(void) {}
bool jellyfin_logged_in(void) { return stub_logged_in; }
void jellyfin_account_info(jellyfin_account_info_t *out) {
	memset(out, 0, sizeof(*out));
	out->logged_in = stub_logged_in;
	out->remembered = stub_remembered;
	snprintf(out->server, sizeof(out->server), "%s", stub_server);
	snprintf(out->server_name, sizeof(out->server_name), "HomeFin");
	snprintf(out->username, sizeof(out->username), "%s", stub_user);
	snprintf(out->user_id, sizeof(out->user_id), "user-42");
	snprintf(out->library_id, sizeof(out->library_id), "%s", stub_lib_id);
	snprintf(out->library_name, sizeof(out->library_name), "%s", stub_lib_name);
}
bool jellyfin_normalize_server(const char *raw, char *out, size_t out_size, char *error, size_t error_size) {
	(void)error;
	(void)error_size;
	if (!raw || !raw[0])
		return false;
	snprintf(out, out_size, "%s", raw);
	return true;
}
bool jellyfin_authenticate(const jellyfin_credentials_t *creds, bool remember, char *error, size_t error_size) {
	(void)error;
	(void)error_size;
	snprintf(stub_server, sizeof(stub_server), "%s", creds->server);
	snprintf(stub_user, sizeof(stub_user), "%s", creds->username);
	stub_remembered = remember;
	stub_logged_in = true;
	return true;
}
bool jellyfin_set_remember(bool remember, char *error, size_t error_size) {
	(void)error;
	(void)error_size;
	stub_remembered = remember;
	return true;
}
jellyfin_qc_state_t jellyfin_qc_initiate(const char *server_url, jellyfin_qc_t *out, char *error, size_t error_size) {
	(void)error;
	(void)error_size;
	memset(out, 0, sizeof(*out));
	snprintf(out->server, sizeof(out->server), "%s", server_url);
	snprintf(out->code, sizeof(out->code), "482913");
	snprintf(out->secret, sizeof(out->secret), "qc-sec");
	snprintf(out->web_url, sizeof(out->web_url), "%s/web/#/quickconnect", server_url);
	return JELLYFIN_QC_PENDING;
}
jellyfin_qc_state_t jellyfin_qc_poll(const jellyfin_qc_t *qc, bool remember, char *error, size_t error_size) {
	(void)qc;
	(void)remember;
	(void)error;
	(void)error_size;
	return JELLYFIN_QC_PENDING;
}
bool jellyfin_set_library(const char *library_id, const char *library_name) {
	snprintf(stub_lib_id, sizeof(stub_lib_id), "%s", library_id ? library_id : "");
	snprintf(stub_lib_name, sizeof(stub_lib_name), "%s", library_name ? library_name : "");
	return true;
}
bool jellyfin_forget(void) {
	stub_logged_in = false;
	stub_lib_id[0] = '\0';
	stub_lib_name[0] = '\0';
	return true;
}
bool jellyfin_logout_and_forget(void) { return jellyfin_forget(); }
bool jellyfin_ping(char *error, size_t error_size) {
	(void)error;
	(void)error_size;
	return true;
}
jellyfin_quality_t jellyfin_get_quality(void) { return stub_quality; }
void jellyfin_set_quality(jellyfin_quality_t q) { stub_quality = q; }
bool jellyfin_report_playback_enabled(void) { return stub_report; }
void jellyfin_set_report_playback(bool enabled) { stub_report = enabled; }
bool jellyfin_api_busy(void) { return false; }
const char *jellyfin_last_error(void) { return ""; }

int jellyfin_browse(jellyfin_browse_t browse, const char *id_or_query, int offset, jellyfin_entry_t *entries, int limit,
					bool *more, char *error, size_t error_size) {
	(void)id_or_query;
	(void)offset;
	(void)error;
	(void)error_size;
	stub_last_browse = browse;
	if (more)
		*more = false;
	if (!entries || limit < 1)
		return 0;
	memset(&entries[0], 0, sizeof(entries[0]));
	switch (browse) {
	case JELLYFIN_BROWSE_LIBRARIES:
		entries[0].kind = JELLYFIN_ITEM_FOLDER;
		snprintf(entries[0].id, sizeof(entries[0].id), "lib-1");
		snprintf(entries[0].title, sizeof(entries[0].title), "Lossless Music");
		return 1;
	case JELLYFIN_BROWSE_ALBUMS:
	case JELLYFIN_BROWSE_RECENT:
	case JELLYFIN_BROWSE_ARTIST_ALBUMS:
	case JELLYFIN_BROWSE_GENRE_ALBUMS:
		entries[0].kind = JELLYFIN_ITEM_ALBUM;
		snprintf(entries[0].id, sizeof(entries[0].id), "alb-1");
		snprintf(entries[0].title, sizeof(entries[0].title), "Music Has the Right to Children");
		snprintf(entries[0].artist, sizeof(entries[0].artist), "Boards of Canada");
		entries[0].year = 1998;
		return 1;
	case JELLYFIN_BROWSE_PLAYLISTS:
		entries[0].kind = JELLYFIN_ITEM_PLAYLIST;
		snprintf(entries[0].id, sizeof(entries[0].id), "pl-1");
		snprintf(entries[0].title, sizeof(entries[0].title), "Late Night");
		return 1;
	case JELLYFIN_BROWSE_ARTISTS:
		entries[0].kind = JELLYFIN_ITEM_ARTIST;
		snprintf(entries[0].id, sizeof(entries[0].id), "art-1");
		snprintf(entries[0].title, sizeof(entries[0].title), "Boards of Canada");
		return 1;
	case JELLYFIN_BROWSE_GENRES:
		entries[0].kind = JELLYFIN_ITEM_GENRE;
		snprintf(entries[0].id, sizeof(entries[0].id), "gen-1");
		snprintf(entries[0].title, sizeof(entries[0].title), "Ambient");
		return 1;
	case JELLYFIN_BROWSE_FOLDERS:
	case JELLYFIN_BROWSE_FOLDER:
		entries[0].kind = JELLYFIN_ITEM_FOLDER;
		snprintf(entries[0].id, sizeof(entries[0].id), "fld-1");
		snprintf(entries[0].title, sizeof(entries[0].title), "Electronic");
		return 1;
	default:
		entries[0].kind = JELLYFIN_ITEM_TRACK;
		snprintf(entries[0].id, sizeof(entries[0].id), "trk-1");
		snprintf(entries[0].title, sizeof(entries[0].title), "Roygbiv");
		snprintf(entries[0].artist, sizeof(entries[0].artist), "Boards of Canada");
		snprintf(entries[0].album, sizeof(entries[0].album), "MHTRTC");
		snprintf(entries[0].container, sizeof(entries[0].container), "flac");
		entries[0].duration = 151;
		return 1;
	}
}

bool jellyfin_is_favorite(const char *id) {
	(void)id;
	return false;
}
void jellyfin_note_favorite(const char *id, bool fav) {
	(void)id;
	(void)fav;
}
bool jellyfin_set_favorite(const char *id, bool fav, char *error, size_t error_size) {
	(void)id;
	(void)fav;
	(void)error;
	(void)error_size;
	return true;
}
void jellyfin_report_start_async(const char *id, const char *sid, long long ticks, bool trc) {
	(void)id;
	(void)sid;
	(void)ticks;
	(void)trc;
}
void jellyfin_report_progress_async(const char *id, const char *sid, long long ticks, bool paused, bool trc) {
	(void)id;
	(void)sid;
	(void)ticks;
	(void)paused;
	(void)trc;
}
void jellyfin_report_stopped_async(const char *id, const char *sid, long long ticks) {
	(void)id;
	(void)sid;
	(void)ticks;
}

void jellyfincache_set_root(const char *root) { (void)root; }
bool jellyfincache_owns(const char *path) { return path && strstr(path, "jellyfin-cache/") != NULL; }
bool jellyfincache_item_id_of(const char *path, char *out, size_t out_size) {
	if (!jellyfincache_owns(path))
		return false;
	snprintf(out, out_size, "trk-1");
	return true;
}
bool jellyfincache_placeholder_path(const char *id, const char *container_hint, char *out, size_t out_size) {
	snprintf(out, out_size, "%s/.local/jellyfin-cache/%s.%s", card_root_buf, id,
			 (container_hint && container_hint[0]) ? container_hint : "flac");
	return true;
}
bool jellyfincache_find_playable(const char *id, char *out, size_t out_size) {
	(void)id;
	(void)out;
	(void)out_size;
	return false;
}
bool jellyfincache_get(const jellyfin_entry_t *track, bool wait_complete, char *out_path, size_t out_size, char *error,
					   size_t error_size) {
	(void)wait_complete;
	(void)error;
	(void)error_size;
	snprintf(out_path, out_size, "%s/.local/jellyfin-cache/%s.flac", card_root_buf, track->id);
	return true;
}
void jellyfincache_prefetch(const jellyfin_entry_t *track) { (void)track; }
void jellyfincache_cancel(void) {}
void jellyfincache_protect(const char *path) { (void)path; }
bool jellyfincache_downloading(void) { return false; }
bool jellyfincache_clear(void) {
	stub_cache_clears++;
	return true;
}
void jellyfincache_stats(int *track_count, uint64_t *bytes) {
	if (track_count)
		*track_count = 3;
	if (bytes)
		*bytes = 9u * 1024u * 1024u;
}
int jellyfincache_get_max_mb(void) { return stub_cache_max_mb; }
void jellyfincache_set_max_mb(int mb) { stub_cache_max_mb = mb; }

/* ---- LVGL helpers & layout assertions ----------------------------------- */

static void flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map) {
	(void)area;
	(void)px_map;
	lv_display_flush_ready(disp);
}

static void pump_until_idle(const char *what) {
	for (int i = 0; i < 400; i++) {
		int ran = drain_async();
		lv_tick_inc(50);
		lv_timer_handler();
		if (ran > 0) {
			lv_obj_update_layout(lv_screen_active());
			continue;
		}
		if (busy_toasts == 0 && !jellyfinpage_waiting_for_track() && i > 15 && drain_async() == 0) {
			lv_obj_update_layout(lv_screen_active());
			return;
		}
		usleep(5000);
	}
	fprintf(stderr, "%s: worker job never completed\n", what);
	assert(!"worker job timed out");
}

static lv_obj_t *find_clickable_with_label(lv_obj_t *root, const char *text) {
	if (!root || !text)
		return NULL;
	if (lv_obj_is_hidden(root))
		return NULL;
	if (lv_obj_check_type(root, &lv_label_class)) {
		const char *s = lv_label_get_text(root);
		if (s && strcmp(s, text) == 0) {
			lv_obj_t *p = root;
			while (p) {
				if (lv_obj_is_clickable(p))
					return p;
				p = lv_obj_get_parent(p);
			}
		}
	}
	uint32_t n = lv_obj_get_child_count(root);
	for (uint32_t i = 0; i < n; i++) {
		lv_obj_t *found = find_clickable_with_label(lv_obj_get_child(root, (int32_t)i), text);
		if (found)
			return found;
	}
	return NULL;
}

// The picture a tile currently draws: its first image child.
static const lv_image_dsc_t *tile_icon_source(lv_obj_t *tile) {
	if (!tile) {
		return NULL;
	}
	uint32_t n = lv_obj_get_child_count(tile);
	for (uint32_t i = 0; i < n; i++) {
		lv_obj_t *child = lv_obj_get_child(tile, (int32_t)i);
		if (lv_obj_check_type(child, &lv_image_class)) {
			return lv_image_get_src(child);
		}
	}
	return NULL;
}

static lv_obj_t *find_textarea(lv_obj_t *root) {
	if (!root || lv_obj_is_hidden(root))
		return NULL;
	if (lv_obj_check_type(root, &lv_textarea_class))
		return root;
	uint32_t n = lv_obj_get_child_count(root);
	for (uint32_t i = 0; i < n; i++) {
		lv_obj_t *found = find_textarea(lv_obj_get_child(root, (int32_t)i));
		if (found)
			return found;
	}
	return NULL;
}

static void assert_no_raw_jellyfin_keys(lv_obj_t *root, const char *lang_name) {
	if (!root || lv_obj_is_hidden(root))
		return;
	if (lv_obj_check_type(root, &lv_label_class)) {
		const char *s = lv_label_get_text(root);
		if (s && strncmp(s, "jellyfin_", 9) == 0) {
			fprintf(stderr, "FAIL raw translation key '%s' in %s\n", s, lang_name);
			abort();
		}
	}
	uint32_t n = lv_obj_get_child_count(root);
	for (uint32_t i = 0; i < n; i++)
		assert_no_raw_jellyfin_keys(lv_obj_get_child(root, (int32_t)i), lang_name);
}

static bool areas_overlap(const lv_area_t *a, const lv_area_t *b) {
	return a->x1 < b->x2 && b->x1 < a->x2 && a->y1 < b->y2 && b->y1 < a->y2;
}

static void assert_screen_no_overlaps(lv_obj_t *scr, const char *name, const char *lang_name, int tk) {
	lv_obj_update_layout(scr);
	uint32_t n = lv_obj_get_child_count(scr);
	for (uint32_t i = 0; i < n; i++) {
		lv_obj_t *a = lv_obj_get_child(scr, (int32_t)i);
		if (lv_obj_is_hidden(a) || lv_obj_is_floating(a))
			continue;
		lv_area_t ca;
		lv_obj_get_coords(a, &ca);
		if (ca.x2 <= ca.x1 || ca.y2 <= ca.y1)
			continue;
		for (uint32_t j = i + 1; j < n; j++) {
			lv_obj_t *b = lv_obj_get_child(scr, (int32_t)j);
			if (lv_obj_is_hidden(b) || lv_obj_is_floating(b))
				continue;
			lv_area_t cb;
			lv_obj_get_coords(b, &cb);
			if (cb.x2 <= cb.x1 || cb.y2 <= cb.y1)
				continue;
			if (areas_overlap(&ca, &cb)) {
				fprintf(stderr, "FAIL overlap on %s (%s, theme %d): child %u (%d,%d-%d,%d) vs %u (%d,%d-%d,%d)\n",
						name, lang_name, tk, i, ca.x1, ca.y1, ca.x2, ca.y2, j, cb.x1, cb.y1, cb.x2, cb.y2);
				abort();
			}
		}
	}
}

static int unlink_cb(const char *fpath, const struct stat *sb, int typeflag, struct FTW *ftwbuf) {
	(void)sb;
	(void)typeflag;
	(void)ftwbuf;
	return remove(fpath);
}

int main(int argc, char **argv) {
	assert(argc >= 2);
	const char *lang_dir = argv[1];

	char tmp[] = "/tmp/test_jellyfin_ui_XXXXXX";
	assert(mkdtemp(tmp) != NULL);
	snprintf(card_root_buf, sizeof(card_root_buf), "%s/card", tmp);
	assert(mkdir(card_root_buf, 0755) == 0);

	char cfg_path[512];
	snprintf(cfg_path, sizeof(cfg_path), "%s/sonix.cfg", tmp);
	config_init(cfg_path);
	lang_init(lang_dir);
	assert(lang_set("English"));

	lv_init();
	init_fonts();
	static uint8_t fb[240 * 320 * 4];
	lv_display_t *disp = lv_display_create(240, 320);
	lv_display_set_flush_cb(disp, flush_cb);
	lv_display_set_buffers(disp, fb, NULL, sizeof(fb), LV_DISPLAY_RENDER_MODE_FULL);
	theme_init();

	player_screen = lv_obj_create(NULL);

	gui_config_t cfg = {
		.screen_width = 240,
		.screen_height = 320,
	};

	/* 1. Verify icon descriptor dimensions */
	assert(icon_jellyfin_mark.header.w == 32 && icon_jellyfin_mark.header.h == 32);
	assert(icon_jellyfin_badge.header.w == 52 && icon_jellyfin_badge.header.h == 52);
	assert(icon_jellyfin_menu.header.w == 112 && icon_jellyfin_menu.header.h == 112);
	assert(icon_jellyfin_saved.header.w == 48 && icon_jellyfin_saved.header.h == 48);
	assert(icon_jellyfin_menu_card.header.w == 112 && icon_jellyfin_menu_card.header.h == 112);
	assert(icon_jellyfin_saved_card.header.w == 48 && icon_jellyfin_saved_card.header.h == 48);
	// The Dark/Light card art is its own bitmaps, not a recolour of the flat
	// ones Custom and the glyph themes keep.
	assert(icon_jellyfin_menu_card.data != icon_jellyfin_menu.data);
	assert(icon_jellyfin_saved_card.data != icon_jellyfin_saved.data);

	/* 2. Init Jellyfin page & load Home */
	jellyfinpage_init(&cfg);
	assert(jellyfin_screen != NULL);
	lv_screen_load(jellyfin_screen);
	lv_obj_update_layout(jellyfin_screen);
	assert_screen_no_overlaps(jellyfin_screen, "jellyfin_home_signed_out", "English", THEME_KIND_DARK);

	/* 3. Open Server settings -> edit URL -> Quick Connect */
	lv_obj_t *server_tile = find_clickable_with_label(jellyfin_screen, tr("jellyfin_server_settings"));
	assert(server_tile != NULL);
	lv_obj_send_event(server_tile, LV_EVENT_CLICKED, NULL);
	lv_obj_t *settings_scr = lv_screen_active();
	assert(settings_scr != jellyfin_screen);
	assert_screen_no_overlaps(settings_scr, "jellyfin_settings_signed_out", "English", THEME_KIND_DARK);

	/* Edit server URL via text editor */
	lv_obj_t *url_row = find_clickable_with_label(settings_scr, tr("jellyfin_server_url"));
	assert(url_row != NULL);
	lv_obj_send_event(url_row, LV_EVENT_CLICKED, NULL);
	lv_obj_t *ta = find_textarea(lv_screen_active());
	assert(ta != NULL);
	lv_textarea_set_text(ta, "http://192.168.1.50:8096");
	lv_obj_t *ok_btn = find_clickable_with_label(lv_screen_active(), "OK");
	assert(ok_btn != NULL);
	lv_obj_send_event(ok_btn, LV_EVENT_CLICKED, NULL);
	assert(lv_screen_active() == settings_scr);

	/* Start Quick Connect -> shows 6-digit code & QR */
	lv_obj_t *qc_row = find_clickable_with_label(settings_scr, tr("jellyfin_quick_connect"));
	assert(qc_row != NULL);
	lv_obj_send_event(qc_row, LV_EVENT_CLICKED, NULL);
	lv_obj_t *qc_scr = lv_screen_active();
	assert(qc_scr != settings_scr);
	pump_until_idle("qc start");
	assert_screen_no_overlaps(qc_scr, "jellyfin_qc", "English", THEME_KIND_DARK);

	/* Sign in via settings Connect & save */
	switch_screen_return_to(settings_scr);
	lv_obj_t *user_row = find_clickable_with_label(settings_scr, tr("jellyfin_username"));
	assert(user_row != NULL);
	lv_obj_send_event(user_row, LV_EVENT_CLICKED, NULL);
	ta = find_textarea(lv_screen_active());
	lv_textarea_set_text(ta, "alice");
	lv_obj_send_event(find_clickable_with_label(lv_screen_active(), "OK"), LV_EVENT_CLICKED, NULL);

	lv_obj_t *connect_row = find_clickable_with_label(settings_scr, tr("jellyfin_connect_save"));
	assert(connect_row != NULL);
	lv_obj_send_event(connect_row, LV_EVENT_CLICKED, NULL);
	pump_until_idle("login");
	assert(stub_logged_in);
	assert(lv_screen_active() == jellyfin_screen);

	/* 4. Settings pills (Quality + Cache limit + Clear cache) */
	lv_obj_send_event(server_tile, LV_EVENT_CLICKED, NULL);
	assert(lv_screen_active() == settings_scr);

	lv_obj_t *pill_mp3 = find_clickable_with_label(settings_scr, "MP3 320");
	assert(pill_mp3 != NULL);
	lv_obj_send_event(pill_mp3, LV_EVENT_CLICKED, NULL);
	assert(stub_quality == JELLYFIN_QUALITY_MP3_320);

	lv_obj_t *pill_2gb = find_clickable_with_label(settings_scr, "2 GB");
	assert(pill_2gb != NULL);
	lv_obj_send_event(pill_2gb, LV_EVENT_CLICKED, NULL);
	assert(stub_cache_max_mb == 2048);

	lv_obj_t *clear_row = find_clickable_with_label(settings_scr, tr("jellyfin_clear_cache"));
	assert(clear_row != NULL);
	lv_obj_send_event(clear_row, LV_EVENT_CLICKED, NULL);
	assert(confirm_state.open);
	confirm_state.open = false;
	confirm_state.on_ok(confirm_state.user);
	assert(stub_cache_clears == 1);

	/* 5. Signed-in Home -> Albums -> Album tracks -> play track */
	lv_screen_load(jellyfin_screen);
	lv_obj_update_layout(jellyfin_screen);
	lv_obj_t *albums_tile = find_clickable_with_label(jellyfin_screen, tr("jellyfin_albums"));
	assert(albums_tile != NULL);
	lv_obj_send_event(albums_tile, LV_EVENT_CLICKED, NULL);
	lv_obj_t *list_scr = lv_screen_active();
	pump_until_idle("albums");
	assert(stub_last_browse == JELLYFIN_BROWSE_ALBUMS);

	lv_obj_t *alb_row = find_clickable_with_label(list_scr, "Music Has the Right to Children");
	assert(alb_row != NULL);
	lv_obj_send_event(alb_row, LV_EVENT_CLICKED, NULL);
	pump_until_idle("album tracks");

	lv_obj_t *trk_row = find_clickable_with_label(list_scr, "Roygbiv");
	assert(trk_row != NULL);
	lv_obj_send_event(trk_row, LV_EVENT_CLICKED, NULL);
	pump_until_idle("play track");
	assert(strstr(last_started_path, "trk-1.flac") != NULL);

	/* 6. Sweep all 11 languages x 5 themes across Home, Settings, List */
	static const char *const LANGS[] = {
		"Deutsch", "English", "Español", "Français", "Italiano", "Japanese",
		"Nederlands", "Polski", "Português BR", "Russian", "Ukrainian",
	};
	// A Streaming-style grid carrying the "jellyfin" tile, so the theme's icon
	// choice is checked for the section grid as well as the home grid.
	lv_obj_t *section_scratch = lv_obj_create(NULL);
	const grid_entry_t stream_entries[] = {
		{"jellyfin", &icon_jellyfin_menu, NULL, NULL},
	};
	gridpage_build(section_scratch, &cfg, stream_entries, 1, 1, 1, true);
	// Resolve the tiles once, in English: the sweep changes the language, and
	// the tile captions keep the text they were built with.
	lv_obj_t *saved_tile = find_clickable_with_label(jellyfin_screen, tr("jellyfin_saved"));
	lv_obj_t *stream_tile = find_clickable_with_label(section_scratch, tr("jellyfin"));
	assert(saved_tile && stream_tile);
	for (size_t li = 0; li < sizeof(LANGS) / sizeof(LANGS[0]); li++) {
		assert(lang_set(LANGS[li]));
		for (int tk = 0; tk < THEME_KIND_COUNT; tk++) {
			theme_set_kind((theme_kind_t)tk);

			lv_screen_load(jellyfin_screen);
			lv_obj_update_layout(jellyfin_screen);
			assert_no_raw_jellyfin_keys(jellyfin_screen, LANGS[li]);
			assert_screen_no_overlaps(jellyfin_screen, "jellyfin_home", LANGS[li], tk);

			// Dark/Light draw the stacked-card artwork; the glyph themes the
			// monochrome marks; nothing else changes the tile's picture.
			const lv_image_dsc_t *saved_src = tile_icon_source(saved_tile);
			const lv_image_dsc_t *stream_src = tile_icon_source(stream_tile);
			assert(saved_src && stream_src);
			if (tk == THEME_KIND_DARK || tk == THEME_KIND_LIGHT) {
				assert(saved_src == &icon_jellyfin_saved_card);
				assert(stream_src == &icon_jellyfin_menu_card);
			} else if (theme_uses_function_glyphs()) {
				assert(saved_src == &icon_podcast_downloaded);
				assert(stream_src == &icon_jellyfin_mark);
			} else {
				assert(saved_src == &icon_jellyfin_saved);
				assert(stream_src == &icon_jellyfin_menu);
			}

			lv_screen_load(settings_scr);
			lv_obj_update_layout(settings_scr);
			assert_no_raw_jellyfin_keys(settings_scr, LANGS[li]);
			assert_screen_no_overlaps(settings_scr, "jellyfin_settings", LANGS[li], tk);

			lv_screen_load(list_scr);
			lv_obj_update_layout(list_scr);
			assert_no_raw_jellyfin_keys(list_scr, LANGS[li]);
			assert_screen_no_overlaps(list_scr, "jellyfin_list", LANGS[li], tk);
		}
	}
	assert(lang_set("English"));
	theme_set_kind(THEME_KIND_DARK);

	/* 6b. A Custom theme from the SD card is not Dark or Light, so it keeps the
	 * original flat Jellyfin icons; switching back to Dark restores the cards. */
	{
		char dir[600], path[700];
		snprintf(dir, sizeof(dir), "%s/Themes", card_root_buf);
		snprintf(path, sizeof(path), "%s/custom.ini", dir);
		assert(mkdir(dir, 0700) == 0);
		FILE *f = fopen(path, "w");
		assert(f);
		fputs("[theme]\nscreen_bg = #010203\nsurface = #101112\nsurface_pressed = #202122\n"
			"panel = #303132\ncover_bg = #404142\ntext_primary = #f0f1f2\n"
			"text_secondary = #a0a1a2\naccent = #123456\n", f);
		assert(fclose(f) == 0);
		theme_set_card_root(card_root_buf);
		theme_set_kind(THEME_KIND_CUSTOM);
		assert(theme_get_kind() == THEME_KIND_CUSTOM);
		assert(!theme_uses_function_glyphs());
		lv_screen_load(jellyfin_screen);
		lv_obj_update_layout(jellyfin_screen);
		assert(tile_icon_source(saved_tile) == &icon_jellyfin_saved);
		assert(tile_icon_source(stream_tile) == &icon_jellyfin_menu);
		theme_set_kind(THEME_KIND_DARK);
		assert(tile_icon_source(saved_tile) == &icon_jellyfin_saved_card);
		assert(tile_icon_source(stream_tile) == &icon_jellyfin_menu_card);
		theme_set_card_root(NULL);
		assert(unlink(path) == 0);
		assert(rmdir(dir) == 0);
	}

	/* 7. Forget server */
	lv_screen_load(settings_scr);
	lv_obj_update_layout(settings_scr);
	lv_obj_t *forget_row = find_clickable_with_label(settings_scr, tr("jellyfin_forget_server"));
	assert(forget_row != NULL);
	lv_obj_send_event(forget_row, LV_EVENT_CLICKED, NULL);
	assert(confirm_state.open);
	confirm_state.open = false;
	confirm_state.on_ok(confirm_state.user);
	pump_until_idle("forget");
	assert(!stub_logged_in);

	nftw(tmp, unlink_cb, 32, FTW_DEPTH | FTW_PHYS);
	printf("PASS jellyfin_pages (240x320, 11 languages x 5 themes zero-overlap, Quick Connect, browse, "
		   "settings & forget)\n");
	return 0;
}
