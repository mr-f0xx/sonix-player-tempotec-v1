#include "themefilepage.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "src/gui/board_profile.h"
#include "src/gui/fonts/fonts.h"
#include "src/gui/shell/icons.h"
#include "src/gui/shell/settingsrow.h"
#include "src/gui/shell/theme.h"
#include "src/gui/shell/toast.h"
#include "src/system/core/lang.h"

lv_obj_t *themefilepage_screen;

// The same bound theme_custom_files() is called with; the page scrolls.
#define MAX_FILES 24

// The same green the settings rows use to mark a page whose feature is on.
#define CHECK_GREEN lv_color_make(46, 194, 126)

typedef struct {
	char name[256];  // the file name; what theme_select_custom_file wants
	lv_obj_t *mark;  // the tick, in the place a chevron would be
} entry_t;

static entry_t entries[MAX_FILES];
static int entry_count;
static lv_obj_t *list;	   // the column the rows go into
static lv_obj_t *empty_note; // what the page says when the folder is bare

// Marks the file the Custom theme wears, and only it. The tick follows
// theme_custom_file() rather than the selected kind: the row names the file
// in force whether or not Custom is the theme on screen, exactly as the
// Appearance value does.
static void paint_checks(void) {
	const char *chosen = theme_custom_file();
	for (int i = 0; i < entry_count; i++) {
		if (!entries[i].mark) {
			continue;
		}
		lv_obj_set_hidden(entries[i].mark, strcmp(chosen, entries[i].name) != 0);
	}
}

static void pick_cb(lv_event_t *e) {
	int index = (int)(intptr_t)lv_event_get_user_data(e);
	if (index < 0 || index >= entry_count) {
		return;
	}
	if (!theme_select_custom_file(entries[index].name)) {
		// The file went with a card that was pulled out, or it no longer
		// parses. Nothing changes.
		toast_error("appearance_custom_invalid");
		return;
	}
	// Choosing a file is choosing Custom, as choosing it from the pills was.
	theme_set_kind(THEME_KIND_CUSTOM);
	paint_checks();
}

static lv_obj_t *add_row(const char *label, int index) {
	lv_obj_t *row = settingsrow_action(list, label, pick_cb, (void *)(intptr_t)index);
	settingsrow_name_lines(row, 2);

	lv_obj_t *mark = lv_image_create(row);
	lv_image_set_src(mark, &icon_check);
	lv_obj_set_style_image_recolor(mark, CHECK_GREEN, 0);
	lv_obj_set_style_image_recolor_opa(mark, LV_OPA_COVER, 0);
	lv_obj_align(mark, LV_ALIGN_RIGHT_MID, 0, 0);
	lv_obj_set_hidden(mark, true);
	return mark;
}

// Reads the folder again on every visit: a file copied over the USB cable
// while the player was running is offered the next time the page comes up,
// and one whose card has been pulled out is gone from the list.
static void rebuild(void) {
	if (!list) {
		return;
	}
	lv_obj_clean(list);
	entry_count = 0;

	char names[MAX_FILES][256];
	int found = theme_custom_files(names, MAX_FILES);
	for (int i = 0; i < found && entry_count < MAX_FILES; i++) {
		// The filename without its .ini, under its theme name where the
		// shipped palettes have one: catppuccin-mocha.ini lists as
		// "Catppuccin", gruvbox.ini as "Gruvbox".
		char label[64];
		theme_item_label(names[i], label, sizeof(label));
		snprintf(entries[entry_count].name, sizeof(entries[0].name), "%.255s", names[i]);
		entries[entry_count].mark = add_row(label, entry_count);
		entry_count++;
	}

	if (empty_note) {
		lv_obj_set_hidden(empty_note, entry_count > 0);
	}
	paint_checks();
}

static void screen_loaded_cb(lv_event_t *e) {
	(void)e;
	rebuild();
}

void themefilepage_init(gui_config_t *cfg) {
	lv_obj_t *container = settingsrow_page(themefilepage_screen, cfg, "appearance_custom_theme");

	// Keep the custom.ini editing hint with the list it describes.
	lv_obj_t *note = lv_label_create(container);
	lv_label_set_long_mode(note, LV_LABEL_LONG_WRAP);
	lv_obj_set_width(note, lv_pct(100));
	lv_obj_add_style(note, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(note, &font_ui_22, 0);
	lv_label_set_text(note, tr("appearance_custom_note"));

	// With no themes on the card, this prompt sits above the empty list.
	empty_note = lv_label_create(container);
	lv_label_set_long_mode(empty_note, LV_LABEL_LONG_WRAP);
	lv_obj_set_width(empty_note, lv_pct(100));
	lv_obj_add_style(empty_note, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(empty_note, &font_ui_22, 0);
	lv_label_set_text(empty_note, tr("appearance_custom_theme_empty"));

	list = lv_obj_create(container);
	lv_obj_set_width(list, lv_pct(100));
	lv_obj_set_height(list, LV_SIZE_CONTENT);
	lv_obj_set_style_bg_opa(list, 0, 0);
	lv_obj_set_style_border_width(list, 0, 0);
	lv_obj_set_style_pad_all(list, 0, 0);
	lv_obj_set_style_pad_row(list, bp_is_tempotec_v1() ? 6 : 12, 0);
	lv_obj_set_scrollable(list, false);
	lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_OFF);
	lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

	rebuild();

	lv_obj_add_event_cb(themefilepage_screen, screen_loaded_cb, LV_EVENT_SCREEN_LOADED, NULL);
	theme_register_refresh(paint_checks);
}
