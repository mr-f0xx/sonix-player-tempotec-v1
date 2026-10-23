/* The production widgets and layout, without any display/audio/font libraries. */
#include "../../lv_conf.h"

#undef LV_USE_FREETYPE
#define LV_USE_FREETYPE 0
#undef LV_USE_SDL
#define LV_USE_SDL 0
#undef LV_USE_LINUX_FBDEV
#define LV_USE_LINUX_FBDEV 0
#undef LV_USE_EVDEV
#define LV_USE_EVDEV 0

/* Supply deterministic font metrics instead of opening device font files.
 * Two sizes, so that a page which draws one part of itself in a larger font
 * than the rest -- the calculator's symbol keys -- can be told apart from one
 * that does not, and so that the layout is measured with both shapes.
 *
 * The others are the sizes the screensaver's strip is drawn at: title, artist
 * and clock, on the V1 and on the 480-wide panels. Its test then replaces their
 * line boxes with the ones the device's FreeType faces give, which is what
 * decides whether the clock clears the swipe-up sign (see test_screensaver.c). */
#undef LV_FONT_MONTSERRAT_14
#define LV_FONT_MONTSERRAT_14 1
#undef LV_FONT_MONTSERRAT_20
#define LV_FONT_MONTSERRAT_20 1
#undef LV_FONT_MONTSERRAT_16
#define LV_FONT_MONTSERRAT_16 1
#undef LV_FONT_MONTSERRAT_22
#define LV_FONT_MONTSERRAT_22 1
#undef LV_FONT_MONTSERRAT_28
#define LV_FONT_MONTSERRAT_28 1
#undef LV_FONT_MONTSERRAT_36
#define LV_FONT_MONTSERRAT_36 1
#undef LV_FONT_MONTSERRAT_48
#define LV_FONT_MONTSERRAT_48 1

/* Small, restrained labels in the live visualization preview fixture. */
#undef LV_FONT_MONTSERRAT_12
#define LV_FONT_MONTSERRAT_12 1
