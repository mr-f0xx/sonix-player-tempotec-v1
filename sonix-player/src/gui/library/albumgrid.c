#include "albumgrid.h"

// The tile's own padding and the air between two of them. On the V1 the gap is
// 4 px each way, which is the same air the list rows have between them.
#define ALBUMGRID_GAP_COMPACT 4
#define ALBUMGRID_GAP_REGULAR 10
#define ALBUMGRID_PAD_COMPACT 4
#define ALBUMGRID_PAD_REGULAR 12

int albumgrid_columns(int screen_width) { return screen_width < 320 ? 2 : 4; }

int albumgrid_rows(int screen_width) {
	(void)screen_width; // two rows on every board: it is what "4 covers" means
	return 2;
}

// A cover would rather not go below this: on a panel too short to fit a
// screenful the grid gives up height before it gives up width, but it never
// gives up so much that the picture stops being a picture.
#define ALBUMGRID_COVER_MIN 72

static int gap_for(int screen_width) {
	return screen_width < 320 ? ALBUMGRID_GAP_COMPACT : ALBUMGRID_GAP_REGULAR;
}
static int pad_for(int screen_width) {
	return screen_width < 320 ? ALBUMGRID_PAD_COMPACT : ALBUMGRID_PAD_REGULAR;
}

void albumgrid_measure(int width, int height, int caption, albumgrid_metrics_t *out) {
	if (!out) {
		return;
	}
	if (width < 1) {
		width = 1;
	}
	if (height < 1) {
		height = 1;
	}
	if (caption < 1) {
		caption = 1;
	}

	// The panel decides the columns and the air; both are fixed by the board
	// rather than by the space, so a wider viewport gets wider tiles and not
	// more of them.
	int columns = albumgrid_columns(width);
	int rows = albumgrid_rows(width);
	int gap = gap_for(width);
	int pad = pad_for(width);

	// What the columns allow, and what the rows allow. The smaller wins, which
	// is what makes a screenful a screenful: with the height the binding
	// constraint the last row's tiles end exactly on the bottom edge.
	int by_width = (width - (columns - 1) * gap) / columns - 2 * pad;
	int rows_total = rows * (2 * pad + caption) + (rows - 1) * gap;
	int by_height = (height - rows_total) / rows;

	int cover = by_width < by_height ? by_width : by_height;
	if (cover < ALBUMGRID_COVER_MIN) {
		// Too little room for a screenful with a decent picture: fit the
		// columns and let the grid scroll a row of tiles instead.
		cover = by_width;
	}
	if (cover < 1) {
		cover = 1;
	}

	out->columns = columns;
	out->rows = rows;
	out->gap = gap;
	out->pad = pad;
	out->caption = caption;
	out->cover = cover;

	// The tile is the picture and the caption between its paddings, and the
	// pitch is the tile and the gap. The tile does not stretch to fill the
	// column: a screenful of covers is centred by the list's own padding
	// rather than by widening each picture past what the row of them allows.
	out->tile_w = cover + 2 * pad;
	out->tile_h = cover + caption + 2 * pad;
	out->pitch_x = out->tile_w + gap;
	out->pitch_y = out->tile_h + gap;
}

int albumgrid_tiles(int count, int columns) {
	if (count <= 0 || columns <= 0) {
		return 0;
	}
	return count; // one tile per album: the grid's canvas lays them out
}

int albumgrid_canvas_height(int tiles, const albumgrid_metrics_t *m) {
	if (!m || tiles <= 0) {
		return 0;
	}
	// `tiles` is a count of TILES, and an album grid's columns are columns of
	// tiles: one tile per cell, four cells on the V1's panel.
	int rows = (tiles + m->columns - 1) / m->columns;
	return rows * m->tile_h + (rows - 1) * m->gap;
}

int albumgrid_tile_of(int row, int columns) { return columns > 0 && row >= 0 ? row / columns : 0; }

int albumgrid_column_of(int row, int columns) { return columns > 0 && row >= 0 ? row % columns : 0; }

int albumgrid_tile_x(int tile, const albumgrid_metrics_t *m) {
	if (!m || tile < 0) {
		return 0;
	}
	return (tile % m->columns) * m->pitch_x;
}

int albumgrid_tile_y(int tile, const albumgrid_metrics_t *m) {
	if (!m || tile < 0) {
		return 0;
	}
	return (tile / m->columns) * m->pitch_y;
}

int albumgrid_row_at(int scroll, const albumgrid_metrics_t *m) {
	if (!m || m->pitch_y <= 0 || scroll <= 0) {
		return 0;
	}
	int row_of_tiles = scroll / m->pitch_y;
	return row_of_tiles * m->columns;
}
