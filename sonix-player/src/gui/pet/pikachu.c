/*
 * The pet: a Pikachu, drawn on a 37 x 40 grid.
 *
 * Every letter below is one cell of the picture the pet was drawn from, copied
 * cell for cell -- the picture is the comment. It uses six colours and nothing
 * else, and the colours are the picture's own:
 *
 *     K  outline         Y  yellow          O  orange (shading, tongue)
 *     R  red (cheeks)    N  brown (ear tip, tail base)
 *     W  white (the glint in each eye)      _  nothing: the card shows through
 *
 * The gap between the tail and the body is nothing, not white: it is a hole in
 * the animal, and a white patch there would be a hole with a lid on it.
 *
 * LVGL cannot enlarge this on the fly without spoiling it: its image scaler
 * samples between cells (measured on this build: a 37 x 40 picture comes out
 * 109 x 117 with the edge cells lost and the others of uneven width). Pixel
 * art is exact or it is not art, so the page is given the picture already
 * enlarged by whole cells -- 2 px a cell for a hatchling, 3 px for the rest --
 * and blits it 1:1. The preprocessor does the enlarging, so the source stays
 * the picture and there is nothing to run before building: the 2x array is
 * 23 KB and the 3x one 52 KB of read-only data, no RAM.
 *
 * To change the picture, change the letters. The arrays below must be exactly
 * 37 letters by 40 rows or the build stops (a row is a macro of 37 arguments,
 * and the arrays' sizes are asserted).
 */

#include "pikachu.h"

#include <stdint.h>

// One pixel of LVGL's ARGB8888 is four bytes in the order blue, green, red,
// alpha, whatever the machine, so the bytes are written out rather than packed
// into a word.
#define PIKACHU_BGRA(hex) (hex) & 0xFF, ((hex) >> 8) & 0xFF, ((hex) >> 16) & 0xFF, 0xFF

#define PIKACHU_PX__ 0x00, 0x00, 0x00, 0x00
#define PIKACHU_PX_K PIKACHU_BGRA(PIKACHU_INK)
#define PIKACHU_PX_Y PIKACHU_BGRA(PIKACHU_YELLOW)
#define PIKACHU_PX_O PIKACHU_BGRA(PIKACHU_ORANGE)
#define PIKACHU_PX_R PIKACHU_BGRA(PIKACHU_RED)
#define PIKACHU_PX_N PIKACHU_BGRA(PIKACHU_BROWN)
#define PIKACHU_PX_W PIKACHU_BGRA(PIKACHU_WHITE)

// A cell, enlarged: the same pixel two or three times across.
#define PIKACHU_CELL2(c) PIKACHU_PX_##c, PIKACHU_PX_##c,
#define PIKACHU_CELL3(c) PIKACHU_PX_##c, PIKACHU_PX_##c, PIKACHU_PX_##c,

// The rows are the picture: one to a line, one letter to a cell. Formatting them
// would only spoil it.
// clang-format off
// A row of the picture, written out as the CELL macro it is given.
#define PIKACHU_ROW(CELL, a0, a1, a2, a3, a4, a5, a6, a7, a8, a9, \
		a10, a11, a12, a13, a14, a15, a16, a17, a18, a19, \
		a20, a21, a22, a23, a24, a25, a26, a27, a28, a29, \
		a30, a31, a32, a33, a34, a35, a36) \
	CELL(a0) CELL(a1) CELL(a2) CELL(a3) CELL(a4) CELL(a5) CELL(a6) CELL(a7) \
	CELL(a8) CELL(a9) CELL(a10) CELL(a11) CELL(a12) CELL(a13) CELL(a14) CELL(a15) \
	CELL(a16) CELL(a17) CELL(a18) CELL(a19) CELL(a20) CELL(a21) CELL(a22) CELL(a23) \
	CELL(a24) CELL(a25) CELL(a26) CELL(a27) CELL(a28) CELL(a29) CELL(a30) CELL(a31) \
	CELL(a32) CELL(a33) CELL(a34) CELL(a35) CELL(a36)

// ... and repeated down, so a cell is as tall as it is wide.
#define PIKACHU_ROW2(...) \
	PIKACHU_ROW(PIKACHU_CELL2, __VA_ARGS__) PIKACHU_ROW(PIKACHU_CELL2, __VA_ARGS__)
#define PIKACHU_ROW3(...) \
	PIKACHU_ROW(PIKACHU_CELL3, __VA_ARGS__) PIKACHU_ROW(PIKACHU_CELL3, __VA_ARGS__) \
	PIKACHU_ROW(PIKACHU_CELL3, __VA_ARGS__)

// ---------------------------------------------------------------------------
// The picture
// ---------------------------------------------------------------------------

#define PIKACHU_ART(ROW) \
	ROW(_,_,_,_,_,_,_,_,_,_,_,_,_,_,_,_,_,_,_,_,_,_,_,_,_,_,_,_,_,_,_,_,_,_,_,K,K) \
	ROW(_,_,_,_,_,_,_,_,_,_,_,_,_,_,_,_,_,_,_,_,_,_,_,_,_,_,_,_,_,_,_,_,_,K,K,K,K) \
	ROW(_,_,_,_,_,_,_,_,_,_,_,_,_,_,_,_,_,_,_,_,_,_,_,_,_,_,_,_,_,_,_,K,K,K,K,K,K) \
	ROW(_,_,_,_,_,_,_,_,_,_,_,_,_,_,_,_,_,_,_,_,_,_,_,_,_,_,_,_,_,_,K,Y,Y,Y,K,K,K) \
	ROW(_,_,_,K,K,K,K,K,K,_,_,_,_,_,_,_,_,_,_,_,_,_,_,_,_,_,_,_,_,K,Y,Y,Y,Y,K,K,_) \
	ROW(_,_,_,K,K,K,O,O,O,K,K,K,K,_,_,_,_,_,_,_,_,_,_,_,_,_,_,_,K,Y,Y,Y,Y,O,K,K,_) \
	ROW(_,_,_,_,_,K,K,N,Y,Y,Y,Y,O,K,K,K,K,_,K,K,K,K,K,K,K,_,K,K,Y,Y,Y,Y,O,O,K,_,_) \
	ROW(_,_,_,_,_,_,K,K,N,O,Y,Y,Y,Y,Y,Y,O,K,Y,Y,Y,Y,Y,Y,Y,K,O,Y,Y,Y,Y,O,O,K,_,_,_) \
	ROW(_,_,_,_,_,_,_,K,K,O,O,O,Y,Y,Y,Y,Y,Y,Y,Y,Y,Y,Y,Y,Y,Y,Y,Y,Y,O,O,O,K,_,_,_,_) \
	ROW(_,_,_,_,_,_,_,_,K,K,O,O,O,O,O,Y,Y,Y,Y,Y,Y,Y,Y,Y,Y,Y,Y,Y,O,K,O,K,_,_,_,_,_) \
	ROW(_,_,_,_,_,_,_,_,_,_,K,K,O,K,Y,Y,Y,Y,Y,Y,Y,Y,Y,Y,Y,Y,Y,Y,Y,O,K,_,_,_,_,_,_) \
	ROW(K,_,_,_,_,_,_,_,_,_,_,_,K,Y,Y,Y,Y,Y,Y,Y,Y,Y,Y,Y,Y,Y,Y,Y,Y,Y,K,_,_,_,_,_,_) \
	ROW(K,K,_,_,_,_,_,_,_,_,_,_,K,Y,Y,Y,K,K,Y,Y,Y,Y,Y,Y,Y,Y,K,K,Y,Y,K,_,_,_,_,_,_) \
	ROW(K,O,K,_,_,_,_,_,_,_,_,K,O,Y,Y,K,K,W,K,Y,Y,Y,Y,Y,Y,K,K,W,K,Y,O,K,_,_,_,_,_) \
	ROW(K,Y,O,K,_,_,_,_,_,_,_,K,Y,Y,Y,K,K,K,K,Y,Y,Y,Y,Y,Y,K,K,K,K,Y,Y,K,_,_,_,_,_) \
	ROW(K,Y,Y,O,K,_,_,_,_,_,_,K,Y,Y,Y,Y,K,K,Y,Y,Y,Y,Y,Y,Y,Y,K,K,Y,Y,Y,K,_,_,_,_,_) \
	ROW(K,Y,Y,Y,O,K,_,_,_,_,K,Y,Y,R,R,Y,Y,Y,Y,Y,Y,K,K,Y,Y,Y,Y,Y,Y,R,R,Y,K,_,_,_,_) \
	ROW(K,Y,Y,Y,Y,O,K,_,_,_,K,Y,R,R,R,R,Y,Y,Y,Y,Y,Y,Y,Y,Y,Y,Y,Y,R,R,R,R,K,_,_,_,_) \
	ROW(K,Y,Y,Y,Y,Y,O,K,_,_,K,Y,R,R,R,R,Y,Y,K,Y,Y,K,K,Y,Y,K,Y,Y,R,R,R,R,K,_,_,_,_) \
	ROW(K,Y,Y,Y,Y,Y,Y,O,K,_,K,Y,Y,R,R,Y,Y,Y,Y,K,K,Y,Y,K,K,Y,Y,Y,Y,R,R,Y,K,_,_,_,_) \
	ROW(K,Y,Y,Y,Y,Y,Y,Y,O,K,K,K,Y,Y,Y,Y,Y,Y,Y,Y,Y,Y,Y,Y,Y,Y,Y,Y,Y,Y,Y,K,_,_,_,_,_) \
	ROW(K,Y,Y,Y,Y,Y,Y,Y,Y,O,K,K,O,Y,Y,Y,Y,Y,Y,Y,Y,Y,Y,Y,Y,Y,Y,Y,Y,Y,O,K,_,_,_,_,_) \
	ROW(_,K,Y,Y,Y,Y,Y,Y,Y,Y,K,_,K,Y,Y,Y,Y,Y,Y,Y,Y,Y,Y,Y,Y,Y,Y,Y,Y,Y,K,_,_,_,_,_,_) \
	ROW(_,_,K,Y,Y,Y,Y,Y,Y,K,_,K,O,Y,Y,Y,Y,Y,Y,Y,O,O,O,O,Y,Y,Y,Y,Y,Y,O,K,_,_,_,_,_) \
	ROW(_,_,_,K,Y,Y,Y,Y,K,_,_,K,K,Y,Y,Y,Y,Y,Y,Y,Y,O,O,Y,Y,Y,Y,Y,Y,Y,Y,K,_,_,_,_,_) \
	ROW(_,_,K,Y,Y,Y,Y,K,_,_,_,K,Y,Y,Y,Y,Y,Y,K,Y,Y,Y,Y,Y,Y,K,Y,Y,Y,Y,Y,K,_,_,_,_,_) \
	ROW(_,_,K,Y,Y,Y,Y,Y,K,_,K,Y,Y,Y,K,Y,Y,Y,K,Y,Y,Y,Y,Y,Y,K,Y,Y,Y,K,Y,K,_,_,_,_,_) \
	ROW(_,_,_,K,Y,Y,Y,Y,Y,K,K,Y,Y,Y,K,Y,Y,Y,Y,K,Y,Y,Y,Y,K,Y,Y,Y,Y,K,Y,Y,K,_,_,_,_) \
	ROW(_,_,_,_,K,Y,Y,O,O,N,K,Y,Y,Y,O,K,Y,Y,Y,K,Y,Y,Y,Y,K,Y,Y,Y,K,O,Y,Y,K,_,_,_,_) \
	ROW(_,_,_,_,_,K,O,O,O,K,K,Y,Y,Y,O,K,Y,Y,Y,K,Y,Y,Y,Y,K,Y,Y,Y,K,O,Y,Y,K,_,_,_,_) \
	ROW(_,_,_,_,_,_,K,O,N,N,K,Y,Y,Y,Y,O,K,Y,Y,K,Y,Y,Y,Y,K,Y,Y,K,O,Y,Y,Y,K,_,_,_,_) \
	ROW(_,_,_,_,_,K,N,N,N,N,K,Y,Y,Y,Y,Y,O,K,K,Y,Y,Y,Y,Y,Y,K,K,O,Y,Y,Y,Y,K,_,_,_,_) \
	ROW(_,_,_,_,_,_,K,N,N,N,K,Y,Y,Y,Y,Y,Y,Y,Y,Y,Y,Y,Y,Y,Y,Y,Y,Y,Y,Y,Y,Y,K,_,_,_,_) \
	ROW(_,_,_,_,_,_,_,K,K,N,K,O,Y,Y,Y,Y,Y,Y,Y,Y,Y,Y,Y,Y,Y,Y,Y,Y,Y,Y,Y,Y,K,_,_,_,_) \
	ROW(_,_,_,_,_,_,_,_,_,K,K,K,Y,Y,Y,Y,Y,Y,Y,Y,Y,Y,Y,Y,Y,Y,Y,Y,Y,Y,Y,K,_,_,_,_,_) \
	ROW(_,_,_,_,_,_,_,_,_,_,_,K,O,Y,Y,Y,Y,Y,Y,Y,Y,Y,Y,Y,Y,Y,Y,Y,Y,Y,Y,K,_,_,_,_,_) \
	ROW(_,_,_,_,_,_,_,_,_,_,_,_,K,Y,Y,Y,Y,Y,Y,O,O,O,O,O,Y,Y,Y,Y,Y,Y,K,_,_,_,_,_,_) \
	ROW(_,_,_,_,_,_,_,_,_,_,_,_,_,K,O,Y,O,O,K,K,K,K,K,K,K,O,O,Y,O,K,_,_,_,_,_,_,_) \
	ROW(_,_,_,_,_,_,_,_,_,_,_,_,K,Y,Y,Y,K,K,K,_,_,_,_,_,_,K,K,Y,Y,Y,K,_,_,_,_,_,_) \
	ROW(_,_,_,_,_,_,_,_,_,_,_,_,K,K,K,K,K,_,_,_,_,_,_,_,_,_,K,K,K,K,K,_,_,_,_,_,_)
// clang-format on

static const uint8_t pikachu_2x_data[] __attribute__((aligned(4))) = {PIKACHU_ART(PIKACHU_ROW2)};
static const uint8_t pikachu_3x_data[] __attribute__((aligned(4))) = {PIKACHU_ART(PIKACHU_ROW3)};

_Static_assert(sizeof(pikachu_2x_data) == PIKACHU_COLS * 2 * PIKACHU_ROWS * 2 * 4, "the picture is 37 x 40 cells");
_Static_assert(sizeof(pikachu_3x_data) == PIKACHU_COLS * 3 * PIKACHU_ROWS * 3 * 4, "the picture is 37 x 40 cells");

const lv_image_dsc_t pikachu_2x = {
	.header = {
		.magic = LV_IMAGE_HEADER_MAGIC,
		.cf = LV_COLOR_FORMAT_ARGB8888,
		.w = PIKACHU_COLS * 2,
		.h = PIKACHU_ROWS * 2,
		.stride = PIKACHU_COLS * 2 * 4,
	},
	.data_size = sizeof(pikachu_2x_data),
	.data = pikachu_2x_data,
};

const lv_image_dsc_t pikachu_3x = {
	.header = {
		.magic = LV_IMAGE_HEADER_MAGIC,
		.cf = LV_COLOR_FORMAT_ARGB8888,
		.w = PIKACHU_COLS * 3,
		.h = PIKACHU_ROWS * 3,
		.stride = PIKACHU_COLS * 3 * 4,
	},
	.data_size = sizeof(pikachu_3x_data),
	.data = pikachu_3x_data,
};
