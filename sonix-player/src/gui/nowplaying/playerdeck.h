#ifndef PLAYERDECK_H
#define PLAYERDECK_H

#include <stdbool.h>
#include "lvgl/lvgl.h"

// V1 deck rows and their insets. The Waveform layout has three rows: its
// waveform, clocks and transport. The info row must not take a flex gap.
#define COMPACT_DECK_PAD_TOP 4
#define COMPACT_DECK_PAD_BOTTOM 4
#define COMPACT_DECK_GAP 4
#define COMPACT_DECK_SLACK 2
#define COMPACT_WAVE_DECK_PAD_TOP 8
#define COMPACT_WAVE_DECK_PAD_BOTTOM 10
#define COMPACT_WAVE_DECK_GAP 7
#define COMPACT_WAVE_HEIGHT 36
#define COMPACT_PLAY_BTN 44
#define PROGRESS_TRACK_HEIGHT 10

static inline int player_deck_bottom_padding(bool waveform) {
	return waveform ? COMPACT_WAVE_DECK_PAD_BOTTOM : COMPACT_DECK_PAD_BOTTOM;
}

// info_height is the larger of the title/artist and favourite/format columns;
// clock_height is the larger of the clock and queue fonts' line heights.
static inline int player_deck_compact_height(bool waveform, int info_height, int clock_height) {
	int first = waveform ? COMPACT_WAVE_HEIGHT : info_height + COMPACT_DECK_GAP + PROGRESS_TRACK_HEIGHT;
	int top = waveform ? COMPACT_WAVE_DECK_PAD_TOP : COMPACT_DECK_PAD_TOP;
	int gap = waveform ? COMPACT_WAVE_DECK_GAP : COMPACT_DECK_GAP;
	return top + first + gap + clock_height + gap + COMPACT_PLAY_BTN + player_deck_bottom_padding(waveform) +
		   COMPACT_DECK_SLACK;
}

// Hiding just the two children leaves an empty flex item and its row gap.
static inline void player_deck_show_info(lv_obj_t *info_row, bool waveform) {
	if (info_row) lv_obj_set_hidden(info_row, waveform);
}

#endif
