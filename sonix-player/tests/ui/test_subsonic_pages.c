/*
 * Headless LVGL integration test for the Subsonic home page
 * (`subsonicpage.c`) at V1 dimensions (240x320) across all 11 shipped
 * languages and all 5 built-in themes.
 *
 * The page is the same tiled grid the SoundCloud home page draws: four cards,
 * two by two, each with a picture and its caption underneath. What this test
 * pins down is that the grid never scrolls, that every card keeps its picture
 * and its caption inside itself in every language, and that Dark and Light
 * show the illustrations while the flat themes show their own glyph.
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
#include "src/gui/streaming/subsonicpage.h"
#include "src/system/audio/audio.h"
#include "src/system/core/config.h"
#include "src/system/core/lang.h"
#include "src/system/core/utils.h"
#include "src/system/device/system.h"
#include "src/system/net/tls.h"
#include "src/system/playback/device_state.h"
#include "src/system/playback/playlist.h"
#include "src/system/streaming/subsonic.h"
#include "src/system/streaming/subsonicstream.h"
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

/* ---- recording Subsonic client & stream stubs --------------------------- */

static subsonic_credentials_t stub_creds;
static bool stub_configured;
static subsonic_quality_t stub_quality = {SUBSONIC_FORMAT_MP3, 320};
static tls_verify_t stub_verify = TLS_VERIFY_STRICT;
static int stub_browses;

void subsonic_init(void) {}

bool subsonic_configure(const subsonic_credentials_t *credentials, bool remember, char *error,
						size_t error_size) {
	(void)remember;
	(void)error;
	(void)error_size;
	if (credentials) stub_creds = *credentials;
	stub_configured = stub_creds.server[0] != '\0';
	return true;
}

void subsonic_credentials(subsonic_credentials_t *out, bool *remembered) {
	if (out) *out = stub_creds;
	if (remembered) *remembered = true;
}

bool subsonic_configured(void) { return stub_configured; }

bool subsonic_forget(void) {
	memset(&stub_creds, 0, sizeof(stub_creds));
	stub_configured = false;
	return true;
}

subsonic_quality_t subsonic_quality(void) { return stub_quality; }
void subsonic_set_quality(subsonic_quality_t quality) { stub_quality = quality; }

bool subsonic_ping(char *error, size_t error_size) {
	(void)error;
	(void)error_size;
	return true;
}

int subsonic_browse(subsonic_browse_t browse, const char *id, int offset, subsonic_entry_t *entries,
					int limit, bool *more, char *error, size_t error_size) {
	(void)browse;
	(void)id;
	(void)offset;
	(void)entries;
	(void)limit;
	(void)error;
	(void)error_size;
	stub_browses++;
	if (more) *more = false;
	return 0;
}

bool subsonicstream_owns(const char *path) {
	(void)path;
	return false;
}
bool subsonicstream_queue(const subsonic_entry_t *tracks, int count, int start, char (*paths)[96],
						  const char **pointers, int *queued, int *selected, char *error,
						  size_t error_size) {
	(void)tracks;
	(void)count;
	(void)start;
	(void)paths;
	(void)pointers;
	(void)error;
	(void)error_size;
	if (queued) *queued = 0;
	if (selected) *selected = 0;
	return true;
}
bool subsonicstream_prepare(char *path, size_t path_size) {
	(void)path;
	(void)path_size;
	return false;
}
void subsonicstream_prefetch(const char *path) { (void)path; }
void subsonicstream_cancel(void) {}
void subsonicstream_reset(void) {}
bool subsonicstream_take_error(char *out, size_t size) {
	(void)out;
	(void)size;
	return false;
}

tls_verify_t tls_verify_mode(void) { return stub_verify; }
void tls_set_verify_mode(tls_verify_t mode) { stub_verify = mode; }

/* ---- widget-tree inspection helpers ------------------------------------- */

static void assert_no_raw_subsonic_keys(lv_obj_t *root, const char *lang_name) {
	if (!root) return;
	if (lv_obj_check_type(root, &lv_label_class)) {
		const char *txt = lv_label_get_text(root);
		if (txt && strncmp(txt, "subsonic_", 9) == 0) {
			fprintf(stderr, "Untranslated key '%s' on screen in language '%s'\n", txt, lang_name);
			assert(!"raw subsonic_* translation key leaked to UI");
		}
	}
	uint32_t cnt = lv_obj_get_child_count(root);
	for (uint32_t i = 0; i < cnt; i++) {
		assert_no_raw_subsonic_keys(lv_obj_get_child(root, (int32_t)i), lang_name);
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
	"subsonic_server_settings",
	"subsonic_test_connection",
	"subsonic_folders",
	"artists",
};

int main(int argc, char **argv) {
	assert(argc >= 2 && "usage: test_subsonic_pages <lang_dir>");
	const char *lang_dir = argv[1];

	char tmp[] = "/tmp/sonix-subsonic-ui-XXXXXX";
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

	// The pictures are rendered at exactly the size the cards draw them, so the
	// page never asks LVGL to resample one.
	assert(icon_menu_sub_server.header.w == 54 && icon_menu_sub_server.header.h == 54);
	assert(icon_menu_sub_test.header.w == 54 && icon_menu_sub_test.header.h == 54);
	assert(icon_menu_sub_folders.header.w == 54 && icon_menu_sub_folders.header.h == 54);
	assert(icon_menu_sub_artists.header.w == 54 && icon_menu_sub_artists.header.h == 54);
	assert(icon_sub_server_glyph.header.w == 46 && icon_sub_server_glyph.header.h == 46);
	assert(icon_sub_test_glyph.header.w == 46 && icon_sub_test_glyph.header.h == 46);
	assert(icon_sub_folders_glyph.header.w == 46 && icon_sub_folders_glyph.header.h == 46);
	assert(icon_sub_artists_glyph.header.w == 46 && icon_sub_artists_glyph.header.h == 46);
	for (int i = 0; i < 4; i++) {
		const lv_image_dsc_t *art[] = {&icon_menu_sub_server, &icon_menu_sub_test, &icon_menu_sub_folders,
									   &icon_menu_sub_artists};
		assert(art[i]->header.stride == art[i]->header.w * 4);
	}

	subsonicpage_init(&cfg);
	assert(subsonic_screen != NULL);
	assert(registered_prep_cb == subsonicstream_prepare);
	switch_screen(subsonic_screen);
	lv_timer_handler();
	lv_obj_update_layout(subsonic_screen);

	// 1. Four tiles, two by two, each carrying its own destination.
	lv_obj_t *grid = screen_container(subsonic_screen);
	assert(grid != NULL);
	assert(lv_obj_get_child_count(grid) == 4);
	for (int i = 0; i < 4; i++) {
		assert(find_clickable_with_label(subsonic_screen, tr(HOME_KEYS[i])) != NULL);
	}
	// Nothing to scroll to: the grid is sized from the space under the heading.
	assert(lv_obj_get_scroll_top(grid) == 0);
	assert(lv_obj_get_scroll_bottom(grid) == 0);
	assert(lv_obj_get_scroll_left(grid) == 0);
	assert(lv_obj_get_scroll_right(grid) == 0);

	// 2. Dark and Light show the illustrations; the flat themes show the glyph
	// in their own accent instead.
	theme_set_kind(THEME_KIND_DARK);
	lv_obj_update_layout(subsonic_screen);
	assert(tree_has_visible_glyph(subsonic_screen, &icon_menu_sub_server));
	assert(!tree_has_visible_glyph(subsonic_screen, &icon_sub_server_glyph));
	theme_set_kind(THEME_KIND_AMBER_ON_BLACK);
	lv_obj_update_layout(subsonic_screen);
	for (int i = 0; i < 4; i++) {
		const lv_image_dsc_t *glyphs[] = {&icon_sub_server_glyph, &icon_sub_test_glyph,
										  &icon_sub_folders_glyph, &icon_sub_artists_glyph};
		const lv_image_dsc_t *art[] = {&icon_menu_sub_server, &icon_menu_sub_test, &icon_menu_sub_folders,
									   &icon_menu_sub_artists};
		assert(tree_has_visible_glyph(subsonic_screen, glyphs[i]));
		assert(!tree_has_visible_glyph(subsonic_screen, art[i]));
	}
	theme_set_kind(THEME_KIND_DARK);
	lv_obj_update_layout(subsonic_screen);

	// 3. A tap on Test connection while no server is saved leads to the server
	// settings, the same way the row it replaced behaved.
	stub_configured = false;
	lv_obj_t *test_tile = find_clickable_with_label(subsonic_screen, tr("subsonic_test_connection"));
	assert(test_tile != NULL);
	lv_obj_send_event(test_tile, LV_EVENT_CLICKED, NULL);
	lv_obj_t *settings_scr = lv_screen_active();
	assert(settings_scr != subsonic_screen);
	assert(settingsrow_page_title(settings_scr) != NULL);

	// 4. The Server settings tile opens the same page.
	switch_screen_return_to(subsonic_screen);
	lv_obj_t *server_tile = find_clickable_with_label(subsonic_screen, tr("subsonic_server_settings"));
	assert(server_tile != NULL);
	lv_obj_send_event(server_tile, LV_EVENT_CLICKED, NULL);
	assert(lv_screen_active() == settings_scr);
	switch_screen_return_to(subsonic_screen);

	// 5. Sweep all 11 shipped languages and all 5 built-in themes across the
	// home page, asserting BOTH zero raw translation keys AND zero overlaps.
	static const char *const LANGS[] = {
		"Deutsch", "English", "Español", "Français", "Italiano", "Japanese",
		"Nederlands", "Polski", "Português BR", "Russian", "Ukrainian",
	};
	for (size_t li = 0; li < sizeof(LANGS) / sizeof(LANGS[0]); li++) {
		assert(lang_set(LANGS[li]));
		for (int tk = 0; tk < THEME_KIND_COUNT; tk++) {
			theme_set_kind((theme_kind_t)tk);
			lv_screen_load(subsonic_screen);
			lv_obj_update_layout(subsonic_screen);
			assert_no_raw_subsonic_keys(subsonic_screen, LANGS[li]);
			assert_screen_no_overlaps(subsonic_screen, "subsonic_home", LANGS[li], tk);
			// The two lines the captions are allowed never turn the page into
			// something to scroll.
			assert(lv_obj_get_scroll_bottom(grid) == 0);
		}
	}
	assert(lang_set("English"));
	theme_set_kind(THEME_KIND_DARK);

	printf("PASS subsonic_pages (240x320, 11 languages x 5 themes zero-overlap, four tiles, "
		   "no scrolling)\n");

	(void)popup_calls;
	(void)confirm_state;
	(void)busy_toasts;
	assert(stub_browses == 0);
	assert(screen_depth >= 0);
	nftw(tmp, unlink_cb, 64, FTW_DEPTH | FTW_PHYS);
	return 0;
}
