/* Waveform's sleeve pills and favourite on LVGL's actual layout engine. */
#include <assert.h>
#include <stdio.h>

#include "src/gui/nowplaying/waveformoverlay.h"
#include "src/gui/shell/scrolltext.h"

lv_font_t font_ui_14;
void fonts_register_change(void (*cb)(void)) { (void)cb; }

static lv_obj_t *plain(lv_obj_t *parent) {
	lv_obj_t *obj = lv_obj_create(parent);
	lv_obj_remove_style_all(obj);
	lv_obj_set_scrollable(obj, false);
	return obj;
}

static void exercise(int width, const lv_font_t *font, int shape) {
	const int inset = 6, pill_pad = 10, fav_size = WAVEFORM_FAV_COMPACT_SIZE, gap = 4;
	lv_obj_t *screen = lv_obj_create(NULL);
	lv_obj_set_style_pad_all(screen, 0, 0);
	lv_obj_t *art = plain(screen);
	lv_obj_set_size(art, width, width);
	lv_obj_set_style_radius(art, shape == 2 ? LV_RADIUS_CIRCLE : shape == 1 ? 14 : 0, 0);
	// Vinyl keeps the pill overlays outside the image clip.
	lv_obj_set_style_clip_corner(art, shape != 2, 0);

	lv_obj_t *column = plain(art);
	lv_obj_set_size(column, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
	lv_obj_align(column, LV_ALIGN_BOTTOM_LEFT, inset, -inset);
	lv_obj_set_flex_flow(column, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(column, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
	lv_obj_set_style_pad_row(column, 6, 0);
	lv_obj_t *pill[2], *label[2];
	for (int i = 0; i < 2; i++) {
		pill[i] = plain(column);
		lv_obj_set_size(pill[i], LV_SIZE_CONTENT, LV_SIZE_CONTENT);
		lv_obj_set_style_radius(pill[i], LV_RADIUS_CIRCLE, 0);
		lv_obj_set_style_pad_hor(pill[i], pill_pad, 0);
		lv_obj_set_style_pad_ver(pill[i], 5, 0);
		label[i] = lv_label_create(pill[i]);
		lv_obj_set_style_text_font(label[i], font, 0);
		scrolltext_apply(label[i]);
		lv_obj_set_width(label[i], LV_SIZE_CONTENT);
		lv_obj_set_style_max_width(label[i], waveform_pill_text_room(width, inset, fav_size, pill_pad, gap), 0);
	}
	lv_obj_t *disc = plain(art);
	lv_obj_set_size(disc, fav_size, fav_size);
	lv_obj_set_style_radius(disc, LV_RADIUS_CIRCLE, 0);
	lv_obj_align(disc, LV_ALIGN_BOTTOM_RIGHT, -inset, -inset);
	lv_obj_t *button = lv_btn_create(disc);
	lv_obj_set_size(button, 34, 26); // the production touch target remains unchanged
	lv_obj_center(button);

	const char *titles[] = {"A very long title that needs to scroll past the favourite button", "Short"};
	for (int t = 0; t < 2; t++) {
		for (int artist = 0; artist < 2; artist++) {
			scrolltext_set(label[0], titles[t]);
			scrolltext_set(label[1], "An exceptionally long artist name that must scroll");
			lv_obj_set_hidden(pill[1], !artist);
			lv_obj_update_layout(screen);
			lv_area_t fav_area, first, second, art_area;
			lv_obj_get_coords(disc, &fav_area);
			lv_obj_get_coords(pill[0], &first);
			lv_obj_get_coords(pill[1], &second);
			lv_obj_get_coords(art, &art_area);
			assert(first.x2 + gap < fav_area.x1);
			if (artist) assert(second.x2 + gap < fav_area.x1);
			assert(first.x1 >= art_area.x1 && first.y1 >= art_area.y1);
			assert(fav_area.x2 <= art_area.x2 && fav_area.y2 <= art_area.y2);
			assert(lv_obj_get_width(button) == 34 && lv_obj_get_height(button) == 26);
			if (t == 0) assert(lv_label_get_long_mode(label[0]) == LV_LABEL_LONG_SCROLL_CIRCULAR);
		}
	}
	lv_obj_delete(screen);
}

int main(void) {
	font_ui_14 = lv_font_montserrat_14;
	lv_init();
	lv_display_create(240, 320);
	for (int width = 128; width <= 224; width += 32) {
		for (int shape = 0; shape < 3; shape++) {
			exercise(width, &lv_font_montserrat_14, shape);
			exercise(width, &lv_font_montserrat_20, shape);
		}
	}
	puts("PASS waveform overlay (long and short titles, with and without artist, two font sizes)");
	return 0;
}
