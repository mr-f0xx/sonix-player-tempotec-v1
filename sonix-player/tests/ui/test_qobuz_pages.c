/*
 * Headless LVGL integration test for the Qobuz home page (`qobuzpage.c`) at V1
 * dimensions (240x320) across all 11 shipped languages and all 5 built-in
 * themes.
 *
 * The page used to be a column of five settings rows with a small accent glyph
 * on each. It is now the same tiled grid the SoundCloud, Radio and Subsonic
 * home pages draw: four cards, two by two, each with a picture and its caption
 * underneath -- the favourites, the account's own playlists and the genre
 * picker that leads to the new releases. What this test pins down is that the
 * grid never scrolls, that every card keeps its picture and its caption inside
 * itself in every language (the last caption is allowed two lines), that Dark
 * and Light show the illustrations while the flat themes show their own glyph,
 * and that the states without an account take the panel instead of leaving
 * dead cards above a sentence.
 *
 * The Qobuz client under the page is a stub (see the recording stubs below);
 * the page, the grid and the shell around them are the real ones.
 */
#define _GNU_SOURCE
#include <assert.h>
#include <ftw.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
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
#include "src/gui/streaming/qobuzpage.h"
#include "src/system/audio/audio.h"
#include "src/system/core/config.h"
#include "src/system/core/lang.h"
#include "src/system/core/utils.h"
#include "src/system/device/system.h"
#include "src/system/net/tls.h"
#include "src/system/net/wifi.h"
#include "src/system/net/wifitransfer.h"
#include "src/system/playback/device_state.h"
#include "src/system/playback/playlist.h"
#include "src/system/streaming/qobuz.h"
#include "src/gui/streaming/qobuzart.h"
#include "src/system/streaming/qobuzcache.h"
#include "src/system/streaming/qobuzdl.h"
#include "src/system/streaming/qobuzsync.h"
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
lv_obj_t *browser_screen;
lv_obj_t *wifisettings_screen;

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
	}
}

void switcher_set_back_guard(lv_obj_t *screen, bool (*handler)(void)) {
	if (back_guard_count < 4) {
		back_guards[back_guard_count].screen = screen;
		back_guards[back_guard_count].handler = handler;
		back_guard_count++;
	}
}

void switcher_set_back_guard_peek(lv_obj_t *screen, bool (*peek)(void)) {
	(void)screen;
	(void)peek;
}

void switcher_attach_back_gesture(lv_obj_t *screen) { (void)screen; }
bool switcher_back_drag_active(void) { return false; }

static int busy_toasts;

void toast_busy(const char *key) {
	(void)key;
	busy_toasts++;
}
void toast_busy_end(void) { busy_toasts--; }
void toast_success(const char *text) { (void)text; }
void toast_error(const char *text) { (void)text; }
void toast_plain(const char *text) { (void)text; }

static int popup_calls;
static int confirm_calls;

void gui_notify_popup(const char *text) {
	(void)text;
	popup_calls++;
}

void confirm_show(const char *title, const char *message, const char *ok_label, void (*on_ok)(void *user),
				  void *user) {
	(void)title;
	(void)message;
	(void)ok_label;
	(void)on_ok;
	(void)user;
	confirm_calls++;
}

bool gui_post(void (*cb)(void *), void *user) {
	(void)cb;
	(void)user;
	return true;
}

void fonts_register_change(void (*cb)(void)) { (void)cb; }
void bootlogo_set(int choice, bool dark) {
	(void)choice;
	(void)dark;
}
bool bootpic_present(void) { return false; }

bool coverflow_drag_active(void) { return false; }
const lv_image_dsc_t icon_pet = {0};
void player_sheet_attach_drag(lv_obj_t *obj, bool opening) {
	(void)obj;
	(void)opening;
}
bool player_sheet_drag_active(void) { return false; }
bool player_sheet_is_open(void) { return false; }
void player_sheet_open(bool animate) { (void)animate; }
void player_refresh_now_playing(void) {}

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
void device_state_stop(void) {}
void device_state_get(device_state_t *out) { memset(out, 0, sizeof(*out)); }
void device_state_add_prepare_cb(device_state_prepare_cb cb) { (void)cb; }
bool device_state_play_queue_index(int index) { return false; }
void device_state_play_list(const char *const *list, int count, int start_index) {}
void device_state_play_list_ordered(const char *const *list, int count, int start_index) {}
void audio_get_current_file(char *out, size_t size) { out[0] = '\0'; }
void audio_get_progress(double *pos, double *dur) {
	*pos = 0;
	*dur = 0;
}
void audio_stop(void) {}
void streamturn_abandon_all(void) {}
void streamturn_register_abandon(void (*cb)(void)) { (void)cb; }
#pragma GCC diagnostic pop

static int queued_count;
static int queued_start_idx = -1;

void playlist_load_paths(const char *const *list, int count, int start_index) {
	queued_count = count;
	queued_start_idx = start_index;
	(void)list;
}
int playlist_count(void) { return queued_count; }
int playlist_current_index(void) { return queued_start_idx; }
bool playlist_path_at(int index, char *out, size_t out_size) {
	(void)index;
	(void)out;
	(void)out_size;
	return false;
}
bool playlist_is_library_backed(void) { return false; }

/* ---- cover loader and art stubs ----------------------------------------- */

void cover_free(cover_image_t *img) {
	if (img) memset(img, 0, sizeof(*img));
}
void coverloader_start(void) {}
void coverloader_request(int slot, const char *path, int size) {
	(void)slot;
	(void)path;
	(void)size;
}
void coverloader_release(int slot) { (void)slot; }
bool coverloader_take(int slot, cover_image_t *out, bool *finished) {
	(void)slot;
	(void)out;
	if (finished) *finished = false;
	return false;
}
void qobuzart_set_root(const char *sd_root) { (void)sd_root; }
void qobuzart_request(const void *owner, const char *folder, int slot, const char *url, int size) {
	(void)owner;
	(void)folder;
	(void)slot;
	(void)url;
	(void)size;
}
void qobuzart_release(const void *owner, int slot) {
	(void)owner;
	(void)slot;
}
bool qobuzart_take(const void *owner, int slot, cover_image_t *out, bool *finished) {
	(void)owner;
	(void)slot;
	(void)out;
	if (finished) *finished = false;
	return false;
}
void qobuzart_clear(void) {}

/* ---- files and wifi ----------------------------------------------------- */

bool growfile_is_growing(const char *path) {
	(void)path;
	return false;
}
bool growfile_prefix_is_growing(const char *prefix) {
	(void)prefix;
	return false;
}
void browser_open_dir_public(const char *path) { (void)path; }
bool wifitransfer_get_enabled(void) { return false; }
void stockkeys_ui_populate_missing(lv_obj_t *home_container, void (*on_ready)(void)) {
	(void)home_container;
	(void)on_ready;
}

// The page asks the network itself, so the test answers for it.
static bool stub_connected = true;
void wifi_get_status(wifi_status_t *out) {
	memset(out, 0, sizeof(*out));
	if (stub_connected) {
		out->state = WIFI_STATE_CONNECTED;
		snprintf(out->ip, sizeof(out->ip), "192.168.1.24");
		snprintf(out->ssid, sizeof(out->ssid), "studio");
	} else {
		out->state = WIFI_STATE_OFF;
	}
}

/* ---- the Qobuz client, recorded rather than reached --------------------- */

static bool stub_configured = true;
static bool stub_logged_in = true;
static int stub_format = QOBUZ_FORMAT_CD;

void qobuz_init(void) {}
bool qobuz_configured(void) { return stub_configured; }
bool qobuz_logged_in(void) { return stub_logged_in; }
const char *qobuz_display_name(void) { return stub_logged_in ? "listener" : ""; }
bool qobuz_login(const char *username, const char *password) {
	(void)username;
	(void)password;
	return true;
}
void qobuz_logout(void) { stub_logged_in = false; }
const char *qobuz_last_error(void) { return ""; }
int qobuz_get_format(void) { return stub_format; }
void qobuz_set_format(int format_id) { stub_format = format_id; }
const char *qobuz_expected_mime(void) { return "audio/flac"; }

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
int qobuz_search_tracks(const char *query, int offset, qobuz_track_t *out, int max) { return 0; }
int qobuz_search_albums(const char *query, int offset, qobuz_album_t *out, int max) { return 0; }
int qobuz_search_artists(const char *query, int offset, qobuz_artist_t *out, int max) { return 0; }
int qobuz_album_tracks(const char *album_id, int offset, qobuz_track_t *out, int max) { return 0; }
int qobuz_artist_albums(long artist_id, int offset, qobuz_album_t *out, int max) { return 0; }
int qobuz_playlist_tracks(long playlist_id, int offset, qobuz_track_t *out, int max) { return 0; }
int qobuz_favorite_tracks(int offset, qobuz_track_t *out, int max) { return 0; }
int qobuz_favorite_albums(int offset, qobuz_album_t *out, int max) { return 0; }
int qobuz_user_playlists(int offset, qobuz_playlist_t *out, int max) { return 0; }
int qobuz_featured_albums(const char *type, int offset, qobuz_album_t *out, int max) { return 0; }
int qobuz_genre_new_releases(long genre_id, int offset, qobuz_album_t *out, int max) { return 0; }
int qobuz_genres(int offset, qobuz_genre_t *out, int max) { return 0; }
int qobuz_reggae_genre(qobuz_genre_t *out) { return 0; }
bool qobuz_track_url(long track_id, char *url, size_t url_size, char *mime, size_t mime_size, int *format_out) {
	return false;
}

void qobuzcache_set_root(const char *sd_root) { (void)sd_root; }
void qobuzcache_path(long track_id, const char *mime, char *out, size_t size) {
	if (out && size) out[0] = '\0';
}
void qobuzcache_set_slow_start_cb(void (*cb)(int seconds)) { (void)cb; }
void qobuzcache_set_protected(const char *const *paths, int count) {}
void qobuzcache_set_network_wanted(bool wanted) {}
bool qobuzcache_owns(const char *path) { return false; }
bool qobuzcache_find(long track_id, char *out, size_t size) { return false; }
const char *qobuzcache_last_error(void) { return ""; }
long qobuzcache_track_id(const char *path) { return 0; }
void qobuzcache_note_played(const char *path) {}
void qobuzcache_abandon_all(void) {}
long qobuzcache_downloading_id(void) { return 0; }
void qobuzcache_wait_idle(void) {}
bool qobuzcache_start(long track_id, const char *url, const char *mime, int duration_secs, char *out,
					  size_t size) {
	return false;
}
void qobuzcache_clear(void) {}
void qobuzcache_write_sidecars(long track_id, const char *mime, const char *title, const char *artist,
							   const char *album, const char *album_id, int track_number, const char *cover_url) {}

bool qobuzdl_track(const qobuz_track_t *track, int format) { return false; }
bool qobuzdl_album(const char *album_id, int format) { return false; }
bool qobuzdl_busy(void) { return false; }
void qobuzdl_progress(qobuzdl_progress_t *out) { memset(out, 0, sizeof(*out)); }
int qobuzdl_history(qobuzdl_entry_t *entries, int max) { return 0; }

void qobuzsync_forget(void) {}
bool qobuzsync_is_favorite(long track_id) { return false; }
void qobuzsync_favorite_toggle(long track_id, bool want, qobuzsync_done_cb done, void *user) {}
void qobuzsync_album_favorite(const char *album_id, bool want, qobuzsync_done_cb done, void *user) {}
void qobuzsync_playlist_remove(long playlist_id, qobuzsync_done_cb done, void *user) {}
#pragma GCC diagnostic pop

/* ---- widget-tree inspection helpers ------------------------------------- */

// No tag may reach the page as text: every caption is a key the language files
// answer for. The Qobuz keys all begin with a letter and an underscore, or are
// one of the four the page names itself, so the test checks the keys it knows.
static void assert_no_raw_qobuz_keys(lv_obj_t *root, const char *lang_name) {
	if (!root) return;
	if (lv_obj_check_type(root, &lv_label_class)) {
		const char *txt = lv_label_get_text(root);
		if (txt && (strncmp(txt, "qobuz_", 6) == 0 || strcmp(txt, "favourite_tracks") == 0 ||
					strcmp(txt, "favourite_albums") == 0 || strcmp(txt, "my_playlists") == 0 ||
					strcmp(txt, "new_releases_by_genre") == 0 || strcmp(txt, "sign_in") == 0 ||
					strcmp(txt, "wi_fi_settings") == 0)) {
			fprintf(stderr, "Untranslated key '%s' on screen in language '%s'\n", txt, lang_name);
			assert(!"raw translation key leaked to UI");
		}
	}
	uint32_t cnt = lv_obj_get_child_count(root);
	for (uint32_t i = 0; i < cnt; i++) {
		assert_no_raw_qobuz_keys(lv_obj_get_child(root, (int32_t)i), lang_name);
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
	uint32_t n = lv_obj_get_child_count(node);
	for (uint32_t i = 0; i < n; i++) {
		collect_card_leaves(lv_obj_get_child(node, (int32_t)i), out, count, max);
	}
}

// The card container: the one child of the screen that is neither the heading
// nor a corner button, and that carries children of its own. The page keeps a
// second, empty one for the states without an account, so an empty container is
// never mistaken for the page's body.
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
						"[%s / %s / theme %d] page title '%s' [%d,%d..%d,%d] overlaps corner button "
						"[%d,%d..%d,%d]\n",
						screen_name, lang_name, theme_kind, lv_label_get_text(title), (int)ta.x1, (int)ta.y1,
						(int)ta.x2, (int)ta.y2, (int)ba.x1, (int)ba.y1, (int)ba.x2, (int)ba.y2);
				assert(!"page title overlaps corner button");
			}
		}
	}

	// 2. Every card must keep its picture and its caption inside itself, and the
	// two must never overlap each other or a neighbour.
	lv_obj_t *container = screen_container(screen);
	if (!container) return;

	uint32_t card_cnt = lv_obj_get_child_count(container);
	for (uint32_t r = 0; r < card_cnt; r++) {
		lv_obj_t *card = lv_obj_get_child(container, (int32_t)r);
		// A settings row's explanatory note is a loose label in the container
		// rather than a card, so it is not one of these.
		if (!card || lv_obj_is_hidden(card) || lv_obj_check_type(card, &lv_label_class)) continue;
		lv_area_t ca;
		lv_obj_get_coords(card, &ca);
		if (ca.x1 < 0 || ca.x2 > bp_screen_w() + 1) {
			fprintf(stderr, "[%s / %s / theme %d] card %u overflows 240px screen [%d..%d]\n", screen_name,
					lang_name, theme_kind, (unsigned)r, (int)ca.x1, (int)ca.x2);
			assert(!"card horizontal overflow");
		}

		leaf_box_t leaves[32];
		int lcount = 0;
		collect_card_leaves(card, leaves, &lcount, 32);
		for (int i = 0; i < lcount; i++) {
			if (leaves[i].area.x1 < ca.x1 - 1 || leaves[i].area.x2 > ca.x2 + 1 ||
				leaves[i].area.y1 < ca.y1 - 1 || leaves[i].area.y2 > ca.y2 + 1) {
				fprintf(stderr,
						"[%s / %s / theme %d] card %u child '%s' [%d,%d..%d,%d] overflows card "
						"[%d,%d..%d,%d]\n",
						screen_name, lang_name, theme_kind, (unsigned)r, leaves[i].desc, (int)leaves[i].area.x1,
						(int)leaves[i].area.y1, (int)leaves[i].area.x2, (int)leaves[i].area.y2, (int)ca.x1,
						(int)ca.y1, (int)ca.x2, (int)ca.y2);
				assert(!"child overflows card bounds");
			}
			for (int j = i + 1; j < lcount; j++) {
				if (areas_overlap(&leaves[i].area, &leaves[j].area)) {
					fprintf(stderr,
							"[%s / %s / theme %d] card %u OVERLAP: '%s' [%d,%d..%d,%d] vs '%s' "
							"[%d,%d..%d,%d]\n",
							screen_name, lang_name, theme_kind, (unsigned)r, leaves[i].desc,
							(int)leaves[i].area.x1, (int)leaves[i].area.y1, (int)leaves[i].area.x2,
							(int)leaves[i].area.y2, leaves[j].desc, (int)leaves[j].area.x1,
							(int)leaves[j].area.y1, (int)leaves[j].area.x2, (int)leaves[j].area.y2);
					assert(!"overlapping widgets inside card");
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

static lv_obj_t *find_label_by_text(lv_obj_t *root, const char *text) {
	if (lv_obj_check_type(root, &lv_label_class) && strcmp(lv_label_get_text(root), text) == 0) {
		return root;
	}
	uint32_t cnt = lv_obj_get_child_count(root);
	for (uint32_t i = 0; i < cnt; i++) {
		lv_obj_t *hit = find_label_by_text(lv_obj_get_child(root, (int32_t)i), text);
		if (hit) {
			return hit;
		}
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

static const char *const HOME_KEYS[] = {
	"favourite_tracks",
	"favourite_albums",
	"my_playlists",
	"new_releases_by_genre",
};

int main(int argc, char **argv) {
	assert(argc >= 2 && "usage: test_qobuz_pages <lang_dir>");
	const char *lang_dir = argv[1];

	char tmp[] = "/tmp/sonix-qobuz-ui-XXXXXX";
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
	// gui.c creates these screens before it inits the pages; the test stands in
	// for it.
	qobuz_screen = lv_obj_create(NULL);
	qobuz_list_screen = lv_obj_create(NULL);
	wifisettings_screen = lv_obj_create(NULL);

	// The pictures are rendered at exactly the size the cards draw them, so the
	// page never asks LVGL to resample one.
	assert(icon_menu_qobuz_favourites.header.w == 54 && icon_menu_qobuz_favourites.header.h == 54);
	assert(icon_menu_qobuz_albums.header.w == 54 && icon_menu_qobuz_albums.header.h == 54);
	assert(icon_menu_qobuz_playlists.header.w == 54 && icon_menu_qobuz_playlists.header.h == 54);
	assert(icon_menu_qobuz_genres.header.w == 54 && icon_menu_qobuz_genres.header.h == 54);
	assert(icon_qobuz_favourites_glyph.header.w == 46 && icon_qobuz_favourites_glyph.header.h == 46);
	assert(icon_qobuz_albums_glyph.header.w == 46 && icon_qobuz_albums_glyph.header.h == 46);
	assert(icon_qobuz_playlists_glyph.header.w == 46 && icon_qobuz_playlists_glyph.header.h == 46);
	assert(icon_qobuz_genres_glyph.header.w == 46 && icon_qobuz_genres_glyph.header.h == 46);

	qobuzpage_init(&cfg);
	assert(qobuz_screen != NULL);
	switch_screen(qobuz_screen);
	lv_timer_handler();
	lv_obj_update_layout(qobuz_screen);

	// 1. Four tiles, two by two, each carrying its own destination. The fifth
	// destination the page used to have -- the global new-release feed -- is
	// gone: it is reached through a genre, so the genre picker is the fourth.
	lv_obj_t *grid = screen_container(qobuz_screen);
	assert(grid != NULL);
	assert(lv_obj_get_child_count(grid) == 4);
	for (int i = 0; i < 4; i++) {
		assert(find_clickable_with_label(qobuz_screen, tr(HOME_KEYS[i])) != NULL);
	}
	assert(find_clickable_with_label(qobuz_screen, tr("new_releases")) == NULL);

	// Nothing to scroll to: the grid is sized from the space under the heading,
	// and the page body the other states use takes no space at all.
	assert(lv_obj_get_scroll_top(grid) == 0);
	assert(lv_obj_get_scroll_bottom(grid) == 0);
	assert(lv_obj_get_scroll_left(grid) == 0);
	assert(lv_obj_get_scroll_right(grid) == 0);

	// Every caption is given two lines, which is what keeps "New releases by
	// genre" whole instead of an ellipsis after the first line.
	for (int i = 0; i < 4; i++) {
		lv_obj_t *tile = lv_obj_get_child(grid, i);
		lv_obj_t *label = lv_obj_get_child(tile, 1);
		assert(label != NULL);
		assert(lv_obj_get_height(label) >= 2 * lv_font_get_line_height(bp_tile_label_font()));
	}

	// 2. Dark and Light show the illustrations; the flat themes show the glyph
	// in their own accent instead.
	static const lv_image_dsc_t *const ART[4] = {&icon_menu_qobuz_favourites, &icon_menu_qobuz_albums,
												 &icon_menu_qobuz_playlists, &icon_menu_qobuz_genres};
	static const lv_image_dsc_t *const GLYPHS[4] = {&icon_qobuz_favourites_glyph, &icon_qobuz_albums_glyph,
													&icon_qobuz_playlists_glyph, &icon_qobuz_genres_glyph};
	theme_set_kind(THEME_KIND_DARK);
	lv_obj_update_layout(qobuz_screen);
	for (int i = 0; i < 4; i++) {
		assert(tree_has_visible_glyph(qobuz_screen, ART[i]));
		assert(!tree_has_visible_glyph(qobuz_screen, GLYPHS[i]));
	}
	theme_set_kind(THEME_KIND_AMBER_ON_BLACK);
	lv_obj_update_layout(qobuz_screen);
	for (int i = 0; i < 4; i++) {
		assert(tree_has_visible_glyph(qobuz_screen, GLYPHS[i]));
		assert(!tree_has_visible_glyph(qobuz_screen, ART[i]));
	}
	theme_set_kind(THEME_KIND_DARK);
	lv_obj_update_layout(qobuz_screen);

	// 3. Signed out, the cards give way to the "Sign in" row: a grid of
	// destinations nothing can open is worse than the one row that fixes it.
	stub_logged_in = false;
	lv_obj_send_event(qobuz_screen, LV_EVENT_SCREEN_LOADED, NULL);
	lv_obj_update_layout(qobuz_screen);
	assert(lv_obj_is_hidden(grid));
	assert(find_clickable_with_label(qobuz_screen, tr("sign_in")) != NULL);
	assert(find_clickable_with_label(qobuz_screen, tr(HOME_KEYS[0])) == NULL);
	stub_logged_in = true;

	// 4. With no network the sign-in row would lead nowhere, so the notice and
	// the Wi-Fi row take its place.
	stub_connected = false;
	lv_obj_send_event(qobuz_screen, LV_EVENT_SCREEN_LOADED, NULL);
	lv_obj_update_layout(qobuz_screen);
	assert(lv_obj_is_hidden(grid));
	assert(find_clickable_with_label(qobuz_screen, tr("wi_fi_settings")) != NULL);
	assert(find_clickable_with_label(qobuz_screen, tr("sign_in")) == NULL);
	// The notice leads the page from the top of the panel, where the tiles
	// would start -- not floating in the middle of an empty page.
	lv_obj_t *notice = find_label_by_text(qobuz_screen, tr("enable_wi_fi_first"));
	assert(notice != NULL);
	lv_area_t notice_area;
	lv_obj_get_coords(notice, &notice_area);
	assert(notice_area.y1 < (int32_t)cfg.screen_height / 3);
	stub_connected = true;

	// 5. Ready again: the tiles come back and the row goes away.
	lv_obj_send_event(qobuz_screen, LV_EVENT_SCREEN_LOADED, NULL);
	lv_obj_update_layout(qobuz_screen);
	assert(!lv_obj_is_hidden(grid));
	assert(find_clickable_with_label(qobuz_screen, tr("wi_fi_settings")) == NULL);
	assert(find_clickable_with_label(qobuz_screen, tr("sign_in")) == NULL);

	// 6. Sweep all 11 shipped languages and all 5 built-in themes across the
	// home page, asserting BOTH zero raw translation keys AND zero overlaps.
	static const char *const LANGS[] = {
		"Deutsch",  "English",   "Español", "Français", "Italiano", "Japanese",
		"Nederlands", "Polski", "Português BR", "Russian", "Ukrainian",
	};
	for (size_t li = 0; li < sizeof(LANGS) / sizeof(LANGS[0]); li++) {
		assert(lang_set(LANGS[li]));
		for (int tk = 0; tk < THEME_KIND_COUNT; tk++) {
			theme_set_kind((theme_kind_t)tk);
			lv_screen_load(qobuz_screen);
			lv_obj_update_layout(qobuz_screen);
			assert_no_raw_qobuz_keys(qobuz_screen, LANGS[li]);
			assert_screen_no_overlaps(qobuz_screen, "qobuz_home", LANGS[li], tk);
			assert(lv_obj_get_scroll_bottom(grid) == 0);
			assert(lv_obj_get_child_count(grid) == 4);
			for (int i = 0; i < 4; i++) {
				// Every card is still a destination, and its caption is still
				// a translation rather than a tag. How much of a long word
				// the two lines can hold is the grid's business (see
				// gridpage.c): a caption that does not fit ends in an
				// ellipsis, as it does on the pages this one follows.
				lv_obj_t *tile = lv_obj_get_child(grid, i);
				assert(tile != NULL && lv_obj_is_clickable(tile));
				lv_obj_t *caption = lv_obj_get_child(tile, 1);
				assert(caption != NULL);
				assert(lv_label_get_text(caption) != NULL && lv_label_get_text(caption)[0] != '\0');
			}
		}
	}
	// The notice has to fit the panel too, in its longest translation.
	assert(lang_set("Deutsch"));
	theme_set_kind(THEME_KIND_DARK);
	stub_connected = false;
	lv_obj_send_event(qobuz_screen, LV_EVENT_SCREEN_LOADED, NULL);
	lv_obj_update_layout(qobuz_screen);
	assert_screen_no_overlaps(qobuz_screen, "qobuz_home_offline", "Deutsch", THEME_KIND_DARK);
	stub_connected = true;
	lv_obj_send_event(qobuz_screen, LV_EVENT_SCREEN_LOADED, NULL);

	assert(lang_set("English"));
	theme_set_kind(THEME_KIND_DARK);

	printf("PASS qobuz_pages (240x320, 11 languages x 5 themes zero-overlap, four tiles, "
		   "no scrolling, sign-in and offline states)\n");

	nftw(tmp, unlink_cb, 64, FTW_DEPTH | FTW_PHYS);
	return 0;
}
