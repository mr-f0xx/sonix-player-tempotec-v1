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
	assert(close_to(pixel_at(shot, badge.x1 + 17, badge.y1 + 2), 0xf8d17c, 12));
	assert(close_to(pixel_at(shot, badge.x1 + 2, badge.y1 + 24), 0x000000, 12));
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
		hiresbadge_place_on_art(&view, art, !right, INSET);
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
			assert(lv_obj_get_width(view.image) == 34 && lv_obj_get_height(view.image) == 34);
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
	hiresbadge_apply(&view, HIRESBADGE_INITIALS);
	lv_obj_set_hidden(view.box, false);
	hiresbadge_place_on_art(&view, studio, true, INSET);
	lv_obj_update_layout(screen);
	assert(lv_obj_get_parent(view.box) == studio);
	assert(!lv_obj_is_hidden(view.label) && lv_obj_is_hidden(view.image));
	lv_obj_delete(screen);
}

// All fixture panels are the same blue. Measure the rendered ink rather
// than counting empty pixels in the chevron's canvas or the unscaled badge.
static lv_area_t header_ink(const lv_draw_buf_t *shot, int x1, int x2, int y1, int y2) {
	lv_area_t ink = {x2, y2, -1, -1};
	for (int y = y1; y <= y2; y++) {
		for (int x = x1; x <= x2; x++) {
			if (close_to(pixel_at(shot, x, y), 0x336699, 8)) continue;
			ink.x1 = LV_MIN(ink.x1, x);
			ink.y1 = LV_MIN(ink.y1, y);
			ink.x2 = LV_MAX(ink.x2, x);
			ink.y2 = LV_MAX(ink.y2, y);
		}
	}
	assert(ink.x2 >= ink.x1 && ink.y2 >= ink.y1);
	return ink;
}

static void exercise_header(const lv_font_t *font, int status_h) {
	lv_obj_t *screen = lv_obj_create(NULL);
	lv_obj_set_style_pad_all(screen, 0, 0);
	lv_obj_t *sheet = plain(screen, 240);
	lv_obj_set_height(sheet, 320 - status_h);
	lv_obj_align(sheet, LV_ALIGN_TOP_LEFT, 0, status_h);
	lv_obj_t *art = plain(sheet, 200);
	lv_obj_align(art, LV_ALIGN_TOP_MID, 0, 18);
	lv_obj_set_style_radius(art, LV_RADIUS_CIRCLE, 0);
	lv_obj_set_style_clip_corner(art, true, 0);
	lv_obj_t *back = plain(screen, 36);
	lv_obj_set_style_bg_opa(back, LV_OPA_TRANSP, 0);
	lv_obj_align(back, LV_ALIGN_TOP_LEFT, 6, status_h + 10);
	lv_obj_t *chevron = lv_image_create(back);
	lv_image_set_src(chevron, &icon_chevron_left);
	lv_obj_center(chevron);
	lv_obj_update_layout(screen);
	lv_draw_buf_t *shot = lv_snapshot_take(screen, LV_COLOR_FORMAT_XRGB8888);
	assert(shot);
	lv_area_t back_ink = header_ink(shot, 0, 60, status_h, status_h + 50);
	lv_draw_buf_destroy(shot);
	int height = lv_area_get_height(&back_ink);
	const int target_height = 27;
	int centre_y = back_ink.y1 + height / 2;
	int inset = back_ink.x1;
	assert(height < lv_obj_get_height(back) && inset > 6);

	font_ui_14 = *font;
	hiresbadge_view_t view;
	hiresbadge_build(&view, art);
	hiresbadge_place_in_header(&view, sheet, false, inset, centre_y - status_h, height);
	assert(lv_obj_is_hidden(view.box));
	lv_obj_set_hidden(view.box, false);
	// Keep the measured size and padding even when the same badge changes
	// sides and the back button uses the other corner.
	for (int style = 0; style < HIRESBADGE_STYLE_COUNT; style++) {
		hiresbadge_apply(&view, (hiresbadge_style_t)style);
		for (int side = 0; side < 2; side++) {
			bool left = side == 0;
			lv_obj_align(back, left ? LV_ALIGN_TOP_RIGHT : LV_ALIGN_TOP_LEFT, left ? -6 : 6, status_h + 10);
			hiresbadge_place_in_header(&view, sheet, left, inset, centre_y - status_h, height);
			lv_obj_update_layout(screen);
			assert(lv_obj_get_parent(view.box) == sheet);
			shot = lv_snapshot_take(screen, LV_COLOR_FORMAT_XRGB8888);
			assert(shot);
			back_ink = header_ink(shot, left ? 150 : 0, left ? 239 : 89, status_h, status_h + 50);
			lv_area_t badge = header_ink(shot, left ? 0 : 150, left ? 89 : 239, status_h, status_h + 50);
			assert((left ? badge.x1 : 239 - badge.x2) == inset);
			assert((left ? 239 - back_ink.x2 : back_ink.x1) == inset);
			assert(lv_area_get_height(&badge) <= target_height);
			assert(lv_area_get_height(&badge) < lv_obj_get_height(view.box));
			int badge_centre = badge.y1 + lv_area_get_height(&badge) / 2;
			assert(badge_centre >= centre_y - 1 && badge_centre <= centre_y + 1);
			if (style == HIRESBADGE_LOGO) {
				assert(lv_area_get_width(&badge) == target_height);
				assert(lv_area_get_height(&badge) == target_height);
				// Allow half a pixel when the heights have different parity.
				assert(LV_ABS(badge.y1 + badge.y2 - back_ink.y1 - back_ink.y2) <= 1);
				assert(close_to(pixel_at(shot, badge.x1 + target_height / 2, badge.y1 + 1), 0xf8d17c, 14));
				assert(close_to(pixel_at(shot, badge.x1 + 2, badge.y1 + target_height * 2 / 3), 0x000000, 14));
			}
			lv_draw_buf_destroy(shot);
			lv_obj_set_hidden(view.box, true);
			int reserved = hiresbadge_header_width(&view, height);
			assert(reserved >= lv_area_get_width(&badge) && reserved <= lv_area_get_width(&badge) + 1);
			lv_obj_set_hidden(view.box, false);
		}
	}
	// The badge rides the player sheet, not the display's global top layer.
	lv_obj_set_x(sheet, 40);
	lv_obj_update_layout(screen);
	lv_area_t box;
	lv_obj_get_coords(view.box, &box);
	assert(box.x2 == 239 - inset + 40);

	// Switching back to a non-Vinyl shape restores the native-size badge at
	// its original artwork anchor, not just the parent.
	hiresbadge_place_on_art(&view, art, true, INSET);
	lv_obj_update_layout(screen);
	lv_area_t sleeve;
	lv_obj_get_coords(art, &sleeve);
	lv_obj_get_coords(view.box, &box);
	assert(lv_obj_get_parent(view.box) == art);
	assert(box.x1 == sleeve.x1 + INSET && box.y1 == sleeve.y1 + INSET);
	assert(lv_obj_get_style_transform_scale_x(view.box, 0) == LV_SCALE_NONE);
	assert(lv_obj_get_style_transform_scale_y(view.box, 0) == LV_SCALE_NONE);
	assert(lv_area_get_width(&box) == 34 && lv_area_get_height(&box) == 34);
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
	exercise_header(&lv_font_montserrat_14, 0);
	exercise_header(&lv_font_montserrat_20, 24);
	puts("PASS hi-res badge designs, sleeve corners and back-button header alignment");
	return 0;
}
