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
// is on screen, and the creature's slow breathing, its tail and, for a fish,
// the bubbles of its aquarium run while the page is up -- faster, never fast,
// while something is playing.

extern lv_obj_t *petpage_screen;

void petpage_init(gui_config_t *cfg);

// Draws the page if it has never been drawn and brings it up, repainted from
// the live state.
void petpage_open(void);

// The animal itself, for the one thing a page cannot ask a label for: the tap
// that gives it a pat. Null until the page has been built.
lv_obj_t *petpage_creature(void);

// The keys a page cannot see. Volume and track keys are handled by the system
// layer, not by the interface, so the system layer tells the pet that one was
// pressed and the pet answers with a jump, a duck or a lean. A no-op when the
// page is not on screen: nothing is drawn that nobody is looking at.
typedef enum {
	PET_POKE_VOLUME_UP = 0,
	PET_POKE_VOLUME_DOWN,
	PET_POKE_NEXT,
	PET_POKE_PREV,
} pet_poke_t;

void petpage_poke(pet_poke_t what);

#endif /* GUI_PET_PETPAGE_H */
