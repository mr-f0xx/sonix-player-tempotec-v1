#ifndef GUI_PET_PETPAGE_H
#define GUI_PET_PETPAGE_H

#include "src/gui/shell/gui.h"

#include "lvgl/lvgl.h"

// The pet, from the More menu.
//
// The creature is one picture, a Pikachu (pikachu.c), put on the card at its own
// size and never scaled. Its mood, what it wears and the surprised face it makes
// while it is fed are a few cells drawn over that picture on the picture's own
// grid; the two meters and the notes are the interface's own pieces. What moves it is the listening, and the arithmetic
// behind that is in src/system/pet/pet.c; the once-a-second reading of the
// playback state is petwatch.c. This file only draws what those two answer.
//
// Nothing is built at start-up: petpage_init() remembers the panel, and the
// first visit draws the page. The page's one-second timer exists only while it
// is on screen, and the creature's slow breathing runs while the page is up --
// faster, never fast, while something is playing, when it also steps from side
// to side. It blinks every few seconds and fidgets now and then.

extern lv_obj_t *petpage_screen;

void petpage_init(gui_config_t *cfg);

// Draws the page if it has never been drawn and brings it up, repainted from
// the live state.
void petpage_open(void);

// The animal itself, for the one thing a page cannot ask a label for: the tap
// that gives it a pat. Null until the page has been built.
lv_obj_t *petpage_creature(void);

// The Poke Ball on the floor, and whatever it has turned up beside it. Both are
// on the animal's own card and both are tapped in place; null until the page
// has been built.
lv_obj_t *petpage_box(void);
lv_obj_t *petpage_find(void);

// Whether his eyes are shut this moment: a blink lasts a tenth of a second, so
// the tests have to ask. False until the page has been built.
bool petpage_blinking(void);

// The route behind the animal, one object drawn in three-pixel cells from its
// own top left corner. The tests read the sky, the ground and the sun back off
// the screen through it; null until the page has been built.
lv_obj_t *petpage_scenery(void);

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
