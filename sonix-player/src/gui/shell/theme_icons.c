#include "theme_icons.h"

#include "icons.h"
#include "icons_rose.h"
#include "theme.h"

// The lookup is one pass over the active set's table. The tables are generated
// (tools/themed_icons.py); a row names the themed bitmap and, where it has one,
// the shared symbol the drawing also stands in for (the corner sizes, the
// status-bar rungs, the standalone service marks).

static const theme_icon_row_t *active_map(size_t *count) {
	switch (theme_glyph_set()) {
	case THEME_GLYPH_SET_ROSE_PINE:
		*count = theme_rose_map_count;
		return theme_rose_map;
	case THEME_GLYPH_SET_LUCIDE:
	default:
		*count = 0;
		return NULL;
	}
}

bool theme_icons_active(void) {
	return theme_glyph_set() != THEME_GLYPH_SET_LUCIDE;
}

const lv_image_dsc_t *theme_icon_home_tile(int tile) {
	if (tile < 0 || tile >= 6 || !theme_icons_active()) {
		return NULL;
	}
	switch (theme_glyph_set()) {
	case THEME_GLYPH_SET_ROSE_PINE:
		return theme_rose_tiles[tile];
	case THEME_GLYPH_SET_LUCIDE:
	default:
		return NULL;
	}
}

static uint32_t scale_of(const lv_image_dsc_t *drawn, int target) {
	if (!drawn || target <= 0) {
		return LV_SCALE_NONE;
	}
	int side = LV_MAX((int)drawn->header.w, (int)drawn->header.h);
	if (side <= 0) {
		return LV_SCALE_NONE;
	}
	uint32_t scale = (uint32_t)((int64_t)LV_SCALE_NONE * target / side);
	return scale ? scale : 1;
}

uint32_t theme_icon_scale_to(const lv_image_dsc_t *drawn, int target) {
	return scale_of(drawn, target);
}

uint32_t theme_icon_scale_for(const lv_image_dsc_t *icon, int target) {
	return scale_of(theme_icon_at(icon, target), target);
}

const lv_image_dsc_t *theme_icon_at(const lv_image_dsc_t *shared, int target) {
	if (!shared) {
		return NULL;
	}
	size_t count = 0;
	const theme_icon_row_t *map = active_map(&count);
	if (!map) {
		return shared;
	}
	// Every row stands on the same 24 px grid as the shared bitmap it answers
	// for, so the caller's size, scale and recolour all still apply -- but a
	// set draws one shape at several sizes (the 22 px home tile and the 30 px
	// settings row both stand in for a 34 px shared bitmap; the 32 px row mark
	// and the 128 px cover for the one 128 px service symbol). The bitmap cut
	// closest to the size the caller draws at is the one that stays sharp:
	// resampling a 128 px mark down to 24 is a smear, and blowing 22 up to 40
	// is the same blur the shared set was avoided for.
	//
	// `target` is the pixels the caller intends to draw; pass 0 (or use
	// theme_icon(), which passes the shared bitmap's own size) when the
	// caller's size is the shared one's.
	if (target <= 0) {
		target = (int)LV_MAX(shared->header.w, shared->header.h);
	}
	const lv_image_dsc_t *best = NULL;
	int best_gap = 0;
	const lv_image_dsc_t *first = NULL;
	for (size_t i = 0; i < count; i++) {
		if (map[i].shared != shared || !map[i].themed) {
			continue;
		}
		const lv_image_dsc_t *candidate = map[i].themed;
		int side = (int)LV_MAX(candidate->header.w, candidate->header.h);
		if (!first) {
			first = candidate;
			best = candidate;
			best_gap = side > target ? side - target : target - side;
			continue;
		}
		int gap = side > target ? side - target : target - side;
		// A tie goes to the larger drawing: it is the one with pixels to give.
		if (gap < best_gap || (gap == best_gap && side > (int)LV_MAX(best->header.w, best->header.h))) {
			best = candidate;
			best_gap = gap;
		}
	}
	return best ? best : shared;
}

const lv_image_dsc_t *theme_icon(const lv_image_dsc_t *shared) {
	if (!shared) {
		return NULL;
	}
	if (!theme_icons_active()) {
		return shared;
	}
	return theme_icon_at(shared, 0);
}
