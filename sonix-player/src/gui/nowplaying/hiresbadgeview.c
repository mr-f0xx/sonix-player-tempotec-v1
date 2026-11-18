#include "hiresbadgeview.h"

#include "src/gui/fonts/fonts.h"
#include "src/gui/shell/icons.h"

static int header_scale(int chevron_height, int design_height) {
	// Scale the logo to 27 px against the V1's 22 px chevron. The same
	// scale keeps the drawn badge and its reserved width in agreement.
	int target_height = chevron_height + 5;
	int natural_height = LV_MAX(icon_hires_audio.header.h, design_height);
	return LV_CLAMP(1, target_height * LV_SCALE_NONE / natural_height, LV_SCALE_NONE);
}

static int scaled_length(int length, int scale) {
	return (length * scale + LV_SCALE_NONE - 1) / LV_SCALE_NONE;
}

static void fit_badge(hiresbadge_view_t *view) {
	int height = lv_obj_get_height(view->box);
	int scale = view->header_height > 0 ? header_scale(view->header_height, height) : LV_SCALE_NONE;
	if (lv_obj_get_style_transform_scale_x(view->box, 0) != scale) {
		lv_obj_set_style_transform_scale(view->box, scale, 0);
	}
	// Scaling from the top edge avoids rounding both ends. Offset by
	// half the scaled height to centre the badge on the chevron.
	int dy = view->header_height > 0 ? -scaled_length(height, scale) / 2 : 0;
	if (lv_obj_get_style_translate_y(view->box, 0) != dy) lv_obj_set_style_translate_y(view->box, dy, 0);
}

static void size_changed_cb(lv_event_t *e) {
	fit_badge(lv_event_get_user_data(e));
}

void hiresbadge_build(hiresbadge_view_t *view, lv_obj_t *parent) {
	view->header_height = 0;
	view->box = lv_obj_create(parent);
	lv_obj_remove_style_all(view->box);
	lv_obj_set_size(view->box, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
	lv_obj_set_scrollable(view->box, false);
	lv_obj_set_clickable(view->box, false);
	lv_obj_add_event_cb(view->box, size_changed_cb, LV_EVENT_SIZE_CHANGED, view);

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

int hiresbadge_header_width(const hiresbadge_view_t *view, int chevron_height) {
	lv_obj_t *design = lv_obj_is_hidden(view->image) ? view->label : view->image;
	int width = lv_obj_calc_dynamic_width(design, LV_STYLE_WIDTH);
	int scale = header_scale(chevron_height, lv_obj_calc_dynamic_height(design, LV_STYLE_HEIGHT));
	return scaled_length(width, scale);
}

static void place(hiresbadge_view_t *view, lv_obj_t *parent, bool left, int inset, int y) {
	if (lv_obj_get_parent(view->box) != parent) lv_obj_set_parent(view->box, parent);
	lv_obj_align(view->box, left ? LV_ALIGN_TOP_LEFT : LV_ALIGN_TOP_RIGHT, left ? inset : -inset, y);
}

void hiresbadge_place_on_art(hiresbadge_view_t *view, lv_obj_t *art, bool left, int inset) {
	view->header_height = 0;
	fit_badge(view);
	place(view, art, left, inset, inset);
}

void hiresbadge_place_in_header(hiresbadge_view_t *view, lv_obj_t *sheet, bool left, int inset,
							   int centre_y, int chevron_height) {
	if (view->header_height != chevron_height) {
		view->header_height = chevron_height;
		fit_badge(view);
	}
	int pivot_x = left ? 0 : lv_pct(100);
	if (lv_obj_get_style_transform_pivot_x(view->box, 0) != pivot_x) {
		lv_obj_set_style_transform_pivot_x(view->box, pivot_x, 0);
	}
	// fit_badge() supplies the offset from the centre to the scaled top edge.
	place(view, sheet, left, inset, centre_y);
}
