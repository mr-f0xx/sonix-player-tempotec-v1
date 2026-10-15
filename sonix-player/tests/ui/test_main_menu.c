/* Real LVGL, menus, appearance controls, themes, translations and persistence.
 * Only fonts and unrelated playback/navigation/board services are substituted. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "src/gui/audio/dacpage.h"
#include "src/gui/board_profile.h"
#include "src/gui/ebook/ebookpage.h"
#include "src/gui/flappybird/flappybird.h"
#include "src/gui/fonts/fonts.h"
#include "src/gui/gearboy/gearboypage.h"
#include "src/gui/library/audiobooks.h"
#include "src/gui/library/filespage.h"
#include "src/gui/library/music.h"
#include "src/gui/nowplaying/coverflow.h"
#include "src/gui/nowplaying/player.h"
#include "src/gui/settings/appearance.h"
#include "src/gui/settings/settings.h"
#include "src/gui/shell/gridpage.h"
#include "src/gui/shell/icons.h"
#include "src/gui/shell/main_menu.h"
#include "src/gui/shell/main_menu_icons.h"
#include "src/gui/shell/morepage.h"
#include "src/gui/shell/switcher.h"
#include "src/gui/shell/theme.h"
#include "src/gui/shell/toast.h"
#include "src/gui/shell/topbar.h"
#include "src/gui/streaming/streaming.h"
#include "src/gui/wireless/wireless.h"
#include "src/system/core/config.h"
#include "src/system/core/lang.h"
#include "src/system/device/bootlogo.h"
#include "src/system/playback/device_state.h"

lv_obj_t *music_screen, *streaming_screen, *wireless_screen;
lv_obj_t *audiobooks_screen, *morepage_screen, *settings_screen, *player_screen;
lv_obj_t *ebookpage_screen, *dacpage_screen, *gearboypage_screen, *filespage_screen, *flappybird_screen;
static int books_opened, files_opened, flappy_opened;
static const char *last_error;

lv_font_t font_ui_14, font_ui_16, font_ui_18, font_ui_20, font_ui_20_bold;
lv_font_t font_ui_22, font_ui_22_bold, font_ui_24, font_ui_24_bold;
lv_font_t font_ui_26, font_ui_28, font_ui_32;
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

void bootlogo_set(int choice, bool dark) { (void)choice; (void)dark; }
void device_state_get(device_state_t *out) { memset(out, 0, sizeof(*out)); }
bool gui_card_available(void) { return true; }
void gui_notify_no_card(void) { assert(!"unexpected missing card"); }
void gui_notify_popup(const char *text) { (void)text; assert(!"unexpected popup"); }
void toast_error(const char *text) { last_error = text; }
void ebookpage_open(void) { books_opened++; switch_screen(ebookpage_screen); }
void filespage_open(void) { files_opened++; switch_screen(filespage_screen); }
void flappybird_open(void) { flappy_opened++; switch_screen(flappybird_screen); }
void topbar_set_battery_percent(bool shown) { (void)shown; }
void topbar_set_clock_position(int pos) { (void)pos; }
void switch_screen(lv_obj_t *screen) { lv_screen_load(screen); }
void switch_screen_cb(lv_event_t *e) { switch_screen(lv_event_get_user_data(e)); }
void switcher_attach_back_gesture(lv_obj_t *obj) { (void)obj; }
bool switcher_back_drag_active(void) { return false; }
void player_sheet_attach_drag(lv_obj_t *obj, bool opening) { (void)obj; (void)opening; }
bool player_sheet_drag_active(void) { return false; }
void coverflow_attach_edge(lv_obj_t *obj, gui_config_t *cfg) { (void)obj; (void)cfg; }
bool coverflow_drag_active(void) { return false; }

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

/* Optional real-renderer diagnostics, kept out of Git. No display driver or
 * image library is needed; PPM can be opened by ordinary image tools. */
static void dump_menu_snapshot(int kind) {
	const char *prefix = getenv("SONIX_MENU_SNAPSHOTS");
	if (!prefix || !prefix[0]) {
		return;
	}
	lv_draw_buf_t *shot = lv_snapshot_take(main_menu_screen, LV_COLOR_FORMAT_RGB888);
	assert(shot);
	char path[512];
	snprintf(path, sizeof(path), "%s-%d-theme%d.ppm", prefix, bp_screen_w(), kind);
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

static lv_obj_t *menu_toggle;
static void set_icons_only(bool enabled) {
	lv_obj_set_state(menu_toggle, LV_STATE_CHECKED, enabled);
	lv_obj_send_event(menu_toggle, LV_EVENT_VALUE_CHANGED, NULL);
	assert(main_menu_icons_only() == enabled);
}

static lv_obj_t *home_grid(void) { return lv_obj_get_child(main_menu_screen, 0); }

static main_menu_button_t home_button_at(int slot) {
	int index = 0;
	for (int i = 0; i < MAIN_MENU_BUTTON_COUNT; i++) {
		if (main_menu_button_enabled((main_menu_button_t)i) && index++ == slot) {
			return (main_menu_button_t)i;
		}
	}
	assert(!"missing home button");
	return MAIN_MENU_SETTINGS;
}

static lv_obj_t *home_button_toggle(lv_obj_t *screen, main_menu_button_t button) {
	lv_obj_t *label = find_label(screen, tr(main_menu_button_label(button)));
	assert(label);
	return child_of_type(lv_obj_get_parent(label), &lv_switch_class);
}

static void toggle_home_button(lv_obj_t *screen, main_menu_button_t button, bool enabled) {
	lv_obj_t *toggle = home_button_toggle(screen, button);
	lv_obj_set_state(toggle, LV_STATE_CHECKED, enabled);
	lv_obj_send_event(toggle, LV_EVENT_VALUE_CHANGED, NULL);
	assert(lv_obj_has_state(toggle, LV_STATE_CHECKED) == enabled);
	assert(main_menu_button_enabled(button) == enabled);
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
	for (int button = 0; button < MAIN_MENU_BUTTON_COUNT; button++) {
		for (int size = 1; size <= 173; size++) {
			const lv_image_dsc_t *glyph = main_menu_icon_native(button, true, size);
			const lv_image_dsc_t *color = main_menu_icon_native(button, false, size);
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
	assert(!main_menu_icon_native(-1, true, 47));
	assert(!main_menu_icon_native(MAIN_MENU_BUTTON_COUNT, false, 74));
	assert(!main_menu_icon_native(0, true, -1));
	assert(!main_menu_icon_native(0, true, 0));
	assert(!main_menu_icon_native(0, false, 174));
}

static void check_grid(lv_obj_t *grid, bool icons_only) {
	lv_obj_update_layout(grid);
	if (lv_obj_get_parent(grid) == main_menu_screen) {
		int count = 0;
		for (int button = 0; button < MAIN_MENU_BUTTON_COUNT; button++) {
			count += main_menu_button_enabled((main_menu_button_t)button);
		}
		assert(count >= 1 && count <= MAIN_MENU_MAX_TILES);
		assert(lv_obj_get_child_count(grid) == (uint32_t)count);
		assert(home_button_at(count - 1) == MAIN_MENU_SETTINGS);
	}
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
		check_native_icon(icon, home_button_at((int)i), expected_size);
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

int main(int argc, char **argv) {
	assert(argc == 2 || argc == 3);
	bool saved_icons_only = argc == 3;
	char dir[] = "/tmp/sonix-menu-test-XXXXXX";
	assert(mkdtemp(dir));
	char config[256];
	snprintf(config, sizeof(config), "%s/device_config.ini", dir);
	config_init(config);
	assert(!main_menu_icons_only()); // no key: preserve the existing menu
	assert(main_menu_button_enabled(MAIN_MENU_AUDIOBOOKS));
	assert(!main_menu_button_enabled(MAIN_MENU_BOOKS));
	assert(main_menu_button_enabled(MAIN_MENU_SETTINGS));
	if (saved_icons_only) {
		// A saved six-tile menu replaces Audiobooks with Books at boot.
		config_set_bool("ui", "main_menu_icons_only", true);
		assert(main_menu_set_button_enabled(MAIN_MENU_AUDIOBOOKS, false));
		assert(main_menu_set_button_enabled(MAIN_MENU_BOOKS, true));
		assert(config_save());
		assert(config_init(config));
	}
	config_set("ui", "language", "English");
	lang_init(argv[1]);
	assert(strcmp(tr("appearance_main_menu_icons_only"), "Main menu icons only") == 0);
	assert(strcmp(tr("appearance_home_buttons"), "Home buttons") == 0);
	init_fonts();
	lv_init();
	check_native_assets();
	gui_config_t cfg = {bp_screen_w(), bp_screen_h(), bp_padding(), bp_status_bar_h(), "/tmp"};
	lv_display_create(cfg.screen_width, cfg.screen_height);
	theme_init();
	music_screen = lv_obj_create(NULL);
	streaming_screen = lv_obj_create(NULL);
	wireless_screen = lv_obj_create(NULL);
	audiobooks_screen = lv_obj_create(NULL);
	morepage_screen = lv_obj_create(NULL);
	settings_screen = lv_obj_create(NULL);
	ebookpage_screen = lv_obj_create(NULL);
	dacpage_screen = lv_obj_create(NULL);
	gearboypage_screen = lv_obj_create(NULL);
	filespage_screen = lv_obj_create(NULL);
	flappybird_screen = lv_obj_create(NULL);
	player_screen = lv_obj_create(NULL);
	main_menu_screen = lv_obj_create(NULL);
	main_menu_init(&cfg);
	lv_screen_load(main_menu_screen);
	lv_obj_t *grid = lv_obj_get_child(main_menu_screen, 0);
	check_grid(grid, saved_icons_only);

	appearance_screen = lv_obj_create(NULL);
	appearance_init(&cfg);
	lv_obj_t *toggle_label = find_label(appearance_screen, tr("appearance_main_menu_icons_only"));
	assert(toggle_label);
	menu_toggle = child_of_type(lv_obj_get_parent(toggle_label), &lv_switch_class);
	assert(lv_obj_has_state(menu_toggle, LV_STATE_CHECKED) == saved_icons_only);

	lv_obj_t *home_label = find_label(appearance_screen, tr("appearance_home_buttons"));
	assert(home_label);
	lv_obj_t *home_row = lv_obj_get_parent(home_label);
	lv_obj_send_event(home_row, LV_EVENT_CLICKED, NULL);
	lv_obj_t *home_options = lv_screen_active();
	assert(home_options != appearance_screen && home_options != main_menu_screen);
	assert(find_label(home_options, tr("appearance_home_buttons_note")));
	for (int i = 0; i < MAIN_MENU_BUTTON_COUNT; i++) {
		if (!main_menu_button_available((main_menu_button_t)i)) {
			continue;
		}
		lv_obj_t *toggle = home_button_toggle(home_options, (main_menu_button_t)i);
		assert(lv_obj_has_state(toggle, LV_STATE_CHECKED) == main_menu_button_enabled((main_menu_button_t)i));
	}
	lv_obj_t *settings_toggle = home_button_toggle(home_options, MAIN_MENU_SETTINGS);
	assert(lv_obj_has_state(settings_toggle, LV_STATE_CHECKED));
	assert(lv_obj_has_state(settings_toggle, LV_STATE_DISABLED));
	assert(!main_menu_set_button_enabled(MAIN_MENU_SETTINGS, false));
	lv_screen_load(main_menu_screen);

	const grid_entry_t entries[] = {
		{"music_all_tracks", &icon_menu_music, &music_screen, NULL},
		{"albums", &icon_album, &music_screen, NULL},
	};
	lv_obj_t *section_grid = gridpage_build(music_screen, &cfg, entries, 2, 2, 3, true);
	gridpage_set_tile_label_hidden(NULL, 0, true);
	gridpage_set_tile_label_hidden(section_grid, -1, true);
	gridpage_set_tile_label_hidden(section_grid, 99, true);

	for (int kind = 0; kind < THEME_KIND_COUNT; kind++) {
		set_icons_only(false);
		theme_set_kind((theme_kind_t)kind);
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
	assert(lang_set("Français"));
	check_grid(grid, true);
	lv_obj_t *music_label = child_of_type(lv_obj_get_child(grid, 0), &lv_label_class);
	assert(strcmp(lv_label_get_text(music_label), tr("music")) == 0);
	assert(lang_set("English"));
	assert(strcmp(lv_label_get_text(music_label), tr("music")) == 0);
	lv_obj_t *destinations[MAIN_MENU_BUTTON_COUNT] = {
		[MAIN_MENU_MUSIC] = music_screen, [MAIN_MENU_STREAMING] = streaming_screen,
		[MAIN_MENU_WIRELESS] = wireless_screen, [MAIN_MENU_AUDIOBOOKS] = audiobooks_screen,
		[MAIN_MENU_BOOKS] = ebookpage_screen, [MAIN_MENU_DAC] = dacpage_screen,
		[MAIN_MENU_GEARBOY] = gearboypage_screen, [MAIN_MENU_FILE_EXPLORER] = filespage_screen,
		[MAIN_MENU_FLAPPY_BIRD] = flappybird_screen, [MAIN_MENU_MORE] = morepage_screen,
		[MAIN_MENU_SETTINGS] = settings_screen,
	};
	for (int i = 0; i < (int)lv_obj_get_child_count(grid); i++) {
		main_menu_button_t button = home_button_at(i);
		lv_obj_send_event(lv_obj_get_child(grid, i), LV_EVENT_CLICKED, NULL);
		assert(lv_screen_active() == destinations[button]);
		lv_screen_load(main_menu_screen);
		check_grid(grid, true);
	}

	// Work through the Appearance switches as a user would. A full menu
	// refuses another tile until one has been switched off, then the new
	// destination replaces the old one without changing the other pages.
	lv_screen_load(home_options);
	if (saved_icons_only) {
		toggle_home_button(home_options, MAIN_MENU_BOOKS, false);
		toggle_home_button(home_options, MAIN_MENU_AUDIOBOOKS, true);
	}
	assert(lv_obj_get_child_count(home_grid()) == MAIN_MENU_MAX_TILES);
	lv_obj_t *books_switch = home_button_toggle(home_options, MAIN_MENU_BOOKS);
	lv_obj_add_state(books_switch, LV_STATE_CHECKED);
	lv_obj_send_event(books_switch, LV_EVENT_VALUE_CHANGED, NULL);
	assert(strcmp(last_error, "appearance_home_buttons_full") == 0);
	assert(!lv_obj_has_state(books_switch, LV_STATE_CHECKED));
	assert(!main_menu_button_enabled(MAIN_MENU_BOOKS));
	assert(lv_obj_get_child_count(home_grid()) == MAIN_MENU_MAX_TILES);

	toggle_home_button(home_options, MAIN_MENU_AUDIOBOOKS, false);
	grid = home_grid();
	assert(lv_obj_get_child_count(grid) == 5);
	assert(home_button_at(4) == MAIN_MENU_SETTINGS);
	assert(lv_obj_get_width(lv_obj_get_child(grid, 4)) == (int)cfg.screen_width - 2 * (int)cfg.padding);
	check_grid(grid, true);
	check_grid(section_grid, false);
	toggle_home_button(home_options, MAIN_MENU_BOOKS, true);
	grid = home_grid();
	assert(lv_obj_get_child_count(grid) == 6);
	assert(home_button_at(3) == MAIN_MENU_BOOKS);
	assert(strcmp(lv_label_get_text(child_of_type(lv_obj_get_child(grid, 3), &lv_label_class)), tr("books")) == 0);
	check_grid(grid, true);
	assert(config_init(config)); // the choices survive an actual reload
	assert(!main_menu_button_enabled(MAIN_MENU_AUDIOBOOKS));
	assert(main_menu_button_enabled(MAIN_MENU_BOOKS));
	assert(main_menu_button_enabled(MAIN_MENU_SETTINGS));
	assert(!main_menu_set_button_enabled(MAIN_MENU_SETTINGS, false));

	// Books uses the shelf-opening action, not a stale target screen: opening
	// it twice must load it twice. All optional More destinations work too.
	lv_screen_load(main_menu_screen);
	for (int n = 0; n < 2; n++) {
		lv_obj_send_event(lv_obj_get_child(grid, 3), LV_EVENT_CLICKED, NULL);
		assert(lv_screen_active() == ebookpage_screen);
		lv_screen_load(main_menu_screen);
	}
	assert(books_opened >= 2);
	lv_screen_load(home_options);
	toggle_home_button(home_options, MAIN_MENU_WIRELESS, false);
	toggle_home_button(home_options, MAIN_MENU_FILE_EXPLORER, true);
	grid = home_grid();
	check_grid(grid, true);
	lv_obj_send_event(lv_obj_get_child(grid, 3), LV_EVENT_CLICKED, NULL);
	assert(files_opened == 1 && lv_screen_active() == filespage_screen);
	lv_screen_load(home_options);
	toggle_home_button(home_options, MAIN_MENU_STREAMING, false);
	toggle_home_button(home_options, MAIN_MENU_DAC, true);
	toggle_home_button(home_options, MAIN_MENU_MORE, false);
	toggle_home_button(home_options, MAIN_MENU_FLAPPY_BIRD, true);
	grid = home_grid();
	check_grid(grid, true);
	int bird_slot = -1;
	for (int i = 0; i < (int)lv_obj_get_child_count(grid); i++) {
		if (home_button_at(i) == MAIN_MENU_FLAPPY_BIRD) {
			bird_slot = i;
		}
	}
	assert(bird_slot >= 0);
	lv_obj_send_event(lv_obj_get_child(grid, bird_slot), LV_EVENT_CLICKED, NULL);
	assert(flappy_opened == 1 && lv_screen_active() == flappybird_screen);
	lv_screen_load(home_options);
#if GB_CORE
	assert(main_menu_button_available(MAIN_MENU_GEARBOY));
	toggle_home_button(home_options, MAIN_MENU_FLAPPY_BIRD, false);
	toggle_home_button(home_options, MAIN_MENU_GEARBOY, true);
	grid = home_grid();
	int gearboy_slot = -1;
	for (int i = 0; i < (int)lv_obj_get_child_count(grid); i++) {
		if (home_button_at(i) == MAIN_MENU_GEARBOY) {
			gearboy_slot = i;
		}
	}
	assert(gearboy_slot >= 0);
	check_grid(grid, true);
	lv_obj_send_event(lv_obj_get_child(grid, gearboy_slot), LV_EVENT_CLICKED, NULL);
	assert(lv_screen_active() == gearboypage_screen);
	lv_screen_load(home_options);
	toggle_home_button(home_options, MAIN_MENU_GEARBOY, false);
	toggle_home_button(home_options, MAIN_MENU_FLAPPY_BIRD, true);
#else
	assert(!main_menu_button_available(MAIN_MENU_GEARBOY));
	assert(!find_label(home_options, tr("gearboy")));
#endif

	// Repeated rebuilds must unregister the old grid's theme reference.
	// Switch themes after them; if the registry fills, the new tile art fails
	// the native-image/color checks or a stale grid gets dereferenced.
	for (int n = 0; n < 20; n++) {
		toggle_home_button(home_options, MAIN_MENU_BOOKS, false);
		toggle_home_button(home_options, MAIN_MENU_BOOKS, true);
		theme_set_kind((theme_kind_t)(n % THEME_KIND_COUNT));
		check_grid(home_grid(), true);
		check_grid(section_grid, false);
	}
	assert(lang_set("Français"));
	grid = home_grid();
	int books_slot = -1;
	for (int i = 0; i < (int)lv_obj_get_child_count(grid); i++) {
		if (home_button_at(i) == MAIN_MENU_BOOKS) {
			books_slot = i;
		}
	}
	assert(books_slot >= 0);
	assert(strcmp(lv_label_get_text(child_of_type(lv_obj_get_child(grid, books_slot), &lv_label_class)), tr("books")) == 0);
	assert(find_label(home_options, tr("appearance_home_buttons_note")));
	assert(lang_set("English"));

	// Bad or stale config cannot hide Settings or overfill the grid. Even a
	// one-tile menu remains usable, and its last tile fills its whole row.
	config_set_int("ui", "home_buttons", 0);
	assert(main_menu_button_enabled(MAIN_MENU_SETTINGS));
	assert(!main_menu_button_enabled(MAIN_MENU_MUSIC));
	assert(main_menu_set_button_enabled(MAIN_MENU_MUSIC, true));
	assert(main_menu_set_button_enabled(MAIN_MENU_MUSIC, false));
	grid = home_grid();
	assert(lv_obj_get_child_count(grid) == 1);
	assert(lv_obj_get_width(lv_obj_get_child(grid, 0)) == (int)cfg.screen_width - 2 * (int)cfg.padding);
	check_grid(grid, true);
	lv_obj_send_event(lv_obj_get_child(grid, 0), LV_EVENT_CLICKED, NULL);
	assert(lv_screen_active() == settings_screen);
	lv_screen_load(home_options);
	assert(main_menu_set_button_enabled(MAIN_MENU_BOOKS, true));
	assert(main_menu_set_button_enabled(MAIN_MENU_MORE, true));
	grid = home_grid();
	assert(lv_obj_get_child_count(grid) == 3);
	assert(lv_obj_get_width(lv_obj_get_child(grid, 2)) == (int)cfg.screen_width - 2 * (int)cfg.padding);
	check_grid(grid, true);
	config_set_int("ui", "home_buttons", -1);
	assert(!main_menu_button_available((main_menu_button_t)-1));
	assert(!main_menu_set_button_enabled(MAIN_MENU_BUTTON_COUNT, true));
	assert(main_menu_button_enabled(MAIN_MENU_SETTINGS));
	assert(main_menu_set_button_enabled(MAIN_MENU_BOOKS, false));
	check_grid(home_grid(), true);

	lv_deinit();
	unlink(config);
	rmdir(dir);
	printf("PASS main-menu appearance (%s, %s)\n", bp_is_tempotec_v1() ? "240x320" : "480x720",
		saved_icons_only ? "saved settings" : "default settings");
	return 0;
}
