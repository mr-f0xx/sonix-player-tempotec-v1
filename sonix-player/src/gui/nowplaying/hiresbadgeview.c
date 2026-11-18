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
	// A visibility change on a non-flex child does not invalidate its parent's
	// layout. Re-measure the content-sized box, including when switching to
	// the logo without changing any text.
	lv_obj_mark_layout_as_dirty(view->box);
	lv_obj_update_layout(view->box);
}

int hiresbadge_width(const hiresbadge_view_t *view) {
	lv_obj_t *design = lv_obj_is_hidden(view->image) ? view->label : view->image;
	return lv_obj_calc_dynamic_width(design, LV_STYLE_WIDTH);
}

static void place(hiresbadge_view_t *view, lv_obj_t *parent, bool left, int inset, int y, int dy) {
	if (lv_obj_get_parent(view->box) != parent) lv_obj_set_parent(view->box, parent);
	if (lv_obj_get_style_translate_y(view->box, 0) != dy) lv_obj_set_style_translate_y(view->box, dy, 0);
	lv_obj_align(view->box, left ? LV_ALIGN_TOP_LEFT : LV_ALIGN_TOP_RIGHT, left ? inset : -inset, y);
}

void hiresbadge_place_on_art(hiresbadge_view_t *view, lv_obj_t *art, bool left, int inset) {
	place(view, art, left, inset, inset, 0);
}

void hiresbadge_place_in_header(hiresbadge_view_t *view, lv_obj_t *sheet, int inset, int centre_y) {
	// A percentage translation follows the badge's own height, including when
	// the design or text size changes, without measuring a hidden object.
	place(view, sheet, false, inset, centre_y, lv_pct(-50));
}
