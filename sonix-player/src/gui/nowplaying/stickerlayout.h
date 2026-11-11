#ifndef STICKERLAYOUT_H
#define STICKERLAYOUT_H

#include <stdbool.h>
#include "lvgl/lvgl.h"

/* Pop-art / sticker player layout, measured for the V1's 240x320 panel:
 * a double neon-framed sleeve with a centre spindle hole and four corner
 * badges (tilted yellow queue pill, tilted cyan format pill, pink shuffle
 * disc and yellow favourite heart disc), over two tilted tape strips
 * (hot-pink title and yellow artist), a cyan album line, an inline seek bar
 * between the two clocks, and a ringed play/pause button. */
#define STICKER_BG_HEX       0x080510
#define STICKER_PINK_HEX     0xff2b93
#define STICKER_YELLOW_HEX   0xffe600
#define STICKER_GOLD_HEX     0xf5d400
#define STICKER_CYAN_HEX     0x18e0f8
#define STICKER_TRACK_HEX    0x392950
#define STICKER_RING_HEX     0xffdff3
#define STICKER_INK_DARK_HEX 0x080510

#define STICKER_ART_BORDER   2
#define STICKER_ART_RADIUS   28
#define STICKER_FRAME_RADIUS 30
#define STICKER_FRAME_OUTSET 3
#define STICKER_SPINDLE_PCT  16
#define STICKER_CORNER_DISC  24
#define STICKER_ICON_SCALE   150

#define STICKER_FRAME_TILT   -60 /* tenths of a degree */
#define STICKER_QUEUE_TILT   -60
#define STICKER_FORMAT_TILT   75
#define STICKER_TITLE_TILT   -25
#define STICKER_ARTIST_TILT   20

#define STICKER_TOP_MIN      6
#define STICKER_ART_GAP      6
#define STICKER_SIDE_MARGIN  38

#define STICKER_TITLE_MARGIN  6
#define STICKER_ARTIST_MARGIN 26
#define STICKER_TITLE_PAD_V   3
#define STICKER_ARTIST_PAD_V  2
#define STICKER_STRIP_RADIUS  2
#define STICKER_STRIP_GAP     4
#define STICKER_ALBUM_GAP     3

#define STICKER_SEEK_PAD_H    8
#define STICKER_SEEK_SIDE     38
#define STICKER_SLIDER_H      5
#define STICKER_KNOB_PAD      3
#define STICKER_ROW_GAP       3
#define STICKER_PLAY_BTN      38
#define STICKER_PLAY_RING_PAD 4
#define STICKER_PAD_BOTTOM    3
#define STICKER_DECK_SLACK    1

typedef struct {
	int art_x, art_y, art_side;
	int frame_x, frame_y, frame_side;
	int spindle_d;
	int deck_y, deck_h;
	int title_x, title_y, title_w, title_h;
	int artist_x, artist_y, artist_w, artist_h;
	int album_y, album_h;
	int seek_y, seek_h, slider_w;
	int controls_y;
} sticker_box_t;

sticker_box_t sticker_layout_box(int screen_w, int screen_h, int title_line_h, int artist_line_h,
								 int album_line_h, int time_line_h);

typedef struct {
	lv_obj_t *tilted_cyan;
	lv_obj_t *tilted_pink;
	lv_obj_t *straight_cyan;
	lv_obj_t *spindle;
	lv_obj_t *queue_pill;
	lv_obj_t *format_pill;
	lv_obj_t *repeat_disc;
	lv_obj_t *fav_disc;
	lv_obj_t *title_strip;
	lv_obj_t *artist_strip;
	lv_obj_t *album_label;
	lv_obj_t *seek_row;
	lv_obj_t *play_ring;
} sticker_view_t;

void sticker_ground_apply(lv_obj_t *sheet, lv_obj_t *deck, lv_obj_t *backdrop);
void sticker_place_sleeve(lv_obj_t *panel, lv_obj_t *sleeve, const sticker_box_t *box, int screen_w);
void sticker_view_build(sticker_view_t *v, lv_obj_t *sheet, lv_obj_t *cover_panel,
						lv_obj_t *sleeve, lv_obj_t *play_btn);
void sticker_view_apply(sticker_view_t *v, const sticker_box_t *box, int screen_w);
void sticker_view_set_hidden(sticker_view_t *v, bool hidden);

#endif /* STICKERLAYOUT_H */
