/* The four hi-res badge designs on the real LVGL renderer, without the
 * player's audio/network shell. Checks text, image colours, corner alignment,
 * repeated switches and the larger UI font on small V1 sleeves. */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "src/gui/nowplaying/hiresbadgeview.h"
#include "src/gui/shell/icons.h"

lv_font_t font_ui_14;

#define INSET 14

static lv_obj_t *plain(lv_obj_t *parent, int side) {
	lv_obj_t *obj = lv_obj_create(parent);
	lv_obj_remove_style_all(obj);
	lv_obj_set_size(obj, side, side);
	lv_obj_set_style_bg_color(obj, lv_color_hex(0x336699), 0);
	lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
	lv_obj_set_scrollable(obj, false);
	return obj;
}

static uint32_t pixel_at(const lv_draw_buf_t *shot, int x, int y) {
	const uint8_t *p = shot->data + (size_t)y * shot->header.stride + (size_t)x * 4;
	return ((uint32_t)p[2] << 16) | ((uint32_t)p[1] << 8) | p[0];
}

static bool close_to(uint32_t a, uint32_t b, int tolerance) {
	for (int shift = 0; shift <= 16; shift += 8) {
		int d = (int)((a >> shift) & 0xff) - (int)((b >> shift) & 0xff);
		if (d < -tolerance || d > tolerance) return false;
	}
	return true;
}

static void check_logo_pixels(lv_obj_t *art, const hiresbadge_view_t *view) {
	lv_obj_update_layout(art);
	lv_draw_buf_t *shot = lv_snapshot_take(art, LV_COLOR_FORMAT_XRGB8888);
	assert(shot);
	lv_area_t badge;
	lv_obj_get_coords(view->box, &badge);
	// Gold in the upper field and black in the lower panel: the official mark
	// must not inherit a theme tint or the text badge's white plate.
	assert(close_to(pixel_at(shot, badge.x1 + 18, badge.y1 + 2), 0xfad27e, 12));
	assert(close_to(pixel_at(shot, badge.x1 + 2, badge.y1 + 25), 0x000000, 12));
	lv_draw_buf_destroy(shot);
}

static void exercise(int width, const lv_font_t *font, bool right) {
	lv_obj_t *screen = lv_obj_create(NULL);
	lv_obj_set_style_pad_all(screen, 0, 0);
	lv_obj_t *art = plain(screen, width);
	font_ui_14 = *font;
	hiresbadge_view_t view;
	hiresbadge_build(&view, art);
	assert(lv_obj_is_hidden(view.box));
	assert(!lv_obj_is_clickable(view.box));
	assert(!lv_obj_is_clickable(view.label));
	assert(!lv_obj_is_clickable(view.image));

	const hiresbadge_style_t order[] = {HIRESBADGE_LOGO, HIRESBADGE_ONE_LINE, HIRESBADGE_INITIALS,
										HIRESBADGE_TWO_LINE, HIRESBADGE_LOGO};
	for (size_t i = 0; i < sizeof(order) / sizeof(order[0]); i++) {
		hiresbadge_style_t style = order[i];
		hiresbadge_apply(&view, style);
		assert(lv_obj_is_hidden(view.box)); // changing design never enables it
		lv_obj_set_hidden(view.box, false);
		lv_obj_align(view.box, right ? LV_ALIGN_TOP_RIGHT : LV_ALIGN_TOP_LEFT,
					 right ? -INSET : INSET, INSET);
		lv_obj_update_layout(screen);
		lv_area_t box, sleeve;
		lv_obj_get_coords(view.box, &box);
		lv_obj_get_coords(art, &sleeve);
		assert(box.x1 >= sleeve.x1 + INSET && box.x2 <= sleeve.x2 - INSET);
		assert(box.y1 >= sleeve.y1 + INSET && box.y2 <= sleeve.y2 - INSET);
		assert(lv_obj_get_width(view.box) == lv_obj_get_width(style == HIRESBADGE_LOGO ? view.image : view.label));
		assert(lv_obj_get_height(view.box) == lv_obj_get_height(style == HIRESBADGE_LOGO ? view.image : view.label));
		if (style == HIRESBADGE_LOGO) {
			assert(lv_obj_is_hidden(view.label));
			assert(!lv_obj_is_hidden(view.image));
			assert(lv_image_get_src(view.image) == &icon_hires_audio);
			assert(lv_obj_get_width(view.image) == 36 && lv_obj_get_height(view.image) == 36);
			assert(lv_obj_get_style_image_recolor_opa(view.image, 0) == LV_OPA_TRANSP);
			check_logo_pixels(art, &view);
		} else {
			assert(!lv_obj_is_hidden(view.label));
			assert(lv_obj_is_hidden(view.image));
			assert(strcmp(lv_label_get_text(view.label), hiresbadge_text(style)) == 0);
			assert(lv_obj_get_style_text_line_space(view.label, 0) == (style == HIRESBADGE_TWO_LINE ? -4 : 0));
		}
		lv_obj_set_hidden(view.box, true);
	}
	// The player moves one badge between the ordinary sleeve and Studio's.
	lv_obj_t *studio = plain(screen, width);
	lv_obj_set_parent(view.box, studio);
	hiresbadge_apply(&view, HIRESBADGE_INITIALS);
	lv_obj_set_hidden(view.box, false);
	lv_obj_align(view.box, LV_ALIGN_TOP_LEFT, INSET, INSET);
	lv_obj_update_layout(screen);
	assert(lv_obj_get_parent(view.box) == studio);
	assert(!lv_obj_is_hidden(view.label) && lv_obj_is_hidden(view.image));
	lv_obj_delete(screen);
}

int main(void) {
	lv_init();
	lv_display_create(240, 320);
	for (int width = 128; width <= 224; width += 32) {
		exercise(width, &lv_font_montserrat_14, false);
		exercise(width, &lv_font_montserrat_14, true);
		exercise(width, &lv_font_montserrat_20, false);
		exercise(width, &lv_font_montserrat_20, true);
	}
	puts("PASS hi-res badge designs (text, official colours, corners, switching, two font sizes)");
	return 0;
}
