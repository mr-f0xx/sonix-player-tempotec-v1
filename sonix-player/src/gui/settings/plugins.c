#include "plugins.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "lvgl/lvgl.h"

#include "src/gui/shell/settingsrow.h"
#include "src/gui/shell/switcher.h"
#include "src/gui/shell/theme.h"
#include "src/gui/shell/toast.h"
#include "src/gui/nowplaying/player.h"
#include "src/system/addons/addons.h"
#include "src/system/core/config.h"
#include "src/system/core/lang.h"

lv_obj_t *plugins_screen;

// One row's switch. The module id is kept on the switch itself (see
// plugins_init), so this one callback serves every row. The module is switched
// in the config here and now; what is already built stays built until the next
// start, which is what the toast says rather than pretending the memory has
// been handed back.
static void addon_switch_cb(lv_event_t *e) {
	lv_obj_t *sw = lv_event_get_target(e);
	const char *id = lv_obj_get_user_data(sw);
	if (!id) {
		return;
	}
	bool on = lv_obj_has_state(sw, LV_STATE_CHECKED);
	if (!addons_set(id, on)) {
		if (on) {
			lv_obj_remove_state(sw, LV_STATE_CHECKED);
		} else {
			lv_obj_add_state(sw, LV_STATE_CHECKED);
		}
		toast_error(tr("addon_save_failed"));
		return;
	}
	toast_success(tr("addon_restart_needed"));
}

// A heading over a group of modules. The fork's rows have no heading of their
// own, so this is a dimmed label in the same column as the rows.
static void section_heading(lv_obj_t *list, const char *key) {
	lv_obj_t *label = lv_label_create(list);
	lv_label_set_text(label, tr(key));
	lv_obj_add_style(label, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(label, &font_ui_14, 0);
	lv_obj_set_width(label, lv_pct(100));
	lv_obj_set_style_pad_top(label, 14, 0);
	lv_obj_set_style_pad_left(label, 6, 0);
	lv_obj_set_style_pad_bottom(label, 2, 0);
}

// The page's explanatory line, under the headings that need one.
static void section_note(lv_obj_t *list, const char *key, int font_size) {
	lv_obj_t *label = lv_label_create(list);
	lv_label_set_text(label, tr(key));
	lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
	lv_obj_add_style(label, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(label, font_size == 22 ? &font_ui_22 : &font_ui_14, 0);
	lv_obj_set_width(label, lv_pct(100));
	lv_obj_set_style_pad_left(label, 6, 0);
	lv_obj_set_style_pad_bottom(label, 6, 0);
}

void plugins_init(gui_config_t *cfg) {
	plugins_screen = lv_obj_create(NULL);
	lv_obj_add_style(plugins_screen, &theme_style_screen, 0);
	lv_obj_t *list = settingsrow_page(plugins_screen, cfg, "plugins");

	section_note(list, "plugins_note", 14);

	// One heading per group of modules, taken from the module list itself: the
	// page never has to know which id belongs where, so a module added to
	// addons.c lands under the right heading by itself.
	const char *section = NULL;
	for (int i = 0; i < addons_count(); i++) {
		const addon_t *addon = addons_at(i);
		if (!addon) {
			continue;
		}
		if (!section || strcmp(section, addon->section) != 0) {
			section = addon->section;
			section_heading(list, section);
		}
		lv_obj_t *sw = NULL;
		// A toggle card carries no caption of its own (the fork's rows do not
		// resize one), so the module's note sits under the card, as the other
		// toggle pages do with their notes.
		settingsrow_toggle(list, addon->label, &sw, addon_switch_cb);
		section_note(list, addon->note, 22);
		if (!sw) {
			continue;
		}
		lv_obj_set_user_data(sw, (void *)addon->id);
		if (addons_enabled(addon->id)) {
			lv_obj_add_state(sw, LV_STATE_CHECKED);
		}
	}

	player_sheet_attach_drag(list, true);
	switcher_attach_back_gesture(plugins_screen);
}
