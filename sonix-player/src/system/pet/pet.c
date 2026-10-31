#include "pet.h"

#include <limits.h>
#include <string.h>
#include <time.h>

#include "src/system/core/config.h"

// The arithmetic behind the music pet. See pet.h for what it is and why it is
// kept away from the screen and the audio thread.

#define PET_SECTION "pet"

// Lifetime seconds of music at which each stage begins. The egg is the first
// handful of minutes -- long enough to be noticed, short enough that the first
// album hatches it -- and the last stage is about fifteen hours of listening,
// which at an hour a day is a fortnight.
static const int64_t STAGE_SECS[PET_STAGE_COUNT] = {
	0,		// egg
	5 * 60,		// baby: hatched by the first track or two
	60 * 60,	// child: about an hour
	4 * 60 * 60,	// teen: about four hours
	15 * 60 * 60,	// adult: about fifteen hours
};

static int clamp_int(int value, int low, int high) {
	if (value < low) {
		return low;
	}
	if (value > high) {
		return high;
	}
	return value;
}

static int64_t clamp_i64(int64_t value, int64_t low, int64_t high) {
	if (value < low) {
		return low;
	}
	if (value > high) {
		return high;
	}
	return value;
}

// Days since 1970-01-01 for a civil date, the arithmetic every calendar library
// uses. It is here rather than a call to timegm() because the day has to be the
// LOCAL one: today ends at the midnight the user's clock shows, not at UTC's.
static long days_from_civil(int year, int month, int day) {
	year -= month <= 2;
	int era = (year >= 0 ? year : year - 399) / 400;
	unsigned yoe = (unsigned)(year - era * 400);
	unsigned doy = (unsigned)((153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1);
	unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
	return (long)era * 146097 + (long)doe - 719468;
}

long pet_day_now(int64_t now) {
	if (now <= 0) {
		return 0;
	}

	time_t t = (time_t)now;
	struct tm local;
	if (!localtime_r(&t, &local)) {
		return (long)(now / 86400);
	}
	return days_from_civil(local.tm_year + 1900, local.tm_mon + 1, local.tm_mday);
}

void pet_defaults(pet_state_t *st, int64_t now, long day) {
	if (!st) {
		return;
	}
	memset(st, 0, sizeof(*st));

	// `adopted` doubles as "there is a pet here". A clock that cannot be
	// trusted still leaves 1 behind, which is a truthy adoption time the
	// arithmetic never uses for anything but this test.
	st->adopted = now > 0 ? now : 1;
	st->last_seen = now;
	st->last_music = now;
	st->day = day;
	st->happy = 50;
	st->energy = 80;
}

int pet_advance(pet_state_t *st, int64_t now, long day, int listened_secs) {
	if (!st) {
		return PET_EVENT_NONE;
	}
	if (listened_secs < 0) {
		listened_secs = 0;
	}
	if (listened_secs > 3600) {
		listened_secs = 3600; // a caller cannot claim more than an hour in one go
	}

	const pet_stage_t stage_before = pet_stage(st);
	int events = PET_EVENT_NONE;

	// ---- the day ---------------------------------------------------------
	//
	// A day the caller cannot name is not a day: the counters are left alone.
	// A real day that is not the stored one ends yesterday, which is where the
	// streak is settled: a goal missed yesterday breaks it, a goal reached
	// yesterday keeps it.
	if (day > 0) {
		if (st->day == 0) {
			st->day = day;
		} else if (day > st->day) {
			// One day later and yesterday's goal was reached: the run goes
			// on. One day later and it was not, or several days gone at
			// once: the run ends. A clock moved backwards is neither, and
			// keeps the run it had.
			if (day - st->day != 1 || st->goal_day != st->day) {
				st->streak = 0;
			}
			st->day = day;
			st->today_secs = 0;
		} else {
			st->day = day;
			st->today_secs = 0;
		}
	}

	// ---- how much time passed -------------------------------------------
	int64_t elapsed = 0;
	if (now > 0 && st->last_seen > 0 && now > st->last_seen) {
		elapsed = now - st->last_seen;
	}
	elapsed = clamp_i64(elapsed, 0, PET_MAX_ELAPSED_SECS);

	// Time that was not music: the part of the gap the caller did not spend
	// listening. A suspend across an album is a gap with music on both sides
	// of it, and only its non-listening part should make the pet hungrier.
	int64_t idle_secs = elapsed - listened_secs;
	if (idle_secs < 0) {
		idle_secs = 0;
	}
	if (idle_secs > INT_MAX) {
		idle_secs = INT_MAX;
	}

	// ---- music -----------------------------------------------------------
	if (listened_secs > 0) {
		if (now > 0) {
			st->last_music = now;
		} else if (st->last_music == 0) {
			st->last_music = 1; // "it has heard music", on a clock we cannot trust
		}

		if (st->xp_secs > INT64_MAX - listened_secs) {
			st->xp_secs = INT64_MAX; // paranoia: never negative from wrapping
		} else {
			st->xp_secs += listened_secs;
		}
		st->today_secs += listened_secs;
		if (st->today_secs > 86400) {
			st->today_secs = 86400;
		}

		// Energy first: it is what makes the happiness gain worth anything.
		if (st->energy < PET_ENERGY_MAX) {
			st->energy_credit += listened_secs;
			int whole = st->energy_credit / PET_ENERGY_SECS_PER_POINT;
			if (whole > 0) {
				st->energy = clamp_int(st->energy + whole, 0, PET_ENERGY_MAX);
				st->energy_credit -= whole * PET_ENERGY_SECS_PER_POINT;
				if (st->energy >= PET_ENERGY_MAX) {
					st->energy_credit = 0;
				}
			}
		} else {
			st->energy_credit = 0;
		}

		// Happiness moves at half speed on an empty battery: a pet that has
		// not been fed for a day has to be fed before it perks up, and the
		// energy this same music is giving it is what does that.
		if (st->happy < PET_HAPPY_MAX) {
			int step = st->energy == 0 ? PET_HUNGRY_HAPPY_SECS_PER_POINT : PET_HAPPY_SECS_PER_POINT;
			st->happy_credit += listened_secs;
			int whole = st->happy_credit / step;
			if (whole > 0) {
				st->happy = clamp_int(st->happy + whole, 0, PET_HAPPY_MAX);
				st->happy_credit -= whole * step;
				if (st->happy >= PET_HAPPY_MAX) {
					st->happy_credit = 0;
				}
			}
		} else {
			st->happy_credit = 0;
		}
	}

	// ---- idleness --------------------------------------------------------
	//
	// The two credits are kept apart from the music ones on purpose: a track
	// after an hour of silence has to pay the hour's decay as well as earn the
	// track's few percent.
	if (idle_secs > 0) {
		if (st->happy > 0) {
			st->idle_happy_credit += (int)idle_secs;
			int whole = st->idle_happy_credit / PET_IDLE_HAPPY_SECS_PER_POINT;
			if (whole > 0) {
				st->happy = clamp_int(st->happy - whole, 0, PET_HAPPY_MAX);
				st->idle_happy_credit -= whole * PET_IDLE_HAPPY_SECS_PER_POINT;
				if (st->happy == 0) {
					st->idle_happy_credit = 0;
				}
			}
		} else {
			st->idle_happy_credit = 0;
		}

		if (st->energy > 0) {
			st->idle_energy_credit += (int)idle_secs;
			int whole = st->idle_energy_credit / PET_IDLE_ENERGY_SECS_PER_POINT;
			if (whole > 0) {
				st->energy = clamp_int(st->energy - whole, 0, PET_ENERGY_MAX);
				st->idle_energy_credit -= whole * PET_IDLE_ENERGY_SECS_PER_POINT;
				if (st->energy == 0) {
					st->idle_energy_credit = 0;
				}
			}
		} else {
			st->idle_energy_credit = 0;
		}
	}

	// ---- the daily goal --------------------------------------------------
	if (st->day > 0 && st->today_secs >= PET_DAY_GOAL_SECS && st->goal_day != st->day) {
		if (st->goal_day == st->day - 1) {
			st->streak++;
		} else {
			st->streak = 1;
		}
		st->goal_day = st->day;
		events |= PET_EVENT_GOAL_MET;
	}

	// ---- the next stage --------------------------------------------------
	if (pet_stage(st) != stage_before) {
		events |= PET_EVENT_STAGE_UP;
	}

	if (now > 0) {
		st->last_seen = now;
	}
	return events;
}

bool pet_pet(pet_state_t *st, int64_t now) {
	if (!st) {
		return false;
	}

	// A clock that went backwards (or has never been set) is not a cooldown:
	// refusing a touch because the stored time is in the future would leave
	// the button dead until the clock caught up.
	if (now > 0 && st->last_pet > 0 && now >= st->last_pet && now - st->last_pet < PET_PET_COOLDOWN_SECS) {
		return false;
	}

	st->happy = clamp_int(st->happy + PET_PET_HAPPY_GAIN, 0, PET_HAPPY_MAX);
	st->energy = clamp_int(st->energy + PET_PET_ENERGY_GAIN, 0, PET_ENERGY_MAX);
	if (st->pets < INT_MAX) {
		st->pets++;
	}
	if (now > 0) {
		st->last_pet = now;
	} else {
		st->last_pet = 1;
	}
	return true;
}

pet_stage_t pet_stage(const pet_state_t *st) {
	int64_t xp = st ? st->xp_secs : 0;
	if (xp < 0) {
		xp = 0;
	}

	pet_stage_t stage = PET_STAGE_EGG;
	for (int i = 1; i < PET_STAGE_COUNT; i++) {
		if (xp >= STAGE_SECS[i]) {
			stage = (pet_stage_t)i;
		}
	}
	return stage;
}

int pet_stage_percent(const pet_state_t *st) {
	pet_stage_t stage = pet_stage(st);
	if (stage >= PET_STAGE_ADULT) {
		return 100;
	}

	int64_t floor = STAGE_SECS[stage];
	int64_t ceiling = STAGE_SECS[stage + 1];
	int64_t span = ceiling - floor;
	if (span <= 0) {
		return 100;
	}
	int64_t done = (st ? st->xp_secs : 0) - floor;
	return clamp_int((int)(done * 100 / span), 0, 100);
}

int64_t pet_stage_remaining(const pet_state_t *st) {
	pet_stage_t stage = pet_stage(st);
	if (stage >= PET_STAGE_ADULT) {
		return 0;
	}
	int64_t left = STAGE_SECS[stage + 1] - (st ? st->xp_secs : 0);
	return left > 0 ? left : 0;
}

bool pet_goal_met_today(const pet_state_t *st) {
	return st && st->day > 0 && st->goal_day == st->day;
}

pet_mood_t pet_mood(const pet_state_t *st, int64_t now) {
	if (!st) {
		return PET_MOOD_OKAY;
	}

	// Asleep when neither music nor a finger has come near it for six hours.
	// A petting wakes it: `last_pet` is as good a "somebody is here" as the
	// last track.
	int64_t last_attention = st->last_music;
	if (st->last_pet > last_attention) {
		last_attention = st->last_pet;
	}
	if (now > 0 && last_attention > 0 && now > last_attention && now - last_attention > PET_SLEEP_AFTER_SECS) {
		return PET_MOOD_SLEEPING;
	}

	int happy = clamp_int(st->happy, 0, PET_HAPPY_MAX);
	if (happy >= 80) {
		return PET_MOOD_BLISSFUL;
	}
	if (happy >= 50) {
		return PET_MOOD_HAPPY;
	}
	if (happy >= 25) {
		return PET_MOOD_OKAY;
	}
	return PET_MOOD_SAD;
}

int pet_hearts(const pet_state_t *st) {
	int happy = clamp_int(st ? st->happy : 0, 0, PET_HAPPY_MAX);
	int hearts = (happy * 5 + 50) / 100; // rounded to the nearest whole heart
	return clamp_int(hearts, 0, 5);
}

const char *pet_mood_tag(pet_mood_t mood) {
	switch (mood) {
	case PET_MOOD_SLEEPING:
		return "pet_mood_sleeping";
	case PET_MOOD_SAD:
		return "pet_mood_sad";
	case PET_MOOD_OKAY:
		return "pet_mood_okay";
	case PET_MOOD_HAPPY:
		return "pet_mood_happy";
	case PET_MOOD_BLISSFUL:
		return "pet_mood_blissful";
	default:
		return "pet_mood_okay";
	}
}

const char *pet_stage_tag(pet_stage_t stage) {
	switch (stage) {
	case PET_STAGE_EGG:
		return "pet_stage_egg";
	case PET_STAGE_BABY:
		return "pet_stage_baby";
	case PET_STAGE_CHILD:
		return "pet_stage_child";
	case PET_STAGE_TEEN:
		return "pet_stage_teen";
	case PET_STAGE_ADULT:
		return "pet_stage_adult";
	default:
		return "pet_stage_egg";
	}
}

// ---------------------------------------------------------------------------
// The configuration
// ---------------------------------------------------------------------------

void pet_load(pet_state_t *st, int64_t now, long day) {
	if (!st) {
		return;
	}
	memset(st, 0, sizeof(*st));

	st->adopted = (int64_t)config_get_int(PET_SECTION, "adopted", 0);
	if (st->adopted <= 0) {
		// No pet yet. The defaults are written now rather than on the first
		// save, so a config read over ADB afterwards shows what the pet is.
		pet_defaults(st, now, day);
		pet_save(st);
		return;
	}

	st->last_seen = (int64_t)config_get_int(PET_SECTION, "last_seen", 0);
	st->last_music = (int64_t)config_get_int(PET_SECTION, "last_music", 0);
	st->last_pet = (int64_t)config_get_int(PET_SECTION, "last_pet", 0);
	st->xp_secs = (int64_t)config_get_int(PET_SECTION, "xp_secs", 0);
	st->day = config_get_int(PET_SECTION, "day", 0);
	st->today_secs = (int)config_get_int(PET_SECTION, "today_secs", 0);
	st->happy = (int)config_get_int(PET_SECTION, "happy", 50);
	st->energy = (int)config_get_int(PET_SECTION, "energy", 80);
	st->streak = (int)config_get_int(PET_SECTION, "streak", 0);
	st->goal_day = config_get_int(PET_SECTION, "goal_day", 0);
	st->happy_credit = (int)config_get_int(PET_SECTION, "happy_credit", 0);
	st->energy_credit = (int)config_get_int(PET_SECTION, "energy_credit", 0);
	st->idle_happy_credit = (int)config_get_int(PET_SECTION, "idle_happy_credit", 0);
	st->idle_energy_credit = (int)config_get_int(PET_SECTION, "idle_energy_credit", 0);
	st->pets = (int)config_get_int(PET_SECTION, "pets", 0);

	// A hand edit, a half-written file or a state from a much older build is
	// brought back inside the ranges this arithmetic assumes.
	if (st->xp_secs < 0) {
		st->xp_secs = 0;
	}
	st->today_secs = clamp_int(st->today_secs, 0, 86400);
	st->happy = clamp_int(st->happy, 0, PET_HAPPY_MAX);
	st->energy = clamp_int(st->energy, 0, PET_ENERGY_MAX);
	st->streak = clamp_int(st->streak, 0, 100000);
	st->happy_credit = clamp_int(st->happy_credit, 0, PET_HAPPY_SECS_PER_POINT);
	st->energy_credit = clamp_int(st->energy_credit, 0, PET_ENERGY_SECS_PER_POINT);
	st->idle_happy_credit = clamp_int(st->idle_happy_credit, 0, PET_IDLE_HAPPY_SECS_PER_POINT);
	st->idle_energy_credit = clamp_int(st->idle_energy_credit, 0, PET_IDLE_ENERGY_SECS_PER_POINT);
	st->pets = clamp_int(st->pets, 0, INT_MAX);
	if (st->day < 0) {
		st->day = 0;
	}
	if (st->goal_day < 0) {
		st->goal_day = 0;
	}
	if (st->last_seen <= 0) {
		st->last_seen = now;
	}
	if (st->last_music <= 0) {
		st->last_music = st->adopted;
	}
	if (st->last_pet < 0) {
		st->last_pet = 0;
	}
}

void pet_save(const pet_state_t *st) {
	if (!st) {
		return;
	}

	// config_set_int() leaves the file alone when the value is what is already
	// there, so this can be called as often as a page likes: the write happens
	// only for the fields that moved.
	config_set_int(PET_SECTION, "adopted", (long)clamp_i64(st->adopted, 0, LONG_MAX));
	config_set_int(PET_SECTION, "last_seen", (long)clamp_i64(st->last_seen, 0, LONG_MAX));
	config_set_int(PET_SECTION, "last_music", (long)clamp_i64(st->last_music, 0, LONG_MAX));
	config_set_int(PET_SECTION, "last_pet", (long)clamp_i64(st->last_pet, 0, LONG_MAX));
	config_set_int(PET_SECTION, "xp_secs", (long)clamp_i64(st->xp_secs, 0, LONG_MAX));
	config_set_int(PET_SECTION, "day", st->day);
	config_set_int(PET_SECTION, "today_secs", st->today_secs);
	config_set_int(PET_SECTION, "happy", st->happy);
	config_set_int(PET_SECTION, "energy", st->energy);
	config_set_int(PET_SECTION, "streak", st->streak);
	config_set_int(PET_SECTION, "goal_day", st->goal_day);
	config_set_int(PET_SECTION, "happy_credit", st->happy_credit);
	config_set_int(PET_SECTION, "energy_credit", st->energy_credit);
	config_set_int(PET_SECTION, "idle_happy_credit", st->idle_happy_credit);
	config_set_int(PET_SECTION, "idle_energy_credit", st->idle_energy_credit);
	config_set_int(PET_SECTION, "pets", st->pets);

	config_save();
}

void pet_name(char *out, size_t out_size) {
	if (!out || out_size == 0) {
		return;
	}

	const char *name = config_get(PET_SECTION, "name", "Momo");
	while (*name == ' ' || *name == '\t') {
		name++;
	}

	size_t len = strlen(name);
	while (len > 0 && (name[len - 1] == ' ' || name[len - 1] == '\t' || name[len - 1] == '\r' || name[len - 1] == '\n')) {
		len--;
	}

	// The page draws the name on one line and clips it, but a clip in the
	// middle of a UTF-8 sequence is a missing glyph. Back off to a character
	// boundary before the buffer ends.
	size_t max = 24;
	if (max > out_size - 1) {
		max = out_size - 1;
	}
	if (len > max) {
		len = max;
		while (len > 0 && ((unsigned char)name[len] & 0xC0) == 0x80) {
			len--;
		}
	}

	if (len == 0) {
		name = "Momo";
		len = strlen(name);
		if (len > out_size - 1) {
			len = out_size - 1;
		}
	}

	memcpy(out, name, len);
	out[len] = '\0';
}
