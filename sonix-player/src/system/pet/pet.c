#include "pet.h"

#include <ctype.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "src/system/core/config.h"

// The arithmetic behind the music pet. See pet.h for what it is and why it is
// kept away from the screen and the audio thread.

#define PET_SECTION "pet"

// Lifetime credited seconds of music at which each stage begins. The first is
// the animal as it arrives; the last is about fifteen hours of listening. At
// the four-hour daily ceiling that is four full days of music, and at an hour a
// day a fortnight; an overnight player gains no shortcut.
static const int64_t STAGE_SECS[PET_STAGE_COUNT] = {
	0,		// baby: the animal as it arrives
	30 * 60,	// child: half an hour of music
	4 * 60 * 60,	// teen: about four hours
	15 * 60 * 60,	// adult: about fifteen hours
};

typedef struct {
	int bit;
	int notes;
} achievement_reward_t;

static const achievement_reward_t ACHIEVEMENT_REWARDS[] = {
	{PET_ACH_NIGHT_OWL, 20},
	{PET_ACH_DEEP_DIVER, 30},
	{PET_ACH_POLYMATH, 25},
	{PET_ACH_FOCUS, 20},
};

// What the wardrobe costs, in notes. A note is a minute of music, so the first
// outfit is about two hours of listening and the crown is an indulgence.
static const int ITEM_PRICES[PET_ITEM_COUNT] = {
	120, // party hat
	200, // headphones
	150, // scarf
	500, // crown
	180, // sunset room
	240, // night room
	150, // streak freeze
};

static const char *const ITEM_TAGS[PET_ITEM_COUNT] = {
	"pet_item_party_hat", "pet_item_headphones", "pet_item_scarf", "pet_item_crown",
	"pet_item_room_sunset", "pet_item_room_night", "pet_item_streak_freeze",
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

// ---------------------------------------------------------------------------
// The day's discoveries
//
// A genre and an album are remembered by a hash, not by their text: the state
// has sixteen counters already and the settings file is read by a person, so
// eight 32-bit numbers written as one short line is a better neighbour than
// eight more strings. FNV-1a, lowercased and trimmed, which is enough to tell
// "Rock" from "Jazz" and "rock" from "Rock" being the same.
// ---------------------------------------------------------------------------

static uint32_t text_hash(const char *text) {
	if (!text) {
		return 0;
	}
	while (*text && isspace((unsigned char)*text)) {
		text++;
	}
	const char *end = text + strlen(text);
	while (end > text && isspace((unsigned char)end[-1])) {
		end--;
	}
	uint32_t hash = 2166136261u;
	for (const char *p = text; p < end; p++) {
		unsigned char c = (unsigned char)*p;
		if (c >= 'A' && c <= 'Z') {
			c = (unsigned char)(c - 'A' + 'a');
		}
		hash ^= c;
		hash *= 16777619u;
	}
	return hash;
}

static bool hash_add(uint32_t *set, int *count, const char *text) {
	if (!text) {
		return false;
	}
	// An empty tag is not a discovery, and the FNV of "" is not zero, so the
	// emptiness is checked on the text rather than on the hash.
	const char *first = text;
	while (*first && isspace((unsigned char)*first)) {
		first++;
	}
	if (!*first) {
		return false;
	}
	uint32_t hash = text_hash(text);
	if (hash == 0 || *count >= PET_DAY_KEYS) {
		return false;
	}
	for (int i = 0; i < *count; i++) {
		if (set[i] == hash) {
			return false;
		}
	}
	set[(*count)++] = hash;
	return true;
}

int pet_note_track(pet_state_t *st, const char *genre, const char *album, int *events) {
	if (events) {
		*events = PET_EVENT_NONE;
	}
	if (!st) {
		return 0;
	}

	bool genre_new = hash_add(st->day_genres, &st->day_genre_count, genre);
	bool album_new = hash_add(st->day_albums, &st->day_album_count, album);

	// What kind of music the day was spent in, for the month's temperament.
	int family = (int)pet_family_for_genre(genre);
	if (family > PET_FAMILY_OTHER && family < PET_FAMILY_COUNT && st->day_family[family] < 100000) {
		st->day_family[family]++;
	}
	int bonus = (genre_new || album_new) ? PET_DIVERSITY_BONUS_PERCENT : 0;

	// Five genres in a day is Polymath. The award carries its own notes, and
	// the caller only has to say that it happened.
	if (genre_new && st->day_genre_count >= 5 && !(st->achievements & PET_ACH_POLYMATH)) {
		if (pet_award_achievement(st, PET_ACH_POLYMATH) && events) {
			*events |= PET_EVENT_POLYMATH;
		}
	}
	return bonus;
}

bool pet_audible_volume(int percent) { return percent >= PET_VOLUME_FLOOR; }

void pet_cheer(pet_state_t *st, int happy_points) {
	if (!st || happy_points <= 0) {
		return;
	}
	st->happy = clamp_int(st->happy + happy_points, 0, PET_HAPPY_MAX);
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
	st->wearing = PET_ITEM_NONE;
	st->room = 0;
	st->species = PET_SPECIES_NONE;
}

// Defined with the month's listening, below; the day rollover in pet_advance()
// is the only caller and it comes first in the file.
static void record_day_family(pet_state_t *st, long day);

int pet_advance(pet_state_t *st, int64_t now, long day, int listened_secs, int bonus_percent) {
	if (!st) {
		return PET_EVENT_NONE;
	}
	if (listened_secs < 0) {
		listened_secs = 0;
	}
	if (listened_secs > 3600) {
		listened_secs = 3600; // a caller cannot claim more than an hour in one go
	}
	if (bonus_percent < 0) {
		bonus_percent = 0;
	}
	if (bonus_percent > 100) {
		bonus_percent = 100;
	}

	const pet_stage_t stage_before = pet_stage(st);
	int events = PET_EVENT_NONE;

	// ---- the day ---------------------------------------------------------
	//
	// A day the caller cannot name is not a day: the counters are left alone.
	// A real day that is not the stored one ends yesterday, which is where the
	// streak is settled. A goal missed yesterday breaks it -- unless a bought
	// freeze covers that day, which is what the freeze is for -- and a goal
	// reached yesterday keeps it.
	if (day > 0) {
		if (st->day == 0) {
			st->day = day;
		} else if (day > st->day) {
			// Only the very next day continues a run. A jump of two days is a
			// day missed in the middle, and one freeze cannot cover a hole it
			// was not spent on.
			bool consecutive = day - st->day == 1;
			bool met = consecutive && st->goal_day == st->day;
			bool frozen = consecutive && st->freeze_day == st->day;

			if (!met && !frozen) {
				if (consecutive && st->streak > 0 && st->freezes > 0) {
					st->freezes--;
					st->freeze_day = st->day;
				} else {
					st->streak = 0;
				}
			}
			// Yesterday, as a line of the month: this is what the
			// temperament is read from.
			record_day_family(st, day);
			st->day = day;
			st->today_secs = 0;
			st->today_xp_secs = 0;
			st->day_genre_count = 0;
			st->day_album_count = 0;
		}
		// A day behind the stored one is a clock that was changed, not a new
		// day: the counters stay where they are rather than being wiped.
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

		// XP and notes stop at the daily ceiling. The meters do not: the music
		// is real even when the day's progression has already been earned.
		// While the pet is dizzy they stop at zero instead -- flicking through
		// a library is not listening -- but the meters still fill.
		int remaining = pet_dizzy(st, now) ? 0 : PET_DAILY_XP_SECS - st->today_xp_secs;
		if (remaining < 0) {
			remaining = 0;
		}
		int base = listened_secs < remaining ? listened_secs : remaining;
		int extra = 0;
		if (base > 0 && bonus_percent > 0) {
			st->bonus_credit += base * bonus_percent;
			extra = st->bonus_credit / 100;
			st->bonus_credit %= 100;
			if (extra > remaining - base) {
				extra = remaining - base;
			}
		}
		int credited = base + extra;
		st->today_xp_secs += credited;

		if (st->xp_secs > INT64_MAX - credited) {
			st->xp_secs = INT64_MAX; // paranoia: never negative from wrapping
		} else {
			st->xp_secs += credited;
		}
		st->note_credit += credited;
		if (st->note_credit >= 60) {
			st->notes += st->note_credit / 60;
			st->note_credit %= 60;
		}

		// An Upbeat pet finds a note or two on the floor every so often. The
		// roll is a hash of the day and the bucket of credited music, so the
		// same listening always pays the same and no random number generator
		// has to live in a settings file.
		if (pet_is_upbeat(st)) {
			int before = (st->today_xp_secs - credited) / PET_DROP_EVERY_SECS;
			int after = st->today_xp_secs / PET_DROP_EVERY_SECS;
			for (int bucket = before + 1; bucket <= after; bucket++) {
				uint32_t roll = (uint32_t)(st->day * 2654435761u) ^ (uint32_t)((uint32_t)bucket * 40503u + 12345u);
				if ((int)(roll % 100) < PET_DROP_CHANCE_PERCENT) {
					st->notes = clamp_int(st->notes + 1 + (int)((roll >> 8) % 3u), 0, INT_MAX);
					events |= PET_EVENT_DROP;
				}
			}
		}

		st->today_secs += listened_secs;
		if (st->today_secs > 86400) {
			st->today_secs = 86400;
		}

		// Energy first: it is what makes the happiness gain worth anything.
		if (st->energy < PET_ENERGY_MAX) {
			// An Upbeat pet's energy fills faster; that is what its month of
			// pop bought it.
			int rate = pet_energy_secs_per_point(st);
			st->energy_credit += listened_secs;
			int whole = st->energy_credit / rate;
			if (whole > 0) {
				st->energy = clamp_int(st->energy + whole, 0, PET_ENERGY_MAX);
				st->energy_credit -= whole * rate;
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
		if (st->goal_day == st->day - 1 || st->freeze_day == st->day - 1) {
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

	pet_stage_t stage = PET_STAGE_BABY;
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

	// Two days without music and it sleeps. Petting is welcome but does not
	// wake it: the page has a line saying what does, and it is a track.
	if (now > 0 && st->last_music > 0 && now > st->last_music && now - st->last_music > PET_SLEEP_AFTER_SECS) {
		return PET_MOOD_SLEEPING;
	}

	// Dizzy is a mood of its own and outranks the meters: the face is what
	// says why nothing is being credited.
	if (pet_dizzy(st, now)) {
		return PET_MOOD_DIZZY;
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
	case PET_MOOD_DIZZY:
		return "pet_mood_dizzy";
	default:
		return "pet_mood_okay";
	}
}

const char *pet_stage_tag(pet_stage_t stage) {
	switch (stage) {
	case PET_STAGE_CHILD:
		return "pet_stage_child";
	case PET_STAGE_TEEN:
		return "pet_stage_teen";
	case PET_STAGE_ADULT:
		return "pet_stage_adult";
	case PET_STAGE_BABY:
	default:
		return "pet_stage_baby";
	}
}

// ---------------------------------------------------------------------------
// The animal
// ---------------------------------------------------------------------------

pet_species_t pet_species(const pet_state_t *st) {
	if (!st || st->species < 1 || st->species >= PET_SPECIES_COUNT) {
		return PET_SPECIES_NONE;
	}
	return (pet_species_t)st->species;
}

bool pet_set_species(pet_state_t *st, pet_species_t species) {
	if (!st || species <= PET_SPECIES_NONE || species >= PET_SPECIES_COUNT) {
		return false;
	}
	st->species = (int)species;
	return true;
}

const char *pet_species_tag(pet_species_t species) {
	switch (species) {
	case PET_SPECIES_CAT:
		return "pet_species_cat";
	case PET_SPECIES_DOG:
		return "pet_species_dog";
	case PET_SPECIES_FISH:
		return "pet_species_fish";
	case PET_SPECIES_NONE:
	default:
		return "pet_species";
	}
}

// ---------------------------------------------------------------------------
// The wardrobe
// ---------------------------------------------------------------------------

static bool item_is_room(pet_item_t item) { return item >= PET_ITEM_FIRST_ROOM && item <= PET_ITEM_ROOM_NIGHT; }

int pet_item_price(pet_item_t item) {
	if ((int)item < 0 || item >= PET_ITEM_COUNT) {
		return -1;
	}
	return ITEM_PRICES[item];
}

const char *pet_item_tag(pet_item_t item) {
	if ((int)item < 0 || item >= PET_ITEM_COUNT) {
		return "pet_item_none";
	}
	return ITEM_TAGS[item];
}

const char *pet_achievement_tag(int achievement) {
	switch (achievement) {
	case PET_ACH_NIGHT_OWL:
		return "pet_ach_night_owl";
	case PET_ACH_DEEP_DIVER:
		return "pet_ach_deep_diver";
	case PET_ACH_POLYMATH:
		return "pet_ach_polymath";
	case PET_ACH_FOCUS:
		return "pet_ach_focus";
	default:
		return "pet_ach_locked";
	}
}

bool pet_owned(const pet_state_t *st, pet_item_t item) {
	if (!st || (int)item < 0 || item >= PET_ITEM_COUNT) {
		return false;
	}
	if (item == PET_ITEM_STREAK_FREEZE) {
		return st->freezes > 0;
	}
	return (st->owned & (1 << (int)item)) != 0;
}

bool pet_wear(pet_state_t *st, pet_item_t item) {
	if (!st) {
		return false;
	}
	if (item == PET_ITEM_NONE) {
		st->wearing = PET_ITEM_NONE;
		return true;
	}
	if ((int)item < 0 || item >= PET_ITEM_COUNT || !pet_owned(st, item)) {
		return false;
	}
	if (item == PET_ITEM_STREAK_FREEZE) {
		return false; // not something a pet can wear
	}
	if (item_is_room(item)) {
		st->room = 1 + ((int)item - (int)PET_ITEM_FIRST_ROOM);
	} else {
		st->wearing = (int)item;
	}
	return true;
}

bool pet_buy(pet_state_t *st, pet_item_t item) {
	if (!st || (int)item < 0 || item >= PET_ITEM_COUNT) {
		return false;
	}
	int price = ITEM_PRICES[item];

	if (item == PET_ITEM_STREAK_FREEZE) {
		if (st->notes < price || st->freezes >= 999) {
			return false;
		}
		st->notes -= price;
		st->freezes++;
		return true;
	}

	if (st->owned & (1 << (int)item)) {
		return false; // already in the wardrobe
	}
	if (st->notes < price) {
		return false;
	}
	st->notes -= price;
	st->owned |= 1 << (int)item;
	pet_wear(st, item); // the thing just bought is the thing being worn
	return true;
}

int pet_wearing(const pet_state_t *st) { return st ? st->wearing : -1; }

int pet_room(const pet_state_t *st) { return st ? clamp_int(st->room, 0, 2) : 0; }

int pet_freezes(const pet_state_t *st) { return st ? st->freezes : 0; }

bool pet_has_achievement(const pet_state_t *st, int achievement) { return st && (st->achievements & achievement) != 0; }

bool pet_award_achievement(pet_state_t *st, int achievement) {
	if (!st) {
		return false;
	}
	for (size_t i = 0; i < sizeof(ACHIEVEMENT_REWARDS) / sizeof(ACHIEVEMENT_REWARDS[0]); i++) {
		if (ACHIEVEMENT_REWARDS[i].bit != achievement) {
			continue;
		}
		if (st->achievements & achievement) {
			return false;
		}
		st->achievements |= achievement;
		st->notes += ACHIEVEMENT_REWARDS[i].notes;
		return true;
	}
	return false;
}

// ---------------------------------------------------------------------------
// The vibe a genre gives the page
// ---------------------------------------------------------------------------

static bool contains_word(const char *text, const char *word) { return strstr(text, word) != NULL; }

pet_vibe_t pet_vibe_for_genre(const char *genre) {
	if (!genre || !genre[0]) {
		return PET_VIBE_NEUTRAL;
	}

	char lower[64];
	size_t used = 0;
	for (const char *p = genre; *p && used < sizeof(lower) - 1; p++) {
		lower[used++] = (char)tolower((unsigned char)*p);
	}
	lower[used] = '\0';

	// Calm first: "lo-fi hip hop" is calm, though "hip hop" alone is not.
	static const char *const calm[] = {
		"ambient", "lo-fi", "lofi", "chill", "classical", "acoustic", "folk", "jazz", "blues",
		"soul",    "downtempo", "new age", "piano", "bossa", "sleep", "meditation", "ballad",
	};
	for (size_t i = 0; i < sizeof(calm) / sizeof(calm[0]); i++) {
		if (contains_word(lower, calm[i])) {
			return PET_VIBE_CALM;
		}
	}

	static const char *const energetic[] = {
		"electronic", "dance",  "edm",       "house",   "techno",    "trance", "drum",  "dnb",
		"rock",       "metal",  "punk",      "hardcore", "hip-hop",  "hip hop", "rap",   "breakbeat",
		"synthwave",  "industrial", "disco", "funk",    "ska",       "dubstep", "bass",  "gabber",
	};
	for (size_t i = 0; i < sizeof(energetic) / sizeof(energetic[0]); i++) {
		if (contains_word(lower, energetic[i])) {
			return PET_VIBE_ENERGETIC;
		}
	}
	return PET_VIBE_NEUTRAL;
}

// ---------------------------------------------------------------------------
// The month's listening: family, temperament, and what they change
// ---------------------------------------------------------------------------

static void lower_copy(char *out, size_t size, const char *text) {
	size_t used = 0;
	if (!out || size == 0) {
		return;
	}
	if (text) {
		for (const char *p = text; *p && used < size - 1; p++) {
			out[used++] = (char)tolower((unsigned char)*p);
		}
	}
	out[used] = '\0';
}

// Which family a tag belongs to. Three rules, in order: calm first, so that
// "lo-fi hip hop" is calm and not upbeat; then the curious ones, so "indie pop"
// and "alternative rock" are exploring rather than chart music; then everything
// that is meant to be danced to.
pet_family_t pet_family_for_genre(const char *genre) {
	if (!genre || !genre[0]) {
		return PET_FAMILY_OTHER;
	}

	char lower[64];
	lower_copy(lower, sizeof(lower), genre);

	static const char *const calm[] = {
		"ambient", "lo-fi", "lofi", "chill", "classical", "acoustic", "folk", "jazz", "blues",
		"soul",    "downtempo", "new age", "piano", "bossa", "sleep", "meditation", "ballad",
		"orchestra", "chamber", "swing", "lounge", "gospel", "celtic",
	};
	static const char *const curious[] = {
		"indie", "alternative", "alt-", "experimental", "avant", "shoegaze", "post-", "math",
		"punk", "metal", "noise", "garage", "psych", "emo", "grunge", "hardcore",
	};
	static const char *const upbeat[] = {
		"pop", "dance", "edm", "house", "techno", "electronic", "hip-hop", "hip hop", "rap",
		"funk", "disco", "r&b", "rnb", "rock", "synthwave", "trance", "drum", "dnb", "dubstep",
		"bass", "breakbeat", "ska", "reggaeton", "latin", "country",
	};

	for (size_t i = 0; i < sizeof(calm) / sizeof(calm[0]); i++) {
		if (contains_word(lower, calm[i])) {
			return PET_FAMILY_CALM;
		}
	}
	for (size_t i = 0; i < sizeof(curious) / sizeof(curious[0]); i++) {
		if (contains_word(lower, curious[i])) {
			return PET_FAMILY_CURIOUS;
		}
	}
	for (size_t i = 0; i < sizeof(upbeat) / sizeof(upbeat[0]); i++) {
		if (contains_word(lower, upbeat[i])) {
			return PET_FAMILY_UPBEAT;
		}
	}
	return PET_FAMILY_OTHER;
}

// Shifts the month by the days that have passed since the stored one, and
// appends the family yesterday was mostly spent in. A gap of a month or more
// empties the ring: a pet that has been off for two months has no recent
// listening, and pretending otherwise would keep a temperament alive for ever.
static void record_day_family(pet_state_t *st, long day) {
	if (!st || day <= st->day) {
		return;
	}

	int winner = PET_FAMILY_OTHER;
	int best = 0;
	for (int f = 1; f < PET_FAMILY_COUNT; f++) {
		if (st->day_family[f] > best) {
			best = st->day_family[f];
			winner = f;
		}
	}

	long gap = day - st->day;
	if (gap >= PET_TEMPERAMENT_DAYS) {
		memset(st->genre_history, 0, sizeof(st->genre_history));
	} else {
		memmove(st->genre_history, st->genre_history + gap, (size_t)(PET_TEMPERAMENT_DAYS - gap));
		for (long i = PET_TEMPERAMENT_DAYS - gap; i < PET_TEMPERAMENT_DAYS; i++) {
			st->genre_history[i] = PET_FAMILY_OTHER;
		}
	}
	st->genre_history[PET_TEMPERAMENT_DAYS - 1] = (uint8_t)winner;
	memset(st->day_family, 0, sizeof(st->day_family));
}

void pet_temperament_days(const pet_state_t *st, int *out) {
	if (!out) {
		return;
	}
	for (int f = 0; f < PET_FAMILY_COUNT; f++) {
		out[f] = 0;
	}
	if (!st) {
		return;
	}
	for (int i = 0; i < PET_TEMPERAMENT_DAYS; i++) {
		int f = st->genre_history[i];
		if (f >= 0 && f < PET_FAMILY_COUNT) {
			out[f]++;
		}
	}
}

pet_temperament_t pet_temperament(const pet_state_t *st) {
	if (!st) {
		return PET_TEMPERAMENT_NONE;
	}

	int days[PET_FAMILY_COUNT];
	pet_temperament_days(st, days);
	int known = 0;
	for (int f = 1; f < PET_FAMILY_COUNT; f++) {
		known += days[f];
	}
	if (known < PET_TEMPERAMENT_MIN_DAYS) {
		return PET_TEMPERAMENT_NONE;
	}

	// The most days wins; a tie goes to the family heard most recently,
	// which is why the ring is walked backwards.
	int best_family = PET_FAMILY_OTHER;
	int best_count = 0;
	for (int i = PET_TEMPERAMENT_DAYS - 1; i >= 0; i--) {
		int f = st->genre_history[i];
		if (f <= PET_FAMILY_OTHER || f >= PET_FAMILY_COUNT) {
			continue;
		}
		if (days[f] > best_count) {
			best_count = days[f];
			best_family = f;
		}
	}

	switch (best_family) {
	case PET_FAMILY_UPBEAT:
		return PET_TEMPERAMENT_UPBEAT;
	case PET_FAMILY_CALM:
		return PET_TEMPERAMENT_CALM;
	case PET_FAMILY_CURIOUS:
		return PET_TEMPERAMENT_CURIOUS;
	default:
		return PET_TEMPERAMENT_NONE;
	}
}

const char *pet_temperament_tag(pet_temperament_t temperament) {
	switch (temperament) {
	case PET_TEMPERAMENT_UPBEAT:
		return "pet_temperament_upbeat";
	case PET_TEMPERAMENT_CALM:
		return "pet_temperament_calm";
	case PET_TEMPERAMENT_CURIOUS:
		return "pet_temperament_curious";
	case PET_TEMPERAMENT_NONE:
	default:
		return "pet_temperament_none";
	}
}

bool pet_is_upbeat(const pet_state_t *st) { return pet_temperament(st) == PET_TEMPERAMENT_UPBEAT; }

bool pet_has_soundscape(const pet_state_t *st) { return pet_temperament(st) == PET_TEMPERAMENT_CALM; }

int pet_energy_secs_per_point(const pet_state_t *st) {
	return pet_is_upbeat(st) ? PET_UPBEAT_ENERGY_SECS_PER_POINT : PET_ENERGY_SECS_PER_POINT;
}

// ---------------------------------------------------------------------------
// Skipping, and being dizzy
// ---------------------------------------------------------------------------

bool pet_skip(pet_state_t *st, int64_t now) {
	if (!st || now <= 0) {
		return false;
	}

	// The ring holds the newest skip last. One that has fallen out of the
	// window on the left is dropped first, so the eight slots are always the
	// last eight skips rather than the first eight.
	while (st->skip_count > 0 && now - st->skip_times[0] > PET_SKIP_WINDOW_SECS) {
		for (int i = 1; i < st->skip_count; i++) {
			st->skip_times[i - 1] = st->skip_times[i];
		}
		st->skip_count--;
	}
	if (st->skip_count >= PET_SKIP_RING) {
		for (int i = 1; i < PET_SKIP_RING; i++) {
			st->skip_times[i - 1] = st->skip_times[i];
		}
		st->skip_count = PET_SKIP_RING - 1;
	}
	st->skip_times[st->skip_count++] = now;

	int in_window = 0;
	for (int i = 0; i < st->skip_count; i++) {
		if (now - st->skip_times[i] <= PET_SKIP_WINDOW_SECS) {
			in_window++;
		}
	}
	if (in_window <= PET_SKIP_LIMIT) {
		return false;
	}
	if (pet_dizzy(st, now)) {
		return false; // already dizzy: one toast, not one per skip
	}
	st->dizzy_until = now + PET_DIZZY_SECS;
	return true;
}

bool pet_dizzy(const pet_state_t *st, int64_t now) {
	if (!st || now <= 0 || st->dizzy_until <= 0) {
		return false;
	}
	return now < st->dizzy_until;
}

// ---------------------------------------------------------------------------
// Foraging, and the tap-along
// ---------------------------------------------------------------------------

int pet_forage(pet_state_t *st, int session_secs) {
	if (!st || session_secs < PET_FORAGE_SECS) {
		return 0;
	}
	if (pet_temperament(st) != PET_TEMPERAMENT_CURIOUS) {
		return 0;
	}
	if (st->day <= 0 || st->forage_day == st->day) {
		return 0;
	}
	st->forage_day = st->day;

	// One day in three the find is an object rather than notes: a streak
	// freeze, which the wardrobe sells for 150.
	uint32_t roll = (uint32_t)(st->day * 2246822519u) ^ 0x9E3779B9u;
	int notes = 0;
	if ((int)(roll % PET_FORAGE_CHANCE) == 0) {
		st->freezes = clamp_int(st->freezes + 1, 0, 999);
		notes = 15;
	} else {
		notes = 20 + (int)((roll >> 8) % 41);
	}
	st->notes = clamp_int(st->notes + notes, 0, INT_MAX);
	return notes;
}

bool pet_rhythm_ready(const pet_state_t *st) { return st && st->day > 0 && st->rhythm_day != st->day; }

int pet_rhythm_finish(pet_state_t *st, int score) {
	if (!pet_rhythm_ready(st)) {
		return 0;
	}
	score = clamp_int(score, 0, 100);

	st->rhythm_day = st->day;
	if (score > st->rhythm_best) {
		st->rhythm_best = score;
	}

	// A game, not an income: two notes for turning up, ten more for a clean
	// run, and a freeze for a nearly perfect one.
	int notes = 2 + score / 10;
	st->notes = clamp_int(st->notes + notes, 0, INT_MAX);
	st->happy = clamp_int(st->happy + score / 5, 0, PET_HAPPY_MAX);
	st->energy = clamp_int(st->energy + score / 10, 0, PET_ENERGY_MAX);
	if (score >= 90) {
		st->freezes = clamp_int(st->freezes + 1, 0, 999);
	}
	return notes;
}

// ---------------------------------------------------------------------------
// The configuration
// ---------------------------------------------------------------------------

// A day's hashes, as one short line: "123,456,789". Empty when there are none.
static void save_hashes(const char *key, const uint32_t *set, int count) {
	char text[128];
	size_t used = 0;
	text[0] = '\0';
	for (int i = 0; i < count && i < PET_DAY_KEYS; i++) {
		int wrote = snprintf(text + used, sizeof(text) - used, "%s%u", i ? "," : "", (unsigned)set[i]);
		if (wrote < 0 || (size_t)wrote >= sizeof(text) - used) {
			break;
		}
		used += (size_t)wrote;
	}
	config_set(PET_SECTION, key, text);
}

static void load_hashes(const char *key, uint32_t *set, int *count) {
	*count = 0;
	const char *text = config_get(PET_SECTION, key, "");
	while (*text && *count < PET_DAY_KEYS) {
		while (*text == ' ') {
			text++;
		}
		char *end = NULL;
		unsigned long value = strtoul(text, &end, 10);
		if (end == text || value == 0 || value > UINT32_MAX) {
			break;
		}
		set[(*count)++] = (uint32_t)value;
		text = end;
		if (*text == ',') {
			text++;
		} else if (*text && *text != ' ') {
			break;
		}
	}
}

// The month as one line of digits, oldest first: "0012000...". Empty when the
// pet has never had a day with a genre in it.
static void save_history(const uint8_t *history) {
	char text[PET_TEMPERAMENT_DAYS + 1];
	for (int i = 0; i < PET_TEMPERAMENT_DAYS; i++) {
		int value = history[i];
		text[i] = (char)('0' + (value >= 0 && value < PET_FAMILY_COUNT ? value : 0));
	}
	text[PET_TEMPERAMENT_DAYS] = '\0';
	config_set(PET_SECTION, "genre_history", text);
}

static void load_history(uint8_t *history) {
	memset(history, 0, PET_TEMPERAMENT_DAYS);
	const char *text = config_get(PET_SECTION, "genre_history", "");
	int i = 0;
	for (; text[i] && i < PET_TEMPERAMENT_DAYS; i++) {
		if (text[i] >= '0' && text[i] < '0' + PET_FAMILY_COUNT) {
			history[i] = (uint8_t)(text[i] - '0');
		}
	}
}

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
	st->today_xp_secs = (int)config_get_int(PET_SECTION, "today_xp_secs", 0);
	st->happy = (int)config_get_int(PET_SECTION, "happy", 50);
	st->energy = (int)config_get_int(PET_SECTION, "energy", 80);
	st->streak = (int)config_get_int(PET_SECTION, "streak", 0);
	st->goal_day = config_get_int(PET_SECTION, "goal_day", 0);
	st->freeze_day = config_get_int(PET_SECTION, "freeze_day", 0);
	st->notes = (int)config_get_int(PET_SECTION, "notes", 0);
	st->note_credit = (int)config_get_int(PET_SECTION, "note_credit", 0);
	st->bonus_credit = (int)config_get_int(PET_SECTION, "bonus_credit", 0);
	st->achievements = (int)config_get_int(PET_SECTION, "achievements", 0);
	st->owned = (int)config_get_int(PET_SECTION, "owned", 0);
	st->wearing = (int)config_get_int(PET_SECTION, "wearing", PET_ITEM_NONE);
	st->room = (int)config_get_int(PET_SECTION, "room", 0);
	st->freezes = (int)config_get_int(PET_SECTION, "freezes", 0);
	st->happy_credit = (int)config_get_int(PET_SECTION, "happy_credit", 0);
	st->energy_credit = (int)config_get_int(PET_SECTION, "energy_credit", 0);
	st->idle_happy_credit = (int)config_get_int(PET_SECTION, "idle_happy_credit", 0);
	st->idle_energy_credit = (int)config_get_int(PET_SECTION, "idle_energy_credit", 0);
	st->pets = (int)config_get_int(PET_SECTION, "pets", 0);
	st->species = (int)config_get_int(PET_SECTION, "species", PET_SPECIES_NONE);
	// The file's shape has a version now. Before it, there were four animals
	// and the dragon came third; the numbers are stored, so removing the
	// dragon would quietly turn its owner into a fish. A version-1 file is
	// read with the old numbers: three was a dragon and four was a fish.
	if (config_get_int(PET_SECTION, "state_version", 1) < 2) {
		if (st->species == 3) {
			st->species = PET_SPECIES_CAT;
		} else if (st->species == 4) {
			st->species = PET_SPECIES_FISH;
		}
	}
	load_hashes("day_genres", st->day_genres, &st->day_genre_count);
	load_hashes("day_albums", st->day_albums, &st->day_album_count);
	load_history(st->genre_history);
	st->dizzy_until = (int64_t)config_get_int(PET_SECTION, "dizzy_until", 0);
	st->rhythm_day = config_get_int(PET_SECTION, "rhythm_day", 0);
	st->rhythm_best = (int)config_get_int(PET_SECTION, "rhythm_best", 0);
	st->forage_day = config_get_int(PET_SECTION, "forage_day", 0);

	// A hand edit, a half-written file or a state from a much older build is
	// brought back inside the ranges this arithmetic assumes.
	if (st->xp_secs < 0) {
		st->xp_secs = 0;
	}
	st->today_secs = clamp_int(st->today_secs, 0, 86400);
	st->today_xp_secs = clamp_int(st->today_xp_secs, 0, PET_DAILY_XP_SECS);
	st->happy = clamp_int(st->happy, 0, PET_HAPPY_MAX);
	st->energy = clamp_int(st->energy, 0, PET_ENERGY_MAX);
	st->streak = clamp_int(st->streak, 0, 100000);
	st->notes = clamp_int(st->notes, 0, INT_MAX);
	st->note_credit = clamp_int(st->note_credit, 0, 59);
	st->bonus_credit = clamp_int(st->bonus_credit, 0, 99);
	st->happy_credit = clamp_int(st->happy_credit, 0, PET_HAPPY_SECS_PER_POINT);
	st->energy_credit = clamp_int(st->energy_credit, 0, PET_ENERGY_SECS_PER_POINT);
	st->idle_happy_credit = clamp_int(st->idle_happy_credit, 0, PET_IDLE_HAPPY_SECS_PER_POINT);
	st->idle_energy_credit = clamp_int(st->idle_energy_credit, 0, PET_IDLE_ENERGY_SECS_PER_POINT);
	st->freezes = clamp_int(st->freezes, 0, 999);
	st->pets = clamp_int(st->pets, 0, INT_MAX);
	if (st->species < 0 || st->species >= PET_SPECIES_COUNT) {
		st->species = PET_SPECIES_NONE; // unknowable: ask again
	}

	// Only the achievements and the things this build knows how to sell; a
	// bit from a newer build would otherwise be a wardrobe entry nobody can
	// see. Consumables are not owned, so their bit means nothing here.
	st->achievements &= PET_ACH_NIGHT_OWL | PET_ACH_DEEP_DIVER | PET_ACH_POLYMATH | PET_ACH_FOCUS;
	st->owned &= (1 << PET_ITEM_PARTY_HAT) | (1 << PET_ITEM_HEADPHONES) | (1 << PET_ITEM_SCARF) |
				 (1 << PET_ITEM_CROWN) | (1 << PET_ITEM_ROOM_SUNSET) | (1 << PET_ITEM_ROOM_NIGHT);

	if (st->day < 0) {
		st->day = 0;
	}
	if (st->goal_day < 0) {
		st->goal_day = 0;
	}
	if (st->freeze_day < 0) {
		st->freeze_day = 0;
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
	if (st->wearing < 0 || st->wearing > PET_ITEM_LAST_OUTFIT || !(st->owned & (1 << st->wearing))) {
		st->wearing = PET_ITEM_NONE;
	}
	if (st->room < 0 || st->room > 2) {
		st->room = 0;
	}
	if (st->room == 1 && !(st->owned & (1 << PET_ITEM_ROOM_SUNSET))) {
		st->room = 0;
	}
	if (st->room == 2 && !(st->owned & (1 << PET_ITEM_ROOM_NIGHT))) {
		st->room = 0;
	}
}

void pet_save(const pet_state_t *st) {
	if (!st) {
		return;
	}

	// Two: the dragon is gone and the fish took its number.
	config_set_int(PET_SECTION, "state_version", 2);
	config_set_int(PET_SECTION, "adopted", (long)st->adopted);
	config_set_int(PET_SECTION, "last_seen", (long)st->last_seen);
	config_set_int(PET_SECTION, "last_music", (long)st->last_music);
	config_set_int(PET_SECTION, "last_pet", (long)st->last_pet);
	config_set_int(PET_SECTION, "xp_secs", (long)st->xp_secs);
	config_set_int(PET_SECTION, "day", st->day);
	config_set_int(PET_SECTION, "today_secs", st->today_secs);
	config_set_int(PET_SECTION, "today_xp_secs", st->today_xp_secs);
	config_set_int(PET_SECTION, "happy", st->happy);
	config_set_int(PET_SECTION, "energy", st->energy);
	config_set_int(PET_SECTION, "streak", st->streak);
	config_set_int(PET_SECTION, "goal_day", st->goal_day);
	config_set_int(PET_SECTION, "freeze_day", st->freeze_day);
	config_set_int(PET_SECTION, "notes", st->notes);
	config_set_int(PET_SECTION, "note_credit", st->note_credit);
	config_set_int(PET_SECTION, "bonus_credit", st->bonus_credit);
	config_set_int(PET_SECTION, "achievements", st->achievements);
	config_set_int(PET_SECTION, "owned", st->owned);
	config_set_int(PET_SECTION, "wearing", st->wearing);
	config_set_int(PET_SECTION, "room", st->room);
	config_set_int(PET_SECTION, "freezes", st->freezes);
	config_set_int(PET_SECTION, "happy_credit", st->happy_credit);
	config_set_int(PET_SECTION, "energy_credit", st->energy_credit);
	config_set_int(PET_SECTION, "idle_happy_credit", st->idle_happy_credit);
	config_set_int(PET_SECTION, "idle_energy_credit", st->idle_energy_credit);
	config_set_int(PET_SECTION, "pets", st->pets);
	config_set_int(PET_SECTION, "species", st->species);
	save_hashes("day_genres", st->day_genres, st->day_genre_count);
	save_hashes("day_albums", st->day_albums, st->day_album_count);
	save_history(st->genre_history);
	config_set_int(PET_SECTION, "dizzy_until", (long)st->dizzy_until);
	config_set_int(PET_SECTION, "rhythm_day", st->rhythm_day);
	config_set_int(PET_SECTION, "rhythm_best", st->rhythm_best);
	config_set_int(PET_SECTION, "forage_day", st->forage_day);
}

void pet_set_name(const char *name) {
	const char *start = name ? name : "";
	while (*start && isspace((unsigned char)*start)) {
		start++;
	}
	const char *end = start + strlen(start);
	while (end > start && isspace((unsigned char)end[-1])) {
		end--;
	}

	// The page has room for a short name, and the settings file is read by
	// people: 24 bytes, cut at a character boundary rather than inside one.
	size_t length = (size_t)(end - start);
	if (length > 24) {
		length = 24;
		while (length > 0 && ((unsigned char)start[length] & 0xC0) == 0x80) {
			length--;
		}
	}

	char clean[32];
	memcpy(clean, start, length);
	clean[length] = '\0';
	if (length == 0) {
		snprintf(clean, sizeof(clean), "Momo"); // an empty name is no name
	}
	config_set(PET_SECTION, "name", clean);
}

void pet_name(char *out, size_t out_size) {
	if (!out || out_size == 0) {
		return;
	}
	const char *stored = config_get(PET_SECTION, "name", "Momo");

	const char *start = stored;
	while (*start && isspace((unsigned char)*start)) {
		start++;
	}
	const char *end = start + strlen(start);
	while (end > start && isspace((unsigned char)end[-1])) {
		end--;
	}

	size_t length = (size_t)(end - start);
	if (length == 0) {
		const char *fallback = "Momo";
		length = strlen(fallback);
		start = fallback;
	}

	// The page has room for a short name. Cut at a UTF-8 boundary so a
	// two-byte character is either whole or absent, never half drawn.
	size_t limit = out_size - 1;
	if (length > limit) {
		length = limit;
		while (length > 0 && ((unsigned char)start[length] & 0xC0) == 0x80) {
			length--;
		}
	}
	memcpy(out, start, length);
	out[length] = '\0';
}
