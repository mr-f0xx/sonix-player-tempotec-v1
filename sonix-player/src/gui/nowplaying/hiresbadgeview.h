#ifndef HIRESBADGEVIEW_H
#define HIRESBADGEVIEW_H

#include "lvgl/lvgl.h"

#include "src/gui/nowplaying/hiresbadge.h"

// The artwork badge as one movable, non-interactive box. A label draws the
// three text designs; an embedded full-colour image draws the official logo.
// Keeping both inside the same box lets the player move the badge between
// sleeves and corners without knowing which design is selected.
typedef struct {
	lv_obj_t *box;
	lv_obj_t *label;
	lv_obj_t *image;
	int header_height; // zero on artwork; otherwise the chevron's visible height
} hiresbadge_view_t;

// Builds a hidden badge under `parent`. The caller owns placement and shows
// the box only when the stream qualifies and the layout's setting is on.
void hiresbadge_build(hiresbadge_view_t *view, lv_obj_t *parent);

// Switches design in place. Safe to call while hidden; the box stays hidden
// until the player shows it. Invalid values fall back to the original look.
void hiresbadge_apply(hiresbadge_view_t *view, hiresbadge_style_t style);

// The active design's scaled width in the header, even while it is hidden.
int hiresbadge_header_width(const hiresbadge_view_t *view, int chevron_height);

// Move the same badge between a sleeve corner and the player header.
void hiresbadge_place_on_art(hiresbadge_view_t *view, lv_obj_t *art, bool left, int inset);
void hiresbadge_place_in_header(hiresbadge_view_t *view, lv_obj_t *sheet, bool left, int inset,
							   int centre_y, int chevron_height);

#endif /* HIRESBADGEVIEW_H */
