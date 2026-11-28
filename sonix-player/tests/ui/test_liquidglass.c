/* Liquid Glass: the theme wiring (kind, persistence, the translucent shared
 * styles) and the backdrop module (glass.c) -- a CONSTANT wallpaper that never
 * follows the playing track, shown while glass is active and dropped with the
 * theme. Real LVGL and the real theme engine; the player and cover blur are
 * substituted so the module's own logic is what runs. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "src/gui/nowplaying/cover.h"
#include "src/gui/shell/glass.h"
#include "src/gui/shell/glassbg.h"
#include "src/gui/shell/glasswallpaper.h"
#include "src/gui/shell/theme.h"
#include "src/system/core/config.h"

// --- the player's artwork, lent on demand (the backdrop must ignore it) -----
static const lv_image_dsc_t *stub_cover;
static void (*stub_observer)(void);

const lv_image_dsc_t *player_cover_image(void) { return stub_cover; }
void player_register_cover_observer(void (*cb)(void)) { stub_observer = cb; }

// --- the cover blur, stubbed; the constant backdrop must never call it ------
static int blur_calls;
bool cover_blur_fill(const cover_image_t *src, int out_w, int out_h, cover_image_t *out) {
	(void)src; (void)out_w; (void)out_h; (void)out;
	blur_calls++;
	return false;
}
void cover_free(cover_image_t *img) { (void)img; }

// --- gui_post, stubbed (the constant backdrop never posts) ------------------
bool gui_post(void (*cb)(void *), void *user) { (void)cb; (void)user; return true; }

// --- the wallpaper codecs, stubbed: no card picture is decoded here ---------
// The provider's choice, dim and fallback are what this test checks; a decode
// that finds nothing leaves the supplied art, the same as a picture gone.
uint8_t *jpeg_scaled_decode(const uint8_t *data, size_t size, int min_box, int *out_w, int *out_h,
							int *src_w, int *src_h) {
	(void)data; (void)size; (void)min_box; (void)out_w; (void)out_h; (void)src_w; (void)src_h;
	return NULL;
}
uint8_t *jpeg_planes_decode(const uint8_t *data, size_t size, int box, size_t budget_bytes, int *out_w,
							int *out_h, size_t *peak_bytes) {
	(void)data; (void)size; (void)box; (void)budget_bytes; (void)out_w; (void)out_h; (void)peak_bytes;
	return NULL;
}
uint8_t *png_scaled_decode(const uint8_t *data, size_t size, int min_box, int *out_w, int *out_h) {
	(void)data; (void)size; (void)min_box; (void)out_w; (void)out_h;
	return NULL;
}

// LVGL's configured default font; the theme and backdrop draw no text, so a
// built-in face stands in for the firmware's UI fonts.
lv_font_t font_ui_14;

// --- the boot marker the theme writes, stubbed ------------------------------
void bootlogo_set(int choice, bool dark) {
	(void)choice;
	(void)dark;
}
bool bootpic_present(void) { return false; }

// --- helpers ----------------------------------------------------------------
// The backdrop owns a single image on the bottom layer: the constant wallpaper.
static lv_obj_t *glass_wallpaper(void) {
	lv_obj_t *bottom = lv_layer_bottom();
	for (uint32_t i = 0; i < lv_obj_get_child_count(bottom); i++) {
		lv_obj_t *child = lv_obj_get_child(bottom, (int32_t)i);
		if (lv_obj_check_type(child, &lv_image_class)) {
			return child;
		}
	}
	return NULL;
}

static int glass_image_count(void) {
	lv_obj_t *bottom = lv_layer_bottom();
	int n = 0;
	for (uint32_t i = 0; i < lv_obj_get_child_count(bottom); i++) {
		if (lv_obj_check_type(lv_obj_get_child(bottom, (int32_t)i), &lv_image_class)) {
			n++;
		}
	}
	return n;
}

// Reads a shared style's opacity through LVGL's own cascade: a bare probe
// object carrying just that style, so the value is what a widget would draw.
static lv_opa_t style_bg_opa(const lv_style_t *style) {
	lv_obj_t *probe = lv_obj_create(lv_screen_active());
	lv_obj_remove_style_all(probe);
	lv_obj_add_style(probe, style, 0);
	lv_opa_t opa = lv_obj_get_style_bg_opa(probe, LV_PART_MAIN);
	lv_obj_delete(probe);
	return opa;
}

static int32_t style_outline_width(const lv_style_t *style) {
	lv_obj_t *probe = lv_obj_create(lv_screen_active());
	lv_obj_remove_style_all(probe);
	lv_obj_add_style(probe, style, 0);
	int32_t w = lv_obj_get_style_outline_width(probe, LV_PART_MAIN);
	lv_obj_delete(probe);
	return w;
}

// A small sleeve to lend the backdrop. The backdrop must not react to it.
static lv_image_dsc_t make_cover(uint8_t fill) {
	static uint8_t buf[3][4 * 4 * 2];
	int idx = (fill >= 1 && fill <= 3) ? fill - 1 : 0;
	memset(buf[idx], fill, sizeof(buf[idx]));
	lv_image_dsc_t dsc;
	memset(&dsc, 0, sizeof(dsc));
	dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
	dsc.header.cf = LV_COLOR_FORMAT_RGB565;
	dsc.header.w = 4;
	dsc.header.h = 4;
	dsc.header.stride = 4 * 2;
	dsc.data = buf[idx];
	dsc.data_size = sizeof(buf[idx]);
	return dsc;
}

int main(void) {
	char dir[] = "/tmp/sonix-glass-test-XXXXXX";
	assert(mkdtemp(dir));
	char config[256];
	snprintf(config, sizeof(config), "%s/device_config.ini", dir);
	config_init(config); // a fresh path starts from defaults (returns false)

	lv_init();
	font_ui_14 = lv_font_montserrat_14;
	lv_display_create(240, 320);
	theme_init();
	glassbg_init();
	glass_init();

	// --- selecting Liquid Glass --------------------------------------------
	theme_set_kind(THEME_KIND_LIQUID_GLASS);
	assert(theme_is_liquid_glass());
	assert(theme_get_kind() == THEME_KIND_LIQUID_GLASS);
	assert(strcmp(config_get("ui", "theme", ""), "liquid-glass") == 0);
	assert(theme()->dark);
	// the iOS system blue is its native accent
	assert(theme()->accent.red == 10 && theme()->accent.green == 132 && theme()->accent.blue == 255);
	// the page is clear; the surfaces are films, not solid fills
	assert(style_bg_opa(&theme_style_screen) == LV_OPA_TRANSP);
	assert(style_bg_opa(&theme_style_panel) == THEME_GLASS_PANEL_OPA);
	assert(style_bg_opa(&theme_style_card) == THEME_GLASS_CARD_OPA);
	assert(style_bg_opa(&theme_style_card_pressed) == THEME_GLASS_CARD_PRESSED_OPA);
	assert(style_bg_opa(&theme_style_accent_bg) == THEME_GLASS_ACCENT_OPA);

	// Even with no track, the menu sits on a visible wallpaper, not flat black.
	lv_obj_t *wall = glass_wallpaper();
	assert(wall);
	assert(!lv_obj_is_hidden(wall));
	assert(glass_image_count() == 1); // only the constant wallpaper, no sleeve layer
	const lv_image_dsc_t *wall_src = lv_image_get_src(wall);
	assert(wall_src && wall_src->data);
	// A smooth colour field, not a flat fill: many distinct pixels, and both a
	// warm (magenta) and a cool (teal) region so the frost has colour to show.
	int warm = 0, cool = 0;
	uint16_t seen[64];
	int seen_n = 0;
	for (uint32_t y = 0; y < wall_src->header.h; y += 4) {
		const uint8_t *row = wall_src->data + y * wall_src->header.stride;
		for (uint32_t x = 0; x < wall_src->header.w; x += 4) {
			uint16_t p;
			memcpy(&p, row + x * 2, sizeof(p));
			int r = (p >> 11) & 31, g = (p >> 5) & 63, b = p & 31;
			if (r > 20 && r > g && r > b) warm++;
			if (g > 25 && g > r && g >= b) cool++;
			int dup = 0;
			for (int i = 0; i < seen_n; i++) if (seen[i] == p) { dup = 1; break; }
			if (!dup && seen_n < 64) seen[seen_n++] = p;
		}
	}
	assert(seen_n >= 32); // a gradient, not a solid colour
	assert(warm > 0 && cool > 0);

	// --- the background never follows the artwork ---------------------------
	// Lend a sleeve, then another, then take it away: the wallpaper must stay
	// the single, unchanged image and no blur may ever be made of the track.
	lv_image_dsc_t a = make_cover(1);
	stub_cover = &a;
	if (stub_observer) stub_observer();
	theme_set_kind(THEME_KIND_LIQUID_GLASS); // a refresh with a track loaded
	assert(glass_image_count() == 1);
	assert(lv_image_get_src(wall) == wall_src);
	assert(!lv_obj_is_hidden(wall));

	lv_image_dsc_t b = make_cover(2);
	stub_cover = &b;
	if (stub_observer) stub_observer();
	assert(glass_image_count() == 1);
	assert(lv_image_get_src(wall) == wall_src);

	stub_cover = NULL;
	if (stub_observer) stub_observer();
	assert(glass_image_count() == 1);
	assert(lv_image_get_src(wall) == wall_src);
	assert(blur_calls == 0); // the constant backdrop never blurs the sleeve

	// --- Apple-style tinted glass follows the chosen accent -----------------
	theme_set_accent(8); // purple
	theme_set_dynamic_tint(true);
	assert(theme_dynamic_tint());
	lv_color_t frost = theme_glass_frost();
	lv_color_t rim = theme_glass_rim();
	// Tinted frost and rim are no longer neutral white...
	assert(!(frost.red == 255 && frost.green == 255 && frost.blue == 255));
	assert(!(rim.red == 255 && rim.green == 255 && rim.blue == 255));
	// ...and they carry the accent's hue (purple: red & blue above green).
	assert(frost.red > frost.green && frost.blue > frost.green);
	// The glass opacities are untouched by the tint.
	assert(style_bg_opa(&theme_style_card) == THEME_GLASS_CARD_OPA);
	assert(style_bg_opa(&theme_style_card_pressed) == THEME_GLASS_CARD_PRESSED_OPA);
	// Turning the tint off returns the clear white frost.
	theme_set_dynamic_tint(false);
	frost = theme_glass_frost();
	assert(frost.red == 255 && frost.green == 255 && frost.blue == 255);

	// --- the custom background: choice, dim, and the fallback ---------------
	// With nothing chosen the supplied art is the source, at the default dim.
	assert(glassbg_source() == &glass_wallpaper_dsc);
	assert(glassbg_dim_percent() == GLASSBG_DIM_DEFAULT);
	assert(glassbg_dim_opa() == (lv_opa_t)(GLASSBG_DIM_DEFAULT * 255 / 100));
	assert(glassbg_current_name()[0] == '\0');

	// A decoded picture becomes the wallpaper the backdrop draws, and the
	// choice is kept by name so it follows the card.
	uint8_t *px = malloc((size_t)240 * 320 * 2);
	assert(px);
	memset(px, 0x5A, (size_t)240 * 320 * 2);
	glassbg_install(px, 240, 320, "holiday.jpg");
	assert(glassbg_source() != &glass_wallpaper_dsc);
	assert(glassbg_source()->header.w == 240 && glassbg_source()->header.h == 320);
	assert(strcmp(glassbg_current_name(), "holiday.jpg") == 0);
	assert(strcmp(config_get("ui", "glass_wallpaper", ""), "holiday.jpg") == 0);
	assert(lv_image_get_src(wall) == glassbg_source()); // the backdrop follows

	// The dim knob clamps to its range, saves on set, and the backdrop wears
	// it; a preview moves the backdrop without touching the saved value.
	glassbg_set_dim(40);
	assert(glassbg_dim_percent() == 40);
	assert(config_get_int("ui", "glass_wallpaper_dim", 0) == 40);
	assert(lv_obj_get_style_image_recolor_opa(wall, 0) == glassbg_dim_opa());
	glassbg_set_dim(500);
	assert(glassbg_dim_percent() == GLASSBG_DIM_MAX);
	glassbg_preview_dim(10);
	assert(glassbg_dim_percent() == 10);
	assert(config_get_int("ui", "glass_wallpaper_dim", 0) == GLASSBG_DIM_MAX); // preview did not save
	glassbg_set_dim(-5);
	assert(glassbg_dim_percent() == 0);

	// Back to the supplied art forgets the choice and frees the picture.
	glassbg_reset();
	assert(glassbg_source() == &glass_wallpaper_dsc);
	assert(glassbg_current_name()[0] == '\0');
	assert(config_get("ui", "glass_wallpaper", "x")[0] == '\0');
	assert(lv_image_get_src(wall) == &glass_wallpaper_dsc);

	// --- leaving Liquid Glass restores the opaque styles --------------------
	theme_set_kind(THEME_KIND_DARK);
	assert(!theme_is_liquid_glass());
	assert(lv_obj_is_hidden(glass_wallpaper())); // wallpaper dropped with the theme
	assert(style_bg_opa(&theme_style_screen) == LV_OPA_COVER);
	assert(style_bg_opa(&theme_style_panel) == LV_OPA_COVER);
	assert(style_bg_opa(&theme_style_card) == LV_OPA_COVER);
	assert(style_bg_opa(&theme_style_accent_bg) == LV_OPA_COVER);

	// --- the choice survives a restart --------------------------------------
	theme_set_kind(THEME_KIND_LIQUID_GLASS);
	assert(config_save());
	assert(config_init(config));
	theme_init();
	assert(theme_get_kind() == THEME_KIND_LIQUID_GLASS);
	assert(theme_is_liquid_glass());
	assert(style_bg_opa(&theme_style_screen) == LV_OPA_TRANSP);

	// --- Hide borders: every rim to zero width, the frost stays -------------
	theme_set_glass_borders_hidden(true);
	assert(theme_glass_borders_hidden());
	assert(style_outline_width(&theme_style_card) == 0);
	assert(style_outline_width(&theme_style_panel) == 0);
	assert(style_outline_width(&theme_style_card_pressed) == 0);
	assert(style_outline_width(&theme_style_switch) == 0);
	assert(style_bg_opa(&theme_style_card) == THEME_GLASS_CARD_OPA);
	lv_obj_t *pane = lv_obj_create(lv_screen_active());
	theme_glass_pane(pane);
	assert(lv_obj_get_style_border_width(pane, LV_PART_MAIN) == 0);
	lv_obj_t *strip = lv_obj_create(lv_screen_active());
	theme_glass_index_bar(strip);
	assert(lv_obj_get_style_border_width(strip, LV_PART_MAIN) == 0);
	lv_obj_t *dlg = lv_obj_create(lv_screen_active());
	theme_glass_dialog(dlg);
	assert(lv_obj_get_style_border_width(dlg, LV_PART_MAIN) == 0);
	// Back off: the rims return, everywhere the helpers draw them.
	theme_set_glass_borders_hidden(false);
	assert(!theme_glass_borders_hidden());
	assert(style_outline_width(&theme_style_card) == 1);
	assert(style_outline_width(&theme_style_switch) == 1);
	theme_glass_pane(pane);
	assert(lv_obj_get_style_border_width(pane, LV_PART_MAIN) == 1);
	theme_glass_index_bar(strip);
	assert(lv_obj_get_style_border_width(strip, LV_PART_MAIN) == 1);
	theme_glass_dialog(dlg);
	assert(lv_obj_get_style_border_width(dlg, LV_PART_MAIN) == 1);
	lv_obj_delete(pane);
	lv_obj_delete(strip);
	lv_obj_delete(dlg);
	// The choice survives a restart like the other glass settings.
	theme_set_glass_borders_hidden(true);
	assert(config_save());
	assert(config_init(config));
	theme_init();
	assert(theme_glass_borders_hidden());
	theme_set_glass_borders_hidden(false);
	assert(!config_get_bool("ui", "glass_hide_borders", true));
	// The flat themes ignore it: their cards never carried a glass rim.
	theme_set_glass_borders_hidden(true);
	theme_set_kind(THEME_KIND_DARK);
	assert(style_outline_width(&theme_style_card) == 0);
	theme_set_kind(THEME_KIND_LIQUID_GLASS);
	theme_set_glass_borders_hidden(false);

	printf("PASS liquid-glass theme and constant backdrop\n");
	return 0;
}
