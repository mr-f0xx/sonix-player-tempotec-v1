/*
 * Headless LVGL integration test for the Radio home page (`radiopage.c`) at V1
 * dimensions (240x320) across all 11 shipped languages and all 5 built-in
 * themes.
 *
 * The page is the same tiled grid the SoundCloud and Subsonic home pages draw:
 * four cards, two by two, each with a picture and its caption underneath. What
 * this test pins down is that the grid never scrolls, that every card keeps its
 * picture and its caption inside itself in every language, that Dark and Light
 * show the illustrations while the flat themes show their own glyph, and that
 * with no network the tiles give way to the notice and the Wi-Fi row.
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
#include "src/gui/streaming/radiopage.h"
#include "src/system/audio/audio.h"
#include "src/system/core/config.h"
#include "src/system/core/lang.h"
#include "src/system/core/utils.h"
#include "src/system/device/system.h"
#include "src/system/net/tls.h"
#include "src/system/playback/device_state.h"
#include "src/system/playback/playlist.h"
#include "src/system/streaming/radio.h"
#include "src/system/net/wifi.h"
#include "src/system/device/power.h"
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

void toast_busy(const char *key) {
	(void)key;
	busy_toasts++;
}
void toast_busy_end(void) {
	if (busy_toasts > 0) busy_toasts--;
}

static int popup_calls;

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
}

int playlist_count(void) { return queued_count; }
int playlist_current_index(void) { return queued_start_idx; }
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
	(void)index;
	return true;
}

void device_state_play_list(const char *const *list, int count, int start_index) {
	playlist_load_paths(list, count, start_index);
}

void device_state_play_file(const char *path) { (void)path; }

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
bool player_sheet_is_open(void) { return false; }
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
void audio_stop(void) {}
void streamturn_abandon_all(void) {}
void streamturn_register_abandon(void (*cb)(void)) { (void)cb; }
#pragma GCC diagnostic pop

/* ---- stubs the radio page asks for outside its own module --------------- */

lv_obj_t *wifisettings_screen;

// The page asks the radio itself, so the test answers for it: connected or
// not, and the offline half of the hub follows.
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

static bool stub_index_ready = true;

// The A-Z index parks its redraw timer with the standby handler; the test has
// no power domain to park it in.
void power_pause_in_standby(lv_timer_t *timer) { (void)timer; }

void library_sort_key(const char *name, char *out, size_t out_size) {
	snprintf(out, out_size, "%s", name ? name : "");
}
char library_index_letter_of_key(const char *key) {
	(void)key;
	return '#';
}
int library_index_slot(char letter) {
	(void)letter;
	return stub_index_ready ? 0 : -1;
}

/* ---- recording radio client stubs --------------------------------------- */

static radio_station_t stub_station;
static radio_term_t stub_term;

void radio_init(void) {}
void radio_request_terms(radio_browse_t kind) { (void)kind; }
void radio_request_stations(radio_browse_t kind, const char *value, int offset) {
	(void)kind;
	(void)value;
	(void)offset;
}
void radio_request_search(const char *text, int offset) {
	(void)text;
	(void)offset;
}
radio_job_t radio_job_state(void) { return RADIO_JOB_IDLE; }
unsigned radio_job_serial(void) { return 0; }
int radio_get_terms(radio_term_t *out, int max) {
	if (out && max > 0) {
		out[0] = stub_term;
		return 1;
	}
	return 0;
}
int radio_get_stations(radio_station_t *out, int max) {
	if (out && max > 0) {
		out[0] = stub_station;
		return 1;
	}
	return 0;
}
int radio_last_raw_count(void) { return 1; }
int radio_fav_count(void) { return 0; }
bool radio_fav_get(int index, radio_station_t *out) {
	(void)index;
	(void)out;
	return false;
}
void radio_fav_remove_station(const radio_station_t *station) { (void)station; }
const char *radio_station_problem(const radio_station_t *station) {
	(void)station;
	return NULL;
}
int radio_recent_count(void) { return 0; }
bool radio_recent_get(int index, radio_station_t *out) {
	(void)index;
	(void)out;
	return false;
}
void radio_custom_reload(void) {}
int radio_custom_count(void) { return 0; }
bool radio_custom_get(int index, radio_station_t *out) {
	(void)index;
	(void)out;
	return false;
}
bool radio_custom_heading(int index, char *out, size_t out_size) {
	(void)index;
	(void)out;
	(void)out_size;
	return false;
}
bool radio_custom_file_present(void) { return false; }
int radio_custom_current_index(void) { return -1; }
void radio_set_list(const radio_station_t *list, int count) {
	(void)list;
	(void)count;
}
bool radio_play(const radio_station_t *station) {
	(void)station;
	return true;
}
bool radio_current_station(radio_station_t *out) {
	(void)out;
	return false;
}

/* ---- widget-tree inspection helpers ------------------------------------- */

static void assert_no_raw_radio_keys(lv_obj_t *root, const char *lang_name) {
	if (!root) return;
	if (lv_obj_check_type(root, &lv_label_class)) {
		const char *txt = lv_label_get_text(root);
		if (txt && strncmp(txt, "radio_", 6) == 0) {
			fprintf(stderr, "Untranslated key '%s' on screen in language '%s'\n", txt, lang_name);
			assert(!"raw radio_* translation key leaked to UI");
		}
	}
	uint32_t cnt = lv_obj_get_child_count(root);
	for (uint32_t i = 0; i < cnt; i++) {
		assert_no_raw_radio_keys(lv_obj_get_child(root, (int32_t)i), lang_name);
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
// nor a corner button.
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
	"language",
	"radio_country",
	"genre",
	"radio_flylo_favs",
};

int main(int argc, char **argv) {
	assert(argc >= 2 && "usage: test_radio_pages <lang_dir>");
	const char *lang_dir = argv[1];

	char tmp[] = "/tmp/sonix-radio-ui-XXXXXX";
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
	radiopage_screen = lv_obj_create(NULL);
	radiolist_screen = lv_obj_create(NULL);
	radiosearch_screen = lv_obj_create(NULL);
	wifisettings_screen = lv_obj_create(NULL);

	// The pictures are rendered at exactly the size the cards draw them, so the
	// page never asks LVGL to resample one.
	assert(icon_menu_radio_language.header.w == 48 && icon_menu_radio_language.header.h == 48);
	assert(icon_menu_radio_country.header.w == 48 && icon_menu_radio_country.header.h == 48);
	assert(icon_menu_radio_genre.header.w == 48 && icon_menu_radio_genre.header.h == 48);
	assert(icon_menu_radio_flylo.header.w == 48 && icon_menu_radio_flylo.header.h == 48);
	assert(icon_radio_language_glyph.header.w == 40 && icon_radio_language_glyph.header.h == 40);
	assert(icon_radio_country_glyph.header.w == 40 && icon_radio_country_glyph.header.h == 40);
	assert(icon_radio_genre_glyph.header.w == 40 && icon_radio_genre_glyph.header.h == 40);
	assert(icon_radio_flylo_glyph.header.w == 40 && icon_radio_flylo_glyph.header.h == 40);

	radiopage_init(&cfg);
	assert(radiopage_screen != NULL);
	switch_screen(radiopage_screen);
	lv_timer_handler();
	lv_obj_update_layout(radiopage_screen);

	// 1. Four tiles, two by two, each carrying its own destination.
	lv_obj_t *grid = screen_container(radiopage_screen);
	assert(grid != NULL);
	assert(lv_obj_get_child_count(grid) == 4);
	for (int i = 0; i < 4; i++) {
		assert(find_clickable_with_label(radiopage_screen, tr(HOME_KEYS[i])) != NULL);
	}
	// Nothing to scroll to: the grid is sized from the space under the heading.
	assert(lv_obj_get_scroll_top(grid) == 0);
	assert(lv_obj_get_scroll_bottom(grid) == 0);
	assert(lv_obj_get_scroll_left(grid) == 0);
	assert(lv_obj_get_scroll_right(grid) == 0);

	// 2. Dark and Light show the illustrations; the flat themes show the glyph
	// in their own accent instead.
	static const lv_image_dsc_t *const ART[4] = {&icon_menu_radio_language, &icon_menu_radio_country,
												 &icon_menu_radio_genre, &icon_menu_radio_flylo};
	static const lv_image_dsc_t *const GLYPHS[4] = {&icon_radio_language_glyph, &icon_radio_country_glyph,
												   &icon_radio_genre_glyph, &icon_radio_flylo_glyph};
	theme_set_kind(THEME_KIND_DARK);
	lv_obj_update_layout(radiopage_screen);
	for (int i = 0; i < 4; i++) {
		assert(tree_has_visible_glyph(radiopage_screen, ART[i]));
		assert(!tree_has_visible_glyph(radiopage_screen, GLYPHS[i]));
	}
	theme_set_kind(THEME_KIND_AMBER_ON_BLACK);
	lv_obj_update_layout(radiopage_screen);
	for (int i = 0; i < 4; i++) {
		assert(tree_has_visible_glyph(radiopage_screen, GLYPHS[i]));
		assert(!tree_has_visible_glyph(radiopage_screen, ART[i]));
	}
	theme_set_kind(THEME_KIND_DARK);
	lv_obj_update_layout(radiopage_screen);

	// 3. With no network the tiles give way to the notice and the Wi-Fi row,
	// and the shortcuts that lead to streams go with them.
	stub_connected = false;
	lv_obj_send_event(radiopage_screen, LV_EVENT_SCREEN_LOADED, NULL);
	lv_obj_update_layout(radiopage_screen);
	assert(lv_obj_is_hidden(grid));
	assert(find_clickable_with_label(radiopage_screen, tr("wi_fi_settings")) != NULL);
	assert(find_clickable_with_label(radiopage_screen, tr(HOME_KEYS[0])) == NULL);
	// The notice leads the page from the top of the panel, where the tiles
	// would start -- not floating in the middle of an empty page.
	lv_obj_t *notice = find_label_by_text(radiopage_screen, tr("enable_wi_fi_first"));
	assert(notice != NULL);
	lv_area_t notice_area;
	lv_obj_get_coords(notice, &notice_area);
	assert(notice_area.y1 < (int32_t)cfg.screen_height / 3);
	stub_connected = true;
	lv_obj_send_event(radiopage_screen, LV_EVENT_SCREEN_LOADED, NULL);
	lv_obj_update_layout(radiopage_screen);
	assert(!lv_obj_is_hidden(grid));
	assert(find_clickable_with_label(radiopage_screen, tr("wi_fi_settings")) == NULL);

	// 4. Sweep all 11 shipped languages and all 5 built-in themes across the
	// home page, asserting BOTH zero raw translation keys AND zero overlaps.
	static const char *const LANGS[] = {
		"Deutsch", "English", "Español", "Français", "Italiano", "Japanese",
		"Nederlands", "Polski", "Português BR", "Russian", "Ukrainian",
	};
	for (size_t li = 0; li < sizeof(LANGS) / sizeof(LANGS[0]); li++) {
		assert(lang_set(LANGS[li]));
		for (int tk = 0; tk < THEME_KIND_COUNT; tk++) {
			theme_set_kind((theme_kind_t)tk);
			lv_screen_load(radiopage_screen);
			lv_obj_update_layout(radiopage_screen);
			assert_no_raw_radio_keys(radiopage_screen, LANGS[li]);
			assert_screen_no_overlaps(radiopage_screen, "radio_home", LANGS[li], tk);
			assert(lv_obj_get_scroll_bottom(grid) == 0);
		}
	}
	// The notice has to fit the panel too, in its longest translation.
	assert(lang_set("Deutsch"));
	theme_set_kind(THEME_KIND_DARK);
	stub_connected = false;
	lv_obj_send_event(radiopage_screen, LV_EVENT_SCREEN_LOADED, NULL);
	lv_obj_update_layout(radiopage_screen);
	assert_screen_no_overlaps(radiopage_screen, "radio_home_offline", "Deutsch", THEME_KIND_DARK);
	stub_connected = true;
	lv_obj_send_event(radiopage_screen, LV_EVENT_SCREEN_LOADED, NULL);

	assert(lang_set("English"));
	theme_set_kind(THEME_KIND_DARK);

	printf("PASS radio_pages (240x320, 11 languages x 5 themes zero-overlap, four tiles, "
		   "no scrolling, offline notice)\n");

	nftw(tmp, unlink_cb, 64, FTW_DEPTH | FTW_PHYS);
	return 0;
}
