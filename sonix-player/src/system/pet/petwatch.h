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
// sending Bluetooth -- and hands that second to pet_advance(). That is the whole
// cost: a handful of atomic reads, a little integer arithmetic, and a write to
// the settings file every two minutes of music rather than every second.
//
// It runs whether or not the pet's page is open, which is the point: the pet
// has to hear the music the user left playing with the screen off. The page's
// own timer is separate and only exists while the page is on screen.

// Reads the pet out of the settings, applies the offline time since the last
// session, and starts the timer. Call once, after the configuration and the
// clock are up (main.c, before gui_init()).
void petwatch_init(void);

// The live state, for the page. Valid for as long as the player runs.
const pet_state_t *petwatch_state(void);

// Whether the last tick heard music. What the page animates along to, and what
// tells its status line "feeding on ..." from "play something".
bool petwatch_listening(void);

// The button. Applies the cooldown and the small gain, and leaves the saving to
// the next tick. False when the pet has had enough for the minute.
bool petwatch_pet_now(void);

// PET_EVENT_* bits collected since the last call, cleared by it. The page takes
// them when it is shown, so a goal reached with the page closed is still worth
// a toast the next time it is opened.
int petwatch_take_events(void);

// Writes the state out now. Called before the player powers off (a clean
// shutdown must not lose the last couple of minutes of music) and when the
// pet's page is left.
void petwatch_flush(void);

// What the pet is feeding on, for the page's status line: the track's title, a
// radio station's name, or "" when the source cannot name it. Filled on every
// tick, so the page only copies a string.
void petwatch_track_title(char *out, size_t out_size);

// The wall clock as the watcher reads it, for a page that wants it without
// asking time() itself.
int64_t petwatch_now(void);

#endif /* SYSTEM_PET_PETWATCH_H */
