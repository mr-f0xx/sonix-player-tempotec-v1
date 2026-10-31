// The music pet's arithmetic, with no screen and no audio.
//
// The creature on the page is a drawing; what can be wrong quietly is how the
// mood moves with the listening, what a minute is worth, how it falls when the
// player is idle, when the goal counts, when the pet grows and what the wallet
// can buy. All of that is pet.c, plain integers, and this is where it is driven
// second by second.

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
// When the day's XP is exhausted the test moves the calendar to midnight, the
// way a real player would have to wait for it.
static void feed(int seconds, int bonus_percent) {
	while (seconds > 0) {
		int room = PET_DAILY_XP_SECS - st.today_xp_secs;
		if (room <= 0) {
			pet_advance(&st, st.last_seen, st.day + 1, 0, 0);
			continue;
		}
		int chunk = seconds > 3600 ? 3600 : seconds;
		if (chunk > room) {
			chunk = room;
		}
		pet_advance(&st, st.last_seen + 1, st.day, chunk, bonus_percent);
		seconds -= chunk;
	}
}

static void listen(int seconds) { feed(seconds, 0); }

static void one_second(int seconds) {
	// The ordinary tick: one second of music, with the clock moving with it.
	for (int i = 0; i < seconds; i++) {
		pet_advance(&st, st.last_seen + 1, st.day, 1, 0);
	}
}

// A tick at midnight, without pretending any time passed.
static void next_day(void) { pet_advance(&st, st.last_seen, st.day + 1, 0, 0); }

static void test_defaults(void) {
	fresh();
	assert(st.adopted == T0);
	assert(st.last_seen == T0);
	assert(st.last_music == T0);
	assert(st.happy == 50);
	assert(st.energy == 80);
	assert(st.xp_secs == 0);
	assert(st.today_secs == 0);
	assert(st.today_xp_secs == 0);
	assert(st.notes == 0);
	assert(st.freezes == 0);
	assert(st.achievements == 0);
	assert(st.owned == 0);
	assert(st.wearing == -1);
	assert(st.room == 0);
	assert(st.day == DAY0);
	assert(st.day_genre_count == 0);
	assert(st.day_album_count == 0);
	assert(pet_stage(&st) == PET_STAGE_BABY);
	assert(pet_species(&st) == PET_SPECIES_NONE);
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
	assert(st.note_credit == 19);

	one_second(1);
	assert(st.happy == 51); // the twentieth second
	assert(st.xp_secs == 20);

	// A long listen: happiness at one point per twenty seconds, energy at one
	// per twelve (and capped at a hundred). Ten minutes is ten notes.
	fresh();
	pet_advance(&st, T0, DAY0, 600, 0);
	assert(st.happy == 80);
	assert(st.energy == 100);
	assert(pet_hearts(&st) == 4);
	assert(st.notes == 10);

	// Nothing goes past a hundred however long the album is.
	fresh();
	pet_advance(&st, T0, DAY0, 3000, 0);
	assert(st.happy == PET_HAPPY_MAX);
	assert(st.energy == PET_ENERGY_MAX);
	assert(pet_hearts(&st) == 5);
}

static void test_idleness_costs(void) {
	fresh();
	pet_advance(&st, T0 + 1800, DAY0, 0, 0);
	assert(st.happy == 49);
	assert(st.energy == 77); // three ten-minute steps

	pet_advance(&st, T0 + 1800 + 600, DAY0, 0, 0);
	assert(st.happy == 49);
	assert(st.energy == 76);

	// A pause after a track is not its own decay: the second that was music
	// costs nothing, and the tick that carries the gap to the next track pays
	// only the part that was silence.
	fresh();
	one_second(20);
	int happy_after_track = st.happy;
	pet_advance(&st, st.last_seen + 3600, DAY0, 1, 0);
	assert(st.happy == happy_after_track - 1); // 3599 idle seconds pay one point
	assert(st.xp_secs == 21);
}

static void test_offline_time_is_capped(void) {
	fresh();
	// A month away is a week of decay, not a month of it: the cap is what
	// keeps a clock set years ahead from rolling the arithmetic over.
	pet_advance(&st, T0 + 30 * DAY, DAY0 + 30, 0, 0);
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

	pet_advance(&st, T0 - 5000, DAY0, 0, 0);
	assert(st.happy == happy);
	assert(st.energy == energy);
	assert(st.xp_secs == xp);
	assert(st.last_seen == T0 - 5000); // the new reading is the one to move from
}

static void test_the_daily_goal_and_the_streak(void) {
	fresh();

	int events = pet_advance(&st, T0, DAY0, PET_DAY_GOAL_SECS, 0);
	assert(events & PET_EVENT_GOAL_MET);
	assert(st.goal_day == DAY0);
	assert(st.streak == 1);
	assert(pet_goal_met_today(&st));

	// The same day does not count twice.
	events = pet_advance(&st, st.last_seen + 1, DAY0, 1, 0);
	assert(!(events & PET_EVENT_GOAL_MET));
	assert(st.streak == 1);

	// The next day starts empty, and yesterday's streak is still alive.
	pet_advance(&st, st.last_seen + 60, DAY0 + 1, 0, 0);
	assert(st.today_secs == 0);
	assert(st.streak == 1);
	assert(!pet_goal_met_today(&st));

	// Reaching it again continues the streak.
	events = pet_advance(&st, st.last_seen + 60, DAY0 + 1, PET_DAY_GOAL_SECS, 0);
	assert(events & PET_EVENT_GOAL_MET);
	assert(st.streak == 2);

	// A day skipped breaks it, and the day after that starts again at one.
	pet_advance(&st, st.last_seen + 60, DAY0 + 3, 0, 0);
	assert(st.streak == 0);
	events = pet_advance(&st, st.last_seen + 60, DAY0 + 3, PET_DAY_GOAL_SECS, 0);
	assert(events & PET_EVENT_GOAL_MET);
	assert(st.streak == 1);
}

static void test_a_freeze_covers_a_missed_day(void) {
	fresh();

	pet_advance(&st, T0, DAY0, PET_DAY_GOAL_SECS, 0);
	assert(st.streak == 1);
	st.freezes = 2;

	// Yesterday was met: nothing is spent.
	next_day();
	assert(st.streak == 1);
	assert(st.freezes == 2);

	// Yesterday was missed and there is a freeze: it covers the day, and the
	// run stays where it was.
	next_day();
	assert(st.streak == 1);
	assert(st.freezes == 1);
	assert(st.freeze_day == DAY0 + 1);

	// Meeting today's goal after a frozen day continues the run.
	pet_advance(&st, st.last_seen + 60, DAY0 + 2, PET_DAY_GOAL_SECS, 0);
	assert(st.streak == 2);
	assert(st.goal_day == DAY0 + 2);

	// Another missed day spends the last freeze and keeps the run; the one
	// after it, with none left, ends it.
	pet_advance(&st, st.last_seen + 60, DAY0 + 3, 0, 0);
	assert(st.streak == 2);
	pet_advance(&st, st.last_seen + 60, DAY0 + 4, 0, 0);
	assert(st.streak == 2);
	assert(st.freezes == 0);
	pet_advance(&st, st.last_seen + 60, DAY0 + 5, 0, 0);
	assert(st.streak == 0);
}

static void test_growth(void) {
	fresh();
	assert(pet_stage(&st) == PET_STAGE_BABY);
	assert(pet_stage_percent(&st) == 0);
	assert(pet_stage_remaining(&st) == 30 * 60);

	listen(30 * 60);
	assert(pet_stage(&st) == PET_STAGE_CHILD);
	assert(pet_stage_percent(&st) == 0);
	assert(pet_stage_remaining(&st) == 210 * 60);

	listen(3 * HOUR + 30 * 60); // up to four hours, into the teen
	assert(pet_stage(&st) == PET_STAGE_TEEN);

	listen(11 * HOUR);
	assert(pet_stage(&st) == PET_STAGE_ADULT);
	assert(pet_stage_percent(&st) == 100);
	assert(pet_stage_remaining(&st) == 0);

	// The stage-up is reported once, at the crossing.
	fresh();
	int events = pet_advance(&st, T0, DAY0, 30 * 60, 0);
	assert(events & PET_EVENT_STAGE_UP);
	events = pet_advance(&st, st.last_seen + 1, DAY0, 1, 0);
	assert(!(events & PET_EVENT_STAGE_UP));
}

static void test_the_daily_xp_ceiling(void) {
	fresh();

	// Five hours of music in one day: four are credited, and the fifth feeds
	// the meters and the goal but buys nothing.
	for (int hour = 0; hour < 5; hour++) {
		pet_advance(&st, T0, DAY0, HOUR, 0);
	}
	assert(st.xp_secs == PET_DAILY_XP_SECS);
	assert(st.today_xp_secs == PET_DAILY_XP_SECS);
	assert(st.today_secs == 5 * HOUR);
	assert(st.notes == PET_DAILY_XP_SECS / 60);
	assert(pet_goal_met_today(&st));

	// The ceiling is the day's, not the pet's: tomorrow starts again.
	next_day();
	assert(st.today_xp_secs == 0);
	pet_advance(&st, st.last_seen, st.day, HOUR, 0);
	assert(st.xp_secs == PET_DAILY_XP_SECS + HOUR);
	assert(st.today_xp_secs == HOUR);
	assert(st.notes == PET_DAILY_XP_SECS / 60 + 60);
}

static void test_diversity_bonus(void) {
	fresh();
	int events = -1;

	// A new album and a new genre: the first track of a day is worth 20%.
	assert(pet_note_track(&st, "Rock", "Album A", &events) == PET_DIVERSITY_BONUS_PERCENT);
	assert(events == PET_EVENT_NONE);
	assert(st.day_genre_count == 1);
	assert(st.day_album_count == 1);

	// The same record again is not a discovery.
	assert(pet_note_track(&st, "Rock", "Album A", &events) == 0);

	// A new record of the same genre is.
	assert(pet_note_track(&st, "Rock", "Album B", &events) == PET_DIVERSITY_BONUS_PERCENT);

	// And a new genre on a record already heard is.
	assert(pet_note_track(&st, "Jazz", "Album A", &events) == PET_DIVERSITY_BONUS_PERCENT);

	// Case and surrounding space do not make a new genre.
	assert(pet_note_track(&st, "  rock ", "Album C", &events) ==
		   PET_DIVERSITY_BONUS_PERCENT); // the album is new; the genre is not
	assert(st.day_genre_count == 2);

	// A live stream has neither, and teaches nothing.
	assert(pet_note_track(&st, "", "", &events) == 0);

	// The bonus is paid on the credited seconds: ten minutes is twelve.
	fresh();
	pet_advance(&st, T0, DAY0, 600, PET_DIVERSITY_BONUS_PERCENT);
	assert(st.xp_secs == 720);
	assert(st.today_xp_secs == 720);
	assert(st.notes == 12);

	// The fraction carries between ticks rather than being rounded away.
	fresh();
	for (int i = 0; i < 5; i++) {
		pet_advance(&st, st.last_seen + 1, DAY0, 1, PET_DIVERSITY_BONUS_PERCENT);
	}
	assert(st.xp_secs == 6); // five fifths of a bonus second arrived

	// Tomorrow, the same record is new again.
	fresh();
	pet_note_track(&st, "Rock", "Album A", &events);
	next_day();
	assert(st.day_genre_count == 0);
	assert(st.day_album_count == 0);
	assert(pet_note_track(&st, "Rock", "Album A", &events) == PET_DIVERSITY_BONUS_PERCENT);
}

static void test_polymath(void) {
	fresh();
	static const char *const genres[] = {"Rock", "Jazz", "Ambient", "Electronic"};
	for (size_t i = 0; i < sizeof(genres) / sizeof(genres[0]); i++) {
		int events = PET_EVENT_NONE;
		assert(pet_note_track(&st, genres[i], genres[i], &events) == PET_DIVERSITY_BONUS_PERCENT);
		assert(!(events & PET_EVENT_POLYMATH));
	}
	assert(!pet_has_achievement(&st, PET_ACH_POLYMATH));

	int notes_before = st.notes;
	int events = PET_EVENT_NONE;
	assert(pet_note_track(&st, "Folk", "E", &events) == PET_DIVERSITY_BONUS_PERCENT);
	assert(events & PET_EVENT_POLYMATH);
	assert(pet_has_achievement(&st, PET_ACH_POLYMATH));
	assert(st.notes == notes_before + 25);

	// The fifth genre of another day is five genres again, but the award was
	// already earned: it pays once.
	next_day();
	notes_before = st.notes;
	for (int i = 0; i < 5; i++) {
		char genre[16];
		snprintf(genre, sizeof(genre), "Genre%d", i);
		pet_note_track(&st, genre, genre, &events);
	}
	assert(st.notes == notes_before);
}

static void test_wallet_and_wardrobe(void) {
	fresh();
	st.notes = 100;
	assert(!pet_buy(&st, PET_ITEM_PARTY_HAT)); // 120, too dear
	assert(st.notes == 100);
	assert(!pet_owned(&st, PET_ITEM_PARTY_HAT));

	st.notes = 500;
	assert(pet_buy(&st, PET_ITEM_PARTY_HAT));
	assert(st.notes == 380);
	assert(pet_owned(&st, PET_ITEM_PARTY_HAT));
	assert(pet_wearing(&st) == PET_ITEM_PARTY_HAT); // bought is worn

	// Buying it again is not a second sale.
	assert(!pet_buy(&st, PET_ITEM_PARTY_HAT));
	assert(st.notes == 380);

	// It can come off and go back on without spending anything.
	assert(pet_wear(&st, PET_ITEM_NONE));
	assert(pet_wearing(&st) == -1);
	assert(pet_wear(&st, PET_ITEM_PARTY_HAT));
	assert(st.notes == 380);

	// Rooms are bought and chosen the same way, and are separate from the
	// outfit.
	assert(pet_buy(&st, PET_ITEM_ROOM_SUNSET));
	assert(st.notes == 200);
	assert(pet_room(&st) == 1);
	assert(pet_wearing(&st) == PET_ITEM_PARTY_HAT);
	assert(!pet_buy(&st, PET_ITEM_ROOM_NIGHT)); // 240, and only 200 is left
	assert(pet_room(&st) == 1);
	assert(st.notes == 200);
	assert(pet_buy(&st, PET_ITEM_STREAK_FREEZE));
	assert(pet_freezes(&st) == 1);
	assert(st.notes == 50);
	assert(!pet_wear(&st, PET_ITEM_STREAK_FREEZE)); // food for the streak, not a hat

	// The prices and tags the wardrobe draws.
	assert(pet_item_price(PET_ITEM_CROWN) == 500);
	assert(strcmp(pet_item_tag(PET_ITEM_CROWN), "pet_item_crown") == 0);
	assert(pet_item_price(PET_ITEM_COUNT) == -1);
}

static void test_achievements(void) {
	fresh();
	int notes = st.notes;
	assert(pet_award_achievement(&st, PET_ACH_FOCUS));
	assert(pet_has_achievement(&st, PET_ACH_FOCUS));
	assert(st.notes == notes + 20);
	assert(!pet_award_achievement(&st, PET_ACH_FOCUS)); // once
	assert(st.notes == notes + 20);
	assert(!pet_award_achievement(&st, 1 << 20)); // not one of ours
	assert(strcmp(pet_achievement_tag(PET_ACH_NIGHT_OWL), "pet_ach_night_owl") == 0);
}

static void test_cheer(void) {
	fresh();
	st.happy = 50;

	// Half an hour without a pause is worth a lift of its own; a record heard
	// through is worth a smaller one.
	pet_cheer(&st, PET_FOCUS_HAPPY_BONUS);
	assert(st.happy == 60);
	pet_cheer(&st, PET_ALBUM_HAPPY_BONUS);
	assert(st.happy == 66);

	// The meter still stops at a hundred, and a cheer is never a loss.
	st.happy = 98;
	pet_cheer(&st, PET_ALBUM_HAPPY_BONUS);
	assert(st.happy == PET_HAPPY_MAX);
	pet_cheer(&st, 0);
	pet_cheer(&st, -5);
	assert(st.happy == PET_HAPPY_MAX);
}

static void test_volume_floor(void) {
	assert(!pet_audible_volume(0));
	assert(!pet_audible_volume(PET_VOLUME_FLOOR - 1));
	assert(pet_audible_volume(PET_VOLUME_FLOOR));
	assert(pet_audible_volume(100));
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

	// Two days without music: asleep. It does not say "sad" for a creature
	// that is only resting, and six hours is not neglect.
	assert(pet_mood(&st, T0 + 6 * HOUR) == PET_MOOD_SAD);
	assert(pet_mood(&st, T0 + PET_SLEEP_AFTER_SECS - 1) == PET_MOOD_SAD);
	assert(pet_mood(&st, T0 + PET_SLEEP_AFTER_SECS + 1) == PET_MOOD_SLEEPING);

	// A pat is welcome, but it does not wake the pet: only music does.
	assert(pet_pet(&st, T0 + PET_SLEEP_AFTER_SECS + 2));
	assert(pet_mood(&st, T0 + PET_SLEEP_AFTER_SECS + 3) == PET_MOOD_SLEEPING);

	pet_advance(&st, T0 + PET_SLEEP_AFTER_SECS + 10, DAY0 + 2, 1, 0);
	assert(pet_mood(&st, T0 + PET_SLEEP_AFTER_SECS + 11) == PET_MOOD_SAD);
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

static void test_vibe(void) {
	assert(pet_vibe_for_genre(NULL) == PET_VIBE_NEUTRAL);
	assert(pet_vibe_for_genre("") == PET_VIBE_NEUTRAL);
	assert(pet_vibe_for_genre("Pop") == PET_VIBE_NEUTRAL);
	assert(pet_vibe_for_genre("Rock") == PET_VIBE_ENERGETIC);
	assert(pet_vibe_for_genre("Melodic Death Metal") == PET_VIBE_ENERGETIC);
	assert(pet_vibe_for_genre("Electronic") == PET_VIBE_ENERGETIC);
	assert(pet_vibe_for_genre("Lo-Fi Hip Hop") == PET_VIBE_CALM);
	assert(pet_vibe_for_genre("Jazz") == PET_VIBE_CALM);
	assert(pet_vibe_for_genre("Ambient") == PET_VIBE_CALM);
	assert(pet_vibe_for_genre("  CLASSICAL ") == PET_VIBE_CALM);
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
	st.today_xp_secs = 654;
	st.streak = 4;
	st.goal_day = DAY0 - 1;
	st.freeze_day = DAY0 - 2;
	st.pets = 9;
	st.notes = 123;
	st.note_credit = 17;
	st.bonus_credit = 42;
	st.freezes = 2;
	st.achievements = PET_ACH_FOCUS | PET_ACH_NIGHT_OWL;
	st.owned = (1 << PET_ITEM_PARTY_HAT) | (1 << PET_ITEM_ROOM_NIGHT);
	st.wearing = PET_ITEM_PARTY_HAT;
	st.room = 2;
	st.species = PET_SPECIES_FISH;
	int events = 0;
	pet_note_track(&st, "Jazz", "Kind of Blue", &events);
	pet_note_track(&st, "Rock", "Abbey Road", &events);
	pet_save(&st);

	pet_state_t loaded;
	pet_load(&loaded, T0, DAY0);
	assert(loaded.adopted == st.adopted);
	assert(loaded.happy == st.happy);
	assert(loaded.energy == st.energy);
	assert(loaded.xp_secs == st.xp_secs);
	assert(loaded.today_secs == st.today_secs);
	assert(loaded.today_xp_secs == st.today_xp_secs);
	assert(loaded.streak == st.streak);
	assert(loaded.goal_day == st.goal_day);
	assert(loaded.freeze_day == st.freeze_day);
	assert(loaded.pets == st.pets);
	assert(loaded.notes == st.notes);
	assert(loaded.note_credit == st.note_credit);
	assert(loaded.bonus_credit == st.bonus_credit);
	assert(loaded.freezes == st.freezes);
	assert(loaded.achievements == st.achievements);
	assert(loaded.owned == st.owned);
	assert(loaded.wearing == st.wearing);
	assert(loaded.room == st.room);
	assert(loaded.species == PET_SPECIES_FISH);
	assert(loaded.day == DAY0);
	assert(loaded.day_genre_count == 2);
	assert(loaded.day_album_count == 2);
	assert(loaded.day_genres[0] == st.day_genres[0]);
	assert(loaded.day_genres[1] == st.day_genres[1]);
	assert(loaded.day_albums[0] == st.day_albums[0]);
	assert(loaded.day_albums[1] == st.day_albums[1]);

	// The discovery survives: the same record is not new after a reboot.
	assert(pet_note_track(&loaded, "Jazz", "Kind of Blue", &events) == 0);

	// A file that has never held a pet gives a new one, and writes it.
	char path2[256];
	snprintf(path2, sizeof(path2), "%s/fresh.ini", dir);
	config_init(path2);
	pet_load(&loaded, T0, DAY0);
	assert(loaded.adopted == T0);
	assert(loaded.happy == 50);
	assert(loaded.wearing == -1);
	assert(config_get_int("pet", "happy", -1) == 50);
	assert(config_get_int("pet", "notes", -1) == 0);

	// A hand-edited file beyond the ranges is brought back inside them.
	config_set_int("pet", "happy", 9999);
	config_set_int("pet", "energy", -20);
	config_set_int("pet", "xp_secs", -5);
	config_set_int("pet", "today_xp_secs", 999999);
	config_set_int("pet", "notes", -50);
	config_set_int("pet", "wearing", PET_ITEM_CROWN); // not owned
	config_set_int("pet", "room", 2);		  // not owned either
	pet_load(&loaded, T0, DAY0);
	assert(loaded.adopted == T0); // unchanged by the clamps below
	assert(loaded.happy == PET_HAPPY_MAX);
	assert(loaded.energy == 0);
	assert(loaded.xp_secs == 0);
	assert(loaded.today_xp_secs == PET_DAILY_XP_SECS);
	assert(loaded.notes == 0);
	assert(loaded.wearing == -1);
	assert(loaded.room == 0);
	assert(loaded.species == PET_SPECIES_NONE);

	// A species that is not one of the four is not a species.
	config_set_int("pet", "species", 99);
	pet_load(&loaded, T0, DAY0);
	assert(pet_species(&loaded) == PET_SPECIES_NONE);
}

static void test_species(void) {
	fresh();
	assert(pet_species(&st) == PET_SPECIES_NONE);
	assert(!pet_set_species(&st, PET_SPECIES_NONE));
	assert(!pet_set_species(&st, PET_SPECIES_COUNT));

	assert(pet_set_species(&st, PET_SPECIES_CAT));
	assert(pet_species(&st) == PET_SPECIES_CAT);
	assert(pet_set_species(&st, PET_SPECIES_DOG));
	assert(pet_species(&st) == PET_SPECIES_DOG);
	assert(pet_set_species(&st, PET_SPECIES_DRAGON));
	assert(pet_species(&st) == PET_SPECIES_DRAGON);
	assert(pet_set_species(&st, PET_SPECIES_FISH));
	assert(pet_species(&st) == PET_SPECIES_FISH);

	assert(strcmp(pet_species_tag(PET_SPECIES_CAT), "pet_species_cat") == 0);
	assert(strcmp(pet_species_tag(PET_SPECIES_FISH), "pet_species_fish") == 0);
	assert(strcmp(pet_species_tag(PET_SPECIES_NONE), "pet_species") == 0);
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
	// The page hands it a 25-byte buffer, so the cut is at 24.
	char narrow[25];
	config_set("pet", "name", "Mélodieeeeeeeeeeeeeeeeeeeeeeeeeeeeee");
	pet_name(narrow, sizeof(narrow));
	assert(strlen(narrow) <= 24);
	for (size_t i = 0; narrow[i]; i++) {
		if (((unsigned char)narrow[i] & 0xC0) == 0x80) {
			assert(i > 0);
		}
	}
	assert(strncmp(narrow, "Mélodie", 8) == 0);
}

// ---------------------------------------------------------------------------
// The month: families, temperament and what they change
// ---------------------------------------------------------------------------

static void test_families(void) {
	assert(pet_family_for_genre(NULL) == PET_FAMILY_OTHER);
	assert(pet_family_for_genre("") == PET_FAMILY_OTHER);
	assert(pet_family_for_genre("Rock") == PET_FAMILY_UPBEAT);
	assert(pet_family_for_genre("Synth-pop") == PET_FAMILY_UPBEAT);
	assert(pet_family_for_genre("Hip-Hop/Rap") == PET_FAMILY_UPBEAT);
	assert(pet_family_for_genre("Jazz") == PET_FAMILY_CALM);
	assert(pet_family_for_genre("Lo-Fi Hip Hop") == PET_FAMILY_CALM);
	assert(pet_family_for_genre("Modern Classical") == PET_FAMILY_CALM);
	// The curious ones win over the chart ones: "indie pop" is not pop here.
	assert(pet_family_for_genre("Indie Pop") == PET_FAMILY_CURIOUS);
	assert(pet_family_for_genre("Alternative Rock") == PET_FAMILY_CURIOUS);
	assert(pet_family_for_genre("Post-Rock") == PET_FAMILY_CURIOUS);
	assert(pet_family_for_genre("Melodic Death Metal") == PET_FAMILY_CURIOUS);
	assert(pet_family_for_genre("Audiobook") == PET_FAMILY_OTHER);
}

// A day of one family: note the tracks and let the day turn over.
static void spend_day(const char *genre) {
	int events = 0;
	for (int i = 0; i < 3; i++) {
		pet_note_track(&st, genre, genre, &events);
	}
	next_day();
}

static void test_temperament(void) {
	fresh();
	// A pet on its first evening has no character yet.
	assert(pet_temperament(&st) == PET_TEMPERAMENT_NONE);
	assert(strcmp(pet_temperament_tag(PET_TEMPERAMENT_NONE), "pet_temperament_none") == 0);

	spend_day("Rock");
	assert(pet_temperament(&st) == PET_TEMPERAMENT_NONE); // one day is not a month
	spend_day("Rock");
	assert(pet_temperament(&st) == PET_TEMPERAMENT_NONE); // nor two
	spend_day("Rock");
	assert(pet_temperament(&st) == PET_TEMPERAMENT_UPBEAT);
	assert(pet_is_upbeat(&st));
	assert(!pet_has_soundscape(&st));
	assert(strcmp(pet_temperament_tag(PET_TEMPERAMENT_UPBEAT), "pet_temperament_upbeat") == 0);

	// Jazz days outnumber the rock ones: the temperament follows the month.
	spend_day("Jazz");
	spend_day("Jazz");
	spend_day("Jazz");
	spend_day("Jazz");
	assert(pet_temperament(&st) == PET_TEMPERAMENT_CALM);
	assert(pet_has_soundscape(&st));
	assert(!pet_is_upbeat(&st));
	assert(pet_energy_secs_per_point(&st) == PET_ENERGY_SECS_PER_POINT);

	int days[PET_FAMILY_COUNT];
	pet_temperament_days(&st, days);
	assert(days[PET_FAMILY_CALM] == 4);
	assert(days[PET_FAMILY_UPBEAT] == 3);

	// And a month of indie after that is the curious one, with the soundscape
	// going away again.
	for (int i = 0; i < PET_TEMPERAMENT_DAYS; i++) {
		spend_day("Indie Rock");
	}
	assert(pet_temperament(&st) == PET_TEMPERAMENT_CURIOUS);
	assert(!pet_has_soundscape(&st));

	// A month away empties the ring: a temperament is recent listening or it is
	// nothing.
	pet_advance(&st, st.last_seen, st.day + PET_TEMPERAMENT_DAYS + 1, 0, 0);
	pet_temperament_days(&st, days);
	for (int f = 1; f < PET_FAMILY_COUNT; f++) {
		assert(days[f] == 0);
	}
	assert(pet_temperament(&st) == PET_TEMPERAMENT_NONE);
}

static void test_upbeat_energy_and_drops(void) {
	fresh();
	spend_day("Rock");
	spend_day("Rock");
	spend_day("Rock");
	assert(pet_is_upbeat(&st));

	// Eight seconds of music is a point of energy for an Upbeat pet and less
	// than one for anybody else.
	int energy = st.energy;
	listen(PET_UPBEAT_ENERGY_SECS_PER_POINT);
	assert(st.energy == energy + 1);

	// Notes fall on the floor as the credited music crosses its buckets: the
	// six notes of six minutes of music come with a few more.
	st.notes = 0;
	st.note_credit = 0;
	listen(6 * 60);
	assert(st.notes >= 6);
	assert(st.notes <= 20);

	// The same music pays the same again: the roll is a hash, not a die.
	pet_state_t again = st;
	fresh();
	spend_day("Rock");
	spend_day("Rock");
	spend_day("Rock");
	st.notes = 0;
	st.note_credit = 0;
	listen(6 * 60);
	assert(st.notes == again.notes);
}

static void test_dizzy(void) {
	fresh();
	int64_t base = T0 + HOUR;

	// Five skips in ten minutes is a listener flicking through a library; the
	// sixth is one skip too many.
	for (int i = 0; i < PET_SKIP_LIMIT; i++) {
		assert(!pet_skip(&st, base + i * 20));
	}
	assert(!pet_dizzy(&st, base + 200));
	assert(pet_skip(&st, base + 200));
	assert(pet_dizzy(&st, base + 201));
	assert(pet_mood(&st, base + 201) == PET_MOOD_DIZZY);
	assert(strcmp(pet_mood_tag(PET_MOOD_DIZZY), "pet_mood_dizzy") == 0);
	// Already dizzy: the seventh skip does not report it again.
	assert(!pet_skip(&st, base + 220));

	// Nothing is credited while dizzy, but the meters still fill: the music is
	// real, it is the progression that waits.
	int happy = st.happy;
	int energy = st.energy;
	int64_t xp = st.xp_secs;
	int notes = st.notes;
	st.last_seen = base + 260; // the tick after the skip, not forty minutes later
	pet_advance(&st, base + 300, DAY0, 30, 0);
	assert(st.xp_secs == xp);
	assert(st.notes == notes);
	assert(st.happy > happy || st.happy == PET_HAPPY_MAX);
	assert(st.energy > energy || st.energy == PET_ENERGY_MAX);
	assert(st.today_secs == 30); // the day's minutes still count

	// Fifteen minutes and it lifts.
	assert(pet_dizzy(&st, base + 200 + PET_DIZZY_SECS - 1));
	assert(!pet_dizzy(&st, base + 200 + PET_DIZZY_SECS));
	pet_advance(&st, base + 200 + PET_DIZZY_SECS, DAY0, 30, 0);
	assert(st.xp_secs == xp + 30);

	// Skips long past do not add up: the window is ten minutes.
	fresh();
	for (int i = 0; i < 10; i++) {
		assert(!pet_skip(&st, base + i * (PET_SKIP_WINDOW_SECS + 60)));
	}
	assert(!pet_dizzy(&st, base + 20000));
}

static void test_forage(void) {
	fresh();
	spend_day("Indie Rock");
	spend_day("Indie Rock");
	spend_day("Indie Rock");
	assert(pet_temperament(&st) == PET_TEMPERAMENT_CURIOUS);

	// Forty-four minutes is not a long session yet.
	assert(pet_forage(&st, PET_FORAGE_SECS - 1) == 0);
	int notes = pet_forage(&st, PET_FORAGE_SECS);
	assert(notes > 0);
	assert(st.notes >= notes);
	// Once a day, however long the session runs.
	assert(pet_forage(&st, PET_FORAGE_SECS + 600) == 0);

	// A different temperament finds nothing, however long it listens.
	fresh();
	spend_day("Rock");
	spend_day("Rock");
	spend_day("Rock");
	assert(pet_forage(&st, PET_FORAGE_SECS) == 0);
	assert(st.notes == 0);
}

static void test_rhythm(void) {
	fresh();
	assert(pet_rhythm_ready(&st));

	int before = st.notes;
	int notes = pet_rhythm_finish(&st, 75);
	assert(notes == 2 + 75 / 10);
	assert(st.notes == before + notes);
	assert(st.rhythm_best == 75);
	assert(!pet_rhythm_ready(&st));
	// A second game the same day pays nothing, and does not lower the best.
	assert(pet_rhythm_finish(&st, 10) == 0);
	assert(st.rhythm_best == 75);

	// A clean run leaves a freeze in hand; tomorrow is another game.
	next_day();
	assert(pet_rhythm_ready(&st));
	int freezes = st.freezes;
	assert(pet_rhythm_finish(&st, 95) > 0);
	assert(st.freezes == freezes + 1);
	assert(st.rhythm_best == 95);
	// The score is clamped rather than trusted.
	next_day();
	pet_rhythm_finish(&st, 1000);
	assert(st.rhythm_best == 100);
}

static void test_v4_persistence(void) {
	char dir[] = "/tmp/sonix-pet-test-XXXXXX";
	assert(mkdtemp(dir));
	char path[256];
	snprintf(path, sizeof(path), "%s/device_config.ini", dir);
	config_init(path);

	fresh();
	spend_day("Jazz");
	spend_day("Jazz");
	spend_day("Jazz");
	spend_day("Rock");
	assert(pet_temperament(&st) == PET_TEMPERAMENT_CALM);
	int64_t base = T0 + HOUR;
	for (int i = 0; i <= PET_SKIP_LIMIT; i++) {
		pet_skip(&st, base + i);
	}
	assert(pet_dizzy(&st, base + 10));
	pet_rhythm_finish(&st, 64);
	pet_forage(&st, PET_FORAGE_SECS);
	pet_save(&st);

	pet_state_t loaded;
	pet_load(&loaded, T0, DAY0);
	assert(memcmp(loaded.genre_history, st.genre_history, PET_TEMPERAMENT_DAYS) == 0);
	assert(pet_temperament(&loaded) == PET_TEMPERAMENT_CALM);
	assert(loaded.dizzy_until == st.dizzy_until);
	assert(loaded.rhythm_day == st.rhythm_day);
	assert(loaded.rhythm_best == st.rhythm_best);
	assert(loaded.forage_day == st.forage_day);
	assert(loaded.notes == st.notes);
	assert(loaded.freezes == st.freezes);

	// A history written by a build that did not have one is all zeros, and a
	// corrupt digit is read as "no genre that day" rather than as a family.
	config_set("pet", "genre_history", "3x3");
	pet_load(&loaded, T0, DAY0);
	assert(loaded.genre_history[0] == PET_FAMILY_CURIOUS);
	assert(loaded.genre_history[1] == PET_FAMILY_OTHER);
	assert(loaded.genre_history[2] == PET_FAMILY_CURIOUS);
	assert(pet_temperament(&loaded) == PET_TEMPERAMENT_NONE);
}

int main(void) {
	test_day_number();
	test_defaults();
	test_music_fills_the_meters();
	test_idleness_costs();
	test_offline_time_is_capped();
	test_a_backwards_clock_is_harmless();
	test_the_daily_goal_and_the_streak();
	test_a_freeze_covers_a_missed_day();
	test_growth();
	test_the_daily_xp_ceiling();
	test_diversity_bonus();
	test_polymath();
	test_wallet_and_wardrobe();
	test_achievements();
	test_cheer();
	test_volume_floor();
	test_mood();
	test_petting();
	test_vibe();
	test_persistence();
	test_species();
	test_name();
	test_families();
	test_temperament();
	test_upbeat_energy_and_drops();
	test_dizzy();
	test_forage();
	test_rhythm();
	test_v4_persistence();

	printf("PASS music pet: XP ceiling, bonuses, wallet, goals, stages and persistence\n");
	return 0;
}
