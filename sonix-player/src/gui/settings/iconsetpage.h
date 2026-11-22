#ifndef ICONSETPAGE_H
#define ICONSETPAGE_H

#include "src/gui/shell/gui.h"

// The "Icon set" page under Settings > Appearance: Default plus every folder
// in Icons/ on the card, and which one the whole interface draws with.
//
// A page rather than a row of pills in Appearance because the folders are
// card content: they are read on the way in, so a set copied over while the
// player was running is offered without anything else moving, and a list of
// them does not crowd the theme row the way pills did the moment there was
// more than a couple.
extern lv_obj_t *iconsetpage_screen;

void iconsetpage_init(gui_config_t *cfg);

#endif // ICONSETPAGE_H
