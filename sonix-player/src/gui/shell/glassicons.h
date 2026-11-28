#ifndef GLASSICONS_H
#define GLASSICONS_H

#include "lvgl/lvgl.h"

// iOS-style app badges for the Liquid Glass theme.
//
// The interface's menu glyphs are plain monochrome masks. Liquid Glass dresses
// each one as an app icon: a rounded square (the iOS "squircle") filled with a
// semantic colour and a subtle top-to-bottom gradient, with the glyph centred
// on it in white. The result is a real ARGB8888 image, so it drops straight
// into an lv_image with no extra objects, no layout change and no halo -- the
// badge IS the picture.
//
// Badges are cached by (glyph, size, colour): the same icon asked for twice
// returns the same descriptor, and the cache is stable across accent changes
// because the badge colours are the fixed iOS system hues, not the accent.
// The caller never owns the returned descriptor.
const lv_image_dsc_t *glass_icon_badge(const lv_image_dsc_t *glyph, int size, lv_color_t color);

// Returns the cached badge for (glyph, size, colour) without rendering a new
// one, or NULL when it has not been created yet. Lets a caller identify which
// badge a widget is drawing without populating the cache.
const lv_image_dsc_t *glass_icon_badge_find(const lv_image_dsc_t *glyph, int size, lv_color_t color);

// If `maybe_badge` is a cached badge, returns the same badge re-rendered at
// `size` (crisp, not a scaled-up copy). Returns NULL when it is not a badge,
// so callers can fall back to ordinary image scaling.
const lv_image_dsc_t *glass_icon_badge_resized(const lv_image_dsc_t *maybe_badge, int size);

// Frees every cached badge. Only safe once no image widget still points at one
// -- i.e. after the pages have been restyled away from Liquid Glass. gridpage
// calls this at the end of a non-glass refresh, when every tile has already
// been handed its plain glyph back.
void glass_icons_clear(void);

#endif // GLASSICONS_H
