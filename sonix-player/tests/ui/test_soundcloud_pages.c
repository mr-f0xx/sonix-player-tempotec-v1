/*
 * Headless LVGL integration test for SoundCloud pages (`soundcloudpage.c` +
 * `soundcloud_icon.c`) at V1 dimensions (240x320) across all 11 shipped
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
#include "src/gui/shell/gui.h"
#include "src/gui/shell/icons.h"
#include "src/gui/shell/keyboard.h"
#include "src/gui/shell/popover.h"
#include "src/gui/shell/settingsrow.h"
#include "src/gui/shell/switcher.h"
#include "src/gui/shell/theme.h"
#include "src/gui/shell/toast.h"
#include "src/gui/streaming/soundcloud_icon.h"
#include "src/gui/streaming/soundcloudpage.h"
#include "src/system/audio/audio.h"
#include "src/system/core/config.h"
#include "src/system/core/lang.h"
#include "src/system/core/utils.h"
#include "src/system/device/system.h"
#include "src/system/playback/device_state.h"
#include "src/system/playback/playlist.h"
#include "src/system/streaming/soundcloud.h"
#include "src/system/streaming/soundcloudcache.h"
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
	assert(back_guard_count < 4);
	back_guards[back_guard_count].screen = screen;
	back_guards[back_guard_count].handler = handler;
	back_guard_count++;
}

static int busy_toasts;
static int downloading_toasts;
static int popup_calls;
static bool stub_sheet_open;
static volatile int gate_stream_get;

void toast_busy(const char *key) {
	if (key && strcmp(key, "downloading") == 0) {
		downloading_toasts++;
	}
	busy_toasts++;
}
void toast_busy_end(void) {
	if (busy_toasts > 0) busy_toasts--;
}

static void pump_until_idle(const char *what) {
	for (int i = 0; i < 400; i++) {
		lv_tick_inc(250);
		lv_timer_handler();
		if (busy_toasts == 0 && !soundcloudpage_waiting_for_track()) {
			lv_obj_update_layout(lv_screen_active());
			return;
		}
		usleep(5000);
	}
	fprintf(stderr, "%s: worker job never completed\n", what);
	assert(!"worker job timed out");
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

static int queued_count;
static char queued_paths[8][576];
static int queued_start_idx = -1;
static int played_queue_idx = -1;
static device_state_prepare_cb registered_prep_cb;

void device_state_add_prepare_cb(device_state_prepare_cb cb) {
	registered_prep_cb = cb;
}

void playlist_load_paths(const char *const *list, int count, int start_index) {
	queued_count = count < 8 ? count : 8;
	for (int i = 0; i < queued_count; i++) {
		snprintf(queued_paths[i], sizeof(queued_paths[i]), "%s", list[i]);
	}
	queued_start_idx = start_index;
	played_queue_idx = start_index;
}

int playlist_count(void) { return queued_count; }
int playlist_current_index(void) { return played_queue_idx; }
bool playlist_path_at(int index, char *out, size_t out_size) {
	if (index < 0 || index >= queued_count || !out || out_size == 0) return false;
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
	return true;
}

void device_state_play_list(const char *const *list, int count, int start_index) {
	playlist_load_paths(list, count, start_index);
}

void device_state_play_file(const char *path) {
	(void)path;
}

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
bool gui_post(void (*cb)(void *), void *user) {
	if (cb) cb(user);
	return true;
}
void fonts_register_change(void (*cb)(void)) {}
void bootlogo_set(int choice, bool dark) {}
bool bootpic_present(void) { return false; }
void gui_notify_popup(const char *text) { popup_calls++; }
void toast_success(const char *text) {}
void toast_error(const char *text) {}
void switcher_attach_back_gesture(lv_obj_t *screen) {}
bool switcher_back_drag_active(void) { return false; }
void switcher_set_back_guard_peek(lv_obj_t *screen, bool (*peek)(void)) {}
/* The tiled home page brings in the grid, which asks the cover flow whether a
 * swipe is in progress and names the pet's artwork. The pet itself is not part
 * of this page. */
bool coverflow_drag_active(void) { return false; }
const lv_image_dsc_t icon_pet = {0};
void player_sheet_attach_drag(lv_obj_t *obj, bool opening) {}
bool player_sheet_drag_active(void) { return false; }
bool player_sheet_is_open(void) { return stub_sheet_open; }
void player_sheet_open(bool animate) {}
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
void streamturn_abandon_all(void) {}
#pragma GCC diagnostic pop

/* ---- recording SoundCloud client & cache stubs -------------------------- */

static soundcloud_credentials_t stub_creds;
static bool stub_has_acc;
static bool stub_remember = true;
static int stub_quality = SOUNDCLOUD_QUALITY_STANDARD;
static int stub_cache_clears;
static int stub_get_calls;

void soundcloud_init(void) {}

bool soundcloud_configure(const soundcloud_credentials_t *credentials, bool remember, char *error, size_t error_size) {
	(void)error;
	(void)error_size;
	if (credentials) stub_creds = *credentials;
	stub_remember = remember;
	stub_has_acc = (stub_creds.username[0] != '\0' || stub_creds.oauth_token[0] != '\0');
	return true;
}

bool soundcloud_save_draft(const soundcloud_credentials_t *credentials, bool remember, char *error,
						   size_t error_size) {
	(void)error;
	(void)error_size;
	if (!credentials) return false;
	stub_creds = *credentials;
	if (strncmp(stub_creds.username, "https://soundcloud.com/", 23) == 0) {
		memmove(stub_creds.username, stub_creds.username + 23, strlen(stub_creds.username + 23) + 1);
		char *q = strpbrk(stub_creds.username, "/?#");
		if (q) *q = '\0';
	}
	stub_remember = remember;
	stub_has_acc = (stub_creds.username[0] != '\0' || stub_creds.oauth_token[0] != '\0');
	return true;
}

void soundcloud_credentials(soundcloud_credentials_t *out, bool *remembered) {
	if (out) *out = stub_creds;
	if (remembered) *remembered = stub_remember;
}

void soundcloud_account_info(soundcloud_account_info_t *out) {
	memset(out, 0, sizeof(*out));
	if (!stub_has_acc) return;
	out->user_id = 4294967305LL;
	snprintf(out->display_name, sizeof(out->display_name), "DEEPERKΔY");
	snprintf(out->permalink, sizeof(out->permalink), "%s",
			 stub_creds.username[0] ? stub_creds.username : "deeperkay");
	if (stub_creds.oauth_token[0]) {
		snprintf(out->tier, sizeof(out->tier), "Go+");
	}
	out->has_token = stub_creds.oauth_token[0] != '\0';
	out->remembered = stub_remember;
}

bool soundcloud_has_account(void) { return stub_has_acc; }
bool soundcloud_can_like(void) { return stub_has_acc && stub_creds.oauth_token[0] != '\0'; }

bool soundcloud_forget(void) {
	memset(&stub_creds, 0, sizeof(stub_creds));
	stub_has_acc = false;
	return true;
}

int soundcloud_get_quality(void) { return stub_quality; }
void soundcloud_set_quality(int q) { stub_quality = q; }
const char *soundcloud_last_error(void) { return "soundcloud_connection_failed"; }

static int fill_two_tracks(soundcloud_track_t *out, int max) {
	assert(max >= 2);
	memset(out, 0, 2 * sizeof(*out));
	out[0].id = 2199023255552LL;
	snprintf(out[0].title, sizeof(out[0].title), "Aurora Borealis");
	snprintf(out[0].artist, sizeof(out[0].artist), "Northern Synth");
	out[0].duration = 215;
	out[0].full_duration = 215;
	out[0].streamable = true;

	out[1].id = 2199023255553LL;
	snprintf(out[1].title, sizeof(out[1].title), "Solar Wind");
	snprintf(out[1].artist, sizeof(out[1].artist), "Northern Synth");
	out[1].duration = 184;
	out[1].full_duration = 184;
	out[1].streamable = true;
	return 2;
}

int soundcloud_search_tracks(const char *query, int offset, soundcloud_track_t *out, int max, bool *more) {
	(void)query;
	(void)offset;
	if (more) *more = false;
	return fill_two_tracks(out, max);
}

int soundcloud_liked_tracks(const char *cursor, soundcloud_track_t *out, int max, char *next_cursor,
							size_t next_cursor_size) {
	(void)cursor;
	if (next_cursor && next_cursor_size) next_cursor[0] = '\0';
	return fill_two_tracks(out, max);
}

int soundcloud_reposts(const char *cursor, soundcloud_repost_t *out, int max, char *next_cursor,
					   size_t next_cursor_size) {
	(void)cursor;
	assert(max >= 1);
	if (next_cursor && next_cursor_size) next_cursor[0] = '\0';
	memset(&out[0], 0, sizeof(out[0]));
	out[0].is_playlist = false;
	soundcloud_track_t tmp[2];
	fill_two_tracks(tmp, 2);
	out[0].track = tmp[0];
	return 1;
}

int soundcloud_playlists(const char *cursor, soundcloud_playlist_t *out, int max, char *next_cursor,
						 size_t next_cursor_size) {
	(void)cursor;
	assert(max >= 1);
	if (next_cursor && next_cursor_size) next_cursor[0] = '\0';
	memset(&out[0], 0, sizeof(out[0]));
	out[0].id = 7700000001LL;
	snprintf(out[0].title, sizeof(out[0].title), "Late Night Electronic");
	snprintf(out[0].owner, sizeof(out[0].owner), "Curator");
	out[0].track_count = 2;
	return 1;
}

int soundcloud_playlist_tracks(long long playlist_id, const char *secret_token, int offset, soundcloud_track_t *out,
							   int max, bool *more) {
	(void)playlist_id;
	(void)secret_token;
	(void)offset;
	if (more) *more = false;
	return fill_two_tracks(out, max);
}

bool soundcloud_is_liked(long long track_id) { return track_id == 2199023255552LL; }
bool soundcloud_like_track(long long track_id, bool like) {
	(void)track_id;
	(void)like;
	return true;
}

static int stub_cache_max_mb = SOUNDCLOUDCACHE_DEFAULT_MAX_MB;

void soundcloudcache_set_root(const char *sd_root) { (void)sd_root; }
bool soundcloudcache_available(void) { return true; }
const char *soundcloudcache_last_error(void) { return "soundcloud_stream_failed"; }
int soundcloudcache_get_max_mb(void) { return stub_cache_max_mb; }
void soundcloudcache_set_max_mb(int max_mb) { stub_cache_max_mb = max_mb; }
long long soundcloudcache_get_max_bytes(void) {
	return stub_cache_max_mb <= 0 ? 0 : (long long)stub_cache_max_mb * 1024LL * 1024LL;
}
void soundcloudcache_set_max_bytes(long long max_bytes) {
	stub_cache_max_mb = max_bytes <= 0 ? 0 : (int)(max_bytes / (1024LL * 1024LL));
}

bool soundcloudcache_is_soundcloud_path(const char *p) {
	return p && strncmp(p, "/mnt/sd/.local/soundcloud-cache/", 32) == 0;
}
long long soundcloudcache_track_id_of(const char *p) {
	if (!soundcloudcache_is_soundcloud_path(p)) return 0;
	return atoll(p + 32);
}
bool soundcloudcache_placeholder_path(long long id, char *out, size_t size) {
	int n = snprintf(out, size, "/mnt/sd/.local/soundcloud-cache/%lld.mp3", id);
	return n > 0 && (size_t)n < size;
}
bool soundcloudcache_find_playable(long long id, char *out, size_t size) {
	if (stub_get_calls == 0 || id == 2199023255553LL) return false;
	int n = snprintf(out, size, "/mnt/sd/.local/soundcloud-cache/%lld.opus", id);
	return n > 0 && (size_t)n < size;
}
bool soundcloudcache_get(const soundcloud_track_t *track, char *out, size_t size) {
	while (__atomic_load_n(&gate_stream_get, __ATOMIC_ACQUIRE)) {
		usleep(2000);
	}
	stub_get_calls++;
	int n = snprintf(out, size, "/mnt/sd/.local/soundcloud-cache/%lld.opus", track->id);
	return n > 0 && (size_t)n < size;
}
bool soundcloudcache_prefetch(const soundcloud_track_t *track) {
	(void)track;
	return true;
}
void soundcloudcache_cancel(void) {}
void soundcloudcache_protect(const char *path) { (void)path; }
void soundcloudcache_clear(void) { stub_cache_clears++; }
void soundcloudcache_stats(int *files, long long *bytes) {
	if (files) *files = 3;
	if (bytes) *bytes = 9 * 1024 * 1024;
}

/* ---- widget-tree inspection helpers ------------------------------------- */

static void assert_no_raw_soundcloud_keys(lv_obj_t *root, const char *lang_name) {
	if (!root) return;
	if (lv_obj_check_type(root, &lv_label_class)) {
		const char *txt = lv_label_get_text(root);
		if (txt && strncmp(txt, "soundcloud_", 11) == 0) {
			fprintf(stderr, "Untranslated key '%s' on screen in language '%s'\n", txt, lang_name);
			assert(!"raw soundcloud_* translation key leaked to UI");
		}
	}
	uint32_t cnt = lv_obj_get_child_count(root);
	for (uint32_t i = 0; i < cnt; i++) {
		assert_no_raw_soundcloud_keys(lv_obj_get_child(root, (int32_t)i), lang_name);
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
	if (lv_obj_check_type(node, &lv_switch_class)) {
		lv_obj_get_coords(node, &out[*count].area);
		out[*count].obj = node;
		out[*count].desc = "<switch>";
		(*count)++;
		return;
	}
	uint32_t n = lv_obj_get_child_count(node);
	for (uint32_t i = 0; i < n; i++) {
		collect_card_leaves(lv_obj_get_child(node, (int32_t)i), out, count, max);
	}
}

static void assert_screen_no_overlaps(lv_obj_t *screen, const char *screen_name, const char *lang_name,
									  int theme_kind) {
	assert(screen != NULL);
	lv_obj_update_layout(screen);

	// 1. Page heading must stay within horizontal screen bounds and never collide with corner buttons.
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
				fprintf(stderr,
						"[%s / %s / theme %d] page title '%s' [%d,%d..%d,%d] overlaps corner button [%d,%d..%d,%d]\n",
						screen_name, lang_name, theme_kind, lv_label_get_text(title), (int)ta.x1, (int)ta.y1,
						(int)ta.x2, (int)ta.y2, (int)ba.x1, (int)ba.y1, (int)ba.x2, (int)ba.y2);
				assert(!"page title overlaps corner button");
			}
		}
	}

	// 2. Every card/row in the scroll container must keep all labels/icons/switches non-overlapping and in-bounds.
	lv_obj_t *container = NULL;
	uint32_t top_cnt = lv_obj_get_child_count(screen);
	for (uint32_t i = 0; i < top_cnt; i++) {
		lv_obj_t *ch = lv_obj_get_child(screen, (int32_t)i);
		if (ch != title && !lv_obj_check_type(ch, &lv_button_class) && lv_obj_get_child_count(ch) > 0) {
			container = ch;
			break;
		}
	}
	if (!container) return;

	uint32_t row_cnt = lv_obj_get_child_count(container);
	for (uint32_t r = 0; r < row_cnt; r++) {
		lv_obj_t *card = lv_obj_get_child(container, (int32_t)r);
		if (!card || lv_obj_is_hidden(card)) continue;
		lv_area_t ca;
		lv_obj_get_coords(card, &ca);
		if (ca.x1 < 0 || ca.x2 > bp_screen_w() + 1) {
			fprintf(stderr, "[%s / %s / theme %d] row %u overflows 240px screen [%d..%d]\n", screen_name, lang_name,
					theme_kind, (unsigned)r, (int)ca.x1, (int)ca.x2);
			assert(!"card horizontal overflow");
		}

		leaf_box_t leaves[32];
		int lcount = 0;
		collect_card_leaves(card, leaves, &lcount, 32);
		for (int i = 0; i < lcount; i++) {
			if (leaves[i].area.x1 < ca.x1 - 1 || leaves[i].area.x2 > ca.x2 + 1 ||
				leaves[i].area.y1 < ca.y1 - 1 || leaves[i].area.y2 > ca.y2 + 1) {
				fprintf(stderr,
						"[%s / %s / theme %d] row %u child '%s' [%d,%d..%d,%d] overflows card [%d,%d..%d,%d]\n",
						screen_name, lang_name, theme_kind, (unsigned)r, leaves[i].desc, (int)leaves[i].area.x1,
						(int)leaves[i].area.y1, (int)leaves[i].area.x2, (int)leaves[i].area.y2, (int)ca.x1,
						(int)ca.y1, (int)ca.x2, (int)ca.y2);
				assert(!"child overflows card bounds");
			}
			for (int j = i + 1; j < lcount; j++) {
				if (areas_overlap(&leaves[i].area, &leaves[j].area)) {
					fprintf(stderr,
							"[%s / %s / theme %d] row %u OVERLAP: '%s' [%d,%d..%d,%d] vs '%s' [%d,%d..%d,%d]\n",
							screen_name, lang_name, theme_kind, (unsigned)r, leaves[i].desc, (int)leaves[i].area.x1,
							(int)leaves[i].area.y1, (int)leaves[i].area.x2, (int)leaves[i].area.y2, leaves[j].desc,
							(int)leaves[j].area.x1, (int)leaves[j].area.y1, (int)leaves[j].area.x2,
							(int)leaves[j].area.y2);
					assert(!"overlapping widgets inside row");
				}
			}
		}
	}
}

static bool tree_has_visible_glyph(lv_obj_t *root, const lv_image_dsc_t *glyph) {
	if (!root || lv_obj_is_hidden(root)) return false;
	if (lv_obj_check_type(root, &lv_image_class) && lv_image_get_src(root) == glyph) {
		return true;
	}
	uint32_t n = lv_obj_get_child_count(root);
	for (uint32_t i = 0; i < n; i++) {
		if (tree_has_visible_glyph(lv_obj_get_child(root, (int32_t)i), glyph)) return true;
	}
	return false;
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

static lv_obj_t *find_clickable_with_glyph(lv_obj_t *root, const lv_image_dsc_t *glyph) {
	if (!root || lv_obj_is_hidden(root) || lv_obj_has_state(root, LV_STATE_DISABLED)) return NULL;
	if (lv_obj_check_type(root, &lv_image_class) && lv_image_get_src(root) == glyph) {
		lv_obj_t *cur = root;
		while (cur && lv_obj_get_parent(cur) != NULL && !lv_obj_is_clickable(cur)) {
			cur = lv_obj_get_parent(cur);
		}
		if (cur && lv_obj_get_parent(cur) != NULL && lv_obj_is_clickable(cur)) return cur;
	}
	uint32_t cnt = lv_obj_get_child_count(root);
	for (uint32_t i = 0; i < cnt; i++) {
		lv_obj_t *hit = find_clickable_with_glyph(lv_obj_get_child(root, (int32_t)i), glyph);
		if (hit) return hit;
	}
	return NULL;
}

static lv_obj_t *find_textarea(lv_obj_t *root) {
	if (!root) return NULL;
	if (lv_obj_check_type(root, &lv_textarea_class)) return root;
	uint32_t cnt = lv_obj_get_child_count(root);
	for (uint32_t i = 0; i < cnt; i++) {
		lv_obj_t *hit = find_textarea(lv_obj_get_child(root, (int32_t)i));
		if (hit) return hit;
	}
	return NULL;
}

static int unlink_cb(const char *fpath, const struct stat *sb, int typeflag, struct FTW *ftwbuf) {
	(void)sb;
	(void)typeflag;
	(void)ftwbuf;
	return remove(fpath);
}

static void dummy_flush_cb(lv_display_t *d, const lv_area_t *area, uint8_t *px_map) {
	(void)area;
	(void)px_map;
	lv_display_flush_ready(d);
}

int main(int argc, char **argv) {
	assert(argc >= 2 && "usage: test_soundcloud_pages <lang_dir>");
	const char *lang_dir = argv[1];

	char tmp[] = "/tmp/sonix-soundcloud-ui-XXXXXX";
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

	// Verify ARGB8888 SoundCloud icons have expected 2x-supersampled V1 dimensions.
	assert(icon_soundcloud_mark.header.w == 32 && icon_soundcloud_mark.header.h == 32);
	assert(icon_soundcloud_badge.header.w == 52 && icon_soundcloud_badge.header.h == 52);
	assert(icon_menu_soundcloud.header.w == 112 && icon_menu_soundcloud.header.h == 112);

	soundcloudpage_init(&cfg);
	assert(soundcloud_screen != NULL);
	assert(registered_prep_cb == soundcloudpage_prepare_track);
	switch_screen(soundcloud_screen);
	lv_timer_handler();
	lv_obj_update_layout(soundcloud_screen);

	// 1. Search works without any account -> Discreet row spinner (no "Downloading" popup!) -> Playback handoff
	// Search has no row of its own any more: the title bar's corner button is the
	// only way in, and the home list must not repeat it.
	assert(find_clickable_with_label(soundcloud_screen, tr("soundcloud_search_tracks")) == NULL);
	lv_obj_t *search_row = find_clickable_with_glyph(soundcloud_screen, &icon_search);
	assert(search_row != NULL);
	lv_obj_send_event(search_row, LV_EVENT_CLICKED, NULL);
	lv_obj_t *search_scr = lv_screen_active();
	assert(search_scr != soundcloud_screen);
	lv_obj_t *ta = find_textarea(search_scr);
	assert(ta != NULL);
	lv_textarea_set_text(ta, "aurora");
	lv_obj_t *search_ok = find_clickable_with_label(search_scr, tr("ok"));
	assert(search_ok != NULL);
	lv_obj_send_event(search_ok, LV_EVENT_CLICKED, NULL);
	pump_until_idle("search tracks");
	lv_obj_t *list_scr = lv_screen_active();
	assert_screen_no_overlaps(list_scr, "list_screen", "English", THEME_KIND_DARK);

	lv_obj_t *track_row = find_clickable_with_label(list_scr, "Aurora Borealis");
	assert(track_row != NULL);
	assert(tree_has_visible_glyph(track_row, &icon_chevron_right));
	assert(!tree_has_visible_glyph(track_row, &icon_loader_small));

	// Gate worker stream fetch so we can inspect the live discreet loading state!
	__atomic_store_n(&gate_stream_get, 1, __ATOMIC_RELEASE);
	lv_obj_send_event(track_row, LV_EVENT_CLICKED, NULL);
	assert(soundcloudpage_waiting_for_track());
	assert(downloading_toasts == 0);
	assert(popup_calls == 0);
	assert(tree_has_visible_glyph(track_row, &icon_loader_small));
	assert(!tree_has_visible_glyph(track_row, &icon_chevron_right));
	assert(!tree_has_visible_glyph(lv_layer_top(), &icon_loader_small));

	// Simulate Now Playing sheet open: top-layer discreet spinner badge takes over cleanly.
	stub_sheet_open = true;
	lv_tick_inc(250);
	lv_timer_handler();
	assert(tree_has_visible_glyph(lv_layer_top(), &icon_loader_small));
	stub_sheet_open = false;

	// Release gate and verify spinner clears and chevron returns once playback starts.
	__atomic_store_n(&gate_stream_get, 0, __ATOMIC_RELEASE);
	pump_until_idle("play track");
	assert(!soundcloudpage_waiting_for_track());
	assert(!tree_has_visible_glyph(track_row, &icon_loader_small));
	assert(tree_has_visible_glyph(track_row, &icon_chevron_right));
	assert(!tree_has_visible_glyph(lv_layer_top(), &icon_loader_small));
	assert(downloading_toasts == 0);
	assert(popup_calls == 0);

	assert(queued_count == 2);
	assert(queued_start_idx == 0);
	assert(strcmp(queued_paths[0], "/mnt/sd/.local/soundcloud-cache/2199023255552.mp3") == 0);

	// Verify soundcloudpage_prepare_track resolves cached `<id>.mp3` -> `<id>.opus` synchronously,
	// AND defers uncached `<id>.mp3` asynchronously with discreet spinner and ZERO "Downloading" popups!
	char prep_path[576];
	snprintf(prep_path, sizeof(prep_path), "%s", queued_paths[0]);
	assert(soundcloudpage_prepare_track(prep_path, sizeof(prep_path)));
	assert(strcmp(prep_path, "/mnt/sd/.local/soundcloud-cache/2199023255552.opus") == 0);

	snprintf(prep_path, sizeof(prep_path), "%s", queued_paths[1]);
	assert(!soundcloudpage_prepare_track(prep_path, sizeof(prep_path)));
	assert(soundcloudpage_waiting_for_track());
	assert(downloading_toasts == 0);
	assert(popup_calls == 0);
	pump_until_idle("prepare uncached track");
	assert(!soundcloudpage_waiting_for_track());
	assert(downloading_toasts == 0);
	assert(popup_calls == 0);

	// 2a. Profile URL saves immediately on keyboard OK even if user backs out before tapping Connect & save!
	switch_screen_return_to(soundcloud_screen);
	lv_obj_t *settings_row = find_clickable_with_label(soundcloud_screen, tr("soundcloud_account_settings"));
	assert(settings_row != NULL);
	lv_obj_send_event(settings_row, LV_EVENT_CLICKED, NULL);
	lv_obj_t *settings_scr = lv_screen_active();
	assert(settings_scr != soundcloud_screen);

	// Check signed-out Home & Settings for overlaps before entering profile.
	assert_screen_no_overlaps(soundcloud_screen, "soundcloud_home_signed_out", "English", THEME_KIND_DARK);
	assert_screen_no_overlaps(settings_scr, "soundcloud_settings_signed_out", "English", THEME_KIND_DARK);

	lv_obj_t *prof_row = find_clickable_with_label(settings_scr, tr("soundcloud_username"));
	assert(prof_row != NULL);
	lv_obj_send_event(prof_row, LV_EVENT_CLICKED, NULL);
	lv_obj_t *prof_ta = find_textarea(lv_screen_active());
	assert(prof_ta != NULL);
	lv_textarea_set_text(prof_ta, "https://soundcloud.com/deeperkay?si=share123");
	lv_obj_t *prof_ok = find_clickable_with_label(lv_screen_active(), tr("ok"));
	assert(prof_ok != NULL);
	lv_obj_send_event(prof_ok, LV_EVENT_CLICKED, NULL);
	assert(lv_screen_active() == settings_scr);
	assert(stub_has_acc);
	assert(stub_remember == true);
	assert(strcmp(stub_creds.username, "deeperkay") == 0);

	// Back out to Home and reopen Account & Settings: saved profile must still be present & non-overlapping!
	switch_screen_return_to(soundcloud_screen);
	assert_screen_no_overlaps(soundcloud_screen, "soundcloud_home_deeperkay", "English", THEME_KIND_DARK);
	lv_obj_send_event(settings_row, LV_EVENT_CLICKED, NULL);
	assert(lv_screen_active() == settings_scr);
	assert(find_clickable_with_label(settings_scr, "deeperkay") != NULL);
	assert_screen_no_overlaps(settings_scr, "soundcloud_settings_deeperkay", "English", THEME_KIND_DARK);

	// 2b. Enter OAuth token -> Connect Go+ account -> Quality pills -> Clear cache

	lv_obj_t *tok_row = find_clickable_with_label(settings_scr, tr("soundcloud_oauth_token"));
	assert(tok_row != NULL);
	lv_obj_send_event(tok_row, LV_EVENT_CLICKED, NULL);
	lv_obj_t *edit_ta = find_textarea(lv_screen_active());
	assert(edit_ta != NULL);
	lv_textarea_set_text(edit_ta, "2-999999-test-token");
	lv_obj_t *edit_ok = find_clickable_with_label(lv_screen_active(), tr("ok"));
	assert(edit_ok != NULL);
	lv_obj_send_event(edit_ok, LV_EVENT_CLICKED, NULL);
	assert(lv_screen_active() == settings_scr);

	lv_obj_t *connect_row = find_clickable_with_label(settings_scr, tr("soundcloud_connect_save"));
	assert(connect_row != NULL);
	lv_obj_send_event(connect_row, LV_EVENT_CLICKED, NULL);
	pump_until_idle("connect save");
	assert(stub_has_acc);
	assert(lv_screen_active() == soundcloud_screen);

	// Re-open settings to test quality pills & clear cache
	lv_obj_send_event(settings_row, LV_EVENT_CLICKED, NULL);
	assert(lv_screen_active() == settings_scr);

	lv_obj_t *pill_high = find_clickable_with_label(settings_scr, "AAC 256k");
	assert(pill_high != NULL);
	lv_obj_send_event(pill_high, LV_EVENT_CLICKED, NULL);
	assert(stub_quality == SOUNDCLOUD_QUALITY_HIGH);

	// Verify SD cache limit pills (512 MB default -> 2 GB -> 8 GB -> Unlimited) & live Clear cache subtitle!
	assert(stub_cache_max_mb == 512);
	assert(find_clickable_with_label(settings_scr, "3 · 9.0 MB / 512 MB") != NULL);

	lv_obj_t *pill_2gb = find_clickable_with_label(settings_scr, "2 GB");
	assert(pill_2gb != NULL);
	lv_obj_send_event(pill_2gb, LV_EVENT_CLICKED, NULL);
	assert(stub_cache_max_mb == 2048);
	assert(find_clickable_with_label(settings_scr, "3 · 9.0 MB / 2 GB") != NULL);

	lv_obj_t *pill_8gb = find_clickable_with_label(settings_scr, "8 GB");
	assert(pill_8gb != NULL);
	lv_obj_send_event(pill_8gb, LV_EVENT_CLICKED, NULL);
	assert(stub_cache_max_mb == 8192);
	assert(find_clickable_with_label(settings_scr, "3 · 9.0 MB / 8 GB") != NULL);

	lv_obj_t *pill_unlim = find_clickable_with_label(settings_scr, tr("soundcloud_cache_unlimited"));
	assert(pill_unlim != NULL);
	lv_obj_send_event(pill_unlim, LV_EVENT_CLICKED, NULL);
	assert(stub_cache_max_mb == 0);
	assert(find_clickable_with_label(settings_scr, "3 · 9.0 MB / Unlimited") != NULL);

	lv_obj_t *clear_row = find_clickable_with_label(settings_scr, tr("soundcloud_clear_cache"));
	assert(clear_row != NULL);
	lv_obj_send_event(clear_row, LV_EVENT_CLICKED, NULL);
	assert(confirm_state.open);
	confirm_state.open = false;
	confirm_state.on_ok(confirm_state.user);
	assert(stub_cache_clears == 1);

	// 3. Sweep all 11 shipped languages and all 5 built-in themes across Home & Settings,
	// asserting BOTH zero raw translation keys AND zero widget overlaps/overflows on 240x320!
	static const char *const LANGS[] = {
		"Deutsch", "English", "Español", "Français", "Italiano", "Japanese",
		"Nederlands", "Polski", "Português BR", "Russian", "Ukrainian",
	};
	for (size_t li = 0; li < sizeof(LANGS) / sizeof(LANGS[0]); li++) {
		assert(lang_set(LANGS[li]));
		for (int tk = 0; tk < THEME_KIND_COUNT; tk++) {
			theme_set_kind((theme_kind_t)tk);
			lv_screen_load(soundcloud_screen);
			lv_obj_update_layout(soundcloud_screen);
			assert_no_raw_soundcloud_keys(soundcloud_screen, LANGS[li]);
			assert_screen_no_overlaps(soundcloud_screen, "soundcloud_home", LANGS[li], tk);

			lv_screen_load(settings_scr);
			lv_obj_update_layout(settings_scr);
			assert_no_raw_soundcloud_keys(settings_scr, LANGS[li]);
			assert_screen_no_overlaps(settings_scr, "soundcloud_settings", LANGS[li], tk);

			lv_screen_load(list_scr);
			lv_obj_update_layout(list_scr);
			assert_no_raw_soundcloud_keys(list_scr, LANGS[li]);
			assert_screen_no_overlaps(list_scr, "list_screen", LANGS[li], tk);
		}
	}
	assert(lang_set("English"));
	theme_set_kind(THEME_KIND_DARK);

	// 4. Signed-in Home -> Playlists -> Playlist tracks
	lv_screen_load(soundcloud_screen);
	lv_obj_update_layout(soundcloud_screen);
	lv_obj_t *playlists_row = find_clickable_with_label(soundcloud_screen, tr("soundcloud_playlists"));
	assert(playlists_row != NULL);
	lv_obj_send_event(playlists_row, LV_EVENT_CLICKED, NULL);
	pump_until_idle("playlists");

	lv_obj_t *pl_item = find_clickable_with_label(lv_screen_active(), "Late Night Electronic");
	assert(pl_item != NULL);
	lv_obj_send_event(pl_item, LV_EVENT_CLICKED, NULL);
	pump_until_idle("playlist tracks");
	assert(find_clickable_with_label(lv_screen_active(), "Solar Wind") != NULL);

	// 5. Forget account clears session & keeps signed-out rows non-overlapping across themes
	lv_screen_load(settings_scr);
	lv_obj_update_layout(settings_scr);
	lv_obj_t *forget_row = find_clickable_with_label(settings_scr, tr("soundcloud_forget_account"));
	assert(forget_row != NULL);
	lv_obj_send_event(forget_row, LV_EVENT_CLICKED, NULL);
	assert(confirm_state.open);
	confirm_state.open = false;
	confirm_state.on_ok(confirm_state.user);
	assert(!stub_has_acc);
	assert_screen_no_overlaps(settings_scr, "soundcloud_settings_forgotten", "English", THEME_KIND_DARK);
	lv_screen_load(soundcloud_screen);
	assert_screen_no_overlaps(soundcloud_screen, "soundcloud_home_forgotten", "English", THEME_KIND_DARK);

	nftw(tmp, unlink_cb, 32, FTW_DEPTH | FTW_PHYS);
	printf("PASS soundcloud_pages (240x320, 11 languages x 5 themes zero-overlap, discreet spinner, "
		   "placeholder->opus handoff, settings & forget)\n");
	return 0;
}
