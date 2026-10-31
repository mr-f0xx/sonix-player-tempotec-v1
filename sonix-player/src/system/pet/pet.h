#ifndef SYSTEM_PET_PET_H
#define SYSTEM_PET_PET_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// The music pet: a small creature that lives on how much music is played.
//
// This is the whole of it that can be wrong quietly -- how the mood moves with
// the listening, how it falls when nothing is played, when it grows -- and it is
// plain arithmetic on purpose. No LVGL, no audio, no clock: a caller says how
// much time has passed and how much of it was music, and gets the new state
// back. petwatch.c is the caller that reads the player's own playback state on
// a one-second timer; petpage.c only draws what this returns.
//
// The numbers are small and whole. Happiness and energy are percentages; the
// stage comes from the lifetime seconds of music; the streak counts days on
// which the daily goal was reached. Nothing here ever kills the pet: a player
// left in a drawer for a month comes back to a sad, sleepy creature that a
// couple of albums will cheer up again. That is a decision, not an oversight --
// a music player should not punish somebody for listening somewhere else for a
// while.
//
// The clock is not trusted. `now` is seconds since the epoch as the caller reads
// it, and a number that goes backwards (the clock is set for the first time, or
// corrected over Wi-Fi) is treated as no time having passed rather than as
// negative listening. Offline time -- the device switched off between two
// sessions -- is the difference between the stored `last_seen` and now, capped
// so that a device left alone for a year comes back to a sad pet rather than to
// arithmetic that has run off the end.

#define PET_HAPPY_MAX 100
#define PET_ENERGY_MAX 100

// The daily goal, in seconds of music: twenty minutes. Meeting it keeps the
// streak alive; the goal is a reason to put something on, not a chore.
#define PET_DAY_GOAL_SECS (20 * 60)

// Listening that fills each meter. Happiness moves slowly -- a track is worth a
// few percent -- and energy quickly, because energy is what a recent session
// leaves behind and what drains while the player is idle. A hungry pet (no
// energy left) earns happiness at half rate until the same music has fed it.
#define PET_HAPPY_SECS_PER_POINT 20
#define PET_ENERGY_SECS_PER_POINT 12
#define PET_HUNGRY_HAPPY_SECS_PER_POINT 40

// What idleness costs: one point of happiness every half hour, one of energy
// every ten minutes. A day away costs most of the energy and a third of the
// happiness; a week away leaves a sleepy, sad pet.
#define PET_IDLE_HAPPY_SECS_PER_POINT 1800
#define PET_IDLE_ENERGY_SECS_PER_POINT 600

// After this long without music (or a touch) the pet is asleep: eyes closed,
// and no bobbing along to a track that is not there.
#define PET_SLEEP_AFTER_SECS (6 * 3600)

// A petting finger is welcome once a minute. Without the limit the button is a
// way to fill the hearts without playing anything, which is the one thing this
// pet is not for.
#define PET_PET_COOLDOWN_SECS 60
#define PET_PET_HAPPY_GAIN 4
#define PET_PET_ENERGY_GAIN 2

// The most offline time one advance will account for. Longer than this and the
// extra is dropped: a clock set years ahead, or a device in a drawer, should
// not roll the arithmetic over.
#define PET_MAX_ELAPSED_SECS (7 * 24 * 3600)

typedef enum {
	PET_STAGE_EGG = 0, // the first few minutes: the egg is warming up
	PET_STAGE_BABY,	   // hatched
	PET_STAGE_CHILD,
	PET_STAGE_TEEN,
	PET_STAGE_ADULT,
	PET_STAGE_COUNT,
} pet_stage_t;

typedef enum {
	PET_MOOD_SLEEPING = 0,
	PET_MOOD_SAD,
	PET_MOOD_OKAY,
	PET_MOOD_HAPPY,
	PET_MOOD_BLISSFUL,
	PET_MOOD_COUNT,
} pet_mood_t;

// What one advance did that a page may want to celebrate. An advance can carry
// both.
typedef enum {
	PET_EVENT_NONE = 0,
	PET_EVENT_GOAL_MET = 1 << 0,  // the daily goal was reached just now
	PET_EVENT_STAGE_UP = 1 << 1,  // it grew a stage just now
} pet_event_t;

typedef struct {
	// Epoch seconds. `adopted` is 0 on a state nobody has loaded yet, which is
	// what pet_load() reads as "a new pet"; the rest are bookkeeping.
	int64_t adopted;
	int64_t last_seen;  // the end of the previous advance
	int64_t last_music; // the last second of music
	int64_t last_pet;   // the last accepted petting

	// Lifetime seconds of music, and the part of it that fell on `day`. The
	// day is a count of local days since the epoch (see pet_day_now()): a day
	// that is not the stored one resets the today counter and settles the
	// streak.
	int64_t xp_secs;
	long day;
	int today_secs;

	// Percentages, 0 to PET_HAPPY_MAX and PET_ENERGY_MAX.
	int happy;
	int energy;

	// Days on which the goal was reached, and how many in a row. `goal_day`
	// is the day the goal was last met, and is what tells a missed day from a
	// day that has not happened yet.
	int streak;
	long goal_day;

	// The parts of a point that have not arrived yet, in seconds. Kept so a
	// one-second tick adds up instead of being rounded away every time.
	int happy_credit;
	int energy_credit;
	int idle_happy_credit;
	int idle_energy_credit;

	int pets; // how many times it has been petted, for the sake of a number
} pet_state_t;

// A brand new pet: an egg, content, with the energy of a device that has just
// been switched on. `now` and `day` must be the caller's clock; a caller with a
// clock it cannot trust passes 0 and 0 and gets a pet that ages from there.
void pet_defaults(pet_state_t *st, int64_t now, long day);

// Moves the state from its last_seen to `now`, with `listened_secs` of that
// time spent playing music. Safe to call every second; also the call that
// applies a long offline gap after pet_load(). `day` is the caller's local day
// (pet_day_now()). Returns the PET_EVENT_* bits.
int pet_advance(pet_state_t *st, int64_t now, long day, int listened_secs);

// The button: a small gain, refused while the cooldown is running. True when
// the petting counted. A sleeping pet wakes up to be petted.
bool pet_pet(pet_state_t *st, int64_t now);

// What the face and the label say. Sleeping outranks every mood, because a
// creature with its eyes shut is not sad, it is asleep.
pet_mood_t pet_mood(const pet_state_t *st, int64_t now);

// The stage the lifetime listening has earned.
pet_stage_t pet_stage(const pet_state_t *st);

// How far into the current stage the pet is, 0 to 100. 100 at the last stage.
int pet_stage_percent(const pet_state_t *st);

// Seconds of music still needed for the next stage, 0 at the last one.
int64_t pet_stage_remaining(const pet_state_t *st);

bool pet_goal_met_today(const pet_state_t *st);

// The number of hearts the page draws filled, out of five.
int pet_hearts(const pet_state_t *st);

// Translation tags. Keeping them here rather than in the page means the tag
// for a mood cannot drift from the mood itself.
const char *pet_mood_tag(pet_mood_t mood);
const char *pet_stage_tag(pet_stage_t stage);

// Local days since the epoch, for a wall clock the caller has already decided
// to trust. The day changes at local midnight; the time zone is the system's,
// so a player carried across one keeps its own idea of "today".
long pet_day_now(int64_t now);

// The configuration. Both live in device_config.ini's [pet] section; the pet
// keeps its whole state there, in numbers a person can read and edit over ADB.
//
// pet_load() fills in defaults (and writes them) when there is nothing to read.
// A state written by a newer build with fields this one does not know about
// loses those fields, which is what a settings file is.
void pet_load(pet_state_t *st, int64_t now, long day);
void pet_save(const pet_state_t *st);

// The name on the page, from [pet] name of the configuration. "Momo" when it
// has never been given one. `out` is always NUL-terminated; a name longer than
// the page can draw is cut, and one with leading or trailing space is trimmed.
void pet_name(char *out, size_t out_size);

#endif /* SYSTEM_PET_PET_H */
