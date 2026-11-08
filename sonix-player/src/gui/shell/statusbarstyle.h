#ifndef STATUSBARSTYLE_H
#define STATUSBARSTYLE_H

#include "lvgl/lvgl.h"

// The player already produces a dark, blurred image for its controls. It needs
// most of its own opacity here: at 30% it was almost indistinguishable from
// the dark panel behind it. The panel still shows through for readability.
static inline void statusbarstyle_frost(lv_obj_t *bar, const lv_image_dsc_t *image) {
	if (image) {
		lv_obj_set_style_bg_image_src(bar, image, 0);
		lv_obj_set_style_bg_image_opa(bar, LV_OPA_80, 0);
	} else {
		lv_obj_remove_local_style_prop(bar, LV_STYLE_BG_IMAGE_SRC, 0);
		lv_obj_remove_local_style_prop(bar, LV_STYLE_BG_IMAGE_OPA, 0);
	}
}

// Keep the album hue but move the ink toward the theme's readable foreground.
static inline lv_color_t statusbarstyle_ink(lv_color_t art, lv_color_t foreground) {
	int luma = (foreground.red * 299 + foreground.green * 587 + foreground.blue * 114) / 1000;
	return lv_color_mix(art, luma >= 128 ? lv_color_white() : lv_color_black(), LV_OPA_30);
}

// A state-specific style wins over the status icons' ordinary local colours.
// Updates to Bluetooth codec, battery, etc. continue underneath, and clearing
// the state immediately reveals the latest normal colour again.
static inline void statusbarstyle_icon(lv_obj_t *icon, bool enabled, lv_color_t color) {
	if (!icon) return;
	if (enabled) {
		lv_obj_set_style_image_recolor(icon, color, LV_STATE_USER_1);
		lv_obj_set_style_image_recolor_opa(icon, LV_OPA_COVER, LV_STATE_USER_1);
	}
	lv_obj_set_state(icon, LV_STATE_USER_1, enabled);
}

// Battery level is a filled object rather than an image. Apply the same ink
// with a state-specific background so ordinary level updates keep running.
static inline void statusbarstyle_fill(lv_obj_t *fill, bool enabled, lv_color_t color) {
	if (!fill) return;
	if (enabled) lv_obj_set_style_bg_color(fill, color, LV_STATE_USER_1);
	lv_obj_set_state(fill, LV_STATE_USER_1, enabled);
}

#endif
