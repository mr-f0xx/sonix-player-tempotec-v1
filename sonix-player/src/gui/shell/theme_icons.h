#ifndef THEME_ICONS_H
#define THEME_ICONS_H

#include <stdbool.h>

#include "lvgl/lvgl.h"

// One row of a themed set's table (tools/themed_icons.py): the bitmap the set
// draws, and the shared symbol the same drawing also stands in for -- NULL
// when the themed bitmap is simply what its own shared name answers with.
typedef struct {
	const lv_image_dsc_t *themed;
	const lv_image_dsc_t *shared;
} theme_icon_row_t;

// The glyph a themed set draws in place of one of the shared icons.
//
// The shared set (icons.h) stays the base: every page asks for a glyph through
// this header and gets either the themed drawing or, when that set has not
// drawn this one, the shared bitmap it asked for. Pages therefore keep naming
// &icon_play and friends -- no branch at the call site, and a themed set never
// has to be complete to be usable.
//
// The returned bitmap has the same shape as the shared one (white ARGB8888,
// drawn on the same 24 px grid), so the caller's size, scale and
// lv_obj_set_style_image_recolor() all still apply and nothing moves on screen.
//
// It is *not* always rasterised at the same pixel size, though: a themed set
// draws each glyph at the size its one call site asks for (24 px rows, 22 px
// home tiles, 128 px service marks), while the shared bitmap it stands in for
// may be drawn at another (Radio's shared mark is 128 px, a row wants 24).
// A caller that scales by the glyph's own width therefore has to measure what
// it is about to draw, not what it asked for -- see theme_icon_scale_to().
const lv_image_dsc_t *theme_icon(const lv_image_dsc_t *shared);

// Which drawing answers for a shared symbol, when the set drew that shape more
// than once: the smallest drawing that is still at least as large as the shared
// bitmap. A caller that asks for the symbol and scales by the drawing's own
// width is then shrinking it, never enlarging it -- enlarging is what smears,
// and a 32 px cut shrunk to a tile's 22 px loses the antialiasing the 22 px cut
// was drawn with. Where the set drew the shape only smaller than the symbol
// (the Streaming tile has no 128 px art), the largest drawing answers instead:
// a bitmap blown up is still better than one that cannot fill the box.
//
// theme_icon_home_tile() is separate from this on purpose. A tile is drawn at
// its own size and answers with the drawing cut for that layout (22 px on V1),
// so the home screen never goes through the alias at all.
//
// The same lookup for a call site that knows the size it is about to draw.
//
// A set draws one shape at several sizes -- a 22 px home tile, a 30 px
// settings row, a 128 px service cover -- and all of them stand in for the one
// shared symbol. theme_icon() answers with the drawing cut at that symbol's
// own size; a caller that draws the glyph at another size wants the drawing
// cut nearest to *its* size instead, so the bitmap on screen is not the
// shared one resampled. `target` is the pixels the glyph will occupy on its
// long side; 0 means "the shared bitmap's own size".
const lv_image_dsc_t *theme_icon_at(const lv_image_dsc_t *shared, int target);

// The lv_image scale that draws `drawn` at `target` pixels on its long side.
// Returns LV_SCALE_NONE when the bitmap has no size to scale from.
//
// Pages that size a glyph by its native pixels (the lists, the status bar, the
// home tiles) hand in the pointer theme_icon() returned; the shared bitmap
// keeps the scale it always had, and a themed drawing -- already rasterised at
// the target -- comes out at exactly 256, with no resampling either way.
uint32_t theme_icon_scale_to(const lv_image_dsc_t *drawn, int target);

// The same scale for a call site that still holds the *shared* symbol: the
// active set's drawing is looked up first and measured instead. Sites that
// also call lv_image_set_src() with the raw symbol need no change -- the
// lookup happens behind their back (see the raw-set sites in the lists, which
// never name theme_icon()) -- so use this where only the scale is computed
// from the glyph's own pixels.
uint32_t theme_icon_scale_for(const lv_image_dsc_t *shared, int target);

// The home tile's function glyph in the active set's own art, drawn at the
// pixel size the labelled V1 layout uses (so it is sharp rather than a shared
// bitmap scaled down). NULL when the active theme has no set of its own, or
// when the tile is outside 0..5.
const lv_image_dsc_t *theme_icon_home_tile(int tile);

// True when the active theme draws its own glyphs (Rose Pine). Pages use it
// where the shared art needs preparing and the themed art does not -- for
// instance choosing a coloured illustration, which would waste the work.
bool theme_icons_active(void);

#endif // THEME_ICONS_H
