#include "vaultpage.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lvgl/lvgl.h"

#include "src/gui/board_profile.h"
#include "src/gui/fonts/fonts.h"
#include "src/gui/shell/confirm.h"
#include "src/gui/shell/icons.h"
#include "src/gui/shell/keyboard.h"
#include "src/gui/shell/popover.h"
#include "src/gui/shell/settingsrow.h"
#include "src/gui/shell/switcher.h"
#include "src/gui/shell/theme.h"
#include "src/gui/shell/toast.h"
#include "src/system/core/lang.h"
#include "src/system/vault/vaultstore.h"

lv_obj_t *vault_screen;

// The two minutes of no typing after which an open vault puts itself away.
// A pocket player is put down mid-sentence; the entries stay encrypted on the
// card, and the keys should not stay in RAM for the rest of the day.
#define VAULT_IDLE_MS (2 * 60 * 1000)

// The pad's keys, measured from the panel rather than guessed: four rows of
// three below a heading, its hint and the PIN dots. On the V1 this lands at
// 73 x 36 a key, which is a finger target and leaves the whole page on the
// glass without a scroll.
#define PIN_KEY_GAP 4
#define PIN_PAD_TOP 90

// The bullet the masked field and the hidden list rows show. One is enough
// repeated: MiSans draws U+2022 as wide as an ideograph, so a long run of them
// would push a title off the row.
#define VAULT_MASK "\xE2\x80\xA2\xE2\x80\xA2\xE2\x80\xA2\xE2\x80\xA2\xE2\x80\xA2\xE2\x80\xA2\xE2\x80\xA2\xE2\x80\xA2"

typedef enum {
	VAULT_STATE_SETUP,	// no vault: choose the first PIN
	VAULT_STATE_CONFIRM,   // and type it again
	VAULT_STATE_LOCKED,	// a vault is there: unlock it
	VAULT_STATE_CHANGE,	// unlocked, choosing a new PIN
	VAULT_STATE_CHANGE_CONFIRM,
	VAULT_STATE_OPEN, // the list, in front of the person who unlocked it
} vault_state_t;

static gui_config_t *config;
static lv_obj_t *title_label;
static lv_obj_t *lock_view;
static lv_obj_t *lock_glyph;
static lv_obj_t *lock_heading;
static lv_obj_t *lock_hint;
static lv_obj_t *pin_dots;
static lv_obj_t *keypad;
static lv_obj_t *pin_check;
static lv_obj_t *pin_check_glyph;
static lv_obj_t *content_view;

// The list face.
static lv_obj_t *list_view;
static lv_obj_t *list_caption;
static lv_obj_t *list_count;
static lv_obj_t *list;

// The editor face. Two fields share one keyboard: tapping either points the
// keys at it, and only that field keeps a caret.
static lv_obj_t *editor_view;
static lv_obj_t *title_field;
static lv_obj_t *secret_field;
static lv_obj_t *secret_eye;
static lv_obj_t *secret_eye_glyph;
static lv_obj_t *title_caption;
static lv_obj_t *secret_caption;

// The two corner controls change meaning with the face: plus/menu on the list,
// check/trash in the editor. Same buttons, different glyphs.
static lv_obj_t *primary_button;
static lv_obj_t *primary_glyph;
static lv_obj_t *secondary_button;
static lv_obj_t *secondary_glyph;
static keyboard_t *keyboard;
static lv_timer_t *idle_timer;

static vault_state_t state;
static vault_session *session;
static bool editor_visible;
static bool dirty;
static int edit_index; // -1 while a new entry is being typed

// What has been typed into the pad, and the first copy while it is being
// confirmed or changed. Both are wiped the moment they are no longer needed.
static char pin_entry[VAULT_PIN_MAX + 1];
static size_t pin_len;
static char first_pin[VAULT_PIN_MAX + 1];

static char path[VAULT_PATH_MAX];
static int key_w;
static int key_h;

static void show_lock_state(void);
static void show_list_face(void);
static void show_editor_face(void);
static void rebuild_list(void);
static void update_corner_buttons(void);
static bool save_editor(bool tell);
static void save_editor_cb(lv_event_t *e);

// A string that is secret is overwritten before it is let go. `volatile` is
// what stops the compiler deciding the zeroes are never read.
static void wipe(void *data, size_t len) {
	volatile unsigned char *p = data;
	while (len--) {
		*p++ = 0;
	}
}

static void clear_pin_entry(void) {
	wipe(pin_entry, sizeof(pin_entry));
	pin_len = 0;
	if (pin_dots) {
		lv_label_set_text(pin_dots, "");
	}
}

static void clear_first_pin(void) {
	wipe(first_pin, sizeof(first_pin));
}

// The dots are the only thing on the page that says how much of the PIN has
// been typed. The PIN itself is never drawn, not even for a moment.
static void pin_dots_update(void) {
	if (!pin_dots) {
		return;
	}
	if (pin_len == 0) {
		lv_label_set_text(pin_dots, "\xE2\x80\xA2  \xE2\x80\xA2  \xE2\x80\xA2  \xE2\x80\xA2");
		lv_obj_set_style_text_color(pin_dots, theme()->text_secondary, 0);
		return;
	}
	char shown[VAULT_PIN_MAX * 4 + 1];
	size_t at = 0;
	for (size_t i = 0; i < pin_len && at + 4 < sizeof(shown); i++) {
		const char *dot = "\xE2\x80\xA2";
		memcpy(shown + at, dot, 3);
		at += 3;
		if (i + 1 < pin_len) {
			shown[at++] = ' ';
		}
	}
	shown[at] = '\0';
	lv_label_set_text(pin_dots, shown);
	lv_obj_set_style_text_color(pin_dots, theme()->accent, 0);
}

static const char *status_text(vault_status_t status, bool saving) {
	switch (status) {
	case VAULT_ERR_AUTH:
		return "vault_wrong_pin";
	case VAULT_ERR_PIN:
		return "vault_pin_hint";
	case VAULT_ERR_TOO_LONG:
		return "vault_too_long";
	case VAULT_ERR_FULL:
		return "vault_full";
	case VAULT_ERR_ARG:
		return "vault_entry_required";
	case VAULT_ERR_NOMEM:
	case VAULT_ERR_IO:
	case VAULT_ERR_RANDOM:
	case VAULT_ERR_FORMAT:
	case VAULT_ERR_NO_FILE:
	default:
		return saving ? "vault_cannot_save" : "vault_cannot_open";
	}
}

// A secret can be a seed phrase. The list row shows one line of it, so a very
// long entry is cut on a character boundary rather than fed whole to a label
// that is going to ellipsise it anyway.
static void list_preview(const char *src, char *out, size_t cap) {
	if (!src || cap < 4) {
		if (cap) {
			out[0] = '\0';
		}
		return;
	}
	size_t max = cap - 4;
	size_t full = strlen(src);
	size_t cut = 0;
	// A list row is one line. An old vault's text may have several, and a
	// newline in a label would make the row two lines tall inside a one-line
	// card; show the first line and an ellipsis for the rest.
	while (cut < max && cut < full && src[cut] != '\n' && src[cut] != '\r') {
		cut++;
	}
	if (cut == full) {
		memcpy(out, src, cut);
		out[cut] = '\0';
		return;
	}
	while (cut > 0 && ((unsigned char)src[cut] & 0xC0) == 0x80) {
		cut--;
	}
	memcpy(out, src, cut);
	memcpy(out + cut, "\xE2\x80\xA6", 4); // an ellipsis, and its NUL
}

// ---------------------------------------------------------------------------
// The entry list
// ---------------------------------------------------------------------------

static const lv_image_dsc_t *row_glyph(void) {
	return &icon_keylock_quick;
}

// The icon set is not one size: the menu glyphs are 140 px colour tiles, the
// small function glyphs 24 px line art. Scaling by a fraction of 24 draws the
// first group enormous -- which is exactly what the empty state looked like
// before this read the source. Ask the picture how big it is.
static void scale_icon(lv_obj_t *icon, int px) {
	const lv_image_dsc_t *source = lv_image_get_src(icon);
	int side = source && source->header.w ? (int)source->header.w : 24;
	if (side <= 0) {
		side = 24;
	}
	lv_image_set_scale(icon, (uint32_t)((int32_t)LV_SCALE_NONE * px / side));
}

static void row_eye_cb(lv_event_t *e) {
	lv_obj_t *button = lv_event_get_target(e);
	lv_obj_t *glyph = lv_obj_get_child(button, 0);
	int index = (int)(intptr_t)lv_obj_get_user_data(glyph);
	if (!session || index < 0 || (size_t)index >= vault_session_count(session)) {
		return;
	}
	// The eye shows the state, not the action: an open eye is a secret that
	// can be read. The button's own user data remembers which way it is.
	bool hidden = (bool)(intptr_t)lv_obj_get_user_data(button);
	hidden = !hidden;
	lv_obj_set_user_data(button, (void *)(intptr_t)hidden);
	lv_obj_t *row = lv_obj_get_parent(button);
	lv_obj_t *column = row ? lv_obj_get_child(row, 1) : NULL;
	lv_obj_t *secret_label = column ? lv_obj_get_child(column, 1) : NULL;
	if (!secret_label) {
		return;
	}
	if (hidden) {
		lv_label_set_text(secret_label, VAULT_MASK);
	} else {
		char preview[96];
		list_preview(vault_session_secret(session, index), preview, sizeof(preview));
		lv_label_set_text(secret_label, preview);
	}
	if (glyph) {
		lv_image_set_src(glyph, hidden ? &icon_eye_off : &icon_eye);
	}
	lv_timer_reset(idle_timer);
}

static void row_clicked_cb(lv_event_t *e) {
	int index = (int)(intptr_t)lv_event_get_user_data(e);
	if (!session || index < 0 || (size_t)index >= vault_session_count(session)) {
		return;
	}
	edit_index = index;
	show_editor_face();
	lv_timer_reset(idle_timer);
}

static lv_obj_t *make_entry_row(int index) {
	int row_h = bp_pick(56, 66);
	int badge_side = bp_pick(32, 38);
	const char *title = vault_session_title(session, (size_t)index);
	const char *secret = vault_session_secret(session, (size_t)index);

	lv_obj_t *row = lv_btn_create(list);
	lv_obj_set_size(row, lv_pct(100), row_h);
	lv_obj_add_style(row, &theme_style_card, 0);
	lv_obj_add_style(row, &theme_style_card_pressed, LV_STATE_PRESSED);
	lv_obj_set_style_radius(row, bp_pick(10, 14), 0);
	lv_obj_set_style_border_width(row, 0, 0);
	lv_obj_set_style_shadow_width(row, 0, 0);
	lv_obj_set_style_pad_all(row, bp_pick(8, 10), 0);
	lv_obj_set_style_pad_gap(row, bp_pick(8, 11), 0);
	lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_scrollable(row, false);
	lv_obj_add_event_cb(row, row_clicked_cb, LV_EVENT_CLICKED, (void *)(intptr_t)index);

	// A soft accent tile stands in for a website favicon: every entry uses the
	// same small key, but the colour ties the list to the page.
	lv_obj_t *badge = lv_obj_create(row);
	lv_obj_set_size(badge, badge_side, badge_side);
	lv_obj_set_style_bg_color(badge, theme()->accent, 0);
	lv_obj_set_style_bg_opa(badge, LV_OPA_20, 0);
	lv_obj_set_style_border_width(badge, 0, 0);
	lv_obj_set_style_radius(badge, bp_pick(8, 11), 0);
	lv_obj_set_style_pad_all(badge, 0, 0);
	lv_obj_set_scrollable(badge, false);
	lv_obj_t *glyph = lv_image_create(badge);
	lv_image_set_src(glyph, row_glyph());
	lv_obj_add_style(glyph, &theme_style_icon, 0);
	lv_obj_set_style_image_recolor(glyph, theme()->accent, 0);
	scale_icon(glyph, bp_pick(16, 19));
	lv_obj_center(glyph);

	lv_obj_t *column = lv_obj_create(row);
	lv_obj_set_height(column, lv_pct(100));
	lv_obj_set_flex_grow(column, 1);
	lv_obj_set_style_bg_opa(column, LV_OPA_TRANSP, 0);
	lv_obj_set_style_border_width(column, 0, 0);
	lv_obj_set_style_pad_all(column, 0, 0);
	lv_obj_set_style_pad_row(column, 1, 0);
	lv_obj_set_flex_flow(column, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(column, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
	lv_obj_set_scrollable(column, false);

	lv_obj_t *title_label_row = lv_label_create(column);
	lv_label_set_text(title_label_row, title);
	lv_obj_add_style(title_label_row, &theme_style_text, 0);
	lv_obj_set_style_text_font(title_label_row, &font_ui_18, 0);
	lv_obj_set_width(title_label_row, lv_pct(100));
	lv_label_set_long_mode(title_label_row, LV_LABEL_LONG_DOT);

	char preview[96];
	list_preview(secret, preview, sizeof(preview));
	lv_obj_t *secret_label = lv_label_create(column);
	lv_label_set_text(secret_label, preview);
	lv_obj_add_style(secret_label, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(secret_label, &font_ui_14, 0);
	lv_obj_set_width(secret_label, lv_pct(100));
	lv_label_set_long_mode(secret_label, LV_LABEL_LONG_DOT);

	lv_obj_t *eye = lv_btn_create(row);
	lv_obj_set_size(eye, bp_pick(30, 36), bp_pick(30, 36));
	lv_obj_add_style(eye, &theme_style_card_pressed, LV_STATE_PRESSED);
	lv_obj_set_style_bg_opa(eye, LV_OPA_TRANSP, 0);
	lv_obj_set_style_border_width(eye, 0, 0);
	lv_obj_set_style_shadow_width(eye, 0, 0);
	lv_obj_set_style_pad_all(eye, 0, 0);
	lv_obj_set_style_radius(eye, LV_RADIUS_CIRCLE, 0);
	lv_obj_add_event_cb(eye, row_eye_cb, LV_EVENT_CLICKED, NULL);
	lv_obj_t *eye_glyph = lv_image_create(eye);
	lv_image_set_src(eye_glyph, &icon_eye);
	lv_obj_add_style(eye_glyph, &theme_style_icon, 0);
	scale_icon(eye_glyph, bp_pick(17, 20));
	lv_obj_center(eye_glyph);
	lv_obj_set_user_data(eye_glyph, (void *)(intptr_t)index);

	return row;
}

static void new_entry_cb(lv_event_t *e) {
	(void)e;
	edit_index = -1;
	show_editor_face();
	lv_timer_reset(idle_timer);
}

static lv_obj_t *make_new_row(void) {
	lv_obj_t *row = lv_btn_create(list);
	lv_obj_set_size(row, lv_pct(100), bp_pick(44, 52));
	lv_obj_add_style(row, &theme_style_card, 0);
	lv_obj_add_style(row, &theme_style_card_pressed, LV_STATE_PRESSED);
	lv_obj_set_style_radius(row, bp_pick(10, 14), 0);
	lv_obj_set_style_border_width(row, 1, 0);
	lv_obj_set_style_border_color(row, theme()->accent, 0);
	lv_obj_set_style_border_opa(row, LV_OPA_40, 0);
	lv_obj_set_style_shadow_width(row, 0, 0);
	lv_obj_set_style_pad_all(row, bp_pick(7, 9), 0);
	lv_obj_set_style_pad_gap(row, bp_pick(8, 11), 0);
	lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_scrollable(row, false);
	lv_obj_add_event_cb(row, new_entry_cb, LV_EVENT_CLICKED, NULL);

	lv_obj_t *badge = lv_obj_create(row);
	lv_obj_set_size(badge, bp_pick(28, 32), bp_pick(28, 32));
	lv_obj_set_style_bg_color(badge, theme()->accent, 0);
	lv_obj_set_style_bg_opa(badge, LV_OPA_COVER, 0);
	lv_obj_set_style_border_width(badge, 0, 0);
	lv_obj_set_style_radius(badge, LV_RADIUS_CIRCLE, 0);
	lv_obj_set_style_pad_all(badge, 0, 0);
	lv_obj_set_scrollable(badge, false);
	lv_obj_t *glyph = lv_image_create(badge);
	lv_image_set_src(glyph, &icon_plus);
	lv_obj_add_style(glyph, &theme_style_icon, 0);
	lv_obj_set_style_image_recolor(glyph, theme_on_accent(), 0);
	scale_icon(glyph, bp_pick(14, 16));
	lv_obj_center(glyph);

	lv_obj_t *label = lv_label_create(row);
	lv_label_set_text(label, tr("vault_new_entry"));
	lv_obj_set_style_text_font(label, &font_ui_16, 0);
	lv_obj_set_style_text_color(label, theme()->accent, 0);
	lv_obj_set_flex_grow(label, 1);

	return row;
}

static void rebuild_list(void) {
	if (!list) {
		return;
	}
	lv_obj_clean(list);
	if (!session) {
		return;
	}
	size_t count = vault_session_count(session);
	lv_label_set_text_fmt(list_count, "%d", (int)count);

	lv_obj_t *new_row = make_new_row();
	(void)new_row;

	for (size_t i = 0; i < count; i++) {
		make_entry_row((int)i);
	}

	if (count == 0) {
		lv_obj_t *empty = lv_obj_create(list);
		lv_obj_set_width(empty, lv_pct(100));
		lv_obj_set_flex_grow(empty, 1);
		lv_obj_set_style_bg_opa(empty, LV_OPA_TRANSP, 0);
		lv_obj_set_style_border_width(empty, 1, 0);
		lv_obj_set_style_border_color(empty, theme()->text_secondary, 0);
		lv_obj_set_style_border_opa(empty, LV_OPA_30, 0);
		lv_obj_set_style_radius(empty, bp_pick(10, 14), 0);
		lv_obj_set_style_pad_all(empty, 8, 0);
		lv_obj_set_style_pad_row(empty, 4, 0);
		lv_obj_set_flex_flow(empty, LV_FLEX_FLOW_COLUMN);
		lv_obj_set_flex_align(empty, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
		lv_obj_set_scrollable(empty, false);

		lv_obj_t *glyph = lv_image_create(empty);
		lv_image_set_src(glyph, &icon_keylock_quick);
		lv_obj_add_style(glyph, &theme_style_icon, 0);
		lv_obj_set_style_image_recolor(glyph, theme()->accent, 0);
		scale_icon(glyph, bp_pick(30, 36));

		lv_obj_t *line = lv_label_create(empty);
		lv_label_set_text(line, tr("vault_empty"));
		lv_obj_add_style(line, &theme_style_text, 0);
		lv_obj_set_style_text_font(line, &font_ui_16, 0);
		lv_obj_set_style_text_align(line, LV_TEXT_ALIGN_CENTER, 0);
		lv_obj_set_width(line, lv_pct(100));
		lv_label_set_long_mode(line, LV_LABEL_LONG_WRAP);

		lv_obj_t *hint = lv_label_create(empty);
		lv_label_set_text(hint, tr("vault_empty_hint"));
		lv_obj_add_style(hint, &theme_style_text_dim, 0);
		lv_obj_set_style_text_font(hint, &font_ui_14, 0);
		lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, 0);
		lv_obj_set_width(hint, lv_pct(100));
		lv_label_set_long_mode(hint, LV_LABEL_LONG_WRAP);
	}
}

// ---------------------------------------------------------------------------
// The editor
// ---------------------------------------------------------------------------

static void clear_editor_fields(void) {
	if (title_field) {
		lv_textarea_set_text(title_field, "");
	}
	if (secret_field) {
		lv_textarea_set_text(secret_field, "");
	}
	dirty = false;
}

static void focus_field(lv_obj_t *field) {
	if (!keyboard || !field || !editor_visible) {
		return;
	}
	keyboard_set_field(keyboard, field);
	keyboard_show_caret(title_field, field == title_field);
	keyboard_show_caret(secret_field, field == secret_field);
	if (field == title_field) {
		lv_obj_add_state(title_field, LV_STATE_FOCUSED);
		lv_obj_remove_state(secret_field, LV_STATE_FOCUSED);
	} else {
		lv_obj_add_state(secret_field, LV_STATE_FOCUSED);
		lv_obj_remove_state(title_field, LV_STATE_FOCUSED);
	}
	keyboard_set_visible(keyboard, true);
	lv_timer_reset(idle_timer);
}

static void field_clicked_cb(lv_event_t *e) {
	focus_field(lv_event_get_target(e));
}

static void field_changed_cb(lv_event_t *e) {
	(void)e;
	if (editor_visible) {
		dirty = true;
		lv_timer_reset(idle_timer);
	}
}

static void secret_eye_cb(lv_event_t *e) {
	lv_obj_t *button = lv_event_get_target(e);
	bool reveal = (bool)(intptr_t)lv_obj_get_user_data(button);
	reveal = !reveal;
	lv_obj_set_user_data(button, (void *)(intptr_t)reveal);
	lv_textarea_set_password_mode(secret_field, !reveal);
	lv_obj_set_style_text_letter_space(secret_field, 0, 0);
	// The eye shows the state: open when the password can be read, struck
	// through when it is masked. Both this field and the list's rows use it
	// the same way.
	lv_image_set_src(secret_eye_glyph, reveal ? &icon_eye : &icon_eye_off);
	lv_timer_reset(idle_timer);
}

static void set_editor_values(const char *title, const char *secret) {
	clear_editor_fields();
	// A normal password is one line. A version-1 entry (the old single text
	// block) may carry newlines, so that one is opened as a multiline field:
	// otherwise the lines after the first would be invisible while editing.
	lv_textarea_set_one_line(secret_field, !secret || !strpbrk(secret, "\r\n"));
	lv_textarea_set_text(title_field, title ? title : "");
	lv_textarea_set_text(secret_field, secret ? secret : "");
	lv_textarea_set_cursor_pos(title_field, 0);
	lv_textarea_set_cursor_pos(secret_field, 0);
	dirty = false;
}

static void show_editor_face(void) {
	if (!session) {
		return;
	}
	state = VAULT_STATE_OPEN;
	editor_visible = true;
	lv_obj_set_hidden(lock_view, true);
	lv_obj_set_hidden(content_view, false);
	lv_obj_set_hidden(list_view, true);
	lv_obj_set_hidden(editor_view, false);

	if (edit_index >= 0 && (size_t)edit_index < vault_session_count(session)) {
		set_editor_values(vault_session_title(session, (size_t)edit_index),
						  vault_session_secret(session, (size_t)edit_index));
	} else {
		edit_index = -1;
		set_editor_values("", "");
	}

	// Every entry opens with its password masked, and the eye reset. The mode
	// is a property of the one widget, so an entry opened after a revealed one
	// would otherwise be revealed from the first frame.
	lv_textarea_set_password_mode(secret_field, true);
	lv_obj_set_style_text_letter_space(secret_field, 0, 0);
	lv_obj_set_user_data(secret_eye, (void *)(intptr_t)0);
	lv_image_set_src(secret_eye_glyph, &icon_eye_off);

	// New entries start in the title; an existing one also opens there, with
	// the password one tap below. Showing the keyboard saves a step and makes
	// the editor feel like a sheet rather than a read-only page.
	keyboard_set_visible(keyboard, true);
	keyboard_reset(keyboard);
	focus_field(title_field);
	update_corner_buttons();
	lv_timer_reset(idle_timer);
	lv_timer_resume(idle_timer);
}

static void show_list_face(void) {
	state = VAULT_STATE_OPEN;
	editor_visible = false;
	lv_obj_set_hidden(lock_view, true);
	lv_obj_set_hidden(content_view, false);
	lv_obj_set_hidden(editor_view, true);
	lv_obj_set_hidden(list_view, false);
	// The editor's buffers are not left holding a password behind a hidden
	// page: whatever is going to stay is on the card, and the fields are wiped
	// as soon as the list is back.
	clear_editor_fields();
	if (keyboard) {
		keyboard_set_visible(keyboard, false);
	}
	update_corner_buttons();
	rebuild_list();
	lv_timer_reset(idle_timer);
	lv_timer_resume(idle_timer);
}

// What is in the fields, compared with the entry being edited. A textarea set
// from code does not always send VALUE_CHANGED, so the save decision cannot
// rely on the event flag alone: the test harness and a restored field both
// write text without pretending to type it.
static bool editor_changed(void) {
	const char *title = lv_textarea_get_text(title_field);
	const char *secret = lv_textarea_get_text(secret_field);
	if (edit_index < 0) {
		return (title && title[0]) || (secret && secret[0]);
	}
	return strcmp(title ? title : "", vault_session_title(session, (size_t)edit_index)) != 0 ||
		   strcmp(secret ? secret : "", vault_session_secret(session, (size_t)edit_index)) != 0;
}

static bool save_editor(bool tell) {
	if (!session || !editor_visible) {
		return false;
	}
	const char *title = lv_textarea_get_text(title_field);
	const char *secret = lv_textarea_get_text(secret_field);
	if (!title || !title[0] || !secret || !secret[0]) {
		if (tell) {
			toast_error(tr("vault_entry_required"));
		}
		return false;
	}

	if (!editor_changed()) {
		dirty = false;
		show_list_face();
		return true;
	}

	// Keep the old strings so a card that refuses the write leaves the entry
	// exactly as it was, not half-updated in RAM.
	char *old_title = NULL;
	char *old_secret = NULL;
	if (edit_index >= 0) {
		old_title = strdup(vault_session_title(session, (size_t)edit_index));
		old_secret = strdup(vault_session_secret(session, (size_t)edit_index));
		if (!old_title || !old_secret) {
			free(old_title);
			free(old_secret);
			if (tell) {
				toast_error(tr("vault_cannot_save"));
			}
			return false;
		}
	}

	vault_status_t status;
	if (edit_index < 0) {
		size_t added = 0;
		status = vault_session_add(session, title, secret, &added);
		if (status == VAULT_OK) {
			status = vault_session_save(session);
			if (status != VAULT_OK) {
				vault_session_remove(session, added);
			} else {
				edit_index = (int)added;
			}
		}
	} else {
		status = vault_session_update(session, (size_t)edit_index, title, secret);
		if (status == VAULT_OK) {
			status = vault_session_save(session);
			if (status != VAULT_OK) {
				vault_session_update(session, (size_t)edit_index, old_title, old_secret);
			}
		}
	}
	free(old_title);
	free(old_secret);

	if (status != VAULT_OK) {
		if (tell) {
			toast_error(tr(status_text(status, true)));
		}
		return false;
	}

	dirty = false;
	show_list_face();
	if (tell) {
		toast_plain(tr("vault_entry_saved"));
	}
	lv_timer_reset(idle_timer);
	return true;
}

static void save_editor_cb(lv_event_t *e) {
	(void)e;
	save_editor(true);
}

static void primary_cb(lv_event_t *e) {
	(void)e;
	if (state != VAULT_STATE_OPEN || !session) {
		return;
	}
	if (editor_visible) {
		save_editor(true);
	} else {
		edit_index = -1;
		show_editor_face();
	}
}

static void delete_done_cb(void *user) {
	(void)user;
	if (!session || edit_index < 0 || (size_t)edit_index >= vault_session_count(session)) {
		return;
	}

	// Removal only happens in RAM until the save succeeds. If the card cannot
	// take the new table, the entry is put back rather than left in a state
	// the file does not agree with.
	char *title = strdup(vault_session_title(session, (size_t)edit_index));
	char *secret = strdup(vault_session_secret(session, (size_t)edit_index));
	if (!title || !secret) {
		free(title);
		free(secret);
		toast_error(tr("vault_cannot_save"));
		return;
	}
	uint32_t index = (uint32_t)edit_index;
	vault_status_t status = vault_session_remove(session, (size_t)index);
	if (status == VAULT_OK) {
		status = vault_session_save(session);
	}
	if (status != VAULT_OK) {
		size_t restored = 0;
		vault_session_add(session, title, secret, &restored);
		// The entry has moved to the end; the editor must follow it, or the
		// next check would update whichever entry now holds the old index.
		edit_index = (int)restored;
		free(title);
		free(secret);
		toast_error(tr("vault_cannot_save"));
		return;
	}
	free(title);
	free(secret);
	edit_index = -1;
	dirty = false;
	show_list_face();
	toast_plain(tr("vault_entry_deleted"));
	lv_timer_reset(idle_timer);
}

static void delete_entry_cb(lv_event_t *e) {
	(void)e;
	if (!session || edit_index < 0) {
		return;
	}
	confirm_show_with_cancel(tr("vault_delete_title"), tr("vault_delete_note"), tr("vault_delete"),
							 delete_done_cb, NULL, NULL, NULL, NULL);
}

static bool editor_can_leave(void) {
	const char *title = lv_textarea_get_text(title_field);
	const char *secret = lv_textarea_get_text(secret_field);
	bool blank = (!title || !title[0]) && (!secret || !secret[0]);
	if (edit_index < 0 && blank) {
		dirty = false;
		show_list_face();
		return true;
	}
	if (!editor_changed()) {
		show_list_face();
		return true;
	}
	return save_editor(true);
}

// ---------------------------------------------------------------------------
// Locking, and the two faces of the page
// ---------------------------------------------------------------------------

static void lock_session(void) {
	if (session) {
		vault_session_lock(session);
		session = NULL;
	}
	editor_visible = false;
	edit_index = -1;
	clear_editor_fields();
	if (keyboard) {
		keyboard_set_visible(keyboard, false);
	}
	dirty = false;
	clear_pin_entry();
	clear_first_pin();
}

static void lock_and_show_locked(void) {
	lock_session();
	state = vault_exists(path) ? VAULT_STATE_LOCKED : VAULT_STATE_SETUP;
	show_lock_state();
}

static void show_lock_state(void) {
	lv_obj_set_hidden(content_view, true);
	lv_obj_set_hidden(lock_view, false);
	lv_obj_set_hidden(editor_view, true);
	lv_obj_set_hidden(list_view, true);
	editor_visible = false;
	update_corner_buttons();

	const char *heading = "vault_locked";
	const char *hint = "vault_enter_pin";
	switch (state) {
	case VAULT_STATE_SETUP:
		heading = "vault_setup_title";
		hint = "vault_setup_hint";
		break;
	case VAULT_STATE_CONFIRM:
		heading = "vault_confirm_title";
		hint = "vault_confirm_hint";
		break;
	case VAULT_STATE_CHANGE:
		heading = "vault_new_pin";
		hint = "vault_setup_hint";
		break;
	case VAULT_STATE_CHANGE_CONFIRM:
		heading = "vault_confirm_title";
		hint = "vault_confirm_hint";
		break;
	case VAULT_STATE_LOCKED:
	default:
		break;
	}
	lv_label_set_text(lock_heading, tr(heading));
	lv_label_set_text(lock_hint, tr(hint));
	pin_dots_update();
	settingsrow_title_realign(title_label);

	if (state != VAULT_STATE_OPEN) {
		lv_timer_pause(idle_timer);
	}
}

// ---------------------------------------------------------------------------
// The PIN pad
// ---------------------------------------------------------------------------

static void pin_digit_cb(lv_event_t *e) {
	int digit = (int)(intptr_t)lv_event_get_user_data(e);
	if (pin_len >= VAULT_PIN_MAX) {
		return;
	}
	pin_entry[pin_len++] = (char)('0' + digit);
	pin_entry[pin_len] = '\0';
	pin_dots_update();
}

// PBKDF2 costs real time on this processor -- a second or two for a PIN -- and
// the page would otherwise sit still while it runs, looking hung. The hint says
// what is happening, and the frame is drawn before the work starts.
static void show_working(void) {
	lv_label_set_text(lock_hint, tr("vault_working"));
	lv_refr_now(NULL);
}

static void pin_backspace_cb(lv_event_t *e) {
	(void)e;
	if (pin_len == 0) {
		return;
	}
	pin_entry[--pin_len] = '\0';
	wipe(pin_entry + pin_len, 1);
	pin_dots_update();
}

static void pin_ok_cb(lv_event_t *e) {
	(void)e;

	if (pin_len < VAULT_PIN_MIN) {
		toast_error(tr("vault_pin_hint"));
		return;
	}

	switch (state) {
	case VAULT_STATE_SETUP:
	case VAULT_STATE_CHANGE: {
		memcpy(first_pin, pin_entry, pin_len + 1);
		clear_pin_entry();
		state = state == VAULT_STATE_SETUP ? VAULT_STATE_CONFIRM : VAULT_STATE_CHANGE_CONFIRM;
		show_lock_state();
		return;
	}

	case VAULT_STATE_CONFIRM: {
		if (strcmp(first_pin, pin_entry) != 0) {
			toast_error(tr("vault_pin_mismatch"));
			clear_pin_entry();
			return;
		}
		show_working();
		vault_status_t status = vault_create(path, first_pin, &session);
		clear_pin_entry();
		clear_first_pin();
		if (status != VAULT_OK) {
			toast_error(tr(status_text(status, true)));
			state = VAULT_STATE_SETUP;
			show_lock_state();
			return;
		}
		show_list_face();
		toast_plain(tr("vault_created"));
		// A brand new vault has nothing to read; opening the editor would be
		// a page asking to be filled in before it has shown what it is for.
		// The list, with its accent "new password" row, says that better.
		return;
	}

	case VAULT_STATE_CHANGE_CONFIRM: {
		if (strcmp(first_pin, pin_entry) != 0) {
			toast_error(tr("vault_pin_mismatch"));
			clear_pin_entry();
			return;
		}
		show_working();
		vault_status_t status = vault_session_change_pin(session, first_pin);
		clear_pin_entry();
		clear_first_pin();
		if (status != VAULT_OK) {
			toast_error(tr(status_text(status, true)));
			state = VAULT_STATE_CHANGE;
			show_lock_state();
			return;
		}
		show_list_face();
		toast_plain(tr("vault_pin_changed"));
		return;
	}

	case VAULT_STATE_LOCKED: {
		show_working();
		vault_status_t status = vault_unlock(path, pin_entry, &session);
		if (status != VAULT_OK) {
			toast_error(tr(status_text(status, false)));
			clear_pin_entry();
			show_lock_state(); // put the hint back after the wait
			return;
		}
		clear_pin_entry();
		show_list_face();
		return;
	}

	case VAULT_STATE_OPEN:
	default:
		return;
	}
}

static lv_obj_t *pin_key(lv_obj_t *parent, const char *text, const lv_image_dsc_t *icon, lv_event_cb_t cb,
						 void *user) {
	lv_obj_t *button = lv_btn_create(parent);
	lv_obj_set_size(button, key_w, key_h);
	lv_obj_add_style(button, &theme_style_card, 0);
	lv_obj_add_style(button, &theme_style_card_pressed, LV_STATE_PRESSED);
	lv_obj_set_style_radius(button, bp_pick(8, 12), 0);
	lv_obj_set_style_border_width(button, icon == &icon_check ? 1 : 0, 0);
	if (icon == &icon_check) {
		lv_obj_set_style_border_color(button, theme()->accent, 0);
		lv_obj_set_style_border_opa(button, LV_OPA_60, 0);
	}
	lv_obj_set_style_shadow_width(button, 0, 0);
	lv_obj_set_style_pad_all(button, 0, 0);
	lv_obj_add_event_cb(button, cb, LV_EVENT_CLICKED, user);
	if (icon == &icon_check) {
		pin_check = button;
	}

	if (text) {
		lv_obj_t *label = lv_label_create(button);
		lv_label_set_text(label, text);
		lv_obj_add_style(label, &theme_style_text, 0);
		lv_obj_set_style_text_font(label, &font_ui_22_bold, 0);
		lv_obj_center(label);
	} else if (icon) {
		lv_obj_t *image = lv_image_create(button);
		lv_image_set_src(image, icon);
		lv_obj_add_style(image, &theme_style_icon, 0);
		if (icon == &icon_check) {
			lv_obj_set_style_image_recolor(image, theme()->accent, 0);
			pin_check_glyph = image;
		} else if (icon == &icon_delete) {
			lv_obj_set_style_image_recolor(image, theme()->text_secondary, 0);
		}
		settingsrow_scale_corner_icon(image, config);
		lv_obj_center(image);
	}
	return button;
}

// ---------------------------------------------------------------------------
// The menu on an open vault
// ---------------------------------------------------------------------------

static void menu_change_pin_cb(void *user) {
	(void)user;
	if (!session) {
		return;
	}
	clear_pin_entry();
	clear_first_pin();
	state = VAULT_STATE_CHANGE;
	show_lock_state();
}

static void menu_lock_cb(void *user) {
	(void)user;
	lock_and_show_locked();
}

static void erase_done_cb(void *user) {
	(void)user;
	if (session) {
		vault_session_lock(session);
		session = NULL;
	}
	if (!vault_erase(path)) {
		toast_error(tr("vault_erase_failed"));
		state = vault_exists(path) ? VAULT_STATE_LOCKED : VAULT_STATE_SETUP;
		show_lock_state();
		return;
	}
	editor_visible = false;
	edit_index = -1;
	dirty = false;
	state = VAULT_STATE_SETUP;
	show_lock_state();
	toast_plain(tr("vault_erased"));
}

static void menu_erase_cb(void *user) {
	(void)user;
	confirm_show(tr("vault_erase_title"), tr("vault_erase_note"), tr("vault_erase"), erase_done_cb, NULL);
}

static void menu_cb(lv_event_t *e) {
	(void)e;
	if (!session || editor_visible) {
		return;
	}
	static const popover_item_t items[] = {
		{"vault_change_pin", menu_change_pin_cb, NULL, false, false},
		{"vault_lock_now", menu_lock_cb, NULL, false, false},
		{"vault_erase", menu_erase_cb, NULL, false, false},
	};
	popover_show(secondary_button, items, (int)(sizeof(items) / sizeof(items[0])));
	lv_timer_reset(idle_timer);
}

static void secondary_cb(lv_event_t *e) {
	(void)e;
	if (!session || state != VAULT_STATE_OPEN) {
		return;
	}
	if (editor_visible) {
		delete_entry_cb(e);
	} else {
		menu_cb(e);
	}
}

// ---------------------------------------------------------------------------
// Leaving, and the idle clock
// ---------------------------------------------------------------------------

static void idle_cb(lv_timer_t *timer) {
	(void)timer;
	if (state != VAULT_STATE_OPEN || !session) {
		return;
	}
	if (editor_visible && (dirty || editor_changed())) {
		// Security wins over an unsaved password: the vault locks either way,
		// and what was typed is saved if the card will take it.
		save_editor(false);
	}
	if (popover_visible()) {
		popover_close();
	}
	if (confirm_visible()) {
		confirm_close();
	}
	lock_and_show_locked();
	toast_plain(tr("vault_locked_toast"));
}

static void loaded_cb(lv_event_t *e) {
	(void)e;
	if (state == VAULT_STATE_OPEN) {
		lv_timer_reset(idle_timer);
		lv_timer_resume(idle_timer);
	}
}

static void unloaded_cb(lv_event_t *e) {
	(void)e;
	if (!session) {
		return;
	}
	if (editor_visible && (dirty || editor_changed())) {
		save_editor(false);
	}
	lock_session();
	state = vault_exists(path) ? VAULT_STATE_LOCKED : VAULT_STATE_SETUP;
}

static bool back_guard(void) {
	if (popover_visible()) {
		popover_close();
		return true;
	}
	if (confirm_visible()) {
		confirm_close();
		return true;
	}

	switch (state) {
	case VAULT_STATE_OPEN:
		if (editor_visible) {
			editor_can_leave();
			return true;
		}
		lock_and_show_locked();
		return true;

	case VAULT_STATE_CONFIRM:
		clear_first_pin();
		clear_pin_entry();
		state = VAULT_STATE_SETUP;
		show_lock_state();
		return true;

	case VAULT_STATE_CHANGE_CONFIRM:
		clear_first_pin();
		clear_pin_entry();
		state = VAULT_STATE_CHANGE;
		show_lock_state();
		return true;

	case VAULT_STATE_CHANGE:
		clear_first_pin();
		clear_pin_entry();
		state = VAULT_STATE_OPEN;
		show_list_face();
		return true;

	case VAULT_STATE_LOCKED:
	case VAULT_STATE_SETUP:
	default:
		return false;
	}
}

static bool back_guard_peek(void) {
	if (popover_visible() || confirm_visible()) {
		return true;
	}
	return state == VAULT_STATE_OPEN || state == VAULT_STATE_CONFIRM || state == VAULT_STATE_CHANGE ||
		   state == VAULT_STATE_CHANGE_CONFIRM;
}

// ---------------------------------------------------------------------------
// Building
// ---------------------------------------------------------------------------

static void paint_corner_glyph(lv_obj_t *glyph, lv_color_t colour) {
	if (glyph) {
		lv_obj_set_style_image_recolor(glyph, colour, 0);
	}
}

static void update_corner_buttons(void) {
	if (!primary_button || !secondary_button) {
		return;
	}
	bool open = state == VAULT_STATE_OPEN && session;
	bool editing = open && editor_visible;
	lv_obj_set_hidden(primary_button, !open);
	lv_obj_set_hidden(secondary_button, !open || (editing && edit_index < 0));

	lv_image_set_src(primary_glyph, editing ? &icon_check : &icon_plus);
	lv_image_set_src(secondary_glyph, editing ? &icon_trash : &icon_ellipsis_vertical);
	paint_corner_glyph(primary_glyph, theme()->accent);
	paint_corner_glyph(secondary_glyph,
					   editing ? theme_semantic_color(THEME_SEMANTIC_RED) : theme()->text_primary);
}

static void vault_refresh_theme(void) {
	if (!vault_screen) {
		return;
	}
	if (pin_dots) {
		pin_dots_update();
	}
	if (list_caption) {
		lv_obj_set_style_text_color(list_caption, theme()->text_secondary, 0);
	}
	if (list_count) {
		lv_obj_set_style_text_color(list_count, theme()->accent, 0);
	}
	if (title_caption) {
		lv_obj_set_style_text_color(title_caption, theme()->text_secondary, 0);
	}
	if (secret_caption) {
		lv_obj_set_style_text_color(secret_caption, theme()->text_secondary, 0);
	}
	if (secret_eye_glyph) {
		lv_obj_set_style_image_recolor(secret_eye_glyph, theme()->text_primary, 0);
	}
	if (lock_glyph) {
		lv_obj_set_style_image_recolor(lock_glyph, theme()->accent, 0);
	}
	if (pin_check) {
		lv_obj_set_style_border_color(pin_check, theme()->accent, 0);
	}
	if (pin_check_glyph) {
		lv_obj_set_style_image_recolor(pin_check_glyph, theme()->accent, 0);
	}
	if (title_field) {
		lv_obj_set_style_border_color(title_field, theme()->accent, 0);
	}
	if (secret_field) {
		lv_obj_set_style_border_color(secret_field, theme()->accent, 0);
	}
	update_corner_buttons();
	// Row tiles carry hand-set accent colours; the simplest correct answer is
	// to draw the list again under the new palette.
	if (session && state == VAULT_STATE_OPEN) {
		rebuild_list();
	}
}

static void build_lock_view(gui_config_t *cfg) {
	int content_top = settingsrow_content_top(cfg);
	int content_h = (int)cfg->screen_height - content_top;

	lock_view = lv_obj_create(vault_screen);
	lv_obj_set_size(lock_view, cfg->screen_width, content_h);
	lv_obj_align(lock_view, LV_ALIGN_TOP_LEFT, 0, content_top);
	lv_obj_set_style_bg_opa(lock_view, LV_OPA_TRANSP, 0);
	lv_obj_set_style_border_width(lock_view, 0, 0);
	lv_obj_set_style_radius(lock_view, 0, 0);
	lv_obj_set_style_pad_all(lock_view, 0, 0);
	lv_obj_set_scrollable(lock_view, false);

	lv_obj_t *heading_row = lv_obj_create(lock_view);
	lv_obj_set_size(heading_row, cfg->screen_width, 24);
	lv_obj_align(heading_row, LV_ALIGN_TOP_MID, 0, 0);
	lv_obj_set_style_bg_opa(heading_row, LV_OPA_TRANSP, 0);
	lv_obj_set_style_border_width(heading_row, 0, 0);
	lv_obj_set_style_pad_all(heading_row, 0, 0);
	lv_obj_set_style_pad_gap(heading_row, 6, 0);
	lv_obj_set_flex_flow(heading_row, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(heading_row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_scrollable(heading_row, false);

	lock_glyph = lv_image_create(heading_row);
	lv_image_set_src(lock_glyph, &icon_keylock_quick);
	lv_obj_add_style(lock_glyph, &theme_style_icon, 0);
	lv_obj_set_style_image_recolor(lock_glyph, theme()->accent, 0);
	scale_icon(lock_glyph, 18);

	lock_heading = lv_label_create(heading_row);
	lv_label_set_text(lock_heading, tr("vault_locked"));
	lv_obj_add_style(lock_heading, &theme_style_text, 0);
	lv_obj_set_style_text_font(lock_heading, &font_ui_20_bold, 0);

	lock_hint = lv_label_create(lock_view);
	lv_label_set_long_mode(lock_hint, LV_LABEL_LONG_WRAP);
	lv_label_set_text(lock_hint, tr("vault_enter_pin"));
	lv_obj_add_style(lock_hint, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(lock_hint, &font_ui_14, 0);
	lv_obj_set_width(lock_hint, cfg->screen_width - 2 * cfg->padding);
	lv_obj_set_style_text_align(lock_hint, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_align(lock_hint, LV_ALIGN_TOP_MID, 0, 28);

	pin_dots = lv_label_create(lock_view);
	lv_label_set_text(pin_dots, "");
	lv_obj_set_style_text_font(pin_dots, &font_ui_24, 0);
	lv_obj_set_width(pin_dots, cfg->screen_width - 2 * cfg->padding);
	lv_obj_set_style_text_align(pin_dots, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_align(pin_dots, LV_ALIGN_TOP_MID, 0, 64);

	int pad_w = (int)cfg->screen_width - 2 * cfg->padding;
	key_w = (pad_w - 2 * PIN_KEY_GAP) / 3;
	key_h = (content_h - PIN_PAD_TOP - 3 * PIN_KEY_GAP - 6) / 4;
	if (key_h < bp_pick(34, 44)) {
		key_h = bp_pick(34, 44);
	}

	keypad = lv_obj_create(lock_view);
	lv_obj_set_size(keypad, pad_w, content_h - PIN_PAD_TOP);
	lv_obj_align(keypad, LV_ALIGN_TOP_LEFT, cfg->padding, PIN_PAD_TOP);
	lv_obj_set_style_bg_opa(keypad, LV_OPA_TRANSP, 0);
	lv_obj_set_style_border_width(keypad, 0, 0);
	lv_obj_set_style_radius(keypad, 0, 0);
	lv_obj_set_style_pad_all(keypad, 0, 0);
	lv_obj_set_style_pad_gap(keypad, PIN_KEY_GAP, 0);
	lv_obj_set_flex_flow(keypad, LV_FLEX_FLOW_ROW_WRAP);
	lv_obj_set_flex_align(keypad, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
	lv_obj_set_scrollable(keypad, false);

	for (int digit = 1; digit <= 9; digit++) {
		char text[2] = {(char)('0' + digit), '\0'};
		pin_key(keypad, text, NULL, pin_digit_cb, (void *)(intptr_t)digit);
	}
	pin_key(keypad, NULL, &icon_delete, pin_backspace_cb, NULL);
	pin_key(keypad, "0", NULL, pin_digit_cb, (void *)(intptr_t)0);
	pin_key(keypad, NULL, &icon_check, pin_ok_cb, NULL);
}

static lv_obj_t *make_field(lv_obj_t *parent, int y, int height, bool one_line, int max_len,
							const char *placeholder) {
	lv_obj_t *field = lv_textarea_create(parent);
	lv_textarea_set_one_line(field, one_line);
	lv_textarea_set_max_length(field, (uint32_t)max_len);
	lv_textarea_set_placeholder_text(field, placeholder);
	lv_obj_set_size(field, config->screen_width - 2 * config->padding, height);
	lv_obj_align(field, LV_ALIGN_TOP_MID, 0, y);
	lv_obj_add_style(field, &theme_style_card, 0);
	lv_obj_set_style_radius(field, bp_pick(8, 12), 0);
	lv_obj_set_style_border_width(field, 1, 0);
	lv_obj_set_style_border_color(field, theme()->accent, 0);
	lv_obj_set_style_border_opa(field, LV_OPA_30, 0);
	lv_obj_set_style_shadow_width(field, 0, 0);
	lv_obj_set_style_pad_all(field, bp_pick(5, 8), 0);
	lv_obj_set_style_text_font(field, &font_ui_18, 0);
	lv_obj_set_scrollbar_mode(field, LV_SCROLLBAR_MODE_AUTO);
	keyboard_style_caret(field);
	lv_obj_add_event_cb(field, field_clicked_cb, LV_EVENT_CLICKED, NULL);
	lv_obj_add_event_cb(field, field_clicked_cb, LV_EVENT_FOCUSED, NULL);
	lv_obj_add_event_cb(field, field_changed_cb, LV_EVENT_VALUE_CHANGED, NULL);
	return field;
}

static void build_content_view(gui_config_t *cfg) {
	int content_top = settingsrow_content_top(cfg);
	int content_h = (int)cfg->screen_height - content_top;
	int keyboard_h = cfg->screen_width < 320 ? 144 : 316;

	content_view = lv_obj_create(vault_screen);
	lv_obj_set_size(content_view, cfg->screen_width, content_h);
	lv_obj_align(content_view, LV_ALIGN_TOP_LEFT, 0, content_top);
	lv_obj_set_style_bg_opa(content_view, LV_OPA_TRANSP, 0);
	lv_obj_set_style_border_width(content_view, 0, 0);
	lv_obj_set_style_radius(content_view, 0, 0);
	lv_obj_set_style_pad_all(content_view, 0, 0);
	lv_obj_set_scrollable(content_view, false);

	// ---- the list ------------------------------------------------------
	list_view = lv_obj_create(content_view);
	lv_obj_set_size(list_view, cfg->screen_width, content_h);
	lv_obj_align(list_view, LV_ALIGN_TOP_LEFT, 0, 0);
	lv_obj_set_style_bg_opa(list_view, LV_OPA_TRANSP, 0);
	lv_obj_set_style_border_width(list_view, 0, 0);
	lv_obj_set_style_radius(list_view, 0, 0);
	lv_obj_set_style_pad_all(list_view, 0, 0);
	lv_obj_set_scrollable(list_view, false);

	list_caption = lv_label_create(list_view);
	lv_label_set_text(list_caption, tr("vault_saved_passwords"));
	lv_obj_add_style(list_caption, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(list_caption, &font_ui_14, 0);
	lv_obj_set_style_text_letter_space(list_caption, 1, 0);
	lv_label_set_long_mode(list_caption, LV_LABEL_LONG_DOT);
	lv_obj_set_width(list_caption, cfg->screen_width - 2 * cfg->padding - 40);
	lv_obj_align(list_caption, LV_ALIGN_TOP_LEFT, cfg->padding, 2);

	list_count = lv_label_create(list_view);
	lv_label_set_text(list_count, "0");
	lv_obj_set_style_text_font(list_count, &font_ui_14, 0);
	lv_obj_set_style_text_color(list_count, theme()->accent, 0);
	lv_obj_align(list_count, LV_ALIGN_TOP_RIGHT, -cfg->padding, 2);

	int list_top = 22;
	list = lv_obj_create(list_view);
	lv_obj_set_size(list, cfg->screen_width - 2 * cfg->padding, content_h - list_top - cfg->padding);
	lv_obj_align(list, LV_ALIGN_TOP_LEFT, cfg->padding, list_top);
	lv_obj_set_style_bg_opa(list, LV_OPA_TRANSP, 0);
	lv_obj_set_style_border_width(list, 0, 0);
	lv_obj_set_style_radius(list, 0, 0);
	lv_obj_set_style_pad_all(list, 0, 0);
	lv_obj_set_style_pad_row(list, bp_pick(5, 7), 0);
	lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
	lv_obj_set_scroll_dir(list, LV_DIR_VER);
	lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_AUTO);

	// ---- the editor ----------------------------------------------------
	int editor_h = content_h - keyboard_h;
	if (editor_h < 104) {
		editor_h = 104;
	}
	editor_view = lv_obj_create(content_view);
	lv_obj_set_size(editor_view, cfg->screen_width, editor_h);
	lv_obj_align(editor_view, LV_ALIGN_TOP_LEFT, 0, 0);
	lv_obj_set_style_bg_opa(editor_view, LV_OPA_TRANSP, 0);
	lv_obj_set_style_border_width(editor_view, 0, 0);
	lv_obj_set_style_radius(editor_view, 0, 0);
	lv_obj_set_style_pad_all(editor_view, 0, 0);
	lv_obj_set_scrollable(editor_view, false);
	lv_obj_set_hidden(editor_view, true);

	title_caption = lv_label_create(editor_view);
	lv_label_set_text(title_caption, tr("vault_entry_title"));
	lv_obj_add_style(title_caption, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(title_caption, &font_ui_14, 0);
	lv_obj_align(title_caption, LV_ALIGN_TOP_LEFT, cfg->padding, 0);

	title_field = make_field(editor_view, 16, bp_pick(30, 38), true, VAULT_MAX_TITLE,
							 tr("vault_title_placeholder"));

	int password_y = 16 + bp_pick(30, 38) + 6;
	secret_caption = lv_label_create(editor_view);
	lv_label_set_text(secret_caption, tr("vault_entry_password"));
	lv_obj_add_style(secret_caption, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(secret_caption, &font_ui_14, 0);
	lv_obj_align(secret_caption, LV_ALIGN_TOP_LEFT, cfg->padding, password_y);

	int secret_y = password_y + 15;
	int secret_h = editor_h - secret_y;
	if (secret_h < bp_pick(30, 36)) {
		secret_h = bp_pick(30, 36);
	}
	secret_field = make_field(editor_view, secret_y, secret_h, true, VAULT_MAX_SECRET,
							  tr("vault_password_placeholder"));
	// The shared bullet tracking is tuned to the wide Qobuz login field; in a
	// 240 px page it pulls the dots on top of each other. The default advance
	// is the right one here, so the field uses plain password mode.
	lv_obj_set_style_text_font(secret_field, &font_ui_22, 0);
	lv_textarea_set_password_mode(secret_field, true);

	secret_eye = lv_btn_create(editor_view);
	lv_obj_set_size(secret_eye, bp_pick(26, 32), bp_pick(26, 32));
	lv_obj_set_style_bg_opa(secret_eye, LV_OPA_TRANSP, 0);
	lv_obj_set_style_border_width(secret_eye, 0, 0);
	lv_obj_set_style_shadow_width(secret_eye, 0, 0);
	lv_obj_set_style_pad_all(secret_eye, 0, 0);
	lv_obj_set_style_radius(secret_eye, LV_RADIUS_CIRCLE, 0);
	lv_obj_align(secret_eye, LV_ALIGN_TOP_RIGHT, -cfg->padding, password_y - 4);
	lv_obj_add_event_cb(secret_eye, secret_eye_cb, LV_EVENT_CLICKED, NULL);
	secret_eye_glyph = lv_image_create(secret_eye);
	lv_image_set_src(secret_eye_glyph, &icon_eye_off);
	lv_obj_add_style(secret_eye_glyph, &theme_style_icon, 0);
	scale_icon(secret_eye_glyph, bp_pick(16, 19));
	lv_obj_center(secret_eye_glyph);

	keyboard = keyboard_create(vault_screen, cfg->screen_width, keyboard_h, title_field, &icon_check, NULL,
							   save_editor_cb, NULL);
	keyboard_set_visible(keyboard, false);
}

static void build_corner_buttons(gui_config_t *cfg) {
	primary_button = lv_btn_create(vault_screen);
	settingsrow_place_corner_button(primary_button, cfg, 0);
	lv_obj_set_style_bg_opa(primary_button, LV_OPA_TRANSP, 0);
	lv_obj_set_style_border_width(primary_button, 0, 0);
	lv_obj_set_style_shadow_width(primary_button, 0, 0);
	lv_obj_set_style_pad_all(primary_button, 0, 0);
	lv_obj_add_event_cb(primary_button, primary_cb, LV_EVENT_CLICKED, NULL);

	primary_glyph = lv_image_create(primary_button);
	lv_image_set_src(primary_glyph, &icon_plus);
	lv_obj_add_style(primary_glyph, &theme_style_icon, 0);
	settingsrow_scale_corner_icon(primary_glyph, cfg);
	lv_obj_center(primary_glyph);
	secondary_button = lv_btn_create(vault_screen);
	settingsrow_place_corner_button(secondary_button, cfg, 1);
	lv_obj_set_style_bg_opa(secondary_button, LV_OPA_TRANSP, 0);
	lv_obj_set_style_border_width(secondary_button, 0, 0);
	lv_obj_set_style_shadow_width(secondary_button, 0, 0);
	lv_obj_set_style_pad_all(secondary_button, 0, 0);
	lv_obj_add_event_cb(secondary_button, secondary_cb, LV_EVENT_CLICKED, NULL);

	secondary_glyph = lv_image_create(secondary_button);
	lv_image_set_src(secondary_glyph, &icon_ellipsis_vertical);
	lv_obj_add_style(secondary_glyph, &theme_style_icon, 0);
	settingsrow_scale_corner_icon(secondary_glyph, cfg);
	lv_obj_center(secondary_glyph);
}

void vaultpage_init(gui_config_t *cfg) {
	config = cfg;
	lv_obj_add_style(vault_screen, &theme_style_screen, 0);

	title_label = settingsrow_title(vault_screen, cfg, "vault");
	settingsrow_title_corner_slots(title_label, cfg, 2);

	build_lock_view(cfg);
	build_content_view(cfg);
	build_corner_buttons(cfg);

	// Nothing is up until a vault is opened; the timer is built paused.
	idle_timer = lv_timer_create(idle_cb, VAULT_IDLE_MS, NULL);
	lv_timer_pause(idle_timer);

	lv_obj_add_event_cb(vault_screen, loaded_cb, LV_EVENT_SCREEN_LOADED, NULL);
	lv_obj_add_event_cb(vault_screen, unloaded_cb, LV_EVENT_SCREEN_UNLOADED, NULL);
	switcher_set_back_guard(vault_screen, back_guard);
	switcher_set_back_guard_peek(vault_screen, back_guard_peek);
	theme_register_refresh(vault_refresh_theme);

	// A page that is never opened still has its widgets; start on the lock
	// face so a stray screenshot or a theme refresh sees the right one.
	state = VAULT_STATE_SETUP;
	show_lock_state();
}

void vault_open(void) {
	if (!config) {
		return;
	}

	char dir[VAULT_PATH_MAX];
	if (!vault_dir(dir, sizeof(dir), config->sd_root_path) || !vault_path(path, sizeof(path), dir, VAULT_FILE)) {
		gui_notify_no_card();
		return;
	}
	if (!gui_card_available()) {
		gui_notify_no_card();
		return;
	}

	if (session) {
		vault_session_lock(session);
		session = NULL;
	}
	editor_visible = false;
	edit_index = -1;
	dirty = false;
	clear_pin_entry();
	clear_first_pin();
	state = vault_exists(path) ? VAULT_STATE_LOCKED : VAULT_STATE_SETUP;
	show_lock_state();
	switch_screen(vault_screen);
}
