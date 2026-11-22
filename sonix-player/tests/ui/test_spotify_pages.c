/*
 * Headless LVGL test for the Spotify pages (`spotifypage.c`) at V1 dimensions:
 * 240x320, all 11 shipped languages, all 5 built-in themes.
 *
 * What is asserted is what a person sees and can press. The client itself is
 * exercised in tests/test_spotify.c against a fake daemon; here every call into
 * it is a stub that answers from a small fixed library, so the pages can be
 * laid out, navigated and measured without a socket, a network, an account or
 * a DAC.
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
#include "src/gui/streaming/spotifypage.h"
#include "src/gui/wireless/wifisettings.h"
#include "src/system/audio/audio.h"
#include "src/system/core/config.h"
#include "src/system/core/lang.h"
#include "src/system/device/system.h"
#include "src/system/net/wifi.h"
#include "src/system/playback/device_state.h"
#include "src/system/playback/playlist.h"
#include "src/system/streaming/spotify.h"

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

/* ---- the snapshot the page is drawn from --------------------------------- */

static spotify_status_t g_status;
static unsigned g_serial;
static int g_wifi_state = 3; /* WIFI_STATE_CONNECTED */

static void fixture_daemon_connected(void) {
	memset(&g_status, 0, sizeof(g_status));
	g_status.connected = true;
	g_status.logged_in = true;
	g_status.state = SPOTIFY_STATE_PLAYING;
	g_status.position_ms = 42000;
	g_status.volume = 80;
	snprintf(g_status.account, sizeof(g_status.account), "sonix");
	snprintf(g_status.backend, sizeof(g_status.backend), "daemon");
	snprintf(g_status.track.id, sizeof(g_status.track.id), "trk01");
	snprintf(g_status.track.title, sizeof(g_status.track.title), "Blue Hour");
	snprintf(g_status.track.artist, sizeof(g_status.track.artist), "The Cardamom Set");
	snprintf(g_status.track.album, sizeof(g_status.track.album), "Blue Hour");
	g_status.track.duration_ms = 247000;
	g_status.track.index = 1;
	g_status.track.count = 12;
}

static void fixture_mock(void) {
	fixture_daemon_connected();
	snprintf(g_status.backend, sizeof(g_status.backend), "mock");
	snprintf(g_status.account, sizeof(g_status.account), "Demo library");
}

static void fixture_offline(void) {
	memset(&g_status, 0, sizeof(g_status));
	snprintf(g_status.backend, sizeof(g_status.backend), "daemon");
}

/* ---- Spotify client stubs ------------------------------------------------ */

static bool g_init_called;
static int g_play_context_calls;
static char g_last_play_kind[32];
static char g_last_play_context[SPOTIFY_ID_MAX];
static char g_last_play_track[SPOTIFY_ID_MAX];
static int g_set_shuffle_calls;
static bool g_last_shuffle;
static int g_liked_calls;
static int g_playlist_calls;
static int g_playlists_calls;
static int g_search_calls;
static char g_last_search[SPOTIFY_NAME_MAX];

bool spotify_init(void) {
	g_init_called = true;
	return true;
}
void spotify_shutdown(void) {}
spotify_backend_t spotify_backend(void) { return SPOTIFY_BACKEND_DAEMON; }
// The address the setup row shows, and the discoverer the row calls. The test
// drives both: g_daemon_address is what spotify_address_set() reports on, and
// spotify_discover() is a stub that either finds something or does not, so the
// page's two states can be checked without a network.
static char g_daemon_address[128];
static bool g_daemon_found;
void spotify_set_socket(const char *p) {
	snprintf(g_daemon_address, sizeof(g_daemon_address), "%s", p ? p : "");
}
const char *spotify_get_socket(void) { return g_daemon_address; }
bool spotify_address_set(void) { return g_daemon_address[0] != '\0' && g_daemon_address[0] != ':'; }
bool spotify_discover(void) {
	if (!g_daemon_found) {
		return false;
	}
	spotify_set_socket("192.168.1.20:6600");
	return true;
}
void spotify_set_command(const char *c) { (void)c; }
const char *spotify_get_command(void) { return ""; }

void spotify_status(spotify_status_t *out) {
	if (out) {
		*out = g_status;
		out->serial = g_serial;
	}
}
unsigned spotify_serial(void) { return g_serial; }

bool spotify_command(const char *c, char *o, size_t s, int t) {
	(void)c;
	(void)o;
	(void)s;
	(void)t;
	return true;
}

void spotify_play_track(const char *id) { (void)id; }
void spotify_play_pause(void) {}
void spotify_next(void) {}
void spotify_prev(void) {}
void spotify_seek_ms(int ms) { (void)ms; }
void spotify_set_shuffle(bool on) {
	g_set_shuffle_calls++;
	g_last_shuffle = on;
}
void spotify_set_repeat(int mode) { (void)mode; }
void spotify_set_volume(int percent) { (void)percent; }

void spotify_play_context(const char *kind, const char *context_id, const char *track_id) {
	g_play_context_calls++;
	snprintf(g_last_play_kind, sizeof(g_last_play_kind), "%s", kind ? kind : "");
	snprintf(g_last_play_context, sizeof(g_last_play_context), "%s", context_id ? context_id : "");
	snprintf(g_last_play_track, sizeof(g_last_play_track), "%s", track_id ? track_id : "");
}

static int fill_tracks(spotify_track_t *out, int capacity, int *count, const char *suffix) {
	static const char *const TITLES[] = {"Northern Lines", "Blue Hour", "Paper Lanterns"};
	static const char *const ARTISTS[] = {"Marta Vey", "The Cardamom Set", "Yuki Mori"};
	int n = capacity < 3 ? capacity : 3;
	for (int i = 0; i < n; i++) {
		memset(&out[i], 0, sizeof(out[i]));
		snprintf(out[i].id, sizeof(out[i].id), "trk%02d-%s", i, suffix);
		snprintf(out[i].title, sizeof(out[i].title), "%s", TITLES[i]);
		snprintf(out[i].artist, sizeof(out[i].artist), "%s", ARTISTS[i]);
		snprintf(out[i].album, sizeof(out[i].album), "%s", TITLES[i]);
		out[i].duration_ms = 200000 + i * 1000;
	}
	if (count) {
		*count = n;
	}
	return n;
}

bool spotify_fetch_liked(int offset, int limit, spotify_track_t *out, int capacity, int *count) {
	(void)offset;
	(void)limit;
	g_liked_calls++;
	fill_tracks(out, capacity, count, "liked");
	return true;
}

bool spotify_fetch_playlists(spotify_list_t *out, int capacity, int *count) {
	g_playlists_calls++;
	if (count) {
		*count = 0;
	}
	if (capacity < 2) {
		return true;
	}
	memset(out, 0, sizeof(out[0]) * 2);
	snprintf(out[0].id, sizeof(out[0].id), "pl01");
	snprintf(out[0].name, sizeof(out[0].name), "Late night");
	snprintf(out[0].owner, sizeof(out[0].owner), "sonix");
	out[0].total = 42;
	snprintf(out[1].id, sizeof(out[1].id), "pl02");
	snprintf(out[1].name, sizeof(out[1].name), "Morning");
	snprintf(out[1].owner, sizeof(out[1].owner), "friend");
	out[1].total = 7;
	if (count) {
		*count = 2;
	}
	return true;
}

bool spotify_fetch_playlist(const char *playlist_id, int offset, int limit, spotify_track_t *out, int capacity,
							int *count) {
	(void)playlist_id;
	(void)offset;
	(void)limit;
	g_playlist_calls++;
	fill_tracks(out, capacity, count, "playlist");
	return true;
}

bool spotify_search(const char *query, int offset, int limit, spotify_track_t *out, int capacity, int *count) {
	(void)offset;
	(void)limit;
	g_search_calls++;
	snprintf(g_last_search, sizeof(g_last_search), "%s", query ? query : "");
	fill_tracks(out, capacity, count, "search");
	return true;
}

bool spotify_fetch_queue(int *index, int *count) {
	if (index) {
		*index = 0;
	}
	if (count) {
		*count = 12;
	}
	return true;
}

bool spotify_stream_active(void) { return false; }
bool spotify_is_active(void) { return g_status.state == SPOTIFY_STATE_PLAYING; }
void spotify_stop(void) {}
void spotify_resume(void) {}

void spotify_get_now(spotify_now_t *out) {
	if (!out) {
		return;
	}
	memset(out, 0, sizeof(*out));
	out->active = g_status.state == SPOTIFY_STATE_PLAYING;
	out->state = g_status.state;
	snprintf(out->title, sizeof(out->title), "%s", g_status.track.title);
	snprintf(out->artist, sizeof(out->artist), "%s", g_status.track.artist);
	snprintf(out->album, sizeof(out->album), "%s", g_status.track.album);
	out->position_ms = g_status.position_ms;
	out->duration_ms = g_status.track.duration_ms;
	out->index = g_status.track.index;
	out->count = g_status.track.count;
}

bool spotify_cover_path(char *out, size_t out_size) {
	if (out && out_size) {
		out[0] = '\0';
	}
	return false;
}

/* ---- the rest of the player --------------------------------------------- */

static char card_root_buf[512];
const char *storage_sd_root(void) { return card_root_buf; }

lv_obj_t *player_screen;
lv_obj_t *wifisettings_screen;

static lv_obj_t *screen_stack[16];
static int screen_depth;

// The shell's history shortened, to the letter: coming back to the page just
// left is a step back and not a step forward, and the stack is bounded because
// the real one is (see history_push / screen_history_len in switcher.c). This
// matters here: the page pushes one entry per open, and the groups below open
// a page a dozen times over.
void switch_screen(lv_obj_t *target_screen) {
	lv_obj_t *current = lv_screen_active();
	if (!target_screen || target_screen == current) {
		return;
	}
	if (screen_depth > 0 && screen_stack[screen_depth - 1] == target_screen) {
		screen_depth--;
	} else if (screen_depth < 16) {
		screen_stack[screen_depth++] = current;
	}
	lv_screen_load(target_screen);
	lv_obj_update_layout(target_screen);
}

// The main menu is the root: nothing above it. The fixture returns here
// between groups, and so does the shell.
void switch_screen_home(lv_obj_t *root) {
	screen_depth = 0;
	lv_screen_load(root);
	lv_obj_update_layout(root);
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
	if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
		return;
	}
	switch_screen((lv_obj_t *)lv_event_get_user_data(e));
}

static int open_sheets;

void player_sheet_open(bool animate) {
	(void)animate;
	open_sheets++;
}

void player_pause_for_loading(void) {}
void player_sheet_attach_drag(lv_obj_t *obj, bool opening) {
	(void)obj;
	(void)opening;
}
bool player_sheet_drag_active(void) { return false; }
bool player_sheet_is_open(void) { return false; }
void player_refresh_now_playing(void) {}

// The page's worker hands a finished request back through this, from its own
// thread. The player's gui_post() puts the callback on the GUI thread's queue;
// running it inline here would have the worker mutating LVGL while the display
// is mid-refresh, which LVGL asserts against (correctly). So the fixture keeps
// the same queue the real one does, and pump() drains it.
typedef struct {
	void (*cb)(void *);
	void *user;
} post_t;

static post_t g_posts[64];
static int g_post_head;
static int g_post_tail;

bool gui_post(void (*cb)(void *), void *user) {
	int next = (g_post_tail + 1) % 64;
	if (next == g_post_head) {
		return false; // the real one drops a callback rather than growing
	}
	g_posts[g_post_tail].cb = cb;
	g_posts[g_post_tail].user = user;
	g_post_tail = next;
	return true;
}

static void drain_posts(void) {
	while (g_post_head != g_post_tail) {
		post_t p = g_posts[g_post_head];
		g_post_head = (g_post_head + 1) % 64;
		if (p.cb) {
			p.cb(p.user);
		}
	}
}

void fonts_register_change(void (*cb)(void)) { (void)cb; }
void bootlogo_set(int choice, bool dark) {
	(void)choice;
	(void)dark;
}
bool bootpic_present(void) { return false; }
void gui_notify_popup(const char *text) { (void)text; }
void toast_success(const char *text) { (void)text; }
void toast_error(const char *text) { (void)text; }
void toast_plain(const char *text) { (void)text; }
void toast_busy(const char *text) { (void)text; }
void toast_busy_end(void) {}
void toast_busy_dismissable(const char *text) { (void)text; }
bool toast_busy_showing(void) { return false; }
void toast_glyph(const lv_image_dsc_t *glyph, const char *text) {
	(void)glyph;
	(void)text;
}
void switcher_attach_back_gesture(lv_obj_t *screen) { (void)screen; }
bool switcher_back_drag_active(void) { return false; }
void switcher_set_back_guard(lv_obj_t *screen, bool (*handler)(void)) {
	(void)screen;
	(void)handler;
}
void switcher_set_back_guard_peek(lv_obj_t *screen, bool (*peek)(void)) {
	(void)screen;
	(void)peek;
}
bool coverflow_drag_active(void) { return false; }
const lv_image_dsc_t icon_pet = {0};

void device_state_stop(void) {}
void device_state_get(device_state_t *out) { memset(out, 0, sizeof(*out)); }
audio_status_t audio_get_status(void) { return AUDIO_STATUS_STOPPED; }
// Counting the stop is the point: starting a Spotify track has to take the DAC
// from whatever was playing locally, not merely pause it. A pause keeps the
// device handle open, and audio_external_begin() would then fail the open.
int g_audio_stops;
void audio_stop(void) { g_audio_stops++; }
void audio_get_current_file(char *out, size_t size) {
	if (out && size) {
		out[0] = '\0';
	}
}
void audio_get_progress(double *pos, double *dur) {
	if (pos) {
		*pos = 0;
	}
	if (dur) {
		*dur = 0;
	}
}
int playlist_count(void) { return 0; }
int playlist_current_index(void) { return -1; }
bool playlist_path_at(int index, char *out, size_t out_size) {
	(void)index;
	(void)out;
	(void)out_size;
	return false;
}
bool playlist_peek_auto(char *out, size_t size) {
	(void)out;
	(void)size;
	return false;
}

void wifi_get_status(wifi_status_t *out) {
	memset(out, 0, sizeof(*out));
	out->state = (wifi_state_t)g_wifi_state;
	snprintf(out->ssid, sizeof(out->ssid), "Home");
}

void confirm_show(const char *title, const char *message, const char *ok_label, void (*on_ok)(void *user), void *user) {
	(void)title;
	(void)message;
	(void)ok_label;
	(void)on_ok;
	(void)user;
}

/* ---- layout helpers ----------------------------------------------------- */

static bool areas_overlap(const lv_area_t *a, const lv_area_t *b) {
	return !(a->x2 < b->x1 || b->x2 < a->x1 || a->y2 < b->y1 || b->y2 < a->y1);
}

typedef struct {
	lv_area_t area;
	char desc[64];
} leaf_box_t;

static void collect_leaves(lv_obj_t *obj, leaf_box_t *out, int *count, int cap) {
	uint32_t n = lv_obj_get_child_count(obj);
	for (uint32_t i = 0; i < n && *count < cap; i++) {
		lv_obj_t *child = lv_obj_get_child(obj, (int32_t)i);
		if (!child || lv_obj_is_hidden(child)) {
			continue;
		}
		if (lv_obj_get_child_count(child) == 0) {
			lv_obj_get_coords(child, &out[*count].area);
			const char *txt = lv_obj_check_type(child, &lv_label_class) ? lv_label_get_text(child) : "";
			snprintf(out[*count].desc, sizeof(out[*count].desc), "%s", txt ? txt : "");
			(*count)++;
			continue;
		}
		collect_leaves(child, out, count, cap);
	}
}

// Every visible card on the page: inside the screen, its own labels inside it,
// and no two cards at the same level overlapping. This is what catches a
// translation that grew a label into its neighbour.
//
// The walk is per level, not across the whole tree: a page body contains the
// tiles and rows that are drawn on it, and a container and its own child always
// "overlap". Only siblings are compared, which is where a real collision shows.
// The shared keyboard's own geometry. Set by the test before the search page is
// checked; every object under it is the keyboard widget's, not the page's.
static lv_obj_t *g_keyboard_tray;

// How many one-letter labels live under this object: a keyboard panel holds
// dozens, a page holds none. Counting them finds the tray without the page
// having to hand out a pointer to its own widget.
static int count_key_labels(lv_obj_t *obj) {
	int total = 0;
	if (lv_obj_check_type(obj, &lv_label_class)) {
		const char *txt = lv_label_get_text(obj);
		if (txt && txt[0] && txt[1] == '\0') {
			return 1;
		}
	}
	uint32_t n = lv_obj_get_child_count(obj);
	for (uint32_t i = 0; i < n; i++) {
		total += count_key_labels(lv_obj_get_child(obj, (int32_t)i));
	}
	return total;
}

static bool under_keyboard(lv_obj_t *obj) {
	for (lv_obj_t *p = obj; p; p = lv_obj_get_parent(p)) {
		if (p == g_keyboard_tray) {
			return true;
		}
	}
	return false;
}

static void check_level(lv_obj_t *root, const char *screen_name, const char *lang_name) {
	lv_area_t scr = {0, 0, bp_screen_w() - 1, bp_screen_h() - 1};
	lv_area_t boxes[64];
	int box_count = 0;
	bool scrolls = lv_obj_get_scroll_dir(root) != LV_DIR_NONE;

	uint32_t n = lv_obj_get_child_count(root);
	for (uint32_t i = 0; i < n; i++) {
		lv_obj_t *child = lv_obj_get_child(root, (int32_t)i);
		if (!child || lv_obj_is_hidden(child) || under_keyboard(child)) {
			continue;
		}
		lv_area_t ca;
		lv_obj_get_coords(child, &ca);
		if (ca.x2 < ca.x1 || ca.y2 < ca.y1) {
			continue; // not laid out yet: nothing to check
		}
		// Children of a scrolling container are meant to run past the fold:
		// that is what scrolling is for. Everything else has to fit.
		if (!scrolls && (ca.x1 < scr.x1 - 1 || ca.x2 > scr.x2 + 1 || ca.y1 < scr.y1 - 1 ||
						 ca.y2 > scr.y2 + 1)) {
			fprintf(stderr, "[%s / %s] child %u overflows the screen [%d,%d..%d,%d]\n", screen_name, lang_name,
					(unsigned)i, (int)ca.x1, (int)ca.y1, (int)ca.x2, (int)ca.y2);
			assert(!"child overflows the screen");
		}
		if (box_count < 64) {
			boxes[box_count++] = ca;
		}

		// A card's labels belong inside it. A scrolling card is the page body
		// and holds the rows that scroll past the fold, so only its width
		// bounds them.
		leaf_box_t leaves[16];
		int lcount = 0;
		collect_leaves(child, leaves, &lcount, 16);
		for (int a = 0; a < lcount; a++) {
			if (leaves[a].area.x1 < ca.x1 - 1 || leaves[a].area.x2 > ca.x2 + 1 ||
				(!scrolls && (leaves[a].area.y1 < ca.y1 - 1 || leaves[a].area.y2 > ca.y2 + 1))) {
				fprintf(stderr, "[%s / %s] '%s' [%d,%d..%d,%d] overflows its card [%d,%d..%d,%d]\n", screen_name,
						lang_name, leaves[a].desc, (int)leaves[a].area.x1, (int)leaves[a].area.y1,
						(int)leaves[a].area.x2, (int)leaves[a].area.y2, (int)ca.x1, (int)ca.y1, (int)ca.x2,
						(int)ca.y2);
				assert(!"label overflows its card");
			}
			for (int b = a + 1; b < lcount; b++) {
				if (areas_overlap(&leaves[a].area, &leaves[b].area)) {
					fprintf(stderr,
							"[%s / %s] OVERLAP: '%s' [%d,%d..%d,%d] vs '%s' [%d,%d..%d,%d] card [%d,%d..%d,%d]\n",
							screen_name, lang_name, leaves[a].desc, (int)leaves[a].area.x1,
							(int)leaves[a].area.y1, (int)leaves[a].area.x2, (int)leaves[a].area.y2,
							leaves[b].desc, (int)leaves[b].area.x1, (int)leaves[b].area.y1,
							(int)leaves[b].area.x2, (int)leaves[b].area.y2, (int)ca.x1, (int)ca.y1,
							(int)ca.x2, (int)ca.y2);
					assert(!"overlapping labels inside a card");
				}
			}
		}
	}

	// Only inside a container. The screen's own children are composed on
	// purpose -- the offline card is drawn over the grid, which stays behind
	// it -- so an overlap there is the design, not a collision.
	bool composed = lv_obj_get_parent(root) == NULL;
	for (int a = 0; !composed && a < box_count; a++) {
		for (int b = a + 1; b < box_count; b++) {
			if (areas_overlap(&boxes[a], &boxes[b])) {
				fprintf(stderr, "[%s / %s] cards %d [%d,%d..%d,%d] and %d [%d,%d..%d,%d] overlap\n", screen_name,
						lang_name, a, (int)boxes[a].x1, (int)boxes[a].y1, (int)boxes[a].x2, (int)boxes[a].y2, b,
						(int)boxes[b].x1, (int)boxes[b].y1, (int)boxes[b].x2, (int)boxes[b].y2);
				assert(!"overlapping cards");
			}
		}
	}

	// Then the same question for each child's own children.
	for (uint32_t i = 0; i < n; i++) {
		lv_obj_t *child = lv_obj_get_child(root, (int32_t)i);
		if (child && !lv_obj_is_hidden(child) && !under_keyboard(child) && lv_obj_get_child_count(child) > 0) {
			check_level(child, screen_name, lang_name);
		}
	}
}

// Every label and every clickable ancestor's text, for when a tile cannot be
// found: the interesting question is whether the text is wrong, or the tile.
static void dump_home_labels(lv_obj_t *root) {
	lv_obj_t *stack[256];
	int top = 0;
	stack[top++] = root;
	while (top > 0) {
		lv_obj_t *cur = stack[--top];
		if (lv_obj_check_type(cur, &lv_label_class)) {
			fprintf(stderr, "   label [%s] hidden=%d\n", lv_label_get_text(cur), (int)lv_obj_is_hidden(cur));
		}
		for (uint32_t i = 0; i < lv_obj_get_child_count(cur) && top < 256; i++) {
			stack[top++] = lv_obj_get_child(cur, (int32_t)i);
		}
	}
}

static void assert_screen_no_overlaps(lv_obj_t *screen, const char *screen_name, const char *lang_name) {
	assert(screen != NULL);
	check_level(screen, screen_name, lang_name);
}

// A label showing a bare translation key is a missing key: every real string
// on this page is prose, and the keys are the only text with an underscore.
static void assert_no_raw_spotify_keys(lv_obj_t *root, const char *lang_name) {
	if (!root || lv_obj_is_hidden(root)) {
		return;
	}
	if (lv_obj_check_type(root, &lv_label_class)) {
		const char *txt = lv_label_get_text(root);
		if (txt && strncmp(txt, "spotify_", 8) == 0) {
			fprintf(stderr, "[%s] raw translation key on screen: '%s'\n", lang_name, txt);
			assert(!"raw translation key shown");
		}
		if (txt && strcmp(txt, "playlists") == 0) {
			fprintf(stderr, "[%s] raw translation key on screen: 'playlists'\n", lang_name);
			assert(!"raw translation key shown");
		}
	}
	uint32_t n = lv_obj_get_child_count(root);
	for (uint32_t i = 0; i < n; i++) {
		assert_no_raw_spotify_keys(lv_obj_get_child(root, (int32_t)i), lang_name);
	}
}

// A grid tile draws its caption with LV_LABEL_LONG_DOT, so a translation that
// does not fit ends in "...". The label still holds the whole string (the
// ellipsis is drawn, not stored in every case), but when LVGL does cut it the
// tile is still the tile the caller means: match the prefix up to the ellipsis.
static bool label_matches(const char *txt, const char *needle) {
	if (!txt || !needle) {
		return false;
	}
	if (strcmp(txt, needle) == 0) {
		return true;
	}
	size_t len = strlen(txt);
	if (len > 0 && strcmp(txt + len - (len >= 3 ? 3 : len), "...") == 0) {
		size_t keep = len - 3;
		return strncmp(txt, needle, keep) == 0 && strlen(needle) >= keep;
	}
	return false;
}

static lv_obj_t *find_clickable_with_label(lv_obj_t *root, const char *needle) {
	if (!root || lv_obj_is_hidden(root) || lv_obj_has_state(root, LV_STATE_DISABLED)) {
		return NULL;
	}
	if (lv_obj_check_type(root, &lv_label_class)) {
		const char *txt = lv_label_get_text(root);
		if (label_matches(txt, needle)) {
			lv_obj_t *cur = root;
			while (cur && lv_obj_get_parent(cur) != NULL && !lv_obj_is_clickable(cur)) {
				cur = lv_obj_get_parent(cur);
			}
			if (cur && lv_obj_get_parent(cur) != NULL && lv_obj_is_clickable(cur)) {
				return cur;
			}
		}
	}
	uint32_t cnt = lv_obj_get_child_count(root);
	for (uint32_t i = 0; i < cnt; i++) {
		lv_obj_t *hit = find_clickable_with_label(lv_obj_get_child(root, (int32_t)i), needle);
		if (hit) {
			return hit;
		}
	}
	return NULL;
}

static lv_obj_t *find_label_text(lv_obj_t *root, const char *needle) {
	if (!root || lv_obj_is_hidden(root)) {
		return NULL;
	}
	if (lv_obj_check_type(root, &lv_label_class)) {
		const char *txt = lv_label_get_text(root);
		if (txt && strcmp(txt, needle) == 0) {
			return root;
		}
	}
	uint32_t cnt = lv_obj_get_child_count(root);
	for (uint32_t i = 0; i < cnt; i++) {
		lv_obj_t *hit = find_label_text(lv_obj_get_child(root, (int32_t)i), needle);
		if (hit) {
			return hit;
		}
	}
	return NULL;
}

static lv_obj_t *find_textarea(lv_obj_t *root) {
	if (!root) {
		return NULL;
	}
	if (lv_obj_check_type(root, &lv_textarea_class)) {
		return root;
	}
	uint32_t cnt = lv_obj_get_child_count(root);
	for (uint32_t i = 0; i < cnt; i++) {
		lv_obj_t *hit = find_textarea(lv_obj_get_child(root, (int32_t)i));
		if (hit) {
			return hit;
		}
	}
	return NULL;
}

static lv_obj_t *find_switch(lv_obj_t *root) {
	if (!root) {
		return NULL;
	}
	if (lv_obj_check_type(root, &lv_switch_class)) {
		return root;
	}
	uint32_t cnt = lv_obj_get_child_count(root);
	for (uint32_t i = 0; i < cnt; i++) {
		lv_obj_t *hit = find_switch(lv_obj_get_child(root, (int32_t)i));
		if (hit) {
			return hit;
		}
	}
	return NULL;
}

static void dummy_flush_cb(lv_display_t *d, const lv_area_t *area, uint8_t *px_map) {
	(void)area;
	(void)px_map;
	lv_display_flush_ready(d);
}

// One turn of the page's poll, and the layout pass a redraw would do.
//
// The real milliseconds matter here and nowhere else in this file: the page's
// requests run on a worker thread of its own, and the result comes back through
// gui_post(), which this fixture runs inline on whichever thread called it.
// Pumping LVGL alone would race that thread; waiting on the clock lets it
// finish, and `ms` is generous enough that the assertions below are about the
// pages, not about scheduling.
static void pump_ms(int ms) {
	int waited = 0;
	for (;;) {
		lv_tick_inc(30);
		lv_timer_handler();
		drain_posts();
		if (waited >= ms) {
			break;
		}
		usleep(3000);
		waited += 3;
	}
	drain_posts();
	lv_obj_update_layout(lv_screen_active());
}

static void pump(void) { pump_ms(260); }

static int unlink_cb(const char *fpath, const struct stat *sb, int typeflag, struct FTW *ftwbuf) {
	(void)sb;
	(void)typeflag;
	(void)ftwbuf;
	return remove(fpath);
}

// The home page is the one screen a person sees first, and it was the one
// screen this test never measured: it checked that the four tile captions
// existed and stopped there. On the device the demo note was drawn across the
// bottom two tiles and the DEMO badge sat on top of "Playlists". Everything
// here is geometry, because existence was already true when it looked wrong.
//
// The note is allowed to be long -- it is a sentence -- but it has to sit
// somewhere that does not cover a tile, and it must not be silently hidden
// behind one either.
static void assert_home_is_clean(lv_obj_t *screen, gui_config_t *cfg, const char *lang_name) {
	assert(screen != NULL);
	// The screen bounds as signed numbers: lv_area_t is int32_t and cfg's sizes
	// are unsigned, and mixing them is a sign-compare warning in this build.
	const int32_t w = (int32_t)cfg->screen_width;
	const int32_t h = (int32_t)cfg->screen_height;

	// The heading is at the top and inside the screen.
	lv_obj_t *title = settingsrow_page_title(screen);
	assert(title != NULL);
	const char *title_text = lv_label_get_text(title);
	assert(title_text && strcmp(title_text, tr("spotify")) == 0);
	lv_area_t title_a;
	lv_obj_get_coords(title, &title_a);
	// Plain comparisons rather than lv_area_is_in(): this LVGL build does not
	// export the area helpers as public API, and the arithmetic is the point
	// anyway -- every failure here should print the numbers.
	if (title_a.x1 < 0 || title_a.y1 < 0 || title_a.x2 >= w || title_a.y2 >= h) {
		fprintf(stderr, "[%s] home heading off screen: [%d,%d..%d,%d]\n", lang_name, (int)title_a.x1,
				(int)title_a.y1, (int)title_a.x2, (int)title_a.y2);
		assert(!"home heading off screen");
	}

	// The grid starts below the heading, in full: a grid aligned to the top of
	// the screen would draw its first row over the title.
	lv_area_t grid_a;
	lv_obj_get_coords(spotify_home_grid_for_test(), &grid_a);
	if (grid_a.y1 < title_a.y2) {
		fprintf(stderr, "[%s] home grid starts at y=%d, above the heading (ends y=%d)\n", lang_name,
				(int)grid_a.y1, (int)title_a.y2);
		assert(!"home grid drawn over the heading");
	}

	// The demo badge and the note, when shown, are inside the screen and clear
	// of the tiles.
	lv_obj_t *notices[] = {spotify_home_demo_for_test(), spotify_home_note_for_test()};
	for (size_t i = 0; i < sizeof(notices) / sizeof(notices[0]); i++) {
		lv_obj_t *c = notices[i];
		if (!c || lv_obj_is_hidden(c)) {
			continue;
		}
		lv_area_t a;
		lv_obj_get_coords(c, &a);
		if (a.x1 < 0 || a.y1 < 0 || a.x2 >= w || a.y2 >= h) {
			fprintf(stderr, "[%s] home notice %zu is off screen: [%d,%d..%d,%d]\n", lang_name, i, (int)a.x1,
					(int)a.y1, (int)a.x2, (int)a.y2);
			assert(!"home notice off screen");
		}
		// A rectangle overlap, written out: these are inclusive coordinates,
		// so the rectangles meet when neither is entirely left of the other.
		bool overlaps = a.x1 <= grid_a.x2 && a.x2 >= grid_a.x1 && a.y1 <= grid_a.y2 && a.y2 >= grid_a.y1;
		if (overlaps) {
			fprintf(stderr, "[%s] home notice %zu [%d,%d..%d,%d] covers the tiles [%d,%d..%d,%d]\n",
					lang_name, i, (int)a.x1, (int)a.y1, (int)a.x2, (int)a.y2, (int)grid_a.x1,
					(int)grid_a.y1, (int)grid_a.x2, (int)grid_a.y2);
			assert(!"home notice drawn over the tiles");
		}
	}
}

/* ---- main --------------------------------------------------------------- */

int main(int argc, char **argv) {
	assert(argc >= 2 && "usage: test_spotify_pages <lang_dir>");
	const char *lang_dir = argv[1];

	char tmp[] = "/tmp/sonix-spotify-ui-XXXXXX";
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
	wifisettings_screen = lv_obj_create(NULL);
	lv_obj_t *settings_stub = wifisettings_screen;

	// The three marks the pages carry: A8, 32x32, recolourable in every palette
	// including OLED's white.
	assert(icon_spotify_mark.header.w == 32 && icon_spotify_mark.header.h == 32);
	assert(icon_spotify_mark.header.cf == LV_COLOR_FORMAT_A8);
	assert(icon_spotify_badge.header.w == 32 && icon_spotify_badge.header.h == 32);
	assert(icon_spotify_badge.header.cf == LV_COLOR_FORMAT_A8);
	assert(icon_spotify_menu.header.w == 32 && icon_spotify_menu.header.h == 32);

	// ---- 1. A signed-in daemon: the home grid, its four tiles, its note.
	fixture_daemon_connected();
	spotifypage_init(&cfg);
	assert(g_init_called);
	assert(spotify_screen != NULL);
	switch_screen(spotify_screen);
	pump();

	assert(find_clickable_with_label(spotify_screen, tr("spotify_liked")) != NULL);
	assert(find_clickable_with_label(spotify_screen, tr("playlists")) != NULL);
	assert(find_clickable_with_label(spotify_screen, tr("spotify_search")) != NULL);
	assert(find_clickable_with_label(spotify_screen, tr("spotify_account")) != NULL);
	// Connected: the account is named, and neither the DEMO badge nor the
	// offline card is on the page.
	assert(find_label_text(spotify_screen, "sonix") != NULL);
	assert(find_label_text(spotify_screen, tr("spotify_demo")) == NULL);
	assert(find_label_text(spotify_screen, tr("spotify_needs_wifi")) == NULL);
	assert_home_is_clean(spotify_screen, &cfg, "English");

	// ---- 2. Liked Songs: the list, the playing mark, and tapping a row.
	assert(spotifypage_open_liked());
	pump();
	assert(g_liked_calls >= 1);
	lv_obj_t *list_screen = lv_screen_active();
	assert(list_screen != spotify_screen);
	assert(find_label_text(list_screen, tr("spotify_liked")) != NULL);
	assert(find_clickable_with_label(list_screen, "Northern Lines") != NULL);
	assert(find_clickable_with_label(list_screen, "Blue Hour") != NULL);
	assert(find_clickable_with_label(list_screen, "Paper Lanterns") != NULL);
	assert_screen_no_overlaps(list_screen, "liked", "English");

	int sheets_before = open_sheets;
	int stops_before = g_audio_stops;
	lv_obj_t *row = find_clickable_with_label(list_screen, "Blue Hour");
	assert(row != NULL);
	lv_obj_send_event(row, LV_EVENT_CLICKED, NULL);
	pump();
	assert(g_play_context_calls == 1);
	// One PCM, one writer: tapping a Spotify track stops local playback
	// outright. A pause would leave the DAC handle open and the PCM thread's
	// audio_external_begin() would fail two seconds later, with the page
	// reporting that it could not open the output.
	assert(g_audio_stops == stops_before + 1);
	assert(strcmp(g_last_play_kind, "liked") == 0);
	assert(strcmp(g_last_play_track, "trk01-liked") == 0);
	assert(open_sheets == sheets_before + 1);

	// ---- 3. Playlists, then one of them, then a track inside it.
	switch_screen_home(spotify_screen);
	lv_obj_t *tile = find_clickable_with_label(spotify_screen, tr("playlists"));
	assert(tile != NULL);
	lv_obj_send_event(tile, LV_EVENT_CLICKED, NULL);
	pump();
	assert(g_playlists_calls >= 1);
	lv_obj_t *pl_screen = lv_screen_active();
	assert(pl_screen != spotify_screen);
	assert(find_clickable_with_label(pl_screen, "Late night") != NULL);
	assert(find_clickable_with_label(pl_screen, "Morning") != NULL);
	// Who made it and how long it is, which is how two playlists are told apart.
	assert(find_label_text(pl_screen, "sonix - 42 tracks") != NULL);
	assert_screen_no_overlaps(pl_screen, "playlists", "English");

	lv_obj_t *pl = find_clickable_with_label(pl_screen, "Late night");
	assert(pl != NULL);
	lv_obj_send_event(pl, LV_EVENT_CLICKED, NULL);
	pump();
	assert(g_playlist_calls >= 1);
	lv_obj_t *tracks_screen = lv_screen_active();
	assert(find_clickable_with_label(tracks_screen, "Northern Lines") != NULL);

	int context_before = g_play_context_calls;
	lv_obj_t *pl_row = find_clickable_with_label(tracks_screen, "Paper Lanterns");
	assert(pl_row != NULL);
	lv_obj_send_event(pl_row, LV_EVENT_CLICKED, NULL);
	pump();
	assert(g_play_context_calls == context_before + 1);
	assert(strcmp(g_last_play_kind, "playlist") == 0);
	assert(strcmp(g_last_play_context, "pl01") == 0);
	assert(strcmp(g_last_play_track, "trk02-playlist") == 0);

	// ---- 4. Search: the field, the keyboard, and the results' own list.
	switch_screen_home(spotify_screen);
	tile = find_clickable_with_label(spotify_screen, tr("spotify_search"));
	assert(tile != NULL);
	lv_obj_send_event(tile, LV_EVENT_CLICKED, NULL);
	pump();
	lv_obj_t *search_screen = lv_screen_active();
	assert(search_screen != spotify_screen);
	lv_obj_t *field = find_textarea(search_screen);
	assert(field != NULL);
	// The keyboard is visible here, and its keys are the shared widget's
	// geometry: the page is checked, the tray is not. Find it first.
	g_keyboard_tray = NULL;
	{
		uint32_t n = lv_obj_get_child_count(search_screen);
		for (uint32_t i = 0; i < n; i++) {
			lv_obj_t *kid = lv_obj_get_child(search_screen, (int32_t)i);
			if (kid && count_key_labels(kid) >= 26) {
				g_keyboard_tray = kid; // a QWERTY panel and its T9 twin: that is the tray
			}
		}
	}
	if (g_keyboard_tray == NULL) {
		fprintf(stderr, "search page children:");
		for (uint32_t i = 0; i < lv_obj_get_child_count(search_screen); i++) {
			lv_obj_t *kid = lv_obj_get_child(search_screen, (int32_t)i);
			fprintf(stderr, " %u(hidden=%d,kids=%u)", (unsigned)i, (int)lv_obj_is_hidden(kid),
					(unsigned)lv_obj_get_child_count(kid));
		}
		fprintf(stderr, "\n");
	}
	assert(g_keyboard_tray != NULL);
	assert_screen_no_overlaps(search_screen, "search", "English");
	g_keyboard_tray = NULL;

	lv_textarea_set_text(field, "northern");
	// The magnifier key runs the page's own submit, which starts the search
	// list. Pressing it through the keyboard's callback is the same path a
	// finger takes.
	lv_obj_t *search_key = NULL;
	{
		lv_obj_t *stack[128];
		int top = 0;
		stack[top++] = search_screen;
		while (top > 0) {
			lv_obj_t *cur = stack[--top];
			if (lv_obj_check_type(cur, &lv_button_class) && lv_obj_get_child_count(cur) == 1) {
				lv_obj_t *kid = lv_obj_get_child(cur, 0);
				if (kid && lv_obj_check_type(kid, &lv_image_class) && lv_image_get_src(kid) == &icon_search) {
					search_key = cur;
					break;
				}
			}
			for (uint32_t i = 0; i < lv_obj_get_child_count(cur) && top < 128; i++) {
				stack[top++] = lv_obj_get_child(cur, (int32_t)i);
			}
		}
	}
	assert(search_key != NULL);
	lv_obj_send_event(search_key, LV_EVENT_CLICKED, NULL);
	pump();
	assert(g_search_calls == 1);
	assert(strcmp(g_last_search, "northern") == 0);
	lv_obj_t *results_screen = lv_screen_active();
	assert(results_screen != search_screen);
	assert(find_clickable_with_label(results_screen, "Northern Lines") != NULL);

	int search_context = g_play_context_calls;
	lv_obj_t *hit = find_clickable_with_label(results_screen, "Blue Hour");
	assert(hit != NULL);
	lv_obj_send_event(hit, LV_EVENT_CLICKED, NULL);
	pump();
	assert(g_play_context_calls == search_context + 1);
	assert(strcmp(g_last_play_kind, "search") == 0);
	assert(strcmp(g_last_play_context, "northern") == 0);

	// ---- 5. Account: who signed in, the two modes, and the notice.
	switch_screen_home(spotify_screen);
	tile = find_clickable_with_label(spotify_screen, tr("spotify_account"));
	assert(tile != NULL);
	lv_obj_send_event(tile, LV_EVENT_CLICKED, NULL);
	pump();
	lv_obj_t *account_screen = lv_screen_active();
	assert(account_screen != spotify_screen);
	assert(find_label_text(account_screen, "sonix") != NULL);
	assert(find_label_text(account_screen, tr("spotify_quality")) != NULL);
	assert(find_label_text(account_screen, tr("spotify_shuffle")) != NULL);
	assert(find_label_text(account_screen, tr("spotify_repeat_mode")) != NULL);
	assert(find_label_text(account_screen, tr("spotify_repeat_off")) != NULL);
	assert_screen_no_overlaps(account_screen, "account", "English");

	// The shuffle switch is the session's, not a local flag: pressing it has to
	// reach the client.
	lv_obj_t *shuffle_switch = find_switch(account_screen);
	assert(shuffle_switch != NULL);
	lv_obj_add_state(shuffle_switch, LV_STATE_CHECKED);
	lv_obj_send_event(shuffle_switch, LV_EVENT_VALUE_CHANGED, NULL);
	assert(g_set_shuffle_calls == 1);
	assert(g_last_shuffle == true);

	// ---- 6. Offline: the card that says why, and the way out of it.
	fixture_offline();
	g_serial++;
	g_wifi_state = 0;
	lv_screen_load(spotify_screen);
	pump();
	lv_obj_t *wifi_button = find_clickable_with_label(spotify_screen, tr("wifisettings"));
	assert(wifi_button != NULL);
	lv_obj_send_event(wifi_button, LV_EVENT_CLICKED, NULL);
	assert(lv_screen_active() == settings_stub);
	g_wifi_state = 3;

	// ---- 7. The demo: the badge, and the sentence that says it is silent.
	fixture_mock();
	g_serial++;
	lv_screen_load(spotify_screen);
	pump();
	assert(find_label_text(spotify_screen, tr("spotify_demo")) != NULL);
	assert(find_label_text(spotify_screen, tr("spotify_demo_note")) != NULL);

	// ---- 8. Not signed in: the daemon answers but nobody is in it, which is
	// the state after installing the helper and before the one-time login.
	memset(&g_status, 0, sizeof(g_status));
	g_status.connected = true;
	snprintf(g_status.backend, sizeof(g_status.backend), "daemon");
	g_serial++;
	lv_screen_load(spotify_screen);
	pump();
	assert(find_label_text(spotify_screen, tr("spotify_sign_in_note")) != NULL);
	assert(find_label_text(spotify_screen, tr("spotify_demo")) == NULL);

	// ---- 9. All 11 languages, all 5 themes, every page: no raw key, nothing
	// overlapping, nothing off the screen.
	static const char *const LANGS[] = {
		"Deutsch", "English", "Español", "Français", "Italiano", "Japanese",
		"Nederlands", "Polski", "Português BR", "Russian", "Ukrainian",
	};
	fixture_daemon_connected();
	g_serial++;
	for (size_t li = 0; li < sizeof(LANGS) / sizeof(LANGS[0]); li++) {
		assert(lang_set(LANGS[li]));
		for (int tk = 0; tk < THEME_KIND_COUNT; tk++) {
			theme_set_kind((theme_kind_t)tk);

			switch_screen_home(spotify_screen);
			pump();
			assert_no_raw_spotify_keys(spotify_screen, LANGS[li]);
			assert_screen_no_overlaps(spotify_screen, "spotify_home", LANGS[li]);
			assert_home_is_clean(spotify_screen, &cfg, LANGS[li]);

			assert(spotifypage_open_liked());
			pump();
			assert_no_raw_spotify_keys(lv_screen_active(), LANGS[li]);
			assert_screen_no_overlaps(lv_screen_active(), "liked", LANGS[li]);

			switch_screen_home(spotify_screen);
			lv_obj_t *t = find_clickable_with_label(spotify_screen, tr("playlists"));
			if (!t) {
				fprintf(stderr, "no '%s' tile in %s; labels on the home screen:\n", tr("playlists"), LANGS[li]);
				dump_home_labels(spotify_screen);
			}
			assert(t != NULL);
			lv_obj_send_event(t, LV_EVENT_CLICKED, NULL);
			pump();
			assert_no_raw_spotify_keys(lv_screen_active(), LANGS[li]);
			assert_screen_no_overlaps(lv_screen_active(), "playlists", LANGS[li]);

			lv_screen_load(spotify_screen);
			t = find_clickable_with_label(spotify_screen, tr("spotify_account"));
			assert(t != NULL);
			lv_obj_send_event(t, LV_EVENT_CLICKED, NULL);
			pump();
			assert_no_raw_spotify_keys(lv_screen_active(), LANGS[li]);
			assert_screen_no_overlaps(lv_screen_active(), "account", LANGS[li]);
		}
	}
	// ---- 9b. The demo library in every language: the state the photo showed.
	// The demo note is the longest thing the page ever puts under the grid, so
	// if any translation is going to collide with the tiles, it is here.
	fixture_mock();
	g_serial++;
	assert(find_label_text(spotify_screen, tr("spotify_demo")) == NULL);
	for (size_t li = 0; li < sizeof(LANGS) / sizeof(LANGS[0]); li++) {
		assert(lang_set(LANGS[li]));
		for (int tk = 0; tk < THEME_KIND_COUNT; tk++) {
			theme_set_kind((theme_kind_t)tk);
			switch_screen_home(spotify_screen);
			pump();
			assert_no_raw_spotify_keys(spotify_screen, LANGS[li]);
			assert_screen_no_overlaps(spotify_screen, "spotify_home_demo", LANGS[li]);
			assert_home_is_clean(spotify_screen, &cfg, LANGS[li]);
			// The badge really is shown in this state; otherwise this pass
			// would be testing the connected page all over again.
			if (tk == 0) {
				assert(find_label_text(spotify_screen, tr("spotify_demo")) != NULL);
			}
		}
	}

	assert(lang_set("English"));
	theme_set_kind(THEME_KIND_DARK);

	// ---- 10. The geometry the pages were built for.
	assert(bp_is_tempotec_v1());
	assert(bp_screen_w() == 240);
	assert(bp_screen_h() == 320);

	printf("PASS spotify_pages (240x320, 11 languages x 5 themes zero-overlap, liked/playlists/search/account, "
		   "DEMO badge, offline card, sign-in note)\n");
	nftw(tmp, unlink_cb, 8, FTW_DEPTH | FTW_PHYS);
	return 0;
}
