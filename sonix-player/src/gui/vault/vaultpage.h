#ifndef GUI_VAULT_VAULTPAGE_H
#define GUI_VAULT_VAULTPAGE_H

#include "src/gui/shell/gui.h"

#include "lvgl/lvgl.h"

// The pocket vault, from the More menu.
//
// One screen with three faces. Locked -- or being set up for the first time --
// it is a PIN pad: three columns of digits, a backspace and a check, and a row
// of dots showing how much of the PIN has been typed. Open, it is the list:
// an accent "new password" row first, then one card per entry with its title
// and the password underneath it, an eye to mask each one, and a corner plus
// and menu. Tapping a row (or the plus) opens the editor: a title field above
// a password field, sharing one keyboard, with the check to save and the
// trash to delete. The list's ellipsis carries the vault-wide actions.
//
// Nothing is remembered between visits. Leaving the page -- by the chevron,
// the swipe back, the home gesture or the two-minute idle timer -- writes any
// unsaved entry and throws the keys away, so coming back means typing the PIN
// again. That is the whole feature: no title or password is on the card in a
// form anything but this player with the PIN can read.
//
// The store underneath (src/system/vault) does the cryptography; this page
// only draws it and never handles a key.

extern lv_obj_t *vault_screen;

void vaultpage_init(gui_config_t *cfg);

// Reads the card and opens the page, at its lock pad or its setup pad. An
// action rather than a screen to switch to, for the notes shelf's reason: what
// is on the card decides which face the page has.
void vault_open(void);

#endif /* GUI_VAULT_VAULTPAGE_H */
