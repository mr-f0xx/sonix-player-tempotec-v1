#ifndef GUI_PET_PETPAGE_H
#define GUI_PET_PETPAGE_H

#include "src/gui/shell/gui.h"

#include "lvgl/lvgl.h"

// The pet, from the More menu.
//
// The creature, its mood and the two meters are drawn with the interface's own
// pieces -- round objects, one arc for the mouth, a label for the snore -- so
// the page adds no art to the image and no space to the card. What moves it is
// the listening, and the arithmetic behind that is in src/system/pet/pet.c;
// the once-a-second reading of the playback state is petwatch.c. This file only
// draws what those two answer.
//
// Nothing is built at start-up: petpage_init() remembers the panel, and the
// first visit draws the page. The page's one-second timer exists only while it
// is on screen, and the bobbing and the drifting notes only run while something
// is playing -- a pet alone on a paused player is a still picture.

extern lv_obj_t *petpage_screen;

void petpage_init(gui_config_t *cfg);

// Draws the page if it has never been drawn and brings it up, repainted from
// the live state.
void petpage_open(void);

#endif /* GUI_PET_PETPAGE_H */
