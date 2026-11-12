/* Real LVGL, menus, appearance controls, themes, translations and persistence.
 * Only fonts and unrelated playback/navigation/board services are substituted. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "src/gui/board_profile.h"
#include "src/gui/fonts/fonts.h"
#include "src/gui/ebook/ebookpage.h"
#include "src/gui/library/audiobooks.h"
#include "src/gui/library/libraryscan.h"
#include "src/gui/library/medialist.h"
#include "src/gui/library/music.h"
#include "src/gui/library/playlistpage.h"
#include "src/gui/library/search.h"
#include "src/gui/nowplaying/coverflow.h"
#include "src/gui/shell/gestures.h"
#include "src/gui/nowplaying/player.h"
#include "src/system/streaming/podcast.h"
#include "src/gui/settings/appearance.h"
#include "src/gui/settings/bootpicpage.h"
#include "src/gui/settings/musicsettings.h"
#include "src/gui/settings/settings.h"
#include "src/gui/shell/corners.h"
#include "src/gui/shell/gridpage.h"
#include "src/gui/shell/icons.h"
#include "src/gui/shell/main_menu.h"
#include "src/gui/shell/main_menu_icons.h"
#include "src/gui/shell/morepage.h"
#include "src/gui/shell/settingsrow.h"
#include "src/gui/shell/switcher.h"
#include "src/system/addons/addons.h"
#include "src/gui/shell/theme.h"
#include "src/gui/shell/toast.h"
#include "src/gui/shell/topbar.h"
#include "src/gui/streaming/streaming.h"
#include "src/gui/wireless/wireless.h"
#include "src/system/core/config.h"
#include "src/system/core/lang.h"
#include "src/system/device/bootlogo.h"
#include "src/system/device/bootpic.h"
#include "src/system/library/audiobookdb.h"
#include "src/system/library/library.h"
#include "src/system/playback/device_state.h"
#include "src/system/playback/playlist.h"

static void check_custom_theme(void) {
	char root[] = "/tmp/sonix-theme-XXXXXX";
	assert(mkdtemp(root));
	char dir[256], path[512];
	snprintf(dir, sizeof(dir), "%s/Themes", root);
	snprintf(path, sizeof(path), "%s/custom.ini", dir);
	assert(mkdir(dir, 0700) == 0);
	theme_set_card_root(root);
	assert(!theme_reload_custom());
	theme_set_kind(THEME_KIND_CUSTOM);
	assert(theme_get_kind() != THEME_KIND_CUSTOM);
	FILE *f = fopen(path, "w");
	assert(f);
	fputs("[theme]\nscreen_bg = #010203\nsurface = #101112\nsurface_pressed = #202122\n"
		"panel = #303132\ncover_bg = #404142\ntext_primary = #f0f1f2\n"
		"text_secondary = #a0a1a2\naccent = #123456\n", f);
	assert(fclose(f) == 0);
	theme_set_kind(THEME_KIND_CUSTOM);
	assert(theme_get_kind() == THEME_KIND_CUSTOM);
	assert(strcmp(config_get("ui", "theme", ""), "custom") == 0);
	assert(theme()->screen_bg.red == 1 && theme()->screen_bg.green == 2);
	assert(theme()->accent.blue == 0x56);
	// A changed file is picked up without restarting the player.
	f = fopen(path, "w");
	assert(f);
	fputs("[theme]\nscreen_bg = #040506\nsurface = #101112\nsurface_pressed = #202122\n"
		"panel = #303132\ncover_bg = #404142\ntext_primary = #f0f1f2\n"
		"text_secondary = #a0a1a2\naccent = #123456\n", f);
	assert(fclose(f) == 0);
	assert(theme_reload_custom());
	assert(theme()->screen_bg.red == 4);
	// Invalid edits must not partially repaint or change the saved choice.
	f = fopen(path, "w");
	assert(f);
	fputs("[theme]\nscreen_bg = #zzzzzz\n", f);
	assert(fclose(f) == 0);
	assert(!theme_reload_custom());
	assert(theme()->screen_bg.red == 4);
	theme_set_kind(THEME_KIND_DARK);
	theme_set_kind(THEME_KIND_CUSTOM);
	assert(theme_get_kind() == THEME_KIND_DARK);
	assert(unlink(path) == 0);
	assert(rmdir(dir) == 0);
	assert(rmdir(root) == 0);
	theme_set_card_root(NULL);
}

const char *storage_sd_root(void) { return NULL; }

lv_obj_t *streaming_screen, *wireless_screen;
lv_obj_t *audiobooks_screen, *morepage_screen, *settings_screen, *player_screen;
lv_obj_t *ebookpage_screen, *browser_screen, *musicsettings_screen;

lv_font_t font_ui_14, font_ui_16, font_ui_18, font_ui_20, font_ui_20_bold;
lv_font_t font_ui_22, font_ui_22_bold, font_ui_24, font_ui_24_bold;
lv_font_t font_ui_26, font_ui_28, font_ui_32;
void thread_be_background(const char *name) { (void)name; }

static void (*font_callbacks[16])(void);
static int font_callback_count;
static int text_size = FONTS_TEXT_NORMAL;

static void init_fonts(void) {
	lv_font_t *fonts[] = {
		&font_ui_14, &font_ui_16, &font_ui_18, &font_ui_20, &font_ui_20_bold,
		&font_ui_22, &font_ui_22_bold, &font_ui_24, &font_ui_24_bold,
		&font_ui_26, &font_ui_28, &font_ui_32,
	};
	for (size_t i = 0; i < sizeof(fonts) / sizeof(fonts[0]); i++) {
		*fonts[i] = lv_font_montserrat_14;
	}
}

void fonts_register_change(void (*cb)(void)) {
	assert(font_callback_count < 16);
	font_callbacks[font_callback_count++] = cb;
}
int fonts_text_size(void) { return text_size; }
bool fonts_set_text_size(int size) {
	text_size = size;
	/* Deliberately enlarge the footer enough to force the grid to reflow. */
	font_ui_14.line_height = size == FONTS_TEXT_LARGE ? 24 : 14;
	for (int i = 0; i < font_callback_count; i++) {
		font_callbacks[i]();
	}
	return true;
}
const char *fonts_choice(void) { return "default"; }
bool fonts_set_face(const char *id) { return strcmp(id, "default") == 0; }
int fonts_choices(const char **ids, const char **labels, int max) {
	assert(max > 0);
	ids[0] = "default";
	labels[0] = "Test font";
	return 1;
}

/* Last choice handed to the flash marker, so the test can see one was made. */
static int boot_choice = -1;
/* The bridge from a worker thread to the interface thread, as one entry: the
 * test runs the callback itself, when it says to, instead of on a timer. */
static void (*posted_cb)(void *);
static void *posted_user;
bool gui_post(void (*cb)(void *), void *user) {
	posted_cb = cb;
	posted_user = user;
	return true;
}
static void drain_post(void) {
	void (*cb)(void *) = posted_cb;
	void *user = posted_user;
	posted_cb = NULL;
	posted_user = NULL;
	if (cb) {
		cb(user);
	}
}

void bootlogo_set(int choice, bool dark) {
	(void)dark;
	boot_choice = choice;
}

/* ---- the Bootlogo folder, substituted -------------------------------------
 * The card is a list of names here: the page lists them, the copy is never
 * really made, and what is checked is the page and the choice around it.
 */
static const char *bootpic_names[4];
static int bootpic_count;
static int bootpic_installed = -1; /* index of the picture last installed */

void bootpic_set_root(const char *card_root) { (void)card_root; }
const char *bootpic_dir(void) { return "<card>/Bootlogo"; }
bool bootpic_present(void) { return bootpic_installed >= 0; }
int bootpic_list(char names[][BOOTPIC_NAME_MAX + 1], int max) {
	int n = bootpic_count < max ? bootpic_count : max;
	for (int i = 0; i < n; i++) {
		snprintf(names[i], BOOTPIC_NAME_MAX + 1, "%s", bootpic_names[i]);
	}
	return n;
}
bool bootpic_size(const char *name, int *width, int *height) {
	(void)name;
	*width = 640;
	*height = 480;
	return true;
}
bootpic_result_t bootpic_install(const char *name, int panel_w, int panel_h) {
	(void)panel_w;
	(void)panel_h;
	for (int i = 0; i < bootpic_count; i++) {
		if (strcmp(bootpic_names[i], name) == 0) {
			bootpic_installed = i;
			return BOOTPIC_OK;
		}
	}
	return BOOTPIC_NO_FILE;
}
/* What the home footer reads. Empty is "no track loaded"; the footer check
 * below fills it in to exercise the playing arrangement. */
static device_state_t strip_state;
void device_state_get(device_state_t *out) { *out = strip_state; }
bool gui_card_available(void) { return true; }
void gui_notify_no_card(void) { assert(!"unexpected missing card"); }
void audiobooks_open(void) { switch_screen(audiobooks_screen); }
void gui_notify_popup(const char *text) { (void)text; assert(!"unexpected popup"); }
static int error_toasts;
void toast_error(const char *text) { assert(text && text[0]); error_toasts++; }
/* The busy card a pick puts up, and the success card that replaces it: this
 * test never asks for the failure one. */
static const char *toast_busy_text;
void toast_busy(const char *text) { toast_busy_text = text; }
void toast_busy_end(void) { toast_busy_text = NULL; }
void toast_success(const char *text) { toast_busy_text = NULL; (void)text; }
const char *bootpic_result_key(bootpic_result_t result) {
	(void)result;
	return "bootpic_unreadable";
}
void topbar_set_battery_percent(bool shown) { (void)shown; }
void topbar_set_clock_position(int pos) { (void)pos; }
void switch_screen(lv_obj_t *screen) { lv_screen_load(screen); }
void switch_screen_cb(lv_event_t *e) { switch_screen(lv_event_get_user_data(e)); }
void ebookpage_open(void) { switch_screen(ebookpage_screen); }
static bool test_back_drag, test_player_drag, test_coverflow_drag;
void switcher_attach_back_gesture(lv_obj_t *obj) { (void)obj; }
bool switcher_back_drag_active(void) { return test_back_drag; }
void player_sheet_attach_drag(lv_obj_t *obj, bool opening) { (void)obj; (void)opening; }
bool player_sheet_drag_active(void) { return test_player_drag; }

/* The carousel's own hooks. The pages here never open it: what they are checked
 * for is that they leave the pull alone (see the tap guards in gridpage.c). */
void coverflow_attach_edge(lv_obj_t *obj, gui_config_t *cfg) { (void)obj; (void)cfg; }
bool coverflow_drag_active(void) { return test_coverflow_drag; }
bool coverflow_is_open(void) { return false; }

void player_refresh_now_playing(void) {}
// The podcast behind the playing file: nothing on this menu is a podcast
// episode, so the mini-player's line falls back to the artist tag as it does
// for a track, and the two lookups behind that answer nothing.
bool player_current_podcast_feed(long long *id_out, podcast_feed_t *feed_out) {
	(void)id_out; (void)feed_out;
	return false;
}
bool podcastcache_is_episode(const char *path) { (void)path; return false; }
bool podcastcache_tag(const char *path, const char *key, char *out, size_t size) {
	(void)path; (void)key;
	if (out && size) {
		out[0] = '\0';
	}
	return false;
}
void player_sheet_open(bool animated) { (void)animated; }
void medialist_open(const char *title, library_list_t kind, library_filter_t filter, const char *value) {
	(void)title; (void)kind; (void)filter; (void)value;
}
void medialist_open_years(void) {}
void medialist_open_most_played(void) {}
void playlistpage_open(void) {}
void search_open(void) {}
void libraryscan_choose_folders(void) {}
bool library_scan_running(void) { return false; }
int library_track_count(void) { return 42; }
bool library_track_names(const char *path, char *title, size_t title_len, char *artist, size_t artist_len) {
	(void)path; (void)title; (void)title_len; (void)artist; (void)artist_len;
	return false;
}
int playlist_count(void) { return 0; }
int playlist_current_index(void) { return -1; }
unsigned playlist_revision(void) { return 0; }
bool playlist_path_at(int index, char *out, size_t size) { (void)index; (void)out; (void)size; return false; }
bool audiobookdb_book_for_file(const char *path, char *book_out, size_t size) {
	(void)path; (void)book_out; (void)size;
	return false;
}
bool device_state_play_queue_index(int index) { (void)index; return false; }
void device_state_play_file(const char *path) { (void)path; }
bool musicsettings_playlists_first(void) { return false; }

static music_tile_id_t test_active_tiles[MUSIC_TILE_COUNT] = {
	MUSIC_TILE_ALL_TRACKS, MUSIC_TILE_ALBUMS, MUSIC_TILE_ARTISTS,
	MUSIC_TILE_ALBUM_ARTISTS, MUSIC_TILE_GENRES, MUSIC_TILE_YEARS,
	MUSIC_TILE_PLAYLISTS, MUSIC_TILE_MOST_PLAYED, MUSIC_TILE_BROWSE,
};
static int test_active_tile_count = MUSIC_TILE_COUNT;

int musicsettings_main_tiles(music_tile_id_t *out, int max) {
	int n = test_active_tile_count < max ? test_active_tile_count : max;
	if (out && n > 0) {
		memcpy(out, test_active_tiles, (size_t)n * sizeof(music_tile_id_t));
	}
	return n;
}

static lv_obj_t *child_of_type(lv_obj_t *obj, const lv_obj_class_t *type) {
	for (uint32_t i = 0; i < lv_obj_get_child_count(obj); i++) {
		lv_obj_t *child = lv_obj_get_child(obj, i);
		if (lv_obj_check_type(child, type) && !lv_obj_is_floating(child)) {
			return child;
		}
	}
	assert(!"missing child");
	return NULL;
}

static lv_obj_t *find_label(lv_obj_t *obj, const char *text) {
	if (lv_obj_check_type(obj, &lv_label_class) && strcmp(lv_label_get_text(obj), text) == 0) {
		return obj;
	}
	for (uint32_t i = 0; i < lv_obj_get_child_count(obj); i++) {
		lv_obj_t *found = find_label(lv_obj_get_child(obj, i), text);
		if (found) {
			return found;
		}
	}
	return NULL;
}

/* The last label in the tree that says this, for a word a page uses twice:
 * "Custom" is both a theme on the Appearance page and, further down it, a boot
 * screen, and find_label() answers with the first. */
static lv_obj_t *find_label_last(lv_obj_t *obj, const char *text) {
	lv_obj_t *found = NULL;
	if (lv_obj_check_type(obj, &lv_label_class) && strcmp(lv_label_get_text(obj), text) == 0) {
		found = obj;
	}
	for (uint32_t i = 0; i < lv_obj_get_child_count(obj); i++) {
		lv_obj_t *deeper = find_label_last(lv_obj_get_child(obj, i), text);
		if (deeper) {
			found = deeper;
		}
	}
	return found;
}

/* Optional real-renderer diagnostics, kept out of Git. No display driver or
 * image library is needed; PPM can be opened by ordinary image tools. */
static void dump_screen_snapshot(lv_obj_t *screen, const char *path) {
	lv_draw_buf_t *shot = lv_snapshot_take(screen, LV_COLOR_FORMAT_RGB888);
	assert(shot);
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

static void dump_menu_snapshot(int kind) {
	const char *prefix = getenv("SONIX_MENU_SNAPSHOTS");
	if (prefix && prefix[0]) {
		char path[512];
		snprintf(path, sizeof(path), "%s-%d-theme%d.ppm", prefix, bp_screen_w(), kind);
		dump_screen_snapshot(main_menu_screen, path);
	}
}

static void dump_visibility_snapshot(int kind, int icons, unsigned mask) {
	const char *prefix = getenv("SONIX_MENU_VIS_SNAPSHOTS");
	// One representative arrangement for each count, not all 64 subsets.
	if (prefix && prefix[0] && (mask & (mask + 1u)) == 0) {
		char path[512];
		snprintf(path, sizeof(path), "%s-theme%d-icons%d-mask%u.ppm", prefix, kind, icons, mask);
		dump_screen_snapshot(main_menu_screen, path);
	}
}

static lv_obj_t *menu_toggle;
static void set_icons_only(bool enabled) {
	lv_obj_set_state(menu_toggle, LV_STATE_CHECKED, enabled);
	lv_obj_send_event(menu_toggle, LV_EVENT_VALUE_CHANGED, NULL);
	assert(main_menu_icons_only() == enabled);
}

/* Layout-only tests missed the previous blur: the box was the right size,
 * but LVGL still enlarged a tiny antialiased toolbar bitmap inside it. Verify
 * the real software renderer preserves the new source's pixels one-for-one,
 * including its alpha edges and theme tint, in ARGB8888 and the panel's RGB565. */
static void check_native_icon(lv_obj_t *icon, int tile, int size) {
	const lv_image_dsc_t *source = lv_image_get_src(icon);
	assert(source == main_menu_icon_native(tile, theme_uses_function_glyphs(), size));
	assert(source && source->header.w == size && source->header.h == size);
	assert(lv_image_get_scale_x(icon) == LV_SCALE_NONE);
	assert(lv_image_get_scale_y(icon) == LV_SCALE_NONE);
	assert(lv_image_get_rotation(icon) == 0);
	bool mask = source->header.cf == LV_COLOR_FORMAT_A8;
	assert(mask == theme_uses_function_glyphs());
	lv_color_t tint = lv_obj_get_style_image_recolor(icon, 0);
	lv_draw_buf_t *rgba = lv_snapshot_take(icon, LV_COLOR_FORMAT_ARGB8888);
	lv_draw_buf_t *rgb565 = lv_snapshot_take(icon, LV_COLOR_FORMAT_RGB565);
	assert(rgba && rgb565);
	assert(rgba->header.w == size && rgba->header.h == size);
	assert(rgb565->header.w == size && rgb565->header.h == size);
	int opaque_pixels = 0;
	for (int y = 0; y < size; y++) {
		const uint8_t *src = source->data + y * source->header.stride;
		const uint8_t *out = rgba->data + y * rgba->header.stride;
		const uint8_t *panel = rgb565->data + y * rgb565->header.stride;
		for (int x = 0; x < size; x++) {
			int alpha = mask ? src[x] : src[x * 4 + 3];
			/* LVGL treats near-transparent/opaque values as 0/255. Any
			 * interpolation, half-pixel shift or lost A8 mask fails this. */
			assert(abs(out[x * 4 + 3] - alpha) <= 3);
			if (alpha != 255) {
				continue;
			}
			opaque_pixels++;
			uint8_t r = mask ? tint.red : src[x * 4 + 2];
			uint8_t g = mask ? tint.green : src[x * 4 + 1];
			uint8_t b = mask ? tint.blue : src[x * 4];
			assert(out[x * 4] == b && out[x * 4 + 1] == g && out[x * 4 + 2] == r);
			uint16_t actual;
			memcpy(&actual, panel + x * 2, sizeof(actual));
			uint16_t expected = (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
			assert(actual == expected);
		}
	}
	assert(opaque_pixels > 0);
	lv_draw_buf_destroy(rgba);
	lv_draw_buf_destroy(rgb565);
}

static void check_native_assets(void) {
	for (int tile = 0; tile < 6; tile++) {
		for (int size = 1; size <= 173; size++) {
			const lv_image_dsc_t *glyph = main_menu_icon_native(tile, true, size);
			const lv_image_dsc_t *color = main_menu_icon_native(tile, false, size);
			assert((glyph != NULL) == (size <= 47 || size == 138 || size == 173));
			assert((color != NULL) == (size == 74 || size == 173));
			const lv_image_dsc_t *sources[] = {glyph, color};
			for (size_t i = 0; i < 2; i++) {
				const lv_image_dsc_t *src = sources[i];
				if (!src) {
					continue;
				}
				assert(src->header.magic == LV_IMAGE_HEADER_MAGIC);
				assert(src->header.w == size && src->header.h == size);
				assert(src->header.cf == (i == 0 ? LV_COLOR_FORMAT_A8 : LV_COLOR_FORMAT_ARGB8888));
				assert(src->header.stride % 4 == 0);
				assert(src->data_size == src->header.stride * src->header.h);
			}
		}
	}
	for (int tile = MAIN_MENU_ICON_BROWSE; tile <= MAIN_MENU_ICON_BOOKS; tile++) {
		assert(!main_menu_icon_native(tile, false, 74));
		for (int glyph = 0; glyph <= 1; glyph++) {
			const lv_image_dsc_t *art = main_menu_icon_native(tile, glyph != 0, 173);
			assert(art && art->header.w == 173 && art->header.h == 173);
			assert(art->header.cf == (glyph ? LV_COLOR_FORMAT_A8 : LV_COLOR_FORMAT_ARGB8888));
		}
	}
	assert(!main_menu_icon_native(-1, true, 47));
	assert(!main_menu_icon_native(8, false, 173));
	assert(!main_menu_icon_native(0, true, -1));
	assert(!main_menu_icon_native(0, true, 0));
	assert(!main_menu_icon_native(0, false, 174));
}

static void check_grid(lv_obj_t *grid, bool icons_only) {
	lv_obj_update_layout(grid);
	for (uint32_t i = 0; i < lv_obj_get_child_count(grid); i++) {
		lv_obj_t *tile = lv_obj_get_child(grid, i);
		lv_obj_t *label = child_of_type(tile, &lv_label_class);
		assert(lv_obj_is_hidden(label) == icons_only);
		assert(lv_label_get_text(label)[0]);
		if (lv_obj_get_parent(grid) == main_menu_screen) {
			/* Icon-only tiles must stay sharp: no visible halo, even if it
			 * already exists from the labeled layout or a previous theme. */
			int visible_halos = 0;
			for (uint32_t j = 0; j < lv_obj_get_child_count(tile); j++) {
				lv_obj_t *child = lv_obj_get_child(tile, j);
				if (lv_obj_is_floating(child) && !lv_obj_is_hidden(child)) {
					visible_halos++;
				}
			}
			bool glow_expected = !icons_only && bp_is_tempotec_v1() && theme_uses_function_glyphs() && (i % 2 == 0);
			assert(visible_halos == (glow_expected ? 1 : 0));
		}
		if (!icons_only) {
			continue;
		}
		lv_obj_t *icon = child_of_type(tile, &lv_image_class);
		int long_side = LV_MAX(lv_obj_get_width(icon), lv_obj_get_height(icon));
		int default_size = theme_uses_function_glyphs() ? bp_pick(22, 46) : bp_pick(46, 128);
		assert(long_side > default_size);
		/* Preserve OLED's earlier reduction, then shrink every theme's
		 * current icon-only size by 5%, rounded to the nearest pixel. */
		int available = LV_MAX(1, LV_MIN(lv_obj_get_width(tile), lv_obj_get_height(tile)) - bp_pick(12, 24));
		int previous_size = theme_is_oled() ? LV_MAX(1, available * 80 / 100) : available;
		int expected_size = LV_MAX(1, (previous_size * 95 + 50) / 100);
		assert(long_side == expected_size);
		check_native_icon(icon, (int)i, expected_size);
		lv_area_t t, image;
		lv_obj_get_coords(tile, &t);
		lv_obj_get_coords(icon, &image);
		assert(abs((t.x1 + t.x2) - (image.x1 + image.x2)) <= 2);
		assert(abs((t.y1 + t.y2) - (image.y1 + image.y2)) <= 2);
		assert(image.x1 >= t.x1 && image.x2 <= t.x2);
		assert(image.y1 >= t.y1 && image.y2 <= t.y2);
	}
}

typedef struct {
	int tile_w, tile_h, icon_w, icon_h, icon_x, icon_y;
	uint32_t scale;
	const void *source;
} tile_metrics_t;
static tile_metrics_t tile_metrics(lv_obj_t *tile) {
	lv_obj_update_layout(tile);
	lv_obj_t *icon = child_of_type(tile, &lv_image_class);
	return (tile_metrics_t){lv_obj_get_width(tile), lv_obj_get_height(tile),
		lv_obj_get_width(icon), lv_obj_get_height(icon), lv_obj_get_x(icon), lv_obj_get_y(icon),
		lv_image_get_scale(icon), lv_image_get_src(icon)};
}
static void check_metrics(lv_obj_t *tile, tile_metrics_t expected) {
	tile_metrics_t now = tile_metrics(tile);
	assert(now.tile_w == expected.tile_w && now.tile_h == expected.tile_h);
	assert(now.icon_w == expected.icon_w && now.icon_h == expected.icon_h);
	assert(now.icon_x == expected.icon_x && now.icon_y == expected.icon_y);
	assert(now.scale == expected.scale && now.source == expected.source);
}

/* The home footer draws the note/play glyph, then one line or two. With no
 * track loaded there is only the placeholder line, and it must sit level with
 * the glyph beside it, not half a line above it because the empty artist line
 * still reserved its space. A loaded track keeps title over artist. */
static void check_strip_text_lines(lv_obj_t *strip, int expected_lines, const char *first_line) {
	lv_obj_t *icon = NULL, *copy = NULL;
	for (uint32_t i = 0; i < lv_obj_get_child_count(strip); i++) {
		lv_obj_t *child = lv_obj_get_child(strip, i);
		if (lv_obj_is_hidden(child)) {
			continue; // the halo
		}
		if (lv_obj_check_type(child, &lv_image_class)) {
			icon = child;
		} else if (lv_obj_get_child_count(child) == 2 &&
				   lv_obj_check_type(lv_obj_get_child(child, 0), &lv_label_class)) {
			copy = child;
		}
	}
	assert(icon && copy);
	lv_label_t *title = (lv_label_t *)lv_obj_get_child(copy, 0);
	lv_obj_t *subtitle = lv_obj_get_child(copy, 1);
	assert(strcmp(lv_label_get_text((lv_obj_t *)title), first_line) == 0);
	lv_obj_update_layout(strip);

	int line_height = (int)lv_font_get_line_height(lv_obj_get_style_text_font((lv_obj_t *)title, 0));
	assert(lv_obj_get_height(copy) == line_height * expected_lines);
	if (expected_lines == 1) {
		/* The second line is out of the layout, and the surviving line is
		 * centred on the glyph rather than stacked from the top. */
		assert(lv_obj_is_hidden(subtitle));
		lv_area_t icon_area, title_area, copy_area;
		lv_obj_get_coords(icon, &icon_area);
		lv_obj_get_coords((lv_obj_t *)title, &title_area);
		lv_obj_get_coords(copy, &copy_area);
		assert(abs((title_area.y1 + title_area.y2) - (icon_area.y1 + icon_area.y2)) <= line_height / 2);
		assert(abs((copy_area.y1 + copy_area.y2) - (icon_area.y1 + icon_area.y2)) <= line_height / 2);
	} else {
		assert(!lv_obj_is_hidden(subtitle));
		lv_area_t title_area, subtitle_area;
		lv_obj_get_coords((lv_obj_t *)title, &title_area);
		lv_obj_get_coords(subtitle, &subtitle_area);
		assert(subtitle_area.y1 >= title_area.y2); // stacked, artist under title
	}
}

/* Validate every visible card against the actual LVGL flex geometry. In
 * particular, child indices are NOT visible positions after a card is hidden. */
static void check_visibility_geometry(lv_obj_t *grid, const gui_config_t *cfg, unsigned mask) {
	lv_obj_update_layout(main_menu_screen);
	assert(lv_obj_get_child_count(grid) == MAIN_MENU_COUNT);
	int count = 0;
	for (int i = 0; i < MAIN_MENU_COUNT; i++) {
		bool shown = (mask & (1u << i)) != 0;
		assert(main_menu_slot_visible((main_menu_slot_t)i) == shown);
		assert(lv_obj_is_hidden(lv_obj_get_child(grid, i)) == !shown);
		count += shown;
	}

	lv_obj_t *hint = find_label(main_menu_screen, tr("main_menu_empty"));
	assert(hint && lv_obj_is_hidden(hint) == (count != 0));
	assert(lv_obj_is_clickable(grid));
	lv_area_t grid_area;
	lv_obj_get_coords(grid, &grid_area);
	if (!count) {
		lv_area_t hint_area;
		lv_obj_get_coords(hint, &hint_area);
		assert(hint_area.x1 >= grid_area.x1 && hint_area.x2 <= grid_area.x2);
		assert(hint_area.y1 >= grid_area.y1 && hint_area.y2 <= grid_area.y2);
		return;
	}

	int columns = count <= 2 ? 1 : 2;
	int rows = count <= 2 ? count : (count + 1) / 2;
	int gap = bp_pick(6, 14);
	int usable_w = (int)cfg->screen_width - 2 * cfg->padding;
	int usable_h = lv_obj_get_height(grid) - 2 * cfg->padding;
	int tile_w = (usable_w - (columns - 1) * gap) / columns;
	int tile_h = (usable_h - (rows - 1) * gap) / rows;
	int position = 0;
	for (int i = 0; i < MAIN_MENU_COUNT; i++) {
		if (!(mask & (1u << i))) {
			continue;
		}
		lv_obj_t *tile = lv_obj_get_child(grid, i);
		lv_area_t area;
		lv_obj_get_coords(tile, &area);
		bool wide = columns == 1 || (count % columns == 1 && position == count - 1);
		assert(lv_obj_get_width(tile) == (wide ? usable_w : tile_w));
		assert(lv_obj_get_height(tile) == tile_h);
		assert(area.x1 == grid_area.x1 + cfg->padding + (position % columns) * (tile_w + gap));
		assert(area.y1 == grid_area.y1 + cfg->padding + (position / columns) * (tile_h + gap));
		assert(area.x2 <= grid_area.x2 - cfg->padding);
		assert(area.y2 <= grid_area.y2 - cfg->padding);

		lv_obj_t *icon = child_of_type(tile, &lv_image_class);
		lv_obj_t *label = child_of_type(tile, &lv_label_class);
		lv_area_t icon_area;
		lv_obj_get_coords(icon, &icon_area);
		assert(icon_area.x1 >= area.x1 && icon_area.x2 <= area.x2);
		assert(icon_area.y1 >= area.y1 && icon_area.y2 <= area.y2);
		assert(lv_obj_is_hidden(label) == main_menu_icons_only());
		bool reflowed = bp_is_tempotec_v1() && count <= 4 && !main_menu_icons_only();
		if (main_menu_icons_only() || (reflowed && theme_uses_function_glyphs())) {
			int art = i == MAIN_MENU_MUSIC && main_menu_browse() ? MAIN_MENU_ICON_BROWSE :
					  i == MAIN_MENU_LIBRARY && main_menu_books() ? MAIN_MENU_ICON_BOOKS : i;
			int size = LV_MAX(lv_obj_get_width(icon), lv_obj_get_height(icon));
			const lv_image_dsc_t *expected = main_menu_icon_native(art, theme_uses_function_glyphs(), size);
			if (!expected) expected = main_menu_icon_native(art, theme_uses_function_glyphs(), 173);
			assert(expected && lv_image_get_src(icon) == expected);
		}
		if (main_menu_icons_only()) {
			assert(abs((icon_area.x1 + icon_area.x2) - (area.x1 + area.x2)) <= 2);
			assert(abs((icon_area.y1 + icon_area.y2) - (area.y1 + area.y2)) <= 2);
		} else {
			lv_area_t label_area;
			lv_obj_get_coords(label, &label_area);
			assert(label_area.x1 >= area.x1 && label_area.x2 <= area.x2);
			assert(label_area.y1 >= area.y1 && label_area.y2 <= area.y2);
			if (reflowed) {
				assert(abs((icon_area.x1 + icon_area.x2) - (area.x1 + area.x2)) <= 2);
				assert(icon_area.y2 < label_area.y1); // art and one/two-line name do not collide
				int line_height = lv_font_get_line_height(bp_tile_label_font());
				assert(lv_obj_get_height(label) == line_height || lv_obj_get_height(label) == 2 * line_height);
			}
		}

		int visible_halos = 0;
		for (uint32_t j = 0; j < lv_obj_get_child_count(tile); j++) {
			lv_obj_t *child = lv_obj_get_child(tile, j);
			visible_halos += lv_obj_is_floating(child) && !lv_obj_is_hidden(child);
		}
		bool glow = !main_menu_icons_only() && bp_is_tempotec_v1() && theme_uses_function_glyphs() &&
				position % columns == 0;
		assert(visible_halos == (glow ? 1 : 0));
		position++;
	}
}

/* A custom SD-card theme shares the colored-art layout, but is selected and
 * refreshed through a different path from the five built-in themes. */
static void check_custom_visibility(gui_config_t *cfg, lv_obj_t *grid) {
	char root[] = "/tmp/sonix-menu-custom-XXXXXX";
	assert(mkdtemp(root));
	char dir[256], path[512];
	snprintf(dir, sizeof(dir), "%s/Themes", root);
	snprintf(path, sizeof(path), "%s/custom.ini", dir);
	assert(mkdir(dir, 0700) == 0);
	FILE *file = fopen(path, "w");
	assert(file);
	fputs("[theme]\nscreen_bg = #010203\nsurface = #101112\nsurface_pressed = #202122\n"
		"panel = #303132\ncover_bg = #404142\ntext_primary = #f0f1f2\n"
		"text_secondary = #a0a1a2\naccent = #123456\n", file);
	assert(fclose(file) == 0);
	theme_set_card_root(root);
	theme_set_kind(THEME_KIND_CUSTOM);
	assert(theme_get_kind() == THEME_KIND_CUSTOM);
	for (int icons = 0; icons <= 1; icons++) {
		set_icons_only(icons != 0);
		for (unsigned step = 0; step < (1u << MAIN_MENU_COUNT); step++) {
			unsigned mask = step ^ (step >> 1);
			for (int i = 0; i < MAIN_MENU_COUNT; i++) {
				assert(main_menu_set_slot_visible((main_menu_slot_t)i, (mask & (1u << i)) != 0));
			}
			check_visibility_geometry(grid, cfg, mask);
		}
	}
	theme_set_kind(THEME_KIND_DARK);
	theme_set_card_root(NULL);
	assert(unlink(path) == 0);
	assert(rmdir(dir) == 0);
	assert(rmdir(root) == 0);
}

static lv_obj_t *visibility_switch(lv_obj_t *items, main_menu_slot_t slot) {
	lv_obj_t *label = find_label(items, tr(main_menu_slot_label(slot)));
	assert(label);
	return child_of_type(lv_obj_get_parent(label), &lv_switch_class);
}

static void visibility_toggle(lv_obj_t *items, main_menu_slot_t slot, bool shown) {
	lv_obj_t *sw = visibility_switch(items, slot);
	lv_obj_set_state(sw, LV_STATE_CHECKED, shown);
	lv_obj_send_event(sw, LV_EVENT_VALUE_CHANGED, NULL);
	assert(main_menu_slot_visible(slot) == shown);
}

static lv_point_t touch_point;
static lv_indev_state_t touch_state;
static void fake_touch_read(lv_indev_t *indev, lv_indev_data_t *data) {
	(void)indev;
	data->point = touch_point;
	data->state = touch_state;
}

static void touch_read(lv_indev_t *indev, lv_indev_state_t state, int millis) {
	touch_state = state;
	lv_tick_inc((uint32_t)millis);
	lv_indev_read(indev);
}

static void touch_center(lv_obj_t *obj) {
	lv_area_t area;
	lv_obj_update_layout(lv_screen_active());
	lv_obj_get_coords(obj, &area);
	touch_point.x = (area.x1 + area.x2) / 2;
	touch_point.y = (area.y1 + area.y2) / 2;
}

static void open_visibility_page(lv_obj_t *items) {
	lv_screen_load(main_menu_screen);
	lv_screen_load(items); // dispatch SCREEN_LOADED, just as navigation does
}

static void check_home_visibility(gui_config_t *cfg, lv_obj_t *grid, const char *config, bool saved_icons_only) {
	lv_obj_t *items = appearance_main_menu_items_screen();
	assert(items);
	lv_obj_t *link = find_label(appearance_screen, tr("appearance_main_menu_items"));
	assert(link);
	lv_screen_load(appearance_screen);
	lv_obj_send_event(lv_obj_get_parent(link), LV_EVENT_CLICKED, NULL);
	assert(lv_screen_active() == items);
	assert(find_label(items, tr("appearance_main_menu_items_note")));
	const char *snapshot_prefix = getenv("SONIX_MENU_VIS_SNAPSHOTS");
	if (snapshot_prefix && snapshot_prefix[0]) {
		char path[512];
		snprintf(path, sizeof(path), "%s-controls.ppm", snapshot_prefix);
		lv_obj_update_layout(items);
		dump_screen_snapshot(items, path);
	}
	for (int i = 0; i < MAIN_MENU_COUNT; i++) {
		assert(main_menu_slot_visible((main_menu_slot_t)i));
		assert(lv_obj_has_state(visibility_switch(items, (main_menu_slot_t)i), LV_STATE_CHECKED));
	}
	assert(!main_menu_slot_visible((main_menu_slot_t)-1));
	assert(!main_menu_set_slot_visible((main_menu_slot_t)MAIN_MENU_COUNT, false));

	/* Turning a shortcut off and on saves only a UI preference. In particular,
	 * an already-disabled service must remain disabled and an enabled one on. */
	bool radio_was_enabled = addons_enabled("radio");
	assert(addons_set("radio", false));
	bool addon_states[64];
	assert(addons_count() <= (int)(sizeof(addon_states) / sizeof(addon_states[0])));
	for (int i = 0; i < addons_count(); i++) {
		addon_states[i] = addons_enabled(addons_at(i)->id);
	}
	visibility_toggle(items, MAIN_MENU_STREAMING, false);
	assert(lv_obj_is_hidden(lv_obj_get_child(grid, MAIN_MENU_STREAMING)));
	assert(config_init(config));
	assert(!main_menu_slot_visible(MAIN_MENU_STREAMING));
	assert(!addons_enabled("radio") && addons_enabled("qobuz"));
	visibility_toggle(items, MAIN_MENU_STREAMING, true);

	/* Settings itself can be hidden. A real pointer hold on a remaining tile
	 * opens the editor, and its release must not click through to Music. */
	visibility_toggle(items, MAIN_MENU_SETTINGS, false);
	lv_screen_load(main_menu_screen);
	check_visibility_geometry(grid, cfg, (1u << MAIN_MENU_SETTINGS) - 1u);
	lv_indev_t *touch = lv_indev_create();
	assert(touch);
	lv_indev_set_type(touch, LV_INDEV_TYPE_POINTER);
	lv_indev_set_display(touch, lv_display_get_default());
	lv_indev_set_read_cb(touch, fake_touch_read);
	lv_indev_set_long_press_time(touch, 500);
	touch_center(lv_obj_get_child(grid, MAIN_MENU_MUSIC));
	touch_read(touch, LV_INDEV_STATE_RELEASED, 20);
	touch_read(touch, LV_INDEV_STATE_PRESSED, 20);
	touch_read(touch, LV_INDEV_STATE_PRESSED, 600);
	assert(lv_screen_active() == items);
	touch_read(touch, LV_INDEV_STATE_RELEASED, 20);
	assert(lv_screen_active() == items);
	assert(!main_menu_slot_visible(MAIN_MENU_SETTINGS));

	/* All six controls, including those below the fold, must be reachable and
	 * work with real pointer taps rather than only synthetic change events. */
	for (int i = 0; i < MAIN_MENU_COUNT; i++) {
		main_menu_slot_t slot = (main_menu_slot_t)i;
		lv_obj_t *sw = visibility_switch(items, slot);
		lv_obj_scroll_to_view_recursive(sw, LV_ANIM_OFF);
		lv_obj_update_layout(items);
		lv_area_t area;
		lv_obj_get_coords(sw, &area);
		assert(area.y1 >= settingsrow_content_top(cfg) && area.y2 < (int)cfg->screen_height);
		bool was_shown = main_menu_slot_visible(slot);
		touch_center(sw);
		touch_read(touch, LV_INDEV_STATE_PRESSED, 20);
		touch_read(touch, LV_INDEV_STATE_RELEASED, 20);
		assert(main_menu_slot_visible(slot) != was_shown);
		touch_read(touch, LV_INDEV_STATE_PRESSED, 20);
		touch_read(touch, LV_INDEV_STATE_RELEASED, 20);
		assert(main_menu_slot_visible(slot) == was_shown);
	}

	/* Do not open the editor when the hold is the tail of another gesture. */
	lv_screen_load(main_menu_screen);
	test_player_drag = true;
	lv_obj_send_event(grid, LV_EVENT_LONG_PRESSED, NULL);
	assert(lv_screen_active() == main_menu_screen);
	test_player_drag = false;
	test_back_drag = true;
	lv_obj_send_event(grid, LV_EVENT_LONG_PRESSED, NULL);
	assert(lv_screen_active() == main_menu_screen);
	test_back_drag = false;
	test_coverflow_drag = true;
	lv_obj_send_event(grid, LV_EVENT_LONG_PRESSED, NULL);
	assert(lv_screen_active() == main_menu_screen);
	test_coverflow_drag = false;

	/* All six can be hidden, including Settings. The empty-state hint sits on
	 * the grid, which still receives a real hold and restores any switch. */
	open_visibility_page(items);
	for (int i = 0; i < MAIN_MENU_COUNT; i++) {
		visibility_toggle(items, (main_menu_slot_t)i, false);
	}
	for (int kind = 0; kind < THEME_KIND_COUNT; kind++) {
		theme_set_kind((theme_kind_t)kind);
		for (int icons = 0; icons <= 1; icons++) {
			set_icons_only(icons != 0);
			lv_screen_load(main_menu_screen);
			check_visibility_geometry(grid, cfg, 0);
			touch_center(grid);
			touch_read(touch, LV_INDEV_STATE_RELEASED, 20);
			touch_read(touch, LV_INDEV_STATE_PRESSED, 20);
			touch_read(touch, LV_INDEV_STATE_PRESSED, 600);
			assert(lv_screen_active() == items);
			touch_read(touch, LV_INDEV_STATE_RELEASED, 20);
			assert(lv_screen_active() == items);
			for (int i = 0; i < MAIN_MENU_COUNT; i++) {
				assert(!lv_obj_has_state(visibility_switch(items, (main_menu_slot_t)i), LV_STATE_CHECKED));
			}
		}
	}
	for (int i = 0; i < MAIN_MENU_COUNT; i++) {
		visibility_toggle(items, (main_menu_slot_t)i, true);
	}
	/* Both action tiles and direct screen targets must bubble a hold without
	 * navigating on release, in labeled and icon-only layouts. */
	const theme_kind_t touch_themes[] = {THEME_KIND_DARK, THEME_KIND_TOKYO_NIGHT};
	for (size_t k = 0; k < sizeof(touch_themes) / sizeof(touch_themes[0]); k++) {
		theme_set_kind(touch_themes[k]);
		for (int icons = 0; icons <= 1; icons++) {
			set_icons_only(icons != 0);
			for (int i = 0; i < MAIN_MENU_COUNT; i++) {
				lv_screen_load(main_menu_screen);
				touch_center(lv_obj_get_child(grid, i));
				touch_read(touch, LV_INDEV_STATE_PRESSED, 20);
				touch_read(touch, LV_INDEV_STATE_PRESSED, 600);
				assert(lv_screen_active() == items);
				touch_read(touch, LV_INDEV_STATE_RELEASED, 20);
				assert(lv_screen_active() == items);
			}
		}
	}
	theme_set_kind(THEME_KIND_DARK);
	set_icons_only(false);
	/* A short tap still opens each destination after the holds. */
	lv_obj_t *tap_destinations[] = {music_screen, streaming_screen, wireless_screen,
			audiobooks_screen, morepage_screen, settings_screen};
	for (int i = 0; i < MAIN_MENU_COUNT; i++) {
		lv_screen_load(main_menu_screen);
		touch_center(lv_obj_get_child(grid, i));
		touch_read(touch, LV_INDEV_STATE_PRESSED, 20);
		touch_read(touch, LV_INDEV_STATE_RELEASED, 20);
		assert(lv_screen_active() == tap_destinations[i]);
	}
	lv_indev_delete(touch);

	/* Failed saves roll back the switch and tile, with a visible error. */
	open_visibility_page(items);
	char blocked[512];
	assert(snprintf(blocked, sizeof(blocked), "%s.tmp", config) < (int)sizeof(blocked));
	assert(mkdir(blocked, 0700) == 0);
	int old_errors = error_toasts;
	lv_obj_t *sw = visibility_switch(items, MAIN_MENU_MUSIC);
	lv_obj_set_state(sw, LV_STATE_CHECKED, false);
	lv_obj_send_event(sw, LV_EVENT_VALUE_CHANGED, NULL);
	assert(error_toasts == old_errors + 1);
	assert(lv_obj_has_state(sw, LV_STATE_CHECKED));
	assert(main_menu_slot_visible(MAIN_MENU_MUSIC));
	assert(!lv_obj_is_hidden(lv_obj_get_child(grid, MAIN_MENU_MUSIC)));
	assert(rmdir(blocked) == 0);
	assert(config_save());

	/* Swapped destinations have independent preferences; labels and switches
	 * follow the active destination on the next visit to the editor. */
	main_menu_set_browse(true);
	open_visibility_page(items);
	assert(find_label(items, tr("music_browse")));
	visibility_toggle(items, MAIN_MENU_MUSIC, false);
	main_menu_set_browse(false);
	assert(main_menu_slot_visible(MAIN_MENU_MUSIC));
	open_visibility_page(items);
	visibility_toggle(items, MAIN_MENU_MUSIC, false);
	main_menu_set_browse(true);
	assert(!main_menu_slot_visible(MAIN_MENU_MUSIC));
	open_visibility_page(items);
	visibility_toggle(items, MAIN_MENU_MUSIC, true);
	main_menu_set_browse(false);
	assert(!main_menu_slot_visible(MAIN_MENU_MUSIC));
	open_visibility_page(items);
	visibility_toggle(items, MAIN_MENU_MUSIC, true);

	assert(addons_enabled("ebook"));
	main_menu_set_books(true);
	open_visibility_page(items);
	assert(find_label(items, tr("books")));
	visibility_toggle(items, MAIN_MENU_LIBRARY, false);
	main_menu_set_books(false);
	assert(main_menu_slot_visible(MAIN_MENU_LIBRARY));
	open_visibility_page(items);
	visibility_toggle(items, MAIN_MENU_LIBRARY, false);
	main_menu_set_books(true);
	assert(!main_menu_slot_visible(MAIN_MENU_LIBRARY));
	open_visibility_page(items);
	visibility_toggle(items, MAIN_MENU_LIBRARY, true);
	main_menu_set_books(false);
	assert(!main_menu_slot_visible(MAIN_MENU_LIBRARY));
	open_visibility_page(items);
	visibility_toggle(items, MAIN_MENU_LIBRARY, true);

	/* A disabled Books plugin falls back to Audiobooks, without losing either
	 * visibility choice. Re-enabling the module restores the Books preference. */
	main_menu_set_books(true);
	open_visibility_page(items);
	visibility_toggle(items, MAIN_MENU_LIBRARY, false);
	assert(addons_set("ebook", false));
	fonts_set_text_size(FONTS_TEXT_LARGE); // simulate a later page/theme refresh
	assert(!main_menu_books() && main_menu_slot_visible(MAIN_MENU_LIBRARY));
	assert(!lv_obj_is_hidden(lv_obj_get_child(grid, MAIN_MENU_LIBRARY)));
	assert(find_label(lv_obj_get_child(grid, MAIN_MENU_LIBRARY), tr("audiobooks")));
	assert(addons_set("ebook", true));
	fonts_set_text_size(FONTS_TEXT_NORMAL);
	assert(main_menu_books() && !main_menu_slot_visible(MAIN_MENU_LIBRARY));
	open_visibility_page(items);
	visibility_toggle(items, MAIN_MENU_LIBRARY, true);
	main_menu_set_books(false);

	/* Alternate destinations also use the sharp large sources when their cards
	 * grow, in both coloured-art and function-glyph themes. */
	main_menu_set_browse(true);
	main_menu_set_books(true);
	for (int i = 0; i < MAIN_MENU_COUNT; i++) {
		if (i != MAIN_MENU_MUSIC && i != MAIN_MENU_LIBRARY) {
			assert(main_menu_set_slot_visible((main_menu_slot_t)i, false));
		}
	}
	for (int kind = 0; kind < THEME_KIND_COUNT; kind++) {
		theme_set_kind((theme_kind_t)kind);
		for (int icons = 0; icons <= 1; icons++) {
			set_icons_only(icons != 0);
			check_visibility_geometry(grid, cfg, (1u << MAIN_MENU_MUSIC) | (1u << MAIN_MENU_LIBRARY));
			assert(main_menu_set_slot_visible(MAIN_MENU_LIBRARY, false));
			check_visibility_geometry(grid, cfg, 1u << MAIN_MENU_MUSIC);
			assert(main_menu_set_slot_visible(MAIN_MENU_MUSIC, false));
			assert(main_menu_set_slot_visible(MAIN_MENU_LIBRARY, true));
			check_visibility_geometry(grid, cfg, 1u << MAIN_MENU_LIBRARY);
			assert(main_menu_set_slot_visible(MAIN_MENU_MUSIC, true));
		}
	}
	for (int i = 0; i < MAIN_MENU_COUNT; i++) {
		assert(main_menu_set_slot_visible((main_menu_slot_t)i, true));
	}
	main_menu_set_browse(false);
	main_menu_set_books(false);

	/* All 64 subsets, both caption modes and every theme. Gray-code order
	 * changes only one slot at a time, exercising live reflow in both
	 * directions without rebuilding the tile objects. */
	lv_screen_load(main_menu_screen);
	for (int kind = 0; kind < THEME_KIND_COUNT; kind++) {
		theme_set_kind((theme_kind_t)kind);
		for (int icons = 0; icons <= 1; icons++) {
			set_icons_only(icons != 0);
			for (unsigned step = 0; step < (1u << MAIN_MENU_COUNT); step++) {
				unsigned mask = step ^ (step >> 1);
				for (int i = 0; i < MAIN_MENU_COUNT; i++) {
					assert(main_menu_set_slot_visible((main_menu_slot_t)i, (mask & (1u << i)) != 0));
				}
				check_visibility_geometry(grid, cfg, mask);
				dump_visibility_snapshot(kind, icons, mask);
				if (step == 7 || step == 32) {
					fonts_set_text_size(FONTS_TEXT_LARGE);
					check_visibility_geometry(grid, cfg, mask);
					fonts_set_text_size(FONTS_TEXT_NORMAL);
					check_visibility_geometry(grid, cfg, mask);
				}
			}
		}
	}
	check_custom_visibility(cfg, grid);
	/* The recovery hint and editor are translated even when every tile is
	 * hidden, including after changing languages while the screens exist. */
	for (int i = 0; i < MAIN_MENU_COUNT; i++) {
		assert(main_menu_set_slot_visible((main_menu_slot_t)i, false));
	}
	const char *languages[] = {"English", "Deutsch", "Español", "Français", "Italiano", "Japanese",
		"Nederlands", "Polski", "Português BR", "Russian", "Ukrainian"};
	for (int kind = 0; kind < THEME_KIND_COUNT; kind++) {
		theme_set_kind((theme_kind_t)kind);
		for (size_t n = 0; n < sizeof(languages) / sizeof(languages[0]); n++) {
			assert(lang_set(languages[n]));
			assert(strcmp(tr("appearance_main_menu_items"), "appearance_main_menu_items") != 0);
			assert(strcmp(tr("appearance_main_menu_items_note"), "appearance_main_menu_items_note") != 0);
			assert(strcmp(tr("main_menu_empty"), "main_menu_empty") != 0);
			for (int size = FONTS_TEXT_NORMAL; size <= FONTS_TEXT_LARGE; size++) {
				fonts_set_text_size(size);
				lv_screen_load(main_menu_screen);
				check_visibility_geometry(grid, cfg, 0);
				lv_screen_load(items);
				lv_obj_update_layout(items);
				lv_obj_t *heading = settingsrow_page_title(items);
				assert(heading && strcmp(lv_label_get_text(heading), tr("appearance_main_menu_items")) == 0);
				lv_point_t title_size;
				lv_text_get_size(&title_size, tr("appearance_main_menu_items"),
								 lv_obj_get_style_text_font(heading, LV_PART_MAIN), 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
				assert(title_size.x <= lv_obj_get_width(heading));
				lv_area_t heading_area;
				lv_obj_get_coords(heading, &heading_area);
				assert(heading_area.y1 >= (int)cfg->top_bar_height && heading_area.y2 < settingsrow_content_top(cfg));
				assert(find_label(items, tr("appearance_main_menu_items_note")));
				for (int i = 0; i < MAIN_MENU_COUNT; i++) {
					lv_obj_t *sw = visibility_switch(items, (main_menu_slot_t)i);
					assert(!lv_obj_has_state(sw, LV_STATE_CHECKED));
					lv_obj_t *row = lv_obj_get_parent(sw);
					lv_obj_t *label = child_of_type(row, &lv_label_class);
					lv_area_t row_area, label_area, switch_area;
					lv_obj_get_coords(row, &row_area);
					lv_obj_get_coords(label, &label_area);
					lv_obj_get_coords(sw, &switch_area);
					assert(label_area.y1 >= row_area.y1 && label_area.y2 <= row_area.y2);
					assert(label_area.x2 - lv_obj_get_style_pad_right(label, LV_PART_MAIN) < switch_area.x1);
					assert(switch_area.y1 >= row_area.y1 && switch_area.y2 <= row_area.y2);
				}
			}
		}
	}
	fonts_set_text_size(FONTS_TEXT_NORMAL);
	/* Four tall cards give their names the full width and, when needed, a
	 * second line. Check retranslation on return from another page too. */
	const unsigned text_mask = (1u << MAIN_MENU_STREAMING) | (1u << MAIN_MENU_WIRELESS) |
			(1u << MAIN_MENU_LIBRARY) | (1u << MAIN_MENU_SETTINGS);
	for (int i = 0; i < MAIN_MENU_COUNT; i++) {
		assert(main_menu_set_slot_visible((main_menu_slot_t)i, (text_mask & (1u << i)) != 0));
	}
	set_icons_only(false);
	bool saw_wrapped_caption = false;
	for (int kind = 0; kind < THEME_KIND_COUNT; kind++) {
		theme_set_kind((theme_kind_t)kind);
		for (size_t n = 0; n < sizeof(languages) / sizeof(languages[0]); n++) {
			assert(lang_set(languages[n]));
			lv_screen_load(items);
			lv_screen_load(main_menu_screen);
			check_visibility_geometry(grid, cfg, text_mask);
			for (int i = 0; i < MAIN_MENU_COUNT; i++) {
				if (!(text_mask & (1u << i))) continue;
				lv_obj_t *label = child_of_type(lv_obj_get_child(grid, i), &lv_label_class);
				assert(strcmp(lv_label_get_text(label), tr(main_menu_slot_label((main_menu_slot_t)i))) == 0);
				saw_wrapped_caption |= lv_obj_get_height(label) == 2 * lv_font_get_line_height(bp_tile_label_font());
			}
		}
	}
	assert(saw_wrapped_caption);
	/* Extreme line metrics from a card-supplied face must not make a two-line
	 * caption overlap its image; it may fall back to one ellipsized line. */
	assert(lang_set("Polski"));
	for (int kind = 0; kind < THEME_KIND_COUNT; kind++) {
		theme_set_kind((theme_kind_t)kind);
		for (int line_height = 15; line_height <= 32; line_height++) {
			font_ui_14.line_height = line_height;
			for (int i = 0; i < font_callback_count; i++) {
				font_callbacks[i]();
			}
			check_visibility_geometry(grid, cfg, text_mask);
		}
	}
	fonts_set_text_size(FONTS_TEXT_NORMAL);
	assert(lang_set("English"));
	for (int i = 0; i < MAIN_MENU_COUNT; i++) {
		assert(main_menu_set_slot_visible((main_menu_slot_t)i, true));
	}
	for (int i = 0; i < addons_count(); i++) {
		assert(addons_enabled(addons_at(i)->id) == addon_states[i]);
	}
	assert(addons_set("radio", radio_was_enabled));
	set_icons_only(saved_icons_only);
	theme_set_kind(THEME_KIND_DARK);
	lv_screen_load(main_menu_screen);
}

int main(int argc, char **argv) {
	assert(argc == 2 || argc == 3);
	bool saved_icons_only = argc == 3 && strcmp(argv[2], "saved") == 0;
	bool saved_visibility = argc == 3 && strcmp(argv[2], "hidden") == 0;
	bool saved_empty = argc == 3 && strcmp(argv[2], "empty") == 0;
	assert(argc == 2 || saved_icons_only || saved_visibility || saved_empty);
	char dir[] = "/tmp/sonix-menu-test-XXXXXX";
	assert(mkdtemp(dir));
	char config[256];
	snprintf(config, sizeof(config), "%s/device_config.ini", dir);
	config_init(config);
	assert(!main_menu_icons_only()); // no key: preserve the existing menu
	if (saved_icons_only) {
		config_set_bool("ui", "main_menu_icons_only", true);
	}
	if (saved_visibility) {
		config_set_bool("home_menu", "music", false);
		config_set_bool("home_menu", "more", false);
		config_set_bool("home_menu", "settings", false);
	}
	if (saved_empty) {
		const char *keys[] = {"music", "streaming", "wireless", "audiobooks", "more", "settings"};
		for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
			config_set_bool("home_menu", keys[i], false);
		}
	}
	if (saved_icons_only || saved_visibility || saved_empty) {
		assert(config_save());
		assert(config_init(config));
	}
	config_set("ui", "language", "English");
	lang_init(argv[1]);
	assert(strcmp(tr("appearance_main_menu_icons_only"), "Main menu icons only") == 0);
	init_fonts();
	lv_init();
	check_native_assets();
	gui_config_t cfg = {bp_screen_w(), bp_screen_h(), bp_padding(), bp_status_bar_h(), "/tmp"};
	lv_display_create(cfg.screen_width, cfg.screen_height);
	theme_init();
	check_custom_theme();
	// As gui_init does on a device that is already set up: every module on.
	addons_seed(false);
	music_screen = lv_obj_create(NULL);
	streaming_screen = lv_obj_create(NULL);
	wireless_screen = lv_obj_create(NULL);
	audiobooks_screen = lv_obj_create(NULL);
	ebookpage_screen = lv_obj_create(NULL);
	browser_screen = lv_obj_create(NULL);
	morepage_screen = lv_obj_create(NULL);
	settings_screen = lv_obj_create(NULL);
	player_screen = lv_obj_create(NULL);
	main_menu_screen = lv_obj_create(NULL);
	main_menu_init(&cfg);
	lv_screen_load(main_menu_screen);
	lv_obj_t *grid = lv_obj_get_child(main_menu_screen, 0);
	if (saved_empty) {
		check_visibility_geometry(grid, &cfg, 0);
	}

	bootpicpage_screen = lv_obj_create(NULL); // the page the boot screen's Custom pill opens
	bootpicpage_init(&cfg);
	appearance_screen = lv_obj_create(NULL); // after the page: its row points at it
	appearance_init(&cfg);
	if (saved_empty) {
		/* At startup the empty grid exists before Appearance is built. Once all
		 * screens are ready, a real hold must still reach the editor and allow
		 * the saved choices to be restored. */
		lv_obj_t *items = appearance_main_menu_items_screen();
		lv_indev_t *touch = lv_indev_create();
		assert(items && touch);
		lv_indev_set_type(touch, LV_INDEV_TYPE_POINTER);
		lv_indev_set_display(touch, lv_display_get_default());
		lv_indev_set_read_cb(touch, fake_touch_read);
		lv_indev_set_long_press_time(touch, 500);
		touch_center(grid);
		touch_read(touch, LV_INDEV_STATE_RELEASED, 20);
		touch_read(touch, LV_INDEV_STATE_PRESSED, 20);
		touch_read(touch, LV_INDEV_STATE_PRESSED, 600);
		assert(lv_screen_active() == items);
		touch_read(touch, LV_INDEV_STATE_RELEASED, 20);
		assert(lv_screen_active() == items);
		for (int i = 0; i < MAIN_MENU_COUNT; i++) {
			assert(!lv_obj_has_state(visibility_switch(items, (main_menu_slot_t)i), LV_STATE_CHECKED));
			visibility_toggle(items, (main_menu_slot_t)i, true);
		}
		lv_indev_delete(touch);
		lv_screen_load(main_menu_screen);
	}
	if (saved_visibility) {
		unsigned mask = (1u << MAIN_MENU_STREAMING) | (1u << MAIN_MENU_WIRELESS) | (1u << MAIN_MENU_LIBRARY);
		check_visibility_geometry(grid, &cfg, mask);
		assert(main_menu_set_slot_visible(MAIN_MENU_MUSIC, true));
		assert(main_menu_set_slot_visible(MAIN_MENU_MORE, true));
		assert(main_menu_set_slot_visible(MAIN_MENU_SETTINGS, true));
	}
	check_grid(grid, saved_icons_only);

	/* The compact home footer (Tokyo Night / Amber / OLED only): the placeholder
	 * line must be centred on the glyph beside it, and a playing track must keep
	 * its title-over-artist pair. */
	lv_obj_t *strip = lv_obj_get_child(main_menu_screen, 3);
	for (int kind = 0; kind < THEME_KIND_COUNT; kind++) {
		theme_set_kind((theme_kind_t)kind);
		lv_obj_update_layout(main_menu_screen);
		if (lv_obj_is_hidden(strip)) {
			continue; // Dark and Light have no footer
		}
		memset(&strip_state, 0, sizeof(strip_state));
		lv_obj_send_event(main_menu_screen, LV_EVENT_SCREEN_LOADED, NULL);
		check_strip_text_lines(strip, 1, tr("player_no_track_loaded"));

		snprintf(strip_state.current_file, sizeof(strip_state.current_file), "/music/a.flac");
		snprintf(strip_state.metadata.title, sizeof(strip_state.metadata.title), "Good Light");
		snprintf(strip_state.metadata.artist, sizeof(strip_state.metadata.artist), "Children of Zeus");
		strip_state.status = AUDIO_STATUS_PLAYING;
		lv_obj_send_event(main_menu_screen, LV_EVENT_SCREEN_LOADED, NULL);
		check_strip_text_lines(strip, 2, "Good Light");
		assert(strcmp(lv_label_get_text(lv_obj_get_child(lv_obj_get_child(strip, 2), 1)),
					  "Children of Zeus") == 0);

		/* Dropping back to nothing must restore the centred placeholder, and a
		 * later text-size change must not leave it stacked again. */
		memset(&strip_state, 0, sizeof(strip_state));
		lv_obj_send_event(main_menu_screen, LV_EVENT_SCREEN_LOADED, NULL);
		check_strip_text_lines(strip, 1, tr("player_no_track_loaded"));
		fonts_set_text_size(FONTS_TEXT_LARGE);
		check_strip_text_lines(strip, 1, tr("player_no_track_loaded"));
		fonts_set_text_size(FONTS_TEXT_NORMAL);
		check_strip_text_lines(strip, 1, tr("player_no_track_loaded"));
	}
	theme_set_kind(THEME_KIND_DARK);

	lv_obj_t *toggle_label = find_label(appearance_screen, tr("appearance_main_menu_icons_only"));
	assert(toggle_label);
	menu_toggle = child_of_type(lv_obj_get_parent(toggle_label), &lv_switch_class);
	assert(lv_obj_has_state(menu_toggle, LV_STATE_CHECKED) == saved_icons_only);
	check_home_visibility(&cfg, grid, config, saved_icons_only);

	assert(!main_menu_books());
	assert(find_label(lv_obj_get_child(grid, 3), tr("audiobooks")));
	// Which edge each sheet is pulled from is a choice, and the old arrangement is
	// the default: nothing moves under anyone's thumb because of an update.
	assert(!gestures_stock());
	lv_obj_t *gesture_label = find_label(appearance_screen, tr("appearance_stock_gestures"));
	assert(gesture_label);
	lv_obj_t *gesture_toggle = child_of_type(lv_obj_get_parent(gesture_label), &lv_switch_class);
	assert(gesture_toggle && !lv_obj_has_state(gesture_toggle, LV_STATE_CHECKED));

	lv_obj_t *books_label = find_label(appearance_screen, tr("appearance_home_books"));
	assert(books_label);
	lv_obj_t *books_toggle = child_of_type(lv_obj_get_parent(books_label), &lv_switch_class);
	assert(!lv_obj_has_state(books_toggle, LV_STATE_CHECKED));
	lv_obj_set_state(books_toggle, LV_STATE_CHECKED, true);
	lv_obj_send_event(books_toggle, LV_EVENT_VALUE_CHANGED, NULL);
	assert(main_menu_books());
	assert(find_label(lv_obj_get_child(grid, 3), tr("books")));
	assert(config_get_bool("ui", "home_books", false));
	lv_obj_set_state(books_toggle, LV_STATE_CHECKED, false);
	lv_obj_send_event(books_toggle, LV_EVENT_VALUE_CHANGED, NULL);
	assert(!main_menu_books());
	assert(find_label(lv_obj_get_child(grid, 3), tr("audiobooks")));

	assert(!main_menu_browse());
	assert(find_label(lv_obj_get_child(grid, 0), tr("music")));
	lv_obj_t *browse_label = find_label(appearance_screen, tr("appearance_home_browse"));
	assert(browse_label);
	lv_obj_t *browse_toggle = child_of_type(lv_obj_get_parent(browse_label), &lv_switch_class);
	assert(!lv_obj_has_state(browse_toggle, LV_STATE_CHECKED));
	lv_obj_set_state(browse_toggle, LV_STATE_CHECKED, true);
	lv_obj_send_event(browse_toggle, LV_EVENT_VALUE_CHANGED, NULL);
	assert(main_menu_browse());
	assert(find_label(lv_obj_get_child(grid, 0), tr("music_browse")));
	lv_obj_send_event(lv_obj_get_child(grid, 0), LV_EVENT_CLICKED, NULL);
	assert(lv_screen_active() == browser_screen);
	lv_screen_load(main_menu_screen);
	lv_obj_set_state(browse_toggle, LV_STATE_CHECKED, false);
	lv_obj_send_event(browse_toggle, LV_EVENT_VALUE_CHANGED, NULL);
	assert(!main_menu_browse());
	assert(find_label(lv_obj_get_child(grid, 0), tr("music")));

	assert(bp_screen_corner_radius() == (bp_is_tempotec_v1() ? 16 : 28));
	assert(!corners_enabled());
	lv_obj_t *corners_label = find_label(appearance_screen, tr("appearance_rounded_corners"));
	assert(corners_label);
	lv_obj_t *corners_card = lv_obj_get_parent(corners_label);
	lv_obj_t *corners_toggle = child_of_type(corners_card, &lv_switch_class);
	lv_obj_t *corners_slider = child_of_type(corners_card, &lv_slider_class);
	assert(!lv_obj_has_state(corners_toggle, LV_STATE_CHECKED));
	/* Off, there is nothing to size: the card is an ordinary toggle row. */
	assert(lv_obj_is_hidden(corners_slider));
	lv_obj_set_state(corners_toggle, LV_STATE_CHECKED, true);
	lv_obj_send_event(corners_toggle, LV_EVENT_VALUE_CHANGED, NULL);
	assert(corners_enabled());
	assert(config_get_int("ui", "rounded_corners", 0) == 1);

	/* On, the slider opens, standing at the arc that was always there. */
	assert(!lv_obj_is_hidden(corners_slider));
	assert(corners_radius() == bp_screen_corner_radius());
	assert(lv_slider_get_value(corners_slider) == (corners_radius() - CORNERS_RADIUS_MIN) / CORNERS_RADIUS_STEP);
	char corners_value[16];
	snprintf(corners_value, sizeof(corners_value), "%d px", bp_screen_corner_radius());
	assert(find_label(corners_card, corners_value));

	/* Every stop: the arc follows the slider, the number beside it says so,
	 * and the choice is written down -- without ever switching them off. */
	for (int radius = CORNERS_RADIUS_MIN; radius <= CORNERS_RADIUS_MAX; radius += CORNERS_RADIUS_STEP) {
		lv_slider_set_value(corners_slider, (radius - CORNERS_RADIUS_MIN) / CORNERS_RADIUS_STEP, LV_ANIM_OFF);
		lv_obj_send_event(corners_slider, LV_EVENT_VALUE_CHANGED, NULL);
		assert(corners_radius() == radius);
		assert(config_get_int("ui", "corner_radius", -1) == radius);
		snprintf(corners_value, sizeof(corners_value), "%d px", radius);
		assert(find_label(corners_card, corners_value));
		assert(corners_enabled());
	}

	/* Off puts the slider away but keeps the arc: back on, they come back as
	 * they were left rather than as 16 again. */
	lv_slider_set_value(corners_slider, 3, LV_ANIM_OFF);
	lv_obj_send_event(corners_slider, LV_EVENT_VALUE_CHANGED, NULL);
	assert(corners_radius() == CORNERS_RADIUS_MIN + 3 * CORNERS_RADIUS_STEP);
	lv_obj_set_state(corners_toggle, LV_STATE_CHECKED, false);
	lv_obj_send_event(corners_toggle, LV_EVENT_VALUE_CHANGED, NULL);
	assert(!corners_enabled());
	assert(config_get_int("ui", "rounded_corners", 0) == 0);
	assert(lv_obj_is_hidden(corners_slider));
	assert(corners_radius() == CORNERS_RADIUS_MIN + 3 * CORNERS_RADIUS_STEP);
	lv_obj_set_state(corners_toggle, LV_STATE_CHECKED, true);
	lv_obj_send_event(corners_toggle, LV_EVENT_VALUE_CHANGED, NULL);
	assert(corners_enabled() && !lv_obj_is_hidden(corners_slider));
	assert(corners_radius() == CORNERS_RADIUS_MIN + 3 * CORNERS_RADIUS_STEP);

	/* Leave them as the rest of the test found them: 16 px, off. */
	lv_slider_set_value(corners_slider, (bp_screen_corner_radius() - CORNERS_RADIUS_MIN) / CORNERS_RADIUS_STEP, LV_ANIM_OFF);
	lv_obj_send_event(corners_slider, LV_EVENT_VALUE_CHANGED, NULL);
	assert(corners_radius() == bp_screen_corner_radius());
	lv_obj_set_state(corners_toggle, LV_STATE_CHECKED, false);
	lv_obj_send_event(corners_toggle, LV_EVENT_VALUE_CHANGED, NULL);
	assert(!corners_enabled());
	assert(config_get_int("ui", "rounded_corners", 0) == 0);

	/* ---- the boot screen, and the page its Custom pill opens ---------------- */

	/* Custom with no picture behind it is a request to go and pick one, not a
	 * choice: the page opens instead of the marker being written. */
	lv_obj_t *custom_pill = find_label_last(appearance_screen, tr("appearance_boot_custom"));
	assert(custom_pill);
	lv_obj_send_event(lv_obj_get_parent(custom_pill), LV_EVENT_CLICKED, NULL);
	assert(lv_screen_active() == bootpicpage_screen);
	assert(boot_choice != BOOTLOGO_CUSTOM);
	lv_screen_load(main_menu_screen);

	/* An empty folder: the page says so, and lists nothing. */
	bootpic_count = 0;
	lv_obj_send_event(bootpicpage_screen, LV_EVENT_SCREEN_LOADED, NULL);
	lv_obj_update_layout(bootpicpage_screen);
	assert(!find_label(bootpicpage_screen, "a.jpg"));

	/* Two pictures on the card, read again on the way in. */
	bootpic_names[0] = "a.jpg";
	bootpic_names[1] = "b.png";
	bootpic_count = 2;
	lv_obj_send_event(bootpicpage_screen, LV_EVENT_SCREEN_LOADED, NULL);
	lv_obj_update_layout(bootpicpage_screen);
	lv_obj_t *a_label = find_label(bootpicpage_screen, "a.jpg");
	assert(a_label);
	assert(find_label(bootpicpage_screen, "b.png"));
	assert(find_label(bootpicpage_screen, "640 x 480")); /* its size, under the name */

	/* Picking one makes the copy on a thread of its own, and the boot screen
	 * follows it once the copy is there. */
	lv_obj_send_event(lv_obj_get_parent(a_label), LV_EVENT_CLICKED, NULL);
	assert(toast_busy_text); /* the page is not silent while the copy is made */
	for (int i = 0; i < 200 && bootpic_installed < 0; i++) {
		usleep(10 * 1000);
	}
	assert(bootpic_installed == 0);
	drain_post(); /* the result, as the interface thread would take it */
	assert(!toast_busy_text); /* and the busy card is put away when it lands */
	assert(strcmp(theme_boot_screen_custom(), "a.jpg") == 0);
	assert(theme_boot_screen() == BOOTLOGO_CUSTOM);
	assert(boot_choice == BOOTLOGO_CUSTOM);

	/* And the Appearance page says which one, on the way back in. */
	lv_obj_send_event(appearance_screen, LV_EVENT_SCREEN_LOADED, NULL);
	assert(find_label(appearance_screen, "a.jpg"));

	/* Custom is only an answer while the copy is there. */
	assert(theme_boot_screen() == BOOTLOGO_CUSTOM);
	bootpic_installed = -1;
	assert(theme_boot_screen() == BOOTLOGO_RETROSPACE);

	assert(icon_year.header.w == 32 && icon_year.header.h == 32 && icon_year.header.stride == 128);
	assert(icon_most_played.header.w == 32 && icon_most_played.header.h == 32 && icon_most_played.header.stride == 128);
	assert(icon_menu_year.header.w == 112 && icon_menu_year.header.h == 112 && icon_menu_year.header.stride == 448);
	assert(icon_menu_most_played.header.w == 112 && icon_menu_most_played.header.h == 112 && icon_menu_most_played.header.stride == 448);

	lv_obj_t *val_label = NULL;
	lv_obj_t *test_slider = NULL;
	lv_obj_t *slider_card = settingsrow_slider(settings_screen, "settings_screen_off", 5, &val_label, &test_slider, NULL);
	lv_obj_set_width(slider_card, cfg.screen_width - cfg.padding * 2);
	lv_obj_update_layout(settings_screen);
	lv_area_t card_area, slider_area;
	lv_obj_get_coords(slider_card, &card_area);
	lv_obj_get_coords(test_slider, &slider_area);
	int32_t knob_overhang = lv_obj_get_height(test_slider) / 2 + lv_obj_get_style_pad_right(test_slider, LV_PART_KNOB) + 2;
	assert(slider_area.x1 - knob_overhang >= card_area.x1 + 1);
	assert(slider_area.x2 + knob_overhang <= card_area.x2 - 1);

	musicsettings_screen = lv_obj_create(NULL);
	music_init(&cfg);
	lv_obj_t *music_tile_grid = lv_obj_get_child(music_screen, 0);
	assert(lv_obj_get_child_count(music_tile_grid) == MUSIC_TILE_COUNT);
	assert(find_label(music_tile_grid, tr("music_years")));
	assert(find_label(music_tile_grid, tr("music_most_played")));
	if (bp_is_tempotec_v1()) {
		lv_obj_t *tokyo_panel = lv_obj_get_child(music_screen, lv_obj_get_child_count(music_screen) - 1);
		assert(find_label(tokyo_panel, tr("music_years")));
		assert(find_label(tokyo_panel, tr("music_most_played")));
	}

	/* Rearrange and disable Years and Most played; verify both tile_grid and tokyo_music_panel update. */
	test_active_tiles[0] = MUSIC_TILE_PLAYLISTS;
	test_active_tiles[1] = MUSIC_TILE_ALBUMS;
	test_active_tiles[2] = MUSIC_TILE_ARTISTS;
	test_active_tiles[3] = MUSIC_TILE_ALL_TRACKS;
	test_active_tiles[4] = MUSIC_TILE_GENRES;
	test_active_tile_count = 5;
	music_refresh_layout();
	assert(lv_obj_get_child_count(music_tile_grid) == 5);
	assert(find_label(lv_obj_get_child(music_tile_grid, 0), tr("playlists")));
	assert(!find_label(music_tile_grid, tr("music_years")));
	assert(!find_label(music_tile_grid, tr("music_most_played")));
	if (bp_is_tempotec_v1()) {
		lv_obj_t *tokyo_panel = lv_obj_get_child(music_screen, lv_obj_get_child_count(music_screen) - 1);
		lv_obj_t *browse_card = lv_obj_get_child(tokyo_panel, 0);
		lv_obj_t *browse_grid = lv_obj_get_child(browse_card, 1);
		assert(find_label(lv_obj_get_child(browse_grid, 0), tr("playlists")));
		assert(!find_label(tokyo_panel, tr("music_years")));
		assert(!find_label(tokyo_panel, tr("music_most_played")));
		assert(!find_label(tokyo_panel, tr("music_album_artist")));
	}

	const grid_entry_t entries[] = {
		{"music_all_tracks", &icon_menu_music, &music_screen, NULL},
		{"albums", &icon_album, &music_screen, NULL},
		{"music_years", &icon_menu_year, &music_screen, NULL},
		{"music_most_played", &icon_menu_most_played, &music_screen, NULL},
		{"playlists", &icon_menu_playlist, &music_screen, NULL},
	};
	lv_obj_t *section_grid = gridpage_build(streaming_screen, &cfg, entries, 5, 2, 3, true);
	gridpage_set_tile_label_hidden(NULL, 0, true);
	gridpage_set_tile_label_hidden(section_grid, -1, true);
	gridpage_set_tile_label_hidden(section_grid, 99, true);

	for (int kind = 0; kind < THEME_KIND_COUNT; kind++) {
		set_icons_only(false);
		theme_set_kind((theme_kind_t)kind);
		tile_metrics_t year_tile = tile_metrics(lv_obj_get_child(section_grid, 2));
		tile_metrics_t most_tile = tile_metrics(lv_obj_get_child(section_grid, 3));
		tile_metrics_t play_tile = tile_metrics(lv_obj_get_child(section_grid, 4));
		if (theme_uses_function_glyphs()) {
			assert(year_tile.source == &icon_year);
			assert(most_tile.source == &icon_most_played);
			assert(play_tile.source == &icon_list_music);
		} else {
			assert(year_tile.source == &icon_menu_year);
			assert(most_tile.source == &icon_menu_most_played);
			assert(play_tile.source == &icon_menu_playlist);
		}
		tile_metrics_t baseline[6];
		for (int i = 0; i < 6; i++) {
			baseline[i] = tile_metrics(lv_obj_get_child(grid, i));
		}
		tile_metrics_t section_before = tile_metrics(lv_obj_get_child(section_grid, 0));
		bool header_hidden = lv_obj_is_hidden(lv_obj_get_child(main_menu_screen, 1));
		bool footer_hidden = lv_obj_is_hidden(lv_obj_get_child(main_menu_screen, 3));

		set_icons_only(true);
		assert(config_init(config)); // a real save/reload, not just the in-memory flag
		assert(main_menu_icons_only());
		check_grid(grid, true);
		check_grid(section_grid, false);
		check_metrics(lv_obj_get_child(section_grid, 0), section_before);
		assert(lv_obj_is_hidden(lv_obj_get_child(main_menu_screen, 1)) == header_hidden);
		assert(lv_obj_is_hidden(lv_obj_get_child(main_menu_screen, 3)) == footer_hidden);
		dump_menu_snapshot(kind);

		theme_set_accent(3);
		theme_set_dynamic_tint(true);
		check_grid(grid, true);
		check_grid(section_grid, false);
		fonts_set_text_size(FONTS_TEXT_LARGE);
		check_grid(grid, true);
		if (bp_is_tempotec_v1() && theme_uses_function_glyphs()) {
			/* Custom faces have different line metrics even at one text size.
			 * Exercise odd/even pixel sizes as the footer takes more space. */
			for (int line_height = 15; line_height <= 32; line_height++) {
				font_ui_14.line_height = line_height;
				for (int i = 0; i < font_callback_count; i++) {
					font_callbacks[i]();
				}
				check_grid(grid, true);
			}
		}
		fonts_set_text_size(FONTS_TEXT_NORMAL);
		check_grid(grid, true);

		set_icons_only(false);
		assert(config_init(config));
		assert(!main_menu_icons_only());
		check_grid(grid, false);
		for (int i = 0; i < 6; i++) {
			check_metrics(lv_obj_get_child(grid, i), baseline[i]);
		}
		/* Refresh callbacks must not undo the preference on a theme change. */
		set_icons_only(true);
		theme_set_kind((theme_kind_t)((kind + 1) % THEME_KIND_COUNT));
		check_grid(grid, true);
		check_grid(section_grid, false);
	}

	/* Hidden labels still translate, and all six destinations remain clickable. */
	char display_name[64];
	lang_display_name("Ukrainian", display_name, sizeof(display_name));
	assert(strcmp(display_name, "Українська") == 0);
	assert(lang_set("Ukrainian"));
	check_grid(grid, true);
	lv_obj_t *music_label = child_of_type(lv_obj_get_child(grid, 0), &lv_label_class);
	assert(strcmp(lv_label_get_text(music_label), tr("music")) == 0);
	assert(strcmp(tr("music"), "Музика") == 0);
	lang_display_name("Russian", display_name, sizeof(display_name));
	assert(strcmp(display_name, "Русский") == 0);
	assert(lang_set("Russian"));
	check_grid(grid, true);
	assert(strcmp(lv_label_get_text(music_label), tr("music")) == 0);
	assert(strcmp(tr("music"), "Музыка") == 0);
	assert(lang_set("Français"));
	check_grid(grid, true);
	assert(strcmp(lv_label_get_text(music_label), tr("music")) == 0);
	assert(lang_set("English"));
	assert(strcmp(lv_label_get_text(music_label), tr("music")) == 0);
	lv_obj_t *destinations[] = {music_screen, streaming_screen, wireless_screen,
		audiobooks_screen, morepage_screen, settings_screen};
	for (int i = 0; i < 6; i++) {
		lv_obj_send_event(lv_obj_get_child(grid, i), LV_EVENT_CLICKED, NULL);
		assert(lv_screen_active() == destinations[i]);
		lv_screen_load(main_menu_screen);
		check_grid(grid, true);
	}

	lv_deinit();
	unlink(config);
	rmdir(dir);
	printf("PASS main-menu visibility and icons (%s, %s)\n", bp_is_tempotec_v1() ? "240x320" : "480x720",
		saved_icons_only ? "saved icons" : saved_visibility ? "saved visibility" : saved_empty ? "saved empty" : "defaults");
	return 0;
}
