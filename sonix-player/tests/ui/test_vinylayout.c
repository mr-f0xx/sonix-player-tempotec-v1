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

static void check_nested_placement(lv_obj_t *panel, lv_obj_t *sleeve, vinyl_record_t *record, const vinyl_box_t *b, int screen_w) {
	/* Seed the anchors the standard layout leaves behind. A bare set_pos()
	 * on a TOP_MID sleeve adds art_x to the centred anchor a second time:
	 * the sleeve drifts right while the record stays put. */
	lv_obj_align(panel, LV_ALIGN_TOP_MID, 37, 12);
	lv_obj_align(sleeve, LV_ALIGN_CENTER, 13, -7);
	vinyl_place_sleeve(panel, sleeve, b, screen_w);
	vinyl_record_apply(record, b, lv_color_white());
	lv_obj_move_background(record->disc);
	lv_obj_update_layout(panel);
	lv_area_t sleeve_area, disc_area;
	lv_obj_get_coords(sleeve, &sleeve_area);
	lv_obj_get_coords(record->disc, &disc_area);
	assert(lv_obj_get_x(panel) == 0);
	assert(lv_obj_get_y(panel) == 0);
	assert(lv_obj_get_width(panel) == screen_w);
	assert(lv_obj_get_height(panel) == b->deck_y);
	assert(lv_obj_get_x(sleeve) == b->art_x);
	assert(lv_obj_get_y(sleeve) == b->art_y);
	assert(sleeve_area.x1 == b->art_x);
	assert(sleeve_area.y1 == b->art_y);
	assert(disc_area.x1 == b->record_x);
	assert(disc_area.y1 == b->record_y);
	assert(sleeve_area.x1 + sleeve_area.x2 == disc_area.x1 + disc_area.x2);
	assert(lv_obj_get_index(record->disc) < lv_obj_get_index(panel));
	check_pressing_mark(record, b, sleeve);

	/* A return through the standard compact placer must not reintroduce the
	 * centred anchor, and the lyrics travel must move sleeve and record as
	 * one pair rather than separating them. */
	lv_obj_align(sleeve, LV_ALIGN_TOP_MID, 0, 22);
	vinyl_place_sleeve(panel, sleeve, b, screen_w);
	lv_obj_set_style_translate_x(sleeve, -40, 0);
	lv_obj_set_style_translate_x(record->disc, -40, 0);
	lv_obj_update_layout(panel);
	lv_obj_get_coords(sleeve, &sleeve_area);
	lv_obj_get_coords(record->disc, &disc_area);
	assert(sleeve_area.x1 == b->art_x - 40);
	assert(sleeve_area.x1 + sleeve_area.x2 == disc_area.x1 + disc_area.x2);
	lv_obj_remove_local_style_prop(sleeve, LV_STYLE_TRANSLATE_X, 0);
	lv_obj_remove_local_style_prop(record->disc, LV_STYLE_TRANSLATE_X, 0);
	vinyl_place_sleeve(panel, sleeve, b, screen_w);
	vinyl_record_apply(record, b, lv_color_white());
	lv_obj_update_layout(panel);
	lv_obj_get_coords(sleeve, &sleeve_area);
	lv_obj_get_coords(record->disc, &disc_area);
	assert(sleeve_area.x1 == b->art_x);
	assert(sleeve_area.x1 + sleeve_area.x2 == disc_area.x1 + disc_area.x2);
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
	lv_display_set_color_format(display, LV_COLOR_FORMAT_XRGB8888);
	lv_obj_t *screen = lv_screen_active();
	lv_obj_set_style_bg_color(screen, lv_color_black(), 0);
	lv_obj_set_style_pad_all(screen, 0, 0);

	/* A tinted theme (Tokyo Night's navy, say) must not show through the
	 * Vinyl sheet, and the standard layout's blurred cover must be taken
	 * down. The deck stays transparent over the black ground. */
	lv_style_t themed;
	lv_style_init(&themed);
	lv_style_set_bg_color(&themed, lv_color_hex(0x1a1b26));
	lv_style_set_bg_opa(&themed, LV_OPA_COVER);
	lv_obj_add_style(screen, &themed, 0);
	lv_obj_t *deck = lv_obj_create(screen);
	lv_obj_remove_style_all(deck);
	lv_obj_set_size(deck, 240, 110);
	lv_obj_set_pos(deck, 0, 210);
	lv_obj_set_style_bg_color(deck, lv_color_hex(0x1a1b26), 0);
	lv_obj_set_style_bg_opa(deck, LV_OPA_COVER, 0);
	lv_obj_t *backdrop = lv_obj_create(screen);
	lv_obj_set_size(backdrop, 240, 320);
	vinyl_ground_apply(screen, deck, backdrop);
	lv_refr_now(display);
	assert(lv_color_eq(lv_obj_get_style_bg_color(screen, 0), lv_color_black()));
	assert(lv_obj_get_style_bg_opa(screen, 0) == LV_OPA_COVER);
	assert(lv_obj_get_style_bg_opa(deck, 0) == LV_OPA_TRANSP);
	assert(lv_obj_is_hidden(backdrop));
	assert((*(uint32_t *)buffer & 0x00FFFFFFu) == 0x000000u);
	lv_obj_delete(backdrop);
	lv_obj_delete(deck);
	lv_obj_remove_style(screen, &themed, 0);

	/* The production sleeve is nested in a full-width cover panel, not
	 * placed straight on the sheet. */
	lv_obj_t *panel = lv_obj_create(screen);
	lv_obj_remove_style_all(panel);
	lv_obj_set_style_bg_opa(panel, LV_OPA_TRANSP, 0);
	lv_obj_t *sleeve = lv_obj_create(panel);
	lv_obj_remove_style_all(sleeve);
	lv_obj_set_style_bg_color(sleeve, lv_color_make(80, 100, 140), 0);
	lv_obj_set_style_bg_opa(sleeve, LV_OPA_COVER, 0);
	lv_obj_set_style_radius(sleeve, VINYL_ART_RADIUS, 0);
	lv_obj_set_style_border_width(sleeve, VINYL_ART_BORDER, 0);
	lv_obj_set_style_border_color(sleeve, lv_color_white(), 0);
	lv_obj_set_style_clip_corner(sleeve, true, 0);
	lv_obj_set_scrollable(sleeve, false);
	lv_obj_set_clickable(sleeve, false);
	/* The record is a sheet-level sibling behind the cover panel, exactly as
	 * player_init() builds it; the sleeve lives inside the panel. */
	vinyl_record_t record;
	vinyl_record_build(&record, screen);

	for (int info = 24; info <= 52; info += 4) {
		vinyl_box_t b = vinyl_layout_box(240, 320, vinyl_deck_height(info));
		for (int pass = 0; pass < 3; pass++)
			check_nested_placement(panel, sleeve, &record, &b, 240);
	}
	assert(!lv_obj_is_hidden(record.disc));
	lv_obj_set_hidden(record.disc, true);
	assert(lv_obj_is_hidden(record.disc));
	lv_obj_set_hidden(record.disc, false);

	/* The 360x640 concept is geometry-only here: the native display is
	 * 240x320, and a wider panel is not a firmware path. */
	vinyl_box_t b = vinyl_layout_box(360, 640, vinyl_deck_height(34));
	check_nested_placement(panel, sleeve, &record, &b, 360);
	puts("PASS vinyl layout (V1, eight text metrics, reference-slanted pressing mark, no deck/info/seek/transport overlap, tiny-screen fallback, repeated apply/take-back, nested-panel alignment and black ground)");
	return 0;
}
