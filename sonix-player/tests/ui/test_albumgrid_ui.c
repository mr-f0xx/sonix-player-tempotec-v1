// Real medialist.c LVGL UI test for toggling the 2x2 album grid on and off.
//
// Opens the Albums list with album grid enabled, checks that the four tiles
// sit in a 2x2 grid (102x122, covers 94x94, chevrons hidden), then turns the
// setting off via medialist_set_album_grid(false) and checks that every row
// returns to the single-column list layout (x = 0, full row width 228,
// height 44, y = index * 48, 36x36 covers, chevrons visible).

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "lvgl/lvgl.h"
#include "src/gui/board_profile.h"
#include "src/gui/fonts/fonts.h"
#include "src/gui/library/albumgrid.h"
#include "src/gui/library/geniusui.h"
#include "src/gui/library/medialist.h"
#include "src/gui/library/playlistpage.h"
#include "src/gui/nowplaying/coverloader.h"
#include "src/gui/nowplaying/player.h"
#include "src/gui/nowplaying/trackmenu.h"
#include "src/gui/settings/musicsettings.h"
#include "src/gui/shell/confirm.h"
#include "src/gui/shell/gui.h"
#include "src/gui/shell/icons.h"
#include "src/gui/shell/popover.h"
#include "src/gui/shell/scrolltext.h"
#include "src/gui/shell/settingsrow.h"
#include "src/gui/shell/switcher.h"
#include "src/gui/shell/theme.h"
#include "src/gui/shell/toast.h"
#include "src/system/core/config.h"
#include "src/system/core/lang.h"
#include "src/system/device/power.h"
#include "src/system/library/library.h"
#include "src/system/library/playlists.h"
#include "src/system/playback/device_state.h"
#include "src/system/playback/playlist.h"

#define W 240
#define H 320

static uint32_t framebuffer[W * H];

static void flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map) {
	(void)area;
	(void)px_map;
	lv_display_flush_ready(disp);
}

lv_font_t font_ui_14, font_ui_16, font_ui_18, font_ui_20, font_ui_20_bold;
lv_font_t font_ui_22, font_ui_22_bold, font_ui_24, font_ui_24_bold;
lv_font_t font_ui_26, font_ui_28, font_ui_32, font_ui_64_bold;

static void init_fonts(void) {
	lv_font_t *all[] = {&font_ui_14, &font_ui_16, &font_ui_18, &font_ui_20, &font_ui_20_bold,
						&font_ui_22, &font_ui_22_bold, &font_ui_24, &font_ui_24_bold, &font_ui_26,
						&font_ui_28, &font_ui_32, &font_ui_64_bold};
	for (size_t i = 0; i < sizeof(all) / sizeof(all[0]); i++) {
		*all[i] = lv_font_montserrat_14;
	}
	font_ui_18 = lv_font_montserrat_16;
	font_ui_20 = font_ui_22 = font_ui_24 = lv_font_montserrat_16;
}

// ---- Stubs ----------------------------------------------------------------

const char *tr(const char *key) { return key; }
void bootlogo_set(int choice, bool dark) { (void)choice; (void)dark; }
bool bootpic_present(void) { return false; }

void fonts_register_change(void (*cb)(void)) { (void)cb; }
int fonts_text_size(void) { return FONTS_TEXT_NORMAL; }
bool player_sheet_drag_active(void) { return false; }
void player_sheet_attach_drag(lv_obj_t *obj, bool opening) { (void)obj; (void)opening; }
bool switcher_back_drag_active(void) { return false; }
void switcher_attach_back_gesture(lv_obj_t *screen) { (void)screen; }
void switch_screen(lv_obj_t *target) {
	if (target) {
		lv_screen_load(target);
	}
}
void switch_screen_no_history(lv_obj_t *target) { switch_screen(target); }

lv_obj_t *player_screen = NULL;
void player_play_file(const char *filepath) { (void)filepath; }
void player_refresh_now_playing(void) {}
void trackmenu_details_open(const char *path) { (void)path; }
void geniusui_start(const char *seed_path) { (void)seed_path; }
void playlistpage_add_track(const char *track_path) { (void)track_path; }
void playlistpage_add_tracks(const char *const *paths, int count) { (void)paths; (void)count; }
void playlistpage_add_favourites(const char *const *paths, int count) { (void)paths; (void)count; }
void popover_show(lv_obj_t *anchor, const popover_item_t *items, int count) { (void)anchor; (void)items; (void)count; }
void confirm_show(const char *title, const char *message, const char *ok_label, void (*on_ok)(void *user), void *user) {
	(void)title; (void)message; (void)ok_label; (void)on_ok; (void)user;
}
void gui_notify_no_card(void) {}
void gui_notify_popup(const char *text) { (void)text; }
void toast_success(const char *text) { (void)text; }
void toast_error(const char *text) { (void)text; }
void scrolltext_apply(lv_obj_t *label) { (void)label; }
void power_pause_in_standby(lv_timer_t *timer) { (void)timer; }
bool musicsettings_allow_delete(void) { return false; }
bool musicsettings_endless_shuffle(void) { return false; }

void coverloader_start(void) {}
void coverloader_request(int slot, const char *path, int size) { (void)slot; (void)path; (void)size; }
bool coverloader_take(int slot, cover_image_t *out, bool *finished) {
	(void)slot;
	(void)out;
	if (finished) {
		*finished = false;
	}
	return false;
}
void coverloader_release(int slot) { (void)slot; }
void cover_free(cover_image_t *img) { (void)img; }

void device_state_get(device_state_t *out) { memset(out, 0, sizeof(*out)); }
void device_state_queue_changed(void) {}
bool device_state_play_index(library_index_t *ix, int start_index) { (void)ix; (void)start_index; return false; }
void device_state_play_list(const char *const *list, int count, int start_index) {
	(void)list; (void)count; (void)start_index;
}
bool device_state_album_chaining(void) { return false; }
void device_state_set_album_chaining_random(bool on) { (void)on; }
bool playlist_insert_next(const char *path) { (void)path; return false; }
const char *playlist_card_root(void) { return ""; }
void playlist_set_album(const char *album) { (void)album; }
void playlist_set_mode(playback_mode_t mode) { (void)mode; }
bool playlists_remove_track(const char *name, const char *track_path) { (void)name; (void)track_path; return false; }
int playlists_remove_positions(const char *name, const int *positions, int count) {
	(void)name; (void)positions; (void)count; return 0;
}

static const uint8_t GLYPH_BYTES[4] = {0xff, 0xff, 0xff, 0xff};
#define DUMMY_ICON(name) \
	const lv_image_dsc_t name = {.header = {.magic = LV_IMAGE_HEADER_MAGIC, .cf = LV_COLOR_FORMAT_A8, .w = 2, .h = 2, .stride = 2}, \
								 .data = GLYPH_BYTES, .data_size = sizeof(GLYPH_BYTES)}

DUMMY_ICON(icon_album);
DUMMY_ICON(icon_album_corner);
DUMMY_ICON(icon_arrow_down_up);
DUMMY_ICON(icon_artist);
DUMMY_ICON(icon_artist_album);
DUMMY_ICON(icon_check);
DUMMY_ICON(icon_chevron_right);
DUMMY_ICON(icon_circle_play);
DUMMY_ICON(icon_close);
DUMMY_ICON(icon_ellipsis_vertical);
DUMMY_ICON(icon_folder);
DUMMY_ICON(icon_genre);
DUMMY_ICON(icon_grip);
DUMMY_ICON(icon_heart_filled);
DUMMY_ICON(icon_heart_outline);
DUMMY_ICON(icon_list_plus);
DUMMY_ICON(icon_list_queue);
DUMMY_ICON(icon_list_x);
DUMMY_ICON(icon_music2);
DUMMY_ICON(icon_quality_cd);
DUMMY_ICON(icon_quality_dsd);
DUMMY_ICON(icon_quality_hifi);
DUMMY_ICON(icon_quality_lossy);
DUMMY_ICON(icon_reorder);
DUMMY_ICON(icon_sort_az);
DUMMY_ICON(icon_sort_date_new);
DUMMY_ICON(icon_sort_date_old);
DUMMY_ICON(icon_sort_year_asc);
DUMMY_ICON(icon_sort_year_desc);
DUMMY_ICON(icon_sort_za);
DUMMY_ICON(icon_trash);
DUMMY_ICON(icon_year);

// ---- Fake library index (4 albums) ----------------------------------------

struct library_index {
	library_list_t kind;
	int count;
};

static int fake_album_count = 4;

static const struct {
	const char *name;
	const char *path;
	const char *artist;
} ALBUMS[4] = {
	{"In Rainbows", "/music/radiohead/in_rainbows/01.flac", "Radiohead"},
	{"Orbital", "/music/orbital/brown/01.flac", "Orbital"},
	{"Mezzanine", "/music/massive/mezzanine/01.flac", "Massive Attack"},
	{"1999", "/music/prince/1999/01.flac", "Prince"},
};

bool library_is_open(void) { return true; }
uint32_t library_play_counts_revision(void) { return 1; }
library_index_t *library_index_open(library_list_t kind, library_filter_t filter, const char *value,
									 library_order_t order, bool desc) {
	(void)filter; (void)value; (void)order; (void)desc;
	library_index_t *ix = calloc(1, sizeof(*ix));
	ix->kind = kind;
	ix->count = fake_album_count;
	return ix;
}
void library_index_close(library_index_t *ix) { free(ix); }
int library_index_count(const library_index_t *ix) { return ix ? ix->count : 0; }
bool library_index_stale(const library_index_t *ix) { (void)ix; return false; }
library_index_t *library_index_clone(const library_index_t *ix) {
	library_index_t *c = calloc(1, sizeof(*c));
	c->kind = ix->kind;
	c->count = ix->count;
	return c;
}
int library_index_window(const library_index_t *ix, int offset, int count, library_row_cb cb, void *user) {
	int total = ix ? ix->count : 0;
	int n = 0;
	for (int i = offset; i < total && n < count; i++, n++) {
		if (total == 4) {
			if (!cb(ALBUMS[i].name, ALBUMS[i].path, ALBUMS[i].artist, user)) {
				break;
			}
		} else {
			char name[64], path[128];
			snprintf(name, sizeof(name), "Album %02d", i + 1);
			snprintf(path, sizeof(path), "/music/artist/album_%02d/01.flac", i + 1);
			if (!cb(name, path, "Prolific Artist", user)) {
				break;
			}
		}
	}
	return n;
}
static int ptr_x, ptr_y;
static lv_indev_state_t ptr_state = LV_INDEV_STATE_RELEASED;
static void read_pointer_cb(lv_indev_t *indev, lv_indev_data_t *data) {
	(void)indev;
	data->point.x = ptr_x;
	data->point.y = ptr_y;
	data->state = ptr_state;
}

bool library_index_buckets(const library_index_t *ix, int counts[LIBRARY_INDEX_BUCKETS]) {
	memset(counts, 0, sizeof(int) * LIBRARY_INDEX_BUCKETS);
	if (ix && ix->count >= 30) {
		int half = ix->count / 2;
		counts[1] = half;              // 'A'
		counts[15] = ix->count - half; // 'O' starts at `half`
	}
	return true;
}
char library_index_letter(const char *name) { return name && name[0] ? name[0] : '#'; }
int library_index_slot(char letter) {
	if (letter >= 'A' && letter <= 'Z') {
		return 1 + (letter - 'A');
	}
	return 0;
}
int library_index_find_name(const library_index_t *ix, const char *name) { (void)ix; (void)name; return -1; }
int library_index_find_path(const library_index_t *ix, const char *path) { (void)ix; (void)path; return -1; }
void library_album_title(const char *value, char *out, size_t size) {
	snprintf(out, size, "%s", value ? value : "");
}
bool library_album_same(const char *a, const char *b) { return a && b && strcmp(a, b) == 0; }
bool library_album_artist(const char *album, char *out, size_t size) {
	(void)album;
	if (out && size) {
		out[0] = '\0';
	}
	return false;
}
bool library_track_album_value(const char *path, char *out, size_t out_size) {
	(void)path;
	if (out && out_size) {
		out[0] = '\0';
	}
	return false;
}
library_quality_t library_track_quality(const char *path, int *rate_out, int *bits_out) {
	(void)path;
	if (rate_out) *rate_out = 0;
	if (bits_out) *bits_out = 0;
	return LIBRARY_QUALITY_NONE;
}
bool library_fav_contains(const char *path) { (void)path; return false; }
bool library_fav_toggle(const char *path, const char *name, const char *artist) {
	(void)path; (void)name; (void)artist; return false;
}
int library_fav_remove_many(const char *const *paths, int count) { (void)paths; (void)count; return 0; }
int library_remove_tracks(const char *const *paths, int count) { (void)paths; (void)count; return 0; }
bool library_playlist_move(const char *name, int from, int to) { (void)name; (void)from; (void)to; return false; }

// ---- Helpers --------------------------------------------------------------

static lv_obj_t *find_label(lv_obj_t *obj, const char *text) {
	uint32_t count = lv_obj_get_child_count(obj);
	for (uint32_t i = 0; i < count; i++) {
		lv_obj_t *child = lv_obj_get_child(obj, (int32_t)i);
		if (lv_obj_check_type(child, &lv_label_class) && strcmp(lv_label_get_text(child), text) == 0) {
			return child;
		}
		lv_obj_t *found = find_label(child, text);
		if (found) {
			return found;
		}
	}
	return NULL;
}

static lv_obj_t *button_of(lv_obj_t *label) {
	lv_obj_t *obj = label;
	while (obj && !lv_obj_check_type(obj, &lv_button_class)) {
		obj = lv_obj_get_parent(obj);
	}
	return obj;
}

static void dump_snapshot(lv_obj_t *screen, const char *tag) {
	const char *prefix = getenv("SONIX_ALBUMGRID_SNAPSHOTS");
	if (!prefix || !prefix[0]) {
		return;
	}
	lv_obj_update_layout(screen);
	lv_draw_buf_t *shot = lv_snapshot_take(screen, LV_COLOR_FORMAT_RGB888);
	assert(shot);
	char path[512];
	snprintf(path, sizeof(path), "%s-%s.ppm", prefix, tag);
	FILE *file = fopen(path, "wb");
	assert(file);
	fprintf(file, "P6\n%u %u\n255\n", shot->header.w, shot->header.h);
	for (uint32_t y = 0; y < shot->header.h; y++) {
		const uint8_t *row = shot->data + y * shot->header.stride;
		for (uint32_t x = 0; x < shot->header.w; x++) {
			uint8_t rgb[] = {row[x * 3 + 2], row[x * 3 + 1], row[x * 3]};
			assert(fwrite(rgb, sizeof(rgb), 1, file) == 1);
		}
	}
	assert(fclose(file) == 0);
	lv_draw_buf_destroy(shot);
}

int main(void) {
	char dir[] = "/tmp/sonix-albumgrid-ui-XXXXXX";
	assert(mkdtemp(dir));
	char path[256];
	snprintf(path, sizeof(path), "%s/device_config.ini", dir);
	config_init(path);

	init_fonts();
	theme_init();

	lv_init();
	lv_display_t *disp = lv_display_create(W, H);
	lv_display_set_color_format(disp, LV_COLOR_FORMAT_XRGB8888);
	lv_display_set_buffers(disp, framebuffer, NULL, sizeof(framebuffer), LV_DISPLAY_RENDER_MODE_DIRECT);
	lv_display_set_flush_cb(disp, flush_cb);
	lv_tick_inc(10);

	gui_config_t cfg = {
		.screen_width = W,
		.screen_height = H,
		.padding = 6,
		.top_bar_height = 24,
		.sd_root_path = dir,
	};

	medialist_screen = lv_obj_create(NULL);
	medialist_tracks_screen = lv_obj_create(NULL);
	medialist_albums_screen = lv_obj_create(NULL);
	medialist_init(&cfg);
	medialist_set_album_grid(true);
	medialist_open("albums", LIBRARY_LIST_ALBUMS, LIBRARY_FILTER_NONE, NULL);
	lv_screen_load(medialist_screen);
	lv_obj_update_layout(medialist_screen);
	dump_snapshot(medialist_screen, "grid");

	// Check 2x2 grid geometry on all 4 tiles.
	lv_area_t body_a;
	lv_obj_t *lbl0 = find_label(medialist_screen, ALBUMS[0].name);
	assert(lbl0);
	lv_obj_t *btn0 = button_of(lbl0);
	assert(btn0);
	lv_obj_get_coords(lv_obj_get_parent(btn0), &body_a);

	albumgrid_metrics_t m;
	albumgrid_measure(228, 248, lv_font_get_line_height(&font_ui_14), &m);
	assert(m.columns == 2 && m.rows == 2);
	for (int i = 0; i < 4; i++) {
		lv_obj_t *lbl = find_label(medialist_screen, ALBUMS[i].name);
		assert(lbl);
		lv_obj_t *btn = button_of(lbl);
		assert(btn);
		lv_area_t ba, la;
		lv_obj_get_coords(btn, &ba);
		lv_obj_get_coords(lbl, &la);
		assert(ba.x1 - body_a.x1 == albumgrid_tile_x(i, &m));
		assert(ba.y1 - body_a.y1 == albumgrid_tile_y(i, &m));
		assert(ba.x2 - ba.x1 + 1 == m.tile_w);
		assert(ba.y2 - ba.y1 + 1 == m.tile_h);
		lv_obj_t *icon = lv_obj_get_child(btn, 0);
		lv_obj_t *chev = lv_obj_get_child(btn, 3);
		lv_area_t ia;
		lv_obj_get_coords(icon, &ia);
		assert(ia.x2 - ia.x1 + 1 == m.cover);
		assert(ia.y2 - ia.y1 + 1 == m.cover);
		assert(lv_obj_is_hidden(chev));
		assert(la.x2 - la.x1 + 1 >= m.cover - 4);
	}

	// Long-press album 0 and tap album 1 in grid mode: the header title hides
	// instead of truncating to "A...", and the check mark sits cleanly inside
	// the top-right corner of each selected tile without clipping.
	lv_obj_t *title_lbl = find_label(medialist_screen, "albums");
	assert(title_lbl && !lv_obj_is_hidden(title_lbl));
	lv_obj_send_event(btn0, LV_EVENT_LONG_PRESSED, NULL);
	lv_obj_t *btn1 = button_of(find_label(medialist_screen, ALBUMS[1].name));
	assert(btn1);
	lv_obj_send_event(btn1, LV_EVENT_CLICKED, NULL);
	lv_obj_update_layout(medialist_screen);
	dump_snapshot(medialist_screen, "selected");

	assert(lv_obj_is_hidden(title_lbl));
	lv_obj_t *sel_btns[2] = {btn0, btn1};
	for (int i = 0; i < 2; i++) {
		lv_obj_t *chk = lv_obj_get_child(sel_btns[i], 4);
		assert(chk && !lv_obj_is_hidden(chk));
		lv_area_t ba, ca;
		lv_obj_get_coords(sel_btns[i], &ba);
		lv_obj_get_coords(chk, &ca);
		assert(ca.x1 >= ba.x1 && ca.x2 <= ba.x2);
		assert(ca.y1 >= ba.y1 && ca.y2 <= ba.y2);
		assert(ca.y1 <= ba.y1 + 10);
		assert(ca.x2 >= ba.x2 - 10);
	}

	// Tap the two selected tiles again to exit selection mode and restore the header.
	lv_obj_send_event(btn0, LV_EVENT_CLICKED, NULL);
	lv_obj_send_event(btn1, LV_EVENT_CLICKED, NULL);
	lv_obj_update_layout(medialist_screen);
	assert(!lv_obj_is_hidden(title_lbl));

	// Disable album grid and verify immediate restoration to list layout.
	medialist_set_album_grid(false);
	lv_obj_update_layout(medialist_screen);
	dump_snapshot(medialist_screen, "list");

	for (int i = 0; i < 4; i++) {
		lv_obj_t *lbl = find_label(medialist_screen, ALBUMS[i].name);
		assert(lbl);
		lv_obj_t *btn = button_of(lbl);
		assert(btn);
		lv_area_t ba, la, ia;
		lv_obj_get_coords(btn, &ba);
		lv_obj_get_coords(lbl, &la);
		lv_obj_t *icon = lv_obj_get_child(btn, 0);
		lv_obj_t *chev = lv_obj_get_child(btn, 3);
		lv_obj_get_coords(icon, &ia);
		assert(ba.x1 - body_a.x1 == 0);
		assert(ba.y1 - body_a.y1 == i * 49);
		assert(ba.x2 - ba.x1 + 1 == 228);
		assert(ba.y2 - ba.y1 + 1 == 44);
		assert(ia.x2 - ia.x1 + 1 == 36);
		assert(ia.y2 - ia.y1 + 1 == 36);
		assert(!lv_obj_is_hidden(chev));
		assert(la.x2 - la.x1 + 1 > 120);
	}

	// Re-enable album grid while on another list (Artists), then open Albums.
	medialist_open("artists", LIBRARY_LIST_ARTISTS, LIBRARY_FILTER_NONE, NULL);
	medialist_set_album_grid(true);
	medialist_open("albums", LIBRARY_LIST_ALBUMS, LIBRARY_FILTER_NONE, NULL);
	lv_obj_update_layout(medialist_screen);
	lv_area_t b1;
	lv_obj_get_coords(button_of(find_label(medialist_screen, ALBUMS[1].name)), &b1);
	assert(b1.x1 - body_a.x1 == albumgrid_tile_x(1, &m));
	assert(b1.y1 - body_a.y1 == 0);
	assert(b1.x2 - b1.x1 + 1 == m.tile_w);

	// Regression: an artist (or full album list) with more than ROW_POOL albums
	// -- especially odd totals like 13, 17, or 19 -- must bind and show every
	// album right down to the final tile when scrolled to the bottom of the 2x2
	// grid, and rebind cleanly when scrolling back up.
	const int counts_to_check[] = {13, 16, 17, 19, 35};
	for (size_t ci = 0; ci < sizeof(counts_to_check) / sizeof(counts_to_check[0]); ci++) {
		int total = counts_to_check[ci];
		fake_album_count = total;

		char artist_name[64];
		snprintf(artist_name, sizeof(artist_name), "Prolific Artist %d", total);
		medialist_open(artist_name, LIBRARY_LIST_ALBUMS, LIBRARY_FILTER_ARTIST, artist_name);
		lv_obj_update_layout(medialist_albums_screen);

		lv_obj_t *lbl_first = find_label(medialist_albums_screen, "Album 01");
		assert(lbl_first);
		lv_obj_t *artist_body = lv_obj_get_parent(button_of(lbl_first));
		lv_obj_t *artist_list = lv_obj_get_parent(artist_body);
		assert(lv_obj_get_height(artist_body) == albumgrid_canvas_height(total, &m));

		// Walk every tile down to the bottom and verify it sits inside the
		// viewport at its exact 2x2 grid position.
		for (int idx = 0; idx < total; idx++) {
			lv_obj_scroll_to_y(artist_list, albumgrid_tile_y(idx, &m), LV_ANIM_OFF);
			lv_obj_send_event(artist_list, LV_EVENT_SCROLL, NULL);
			lv_obj_update_layout(medialist_albums_screen);

			char expected[64];
			snprintf(expected, sizeof(expected), "Album %02d", idx + 1);
			lv_obj_t *lbl = find_label(medialist_albums_screen, expected);
			assert(lbl);
			lv_obj_t *tile = button_of(lbl);
			assert(tile && !lv_obj_is_hidden(tile));
			lv_area_t abody_a, tile_a, list_a;
			lv_obj_get_coords(artist_body, &abody_a);
			lv_obj_get_coords(tile, &tile_a);
			lv_obj_get_coords(artist_list, &list_a);
			assert(tile_a.x1 - abody_a.x1 == albumgrid_tile_x(idx, &m));
			assert(tile_a.y1 - abody_a.y1 == albumgrid_tile_y(idx, &m));
			assert(tile_a.y1 >= list_a.y1 && tile_a.y2 <= list_a.y2);
		}

		// Reopening the same 17-album artist at the bottom preserves scroll and
		// keeps Album 17 bound and visible.
		if (total == 17) {
			medialist_open(artist_name, LIBRARY_LIST_ALBUMS, LIBRARY_FILTER_ARTIST, artist_name);
			lv_obj_update_layout(medialist_albums_screen);
			lv_obj_t *lbl17 = find_label(medialist_albums_screen, "Album 17");
			assert(lbl17 && !lv_obj_is_hidden(button_of(lbl17)));
		}

		// Walk every tile back up to Album 01 to verify paired rows rebind cleanly.
		for (int idx = total - 1; idx >= 0; idx--) {
			lv_obj_scroll_to_y(artist_list, albumgrid_tile_y(idx, &m), LV_ANIM_OFF);
			lv_obj_send_event(artist_list, LV_EVENT_SCROLL, NULL);
			lv_obj_update_layout(medialist_albums_screen);

			char expected[64];
			snprintf(expected, sizeof(expected), "Album %02d", idx + 1);
			lv_obj_t *lbl = find_label(medialist_albums_screen, expected);
			assert(lbl && !lv_obj_is_hidden(button_of(lbl)));
		}

		// Also verify the top-level Albums panel with 17 albums.
		if (total == 17) {
			medialist_open("albums_17", LIBRARY_LIST_ALBUMS, LIBRARY_FILTER_NONE, NULL);
			lv_obj_update_layout(medialist_screen);
			lv_obj_t *top_lbl1 = find_label(medialist_screen, "Album 01");
			assert(top_lbl1);
			lv_obj_t *top_list = lv_obj_get_parent(lv_obj_get_parent(button_of(top_lbl1)));
			lv_obj_scroll_to_y(top_list, albumgrid_tile_y(16, &m), LV_ANIM_OFF);
			lv_obj_send_event(top_list, LV_EVENT_SCROLL, NULL);
			lv_obj_update_layout(medialist_screen);
			lv_obj_t *top_lbl17 = find_label(medialist_screen, "Album 17");
			assert(top_lbl17 && !lv_obj_is_hidden(button_of(top_lbl17)));
		}
	}

	// Verify A-Z strip jump on a 60-album grid lands at albumgrid_tile_y(30, &m)
	// rather than list-mode 30 * ROW_PITCH.
	medialist_open("artists", LIBRARY_LIST_ARTISTS, LIBRARY_FILTER_NONE, NULL);
	fake_album_count = 60;
	medialist_open("albums_60", LIBRARY_LIST_ALBUMS, LIBRARY_FILTER_NONE, NULL);
	lv_obj_update_layout(medialist_screen);
	lv_obj_t *top_lbl1 = find_label(medialist_screen, "Album 01");
	assert(top_lbl1);
	lv_obj_t *top_list = lv_obj_get_parent(lv_obj_get_parent(button_of(top_lbl1)));
	// Scrolling wakes the A-Z strip on long lists.
	lv_obj_scroll_to_y(top_list, m.pitch_y, LV_ANIM_OFF);
	lv_obj_send_event(top_list, LV_EVENT_SCROLL, NULL);
	lv_obj_update_layout(medialist_screen);
	lv_obj_t *letter_o = find_label(medialist_screen, "O");
	assert(letter_o && !lv_obj_is_hidden(lv_obj_get_parent(letter_o)));
	lv_area_t oa;
	lv_obj_get_coords(letter_o, &oa);

	lv_indev_t *indev = lv_indev_create();
	lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
	lv_indev_set_read_cb(indev, read_pointer_cb);
	ptr_x = 0; ptr_y = 0; ptr_state = LV_INDEV_STATE_RELEASED;
	lv_indev_read(indev);

	ptr_x = (oa.x1 + oa.x2) / 2;
	ptr_y = (oa.y1 + oa.y2) / 2;
	ptr_state = LV_INDEV_STATE_PRESSED;
	lv_indev_read(indev);
	lv_tick_inc(30);
	ptr_state = LV_INDEV_STATE_RELEASED;
	lv_indev_read(indev);
	lv_obj_update_layout(medialist_screen);

	assert(lv_obj_get_scroll_y(top_list) == albumgrid_tile_y(30, &m));
	lv_obj_t *lbl31 = find_label(medialist_screen, "Album 31");
	assert(lbl31 && !lv_obj_is_hidden(button_of(lbl31)));

	printf("albumgrid-ui: 2x2 grid toggles on and restores clean single-column list rows\n");
	return 0;
}
