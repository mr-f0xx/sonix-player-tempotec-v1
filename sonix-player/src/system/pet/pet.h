#ifndef SYSTEM_PET_PET_H
#define SYSTEM_PET_PET_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// The music pet: a small creature that lives on healthy listening.
//
// This is the whole of it that can be wrong quietly -- how the mood moves with
// the music, what a minute of it is worth, when the creature grows -- and it is
// plain arithmetic on purpose. No LVGL, no audio, no clock: a caller says how
// much time has passed and how much of that was music it could actually hear,
// and gets the new state back. petwatch.c reads the player's own playback state
// on a one-second timer and petlisten.c decides what that second was worth;
// petpage.c only draws what this returns.
//
// The listening rules exist to reward music somebody is actually hearing:
//
//   * a minute of audible music is a minute of XP. petwatch.c credits nothing
//     when the player is paused, or when the volume is at or below the floor
//     (muted music is not listening), and petlisten.c refuses to credit a late
//     timer callback for more than a few seconds;
//   * XP is capped per day, so playing through the night cannot buy a stage
//     that a day of ordinary listening would not;
//   * a track from an album or genre that has not been heard today is worth a
//     little more, which rewards exploring over leaving one record on repeat;
//   * the daily goal, the streak and the achievements are about time actually
//     spent listening, never about how long the player was switched on.
//
// The numbers are small and whole. Happiness and energy are percentages; the
// stage comes from the lifetime credited seconds; the streak counts days on
// which the daily goal was reached. Nothing here ever kills the pet: a player
// left in a drawer for a month comes back to a sad, sleepy creature that a
// couple of albums will cheer up. That is a decision, not an oversight -- a
// music player should not punish somebody for listening somewhere else for a
// while, and inspiration is not a resource that can be lost for good.
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

// The daily goal, in seconds of audible music: fifteen minutes. Meeting it
// keeps the streak alive. It is deliberately small: the goal is a reason to put
// something on, not a chore.
#define PET_DAY_GOAL_SECS (15 * 60)

// How much audible music a day can turn into XP and notes. Four hours is past
// any ordinary listening day and short enough that a player left running
// overnight gains nothing after the first four hours of it. Listening after the
// cap still feeds the meters (the music is real) but no longer moves the stage,
// the notes or the day's XP.
#define PET_DAILY_XP_SECS (4 * 60 * 60)

// Listening that fills each meter. Happiness moves slowly -- a track is worth a
// few percent -- and energy quickly, because energy is what a recent session
// leaves behind and what drains while the player is idle. A hungry pet (no
// energy left) earns happiness at half rate until the same music has fed it.
#define PET_HAPPY_SECS_PER_POINT 20
#define PET_ENERGY_SECS_PER_POINT 12
#define PET_HUNGRY_HAPPY_SECS_PER_POINT 40

// What idleness costs: one point of happiness every half hour, one of energy
// every ten minutes. A day away costs almost half the happiness and all of the
// energy; the pet does not die, and a couple of albums bring it back.
#define PET_IDLE_HAPPY_SECS_PER_POINT 1800
#define PET_IDLE_ENERGY_SECS_PER_POINT 600

// No music for this long and the pet is asleep: eyes closed, no bobbing, and
// the page says so. Two days, not six hours, on purpose -- a player who spends
// a day listening to something else has not neglected anything. The state is
// called "sleeping" rather than "silent" because that is what the face shows;
// a pat does not wake it, only a track does.
#define PET_SLEEP_AFTER_SECS (48 * 3600)

// A petting finger is welcome once a minute. Without the limit the button is a
// way to fill the hearts without playing anything, which is the one thing this
// pet is not for.
#define PET_PET_COOLDOWN_SECS 60
#define PET_PET_HAPPY_GAIN 4
#define PET_PET_ENERGY_GAIN 2

// A held finger is not a tap: two seconds down is a grooming, and it is worth
// more than the tap it starts with -- once a day, so that holding the dog is
// not a way to fill the meter without any music.
#define PET_HOLD_HAPPY_GAIN 6

// The most offline time one advance will account for. Longer than this and the
// extra is dropped: a clock set years ahead, or a device in a drawer, should
// not roll the arithmetic over.
#define PET_MAX_ELAPSED_SECS (7 * 24 * 3600)

// Music at or below this volume is not listened to, so it is not paid for. A
// player muted overnight, or with the slider at its bottom, feeds nothing.
#define PET_VOLUME_FLOOR 10

// What half an hour without a pause, and a record heard through, are worth on
// top of the music itself: a small lift of the heart meter, once each.
#define PET_FOCUS_HAPPY_BONUS 10
#define PET_ALBUM_HAPPY_BONUS 6

// The bonus for a track whose album or genre is new for the day. It is a
// percentage of that track's credited seconds.
#define PET_DIVERSITY_BONUS_PERCENT 20

// How many distinct genres or albums a day remembers. Five genres is the
// Polymath milestone, with room for a couple of extras; eight albums is more
// than an ordinary day explores. Once the set is full new keys stop earning the
// bonus, which is the point -- a hundred one-track albums should not outperform
// an actual listening session.
#define PET_DAY_KEYS 8

// A session this long without a pause or a skip is "focus": a quiet, one-off
// achievement and a small note bonus. It is measured by petlisten.c, not here.
#define PET_FOCUS_SECS (30 * 60)

// A minute of audible music between midnight and five in the morning is the
// Night Owl. The window is deliberately short: "past midnight" is a mood, not
// an all-night grind.
#define PET_NIGHT_OWL_SECS 60

// Three tracks of the same album, each heard to its end, without a skip: the
// Deep Diver. A whole album is often more than the daily XP cap, which is why
// this is a milestone rather than a per-track reward.
#define PET_DEEP_DIVER_TRACKS 3

// ---- what the listening was, over a month -------------------------------
//
// The pet's temperament comes from the *kind* of music it heard most over the
// last thirty days, not from what is playing now: a month of the same family
// of tags is what a person's listening actually looks like, and a page that
// changed character with every track would be noise. One entry a day, the
// family the day was mostly spent in, kept in a ring thirty days long.
#define PET_TEMPERAMENT_DAYS 30

// Fewer than this many days with a recognisable genre and there is no
// temperament yet: three days is enough to mean something and short enough that
// a new pet has a character by the end of its first week.
#define PET_TEMPERAMENT_MIN_DAYS 3

// What an Upbeat pet's liveliness is worth: energy fills faster, and every two
// minutes of credited music has a chance of turning up a note or two on the
// floor. The chance is a hash of the day and the bucket, not a random number
// generator, so the same listening always pays the same and the suite can
// predict it.
#define PET_UPBEAT_ENERGY_SECS_PER_POINT 8
#define PET_DROP_EVERY_SECS 120
#define PET_DROP_CHANCE_PERCENT 60

// What a long session turns up for a Curious (indie and alternative) pet:
// after this much uninterrupted listening, once a day, something is found.
#define PET_FORAGE_SECS (45 * 60)
#define PET_FORAGE_CHANCE 3 // one day in three

// ---- being skipped at ----------------------------------------------------
//
// Skipping is not a crime, but flicking through a library is not listening.
// More than PET_SKIP_LIMIT tracks abandoned inside PET_SKIP_WINDOW seconds and
// the pet is Dizzy for PET_DIZZY_SECS: the meters still fill with whatever
// music is on, but nothing is credited towards the stage, the notes or the day.
// Fifteen minutes, not an hour, and never a lost stage.
#define PET_SKIP_LIMIT 5
#define PET_SKIP_WINDOW_SECS 600
#define PET_DIZZY_SECS (15 * 60)
#define PET_SKIP_RING 8

// ---- tap along -----------------------------------------------------------
//
// Once a day the page offers a short rhythm game: sixteen beats of the tempo
// the metronome page uses, tapped on the glass. The score is the share of the
// beats hit near their moment, and it pays a handful of notes and a little of
// both meters. It is a game, not a source of income: the ceiling is a dozen
// notes a day.
#define PET_RHYTHM_BPM 100
#define PET_RHYTHM_BEATS 16
#define PET_RHYTHM_PERFECT_MS 120
#define PET_RHYTHM_GOOD_MS 260

// Four stages of one animal: it arrives small and grows with the listening.
// There is no egg -- the pet the owner chose is the pet they get.
typedef enum {
	PET_STAGE_BABY = 0,
	PET_STAGE_CHILD,
	PET_STAGE_TEEN,
	PET_STAGE_ADULT,
	PET_STAGE_COUNT,
} pet_stage_t;

// What the owner chose to live with. NONE is a pet that has not been chosen
// yet: the page puts up the chooser before it draws anything.
typedef enum {
	PET_SPECIES_NONE = 0,
	PET_SPECIES_CAT,
	PET_SPECIES_DOG,
	PET_SPECIES_FISH,
	PET_SPECIES_COUNT,
} pet_species_t;

typedef enum {
	PET_MOOD_SLEEPING = 0,
	PET_MOOD_SAD,
	PET_MOOD_OKAY,
	PET_MOOD_HAPPY,
	PET_MOOD_BLISSFUL,
	PET_MOOD_DIZZY,
	PET_MOOD_COUNT,
} pet_mood_t;

// The families the genre words are sorted into. There are four because there
// are three temperaments and everything else; a longer list would need a
// taxonomy nobody agrees on, and the point is a character, not a library
// classification.
typedef enum {
	PET_FAMILY_OTHER = 0,
	PET_FAMILY_UPBEAT, // pop, dance, electronic, rock, hip hop
	PET_FAMILY_CALM,   // classical, jazz, ambient, lo-fi, acoustic, soul
	PET_FAMILY_CURIOUS,// indie, alternative, punk, metal, experimental
	PET_FAMILY_COUNT,
} pet_family_t;

// What thirty days of listening made of the pet. NONE until there are enough
// days to say: a pet does not have a personality on its first evening.
typedef enum {
	PET_TEMPERAMENT_NONE = 0,
	PET_TEMPERAMENT_UPBEAT,
	PET_TEMPERAMENT_CALM,
	PET_TEMPERAMENT_CURIOUS,
	PET_TEMPERAMENT_COUNT,
} pet_temperament_t;

// What one advance did that a page may want to celebrate. An advance can carry
// both; pet_note_track() reports its own.
typedef enum {
	PET_EVENT_NONE = 0,
	PET_EVENT_GOAL_MET = 1 << 0,  // the daily goal was reached just now
	PET_EVENT_STAGE_UP = 1 << 1,  // it grew a stage just now
	PET_EVENT_POLYMATH = 1 << 2,  // this track added the fifth genre today
	PET_EVENT_DROP = 1 << 3,      // an Upbeat pet found notes on the floor
	PET_EVENT_DIZZY = 1 << 4,     // too much skipping: the pet is dizzy now
	PET_EVENT_FORAGE = 1 << 5,    // a Curious pet found something on a long session
} pet_event_t;

// Achievements are permanent, a bit each. The notes they pay are one-off.
typedef enum {
	PET_ACH_NIGHT_OWL = 1 << 0,
	PET_ACH_DEEP_DIVER = 1 << 1,
	PET_ACH_POLYMATH = 1 << 2,
	PET_ACH_FOCUS = 1 << 3,
} pet_achievement_t;

// The wardrobe. Outfits are worn one at a time; the two rooms are separate, and
// the first room (the meadow the page is born on) is always there.
typedef enum {
	PET_ITEM_PARTY_HAT = 0,
	PET_ITEM_HEADPHONES,
	PET_ITEM_SCARF,
	PET_ITEM_CROWN,
	PET_ITEM_ROOM_SUNSET,
	PET_ITEM_ROOM_NIGHT,
	PET_ITEM_STREAK_FREEZE,
	PET_ITEM_COUNT,
} pet_item_t;

#define PET_ITEM_NONE ((pet_item_t)-1)
#define PET_ITEM_LAST_OUTFIT PET_ITEM_CROWN
#define PET_ITEM_FIRST_ROOM PET_ITEM_ROOM_SUNSET

// How the music feels, from its tags. There is no BPM analysis in the player --
// decoding a window of every track to find the tempo is not something a
// 240 x 320 MIPS player should do on the interface thread -- so the genre the
// file carries is what the page animates to. Anything it does not recognise is
// NEUTRAL, which is the ordinary bob.
typedef enum {
	PET_VIBE_CALM = 0,
	PET_VIBE_NEUTRAL,
	PET_VIBE_ENERGETIC,
} pet_vibe_t;

typedef struct {
	// Epoch seconds. `adopted` is 0 on a state nobody has loaded yet, which is
	// what pet_load() reads as "a new pet"; the rest are bookkeeping.
	int64_t adopted;
	int64_t last_seen;  // the end of the previous advance
	int64_t last_music; // the last second of music it could hear
	int64_t last_pet;   // the last accepted petting

	// Lifetime credited seconds of music (the stage's clock), and the part of
	// it that fell on `day`. The day is a count of local days since the epoch
	// (see pet_day_now()): a day that is not the stored one resets the today
	// counters, settles the streak and empties the day's discoveries.
	int64_t xp_secs;
	long day;
	int today_secs;    // audible seconds today, for the 15-minute goal
	int today_xp_secs; // credited seconds today, against the daily cap

	// Percentages, 0 to PET_HAPPY_MAX and PET_ENERGY_MAX.
	int happy;
	int energy;

	// Days on which the goal was reached, and how many in a row. `goal_day`
	// is the day the goal was last met; `freeze_day` is the last day a bought
	// streak freeze covered, which is what tells a frozen day from a met one.
	int streak;
	long goal_day;
	long freeze_day;

	// Musical Notes, the wardrobe's currency: one per credited minute of
	// music, plus the one-off achievement rewards. `note_credit` is the part
	// of the next note that has already been earned.
	int notes;
	int note_credit;

	// The fraction of a second the diversity bonus has earned but not yet
	// been able to pay, in hundredths.
	int bonus_credit;

	// What has been heard today, as small hashes so the state stays a handful
	// of whole numbers in the settings file. The counts are how much of each
	// array is in use.
	uint32_t day_genres[PET_DAY_KEYS];
	int day_genre_count;
	uint32_t day_albums[PET_DAY_KEYS];
	int day_album_count;

	// PET_ACH_* bits, PET_ITEM_* bits (consumables are not in here), the
	// outfit being worn (-1 for none), the room (0 meadow, 1 sunset, 2 night),
	// and the streak freezes in hand.
	int achievements;
	int owned;
	int wearing;
	int room;
	int freezes;

	// The parts of a point that have not arrived yet, in seconds. Kept so a
	// one-second tick adds up instead of being rounded away every time.
	int happy_credit;
	int energy_credit;
	int idle_happy_credit;
	int idle_energy_credit;

	// PET_SPECIES_*, chosen once and changeable: a fish lives in an aquarium,
	// the rest in a room.
	int species;

	// The last thirty days, oldest first, each entry a PET_FAMILY_*: the
	// family that day was mostly spent in. This is what the temperament is
	// read from.
	uint8_t genre_history[PET_TEMPERAMENT_DAYS];

	// Today's tally, per family, of the tracks noted (pet_note_track). Not
	// persisted: a reboot in the middle of a day costs that day's count, which
	// a temperament that needs three days cannot notice.
	int day_family[PET_FAMILY_COUNT];

	// Epoch seconds the pet stops being dizzy, 0 when it is not. See
	// pet_skip() and PET_SKIP_LIMIT.
	int64_t dizzy_until;

	// The abandoned tracks still in the window, newest last. In memory only:
	// a reboot is not a way to grind skips, it is also not a way to dodge the
	// consequence of having done it.
	int64_t skip_times[PET_SKIP_RING];
	int skip_count;

	// The day the tap-along was last paid for, and the best score so far.
	long rhythm_day;
	int rhythm_best;

	// The day the held-pat bonus was last paid. Once a day: see pet_hold().
	long hold_day;

	// The day a Curious pet last found something while foraging.
	long forage_day;

	int pets; // how many times it has been petted, for the sake of a number
} pet_state_t;

// A brand new pet: an egg, content, with the energy of a device that has just
// been switched on. `now` and `day` must be the caller's clock; a caller with a
// clock it cannot trust passes 0 and 0 and gets a pet that ages from there.
void pet_defaults(pet_state_t *st, int64_t now, long day);

// Moves the state from its last_seen to `now`, with `listened_secs` of that
// time spent hearing music that was neither paused nor muted. `bonus_percent`
// is the PET_DIVERSITY_BONUS_PERCENT for a track that was new to the day, or 0.
// Safe to call every second; also the call that applies a long offline gap after
// pet_load(). `day` is the caller's local day (pet_day_now()). Returns the
// PET_EVENT_* bits.
int pet_advance(pet_state_t *st, int64_t now, long day, int listened_secs, int bonus_percent);

// Records the genre and album of a track at the first second it was actually
// heard, and returns the bonus its listening is worth this track (0 or
// PET_DIVERSITY_BONUS_PERCENT). The fifth genre of a day awards Polymath and
// reports PET_EVENT_POLYMATH through `events`. Either key may be NULL or empty;
// a stream with no album and no genre earns no bonus and teaches nothing.
int pet_note_track(pet_state_t *st, const char *genre, const char *album, int *events);

// Whether a volume is high enough to count as listening at all.
bool pet_audible_volume(int percent);

// A one-off lift the listening itself earned -- the mood boost of a full album
// or of half an hour without a pause. It is not XP and it does not touch the
// wallet; it only moves the heart meter, clamped.
void pet_cheer(pet_state_t *st, int happy_points);

// The button: a small gain, refused while the cooldown is running. True when
// the petting counted. Petting does not wake a sleeping pet -- only music does
// -- but it is still welcome.
bool pet_pet(pet_state_t *st, int64_t now);

// A finger held on the animal, which is a groom rather than a tap: worth
// PET_HOLD_HAPPY_GAIN, and once a day -- false, and nothing paid, on a second
// hold the same day. The day is the state's own, which pet_advance() keeps.
bool pet_hold(pet_state_t *st);

// What the face and the label say. Sleeping outranks every mood, because a
// creature with its eyes shut is not sad, it is asleep.
pet_mood_t pet_mood(const pet_state_t *st, int64_t now);

// The animal, or PET_SPECIES_NONE before one has been chosen.
pet_species_t pet_species(const pet_state_t *st);

// Chooses or changes it. False for anything that is not one of the three.
bool pet_set_species(pet_state_t *st, pet_species_t species);

// The species' translation tag, for the chooser.
const char *pet_species_tag(pet_species_t species);

// Writes the name into the settings, trimmed and cut at a character boundary.
// An empty name puts the default back.
void pet_set_name(const char *name);

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

// The wardrobe: price in notes, translation tag, whether it is owned, what is
// worn, and the room in use. pet_buy() refuses what is already owned (except a
// streak freeze, which is consumable) and what the wallet cannot cover.
int pet_item_price(pet_item_t item);
const char *pet_item_tag(pet_item_t item);
const char *pet_achievement_tag(int achievement);
bool pet_owned(const pet_state_t *st, pet_item_t item);
bool pet_buy(pet_state_t *st, pet_item_t item);
bool pet_wear(pet_state_t *st, pet_item_t item); // PET_ITEM_NONE takes an outfit off
int pet_wearing(const pet_state_t *st);
int pet_room(const pet_state_t *st);
int pet_freezes(const pet_state_t *st);
bool pet_has_achievement(const pet_state_t *st, int achievement);

// Awards an achievement and its one-off notes. True when it was not already
// there; false for a repeat or an unknown bit.
bool pet_award_achievement(pet_state_t *st, int achievement);

// The family a genre tag belongs to. Same matching as pet_vibe_for_genre()
// (sub-strings, case-insensitive), and PET_FAMILY_OTHER for an empty tag.
pet_family_t pet_family_for_genre(const char *genre);

// The temperament the last thirty days earned. NONE until
// PET_TEMPERAMENT_MIN_DAYS days have a recognisable family.
pet_temperament_t pet_temperament(const pet_state_t *st);

// The temperament's translation tag, for the page's caption.
const char *pet_temperament_tag(pet_temperament_t temperament);

// How many days of the ring hold each family, for the page's one-line summary
// and for the suite. `out` is PET_FAMILY_COUNT long and may be NULL.
void pet_temperament_days(const pet_state_t *st, int *out);

// True when the temperament is Upbeat, which is what makes the energy meter
// fill faster: the page says so under the meters.
bool pet_is_upbeat(const pet_state_t *st);

// True when the temperament is Calm, which unlocks the drifting soundscape on
// the page. It is a picture, not audio: nothing here decodes music.
bool pet_has_soundscape(const pet_state_t *st);

// Seconds of music per point of energy, from the temperament.
int pet_energy_secs_per_point(const pet_state_t *st);

// ---- skipping -------------------------------------------------------------
//
// Records an abandoned track. Returns true when this one was the skip that
// tipped the pet into being dizzy, which the caller celebrates with a toast.
// The window is a ring of the last eight skips and the answer is whether more
// than PET_SKIP_LIMIT of them fall inside PET_SKIP_WINDOW_SECS.
bool pet_skip(pet_state_t *st, int64_t now);

// True while the pet is dizzy: nothing is credited to the stage, the notes or
// the day's XP until this is false again. The meters still fill.
bool pet_dizzy(const pet_state_t *st, int64_t now);

// ---- foraging -------------------------------------------------------------
//
// Called with the length of the current uninterrupted session. A Curious pet
// that has listened for PET_FORAGE_SECS without a break, once a day, turns
// something up: notes, and on the days the hash says so, a streak freeze in
// hand. Returns the notes found (0 when nothing was, or when the temperament
// is not Curious).
int pet_forage(pet_state_t *st, int session_secs);

// ---- the tap-along --------------------------------------------------------
//
// Ready once a day. pet_rhythm_finish() pays for the score (0 to 100) and
// returns the notes it earned; a score of 90 or more also leaves a streak
// freeze in hand. False/0 when today has already been paid for.
bool pet_rhythm_ready(const pet_state_t *st);
int pet_rhythm_finish(pet_state_t *st, int score);

// The genre's mood, for the page's animation. Case-insensitive, and tolerant of
// the sub-genres taggers actually write ("Lo-Fi Hip Hop", "Melodic Death
// Metal"): the words are matched, not the whole string.
pet_vibe_t pet_vibe_for_genre(const char *genre);

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
