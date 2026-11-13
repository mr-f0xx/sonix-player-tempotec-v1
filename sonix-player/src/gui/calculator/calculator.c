#include "calculator.h"

#include <string.h>

#include "lvgl/lvgl.h"

#include "src/gui/board_profile.h"
#include "src/gui/calculator/calcmodel.h"
#include "src/gui/fonts/fonts.h"
#include "src/gui/shell/icons.h"
#include "src/gui/shell/settingsrow.h"
#include "src/gui/shell/switcher.h" // switch_screen
#include "src/gui/shell/theme.h"
#include "src/system/core/lang.h"

lv_obj_t *calculator_screen;

// The keypad is the four columns and five rows every calculator has. The keys
// are the page's whole layout: the display takes what is left above them.
#define CALC_COLS 4
#define CALC_ROWS 5
#define CALC_KEYS (CALC_COLS * CALC_ROWS)

typedef enum {
	KEY_CLEAR,
	KEY_BACKSPACE,
	KEY_PERCENT,
	KEY_DIGIT,
	KEY_DECIMAL,
	KEY_SIGN,
	KEY_OPERATOR,
	KEY_EQUALS,
} calc_key_kind_t;

typedef struct {
	calc_key_kind_t kind;
	const char *label; // NULL for the backspace key, which draws a glyph
	char value;        // the digit, or the operator
} calc_key_t;

// Top left to bottom right, the way the keys are laid out on the glass. The
// minus key wears the minus sign (U+2212) rather than the hyphen, which is a
// punctuation mark that happens to look like one: the face draws the sign half
// as long again.
static const calc_key_t KEYS[CALC_KEYS] = {
	{KEY_CLEAR, "AC", 0},     {KEY_BACKSPACE, NULL, 0}, {KEY_PERCENT, "%", 0},      {KEY_OPERATOR, "\xC3\xB7", '/'},
	{KEY_DIGIT, "7", '7'},    {KEY_DIGIT, "8", '8'},    {KEY_DIGIT, "9", '9'},      {KEY_OPERATOR, "\xC3\x97", '*'},
	{KEY_DIGIT, "4", '4'},    {KEY_DIGIT, "5", '5'},    {KEY_DIGIT, "6", '6'},      {KEY_OPERATOR, "\xE2\x88\x92", '-'},
	{KEY_DIGIT, "1", '1'},    {KEY_DIGIT, "2", '2'},    {KEY_DIGIT, "3", '3'},      {KEY_OPERATOR, "+", '+'},
	{KEY_SIGN, "\xC2\xB1", 0}, {KEY_DIGIT, "0", '0'},    {KEY_DECIMAL, ".", 0},     {KEY_EQUALS, "=", 0},
};

// The sum in progress. Kept here rather than in the page because the page
// outlives every visit: the keys are built once at startup and stay.
static calc_model_t model = {.shown = "0"};

static lv_obj_t *display_card; // the card the sum is drawn in
static lv_obj_t *display_label;
static lv_obj_t *operation_label;

static const char *operation_symbol(char op) {
	switch (op) {
	case '+': return "+";
	case '-': return "\xE2\x88\x92";
	case '*': return "\xC3\x97";
	case '/': return "\xC3\xB7";
	case '%': return "%";
	default: return "";
	}
}

static void operation_refresh(char op) {
	if (operation_label) {
		lv_label_set_text(operation_label, operation_symbol(op));
	}
}

// What the display shows: the model's own text, or the error state's, which is
// the one string here that has to be read in the user's language.
static void display_refresh(void) {
	lv_label_set_text(display_label, calc_is_error(&model) ? tr("calculator_error") : calc_display(&model));
}

static void key_cb(lv_event_t *e) {
	const calc_key_t *key = lv_event_get_user_data(e);
	if (!key) {
		return;
	}

	switch (key->kind) {
	case KEY_CLEAR:
		operation_refresh(0);
		calc_key_clear(&model);
		break;
	case KEY_BACKSPACE:
		calc_key_backspace(&model);
		break;
	case KEY_PERCENT:
		operation_refresh('%');
		calc_key_percent(&model);
		break;
	case KEY_DIGIT:
		calc_key_digit(&model, key->value);
		break;
	case KEY_DECIMAL:
		calc_key_decimal(&model);
		break;
	case KEY_SIGN:
		calc_key_sign(&model);
		break;
	case KEY_OPERATOR:
		operation_refresh(key->value);
		calc_key_operator(&model, key->value);
		break;
	case KEY_EQUALS:
		operation_refresh(0);
		calc_key_equals(&model);
		break;
	}
	display_refresh();
}

// The keys whose caption is a sign rather than a number or a letter. They are
// drawn a size up, and in the bold weight: a face sets a multiplication sign a
// little over half the height of the digits it sits beside and a hyphen a
// tenth of it, so in the keypad's own size the operator column came out as
// specks next to the numbers they work on -- on a 240-pixel panel, eight
// pixels of multiply sign. A step up in size and the bold face bring them to
// the digits' own height and weight. The digits, the percent and the decimal
// point stay where they are: they were never hard to read.
static bool key_is_symbol(calc_key_kind_t kind) {
	return kind == KEY_OPERATOR || kind == KEY_SIGN || kind == KEY_EQUALS;
}

// A key. Number keys wear the ordinary card, the operators the accent's ink
// and the equals key the accent itself, which is how a calculator says which
// key finishes the sum.
static lv_obj_t *key_make(lv_obj_t *parent, const calc_key_t *key) {
	const bool equals = key->kind == KEY_EQUALS;

	lv_obj_t *button = lv_btn_create(parent);
	lv_obj_add_style(button, equals ? &theme_style_accent_bg : &theme_style_card, 0);
	lv_obj_add_style(button, &theme_style_card_pressed, LV_STATE_PRESSED);
	lv_obj_set_style_radius(button, bp_pick(10, 16), 0);
	lv_obj_set_style_shadow_width(button, 0, 0);
	lv_obj_set_style_pad_all(button, 0, 0);
	lv_obj_set_scrollable(button, false);
	lv_obj_add_event_cb(button, key_cb, LV_EVENT_CLICKED, (void *)key);

	if (!key->label) {
		lv_obj_t *glyph = lv_image_create(button);
		lv_image_set_src(glyph, &icon_delete);
		lv_obj_add_style(glyph, &theme_style_icon, 0);
		lv_obj_center(glyph);
		return button;
	}

	lv_obj_t *label = lv_label_create(button);
	lv_label_set_text(label, key->label);
	lv_obj_set_style_text_font(label, key_is_symbol(key->kind) ? &font_ui_36_bold : &font_ui_24, 0);
	if (equals) {
		lv_obj_set_style_text_color(label, theme_on_accent(), 0);
	} else if (key->kind == KEY_OPERATOR) {
		lv_obj_set_style_text_color(label, theme()->accent, 0);
	} else {
		lv_obj_add_style(label, &theme_style_text, 0);
	}
	lv_obj_center(label);
	return button;
}

static lv_obj_t *key_labels[CALC_KEYS];

// The equals key's label is drawn in whatever colour reads on the accent, and
// the operators' in the accent itself. Both are hand-set, so they have to be
// set again after a theme or accent change: a style would follow on its own.
static void calc_refresh_theme(void) {
	// The display's keyline is the one edge drawn on the page, and it wears the
	// accent rather than the grey the built-in theme gives a plain card. The
	// width stays the theme's own; only the colour is ours, and a colour set by
	// hand is a snapshot -- so it is put back here whenever the palette moves.
	if (display_card) {
		lv_obj_set_style_border_color(display_card, theme()->accent, 0);
	}
	if (operation_label) {
		lv_obj_set_style_text_color(operation_label, theme()->accent, 0);
	}
	for (int i = 0; i < CALC_KEYS; i++) {
		if (!key_labels[i]) {
			continue;
		}
		if (KEYS[i].kind == KEY_EQUALS) {
			lv_obj_set_style_text_color(key_labels[i], theme_on_accent(), 0);
		} else if (KEYS[i].kind == KEY_OPERATOR) {
			lv_obj_set_style_text_color(key_labels[i], theme()->accent, 0);
		}
	}
}

static void screen_loaded_cb(lv_event_t *e) {
	(void)e;
	// The sum is where it was left, and the error state's words are the
	// language's own -- which may have changed since the page was built.
	display_refresh();
}

void calculator_init(gui_config_t *cfg) {
	lv_obj_add_style(calculator_screen, &theme_style_screen, 0);
	lv_obj_t *title = settingsrow_title(calculator_screen, cfg, "calculator");
	settingsrow_title_corner_slots(title, cfg, 0);

	// Keep the last arithmetic key visible in the header: on the V1's small
	// touch panel this gives an immediate, legible check of the tapped key.
	operation_label = lv_label_create(calculator_screen);
	lv_obj_set_style_text_font(operation_label, &font_ui_36_bold, 0);
	lv_obj_set_style_text_color(operation_label, theme()->accent, 0);
	lv_obj_set_width(operation_label, settingsrow_corner_button_size(cfg));
	lv_obj_set_style_text_align(operation_label, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_align(operation_label, LV_ALIGN_TOP_RIGHT, -cfg->padding, cfg->top_bar_height + cfg->padding + (cfg->screen_width < 320 ? 6 : 10));
	operation_refresh(0);

	const int padding = cfg->padding;
	const int gap = bp_pick(4, 10);
	const int display_h = bp_pick(46, 96);
	const int content_top = settingsrow_content_top(cfg);
	const int keypad_top = content_top + display_h + bp_pick(6, 14);
	const int keypad_w = cfg->screen_width - 2 * padding;
	const int key_w = (keypad_w - (CALC_COLS - 1) * gap) / CALC_COLS;
	const int key_h = (cfg->screen_height - keypad_top - padding - (CALC_ROWS - 1) * gap) / CALC_ROWS;

	lv_obj_t *display = lv_obj_create(calculator_screen);
	lv_obj_set_size(display, keypad_w, display_h);
	lv_obj_align(display, LV_ALIGN_TOP_LEFT, padding, content_top);
	lv_obj_add_style(display, &theme_style_card, 0);
	lv_obj_set_style_radius(display, bp_pick(10, 16), 0);
	lv_obj_set_style_shadow_width(display, 0, 0);
	// An opaque border: its width and colour are the theme's own until
	// calc_refresh_theme() gives it the accent.
	lv_obj_set_style_border_opa(display, LV_OPA_COVER, 0);
	lv_obj_set_style_pad_hor(display, bp_pick(8, 18), 0);
	lv_obj_set_style_pad_ver(display, 0, 0);
	lv_obj_set_scrollable(display, false);
	display_card = display;

	display_label = lv_label_create(display);
	lv_obj_add_style(display_label, &theme_style_text, 0);
	lv_obj_set_style_text_font(display_label, &font_ui_32, 0);
	// Right-aligned, and clipped rather than wrapped: the model keeps the text
	// inside the display already, and a number that wrapped onto a second line
	// would push the first one out of the card.
	lv_obj_set_width(display_label, lv_pct(100));
	lv_obj_set_style_text_align(display_label, LV_TEXT_ALIGN_RIGHT, 0);
	lv_label_set_long_mode(display_label, LV_LABEL_LONG_CLIP);
	lv_obj_align(display_label, LV_ALIGN_RIGHT_MID, 0, 0);
	lv_label_set_text(display_label, calc_display(&model));

	lv_obj_t *keypad = lv_obj_create(calculator_screen);
	lv_obj_set_size(keypad, keypad_w, key_h * CALC_ROWS + gap * (CALC_ROWS - 1));
	lv_obj_align(keypad, LV_ALIGN_TOP_LEFT, padding, keypad_top);
	lv_obj_set_style_bg_opa(keypad, LV_OPA_TRANSP, 0);
	lv_obj_set_style_border_width(keypad, 0, 0);
	lv_obj_set_style_radius(keypad, 0, 0);
	lv_obj_set_style_pad_all(keypad, 0, 0);
	lv_obj_set_style_pad_gap(keypad, gap, 0);
	lv_obj_set_scrollable(keypad, false);
	lv_obj_set_flex_flow(keypad, LV_FLEX_FLOW_ROW_WRAP);
	// SPACE_BETWEEN rather than START: a width that does not divide by four
	// leaves a few pixels over, and they belong in the gaps rather than at the
	// right-hand edge.
	lv_obj_set_flex_align(keypad, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

	for (int i = 0; i < CALC_KEYS; i++) {
		lv_obj_t *button = key_make(keypad, &KEYS[i]);
		lv_obj_set_size(button, key_w, key_h);
		key_labels[i] = lv_obj_get_child(button, 0);
	}

	lv_obj_add_event_cb(calculator_screen, screen_loaded_cb, LV_EVENT_SCREEN_LOADED, NULL);
	theme_register_refresh(calc_refresh_theme);
	calc_refresh_theme();

	// No swipe-back gesture on this page: the keypad is the page, and a finger
	// dragged across it is a finger on a key. The chevron still goes back.
}

void calculator_open(void) {
	display_refresh();
	switch_screen(calculator_screen);
}
