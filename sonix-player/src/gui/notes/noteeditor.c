#include "noteeditor.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lvgl/lvgl.h"

#include "src/gui/board_profile.h"
#include "src/gui/fonts/fonts.h"
#include "src/gui/library/textview.h"
#include "src/gui/shell/icons.h"
#include "src/gui/shell/keyboard.h"
#include "src/gui/shell/settingsrow.h"
#include "src/gui/shell/switcher.h"
#include "src/gui/shell/theme.h"
#include "src/gui/shell/theme_icons.h"
#include "src/gui/shell/toast.h"
#include "src/system/core/lang.h"
#include "src/system/notes/notesstore.h"

lv_obj_t *noteeditor_screen;

// How long the writing has to have stopped before the page writes the note by
// itself. Long enough that a pause between words does not write, short enough
// that a power-off in the middle of a sentence costs one.
#define AUTOSAVE_MS 2000

static lv_obj_t *title_label;
static lv_obj_t *field;
static lv_obj_t *edit_button;
static lv_obj_t *edit_glyph;
static keyboard_t *keyboard;
static lv_timer_t *autosave_timer;

static char dir[1024];
static char name[NOTES_NAME_MAX + 1];
static bool dirty;         // typed into since the last write
static char idle_text[64]; // the end of the text as of the last tick

// Whether the note is being written or only read.
//
// A note opens the way the file manager opens a text file: the text fills the
// page, there is no keyboard, and nothing on the card can be written. The
// corner button -- a pencil -- is what turns the page into an editor: the
// keyboard comes up, the note makes room for it, and the pencil becomes the
// check that puts the writing away again. Autosave runs only while `editing`,
// so a note that was only read is left exactly as it is on the card.
static bool editing;

// The field's two heights: the whole page while reading, and as much as is left
// above the keyboard while writing. Kept from the layout in noteeditor_init().
static int read_h;
static int write_h;

static void set_editing(bool on);

// The reading state, without the toggling: what noteeditor_open() and
// noteeditor_new() leave the page in before they add what they add.
static void field_set_height(void) {
	lv_obj_set_height(field, editing ? write_h : read_h);
}

static void write_note(void) {
	if (!dirty) {
		return;
	}
	if (!notes_fits_editor(lv_textarea_get_text(field))) {
		// Unreachable through the keyboard -- the field's own limit is the
		// same character count -- but the card is not the place to find out.
		return;
	}
	if (!notes_write(dir, name, lv_textarea_get_text(field))) {
		// Still dirty: the next pause, the check key or leaving the page will
		// try again rather than the text being quietly dropped.
		return;
	}
	dirty = false;
}

// Reading and writing. While the note is being read the button shows the pencil
// and this opens the keyboard; while it is being written it shows the check and
// this puts the writing on the card and hands the note back to be read.
static void edit_toggle_cb(lv_event_t *e) {
	(void)e;
	if (!editing) {
		set_editing(true);
		return;
	}

	// The check is also the way back to reading, and reading is what the page
	// is for: the writing goes to the card on the way out.
	bool was_dirty = dirty;
	bool wrote = was_dirty && notes_write(dir, name, lv_textarea_get_text(field));
	if (wrote) {
		dirty = false;
	}
	set_editing(false);
	if (was_dirty && !wrote) {
		toast_error(tr("notes_cannot_save"));
	} else if (wrote) {
		toast_plain(tr("notes_saved"));
	}
}

// A new line, from the keyboard's accent key. A note is a list of lines as
// often as it is a paragraph, and this is the only page that has to be able to
// start one.
static void newline_cb(lv_event_t *e) {
	lv_obj_t *target = lv_event_get_user_data(e);
	if (target) {
		lv_textarea_add_text(target, "\n");
	}
}

static void value_changed_cb(lv_event_t *e) {
	(void)e;
	// The field's text only changes from the keyboard, which is only up while
	// the note is being written -- but this is the flag a write hangs on, so
	// it is set by an edit and by nothing else.
	if (editing) {
		dirty = true;
	}
}

// A few characters off the end of the text, which is enough to tell one tick's
// text from the next one's without holding a second copy of the note.
static void text_tail(const char *text, char *out, size_t size) {
	size_t len = strlen(text);
	if (len > size - 1) {
		len = size - 1;
	}
	memcpy(out, text + strlen(text) - len, len);
	out[len] = '\0';
}

static void autosave_cb(lv_timer_t *timer) {
	(void)timer;
	if (!dirty) {
		return;
	}
	// Only once the typing has stopped: the timer runs all the while, and
	// writing on every tick would put a card write between every word.
	char tail[64];
	text_tail(lv_textarea_get_text(field), tail, sizeof(tail));
	if (strcmp(tail, idle_text) == 0) {
		write_note();
		return;
	}
	memcpy(idle_text, tail, sizeof(idle_text));
}

static void unloaded_cb(lv_event_t *e) {
	(void)e;
	// Only what was typed while the page was in editing mode. A note opened to
	// be read is closed without a write, which is also what makes it safe to
	// open one at all.
	if (editing) {
		write_note();
	}
	// And back to reading: the page is one object shown many times, and the
	// next note to open has to start with the keyboard down.
	set_editing(false);
}

// The title row says which note this is, the way the text viewer says which
// file it is showing.
static void title_set(const char *note_name) {
	char shown[NOTES_NAME_MAX + 1];
	notes_display_name(note_name, shown, sizeof(shown));
	lv_label_set_text(title_label, shown);
}

// Reading a note and writing one are the same page in two states, and this is
// the whole of the difference: the keyboard, the caret, whether a tap places
// the caret, the room the note has, and which glyph the corner button wears.
static void set_editing(bool on) {
	// Applied every time rather than only on a change: leaving the page with
	// the keyboard up has to put it down again the next time the page is
	// opened, whether or not the page it is opened for is a new one.
	editing = on;
	field_set_height();

	if (on) {
		keyboard_set_visible(keyboard, true);
		keyboard_reset(keyboard);
		keyboard_show_caret(field, true);
		// A tap may place the caret while writing, and may not while reading:
		// on a page that cannot be typed into, a tap on the text is a tap on
		// the note, not a place to put a cursor.
		lv_textarea_set_cursor_click_pos(field, true);
		lv_obj_add_state(field, LV_STATE_FOCUSED);
		// Writing continues the note rather than starting at the top of it --
		// and a tap into the text still moves the caret where it is wanted.
		lv_textarea_set_cursor_pos(field, LV_TEXTAREA_CURSOR_LAST);
		lv_image_set_src(edit_glyph, theme_icon(&icon_check));
		return;
	}

	keyboard_set_visible(keyboard, false);
	keyboard_show_caret(field, false);
	lv_textarea_set_cursor_click_pos(field, false);
	lv_obj_remove_state(field, LV_STATE_FOCUSED);
	lv_image_set_src(edit_glyph, &icon_pencil);
}

// Reads the note into the field and shows it, to be read. Setting a textarea's
// text costs a walk of the text for every character -- a non-zero max_length
// makes LVGL insert them one at a time -- so the note's length is judged before
// the text is handed over (see noteeditor_open) rather than by the field
// refusing the tail of it.
static bool show_note(const char *note_dir, const char *note_name, const char *text) {
	if (!note_dir || !note_dir[0] || !note_name || !note_name[0]) {
		return false;
	}
	snprintf(dir, sizeof(dir), "%s", note_dir);
	snprintf(name, sizeof(name), "%s", note_name);
	title_set(name);

	set_editing(false);

	// The label directly rather than lv_textarea_set_text(): there is nothing
	// to filter and nothing to limit here -- the note is known to fit -- and
	// this is the one assignment rather than a character at a time.
	lv_label_set_text(lv_textarea_get_label(field), text ? text : "");
	lv_textarea_set_cursor_pos(field, 0);
	lv_obj_scroll_to_y(field, 0, LV_ANIM_OFF);
	dirty = false;
	idle_text[0] = '\0';
	lv_timer_resume(autosave_timer);
	lv_timer_reset(autosave_timer);

	switch_screen(noteeditor_screen);
	return true;
}

bool noteeditor_open(const char *note_dir, const char *note_name) {
	bool truncated = false;
	char path[1200];
	if (!notes_path(path, sizeof(path), note_dir, note_name)) {
		return false;
	}

	char *text = notes_read(path, &truncated);
	if (!text) {
		gui_notify_popup(tr("notes_cannot_open"));
		return false;
	}
	if (truncated) {
		// Past the limit on what is read at all, so not even a note the reader
		// could show: this is the word for a file that cannot be opened.
		free(text);
		gui_notify_popup(tr("notes_cannot_open"));
		return false;
	}

	// A note too long to be typed into is still a note, and reading it is the
	// common thing to do with one. It is shown by the file manager's text
	// viewer -- a page of text per label, the way long text is shown anywhere
	// -- rather than in the editor, which would stop at its own limit and cut
	// the rest of the file off on the next save.
	if (!notes_fits_editor(text)) {
		char shown[NOTES_NAME_MAX + 1];
		notes_display_name(note_name, shown, sizeof(shown));
		bool ok = textview_open_text(shown, text, tr("notes_too_long"), false);
		free(text);
		return ok;
	}

	bool ok = show_note(note_dir, note_name, text);
	free(text);
	return ok;
}

bool noteeditor_new(const char *note_dir, const char *note_name) {
	if (!show_note(note_dir, note_name, "")) {
		return false;
	}
	// A note made to be written: the page opens with the keyboard up and the
	// check in the corner, which is what the naming dialog leads to.
	set_editing(true);
	lv_textarea_set_cursor_pos(field, 0);
	return true;
}

void noteeditor_init(gui_config_t *cfg) {
	lv_obj_add_style(noteeditor_screen, &theme_style_screen, 0);
	title_label = settingsrow_title(noteeditor_screen, cfg, "");
	settingsrow_title_corner_slots(title_label, cfg, 1);

	// While a note is being written the keyboard takes the bottom of the page
	// and the checking key the corner above it, and the text gets everything
	// left. While it is being read there is no keyboard, so the note runs to
	// the panel's own foot and the page reads as a page.
	const int keyboard_h = cfg->screen_width < 320 ? 144 : 316;
	const int content_top = settingsrow_content_top(cfg);
	read_h = cfg->screen_height - content_top - cfg->padding;
	write_h = read_h - keyboard_h;

	field = lv_textarea_create(noteeditor_screen);
	lv_textarea_set_one_line(field, false);
	lv_textarea_set_max_length(field, NOTES_MAX_CHARS);
	lv_textarea_set_placeholder_text(field, tr("notes_placeholder"));
	lv_obj_set_size(field, cfg->screen_width - 2 * cfg->padding, read_h);
	lv_obj_align(field, LV_ALIGN_TOP_LEFT, cfg->padding, content_top);
	lv_obj_add_style(field, &theme_style_card, 0);
	lv_obj_set_style_radius(field, bp_pick(10, 16), 0);
	lv_obj_set_style_border_width(field, 0, 0);
	lv_obj_set_style_shadow_width(field, 0, 0);
	lv_obj_set_style_pad_all(field, bp_pick(8, 14), 0);
	lv_obj_set_style_text_font(field, &font_ui_22, 0);
	lv_obj_set_scrollbar_mode(field, LV_SCROLLBAR_MODE_AUTO);
	keyboard_style_caret(field);
	lv_obj_add_event_cb(field, value_changed_cb, LV_EVENT_VALUE_CHANGED, NULL);

	// The corner button: the pencil while the note is being read, the check
	// while it is being written.
	edit_button = lv_btn_create(noteeditor_screen);
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

	// The accent key, where a keyboard has its return key, starts a line. The
	// keyboard stays hidden until the note is being written: the page opens on
	// a note to be read, and the note keeps the whole page until then.
	keyboard = keyboard_create(noteeditor_screen, cfg->screen_width, keyboard_h, field, &icon_enter, NULL,
							   newline_cb, field);

	// Reading, and nothing else, from the start.
	editing = false;
	keyboard_set_visible(keyboard, false);
	keyboard_show_caret(field, false);
	lv_textarea_set_cursor_click_pos(field, false);

	lv_obj_add_event_cb(noteeditor_screen, unloaded_cb, LV_EVENT_SCREEN_UNLOADED, NULL);

	// Paused until a note is opened: the page is built once, at startup, and a
	// timer that ran from then on would be one more thing every page pays for.
	autosave_timer = lv_timer_create(autosave_cb, AUTOSAVE_MS, NULL);
	lv_timer_pause(autosave_timer);
}
