/* Sticker player layout geometry and real LVGL widget layering on 240x320. */
#include <assert.h>
#include <stdio.h>

#include "lvgl/lvgl.h"
#include "src/gui/nowplaying/stickerlayout.h"

lv_font_t font_ui_14;

#define CHECK(cond, msg, ...) do { \
	if (!(cond)) { \
		fprintf(stderr, "FAIL: " msg "\n", ##__VA_ARGS__); \
		return 1; \
	} \
} while (0)

static int check_geometry(int screen_h, int title_fh, int artist_fh, int album_fh, int time_fh) {
	sticker_box_t b = sticker_layout_box(240, screen_h, title_fh, artist_fh, album_fh, time_fh);

	CHECK(b.art_side >= 80 && (b.art_side & 1) == 0,
		  "h=%d art_side=%d must be even and >= 80", screen_h, b.art_side);
	CHECK(b.art_x == (240 - b.art_side) / 2,
		  "art_x=%d must center art_side=%d", b.art_x, b.art_side);
	CHECK(b.art_y >= STICKER_TOP_MIN,
		  "art_y=%d below STICKER_TOP_MIN", b.art_y);
	CHECK(b.art_y + b.art_side + STICKER_ART_GAP <= b.deck_y,
		  "sleeve bottom %d reaches deck_y %d", b.art_y + b.art_side + STICKER_ART_GAP, b.deck_y);
	CHECK(b.frame_side == b.art_side + 2 * STICKER_FRAME_OUTSET,
		  "frame_side=%d expected %d", b.frame_side, b.art_side + 2 * STICKER_FRAME_OUTSET);
	CHECK(b.spindle_d >= 18 && (b.spindle_d & 1) == 0,
		  "spindle_d=%d must be even and >= 18", b.spindle_d);
	CHECK(b.deck_y + b.deck_h == screen_h,
		  "deck_y=%d + deck_h=%d != screen_h=%d", b.deck_y, b.deck_h, screen_h);
	CHECK(b.artist_y > b.title_y + b.title_h,
		  "artist_y=%d overlaps title bottom %d", b.artist_y, b.title_y + b.title_h);
	CHECK(b.album_y >= b.artist_y + b.artist_h,
		  "album_y=%d overlaps artist bottom %d", b.album_y, b.artist_y + b.artist_h);
	CHECK(b.seek_y >= b.album_y + b.album_h,
		  "seek_y=%d overlaps album bottom %d", b.seek_y, b.album_y + b.album_h);
	CHECK(b.controls_y >= b.seek_y + b.seek_h,
		  "controls_y=%d overlaps seek bottom %d", b.controls_y, b.seek_y + b.seek_h);
	CHECK(b.slider_w >= 48 && b.slider_w <= 240 - 2 * STICKER_SEEK_SIDE,
		  "slider_w=%d out of bounds", b.slider_w);
	return 0;
}

int main(void) {
	lv_init();
	font_ui_14 = lv_font_montserrat_14;
	lv_display_t *disp = lv_display_create(240, 320);
	static uint8_t buf[240 * 320 * 4];
	lv_display_set_buffers(disp, buf, NULL, sizeof(buf), LV_DISPLAY_RENDER_MODE_DIRECT);

	/* Both full-screen (320 px) and status-bar-visible (296 px) heights across text sizes. */
	if (check_geometry(320, 16, 15, 14, 14)) return 1;
	if (check_geometry(296, 16, 15, 14, 14)) return 1;
	if (check_geometry(296, 15, 14, 13, 13)) return 1;
	if (check_geometry(296, 17, 16, 15, 15)) return 1;

	lv_obj_t *sheet = lv_screen_active();
	lv_obj_t *backdrop = lv_image_create(sheet);
	lv_obj_t *panel = lv_obj_create(sheet);
	lv_obj_remove_style_all(panel);
	lv_obj_t *sleeve = lv_obj_create(panel);
	lv_obj_remove_style_all(sleeve);
	lv_obj_t *deck = lv_obj_create(sheet);
	lv_obj_remove_style_all(deck);
	lv_obj_t *play_btn = lv_obj_create(deck);
	lv_obj_remove_style_all(play_btn);
	lv_obj_set_size(play_btn, STICKER_PLAY_BTN, STICKER_PLAY_BTN);

	sticker_view_t v = {0};
	sticker_view_build(&v, sheet, panel, sleeve, play_btn);
	CHECK(v.tilted_cyan && v.tilted_pink && v.straight_cyan, "decorative frames built");
	CHECK(v.spindle && v.queue_pill && v.format_pill && v.repeat_disc && v.fav_disc, "sleeve overlays built");
	CHECK(v.title_strip && v.artist_strip && v.album_label && v.seek_row && v.play_ring, "deck overlays built");

	/* Initially hidden until sticker_put activates the layout. */
	CHECK(lv_obj_is_hidden(v.tilted_cyan) && lv_obj_is_hidden(v.title_strip) && lv_obj_is_hidden(v.play_ring),
		  "sticker widgets must start hidden");

	lv_obj_t *q_lbl = lv_label_create(v.queue_pill);
	lv_label_set_text(q_lbl, "5/15");
	lv_obj_t *f_lbl = lv_label_create(v.format_pill);
	lv_label_set_text(f_lbl, "24/96 FLAC");

	sticker_box_t b = sticker_layout_box(240, 296, 16, 15, 14, 14);
	sticker_ground_apply(sheet, deck, backdrop);
	sticker_place_sleeve(panel, sleeve, &b, 240);
	sticker_view_set_hidden(&v, false);
	sticker_view_apply(&v, &b, 240);
	lv_obj_update_layout(sheet);

	CHECK(lv_obj_is_hidden(backdrop), "backdrop must be hidden on sticker ground");
	CHECK(lv_obj_get_style_bg_opa(sleeve, 0) == LV_OPA_COVER,
		  "sleeve must have opaque ground to mask tilted frames behind it");
	CHECK(lv_obj_get_index(v.tilted_cyan) < lv_obj_get_index(sleeve),
		  "tilted_cyan must sit behind sleeve");
	CHECK(lv_obj_get_index(v.tilted_pink) < lv_obj_get_index(sleeve),
		  "tilted_pink must sit behind sleeve");
	CHECK(lv_obj_get_index(v.straight_cyan) < lv_obj_get_index(sleeve),
		  "straight_cyan must sit behind sleeve");
	CHECK(lv_obj_get_index(v.queue_pill) > lv_obj_get_index(sleeve),
		  "queue_pill must sit in front of sleeve");
	CHECK(lv_obj_get_index(v.format_pill) > lv_obj_get_index(sleeve),
		  "format_pill must sit in front of sleeve");

	lv_area_t q_area, f_area, r_area, fav_area;
	lv_obj_get_coords(v.queue_pill, &q_area);
	lv_obj_get_coords(v.format_pill, &f_area);
	lv_obj_get_coords(v.repeat_disc, &r_area);
	lv_obj_get_coords(v.fav_disc, &fav_area);
	CHECK(q_area.x2 < f_area.x1, "queue_pill x2=%d overlaps format_pill x1=%d",
		  (int)q_area.x2, (int)f_area.x1);
	CHECK(r_area.x2 < fav_area.x1, "repeat_disc x2=%d overlaps fav_disc x1=%d",
		  (int)r_area.x2, (int)fav_area.x1);

	sticker_view_set_hidden(&v, true);
	CHECK(lv_obj_is_hidden(v.tilted_cyan) && lv_obj_is_hidden(v.title_strip) && lv_obj_is_hidden(v.play_ring),
		  "sticker_view_set_hidden(true) must hide all sticker widgets");

	printf("PASS sticker layout geometry and LVGL layering\n");
	return 0;
}
