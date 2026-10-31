#ifndef SYSTEM_PET_PETWATCH_H
#define SYSTEM_PET_PETWATCH_H

#include <stdbool.h>
#include <stdint.h>

#include "src/system/pet/pet.h"

// The one-second bridge between the player's playback and the pet.
//
// There is no process, no thread and no sound of its own here: the player
// already runs a timer for the clock in its status bar, and this is one more
// LVGL timer on the same interface thread. Once a second it asks the audio
// engine whether anything is playing -- local files, the radio, or a phone
// sending Bluetooth -- and how loud; petlisten.c decides whether that second
// was audible listening, and pet_advance() turns it into XP and mood. That is
// the whole cost: a handful of reads, a little integer arithmetic, and a write
// to the settings file every two minutes of music rather than every second.
//
// It runs whether or not the pet's page is open, which is the point: the pet
// has to hear the music the user left playing with the screen off. The page's
// own timer is separate and only exists while the page is on screen.
//
// What it refuses to pay for is the other half of the same rule: paused audio,
// a muted output, a volume below PET_VOLUME_FLOOR, a timer callback that slept
// through the music, and listening past the day's ceiling all earn nothing.
// That is what makes a player left running overnight worth no more than four
// honest hours.

// Reads the pet out of the settings, applies the offline time since the last
// session, and starts the timer. Call once, after the configuration and the
// clock are up (main.c, before gui_init()).
void petwatch_init(void);

// The live state, for the page. Valid for as long as the player runs.
const pet_state_t *petwatch_state(void);

// Whether the last tick heard music it could also be heard by the user.
// What the page animates along to, and what tells its status line "feeding on
// ..." from "play something".
bool petwatch_listening(void);

// The button. Applies the cooldown and the small gain, and leaves the saving to
// the next tick. False when the pet has had enough for the minute.
bool petwatch_pet_now(void);

// PET_EVENT_* bits collected since the last call, cleared by it. The page takes
// them when it is shown, so a goal reached with the page closed is still worth
// a toast the next time it is opened.
int petwatch_take_events(void);

// The last achievement unlocked since the page last asked, as one PET_ACH_* bit
// (0 when there is none), cleared by the call. It is separate from the events
// because each achievement has its own name to toast.
int petwatch_take_achievement(void);

// The vibe of the track being heard, from its genre: what the page changes its
// animation to. NEUTRAL when nothing is playing or the tags say nothing.
pet_vibe_t petwatch_vibe(void);

// True while the current listening session has passed PET_FOCUS_SECS without a
// pause, a mute or a skip. The page uses it for its line; the achievement is
// awarded by the watcher.
bool petwatch_focus(void);

// The owner's choices. `species` is one of the four animals, or NONE to clear
// it (which puts the chooser back); the name is written straight to the
// settings, trimmed, and an empty one restores the default. Both save.
bool petwatch_set_species(pet_species_t species);
bool petwatch_set_name(const char *name);

// The month's listening, as the page and the game read it: the temperament the
// last thirty days earned (NONE before there are three days of it), whether
// that temperament unlocked the drifting soundscape, and whether today's
// tap-along is still unplayed.
pet_temperament_t petwatch_temperament(void);
bool petwatch_has_soundscape(void);
bool petwatch_rhythm_ready(void);

// Pays for a finished tap-along: the score is 0 to 100, and the return is the
// notes earned (0 when today has already been paid for). Saves.
int petwatch_rhythm_finish(int score);

// The wardrobe and the wallet, both of which change the live state: buy an item
// (notes are checked in pet_buy()), or put an owned outfit/room on. Both write
// the settings only when they did something.
bool petwatch_buy(pet_item_t item);
bool petwatch_wear(pet_item_t item);

// Writes the state out now. Called before the player powers off (a clean
// shutdown must not lose the last couple of minutes of music), when the pet's
// page is left, and after a purchase.
void petwatch_flush(void);

// What the pet is feeding on, for the page's status line: the track's title, a
// radio station's name, or "" when the source cannot name it. Filled on every
// tick, so the page only copies a string.
void petwatch_track_title(char *out, size_t out_size);

// The wall clock as the watcher reads it, for a page that wants it without
// asking time() itself.
int64_t petwatch_now(void);

#endif /* SYSTEM_PET_PETWATCH_H */
