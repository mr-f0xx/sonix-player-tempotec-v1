#include "playbutton.h"

#include <stdint.h>

#include "src/gui/shell/icons.h"
#include "src/gui/shell/theme.h"

// How much of the button's width the glyph's bitmap takes once there is a disc
// around it. The play and pause bitmaps are drawn for a button with nothing
// behind it, so at their own size the corners of the triangle would run into
// the rim of the disc; the original player's glyph was a little under half the
// width of its disc, and this is a touch more of a smaller one. Never above
// the bitmap's own size -- a larger button keeps the glyph as it was drawn.
#define GLYPH_PERCENT 55

// Contrast is decided on relative luminance. The scale runs 0..255*255, with
// the gamma taken as 2 instead of sRGB's piecewise curve -- close enough to
// choose between two colours, and it needs no libm. The weights are Rec. 709's,
// as 256ths.
#define LUMINANCE_ONE (255u * 255u)
// WCAG's 0.05 of flare, added to both sides before they are compared.
#define LUMINANCE_FLARE (LUMINANCE_ONE / 20u)

static uint32_t luminance(lv_color_t c) { return (54u * c.red * c.red + 183u * c.green * c.green + 19u * c.blue * c.blue) >> 8; }

// Whether two colours are at least `tenths`/10 : 1 apart. The glyph on its disc
// is a graphic that carries meaning -- nothing else about the button tells play
// from pause -- so it is held to a floor, not left to whatever the accent is.
static bool contrast_at_least(lv_color_t a, lv_color_t b, uint32_t tenths) {
	uint32_t la = luminance(a) + LUMINANCE_FLARE;
	uint32_t lb = luminance(b) + LUMINANCE_FLARE;
	return la >= lb ? 10u * la >= tenths * lb : 10u * lb >= tenths * la;
}

// The accent glyph on a light disc is the original look, and it stays for as
// long as the two can be told apart: 2.5:1. The original shipped its default
// blue on the light theme's grey disc at 2.8, so a floor of 3 would have
// changed the look of the one case everybody has; yellow, lime, green and
// Tokyo Night's violet are all under 2.4 on white and wash out.
#define GLYPH_ON_DISC_TENTHS 25
// White on a disc that had to become the accent: kept while it holds 3:1 (blue,
// pink, brown), and black when it does not -- which on such a disc is 7:1 or
// better, where the white that fails would be 2:1.
#define WHITE_INK_TENTHS 30

// The glyph's scale: what makes its bitmap GLYPH_PERCENT of the button.
static uint32_t glyph_scale(lv_obj_t *button) {
	int32_t side = lv_obj_get_style_width(button, LV_PART_MAIN);
	int32_t native = (int32_t)icon_play.header.w;
	if (!LV_COORD_IS_PX(side)) {
		return LV_SCALE_NONE; // not a size in pixels; nothing to work it out from
	}
	int32_t want = side * GLYPH_PERCENT / 100;
	return want > 0 && want < native ? (uint32_t)(LV_SCALE_NONE * want / native) : LV_SCALE_NONE;
}

void playbutton_paint(lv_obj_t *button, lv_obj_t *glyph, bool legacy, lv_color_t accent, bool white_disc) {
	lv_color_t ink = accent;

	if (legacy) {
		lv_color_t disc = white_disc ? lv_color_white() : theme()->surface_pressed;
		// The accent on that disc is the original look. Where it would not read
		// -- or where the palette draws everything in one hue and a white disc
		// would be a second -- the disc is the accent and the glyph is white on
		// it, or black where white would not read there either.
		if (theme_is_monochrome() || !contrast_at_least(disc, accent, GLYPH_ON_DISC_TENTHS)) {
			disc = accent;
			ink = contrast_at_least(disc, lv_color_white(), WHITE_INK_TENTHS) ? lv_color_white() : lv_color_black();
		}
		lv_obj_set_style_radius(button, LV_RADIUS_CIRCLE, 0);
		lv_obj_set_style_bg_color(button, disc, 0);
		lv_obj_set_style_bg_opa(button, LV_OPA_COVER, 0);
		lv_image_set_scale(glyph, glyph_scale(button));
	} else {
		lv_obj_set_style_bg_opa(button, LV_OPA_TRANSP, 0);
		lv_image_set_scale(glyph, LV_SCALE_NONE);
	}
	lv_obj_set_style_image_recolor(glyph, ink, 0);
	lv_obj_set_style_image_recolor_opa(glyph, LV_OPA_COVER, 0);
}
