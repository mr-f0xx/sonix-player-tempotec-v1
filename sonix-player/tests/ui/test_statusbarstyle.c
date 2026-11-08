/* Exercise the actual LVGL background rendering used by the player bar. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include "src/gui/shell/statusbarstyle.h"
#include "src/gui/nowplaying/statusbarlayout.h"

lv_font_t font_ui_14;

static uint16_t pixels[240 * 96];
static lv_image_dsc_t blur = {
	.header = {.magic = LV_IMAGE_HEADER_MAGIC, .cf = LV_COLOR_FORMAT_RGB565,
		.w = 240, .h = 96, .stride = 480},
	.data_size = sizeof(pixels), .data = (const uint8_t *)pixels,
};

static uint32_t sample(lv_obj_t *bar) {
	lv_obj_update_layout(bar);
	lv_draw_buf_t *shot = lv_snapshot_take(bar, LV_COLOR_FORMAT_XRGB8888);
	assert(shot);
	const uint8_t *p = shot->data + 10 * shot->header.stride + 20 * 4;
	uint32_t color = (uint32_t)p[2] << 16 | (uint32_t)p[1] << 8 | p[0];
	lv_draw_buf_destroy(shot);
	return color;
}

int main(void) {
	lv_init();
	font_ui_14 = lv_font_montserrat_14;
	lv_display_create(240, 320);
	lv_obj_t *screen = lv_obj_create(NULL);
	lv_obj_t *bar = lv_obj_create(screen);
	lv_obj_remove_style_all(bar);
	lv_obj_set_size(bar, 240, 24);
	lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
	lv_style_t theme;
	lv_style_init(&theme);
	lv_style_set_bg_color(&theme, lv_color_hex(0x202020));
	lv_obj_add_style(bar, &theme, 0);
	for (size_t i = 0; i < sizeof(pixels) / sizeof(pixels[0]); i++) pixels[i] = 0xf800;
	// Standard/Waveform/Album Art Max use the full backdrop except in OLED;
	// Studio and Immersive keep their blur even in OLED. Poster and Vinyl use
	// their solid grounds. A not-yet-loaded or mismatched image is not used.
	assert(statusbarlayout_uses_blur(false, false, false, false, false, true, false));
	assert(!statusbarlayout_uses_blur(false, false, false, false, true, true, false));
	assert(statusbarlayout_uses_blur(false, false, true, false, true, true, false));
	assert(statusbarlayout_uses_blur(false, false, false, true, true, false, true));
	assert(!statusbarlayout_uses_blur(false, false, false, true, false, true, false));
	assert(!statusbarlayout_uses_blur(true, false, false, false, false, true, true));
	assert(!statusbarlayout_uses_blur(false, true, false, false, false, true, true));
	assert(!statusbarlayout_uses_blur(false, false, false, false, false, false, false));
	uint32_t plain = sample(bar);
	statusbarstyle_frost(bar, &blur);
	uint32_t frosted = sample(bar);
	assert(((frosted >> 16) & 255) > ((plain >> 16) & 255) + 100);
	statusbarstyle_frost(bar, NULL);
	assert(sample(bar) == plain);
	// Polling status updates use default-state local colours. The album state
	// must take precedence, then reveal the latest radio/battery colour again.
	lv_obj_t *icon = lv_image_create(bar);
	lv_obj_set_style_image_recolor(icon, lv_color_hex(0x00ff00), 0);
	lv_obj_set_style_image_recolor_opa(icon, LV_OPA_COVER, 0);
	lv_color_t ink = statusbarstyle_ink(lv_color_hex(0xff0000), lv_color_white());
	statusbarstyle_icon(icon, true, ink);
	assert(lv_color_to_u32(lv_obj_get_style_image_recolor(icon, 0)) == lv_color_to_u32(ink));
	lv_obj_set_style_image_recolor(icon, lv_color_hex(0x0000ff), 0);
	assert(lv_color_to_u32(lv_obj_get_style_image_recolor(icon, 0)) == lv_color_to_u32(ink));
	statusbarstyle_icon(icon, false, ink);
	assert(lv_color_to_u32(lv_obj_get_style_image_recolor(icon, 0)) == lv_color_to_u32(lv_color_hex(0x0000ff)));
	// The level fill is an object, not an image: it uses the same album ink,
	// survives periodic battery updates, and restores the latest status color.
	lv_obj_t *fill = lv_obj_create(bar);
	lv_obj_set_hidden(fill, true); // do not cover the bar sample below
	lv_obj_set_style_bg_color(fill, lv_color_hex(0x00ff00), 0);
	statusbarstyle_fill(fill, true, ink);
	assert(lv_color_to_u32(lv_obj_get_style_bg_color(fill, 0)) == lv_color_to_u32(ink));
	lv_obj_set_style_bg_color(fill, lv_color_hex(0xff0000), 0);
	assert(lv_color_to_u32(lv_obj_get_style_bg_color(fill, 0)) == lv_color_to_u32(ink));
	statusbarstyle_fill(fill, false, ink);
	assert(lv_color_to_u32(lv_obj_get_style_bg_color(fill, 0)) == lv_color_to_u32(lv_color_hex(0xff0000)));
	// Icon recoloring does not change the bar's theme background, and can be
	// enabled while the layout background uses the blurred artwork.
	statusbarstyle_icon(icon, true, ink);
	assert(sample(bar) == plain);
	statusbarstyle_frost(bar, &blur);
	assert(sample(bar) == frosted);
	statusbarstyle_frost(bar, NULL);
	assert(sample(bar) == plain);
	statusbarstyle_icon(icon, false, ink);
	lv_obj_delete(screen);
	lv_style_reset(&theme);
	puts("PASS status bar icon tint, frosted image, combined options and restore");
	return 0;
}
