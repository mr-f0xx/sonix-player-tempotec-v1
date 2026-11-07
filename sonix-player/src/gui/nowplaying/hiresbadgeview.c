#include "hiresbadgeview.h"

#include "src/gui/fonts/fonts.h"
#include "src/gui/shell/icons.h"

void hiresbadge_build(hiresbadge_view_t *view, lv_obj_t *parent) {
	view->box = lv_obj_create(parent);
	lv_obj_remove_style_all(view->box);
	lv_obj_set_size(view->box, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
	lv_obj_set_scrollable(view->box, false);
	lv_obj_set_clickable(view->box, false);

	// Compact, high-contrast text like the reference, legible on any sleeve.
	// It is a label, not a control: gestures still reach the artwork below.
	view->label = lv_label_create(view->box);
	lv_obj_set_style_text_font(view->label, &font_ui_14, 0);
	lv_obj_set_style_text_color(view->label, lv_color_black(), 0);
	lv_obj_set_style_bg_color(view->label, lv_color_white(), 0);
	lv_obj_set_style_bg_opa(view->label, LV_OPA_COVER, 0);
	lv_obj_set_style_radius(view->label, 3, 0);
	lv_obj_set_style_pad_hor(view->label, 3, 0);
	lv_obj_set_style_pad_ver(view->label, 1, 0);
	lv_obj_set_clickable(view->label, false);

	// The supplied Hi-Res Audio mark keeps its own gold/black colours in every
	// theme. No recolor, border or white plate is added around the artwork.
	view->image = lv_image_create(view->box);
	lv_obj_remove_style_all(view->image);
	lv_image_set_src(view->image, &icon_hires_audio);
	lv_obj_set_clickable(view->image, false);

	hiresbadge_apply(view, HIRESBADGE_TWO_LINE);
	lv_obj_set_hidden(view->box, true);
}

void hiresbadge_apply(hiresbadge_view_t *view, hiresbadge_style_t style) {
	style = hiresbadge_style_from_int(style);
	const char *text = hiresbadge_text(style);
	lv_obj_set_hidden(view->label, text == NULL);
	lv_obj_set_hidden(view->image, text != NULL);
	if (text) {
		lv_label_set_text(view->label, text);
		lv_obj_set_style_text_line_space(view->label, style == HIRESBADGE_TWO_LINE ? -4 : 0, 0);
	}
	// The hidden child takes no space in the content-sized box, so a change
	// from the square logo to a narrow text mark re-aligns tightly to its corner.
	lv_obj_update_layout(view->box);
}
