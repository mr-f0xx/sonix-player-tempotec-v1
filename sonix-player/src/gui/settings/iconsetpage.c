#include "iconsetpage.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "src/gui/board_profile.h"
#include "src/gui/fonts/fonts.h"
#include "src/gui/shell/iconset.h"
#include "src/gui/shell/icons.h"
#include "src/gui/shell/settingsrow.h"
#include "src/gui/shell/theme.h"
#include "src/gui/shell/toast.h"
#include "src/system/core/config.h"
#include "src/system/core/lang.h"

lv_obj_t *iconsetpage_screen;

// Default plus every folder the card holds; the page scrolls past that.
#define MAX_SETS 16
#define NAME_COPY 64

// The same green the settings rows use to mark a page whose feature is on.
#define CHECK_GREEN lv_color_make(46, 194, 126)

typedef struct {
	char name[NAME_COPY]; // the folder name, "" for Default; what iconset_select wants
	lv_obj_t *mark;	   // the tick, in the place a chevron would be
} entry_t;

static entry_t entries[MAX_SETS];
static int entry_count;
static lv_obj_t *list; // the column the rows go into

// Marks the set in force, and only it. Default's name is the empty string,
// which is what iconset_active_name() reports with no set loaded, so the
// absence of a set ticks the same row as any other choice.
static void paint_checks(void) {
	const char *active = iconset_active_name();
	for (int i = 0; i < entry_count; i++) {
		if (!entries[i].mark) {
			continue;
		}
		lv_obj_set_hidden(entries[i].mark, strcmp(entries[i].name, active) != 0);
	}
}

// A row that marks itself rather than leading somewhere: the tick goes where
// a chevron would be, so every set sits on the same grid. The label is the
// folder name made readable (theme_item_label); settingsrow tr()s whatever
// it is given and passes unknown text through unchanged.
static void pick_cb(lv_event_t *e) {
	int index = (int)(intptr_t)lv_event_get_user_data(e);
	if (index < 0 || index >= entry_count) {
		return;
	}
	if (!iconset_select(entries[index].name)) {
		// The folder went with a card that was pulled out, or the set
		// shipped no icons the firmware knows. Nothing changes.
		toast_error("appearance_icon_set_failed");
		return;
	}
	config_set("ui", "icon_set", entries[index].name);
	config_save();
	// The pixels behind every screen on the device just changed.
	theme_repaint();
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

// Reads the folder again on every visit: a set copied over the USB cable
// while the player was running is offered the next time the page comes up,
// and one whose card has been pulled out is gone from the list.
static void rebuild(void) {
	if (!list) {
		return;
	}
	lv_obj_clean(list);
	entry_count = 0;

	// Default first: the built-in glyphs, always on offer whatever the
	// card holds. Its label is a translation key; the folders' are names.
	entries[0].name[0] = '\0';
	entries[0].mark = add_row("appearance_icon_set_default", 0);
	entry_count = 1;

	const char *sets[MAX_SETS - 1];
	int folders = iconset_choices(sets, MAX_SETS - 1);
	for (int i = 0; i < folders && entry_count < MAX_SETS; i++) {
		char label[NAME_COPY];
		theme_item_label(sets[i], label, sizeof(label));
		snprintf(entries[entry_count].name, sizeof(entries[0].name), "%s", sets[i]);
		entries[entry_count].mark = add_row(label, entry_count);
		entry_count++;
	}

	paint_checks();
}

static void screen_loaded_cb(lv_event_t *e) {
	(void)e;
	rebuild();
}

void iconsetpage_init(gui_config_t *cfg) {
	lv_obj_t *container = settingsrow_page(iconsetpage_screen, cfg, "appearance_icon_set");

	lv_obj_t *note = lv_label_create(container);
	lv_label_set_long_mode(note, LV_LABEL_LONG_WRAP);
	lv_obj_set_width(note, lv_pct(100));
	lv_obj_add_style(note, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(note, &font_ui_22, 0);
	lv_label_set_text(note, tr("appearance_icon_set_note"));

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

	lv_obj_add_event_cb(iconsetpage_screen, screen_loaded_cb, LV_EVENT_SCREEN_LOADED, NULL);
	theme_register_refresh(paint_checks);
}
