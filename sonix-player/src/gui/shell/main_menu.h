#ifndef MAIN_MENU_H
#define MAIN_MENU_H

#include "src/gui/shell/gui.h"

#include "lvgl/lvgl.h"

extern lv_obj_t *main_menu_screen;

void main_menu_init(gui_config_t *cfg);

// Settings -> Appearance: larger, centered icons with no tile captions on the
// main menu only. Off by default; the setter applies and saves the choice.
bool main_menu_icons_only(void);
void main_menu_set_icons_only(bool enabled);

bool main_menu_books(void);
bool main_menu_browse(void);
void main_menu_set_books(bool enabled);
void main_menu_set_browse(bool enabled);

// The six home slots keep their original order even when some are hidden.
// Music/Browse and Audiobooks/Books share slots, but each destination has its
// own saved visibility. This is strictly a UI preference, not a plugin gate.
typedef enum {
	MAIN_MENU_MUSIC = 0,
	MAIN_MENU_STREAMING,
	MAIN_MENU_WIRELESS,
	MAIN_MENU_LIBRARY,
	MAIN_MENU_MORE,
	MAIN_MENU_SETTINGS,
	MAIN_MENU_COUNT,
} main_menu_slot_t;

const char *main_menu_slot_label(main_menu_slot_t slot);
bool main_menu_slot_visible(main_menu_slot_t slot);
// False if the slot is invalid or the choice could not be saved.
bool main_menu_set_slot_visible(main_menu_slot_t slot, bool visible);

#endif
