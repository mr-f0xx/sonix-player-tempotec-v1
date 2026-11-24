#include "vaultpage.h"

#include <stdint.h>
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
// A pocket player is put down mid-sentence; the text stays encrypted on the
// card, and the keys should not stay in RAM for the rest of the day.
#define VAULT_IDLE_MS (2 * 60 * 1000)

// The pad's keys, measured from the panel rather than guessed: four rows of
// three below a heading, its hint and the PIN dots. On the V1 this lands at
// 73 x 36 a key, which is a finger target and leaves the whole page on the
// glass without a scroll.
#define PIN_KEY_GAP 4
#define PIN_PAD_TOP 92

typedef enum {
	VAULT_STATE_SETUP,	// no vault: choose the first PIN
	VAULT_STATE_CONFIRM,   // and type it again
	VAULT_STATE_LOCKED,	// a vault is there: unlock it
	VAULT_STATE_CHANGE,	// unlocked, choosing a new PIN
	VAULT_STATE_CHANGE_CONFIRM,
	VAULT_STATE_OPEN, // the text, in front of the person who unlocked it
} vault_state_t;

static gui_config_t *config;
static lv_obj_t *title_label;
static lv_obj_t *lock_view;
static lv_obj_t *lock_heading;
static lv_obj_t *lock_hint;
static lv_obj_t *pin_dots;
static lv_obj_t *keypad;
static lv_obj_t *content_view;
static lv_obj_t *field;
static lv_obj_t *edit_button;
static lv_obj_t *edit_glyph;
static lv_obj_t *menu_button;
static keyboard_t *keyboard;
static lv_timer_t *idle_timer;

static vault_state_t state;
static vault_session *session;
static bool editing;
static bool dirty;

// What has been typed into the pad, and the first copy while it is being
// confirmed or changed. Both are wiped the moment they are no longer needed.
static char pin_entry[VAULT_PIN_MAX + 1];
static size_t pin_len;
static char first_pin[VAULT_PIN_MAX + 1];

static char path[VAULT_PATH_MAX];
static int read_h;
static int write_h;
static int key_w;
static int key_h;

static void show_lock_state(void);

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
	char shown[VAULT_PIN_MAX * 4 + 1];
	size_t at = 0;
	for (size_t i = 0; i < pin_len && at + 4 < sizeof(shown); i++) {
		// A middle dot and a space: the typeface draws the bullet as wide as
		// an ideograph, which at twelve digits would run off the panel.
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
	case VAULT_ERR_NOMEM:
	case VAULT_ERR_IO:
	case VAULT_ERR_RANDOM:
		return saving ? "vault_cannot_save" : "vault_cannot_open";
	case VAULT_ERR_FORMAT:
	case VAULT_ERR_NO_FILE:
		return saving ? "vault_cannot_save" : "vault_cannot_open";
	default:
		return saving ? "vault_cannot_save" : "vault_cannot_open";
	}
}

// ---------------------------------------------------------------------------
// The text
// ---------------------------------------------------------------------------

static void field_set_height(void) {
	if (field) {
		lv_obj_set_height(field, editing ? write_h : read_h);
	}
}

// Reading a vault is not a wait: the text is handed to the label in one
// assignment rather than through lv_textarea_set_text(), which inserts a
// character at a time when a length limit is in force.
static void field_set_text(const char *text) {
	lv_label_set_text(lv_textarea_get_label(field), text ? text : "");
	lv_textarea_set_cursor_pos(field, 0);
	lv_obj_scroll_to_y(field, 0, LV_ANIM_OFF);
}

static void field_clear(void) {
	// The textarea drops its copy here. The copies that matter -- the session's
	// text and the keys -- are wiped by vault_session_lock(); LVGL's freed
	// buffer is the one thing this layer cannot promise to scrub, which
	// docs/vault.md says rather than pretending otherwise.
	lv_label_set_text(lv_textarea_get_label(field), "");
	lv_textarea_set_cursor_pos(field, 0);
}

static void set_editing(bool on) {
	if (!field || !keyboard) {
		return;
	}
	editing = on && session;
	field_set_height();

	if (editing) {
		lv_obj_set_hidden(menu_button, true);
		lv_image_set_src(edit_glyph, &icon_check);
		lv_textarea_set_placeholder_text(field, tr("vault_placeholder"));
		keyboard_set_visible(keyboard, true);
		keyboard_reset(keyboard);
		keyboard_show_caret(field, true);
		lv_textarea_set_cursor_click_pos(field, true);
		lv_obj_add_state(field, LV_STATE_FOCUSED);
		lv_textarea_set_cursor_pos(field, LV_TEXTAREA_CURSOR_LAST);
		return;
	}

	keyboard_set_visible(keyboard, false);
	keyboard_show_caret(field, false);
	lv_textarea_set_cursor_click_pos(field, false);
	lv_obj_remove_state(field, LV_STATE_FOCUSED);
	lv_image_set_src(edit_glyph, &icon_pencil);
	lv_textarea_set_placeholder_text(field, tr("vault_empty"));
	// Back to reading the open vault, the menu is available again. A page
	// that is locking hides it after this call; the condition keeps this from
	// putting it back over the lock pad on the way out.
	if (state == VAULT_STATE_OPEN && session) {
		lv_obj_set_hidden(menu_button, false);
	}
}

static bool save_text(bool tell) {
	if (!session) {
		return false;
	}
	vault_status_t status = vault_session_save(session, lv_textarea_get_text(field));
	if (status != VAULT_OK) {
		if (tell) {
			toast_error(tr(status_text(status, true)));
		}
		return false;
	}
	dirty = false;
	return true;
}

// The corner check and the keyboard's accent key both land here. The writing
// goes to the card first; only then does the page go back to reading it.
static void save_cb(lv_event_t *e) {
	(void)e;
	if (!editing || !session) {
		return;
	}
	if (!dirty) {
		set_editing(false);
		return;
	}
	if (!save_text(true)) {
		return; // still editing, still dirty: nothing has been lost
	}
	set_editing(false);
	toast_plain(tr("vault_saved"));
	lv_timer_reset(idle_timer);
}

static void edit_toggle_cb(lv_event_t *e) {
	(void)e;
	if (editing) {
		save_cb(e);
		return;
	}
	if (!session) {
		return;
	}
	dirty = false;
	set_editing(true);
	lv_timer_reset(idle_timer);
}

static void value_changed_cb(lv_event_t *e) {
	(void)e;
	if (editing) {
		dirty = true;
		lv_timer_reset(idle_timer);
	}
}

// ---------------------------------------------------------------------------
// Locking, and the two faces of the page
// ---------------------------------------------------------------------------

static void lock_session(void) {
	if (session) {
		vault_session_lock(session);
		session = NULL;
	}
	set_editing(false);
	field_clear();
	dirty = false;
	clear_pin_entry();
	clear_first_pin();
}

static void lock_and_show_locked(void) {
	lock_session();
	state = vault_exists(path) ? VAULT_STATE_LOCKED : VAULT_STATE_SETUP;
	show_lock_state();
}

static void show_open(void) {
	state = VAULT_STATE_OPEN;
	lv_obj_set_hidden(lock_view, true);
	lv_obj_set_hidden(content_view, false);
	lv_obj_set_hidden(edit_button, false);
	lv_obj_set_hidden(menu_button, false);
	set_editing(false);
	field_set_text(vault_session_text(session));
	dirty = false;
	clear_pin_entry();
	clear_first_pin();
	lv_timer_reset(idle_timer);
	lv_timer_resume(idle_timer);
}

static void show_lock_state(void) {
	lv_obj_set_hidden(content_view, true);
	lv_obj_set_hidden(lock_view, false);
	lv_obj_set_hidden(edit_button, true);
	lv_obj_set_hidden(menu_button, true);
	set_editing(false);

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

	// Only an open vault needs the idle clock.
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
		vault_status_t status = vault_create(path, first_pin, "", &session);
		clear_pin_entry();
		clear_first_pin();
		if (status != VAULT_OK) {
			toast_error(tr(status_text(status, true)));
			state = VAULT_STATE_SETUP;
			show_lock_state();
			return;
		}
		show_open();
		toast_plain(tr("vault_created"));
		// A new, empty vault opens in editing: the first thing to do with it
		// is write something.
		set_editing(true);
		return;
	}

	case VAULT_STATE_CHANGE_CONFIRM: {
		if (strcmp(first_pin, pin_entry) != 0) {
			toast_error(tr("vault_pin_mismatch"));
			clear_pin_entry();
			return;
		}
		show_working();
		vault_status_t status = vault_session_change_pin(session, first_pin, vault_session_text(session));
		clear_pin_entry();
		clear_first_pin();
		if (status != VAULT_OK) {
			toast_error(tr(status_text(status, true)));
			state = VAULT_STATE_CHANGE;
			show_lock_state();
			return;
		}
		state = VAULT_STATE_OPEN;
		show_open();
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
		show_open();
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
	lv_obj_set_style_border_width(button, 0, 0);
	lv_obj_set_style_shadow_width(button, 0, 0);
	lv_obj_set_style_pad_all(button, 0, 0);
	lv_obj_add_event_cb(button, cb, LV_EVENT_CLICKED, user);

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
		// The session is already gone, so the page cannot go back to showing
		// the text. If the file is still there, ask for the PIN again.
		toast_error(tr("vault_erase_failed"));
		state = vault_exists(path) ? VAULT_STATE_LOCKED : VAULT_STATE_SETUP;
		show_lock_state();
		return;
	}
	editing = false;
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
	if (!session || editing) {
		return;
	}
	static const popover_item_t items[] = {
		{"vault_change_pin", menu_change_pin_cb, NULL, false, false},
		{"vault_lock_now", menu_lock_cb, NULL, false, false},
		{"vault_erase", menu_erase_cb, NULL, false, false},
	};
	popover_show(menu_button, items, (int)(sizeof(items) / sizeof(items[0])));
	lv_timer_reset(idle_timer);
}

// ---------------------------------------------------------------------------
// Leaving, and the idle clock
// ---------------------------------------------------------------------------

static void idle_cb(lv_timer_t *timer) {
	(void)timer;
	if (state != VAULT_STATE_OPEN || !session) {
		return;
	}
	if (editing && dirty) {
		// Security wins over an unsaved sentence: the vault locks either way,
		// and what was typed is saved if the card will take it.
		save_text(false);
	}
	// A menu left open over the pad is part of what is being put away; a
	// question about erasing must not outlive the vault it was asked about.
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
	if (editing && dirty) {
		save_text(false);
	}
	vault_session_lock(session);
	session = NULL;
	set_editing(false);
	field_clear();
	dirty = false;
	state = vault_exists(path) ? VAULT_STATE_LOCKED : VAULT_STATE_SETUP;
	clear_pin_entry();
	clear_first_pin();
}

static bool back_guard(void) {
	// A menu or a question is the first thing to put away.
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
		if (editing) {
			if (dirty && !save_text(true)) {
				return true; // stay in the editor rather than lose the typing
			}
			dirty = false;
			set_editing(false);
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
		show_open();
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

static void vault_refresh_theme(void) {
	if (pin_dots) {
		lv_obj_set_style_text_color(pin_dots, theme()->accent, 0);
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

	lock_heading = lv_label_create(lock_view);
	lv_label_set_text(lock_heading, tr("vault_locked"));
	lv_obj_add_style(lock_heading, &theme_style_text, 0);
	lv_obj_set_style_text_font(lock_heading, &font_ui_20_bold, 0);
	lv_obj_set_width(lock_heading, cfg->screen_width - 2 * cfg->padding);
	lv_obj_set_style_text_align(lock_heading, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_align(lock_heading, LV_ALIGN_TOP_MID, 0, 0);

	lock_hint = lv_label_create(lock_view);
	lv_label_set_long_mode(lock_hint, LV_LABEL_LONG_WRAP);
	lv_label_set_text(lock_hint, tr("vault_enter_pin"));
	lv_obj_add_style(lock_hint, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(lock_hint, &font_ui_14, 0);
	lv_obj_set_width(lock_hint, cfg->screen_width - 2 * cfg->padding);
	lv_obj_set_style_text_align(lock_hint, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_align(lock_hint, LV_ALIGN_TOP_MID, 0, 24);

	pin_dots = lv_label_create(lock_view);
	lv_label_set_text(pin_dots, "");
	lv_obj_set_style_text_font(pin_dots, &font_ui_24, 0);
	lv_obj_set_width(pin_dots, cfg->screen_width - 2 * cfg->padding);
	lv_obj_set_style_text_align(pin_dots, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_align(pin_dots, LV_ALIGN_TOP_MID, 0, 62);

	// The keys, measured from what is left: three columns and four rows, with
	// the same air the rest of the page uses.
	int pad_w = (int)cfg->screen_width - 2 * cfg->padding;
	key_w = (pad_w - 2 * PIN_KEY_GAP) / 3;
	key_h = (content_h - PIN_PAD_TOP - 3 * PIN_KEY_GAP) / 4;
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

static void build_content_view(gui_config_t *cfg) {
	int content_top = settingsrow_content_top(cfg);
	int content_h = (int)cfg->screen_height - content_top;
	int keyboard_h = cfg->screen_width < 320 ? 144 : 316;
	read_h = content_h - cfg->padding;
	write_h = read_h - keyboard_h;

	content_view = lv_obj_create(vault_screen);
	lv_obj_set_size(content_view, cfg->screen_width, content_h);
	lv_obj_align(content_view, LV_ALIGN_TOP_LEFT, 0, content_top);
	lv_obj_set_style_bg_opa(content_view, LV_OPA_TRANSP, 0);
	lv_obj_set_style_border_width(content_view, 0, 0);
	lv_obj_set_style_radius(content_view, 0, 0);
	lv_obj_set_style_pad_all(content_view, 0, 0);
	lv_obj_set_scrollable(content_view, false);

	field = lv_textarea_create(content_view);
	lv_textarea_set_one_line(field, false);
	lv_textarea_set_max_length(field, VAULT_MAX_CHARS);
	lv_textarea_set_placeholder_text(field, tr("vault_empty"));
	lv_obj_set_size(field, cfg->screen_width - 2 * cfg->padding, read_h);
	lv_obj_align(field, LV_ALIGN_TOP_LEFT, cfg->padding, 0);
	lv_obj_add_style(field, &theme_style_card, 0);
	lv_obj_set_style_radius(field, bp_pick(10, 16), 0);
	lv_obj_set_style_border_width(field, 0, 0);
	lv_obj_set_style_shadow_width(field, 0, 0);
	lv_obj_set_style_pad_all(field, bp_pick(8, 14), 0);
	lv_obj_set_style_text_font(field, &font_ui_22, 0);
	lv_obj_set_scrollbar_mode(field, LV_SCROLLBAR_MODE_AUTO);
	keyboard_style_caret(field);
	lv_obj_add_event_cb(field, value_changed_cb, LV_EVENT_VALUE_CHANGED, NULL);

	keyboard = keyboard_create(vault_screen, cfg->screen_width, keyboard_h, field, &icon_check, NULL, save_cb, NULL);
}

static void build_corner_buttons(gui_config_t *cfg) {
	edit_button = lv_btn_create(vault_screen);
	settingsrow_place_corner_button(edit_button, cfg, 0);
	lv_obj_set_style_bg_opa(edit_button, LV_OPA_TRANSP, 0);
	lv_obj_set_style_border_width(edit_button, 0, 0);
	lv_obj_set_style_shadow_width(edit_button, 0, 0);
	lv_obj_set_style_pad_all(edit_button, 0, 0);
	lv_obj_add_event_cb(edit_button, edit_toggle_cb, LV_EVENT_CLICKED, NULL);

	edit_glyph = lv_image_create(edit_button);
	lv_image_set_src(edit_glyph, &icon_pencil);
	lv_obj_add_style(edit_glyph, &theme_style_icon, 0);
	settingsrow_scale_corner_icon(edit_glyph, cfg);
	lv_obj_center(edit_glyph);

	menu_button = lv_btn_create(vault_screen);
	settingsrow_place_corner_button(menu_button, cfg, 1);
	lv_obj_set_style_bg_opa(menu_button, LV_OPA_TRANSP, 0);
	lv_obj_set_style_border_width(menu_button, 0, 0);
	lv_obj_set_style_shadow_width(menu_button, 0, 0);
	lv_obj_set_style_pad_all(menu_button, 0, 0);
	lv_obj_add_event_cb(menu_button, menu_cb, LV_EVENT_CLICKED, NULL);

	lv_obj_t *menu_glyph = lv_image_create(menu_button);
	lv_image_set_src(menu_glyph, &icon_ellipsis_vertical);
	lv_obj_add_style(menu_glyph, &theme_style_icon, 0);
	settingsrow_scale_corner_icon(menu_glyph, cfg);
	lv_obj_center(menu_glyph);
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
	dirty = false;
	clear_pin_entry();
	clear_first_pin();
	state = vault_exists(path) ? VAULT_STATE_LOCKED : VAULT_STATE_SETUP;
	show_lock_state();
	switch_screen(vault_screen);
}
