/* V1 deck geometry on LVGL's real flex layout. A hidden, empty info row still
 * occupies a flex gap unless the row itself is hidden: the production height
 * calculation must account for every visible row, including after switching. */
#include <assert.h>
#include <stdio.h>

#include "src/gui/nowplaying/playerdeck.h"

lv_font_t font_ui_14; // LVGL default font in the headless configuration

#define SCREEN_W 240
#define SCREEN_H 320
#define SIDE 12
#define SKIP_SIZE 42
#define CORNER_SIZE 36
#define FAV_HEIGHT 26

static lv_obj_t *transparent(lv_obj_t *parent) {
	lv_obj_t *obj = lv_obj_create(parent);
	lv_obj_remove_style_all(obj);
	lv_obj_set_scrollable(obj, false);
	return obj;
}

static int top_y(lv_obj_t *obj) {
	lv_area_t a;
	lv_obj_get_coords(obj, &a);
	return a.y1;
}
static int bottom_y(lv_obj_t *obj) {
	lv_area_t a;
	lv_obj_get_coords(obj, &a);
	return a.y2;
}

static void exercise(int line_height) {
	lv_font_t font = lv_font_montserrat_14;
	font.line_height = line_height;
	lv_obj_t *screen = lv_obj_create(NULL);
	lv_obj_set_size(screen, SCREEN_W, SCREEN_H);
	lv_obj_set_style_pad_all(screen, 0, 0);

	lv_obj_t *deck = transparent(screen);
	lv_obj_set_flex_flow(deck, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(deck, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_hor(deck, SIDE, 0);

	lv_obj_t *info = transparent(deck);
	lv_obj_set_size(info, lv_pct(100), LV_SIZE_CONTENT);
	lv_obj_set_flex_flow(info, LV_FLEX_FLOW_ROW);
	lv_obj_t *names = transparent(info);
	lv_obj_set_size(names, 100, LV_SIZE_CONTENT);
	lv_obj_set_flex_flow(names, LV_FLEX_FLOW_COLUMN);
	for (int i = 0; i < 2; i++) {
		lv_obj_t *label = lv_label_create(names);
		lv_obj_set_style_text_font(label, &font, 0);
		lv_label_set_text(label, i ? "Artist" : "Title");
	}
	lv_obj_t *side = transparent(info);
	lv_obj_set_size(side, 40, LV_SIZE_CONTENT);
	lv_obj_set_flex_flow(side, LV_FLEX_FLOW_COLUMN);
	lv_obj_t *fav = transparent(side);
	lv_obj_set_size(fav, 34, FAV_HEIGHT);
	lv_obj_t *format = lv_label_create(side);
	lv_obj_set_style_text_font(format, &font, 0);
	lv_label_set_text(format, "FLAC");

	lv_obj_t *wave = transparent(deck);
	lv_obj_set_size(wave, lv_pct(100), COMPACT_WAVE_HEIGHT);
	lv_obj_t *slider = transparent(deck);
	lv_obj_set_size(slider, lv_pct(100), PROGRESS_TRACK_HEIGHT);
	lv_obj_t *clocks = transparent(deck);
	lv_obj_set_size(clocks, lv_pct(100), LV_SIZE_CONTENT);
	for (int i = 0; i < 3; i++) {
		lv_obj_t *label = lv_label_create(clocks);
		lv_obj_set_style_text_font(label, &font, 0);
		lv_label_set_text(label, i == 1 ? "2/17" : "1:10");
		lv_obj_set_align(label, i == 0 ? LV_ALIGN_LEFT_MID : i == 1 ? LV_ALIGN_CENTER : LV_ALIGN_RIGHT_MID);
	}
	lv_obj_t *controls = transparent(deck);
	lv_obj_set_size(controls, SCREEN_W - 2 * SIDE, LV_SIZE_CONTENT);
	lv_obj_set_flex_flow(controls, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(controls, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_column(controls, 8, 0);
	lv_obj_t *repeat = transparent(controls);
	lv_obj_set_size(repeat, CORNER_SIZE, CORNER_SIZE);
	lv_obj_set_ignore_layout(repeat, true);
	lv_obj_align(repeat, LV_ALIGN_LEFT_MID, 0, 0);
	lv_obj_t *more = transparent(controls);
	lv_obj_set_size(more, CORNER_SIZE, CORNER_SIZE);
	lv_obj_set_ignore_layout(more, true);
	lv_obj_align(more, LV_ALIGN_RIGHT_MID, 0, 0);
	lv_obj_t *buttons[3];
	for (int i = 0; i < 3; i++) {
		buttons[i] = transparent(controls);
		lv_obj_set_size(buttons[i], i == 1 ? COMPACT_PLAY_BTN : SKIP_SIZE,
						i == 1 ? COMPACT_PLAY_BTN : SKIP_SIZE);
	}

	int info_h = LV_MAX(line_height * 2, FAV_HEIGHT + line_height);
	// Switch both directions repeatedly, including when no waveform is available.
	for (int pass = 0; pass < 6; pass++) {
		bool waveform = pass % 2 != 0;
		bool wave_available = pass != 5;
		int h = player_deck_compact_height(waveform, info_h, line_height);
		lv_obj_set_size(deck, SCREEN_W, h);
		lv_obj_align(deck, LV_ALIGN_BOTTOM_MID, 0, 0);
		lv_obj_set_style_pad_top(deck, waveform ? COMPACT_WAVE_DECK_PAD_TOP : COMPACT_DECK_PAD_TOP, 0);
		lv_obj_set_style_pad_bottom(deck, player_deck_bottom_padding(waveform), 0);
		lv_obj_set_style_pad_gap(deck, waveform ? COMPACT_WAVE_DECK_GAP : COMPACT_DECK_GAP, 0);
		player_deck_show_info(info, waveform);
		lv_obj_set_hidden(names, waveform);
		lv_obj_set_hidden(side, waveform);
		if (waveform && wave_available) {
			lv_obj_set_hidden(wave, false);
			lv_obj_move_to_index(wave, lv_obj_get_index(slider));
			lv_obj_set_parent(slider, wave);
			lv_obj_set_size(slider, lv_pct(100), COMPACT_WAVE_HEIGHT);
			lv_obj_center(slider);
		} else {
			if (lv_obj_get_parent(slider) == wave) {
				lv_obj_set_parent(slider, deck);
				lv_obj_move_to_index(slider, lv_obj_get_index(wave) + 1);
			}
			lv_obj_set_size(slider, lv_pct(100), PROGRESS_TRACK_HEIGHT);
			lv_obj_set_hidden(wave, true);
		}
		lv_obj_update_layout(screen);
		assert(lv_obj_is_hidden(info) == waveform);
		assert(top_y(controls) > bottom_y(clocks));
		if (waveform && wave_available) {
			assert(top_y(wave) == SCREEN_H - h + COMPACT_WAVE_DECK_PAD_TOP);
			assert(top_y(clocks) - bottom_y(wave) - 1 == COMPACT_WAVE_DECK_GAP);
		}
		assert(top_y(controls) - bottom_y(clocks) - 1 == (waveform ? COMPACT_WAVE_DECK_GAP : COMPACT_DECK_GAP));
		for (int i = 0; i < 3; i++) {
			assert(top_y(buttons[i]) >= top_y(controls));
			assert(bottom_y(buttons[i]) <= bottom_y(controls));
			assert(bottom_y(buttons[i]) <= SCREEN_H - 1 - player_deck_bottom_padding(waveform));
		}
		assert(bottom_y(repeat) <= SCREEN_H - 1 - player_deck_bottom_padding(waveform));
		assert(bottom_y(more) <= SCREEN_H - 1 - player_deck_bottom_padding(waveform));
	}
	lv_obj_delete(screen);
}

int main(void) {
	font_ui_14 = lv_font_montserrat_14;
	lv_init();
	lv_display_create(SCREEN_W, SCREEN_H);
	exercise(16);
	exercise(20);
	exercise(24);
	puts("PASS player deck (waveform and standard, three font sizes and waveform fallback)");
	return 0;
}
