#include "notes.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "lvgl/lvgl.h"

#include "src/gui/board_profile.h"
#include "src/gui/fonts/fonts.h"
#include "src/gui/notes/noteeditor.h"
#include "src/gui/nowplaying/coverflow.h"
#include "src/gui/nowplaying/player.h"
#include "src/gui/shell/icons.h"
#include "src/gui/shell/keyboard.h"
#include "src/gui/shell/settingsrow.h"
#include "src/gui/shell/switcher.h"
#include "src/gui/shell/theme.h"
#include "src/gui/shell/toast.h"
#include "src/system/core/lang.h"
#include "src/system/notes/notesstore.h"

lv_obj_t *notes_screen;

static gui_config_t *config;
static lv_obj_t *list;
static lv_obj_t *new_row_label; // repainted when the accent changes

// The folder as of the last read, and the notes it holds. Static rather than
// allocated: a page that is entered over and over should not be a page that
// allocates over and over, and 200 rows is the whole of what it can show.
static char dir[1024];
static note_entry_t entries[NOTES_MAX_ENTRIES];
static int entry_count;
static bool more_than_shown;

// The naming dialog: the same full-screen layer with the shared keyboard that
// a new playlist is named in.
static lv_obj_t *name_layer;
static lv_obj_t *name_field;
static keyboard_t *name_keyboard;

// ---------------------------------------------------------------------------
// Opening a note
// ---------------------------------------------------------------------------

// The release of a swipe is not a tap: LVGL sends CLICKED to whatever the
// finger was on when it lifted. Same guard the file manager and the library
// lists use.
static bool gesture_not_tap(void) {
	return player_sheet_drag_active() || switcher_back_drag_active() || coverflow_drag_active();
}

static void row_clicked_cb(lv_event_t *e) {
	if (gesture_not_tap()) {
		return;
	}
	int index = (int)(intptr_t)lv_event_get_user_data(e);
	if (index < 0 || index >= entry_count) {
		return;
	}
	noteeditor_open(dir, entries[index].name);
}

// ---------------------------------------------------------------------------
// The listing
// ---------------------------------------------------------------------------

static void new_note_cb(lv_event_t *e);

static void rebuild(void) {
	lv_obj_clean(list);
	entry_count = notes_list(dir, entries, NOTES_MAX_ENTRIES);
	more_than_shown = entry_count == NOTES_MAX_ENTRIES;

	// The way to write something comes first, as the new-playlist row does on
	// the playlists page: it is the one action the page has.
	lv_obj_t *row = settingsrow_add(list, "", NULL, new_note_cb, NULL);
	new_row_label = settingsrow_name_label(row);
	lv_label_set_text(new_row_label, tr("notes_new"));
	lv_obj_set_style_text_color(new_row_label, theme()->accent, 0);
	settingsrow_add_icon(row, &icon_plus, THEME_SEMANTIC_BLUE);

	for (int i = 0; i < entry_count; i++) {
		char shown[NOTES_NAME_MAX + 1];
		notes_display_name(entries[i].name, shown, sizeof(shown));
		lv_obj_t *note_row = settingsrow_add(list, "", NULL, row_clicked_cb, (void *)(intptr_t)i);
		lv_label_set_text(settingsrow_name_label(note_row), shown);
		// The name is data, not a tag: it is drawn as it stands, so a note
		// called "cancel" is not translated into the button it names.
		settingsrow_name_lines(note_row, 1);
		// The same violet the More menu gives this page's own entry.
		settingsrow_add_icon(note_row, &icon_notes, THEME_SEMANTIC_PURPLE);
	}

	if (entry_count == 0) {
		lv_obj_t *empty = lv_label_create(list);
		lv_label_set_text(empty, tr("notes_empty"));
		lv_obj_set_style_text_align(empty, LV_TEXT_ALIGN_CENTER, 0);
		lv_obj_set_width(empty, lv_pct(100));
		lv_obj_add_style(empty, &theme_style_text_dim, 0);
		lv_obj_set_style_text_font(empty, &font_ui_22, 0);
		lv_obj_set_style_pad_top(empty, bp_pick(24, 60), 0);
	} else if (more_than_shown) {
		lv_obj_t *more = lv_label_create(list);
		lv_label_set_text(more, tr("notes_more"));
		lv_obj_set_style_text_align(more, LV_TEXT_ALIGN_CENTER, 0);
		lv_obj_set_width(more, lv_pct(100));
		lv_obj_add_style(more, &theme_style_text_dim, 0);
		lv_obj_set_style_text_font(more, &font_ui_18, 0);
	}
	lv_obj_scroll_to_y(list, 0, LV_ANIM_OFF);
}

// The accent is hand-set on the new-note row's label, so it has to be set
// again after a theme or accent change.
static void notes_refresh_theme(void) {
	if (new_row_label) {
		lv_obj_set_style_text_color(new_row_label, theme()->accent, 0);
	}
}

static void loaded_cb(lv_event_t *e) {
	(void)e;
	rebuild();
}

// ---------------------------------------------------------------------------
// The new-note dialog
// ---------------------------------------------------------------------------

static void name_layer_hide(void) {
	lv_obj_set_hidden(name_layer, true);
	keyboard_set_visible(name_keyboard, false);
}

static void name_cancel_cb(lv_event_t *e) {
	(void)e;
	name_layer_hide();
}

// The dialog's accept key: make the note, then open it. Writing it here and
// not in the editor is what makes a note appear in the list the moment it is
// made, even if nothing is ever typed into it.
static void name_accept_cb(lv_event_t *e) {
	(void)e;
	const char *typed = lv_textarea_get_text(name_field);
	if (!notes_name_ok(typed)) {
		toast_error(tr("name_required"));
		return;
	}

	char file[NOTES_NAME_MAX + 8];
	if (!notes_file_name(typed, file, sizeof(file))) {
		toast_error(tr("notes_cannot_save"));
		return;
	}
	if (!notes_write(dir, file, "")) {
		toast_error(tr("notes_cannot_save"));
		return;
	}

	name_layer_hide();
	// The one note that opens to be written rather than read: it was made a
	// moment ago, and the person who made it is about to type into it.
	noteeditor_new(dir, file);
}

static void name_layer_show(void) {
	char suggested[NOTES_NAME_MAX + 1];
	notes_default_name(dir, suggested, sizeof(suggested));
	lv_textarea_set_text(name_field, suggested);
	lv_obj_set_hidden(name_layer, false);
	keyboard_reset(name_keyboard);
	keyboard_set_visible(name_keyboard, true);
	lv_obj_scroll_to_y(name_field, 0, LV_ANIM_OFF);
}

static void new_note_cb(lv_event_t *e) {
	if (gesture_not_tap()) {
		return;
	}
	(void)e;
	name_layer_show();
}

// The chevron closes the dialog rather than leaving the page, the way it does
// on every other page that keeps a dialog of its own. The peek answers the
// same question for the swipe-back's preview.
static bool back_guard(void) {
	if (!lv_obj_is_hidden(name_layer)) {
		name_layer_hide();
		return true;
	}
	return false;
}

static bool back_guard_peek(void) { return !lv_obj_is_hidden(name_layer); }

// ---------------------------------------------------------------------------
// Building
// ---------------------------------------------------------------------------

static void build_name_dialog(gui_config_t *cfg) {
	name_layer = lv_obj_create(notes_screen);
	lv_obj_set_size(name_layer, cfg->screen_width, cfg->screen_height);
	lv_obj_align(name_layer, LV_ALIGN_TOP_LEFT, 0, 0);
	lv_obj_add_style(name_layer, &theme_style_screen, 0);
	lv_obj_set_style_bg_opa(name_layer, LV_OPA_COVER, 0);
	lv_obj_set_style_border_width(name_layer, 0, 0);
	lv_obj_set_style_radius(name_layer, 0, 0);
	lv_obj_set_style_pad_all(name_layer, 0, 0);
	lv_obj_set_scrollable(name_layer, false);
	lv_obj_set_hidden(name_layer, true);

	lv_obj_t *heading = lv_label_create(name_layer);
	lv_label_set_text(heading, tr("notes_new"));
	lv_obj_add_style(heading, &theme_style_text, 0);
	lv_obj_set_style_text_font(heading, &font_ui_24, 0);
	// Offset clear of the floating back chevron, which sits on top of this
	// layer: the same offset every page title uses.
	lv_obj_align(heading, LV_ALIGN_TOP_LEFT, cfg->padding + 56 + 14, settingsrow_header_top(cfg, &font_ui_24));
	settingsrow_register_header(heading, cfg);

	lv_obj_t *cancel = lv_btn_create(name_layer);
	settingsrow_place_corner_button(cancel, cfg, 0);
	lv_obj_set_style_bg_opa(cancel, LV_OPA_TRANSP, 0);
	lv_obj_set_style_border_width(cancel, 0, 0);
	lv_obj_set_style_shadow_width(cancel, 0, 0);
	lv_obj_set_style_pad_all(cancel, 0, 0);
	lv_obj_add_event_cb(cancel, name_cancel_cb, LV_EVENT_CLICKED, NULL);

	lv_obj_t *cancel_icon = lv_image_create(cancel);
	lv_image_set_src(cancel_icon, &icon_close);
	lv_obj_add_style(cancel_icon, &theme_style_icon, 0);
	settingsrow_scale_corner_icon(cancel_icon, cfg);
	lv_obj_center(cancel_icon);

	name_field = lv_textarea_create(name_layer);
	lv_textarea_set_one_line(name_field, true);
	lv_textarea_set_max_length(name_field, NOTES_NAME_MAX);
	lv_textarea_set_placeholder_text(name_field, tr("name"));
	lv_obj_set_size(name_field, cfg->screen_width - 2 * cfg->padding, bp_pick(48, 62));
	lv_obj_set_scrollbar_mode(name_field, LV_SCROLLBAR_MODE_OFF);
	lv_obj_align(name_field, LV_ALIGN_TOP_LEFT, cfg->padding, cfg->padding + cfg->top_bar_height + bp_pick(46, 60));
	lv_obj_add_style(name_field, &theme_style_card, 0);
	lv_obj_set_style_radius(name_field, bp_pick(10, 12), 0);
	lv_obj_set_style_border_width(name_field, 0, 0);
	lv_obj_set_style_shadow_width(name_field, 0, 0);
	lv_obj_set_style_pad_all(name_field, bp_pick(10, 14), 0);
	lv_obj_set_style_text_font(name_field, &font_ui_24, 0);
	keyboard_style_caret(name_field);

	name_keyboard = keyboard_create(name_layer, cfg->screen_width, cfg->screen_width < 320 ? 144 : 316, name_field, NULL,
									"create", name_accept_cb, NULL);
}

void notes_open(void) {
	if (!config || !notes_dir(dir, sizeof(dir), config->sd_root_path)) {
		gui_notify_no_card();
		return;
	}
	if (!gui_card_available()) {
		gui_notify_no_card();
		return;
	}
	name_layer_hide();
	switch_screen(notes_screen);
}

void notes_init(gui_config_t *cfg) {
	config = cfg;
	list = settingsrow_page(notes_screen, cfg, "notes");
	settingsrow_title_corner_slots(settingsrow_page_title(notes_screen), cfg, 0);
	player_sheet_attach_drag(list, true);

	build_name_dialog(cfg);

	lv_obj_add_event_cb(notes_screen, loaded_cb, LV_EVENT_SCREEN_LOADED, NULL);
	switcher_set_back_guard(notes_screen, back_guard);
	switcher_set_back_guard_peek(notes_screen, back_guard_peek);
	theme_register_refresh(notes_refresh_theme);
}
