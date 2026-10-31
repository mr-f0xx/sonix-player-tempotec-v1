// The music pet's arithmetic, with no screen and no audio.
//
// The creature on the page is a drawing; what can be wrong quietly is how the
// mood moves with the listening, how it falls when the player is idle, when the
// goal counts and when the pet grows. All of that is pet.c, plain integers, and
// this is where it is driven second by second.

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "src/system/core/config.h"
#include "src/system/pet/pet.h"

#define DAY 86400
#define HOUR 3600

static const int64_t T0 = 1761868800; // 2025-10-31 00:00:00 UTC
static const long DAY0 = 20392;       // the local day of T0, in UTC

static pet_state_t st;

static void fresh(void) { pet_defaults(&st, T0, DAY0); }

// Long listens go through the same one-hour ceiling a tick has: pet_advance()
// is a second-by-second call, and a caller cannot claim a whole day at once.
static void listen(int seconds) {
	while (seconds > 0) {
		int chunk = seconds > 3600 ? 3600 : seconds;
		pet_advance(&st, st.last_seen + 1, st.day, chunk);
		seconds -= chunk;
	}
}

static void one_second(int seconds) {
	// The ordinary tick: one second of music, with the clock moving with it.
	for (int i = 0; i < seconds; i++) {
		pet_advance(&st, st.last_seen + 1, st.day, 1);
	}
}

static void test_defaults(void) {
	fresh();
	assert(st.adopted == T0);
	assert(st.last_seen == T0);
	assert(st.last_music == T0);
	assert(st.happy == 50);
	assert(st.energy == 80);
	assert(st.xp_secs == 0);
	assert(st.today_secs == 0);
	assert(st.day == DAY0);
	assert(pet_stage(&st) == PET_STAGE_EGG);
	assert(pet_hearts(&st) == 3); // 50 percent, rounded
	assert(pet_mood(&st, T0) == PET_MOOD_HAPPY);
}

static void test_music_fills_the_meters(void) {
	fresh();

	// Nineteen seconds is not yet a point of happiness, and the energy has
	// already had one at twelve.
	one_second(19);
	assert(st.happy == 50);
	assert(st.energy == 81);
	assert(st.xp_secs == 19);

	one_second(1);
	assert(st.happy == 51); // the twentieth second
	assert(st.xp_secs == 20);

	// A long listen: happiness at one point per twenty seconds, energy at one
	// per twelve (and capped at a hundred).
	fresh();
	pet_advance(&st, T0, DAY0, 600);
	assert(st.happy == 80);
	assert(st.energy == 100);
	assert(pet_hearts(&st) == 4);

	// Nothing goes past a hundred however long the album is.
	fresh();
	pet_advance(&st, T0, DAY0, 3000);
	assert(st.happy == PET_HAPPY_MAX);
	assert(st.energy == PET_ENERGY_MAX);
	assert(pet_hearts(&st) == 5);
}

static void test_idleness_costs(void) {
	fresh();
	pet_advance(&st, T0 + 1800, DAY0, 0);
	assert(st.happy == 49);
	assert(st.energy == 77); // three ten-minute steps

	pet_advance(&st, T0 + 1800 + 600, DAY0, 0);
	assert(st.happy == 49);
	assert(st.energy == 76);

	// A pause after a track is not its own decay: the second that was music
	// costs nothing, and the tick that carries the gap to the next track pays
	// only the part that was silence.
	fresh();
	one_second(20);
	int happy_after_track = st.happy;
	pet_advance(&st, st.last_seen + 3600, DAY0, 1);
	assert(st.happy == happy_after_track - 1); // 3599 idle seconds pay one point
	assert(st.xp_secs == 21);
}

static void test_offline_time_is_capped(void) {
	fresh();
	// A month away is a week of decay, not a month of it: the cap is what
	// keeps a clock set years ahead from rolling the arithmetic over.
	pet_advance(&st, T0 + 30 * DAY, DAY0 + 30, 0);
	assert(st.happy == 0);
	assert(st.energy == 0);
	assert(st.last_seen == T0 + 30 * DAY);
}

static void test_a_backwards_clock_is_harmless(void) {
	fresh();
	one_second(60);
	int happy = st.happy;
	int energy = st.energy;
	int64_t xp = st.xp_secs;

	pet_advance(&st, T0 - 5000, DAY0, 0);
	assert(st.happy == happy);
	assert(st.energy == energy);
	assert(st.xp_secs == xp);
	assert(st.last_seen == T0 - 5000); // the new reading is the one to move from
}

static void test_the_daily_goal_and_the_streak(void) {
	fresh();

	int events = pet_advance(&st, T0, DAY0, PET_DAY_GOAL_SECS);
	assert(events & PET_EVENT_GOAL_MET);
	assert(st.goal_day == DAY0);
	assert(st.streak == 1);
	assert(pet_goal_met_today(&st));

	// The same day does not count twice.
	events = pet_advance(&st, st.last_seen + 1, DAY0, 1);
	assert(!(events & PET_EVENT_GOAL_MET));
	assert(st.streak == 1);

	// The next day starts empty, and yesterday's streak is still alive.
	pet_advance(&st, st.last_seen + 60, DAY0 + 1, 0);
	assert(st.today_secs == 0);
	assert(st.streak == 1);
	assert(!pet_goal_met_today(&st));

	// Reaching it again continues the streak.
	events = pet_advance(&st, st.last_seen + 60, DAY0 + 1, PET_DAY_GOAL_SECS);
	assert(events & PET_EVENT_GOAL_MET);
	assert(st.streak == 2);

	// A day skipped breaks it, and the day after that starts again at one.
	pet_advance(&st, st.last_seen + 60, DAY0 + 3, 0);
	assert(st.streak == 0);
	events = pet_advance(&st, st.last_seen + 60, DAY0 + 3, PET_DAY_GOAL_SECS);
	assert(events & PET_EVENT_GOAL_MET);
	assert(st.streak == 1);
}

static void test_growth(void) {
	fresh();
	assert(pet_stage(&st) == PET_STAGE_EGG);
	assert(pet_stage_percent(&st) == 0);
	assert(pet_stage_remaining(&st) == 5 * 60);

	listen(5 * 60);
	assert(pet_stage(&st) == PET_STAGE_BABY);
	assert(pet_stage_percent(&st) == 0);
	assert(pet_stage_remaining(&st) == 55 * 60);

	listen(HOUR); // past the hour, into the child
	assert(pet_stage(&st) == PET_STAGE_CHILD);

	listen(3 * HOUR);
	assert(pet_stage(&st) == PET_STAGE_TEEN);

	listen(11 * HOUR);
	assert(pet_stage(&st) == PET_STAGE_ADULT);
	assert(pet_stage_percent(&st) == 100);
	assert(pet_stage_remaining(&st) == 0);

	// The stage-up is reported once, at the crossing.
	fresh();
	int events = pet_advance(&st, T0, DAY0, 5 * 60);
	assert(events & PET_EVENT_STAGE_UP);
	events = pet_advance(&st, st.last_seen + 1, DAY0, 1);
	assert(!(events & PET_EVENT_STAGE_UP));
}

static void test_mood(void) {
	fresh();
	st.happy = 90;
	assert(pet_mood(&st, T0) == PET_MOOD_BLISSFUL);
	st.happy = 50;
	assert(pet_mood(&st, T0) == PET_MOOD_HAPPY);
	st.happy = 30;
	assert(pet_mood(&st, T0) == PET_MOOD_OKAY);
	st.happy = 10;
	assert(pet_mood(&st, T0) == PET_MOOD_SAD);

	// Six hours without music or a finger: asleep. The mood does not say
	// "sad" for a creature that is only resting.
	assert(pet_mood(&st, T0 + 6 * HOUR - 1) == PET_MOOD_SAD);
	assert(pet_mood(&st, T0 + 6 * HOUR + 1) == PET_MOOD_SLEEPING);

	// A petting wakes it.
	assert(pet_pet(&st, T0 + 6 * HOUR + 2));
	assert(pet_mood(&st, T0 + 6 * HOUR + 3) == PET_MOOD_SAD);
}

static void test_petting(void) {
	fresh();
	int happy = st.happy;
	int energy = st.energy;

	assert(pet_pet(&st, T0 + 1000));
	assert(st.happy == happy + PET_PET_HAPPY_GAIN);
	assert(st.energy == energy + PET_PET_ENERGY_GAIN);
	assert(st.pets == 1);

	// The cooldown is a minute, and it is a minute of the same clock.
	assert(!pet_pet(&st, T0 + 1000 + PET_PET_COOLDOWN_SECS - 1));
	assert(pet_pet(&st, T0 + 1000 + PET_PET_COOLDOWN_SECS));
	assert(st.pets == 2);

	// Nothing goes past a hundred.
	st.happy = 99;
	st.energy = 99;
	assert(pet_pet(&st, T0 + 10000));
	assert(st.happy == 100);
	assert(st.energy == 100);

	// A clock that went backwards is not a cooldown that never ends.
	assert(pet_pet(&st, T0)); // stored last_pet is ahead of this
}

static void test_day_number(void) {
	setenv("TZ", "UTC", 1);
	tzset();
	assert(pet_day_now(0) == 0);
	assert(pet_day_now(DAY0 * DAY) == DAY0);
	assert(pet_day_now(DAY0 * DAY + DAY - 1) == DAY0);
	assert(pet_day_now(DAY0 * DAY + DAY) == DAY0 + 1);
	assert(pet_day_now(-1) == 0); // a clock that has never been set
}

static void test_persistence(void) {
	char dir[] = "/tmp/sonix-pet-test-XXXXXX";
	assert(mkdtemp(dir));
	char path[256];
	snprintf(path, sizeof(path), "%s/device_config.ini", dir);
	config_init(path); // false: the file is not there yet, which is the point

	fresh();
	st.happy = 73;
	st.energy = 41;
	st.xp_secs = 4321;
	st.today_secs = 321;
	st.streak = 4;
	st.goal_day = DAY0 - 1;
	st.pets = 9;
	pet_save(&st);

	pet_state_t loaded;
	pet_load(&loaded, T0, DAY0);
	assert(loaded.adopted == st.adopted);
	assert(loaded.happy == st.happy);
	assert(loaded.energy == st.energy);
	assert(loaded.xp_secs == st.xp_secs);
	assert(loaded.today_secs == st.today_secs);
	assert(loaded.streak == st.streak);
	assert(loaded.goal_day == st.goal_day);
	assert(loaded.pets == st.pets);
	assert(loaded.day == DAY0);

	// A file that has never held a pet gives a new one, and writes it.
	char path2[256];
	snprintf(path2, sizeof(path2), "%s/fresh.ini", dir);
	config_init(path2);
	pet_load(&loaded, T0, DAY0);
	assert(loaded.adopted == T0);
	assert(loaded.happy == 50);
	assert(config_get_int("pet", "happy", -1) == 50);

	// A hand-edited file beyond the ranges is brought back inside them.
	config_set_int("pet", "happy", 9999);
	config_set_int("pet", "energy", -20);
	config_set_int("pet", "xp_secs", -5);
	pet_load(&loaded, T0, DAY0);
	assert(loaded.adopted == T0); // unchanged by the clamps below
	assert(loaded.happy == PET_HAPPY_MAX);
	assert(loaded.energy == 0);
	assert(loaded.xp_secs == 0);
}

static void test_name(void) {
	char out[40];

	// The configuration has to be up for these; test_persistence() did that.
	config_set("pet", "name", "Momo");
	pet_name(out, sizeof(out));
	assert(strcmp(out, "Momo") == 0);

	config_set("pet", "name", "  Pixie  ");
	pet_name(out, sizeof(out));
	assert(strcmp(out, "Pixie") == 0);

	config_set("pet", "name", "");
	pet_name(out, sizeof(out));
	assert(strcmp(out, "Momo") == 0); // an empty name is no name

	// A long name is cut at a character boundary, never inside a UTF-8
	// sequence: the last byte of the result is not a continuation byte.
	config_set("pet", "name", "Mélodieeeeeeeeeeeeeeeeeeeeeeeeeeeeee");
	pet_name(out, sizeof(out));
	assert(strlen(out) <= 24);
	for (size_t i = 0; out[i]; i++) {
		if (((unsigned char)out[i] & 0xC0) == 0x80) {
			assert(i > 0);
		}
	}
	assert(strncmp(out, "Mélodie", 8) == 0);
}

int main(void) {
	test_day_number();
	test_defaults();
	test_music_fills_the_meters();
	test_idleness_costs();
	test_offline_time_is_capped();
	test_a_backwards_clock_is_harmless();
	test_the_daily_goal_and_the_streak();
	test_growth();
	test_mood();
	test_petting();
	test_persistence();
	test_name();

	printf("PASS music pet: metres, decay, goal, streak, stages and persistence\n");
	return 0;
}
