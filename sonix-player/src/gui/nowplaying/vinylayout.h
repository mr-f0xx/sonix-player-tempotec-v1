#ifndef VINYLLAYOUT_H
#define VINYLLAYOUT_H

#include "lvgl/lvgl.h"

/* Sleeve + record geometry, measured for the V1's 240x320 panel. The record
 * is deliberately static: a spinning full disc would redraw most of the page
 * on every frame. The deck keeps the title, artist, seek bar and transport;
 * its clocks are omitted here. The exposed record ends above the deck with
 * VINYL_GAP clear pixels: it never sits behind the names, seek bar or buttons.
 * Only the sleeve covers it, hiding the top of the disc as in the reference. */
#define VINYL_ART_BORDER 2
#define VINYL_ART_RADIUS 12
#define VINYL_TOP 10
#define VINYL_GAP 6
#define VINYL_SIDE_MARGIN 24
#define VINYL_RECORD_PERCENT 86
#define VINYL_RECESS_PERCENT 10
#define VINYL_PEEK_PERCENT (VINYL_RECORD_PERCENT / 2 - VINYL_RECESS_PERCENT)

/* The reference pressing mark: a thin rounded stroke leaning down-right.
 * Endpoints are percentages of the record diameter, measured from the supplied
 * 460x608 mockup (record ~346 px, stroke ~65 px at ~15 degrees from vertical).
 * Keeping them record-relative preserves the look on the V1 and concept view. */
#define VINYL_MARK_X1_PERCENT 56
#define VINYL_MARK_Y1_PERCENT 71
#define VINYL_MARK_DX_PERCENT 5
#define VINYL_MARK_DY_PERCENT 18
#define VINYL_MARK_ANGLE -150 /* tenths of a degree; negative leans down-right */

/* Three rows, without the clock row: info, seek bar, transport. */
#define VINYL_DECK_PAD_TOP 4
#define VINYL_DECK_PAD_BOTTOM 4
#define VINYL_DECK_GAP 4
#define VINYL_DECK_SLACK 2
#define VINYL_PLAY_BTN 44
#define VINYL_TRACK_H 10

static inline int vinyl_deck_height(int info_height) {
	if (info_height < 0)
		info_height = 0;
	return VINYL_DECK_PAD_TOP + info_height + VINYL_DECK_GAP + VINYL_TRACK_H + VINYL_DECK_GAP + VINYL_PLAY_BTN + VINYL_DECK_PAD_BOTTOM + VINYL_DECK_SLACK;
}

typedef struct {
	int art_x, art_y, art_side;
	int record_x, record_y, record_d;
	int record_cx, record_cy;
	int deck_y;
	int controls_y;
} vinyl_box_t;

/* All coordinates are relative to the player sheet. The artwork is decoded
 * at art_side - 2*VINYL_ART_BORDER, so the border never covers its pixels. */
vinyl_box_t vinyl_layout_box(int screen_w, int screen_h, int deck_h);

typedef struct {
	lv_obj_t *disc;
	lv_obj_t *grooves[3];
	lv_obj_t *label;
	lv_obj_t *shine;
} vinyl_record_t;

/* Build once, then move/paint on layout or theme changes. No timers, canvas,
 * image buffer or per-frame allocation. The sleeve is a later sibling and
 * hides the top of the record, including most of its centre label. The
 * pressing mark is a static rounded stroke in the exposed lower-right arc. */
void vinyl_record_build(vinyl_record_t *record, lv_obj_t *parent);
void vinyl_record_apply(vinyl_record_t *record, const vinyl_box_t *box, lv_color_t rim);

#endif /* VINYLLAYOUT_H */
