// test_albumgrid.c -- the album grid's geometry, on its own.
//
// The arithmetic this pins is the whole reason the grid fits the V1's panel: a
// 240x320 screen has 248 px of list viewport and 228 px inside the list's
// padding, and four covers have to fill that -- two across, two down, the last
// row's bottom edge on the viewport's bottom edge rather than below it. The
// numbers here are the panel's, not a convention: change one and the cover
// that was exactly 102 px becomes one that is cut off.
//
// Plain C, no LVGL: the module is arithmetic.
#include <stdio.h>
#include <string.h>

#include "src/gui/library/albumgrid.h"

static int failures;

#define CHECK(cond)                                                                                                    \
	do {                                                                                                               \
		if (!(cond)) {                                                                                                 \
			fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                             \
			failures++;                                                                                                \
		}                                                                                                              \
	} while (0)

// The V1's numbers, as the list lays them out: 240 px screen, 6 px of padding
// either side of the list, 320 less the 72 px of status bar and title row.
#define V1_SCREEN 240
#define V1_LIST_WIDTH (V1_SCREEN - 2 * 6)
#define V1_LIST_HEIGHT 248
#define V1_CAPTION 20 // font_ui_14's line height, measured

static void test_v1_geometry(void) {
	albumgrid_metrics_t m;
	albumgrid_measure(V1_LIST_WIDTH, V1_LIST_HEIGHT, V1_CAPTION, &m);

	// Four covers, two across and two down.
	CHECK(m.columns == 2);
	CHECK(m.rows == 2);
	CHECK(m.gap == 4);
	CHECK(m.pad == 4);
	CHECK(m.caption == V1_CAPTION);

	// Two tiles and one 4 px gap fit the 228 px of list, and a 4 px padding
	// either side leaves just over a hundred for the cover -- but the height
	// has the last word: four covers stacked two by two, each with its line of
	// text, have 248 px to live in, and that is what the picture is measured
	// against. Which is the point of measuring at all.
	CHECK(m.tile_w == m.cover + 8);
	CHECK(m.tile_h == m.cover + V1_CAPTION + 8);
	CHECK(m.pitch_x * m.columns - m.gap <= V1_LIST_WIDTH);
	int by_width = (V1_LIST_WIDTH - m.gap) / 2 - 2 * m.pad;
	CHECK(m.cover > 90);
	CHECK(m.cover <= by_width);

	// The row pitch, and the screenful: two tiles and the gap between them.
	CHECK(m.pitch_x == m.tile_w + m.gap);
	CHECK(m.pitch_y == m.tile_h + m.gap);
	// Four albums are four tiles: two rows of two.
	CHECK(albumgrid_tiles(4, m.columns) == 4);
	int canvas = albumgrid_canvas_height(4, &m);
	CHECK(canvas == 2 * m.tile_h + m.gap);

	// A screenful fits, with air to spare and not a cut-off row: the last
	// tile's bottom is at or above the viewport's, the next row of tiles is
	// clear of it entirely, and the four of them fill the height to within a
	// few pixels rather than leaving a strip of nothing under them.
	CHECK(canvas <= V1_LIST_HEIGHT);
	CHECK(albumgrid_tile_y(3, &m) + m.tile_h <= V1_LIST_HEIGHT);
	CHECK(albumgrid_tile_y(4, &m) >= V1_LIST_HEIGHT);
	CHECK(V1_LIST_HEIGHT - canvas < 8);

	// Two columns: the second tile of a row starts one pitch along and shares
	// the first one's top.
	CHECK(albumgrid_tile_x(0, &m) == 0);
	CHECK(albumgrid_tile_x(1, &m) == m.pitch_x);
	CHECK(albumgrid_tile_x(2, &m) == 0);
	CHECK(albumgrid_tile_y(0, &m) == 0);
	CHECK(albumgrid_tile_y(1, &m) == 0);
	CHECK(albumgrid_tile_y(2, &m) == m.pitch_y);
	CHECK(albumgrid_tile_y(3, &m) == m.pitch_y);
}

// A screen whose height is the binding constraint: taller captions and a
// shorter viewport still have to end in whole tiles, with the row's bottom on
// the edge rather than past it.
static void test_height_bound(void) {
	albumgrid_metrics_t m;
	albumgrid_measure(V1_LIST_WIDTH, 240, 28, &m);
	int canvas = albumgrid_canvas_height(4, &m);
	CHECK(canvas <= 240);
	CHECK(albumgrid_tile_y(3, &m) + m.tile_h <= 240);

	// Which constraint bit: the cover is the height's answer, well below what
	// the columns alone would have allowed.
	int by_width = (V1_LIST_WIDTH - m.gap) / 2 - 2 * m.pad;
	CHECK(m.cover < by_width);
	CHECK(m.cover >= 72); // never smaller than a thumbnail
}

// The wide boards: six across, twelve a screen, the same rules.
static void test_wide_geometry(void) {
	albumgrid_metrics_t m;
	albumgrid_measure(468, 520, 24, &m);
	CHECK(m.columns == 4); // four across, two down: eight covers a screen
	CHECK(m.rows == 2);
	CHECK(m.gap == 10);
	CHECK(m.pad == 12);
	CHECK(m.cover > 0);
	int canvas = albumgrid_canvas_height(8, &m);
	CHECK(canvas <= 520);
	CHECK(albumgrid_tile_x(m.columns - 1, &m) + m.tile_w <= 468);

	// A wide panel that is too short for a screenful of covers takes it out of
	// the height and scrolls; it does not shrink the pictures into stamps.
	albumgrid_measure(468, 200, 24, &m);
	CHECK(m.cover >= 72);
	CHECK(albumgrid_tile_x(m.columns - 1, &m) + m.tile_w <= 468);
}

static void test_rows_and_tiles(void) {
	albumgrid_metrics_t m;
	albumgrid_measure(V1_LIST_WIDTH, V1_LIST_HEIGHT, V1_CAPTION, &m);

	// One tile per album, whatever the columns: the columns decide where the
	// tiles go, not how many there are.
	CHECK(albumgrid_tiles(0, 2) == 0);
	CHECK(albumgrid_tiles(1, 2) == 1);
	CHECK(albumgrid_tiles(4, 2) == 4);
	CHECK(albumgrid_tiles(5, 2) == 5);

	// An odd last row: the canvas still ends on the last row of tiles drawn.
	CHECK(albumgrid_canvas_height(3, &m) == 2 * m.tile_h + m.gap);
	CHECK(albumgrid_tile_of(0, 2) == 0);
	CHECK(albumgrid_tile_of(1, 2) == 0);
	CHECK(albumgrid_tile_of(2, 2) == 1);
	CHECK(albumgrid_column_of(0, 2) == 0);
	CHECK(albumgrid_column_of(1, 2) == 1);
	CHECK(albumgrid_column_of(3, 2) == 1);

	// A canvas scrolled to a row of tiles comes back as the first album of
	// that row, which is what keeps the same record on screen when the grid
	// is turned back into a list.
	CHECK(albumgrid_row_at(0, &m) == 0);
	CHECK(albumgrid_row_at(m.pitch_y - 1, &m) == 0);
	CHECK(albumgrid_row_at(m.pitch_y, &m) == 2);
	CHECK(albumgrid_row_at(2 * m.pitch_y + 3, &m) == 4);
}

int main(void) {
	test_v1_geometry();
	test_height_bound();
	test_wide_geometry();
	test_rows_and_tiles();

	if (failures) {
		fprintf(stderr, "%d check(s) failed\n", failures);
		return 1;
	}
	printf("albumgrid: four covers fill the V1's panel, two across and two down\n");
	return 0;
}
