#include "vinylayout.h"

vinyl_box_t vinyl_layout_box(int screen_w, int screen_h, int deck_h) {
	vinyl_box_t b = {0};
	if (screen_w < 1)
		screen_w = 1;
	if (screen_h < 1)
		screen_h = 1;
	if (deck_h < 0)
		deck_h = 0;
	/* On a panel too short for the full deck, keep a drawable art area rather
	 * than handing widgets negative sizes. The V1 never takes this path. */
	if (deck_h > screen_h - 32)
		deck_h = screen_h > 32 ? screen_h - 32 : 0;
	b.deck_y = screen_h - deck_h;
	if (b.deck_y < 0)
		b.deck_y = 0;
	/* The transport is the last row, so its top is independent of text size. */
	b.controls_y = screen_h - VINYL_PLAY_BTN - VINYL_DECK_PAD_BOTTOM - VINYL_DECK_SLACK;
	if (b.controls_y < 0)
		b.controls_y = 0;

	/* Reserve the WHOLE exposed record above the deck, not just above the
	 * transport. Transparent rows are still rows: drawing the disc behind
	 * their text or seek bar reads as an overlap. The sleeve alone may cover
	 * the disc; every other widget starts below it with a clear gap. */
	int top = VINYL_TOP;
	int gap = VINYL_GAP;
	if (b.deck_y < top + gap + 8) {
		top = 0;
		gap = 0;
	}
	int room = b.deck_y - top - gap;
	if (room < 1)
		room = 1;
	int side = room * 100 / (100 + VINYL_PEEK_PERCENT);
	int sleeve_room = b.deck_y - top;
	if (sleeve_room < 1)
		sleeve_room = 1;
	if (side > sleeve_room)
		side = sleeve_room;
	int max_side = screen_w - 2 * VINYL_SIDE_MARGIN;
	if (max_side < 1)
		max_side = 1;
	if (side > max_side)
		side = max_side;
	side &= ~1;
	if (side < 2)
		side = 2;
	b.art_side = side;
	b.art_x = (screen_w - side) / 2;
	b.art_y = top;

	int d = side * VINYL_RECORD_PERCENT / 100;
	d &= ~1;
	if (d < 2)
		d = 2;
	b.record_d = d;
	b.record_cx = screen_w / 2;
	b.record_cy = b.art_y + side - side * VINYL_RECESS_PERCENT / 100;
	b.record_x = b.record_cx - d / 2;
	b.record_y = b.record_cy - d / 2;
	return b;
}

static lv_obj_t *plain_circle(lv_obj_t *parent) {
	lv_obj_t *obj = lv_obj_create(parent);
	lv_obj_remove_style_all(obj);
	lv_obj_set_style_radius(obj, LV_RADIUS_CIRCLE, 0);
	lv_obj_set_style_pad_all(obj, 0, 0);
	lv_obj_set_style_border_width(obj, 0, 0);
	lv_obj_set_style_bg_opa(obj, LV_OPA_TRANSP, 0);
	lv_obj_set_scrollable(obj, false);
	lv_obj_set_clickable(obj, false);
	return obj;
}

void vinyl_record_build(vinyl_record_t *r, lv_obj_t *parent) {
	r->disc = plain_circle(parent);
	lv_obj_set_style_bg_color(r->disc, lv_color_black(), 0);
	lv_obj_set_style_bg_opa(r->disc, LV_OPA_COVER, 0);
	lv_obj_set_style_border_width(r->disc, VINYL_ART_BORDER, 0);
	lv_obj_set_style_border_opa(r->disc, LV_OPA_COVER, 0);
	/* Quiet concentric grooves, not a bright target pattern. */
	for (int i = 0; i < 3; i++) {
		r->grooves[i] = plain_circle(r->disc);
		lv_obj_set_style_border_width(r->grooves[i], 1, 0);
		lv_obj_set_style_border_color(r->grooves[i], lv_color_make(38, 38, 38), 0);
		lv_obj_set_style_border_opa(r->grooves[i], LV_OPA_COVER, 0);
	}
	r->label = plain_circle(r->disc);
	lv_obj_set_style_border_width(r->label, VINYL_ART_BORDER, 0);
	lv_obj_set_style_border_opa(r->label, LV_OPA_COVER, 0);
	r->shine = lv_obj_create(r->disc);
	lv_obj_remove_style_all(r->shine);
	lv_obj_set_style_radius(r->shine, LV_RADIUS_CIRCLE, 0);
	lv_obj_set_style_bg_color(r->shine, lv_color_white(), 0);
	lv_obj_set_style_bg_opa(r->shine, LV_OPA_COVER, 0);
	lv_obj_set_scrollable(r->shine, false);
	lv_obj_set_clickable(r->shine, false);
}

void vinyl_record_apply(vinyl_record_t *r, const vinyl_box_t *b, lv_color_t rim) {
	if (!r->disc || !b)
		return;
	int d = b->record_d;
	lv_obj_set_size(r->disc, d, d);
	lv_obj_set_pos(r->disc, b->record_x, b->record_y);
	lv_obj_set_style_border_color(r->disc, rim, 0);
	for (int i = 0; i < 3; i++) {
		int gd = d - d * (12 + i * 10) / 100;
		lv_obj_set_size(r->grooves[i], gd, gd);
		lv_obj_center(r->grooves[i]);
	}
	int ld = d * 28 / 100;
	lv_obj_set_size(r->label, ld, ld);
	lv_obj_center(r->label);
	lv_obj_set_style_border_color(r->label, rim, 0);
	/* The reference's small pressing mark: a thin rounded stroke leaning
	 * down to the right, in the exposed lower arc. A rotated rounded object
	 * keeps LV_USE_LINE off in the firmware build and needs no persistent
	 * image buffer; it is painted only when the layout/theme changes. */
	int mark_w = (d + 86) / 173;
	if (mark_w < 1)
		mark_w = 1;
	int mark_h = d * VINYL_MARK_DY_PERCENT / 100;
	if (mark_h < 2)
		mark_h = 2;
	int x1 = d * VINYL_MARK_X1_PERCENT / 100;
	int y1 = d * VINYL_MARK_Y1_PERCENT / 100;
	int dx = d * VINYL_MARK_DX_PERCENT / 100;
	if (dx < 1)
		dx = 1;
	lv_obj_set_size(r->shine, mark_w, mark_h);
	lv_obj_set_pos(r->shine, x1 + dx / 2 - mark_w / 2, y1);
	lv_obj_set_style_transform_pivot_x(r->shine, lv_pct(50), 0);
	lv_obj_set_style_transform_pivot_y(r->shine, lv_pct(50), 0);
	lv_obj_set_style_transform_rotation(r->shine, VINYL_MARK_ANGLE, 0);
	lv_obj_set_hidden(r->disc, false);
}
