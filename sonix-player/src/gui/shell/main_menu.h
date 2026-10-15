#ifndef MAIN_MENU_H
#define MAIN_MENU_H

#include "src/gui/shell/gui.h"

#include "lvgl/lvgl.h"

extern lv_obj_t *main_menu_screen;

void main_menu_init(gui_config_t *cfg);

// Stable identities, not tile positions. The home screen shows up to six of
// these in this order; an older config with no selection keeps the original
// six tiles. Settings cannot be hidden, so Appearance is always reachable.
typedef enum {
	MAIN_MENU_MUSIC,
	MAIN_MENU_STREAMING,
	MAIN_MENU_WIRELESS,
	MAIN_MENU_AUDIOBOOKS,
	MAIN_MENU_BOOKS,
	MAIN_MENU_DAC,
	MAIN_MENU_GEARBOY,
	MAIN_MENU_FILE_EXPLORER,
	MAIN_MENU_FLAPPY_BIRD,
	MAIN_MENU_MORE,
	MAIN_MENU_SETTINGS,
	MAIN_MENU_BUTTON_COUNT,
} main_menu_button_t;

#define MAIN_MENU_MAX_TILES 6

bool main_menu_button_available(main_menu_button_t button);
const char *main_menu_button_label(main_menu_button_t button); // translation key, or NULL
bool main_menu_button_enabled(main_menu_button_t button);
// Returns false when a seventh tile is requested, or for an unavailable tile
// or an attempt to hide Settings. Changes apply to the home screen and save.
bool main_menu_set_button_enabled(main_menu_button_t button, bool enabled);

// Settings -> Appearance: larger, centered icons with no tile captions on the
// main menu only. Off by default; the setter applies and saves the choice.
bool main_menu_icons_only(void);
void main_menu_set_icons_only(bool enabled);

#endif
