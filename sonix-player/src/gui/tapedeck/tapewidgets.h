#ifndef TAPEWIDGETS_H
#define TAPEWIDGETS_H

#include "src/gui/board_profile.h"
#include "src/gui/fonts/fonts.h"
#include "src/gui/nowplaying/coverflow.h"
#include "src/gui/nowplaying/player.h"
#include "src/gui/shell/switcher.h"
#include "src/gui/shell/theme.h"

#include <stdint.h>
#include <string.h>

static inline bool clicked(lv_event_t *e) {
	return !lv_obj_has_state(lv_event_get_current_target(e), LV_STATE_DISABLED) &&
		!switcher_back_drag_active() && !player_sheet_drag_active() && !coverflow_drag_active();
}

static inline void label_text(lv_obj_t *label, const char *text) {
	if (strcmp(lv_label_get_text(label), text) != 0) lv_label_set_text(label, text);
}

static inline lv_obj_t *stack(lv_obj_t *parent, lv_flex_flow_t flow, int height, int gap, bool card) {
	lv_obj_t *obj = lv_obj_create(parent);
	lv_obj_remove_style_all(obj);
	if (card) lv_obj_add_style(obj, &theme_style_card, 0);
	lv_obj_set_size(obj, lv_pct(100), height);
	lv_obj_set_style_radius(obj, card ? bp_tile_radius() : 0, 0);
	lv_obj_set_style_pad_all(obj, card ? 8 : 0, 0);
	lv_obj_set_style_pad_gap(obj, gap, 0);
	lv_obj_set_scrollable(obj, false);
	lv_obj_set_event_bubble(obj, true);
	lv_obj_set_flex_flow(obj, flow);
	lv_obj_set_flex_align(obj, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	return obj;
}

static inline lv_obj_t *text_label(lv_obj_t *parent, const char *text, const lv_font_t *font, bool dim) {
	lv_obj_t *label = lv_label_create(parent);
	lv_obj_add_style(label, dim ? &theme_style_text_dim : &theme_style_text, 0);
	lv_obj_set_style_text_font(label, font, 0);
	lv_obj_set_event_bubble(label, true);
	lv_label_set_text(label, text);
	return label;
}

static inline lv_obj_t *note_label(lv_obj_t *parent, const char *text) {
	lv_obj_t *label = text_label(parent, text, &font_ui_14, true);
	lv_obj_set_width(label, lv_pct(100));
	return label;
}

// Each key grows into the available column; the caption cannot resize it.
static inline lv_obj_t *key(lv_obj_t *parent, const char *text, int height, lv_event_cb_t cb, intptr_t arg) {
	lv_obj_t *btn = lv_button_create(parent);
	lv_obj_remove_style_all(btn);
	lv_obj_add_style(btn, &theme_style_card, 0);
	lv_obj_add_style(btn, &theme_style_card_pressed, LV_STATE_PRESSED);
	lv_obj_add_style(btn, &theme_style_accent_bg, LV_STATE_CHECKED);
	lv_obj_set_style_opa(btn, LV_OPA_50, LV_STATE_DISABLED);
	lv_obj_set_size(btn, lv_pct(100), height);
	lv_obj_set_style_radius(btn, bp_tile_radius(), 0);
	lv_obj_set_event_bubble(btn, true);
	if (cb) lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, (void *)arg);
	lv_obj_t *label = lv_label_create(btn);
	lv_obj_set_style_text_font(label, &font_ui_16, 0);
	lv_obj_set_width(label, lv_pct(92));
	lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_set_height(label, lv_font_get_line_height(&font_ui_16));
	lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
	lv_label_set_text(label, text);
	lv_obj_center(label);
	return btn;
}

static inline lv_obj_t *row_key(lv_obj_t *row, const char *text, lv_event_cb_t cb, intptr_t arg) {
	lv_obj_t *btn = key(row, text, lv_pct(100), cb, arg);
	lv_obj_set_width(btn, 0);
	lv_obj_set_flex_grow(btn, 1);
	return btn;
}

static inline lv_obj_t *compact_key(lv_obj_t *row, const char *text, lv_event_cb_t cb, intptr_t arg) {
	lv_obj_t *btn = row_key(row, text, cb, arg);
	lv_obj_t *caption = lv_obj_get_child(btn, 0);
	lv_obj_set_style_text_font(caption, &font_ui_14, 0);
	lv_obj_set_width(caption, lv_pct(98));
	lv_obj_set_height(caption, lv_font_get_line_height(&font_ui_14));
	return btn;
}

static inline void disabled(lv_obj_t *obj, bool value) { lv_obj_set_state(obj, LV_STATE_DISABLED, value); }
static inline void selected(lv_obj_t *obj, bool value) { lv_obj_set_state(obj, LV_STATE_CHECKED, value); }

#endif
