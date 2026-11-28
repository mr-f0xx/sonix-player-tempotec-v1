#include "glass.h"

#include "src/gui/shell/glassbg.h"
#include "src/gui/shell/glasswallpaper.h"
#include "src/gui/shell/gui.h"
#include "src/gui/shell/theme.h"

// The always-on wallpaper under everything: the supplied abstract-glass art,
// shown so the frosted panels have something to show through even with no
// track loaded. Static art, no allocation; hidden when the theme leaves glass.
//
// The background is deliberately CONSTANT: it never follows the playing
// track's artwork. The main menu (and every other page) sits on the same
// wallpaper whatever is listening, so the backdrop never shifts under the
// user. The Now Playing deck keeps its own artwork treatment separately.
static lv_obj_t *glass_ambient_img;

static bool glass_ready;

// The ambient ground under the wallpaper: the palette's page colour with a
// faint lift towards the top, so the glass sits on something with a little
// depth rather than a flat black. The wallpaper covers it edge to edge.
static void glass_base_paint(void) {
	lv_obj_t *bottom = lv_layer_bottom();
	if (!bottom) {
		return;
	}
	lv_color_t base = theme()->screen_bg;
	lv_color_t top = lv_color_mix(theme()->accent, base, LV_OPA_30);
	top = lv_color_mix(lv_color_white(), top, LV_OPA_10);
	lv_obj_set_style_bg_opa(bottom, LV_OPA_COVER, 0);
	lv_obj_set_style_bg_color(bottom, top, 0);
	lv_obj_set_style_bg_grad_color(bottom, base, 0);
	lv_obj_set_style_bg_grad_dir(bottom, LV_GRAD_DIR_VER, 0);
}

static void glass_ambient_free(void) {
	if (glass_ambient_img) {
		lv_image_set_src(glass_ambient_img, NULL);
		lv_obj_set_hidden(glass_ambient_img, true);
	}
}

// Shows the supplied abstract-glass wallpaper on the bottom layer, scaled to
// cover the panel. It is static art (no allocation), always present while glass
// is active so the frosted panels have something to show through even with no
// track. It does not change with the artwork.
static void glass_ambient_paint(void) {
	lv_obj_t *bottom = lv_layer_bottom();
	if (!bottom) {
		return;
	}
	int w = (int)lv_display_get_horizontal_resolution(lv_display_get_default());
	int h = (int)lv_display_get_vertical_resolution(lv_display_get_default());
	if (w <= 0 || h <= 0) {
		return;
	}
	if (!glass_ambient_img) {
		glass_ambient_img = lv_image_create(bottom);
		lv_obj_set_clickable(glass_ambient_img, false);
		lv_obj_move_to_index(glass_ambient_img, 0);
	}
	lv_obj_set_size(glass_ambient_img, w, h);
	lv_obj_align(glass_ambient_img, LV_ALIGN_TOP_LEFT, 0, 0);
	// The supplied art, or the user's own picture from the card (glassbg).
	const lv_image_dsc_t *src = glassbg_source();
	lv_image_set_src(glass_ambient_img, src);
	// Cover the panel: scale by the larger axis ratio so no edge is left bare.
	// A custom picture is already panel-sized, so this leaves it at 1:1.
	uint32_t sx = (uint32_t)(LV_SCALE_NONE * w / (int)src->header.w);
	uint32_t sy = (uint32_t)(LV_SCALE_NONE * h / (int)src->header.h);
	lv_image_set_scale(glass_ambient_img, sx > sy ? sx : sy);
	lv_image_set_inner_align(glass_ambient_img, LV_IMAGE_ALIGN_CENTER);
	// The dim the user chose (darker/lighter), black over the picture so the
	// background sits behind the clear glass and white labels stay legible.
	lv_obj_set_style_image_recolor(glass_ambient_img, lv_color_black(), 0);
	lv_obj_set_style_image_recolor_opa(glass_ambient_img, glassbg_dim_opa(), 0);
	lv_obj_set_hidden(glass_ambient_img, false);
}

// Brings the backdrop in line with the theme. Run on the UI thread from the
// theme-refresh list. The wallpaper is constant; only its presence follows the
// theme (hidden outside Liquid Glass).
static void glass_sync(void) {
	if (!glass_ready) {
		return;
	}
	if (!theme_is_liquid_glass()) {
		glass_ambient_free(); // hidden behind the opaque pages of every other theme
		return;
	}
	glassbg_sync(); // read a chosen picture pending from startup or a card change
	glass_base_paint();
	glass_ambient_paint();
}

void glass_init(void) {
	lv_obj_t *bottom = lv_layer_bottom();
	if (!bottom) {
		return;
	}
	glass_ready = true;
	theme_register_refresh(glass_sync);
	glass_sync();
}
