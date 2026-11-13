/*
 * Headless LVGL integration test for the WebDAV pages (`webdavpage.c`,
 * `webdavdlpage.c`) and the Streaming menu entry (`streaming.c`, `addons.c`,
 * `gridpage.c`) at V1 dimensions (240x320) across all 11 shipped languages
 * and all 5 built-in themes.
 */
#define _GNU_SOURCE
#include <assert.h>
#include <ftw.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
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
#include "src/gui/streaming/streaming.h"
#include "src/gui/streaming/webdavdlpage.h"
#include "src/gui/streaming/webdavpage.h"
#include "src/system/addons/addons.h"
#include "src/system/audio/audio.h"
#include "src/system/core/config.h"
#include "src/system/core/lang.h"
#include "src/system/device/system.h"
#include "src/system/net/http.h"
#include "src/system/net/tls.h"
#include "src/system/playback/device_state.h"
#include "src/system/playback/playlist.h"
#include "src/system/streaming/streamturn.h"
#include "src/system/streaming/webdav.h"
#include "src/system/streaming/webdavcache.h"
#include "src/system/streaming/webdavdl.h"
#include "src/system/streaming/webdavstream.h"

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

/* ---- shell / sibling streaming screen stubs ----------------------------- */

static char card_root_buf[512];
const char *storage_sd_root(void) { return card_root_buf; }
bool storage_sd_writable(void) { return true; }
bool storage_card_attached(void) { return true; }
bool usb_storage_active(void) { return false; }

lv_obj_t *player_screen;
lv_obj_t *radiopage_screen;
lv_obj_t *podcast_screen;
lv_obj_t *soundcloud_screen;
lv_obj_t *subsonic_screen;
lv_obj_t *tidal_screen;
lv_obj_t *qobuz_screen;

void podcastpage_init(gui_config_t *cfg) { (void)cfg; podcast_screen = lv_obj_create(NULL); }
void soundcloudpage_init(gui_config_t *cfg) { (void)cfg; soundcloud_screen = lv_obj_create(NULL); }
void subsonicpage_init(gui_config_t *cfg) { (void)cfg; subsonic_screen = lv_obj_create(NULL); }
void tidalpage_init(gui_config_t *cfg) { (void)cfg; tidal_screen = lv_obj_create(NULL); }
void qobuzpage_init(gui_config_t *cfg) { (void)cfg; qobuz_screen = lv_obj_create(NULL); }

static lv_obj_t *screen_stack[16];
static int screen_depth;
static struct {
	lv_obj_t *screen;
	bool (*handler)(void);
} back_guards[8];
static int back_guard_count;

void switch_screen(lv_obj_t *target_screen) {
	if (!target_screen || target_screen == lv_screen_active()) return;
	assert(screen_depth < 16);
	screen_stack[screen_depth++] = lv_screen_active();
	lv_screen_load(target_screen);
	lv_obj_update_layout(target_screen);
}

void switch_screen_return_to(lv_obj_t *target_screen) {
	while (screen_depth > 0 && screen_stack[screen_depth - 1] != target_screen) {
		screen_depth--;
	}
	if (screen_depth > 0 && screen_stack[screen_depth - 1] == target_screen) {
		screen_depth--;
	}
	lv_screen_load(target_screen);
	lv_obj_update_layout(target_screen);
}

void switch_screen_cb(lv_event_t *e) {
	if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
	lv_obj_t *target = lv_event_get_user_data(e);
	switch_screen(target);
}

void back_btn_cb(lv_event_t *e) {
	if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
	for (int i = 0; i < back_guard_count; i++) {
		if (back_guards[i].screen == lv_screen_active() && back_guards[i].handler()) {
			return;
		}
	}
	if (screen_depth > 0) {
		lv_screen_load(screen_stack[--screen_depth]);
		lv_obj_update_layout(lv_screen_active());
	}
}

void switcher_set_back_guard(lv_obj_t *screen, bool (*handler)(void)) {
	assert(back_guard_count < 8);
	back_guards[back_guard_count].screen = screen;
	back_guards[back_guard_count].handler = handler;
	back_guard_count++;
}

static char last_error_toast[256];
static char last_success_toast[256];
static int busy_toasts;

void toast_busy(const char *key) {
	(void)key;
	busy_toasts++;
}
void toast_busy_end(void) {
	if (busy_toasts > 0) busy_toasts--;
}
void toast_error(const char *text) {
	snprintf(last_error_toast, sizeof(last_error_toast), "%s", text ? tr(text) : "");
}
void toast_success(const char *text) {
	snprintf(last_success_toast, sizeof(last_success_toast), "%s", text ? tr(text) : "");
}
void toast_plain(const char *text) { (void)text; }

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

static int queued_count;
static char queued_paths[8][576];
static int queued_start_idx = -1;
static device_state_prepare_cb prep_cbs[8];
static int prep_cb_count;

void device_state_add_prepare_cb(device_state_prepare_cb cb) {
	assert(prep_cb_count < 8);
	prep_cbs[prep_cb_count++] = cb;
}

void playlist_load_paths(const char *const *list, int count, int start_index) {
	queued_count = count < 8 ? count : 8;
	for (int i = 0; i < queued_count; i++) {
		snprintf(queued_paths[i], sizeof(queued_paths[i]), "%s", list[i]);
	}
	queued_start_idx = start_index;
}

int playlist_count(void) { return queued_count; }
int playlist_current_index(void) { return queued_start_idx; }
bool playlist_path_at(int index, char *out, size_t out_size) {
	if (index < 0 || index >= queued_count || !out || out_size == 0) return false;
	snprintf(out, out_size, "%s", queued_paths[index]);
	return true;
}
bool playlist_peek_auto(char *out, size_t size) {
	(void)out; (void)size;
	return false;
}
bool playlist_is_playable_file(const char *name) {
	const char *dot = name ? strrchr(name, '.') : NULL;
	return dot && (strcasecmp(dot, ".mp3") == 0 || strcasecmp(dot, ".flac") == 0 || strcasecmp(dot, ".m4a") == 0);
}
bool device_state_play_queue_index(int index) {
	queued_start_idx = index;
	return true;
}
void device_state_play_list(const char *const *list, int count, int start_index) {
	playlist_load_paths(list, count, start_index);
}
void device_state_play_file(const char *path) { (void)path; }
void device_state_play_list_ordered(const char *const *list, int count, int start_index) {
	playlist_load_paths(list, count, start_index);
}

typedef struct {
	void (*cb)(void *);
	void *user;
} posted_cb_t;
static pthread_mutex_t post_lock = PTHREAD_MUTEX_INITIALIZER;
static posted_cb_t post_queue[32];
static int post_count;

bool gui_post(void (*cb)(void *), void *user) {
	pthread_mutex_lock(&post_lock);
	if (cb && post_count < 32) {
		post_queue[post_count++] = (posted_cb_t){cb, user};
	}
	pthread_mutex_unlock(&post_lock);
	return true;
}

static void drain_posts(void) {
	for (;;) {
		pthread_mutex_lock(&post_lock);
		if (post_count == 0) {
			pthread_mutex_unlock(&post_lock);
			break;
		}
		posted_cb_t item = post_queue[0];
		memmove(&post_queue[0], &post_queue[1], (size_t)(--post_count) * sizeof(posted_cb_t));
		pthread_mutex_unlock(&post_lock);
		item.cb(item.user);
	}
}

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
void fonts_register_change(void (*cb)(void)) {}
void bootlogo_set(int choice, bool dark) {}
bool bootpic_present(void) { return false; }
void gui_notify_popup(const char *text) {}
void switcher_attach_back_gesture(lv_obj_t *screen) {}
bool switcher_back_drag_active(void) { return false; }
void switcher_set_back_guard_peek(lv_obj_t *screen, bool (*peek)(void)) {}
bool coverflow_drag_active(void) { return false; }
const lv_image_dsc_t icon_pet = {0};
static int sheet_open_count;
void player_sheet_attach_drag(lv_obj_t *obj, bool opening) {}
bool player_sheet_drag_active(void) { return false; }
bool player_sheet_is_open(void) { return false; }
void player_sheet_open(bool animate) { sheet_open_count++; }
void player_refresh_now_playing(void) {}
void device_state_stop(void) {}
void device_state_get(device_state_t *out) { memset(out, 0, sizeof(*out)); }
audio_status_t audio_get_status(void) { return AUDIO_STATUS_STOPPED; }
void audio_get_current_file(char *out, size_t size) {
	if (out && size) out[0] = '\0';
}
void audio_get_progress(double *pos, double *dur) {
	if (pos) *pos = 0;
	if (dur) *dur = 0;
}
void audio_stop(void) {}
void streamturn_abandon_all(void) {}
void streamturn_register_abandon(void (*cb)(void)) {}
int library_forget_file(const char *filepath) { return 0; }
#pragma GCC diagnostic pop

/* ---- deterministic WebDAV backend stubs for UI testing ------------------ */

static webdav_credentials_t stub_creds;
static bool stub_configured;
static bool stub_remembered = true;
static bool stub_sd_cache = false;
static int stub_cache_max_mb = 512;
static tls_verify_t stub_verify = TLS_VERIFY_STRICT;

void webdav_init(void) {}
bool webdav_configure(const webdav_credentials_t *credentials, bool remember, char *error, size_t error_size) {
	(void)error; (void)error_size;
	if (!credentials || !credentials->server[0]) return false;
	stub_creds = *credentials;
	stub_remembered = remember;
	stub_configured = true;
	return true;
}
void webdav_credentials(webdav_credentials_t *out, bool *remembered) {
	if (out) *out = stub_creds;
	if (remembered) *remembered = stub_remembered;
}
bool webdav_configured(void) { return stub_configured; }
bool webdav_forget(void) {
	memset(&stub_creds, 0, sizeof(stub_creds));
	stub_configured = false;
	return true;
}
bool webdav_api_busy(void) { return false; }
bool webdav_sd_cache(void) { return stub_sd_cache; }
void webdav_set_sd_cache(bool enabled) { stub_sd_cache = enabled; }
int webdav_cache_max_mb(void) { return stub_cache_max_mb; }
void webdav_set_cache_max_mb(int mb) { stub_cache_max_mb = mb; }
bool webdav_ping(char *error, size_t error_size) {
	(void)error; (void)error_size;
	return stub_configured;
}
int webdav_browse(const char *rel_path, int offset, webdav_entry_t *entries, int limit, bool *more,
				  char *error, size_t error_size) {
	(void)offset; (void)limit; (void)error; (void)error_size;
	if (more) *more = false;
	if (!stub_configured) return -1;
	if (!rel_path || rel_path[0] == '\0') {
		memset(&entries[0], 0, sizeof(entries[0]));
		entries[0].kind = WEBDAV_DIR;
		snprintf(entries[0].path, sizeof(entries[0].path), "Jazz");
		snprintf(entries[0].title, sizeof(entries[0].title), "Jazz");

		memset(&entries[1], 0, sizeof(entries[1]));
		entries[1].kind = WEBDAV_TRACK;
		snprintf(entries[1].path, sizeof(entries[1].path), "01 - So What.flac");
		snprintf(entries[1].title, sizeof(entries[1].title), "01 - So What.flac");
		snprintf(entries[1].suffix, sizeof(entries[1].suffix), "flac");
		entries[1].size = 28 * 1024 * 1024;

		memset(&entries[2], 0, sizeof(entries[2]));
		entries[2].kind = WEBDAV_TRACK;
		snprintf(entries[2].path, sizeof(entries[2].path), "02 - Blue.m4a");
		snprintf(entries[2].title, sizeof(entries[2].title), "02 - Blue.m4a");
		snprintf(entries[2].suffix, sizeof(entries[2].suffix), "m4a");
		entries[2].size = 8 * 1024 * 1024;
		return 3;
	}
	if (strcmp(rel_path, "Jazz") == 0) {
		memset(&entries[0], 0, sizeof(entries[0]));
		entries[0].kind = WEBDAV_TRACK;
		snprintf(entries[0].path, sizeof(entries[0].path), "Jazz/Take Five.mp3");
		snprintf(entries[0].title, sizeof(entries[0].title), "Take Five.mp3");
		snprintf(entries[0].album, sizeof(entries[0].album), "Jazz");
		snprintf(entries[0].suffix, sizeof(entries[0].suffix), "mp3");
		entries[0].size = 9 * 1024 * 1024;
		return 1;
	}
	return 0;
}
bool webdav_file_request_info(const char *rel_path, char *url_out, size_t url_size,
							  char *auth_hdr_out, size_t auth_hdr_size, char *error, size_t error_size) {
	(void)error; (void)error_size;
	snprintf(url_out, url_size, "https://dav.example/%s", rel_path ? rel_path : "");
	if (auth_hdr_out && auth_hdr_size) auth_hdr_out[0] = '\0';
	return true;
}
bool webdav_is_audio_suffix(const char *ext) {
	return ext && (strcasecmp(ext, "flac") == 0 || strcasecmp(ext, "mp3") == 0 || strcasecmp(ext, "m4a") == 0);
}

bool webdavstream_owns(const char *path) { return path && strncmp(path, "webdav://", 9) == 0; }
bool webdavstream_queue(const webdav_entry_t *tracks, int count, int start,
						char paths[][96], const char **pointers, int *queued, int *selected,
						char *error, size_t error_size) {
	if (strcasecmp(tracks[start].suffix, "flac") != 0 &&
		strcasecmp(tracks[start].suffix, "mp3") != 0 &&
		strcasecmp(tracks[start].suffix, "opus") != 0) {
		snprintf(error, error_size, "webdav_enable_sd_cache_for_format");
		return false;
	}
	int q = 0, sel = 0;
	for (int i = 0; i < count; i++) {
		if (tracks[i].kind != WEBDAV_TRACK) continue;
		if (strcasecmp(tracks[i].suffix, "flac") != 0 &&
			strcasecmp(tracks[i].suffix, "mp3") != 0 &&
			strcasecmp(tracks[i].suffix, "opus") != 0) continue;
		if (i == start) sel = q;
		snprintf(paths[q], 96, "webdav://%d.%s", q, tracks[i].suffix);
		pointers[q] = paths[q];
		q++;
	}
	*queued = q;
	*selected = sel;
	return q > 0;
}
bool webdavstream_prepare(char *path, size_t path_size) { (void)path; (void)path_size; return false; }
void webdavstream_prefetch(const char *path) { (void)path; }
void webdavstream_cancel(void) {}
void webdavstream_reset(void) {}
bool webdavstream_take_error(char *out, size_t size) { (void)out; (void)size; return false; }

bool webdavcache_owns(const char *path) { return path && strstr(path, "/.local/webdav-cache/") != NULL; }
bool webdavcache_placeholder(const webdav_entry_t *track, char *out, size_t size) {
	snprintf(out, size, "webdavcache://%s", track->path);
	return true;
}
bool webdavcache_is_placeholder(const char *path) {
	return path && strncmp(path, "webdavcache://", 14) == 0;
}
bool webdavcache_cached(const webdav_entry_t *track, char *out, size_t size) {
	if (out && size) snprintf(out, size, "%s/.local/webdav-cache/%s", card_root_buf, track->title);
	return true; // instant hit for deterministic UI test
}
bool webdavcache_get(const webdav_entry_t *track, char *out_path, size_t out_size,
					 char *error, size_t error_size) {
	(void)error; (void)error_size;
	snprintf(out_path, out_size, "%s/.local/webdav-cache/%s", card_root_buf, track->title);
	return true;
}
int webdavcache_max_mb(void) { return stub_cache_max_mb; }
void webdavcache_set_max_mb(int mb) { stub_cache_max_mb = mb; }
void webdavcache_stats(int *files_out, uint64_t *bytes_out) {
	if (files_out) *files_out = 0;
	if (bytes_out) *bytes_out = 0;
}
void webdavcache_cancel(void) {}
bool webdavcache_clear(int *removed_out) {
	if (removed_out) *removed_out = 0;
	return true;
}

bool http_stream_open_ex(http_stream_t *st, const char *url, int timeout_secs,
						 const http_stream_options_t *options) {
	(void)st; (void)url; (void)timeout_secs; (void)options;
	return false;
}
int http_stream_read(http_stream_t *st, void *buf, int len) {
	(void)st; (void)buf; (void)len;
	return -1;
}
void http_stream_close(http_stream_t *st) { (void)st; }
void http_stream_wake(http_stream_t *st) { (void)st; }

tls_verify_t tls_verify_mode(void) { return stub_verify; }
void tls_set_verify_mode(tls_verify_t mode) { stub_verify = mode; }

/* ---- widget-tree geometry & localization assertions --------------------- */

static void assert_no_raw_webdav_keys(lv_obj_t *root, const char *lang_name) {
	if (!root) return;
	if (lv_obj_check_type(root, &lv_label_class)) {
		const char *txt = lv_label_get_text(root);
		if (txt && (strncmp(txt, "webdav_", 7) == 0 || strcmp(txt, "addon_webdav_note") == 0)) {
			fprintf(stderr, "Untranslated key '%s' on screen in language '%s'\n", txt, lang_name);
			assert(!"raw webdav_* translation key leaked to UI");
		}
	}
	uint32_t cnt = lv_obj_get_child_count(root);
	for (uint32_t i = 0; i < cnt; i++) {
		assert_no_raw_webdav_keys(lv_obj_get_child(root, (int32_t)i), lang_name);
	}
}

typedef struct {
	lv_obj_t *obj;
	lv_area_t area;
	const char *desc;
} leaf_box_t;

static bool areas_overlap(const lv_area_t *a, const lv_area_t *b) {
	return (a->x1 + 1 < b->x2) && (b->x1 + 1 < a->x2) && (a->y1 + 1 < b->y2) && (b->y1 + 1 < a->y2);
}

static void collect_card_leaves(lv_obj_t *node, leaf_box_t *out, int *count, int max) {
	if (!node || lv_obj_is_hidden(node) || *count >= max) return;
	// Floating echo/halo icons in Tokyo Night intentionally sit behind the foreground glyph
	if (lv_obj_is_floating(node)) return;
	if (lv_obj_check_type(node, &lv_label_class)) {
		const char *txt = lv_label_get_text(node);
		if (txt && txt[0]) {
			lv_area_t a;
			lv_obj_get_coords(node, &a);
			a.x1 += lv_obj_get_style_pad_left(node, LV_PART_MAIN);
			a.x2 -= lv_obj_get_style_pad_right(node, LV_PART_MAIN);
			a.y1 += lv_obj_get_style_pad_top(node, LV_PART_MAIN);
			a.y2 -= lv_obj_get_style_pad_bottom(node, LV_PART_MAIN);
			out[*count].area = a;
			out[*count].obj = node;
			out[*count].desc = txt;
			(*count)++;
		}
		return;
	}
	if (lv_obj_check_type(node, &lv_image_class)) {
		lv_obj_get_coords(node, &out[*count].area);
		out[*count].obj = node;
		out[*count].desc = "<icon>";
		(*count)++;
		return;
	}
	uint32_t n = lv_obj_get_child_count(node);
	for (uint32_t i = 0; i < n; i++) {
		collect_card_leaves(lv_obj_get_child(node, (int32_t)i), out, count, max);
	}
}

static lv_obj_t *screen_container(lv_obj_t *screen) {
	lv_obj_t *title = settingsrow_page_title(screen);
	uint32_t top_cnt = lv_obj_get_child_count(screen);
	for (uint32_t i = 0; i < top_cnt; i++) {
		lv_obj_t *ch = lv_obj_get_child(screen, (int32_t)i);
		if (ch != title && !lv_obj_check_type(ch, &lv_button_class) && lv_obj_get_child_count(ch) > 0) {
			return ch;
		}
	}
	return NULL;
}

static void assert_screen_no_overlaps(lv_obj_t *screen, const char *screen_name, const char *lang_name,
									  int theme_kind) {
	assert(screen != NULL);
	lv_obj_update_layout(screen);

	lv_obj_t *title = settingsrow_page_title(screen);
	if (title && !lv_obj_is_hidden(title)) {
		lv_area_t ta;
		lv_obj_get_coords(title, &ta);
		if (ta.x1 < 0 || ta.x2 > bp_screen_w() + 1) {
			fprintf(stderr, "[%s / %s / theme %d] page title '%s' overflows screen [%d..%d]\n", screen_name,
					lang_name, theme_kind, lv_label_get_text(title), (int)ta.x1, (int)ta.x2);
			assert(!"page title horizontal overflow");
		}
		uint32_t sc_cnt = lv_obj_get_child_count(screen);
		for (uint32_t i = 0; i < sc_cnt; i++) {
			lv_obj_t *ch = lv_obj_get_child(screen, (int32_t)i);
			if (ch == title || lv_obj_is_hidden(ch) || !lv_obj_check_type(ch, &lv_button_class)) continue;
			lv_area_t ba;
			lv_obj_get_coords(ch, &ba);
			if (areas_overlap(&ta, &ba)) {
				fprintf(stderr, "[%s / %s / theme %d] page title overlaps corner button\n",
						screen_name, lang_name, theme_kind);
				assert(!"page title overlaps corner button");
			}
		}
	}

	lv_obj_t *container = screen_container(screen);
	if (!container) return;

	uint32_t card_cnt = lv_obj_get_child_count(container);
	for (uint32_t r = 0; r < card_cnt; r++) {
		lv_obj_t *card = lv_obj_get_child(container, (int32_t)r);
		if (!card || lv_obj_is_hidden(card)) continue;
		lv_area_t ca;
		lv_obj_get_coords(card, &ca);
		if (ca.x1 < 0 || ca.x2 > bp_screen_w() + 1) {
			fprintf(stderr, "[%s / %s / theme %d] item %u overflows 240px screen [%d..%d]\n", screen_name,
					lang_name, theme_kind, (unsigned)r, (int)ca.x1, (int)ca.x2);
			assert(!"item horizontal overflow");
		}
		if (lv_obj_check_type(card, &lv_label_class)) continue; // loose note label

		leaf_box_t leaves[32];
		int lcount = 0;
		collect_card_leaves(card, leaves, &lcount, 32);
		for (int i = 0; i < lcount; i++) {
			if (leaves[i].area.x1 < ca.x1 - 1 || leaves[i].area.x2 > ca.x2 + 1 ||
				leaves[i].area.y1 < ca.y1 - 1 || leaves[i].area.y2 > ca.y2 + 1) {
				fprintf(stderr,
						"[%s / %s / theme %d] card %u child '%s' [%d,%d..%d,%d] overflows card [%d,%d..%d,%d]\n",
						screen_name, lang_name, theme_kind, (unsigned)r, leaves[i].desc, (int)leaves[i].area.x1,
						(int)leaves[i].area.y1, (int)leaves[i].area.x2, (int)leaves[i].area.y2, (int)ca.x1,
						(int)ca.y1, (int)ca.x2, (int)ca.y2);
				assert(!"child overflows card bounds");
			}
			for (int j = i + 1; j < lcount; j++) {
				if (areas_overlap(&leaves[i].area, &leaves[j].area)) {
					fprintf(stderr,
							"[%s / %s / theme %d] card %u OVERLAP: '%s' vs '%s'\n",
							screen_name, lang_name, theme_kind, (unsigned)r, leaves[i].desc, leaves[j].desc);
					assert(!"overlapping widgets inside card");
				}
			}
		}
	}
}

static lv_obj_t *find_clickable_with_label(lv_obj_t *root, const char *needle) {
	if (!root || lv_obj_is_hidden(root) || lv_obj_has_state(root, LV_STATE_DISABLED)) return NULL;
	if (lv_obj_check_type(root, &lv_label_class)) {
		const char *txt = lv_label_get_text(root);
		if (txt && strcmp(txt, needle) == 0) {
			lv_obj_t *cur = root;
			while (cur && lv_obj_get_parent(cur) != NULL && !lv_obj_is_clickable(cur)) {
				cur = lv_obj_get_parent(cur);
			}
			if (cur && lv_obj_get_parent(cur) != NULL && lv_obj_is_clickable(cur)) return cur;
		}
	}
	uint32_t cnt = lv_obj_get_child_count(root);
	for (uint32_t i = 0; i < cnt; i++) {
		lv_obj_t *hit = find_clickable_with_label(lv_obj_get_child(root, (int32_t)i), needle);
		if (hit) return hit;
	}
	return NULL;
}

static lv_obj_t *find_switch_for_label(lv_obj_t *root, const char *needle) {
	if (!root || lv_obj_is_hidden(root)) return NULL;
	if (lv_obj_check_type(root, &lv_label_class)) {
		const char *txt = lv_label_get_text(root);
		if (txt && strcmp(txt, needle) == 0) {
			lv_obj_t *parent = lv_obj_get_parent(root);
			if (parent) {
				uint32_t pc = lv_obj_get_child_count(parent);
				for (uint32_t j = 0; j < pc; j++) {
					lv_obj_t *sib = lv_obj_get_child(parent, (int32_t)j);
					if (lv_obj_check_type(sib, &lv_switch_class)) return sib;
				}
			}
		}
	}
	uint32_t cnt = lv_obj_get_child_count(root);
	for (uint32_t i = 0; i < cnt; i++) {
		lv_obj_t *hit = find_switch_for_label(lv_obj_get_child(root, (int32_t)i), needle);
		if (hit) return hit;
	}
	return NULL;
}

static lv_obj_t *find_corner_download_button(lv_obj_t *screen) {
	uint32_t top_cnt = lv_obj_get_child_count(screen);
	for (uint32_t i = 0; i < top_cnt; i++) {
		lv_obj_t *ch = lv_obj_get_child(screen, (int32_t)i);
		if (lv_obj_check_type(ch, &lv_button_class) && lv_obj_get_child_count(ch) == 1 &&
			lv_obj_check_type(lv_obj_get_child(ch, 0), &lv_image_class)) {
			if (lv_image_get_src(lv_obj_get_child(ch, 0)) == &icon_download) {
				return ch;
			}
		}
	}
	return NULL;
}

static void press_back(void) {
	for (int i = 0; i < back_guard_count; i++) {
		if (back_guards[i].screen == lv_screen_active() && back_guards[i].handler()) {
			return;
		}
	}
	if (screen_depth > 0) {
		lv_screen_load(screen_stack[--screen_depth]);
		lv_obj_update_layout(lv_screen_active());
	}
}

static int unlink_cb(const char *fpath, const struct stat *sb, int typeflag, struct FTW *ftwbuf) {
	(void)sb; (void)typeflag; (void)ftwbuf;
	return remove(fpath);
}

static void dummy_flush_cb(lv_display_t *d, const lv_area_t *area, uint8_t *px_map) {
	(void)area; (void)px_map;
	lv_display_flush_ready(d);
}

int main(int argc, char **argv) {
	assert(argc >= 2 && "usage: test_webdav_pages <lang_dir>");
	const char *lang_dir = argv[1];

	char tmp[] = "/tmp/sonix-webdav-ui-XXXXXX";
	assert(mkdtemp(tmp) != NULL);
	snprintf(card_root_buf, sizeof(card_root_buf), "%s/card", tmp);
	assert(mkdir(card_root_buf, 0700) == 0);
	char cfg_file[512];
	snprintf(cfg_file, sizeof(cfg_file), "%s/device_config.ini", tmp);
	(void)config_init(cfg_file);
	config_set("ui", "language", "English");
	lang_init(lang_dir);

	init_fonts();
	lv_init();
	gui_config_t cfg = {bp_screen_w(), bp_screen_h(), bp_padding(), bp_status_bar_h(), card_root_buf};
	lv_display_t *disp = lv_display_create(cfg.screen_width, cfg.screen_height);
	static uint8_t draw_buf[240 * 32 * 4];
	lv_display_set_flush_cb(disp, dummy_flush_cb);
	lv_display_set_buffers(disp, draw_buf, NULL, sizeof(draw_buf), LV_DISPLAY_RENDER_MODE_PARTIAL);
	theme_init();
	popover_init();
	player_screen = lv_obj_create(NULL);
	radiopage_screen = lv_obj_create(NULL);

	// 1. Addon default ON + Streaming page 7-tile layout & toggle off/on
	addons_seed(false);
	assert(addons_enabled("webdav"));

	assert(addons_set("webdav", false));
	assert(!addons_enabled("webdav"));
	streaming_screen = lv_obj_create(NULL);
	streaming_init(&cfg);
	assert(webdav_screen == NULL);
	lv_screen_load(streaming_screen);
	lv_obj_update_layout(streaming_screen);
	lv_obj_t *sgrid_off = screen_container(streaming_screen);
	assert(sgrid_off != NULL && lv_obj_get_child_count(sgrid_off) == 6);
	assert(find_clickable_with_label(streaming_screen, tr("webdav")) == NULL);

	assert(addons_set("webdav", true));
	assert(addons_enabled("webdav"));
	streaming_screen = lv_obj_create(NULL);
	streaming_init(&cfg);
	assert(webdav_screen != NULL);
	assert(prep_cb_count == 2); // webdavstream_prepare + webdavcache_prepare

	lv_screen_load(streaming_screen);
	lv_obj_update_layout(streaming_screen);
	lv_obj_t *sgrid = screen_container(streaming_screen);
	assert(sgrid != NULL && lv_obj_get_child_count(sgrid) == 7);
	lv_obj_t *webdav_tile = find_clickable_with_label(streaming_screen, tr("webdav"));
	assert(webdav_tile != NULL);
	lv_obj_send_event(webdav_tile, LV_EVENT_CLICKED, NULL);
	assert(lv_screen_active() == webdav_screen);

	// 2. WebDAV Home -> Browse folders while unconfigured redirects to Server Settings
	lv_obj_t *folders_row = find_clickable_with_label(webdav_screen, tr("webdav_folders"));
	assert(folders_row != NULL);
	lv_obj_send_event(folders_row, LV_EVENT_CLICKED, NULL);
	lv_obj_t *settings_scr = lv_screen_active();
	assert(settings_scr != webdav_screen);
	assert(strcmp(last_error_toast, tr("webdav_not_configured")) == 0);

	// Verify Cache size limit pill updates cleanly when SD cache switch toggled ON
	lv_obj_t *sd_sw = find_switch_for_label(settings_scr, tr("webdav_sd_cache"));
	assert(sd_sw != NULL);
	lv_obj_add_state(sd_sw, LV_STATE_CHECKED);
	lv_obj_send_event(sd_sw, LV_EVENT_VALUE_CHANGED, NULL);
	assert(stub_sd_cache == true);
	lv_obj_t *pill_1gb = find_clickable_with_label(settings_scr, "1 GB");
	assert(pill_1gb != NULL);
	lv_obj_send_event(pill_1gb, LV_EVENT_CLICKED, NULL);
	assert(stub_cache_max_mb == 1024);
	lv_obj_remove_state(sd_sw, LV_STATE_CHECKED);
	lv_obj_send_event(sd_sw, LV_EVENT_VALUE_CHANGED, NULL);
	assert(stub_sd_cache == false);

	switch_screen_return_to(webdav_screen);

	// 3. Corner arrow opens WebDAV Downloads page (empty card state)
	lv_obj_t *dl_btn = find_corner_download_button(webdav_screen);
	assert(dl_btn != NULL);
	lv_obj_send_event(dl_btn, LV_EVENT_CLICKED, NULL);
	lv_obj_t *dl_scr = lv_screen_active();
	assert(dl_scr == webdavdl_screen);
	assert(find_clickable_with_label(dl_scr, tr("webdav_downloads_empty")) != NULL);
	switch_screen_return_to(webdav_screen);

	// 4. Configure server and open Folder Browser: test subfolder drill-down,
	// back-guard folder ascent, SD Cache OFF format guard & RAM-stream queue,
	// and SD Cache ON placeholder queue!
	snprintf(stub_creds.server, sizeof(stub_creds.server), "https://dav.example/music");
	snprintf(stub_creds.username, sizeof(stub_creds.username), "alice");
	stub_configured = true;

	folders_row = find_clickable_with_label(webdav_screen, tr("webdav_folders"));
	lv_obj_send_event(folders_row, LV_EVENT_CLICKED, NULL);
	lv_obj_t *browser_scr = lv_screen_active();
	assert(browser_scr != webdav_screen);
	// Pump LVGL timer (500 ms poll_cb) until browse worker delivers entries
	for (int i = 0; i < 100 && !find_clickable_with_label(browser_scr, "Jazz"); i++) {
		usleep(5000);
		drain_posts();
		lv_tick_inc(550);
		lv_timer_handler();
	}
	lv_obj_update_layout(browser_scr);
	assert(find_clickable_with_label(browser_scr, "Jazz") != NULL);

	// Drill into "Jazz" subfolder
	lv_obj_t *jazz_row = find_clickable_with_label(browser_scr, "Jazz");
	assert(jazz_row != NULL);
	lv_obj_send_event(jazz_row, LV_EVENT_CLICKED, NULL);
	for (int i = 0; i < 100 && !find_clickable_with_label(browser_scr, "Take Five.mp3"); i++) {
		usleep(5000);
		drain_posts();
		lv_tick_inc(550);
		lv_timer_handler();
	}
	assert(find_clickable_with_label(browser_scr, "Take Five.mp3") != NULL);

	// Back guard climbs from "Jazz" back up to root "" WITHOUT leaving browser_scr!
	press_back();
	assert(lv_screen_active() == browser_scr);
	for (int i = 0; i < 100 && !find_clickable_with_label(browser_scr, "01 - So What.flac"); i++) {
		usleep(5000);
		drain_posts();
		lv_tick_inc(550);
		lv_timer_handler();
	}
	assert(find_clickable_with_label(browser_scr, "01 - So What.flac") != NULL);

	// SD Cache OFF: tapping `.m4a` shows `webdav_enable_sd_cache_for_format` toast
	last_error_toast[0] = '\0';
	lv_obj_t *m4a_row = find_clickable_with_label(browser_scr, "02 - Blue.m4a");
	assert(m4a_row != NULL);
	lv_obj_send_event(m4a_row, LV_EVENT_CLICKED, NULL);
	assert(strcmp(last_error_toast, tr("webdav_enable_sd_cache_for_format")) == 0);
	assert(lv_screen_active() == browser_scr);

	// SD Cache OFF: tapping `.flac` queues `webdav://0.flac` and opens player sheet
	int prev_sheets = sheet_open_count;
	lv_obj_t *flac_row = find_clickable_with_label(browser_scr, "01 - So What.flac");
	assert(flac_row != NULL);
	lv_obj_send_event(flac_row, LV_EVENT_CLICKED, NULL);
	assert(sheet_open_count == prev_sheets + 1);
	assert(queued_count == 1 && strcmp(queued_paths[0], "webdav://0.flac") == 0);

	// SD Cache ON: tapping `.m4a` queues `webdavcache://02 - Blue.m4a` and opens player sheet!
	stub_sd_cache = true;
	prev_sheets = sheet_open_count;
	m4a_row = find_clickable_with_label(browser_scr, "02 - Blue.m4a");
	assert(m4a_row != NULL);
	lv_obj_send_event(m4a_row, LV_EVENT_CLICKED, NULL);
	assert(sheet_open_count == prev_sheets + 1);
	assert(queued_count == 2);
	assert(strcmp(queued_paths[0], "webdavcache://01 - So What.flac") == 0);
	assert(strcmp(queued_paths[1], "webdavcache://02 - Blue.m4a") == 0);
	assert(queued_start_idx == 1);

	// Second press_back() from root "" exits browser_scr back to webdav_screen!
	press_back();
	assert(lv_screen_active() == webdav_screen);

	// 5. Sweep all 11 shipped languages x 5 built-in themes across Streaming Grid,
	// WebDAV Home, Server Settings, Folder Browser, and Downloads!
	static const char *const LANGS[] = {
		"Deutsch", "English", "Español", "Français", "Italiano", "Japanese",
		"Nederlands", "Polski", "Português BR", "Russian", "Ukrainian",
	};
	lv_obj_t *screens[] = {streaming_screen, webdav_screen, settings_scr, browser_scr, dl_scr};
	const char *names[] = {"streaming_grid", "webdav_home", "webdav_settings", "webdav_browser", "webdav_downloads"};

	for (size_t li = 0; li < sizeof(LANGS) / sizeof(LANGS[0]); li++) {
		assert(lang_set(LANGS[li]));
		for (int tk = 0; tk < THEME_KIND_COUNT; tk++) {
			theme_set_kind((theme_kind_t)tk);
			for (size_t si = 0; si < sizeof(screens) / sizeof(screens[0]); si++) {
				lv_screen_load(screens[si]);
				lv_obj_update_layout(screens[si]);
				assert_no_raw_webdav_keys(screens[si], LANGS[li]);
				assert_screen_no_overlaps(screens[si], names[si], LANGS[li], tk);
			}
		}
	}

	assert(lang_set("English"));
	theme_set_kind(THEME_KIND_DARK);

	printf("PASS webdav_pages (240x320, 11 languages x 5 themes zero-overlap across Streaming grid, "
		   "Home, Settings, Browser & Downloads; folder back-stack; RAM & SD-cache playback queues)\n");

	(void)confirm_state;
	nftw(tmp, unlink_cb, 64, FTW_DEPTH | FTW_PHYS);
	return 0;
}
