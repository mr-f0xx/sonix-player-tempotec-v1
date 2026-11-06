/* Vinyl sleeve/record geometry and layering on the real LVGL renderer.
 * The deck is represented by its measured height; the production player's
 * audio/network shell is not linked. Checks the V1, larger text, repeated
 * apply/take-back, and the no-art fallback's reserved record space. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

#include "src/gui/nowplaying/vinylayout.h"

lv_font_t font_ui_14;

static void check_box(int w, int h, int info_h) {
	int deck_h = vinyl_deck_height(info_h);
	vinyl_box_t b = vinyl_layout_box(w, h, deck_h);
	assert(b.art_side > 0 && b.record_d > 0);
	assert(b.art_x >= 0 && b.art_x + b.art_side <= w);
	assert(b.art_y >= 0);
	assert(b.record_x >= 0 && b.record_x + b.record_d <= w);
	assert(b.record_y >= b.art_y);
	assert(b.record_cy + b.record_d / 2 <= b.deck_y);
	if (w >= 240 && h >= 320) {
		/* The entire exposed disc clears the whole deck, including the
		 * transparent info and seek rows -- not only the transport. */
		assert(b.record_cy + b.record_d / 2 + VINYL_GAP <= b.deck_y);
		int info_y = b.deck_y + VINYL_DECK_PAD_TOP;
		int seek_y = info_y + info_h + VINYL_DECK_GAP;
		int record_bottom = b.record_cy + b.record_d / 2;
		assert(record_bottom < info_y && record_bottom < seek_y && record_bottom < b.controls_y);
	}
	assert(b.art_y + b.art_side <= b.deck_y);
	if (h >= deck_h + 32)
		assert(b.deck_y + deck_h <= h);
	assert(b.art_side % 2 == 0 && b.record_d % 2 == 0);
	assert(b.record_d < b.art_side);
	assert(b.record_cy - b.record_d / 2 < b.art_y + b.art_side);
	assert(b.record_cy + b.record_d / 2 > b.art_y + b.art_side);
}

static void check_pressing_mark(const vinyl_record_t *record, const vinyl_box_t *b, const lv_obj_t *sleeve) {
	lv_area_t disc, mark, cover, rotated;
	lv_obj_get_coords(record->disc, &disc);
	lv_obj_get_coords(record->shine, &mark);
	lv_obj_get_coords(sleeve, &cover);
	int d = b->record_d;
	int mark_w = (d + 86) / 173;
	if (mark_w < 1)
		mark_w = 1;
	int mark_h = d * VINYL_MARK_DY_PERCENT / 100;
	if (mark_h < 2)
		mark_h = 2;
	assert(lv_obj_get_width(record->shine) == mark_w);
	assert(lv_obj_get_height(record->shine) == mark_h);
	assert(lv_obj_get_style_transform_rotation(record->shine, 0) == VINYL_MARK_ANGLE);
	/* The reference stroke sits in the exposed lower-right arc, not on the
	 * hidden disc centre and not on the vertical axis. */
	assert(mark.x1 + mark.x2 > disc.x1 + disc.x2);
	rotated = mark;
	lv_obj_get_transformed_area(record->shine, &rotated, LV_OBJ_POINT_TRANSFORM_FLAG_NONE);
	assert(rotated.x2 > rotated.x1);
	assert(rotated.y1 > cover.y2 && rotated.y2 <= disc.y2);
	assert(rotated.x1 >= disc.x1 && rotated.x2 <= disc.x2);
}

int main(void) {
	for (int info = 24; info <= 52; info += 4)
		check_box(240, 320, info);
	check_box(360, 640, 34);
	check_box(64, 64, 120);
	check_box(64, 64, 20);

	font_ui_14 = lv_font_montserrat_14;
	lv_init();
	lv_display_t *display = lv_display_create(240, 320);
	static uint8_t buffer[240 * 320 * 4];
	lv_display_set_buffers(display, buffer, NULL, sizeof(buffer), LV_DISPLAY_RENDER_MODE_DIRECT);
	lv_obj_t *screen = lv_screen_active();
	lv_obj_set_style_bg_color(screen, lv_color_black(), 0);
	lv_obj_set_style_pad_all(screen, 0, 0);

	vinyl_record_t record;
	vinyl_record_build(&record, screen);
	lv_obj_t *sleeve = lv_obj_create(screen);
	lv_obj_remove_style_all(sleeve);
	lv_obj_set_style_bg_color(sleeve, lv_color_make(80, 100, 140), 0);
	lv_obj_set_style_bg_opa(sleeve, LV_OPA_COVER, 0);
	lv_obj_set_style_radius(sleeve, VINYL_ART_RADIUS, 0);
	lv_obj_set_style_border_width(sleeve, VINYL_ART_BORDER, 0);
	lv_obj_set_style_border_color(sleeve, lv_color_white(), 0);
	lv_obj_set_style_clip_corner(sleeve, true, 0);
	lv_obj_set_scrollable(sleeve, false);
	lv_obj_set_clickable(sleeve, false);

	for (int info = 24; info <= 52; info += 4) {
		vinyl_box_t b = vinyl_layout_box(240, 320, vinyl_deck_height(info));
		for (int pass = 0; pass < 3; pass++) {
			vinyl_record_apply(&record, &b, lv_color_white());
			lv_obj_set_size(sleeve, b.art_side, b.art_side);
			lv_obj_set_pos(sleeve, b.art_x, b.art_y);
			lv_obj_move_background(record.disc);
			lv_obj_update_layout(screen);
			assert(lv_obj_get_index(record.disc) < lv_obj_get_index(sleeve));
			assert(lv_obj_get_width(record.disc) == b.record_d);
			assert(lv_obj_get_height(sleeve) == b.art_side);
			check_pressing_mark(&record, &b, sleeve);
			assert(!lv_obj_is_hidden(record.disc));
			lv_obj_set_hidden(record.disc, true);
			assert(lv_obj_is_hidden(record.disc));
		}
	}
	vinyl_box_t b = vinyl_layout_box(360, 640, vinyl_deck_height(34));
	vinyl_record_apply(&record, &b, lv_color_white());
	lv_obj_set_size(sleeve, b.art_side, b.art_side);
	lv_obj_set_pos(sleeve, b.art_x, b.art_y);
	lv_obj_update_layout(screen);
	check_pressing_mark(&record, &b, sleeve);
	lv_refr_now(display);
	puts("PASS vinyl layout (V1, eight text metrics, reference-slanted pressing mark, no deck/info/seek/transport overlap, tiny-screen fallback, repeated apply/take-back and sleeve-over-record layering)");
	return 0;
}
