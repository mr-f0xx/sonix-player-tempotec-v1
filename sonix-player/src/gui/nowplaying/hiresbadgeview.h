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
} hiresbadge_view_t;

// Builds a hidden badge under `parent`. The caller owns placement and shows
// the box only when the stream qualifies and the layout's setting is on.
void hiresbadge_build(hiresbadge_view_t *view, lv_obj_t *parent);

// Switches design in place. Safe to call while hidden; the box stays hidden
// until the player shows it. Invalid values fall back to the original look.
void hiresbadge_apply(hiresbadge_view_t *view, hiresbadge_style_t style);

#endif /* HIRESBADGEVIEW_H */
