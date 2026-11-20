#ifndef ALBUMGRID_H
#define ALBUMGRID_H

// The album grid's geometry, as arithmetic and nothing else.
//
// The grid shows a fixed number of covers on one screen -- four on the V1's
// 240x320 panel, twelve on the wide boards -- and "fixed" is the point: a
// screenful of covers should be a screenful, with the last row's bottoms on
// the panel's bottom edge and not cut off by it. So the cover is the smaller
// of what the columns allow and what the rows allow, and everything else --
// the tile, the pitch, the canvas the grid scrolls on -- follows from it.
//
// It lives apart from medialist.c because it is worth testing on its own: the
// numbers below are the ones the panel is checked against, and a test can ask
// for the geometry of a 240 px screen without building a library, a screen or
// an LVGL instance.

#include <stdbool.h>

typedef struct {
	int columns; // tiles across
	int rows;    // tiles down, on one screenful
	int gap;     // between tiles, horizontally and vertically
	int pad;     // inside a tile, all round
	int caption; // the line of text under each cover

	int tile_w;  // one tile
	int tile_h;
	int cover;   // the picture inside it
	int pitch_x; // tile to the next column
	int pitch_y; // tile to the next row
} albumgrid_metrics_t;

// Columns for a panel: the V1's 240 px screen holds two and four tiles, a
// wider board six and twelve. Everything else is derived.
int albumgrid_columns(int screen_width);
int albumgrid_rows(int screen_width);

// Works out the geometry for a content area `width` x `height` (the list's
// viewport, inside its own padding) with `caption` being the line height of
// the font the names are drawn in.
void albumgrid_measure(int width, int height, int caption, albumgrid_metrics_t *out);

// How many tiles `count` albums need -- one each, so the count itself, but
// spelled out because the grid's canvas is measured in tiles and not in the
// library's rows -- and the canvas height they scroll on: whole rows of them,
// each `pitch_y` apart, the last one's bottom exactly on the canvas edge.
int albumgrid_tiles(int count, int columns);
int albumgrid_canvas_height(int tiles, const albumgrid_metrics_t *m);

// Which tile a list row belongs to, and which column inside it. Rows are
// numbered the way the library's own list numbers them, so a grid never needs
// a second numbering.
int albumgrid_tile_of(int row, int columns);
int albumgrid_column_of(int row, int columns);

// Where a tile sits on the canvas.
int albumgrid_tile_x(int tile, const albumgrid_metrics_t *m);
int albumgrid_tile_y(int tile, const albumgrid_metrics_t *m);

// The list row at the top of a canvas scrolled to `scroll`, snapped back to
// the first column of its row of tiles: turning the grid off should put the
// same album where the grid had it.
int albumgrid_row_at(int scroll, const albumgrid_metrics_t *m);

#endif /* ALBUMGRID_H */
