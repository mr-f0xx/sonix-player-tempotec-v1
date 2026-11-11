#include "stickerlayout.h"

int sticker_cap_offset_y(const lv_font_t *font) {
	if (!font)
		return 0;
	int line = lv_font_get_line_height(font);
	lv_font_glyph_dsc_t g;
	if (lv_font_get_glyph_dsc(font, &g, 'H', 0) && g.box_h > 0) {
		int cap_top = line - font->base_line - (int)g.box_h - (int)g.ofs_y;
		return (line - (2 * cap_top + (int)g.box_h)) / 2;
	}
	return 0;
}

void sticker_align_label_cap(lv_obj_t *label, lv_align_t align, int x_ofs) {
	if (!label)
		return;
	const lv_font_t *font = lv_obj_get_style_text_font(label, 0);
	lv_obj_align(label, align, x_ofs, sticker_cap_offset_y(font));
}

static int pill_height_for(lv_obj_t *pill) {
	int h = STICKER_PILL_H;
	if (pill && lv_obj_get_child_count(pill) > 0) {
		lv_obj_t *child = lv_obj_get_child(pill, 0);
		const lv_font_t *font = child ? lv_obj_get_style_text_font(child, 0) : NULL;
		lv_font_glyph_dsc_t g;
		if (font && lv_font_get_glyph_dsc(font, &g, 'H', 0) && g.box_h > 0) {
			int want = (int)g.box_h + 6;
			if (want > h)
				h = want;
		}
	}
	return h;
}

sticker_box_t sticker_layout_box(int screen_w, int screen_h, int title_line_h, int artist_line_h,
								 int album_line_h, int time_line_h) {
	sticker_box_t b = {0};
	if (screen_w < 1)
		screen_w = 1;
	if (screen_h < 1)
		screen_h = 1;
	if (title_line_h < 1)
		title_line_h = 1;
	if (artist_line_h < 1)
		artist_line_h = 1;
	if (album_line_h < 1)
		album_line_h = 1;
	if (time_line_h < 1)
		time_line_h = 1;

	/* CJK UI fonts (MiSans) carry internal vertical leading in line_height
	 * already; floor the strips at their compact design heights without
	 * double-padding tall line boxes, while still growing for Large text. */
	int want_title = title_line_h + 2;
	b.title_h = want_title > STICKER_TITLE_MIN_H ? want_title : STICKER_TITLE_MIN_H;
	int want_artist = artist_line_h + 1;
	b.artist_h = want_artist > STICKER_ARTIST_MIN_H ? want_artist : STICKER_ARTIST_MIN_H;
	b.album_h = album_line_h;
	b.seek_h = time_line_h > STICKER_SLIDER_H + 2 * STICKER_KNOB_PAD
				   ? time_line_h
				   : STICKER_SLIDER_H + 2 * STICKER_KNOB_PAD;

	int album_lead = album_line_h > 16 ? album_line_h - 16 : 0;
	if (album_lead > 2)
		album_lead = 2;
	int seek_lead = b.seek_h > 16 ? b.seek_h - 16 : 0;
	if (seek_lead > 2)
		seek_lead = 2;
	int album_gap = STICKER_ALBUM_GAP - album_lead;
	if (album_gap < 1)
		album_gap = 1;
	int row_gap = STICKER_ROW_GAP - seek_lead;
	if (row_gap < 1)
		row_gap = 1;

	int deck_h = STICKER_DECK_SLACK + b.title_h + STICKER_STRIP_GAP + b.artist_h +
				 album_gap + b.album_h + row_gap + b.seek_h + row_gap +
				 STICKER_PLAY_BTN + STICKER_PAD_BOTTOM;
	if (deck_h > screen_h - 32)
		deck_h = screen_h > 32 ? screen_h - 32 : 0;
	b.deck_h = deck_h;
	b.deck_y = screen_h - deck_h;
	if (b.deck_y < 0)
		b.deck_y = 0;

	b.title_w = screen_w - 2 * STICKER_TITLE_MARGIN;
	if (b.title_w < 1)
		b.title_w = 1;
	b.title_x = (screen_w - b.title_w) / 2;
	b.title_y = STICKER_DECK_SLACK;

	b.artist_w = screen_w - 2 * STICKER_ARTIST_MARGIN;
	if (b.artist_w < 1)
		b.artist_w = 1;
	b.artist_x = (screen_w - b.artist_w) / 2;
	b.artist_y = b.title_y + b.title_h + STICKER_STRIP_GAP;

	b.album_y = b.artist_y + b.artist_h + album_gap;
	b.seek_y = b.album_y + b.album_h + row_gap;
	b.slider_w = screen_w - 2 * STICKER_SEEK_SIDE;
	if (b.slider_w < 16)
		b.slider_w = screen_w > 16 ? 16 : screen_w;

	b.controls_y = screen_h - STICKER_PLAY_BTN - STICKER_PAD_BOTTOM;
	if (b.controls_y < 0)
		b.controls_y = 0;

	/* Reserve room above, below and beside the sleeve for the rotated neon
	 * frame, corner pills and right-hand back chevron. */
	int top = STICKER_TOP_MIN;
	int gap = STICKER_ART_GAP;
	if (b.deck_y < top + gap + 16) {
		top = 0;
		gap = 0;
	}
	int room_h = b.deck_y - top - gap;
	if (room_h < 2)
		room_h = 2;
	int max_h_side = (room_h - 2 * STICKER_FRAME_OUTSET) * 100 / 106;
	int max_w_side = screen_w - 2 * STICKER_SIDE_MARGIN;
	if (max_w_side < 2)
		max_w_side = screen_w;
	int side = max_h_side < max_w_side ? max_h_side : max_w_side;
	side &= ~1;
	if (side < 2)
		side = 2;
	b.art_side = side;
	b.art_x = (screen_w - side) / 2;
	b.art_y = top + (room_h - side) / 2;
	if (b.art_y < 0)
		b.art_y = 0;

	b.frame_side = side + 2 * STICKER_FRAME_OUTSET;
	b.frame_x = b.art_x - STICKER_FRAME_OUTSET;
	b.frame_y = b.art_y - STICKER_FRAME_OUTSET;

	int sd = side * STICKER_SPINDLE_PCT / 100;
	sd &= ~1;
	if (sd < 6)
		sd = 6;
	b.spindle_d = sd;
	return b;
}

void sticker_ground_apply(lv_obj_t *sheet, lv_obj_t *deck, lv_obj_t *backdrop) {
	if (sheet) {
		lv_obj_set_style_bg_color(sheet, lv_color_hex(STICKER_BG_HEX), 0);
		lv_obj_set_style_bg_opa(sheet, LV_OPA_COVER, 0);
		lv_obj_set_style_bg_grad_dir(sheet, LV_GRAD_DIR_NONE, 0);
		lv_obj_set_style_bg_image_opa(sheet, LV_OPA_TRANSP, 0);
	}
	if (deck) {
		lv_obj_set_style_bg_opa(deck, LV_OPA_TRANSP, 0);
		lv_obj_set_style_bg_image_opa(deck, LV_OPA_TRANSP, 0);
	}
	if (backdrop)
		lv_obj_set_hidden(backdrop, true);
}

void sticker_place_sleeve(lv_obj_t *panel, lv_obj_t *sleeve, const sticker_box_t *b, int screen_w) {
	if (!panel || !sleeve || !b)
		return;
	if (screen_w < 1)
		screen_w = 1;
	lv_obj_set_size(panel, screen_w, b->deck_y);
	lv_obj_align(panel, LV_ALIGN_TOP_LEFT, 0, 0);
	lv_obj_set_size(sleeve, b->art_side, b->art_side);
	lv_obj_align(sleeve, LV_ALIGN_TOP_LEFT, b->art_x, b->art_y);
	lv_obj_set_style_bg_color(sleeve, lv_color_hex(STICKER_BG_HEX), 0);
	lv_obj_set_style_bg_opa(sleeve, LV_OPA_COVER, 0);
}

static lv_obj_t *plain_box(lv_obj_t *parent) {
	lv_obj_t *obj = lv_obj_create(parent);
	lv_obj_remove_style_all(obj);
	lv_obj_set_style_pad_all(obj, 0, 0);
	lv_obj_set_style_border_width(obj, 0, 0);
	lv_obj_set_style_bg_opa(obj, LV_OPA_TRANSP, 0);
	lv_obj_set_scrollable(obj, false);
	lv_obj_set_clickable(obj, false);
	return obj;
}

static lv_obj_t *frame_ring(lv_obj_t *parent, uint32_t hex, int radius, int tilt) {
	lv_obj_t *obj = plain_box(parent);
	lv_obj_set_style_radius(obj, radius, 0);
	lv_obj_set_style_border_width(obj, STICKER_ART_BORDER, 0);
	lv_obj_set_style_border_color(obj, lv_color_hex(hex), 0);
	lv_obj_set_style_border_opa(obj, LV_OPA_COVER, 0);
	if (tilt != 0) {
		lv_obj_set_style_transform_pivot_x(obj, lv_pct(50), 0);
		lv_obj_set_style_transform_pivot_y(obj, lv_pct(50), 0);
		lv_obj_set_style_transform_rotation(obj, tilt, 0);
	}
	lv_obj_set_hidden(obj, true);
	return obj;
}

static lv_obj_t *corner_pill(lv_obj_t *parent, uint32_t hex, int tilt) {
	lv_obj_t *pill = plain_box(parent);
	lv_obj_set_size(pill, LV_SIZE_CONTENT, STICKER_PILL_H);
	lv_obj_set_style_radius(pill, LV_RADIUS_CIRCLE, 0);
	lv_obj_set_style_bg_color(pill, lv_color_hex(hex), 0);
	lv_obj_set_style_bg_opa(pill, LV_OPA_COVER, 0);
	lv_obj_set_style_pad_hor(pill, 7, 0);
	lv_obj_set_style_pad_ver(pill, 0, 0);
	lv_obj_set_style_transform_pivot_x(pill, lv_pct(50), 0);
	lv_obj_set_style_transform_pivot_y(pill, lv_pct(50), 0);
	lv_obj_set_style_transform_rotation(pill, tilt, 0);
	lv_obj_set_hidden(pill, true);
	return pill;
}

static lv_obj_t *corner_disc(lv_obj_t *parent, uint32_t hex) {
	lv_obj_t *disc = plain_box(parent);
	lv_obj_set_size(disc, STICKER_CORNER_DISC, STICKER_CORNER_DISC);
	lv_obj_set_style_radius(disc, LV_RADIUS_CIRCLE, 0);
	lv_obj_set_style_bg_color(disc, lv_color_hex(hex), 0);
	lv_obj_set_style_bg_opa(disc, LV_OPA_COVER, 0);
	lv_obj_set_hidden(disc, true);
	return disc;
}

static lv_obj_t *tape_strip(lv_obj_t *parent, uint32_t hex, int tilt) {
	lv_obj_t *strip = plain_box(parent);
	lv_obj_set_ignore_layout(strip, true);
	lv_obj_set_style_radius(strip, STICKER_STRIP_RADIUS, 0);
	lv_obj_set_style_bg_color(strip, lv_color_hex(hex), 0);
	lv_obj_set_style_bg_opa(strip, LV_OPA_COVER, 0);
	lv_obj_set_style_pad_hor(strip, 6, 0);
	lv_obj_set_style_transform_pivot_x(strip, lv_pct(50), 0);
	lv_obj_set_style_transform_pivot_y(strip, lv_pct(50), 0);
	lv_obj_set_style_transform_rotation(strip, tilt, 0);
	lv_obj_set_hidden(strip, true);
	return strip;
}

void sticker_view_build(sticker_view_t *v, lv_obj_t *sheet, lv_obj_t *cover_panel,
						lv_obj_t *sleeve, lv_obj_t *play_btn) {
	if (!v)
		return;
	*v = (sticker_view_t){0};

	if (cover_panel) {
		v->tilted_cyan = frame_ring(cover_panel, STICKER_CYAN_HEX, STICKER_FRAME_RADIUS, STICKER_FRAME_TILT);
		v->tilted_pink = frame_ring(cover_panel, STICKER_PINK_HEX, STICKER_ART_RADIUS, STICKER_FRAME_TILT);
		v->straight_cyan = frame_ring(cover_panel, STICKER_CYAN_HEX, STICKER_FRAME_RADIUS, 0);
		v->queue_pill = corner_pill(cover_panel, STICKER_YELLOW_HEX, STICKER_QUEUE_TILT);
		v->format_pill = corner_pill(cover_panel, STICKER_CYAN_HEX, STICKER_FORMAT_TILT);
		v->repeat_disc = corner_disc(cover_panel, STICKER_PINK_HEX);
		v->fav_disc = corner_disc(cover_panel, STICKER_GOLD_HEX);
	}
	if (sleeve) {
		v->spindle = plain_box(sleeve);
		lv_obj_set_style_radius(v->spindle, LV_RADIUS_CIRCLE, 0);
		lv_obj_set_style_bg_color(v->spindle, lv_color_hex(STICKER_BG_HEX), 0);
		lv_obj_set_style_bg_opa(v->spindle, LV_OPA_COVER, 0);
		lv_obj_set_style_border_width(v->spindle, STICKER_ART_BORDER, 0);
		lv_obj_set_style_border_color(v->spindle, lv_color_hex(STICKER_YELLOW_HEX), 0);
		lv_obj_set_style_border_opa(v->spindle, LV_OPA_COVER, 0);
		lv_obj_set_hidden(v->spindle, true);
	}
	if (sheet) {
		v->title_strip = tape_strip(sheet, STICKER_PINK_HEX, STICKER_TITLE_TILT);
		v->artist_strip = tape_strip(sheet, STICKER_YELLOW_HEX, STICKER_ARTIST_TILT);
		v->album_label = lv_label_create(sheet);
		lv_obj_set_ignore_layout(v->album_label, true);
		lv_label_set_text(v->album_label, "");
		lv_label_set_long_mode(v->album_label, LV_LABEL_LONG_DOT);
		lv_obj_set_style_text_color(v->album_label, lv_color_hex(STICKER_CYAN_HEX), 0);
		lv_obj_set_style_text_align(v->album_label, LV_TEXT_ALIGN_CENTER, 0);
		lv_obj_set_clickable(v->album_label, false);
		lv_obj_set_hidden(v->album_label, true);

		v->seek_row = plain_box(sheet);
		lv_obj_set_ignore_layout(v->seek_row, true);
		lv_obj_set_hidden(v->seek_row, true);
	}
	if (play_btn) {
		v->play_ring = plain_box(play_btn);
		lv_obj_set_style_radius(v->play_ring, LV_RADIUS_CIRCLE, 0);
		lv_obj_set_style_border_width(v->play_ring, 2, 0);
		lv_obj_set_style_border_color(v->play_ring, lv_color_hex(STICKER_RING_HEX), 0);
		lv_obj_set_style_border_opa(v->play_ring, LV_OPA_COVER, 0);
		lv_obj_set_hidden(v->play_ring, true);
	}
}

void sticker_view_apply(sticker_view_t *v, const sticker_box_t *b, int screen_w) {
	if (!v || !b)
		return;
	/* Keep all three decorative neon frames behind the sleeve so the rotated
	 * outlines peek out around the edges without crossing the artwork. */
	if (v->tilted_cyan) {
		lv_obj_set_size(v->tilted_cyan, b->frame_side, b->frame_side);
		lv_obj_align(v->tilted_cyan, LV_ALIGN_TOP_LEFT, b->frame_x, b->frame_y);
		lv_obj_move_to_index(v->tilted_cyan, 0);
	}
	if (v->tilted_pink) {
		lv_obj_set_size(v->tilted_pink, b->art_side, b->art_side);
		lv_obj_align(v->tilted_pink, LV_ALIGN_TOP_LEFT, b->art_x, b->art_y);
		lv_obj_move_to_index(v->tilted_pink, 1);
	}
	if (v->straight_cyan) {
		lv_obj_set_size(v->straight_cyan, b->frame_side, b->frame_side);
		lv_obj_align(v->straight_cyan, LV_ALIGN_TOP_LEFT, b->frame_x, b->frame_y);
		lv_obj_move_to_index(v->straight_cyan, 2);
	}
	if (v->spindle) {
		lv_obj_set_size(v->spindle, b->spindle_d, b->spindle_d);
		lv_obj_center(v->spindle);
		lv_obj_move_foreground(v->spindle);
	}
	if (v->queue_pill) {
		lv_obj_set_size(v->queue_pill, LV_SIZE_CONTENT, pill_height_for(v->queue_pill));
		int qx = b->art_x - 24;
		if (qx < 4)
			qx = 4;
		int qy = b->art_y - 4;
		if (qy < 1)
			qy = 1;
		lv_obj_align(v->queue_pill, LV_ALIGN_TOP_LEFT, qx, qy);
		lv_obj_move_foreground(v->queue_pill);
	}
	if (v->format_pill) {
		lv_obj_set_size(v->format_pill, LV_SIZE_CONTENT, pill_height_for(v->format_pill));
		int right_in = screen_w - (b->art_x + b->art_side + 12);
		if (screen_w >= 240 && right_in < 38)
			right_in = 38;
		if (right_in < 4)
			right_in = 4;
		lv_obj_align(v->format_pill, LV_ALIGN_TOP_RIGHT, -right_in, b->art_y + 7);
		lv_obj_move_foreground(v->format_pill);
	}
	if (v->repeat_disc) {
		lv_obj_align(v->repeat_disc, LV_ALIGN_TOP_LEFT, b->art_x + 2,
					 b->art_y + b->art_side - STICKER_CORNER_DISC - 2);
		lv_obj_move_foreground(v->repeat_disc);
	}
	if (v->fav_disc) {
		lv_obj_align(v->fav_disc, LV_ALIGN_TOP_LEFT, b->art_x + b->art_side - STICKER_CORNER_DISC - 2,
					 b->art_y + b->art_side - STICKER_CORNER_DISC - 2);
		lv_obj_move_foreground(v->fav_disc);
	}
	if (v->title_strip) {
		lv_obj_set_size(v->title_strip, b->title_w, b->title_h);
		lv_obj_align(v->title_strip, LV_ALIGN_TOP_LEFT, b->title_x, b->deck_y + b->title_y);
	}
	if (v->artist_strip) {
		lv_obj_set_size(v->artist_strip, b->artist_w, b->artist_h);
		lv_obj_align(v->artist_strip, LV_ALIGN_TOP_LEFT, b->artist_x, b->deck_y + b->artist_y);
	}
	if (v->album_label) {
		int aw = screen_w - 2 * STICKER_TITLE_MARGIN;
		if (aw < 1)
			aw = 1;
		const lv_font_t *af = lv_obj_get_style_text_font(v->album_label, 0);
		lv_obj_set_width(v->album_label, aw);
		lv_obj_set_height(v->album_label, b->album_h);
		lv_obj_align(v->album_label, LV_ALIGN_TOP_MID, 0,
					 b->deck_y + b->album_y + sticker_cap_offset_y(af));
	}
	if (v->seek_row) {
		lv_obj_set_size(v->seek_row, screen_w, b->seek_h);
		lv_obj_align(v->seek_row, LV_ALIGN_TOP_LEFT, 0, b->deck_y + b->seek_y);
	}
	if (v->play_ring) {
		int side = STICKER_PLAY_BTN - 2 * STICKER_PLAY_RING_PAD;
		if (side < 8)
			side = 8;
		lv_obj_set_size(v->play_ring, side, side);
		lv_obj_center(v->play_ring);
	}
}

void sticker_view_set_hidden(sticker_view_t *v, bool hidden) {
	if (!v)
		return;
	lv_obj_t *const all[] = {
		v->tilted_cyan, v->tilted_pink, v->straight_cyan, v->spindle,
		v->queue_pill, v->format_pill, v->repeat_disc, v->fav_disc,
		v->title_strip, v->artist_strip, v->album_label, v->seek_row, v->play_ring,
	};
	for (size_t i = 0; i < sizeof(all) / sizeof(all[0]); i++) {
		if (all[i])
			lv_obj_set_hidden(all[i], hidden);
	}
}
