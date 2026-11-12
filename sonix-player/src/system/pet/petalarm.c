#include "petalarm.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "src/system/core/config.h"

#define SECTION "pet_alarm"
#define NAME_PART_BYTES 50
#define NAME_PARTS 4

static bool enabled;
static int minutes;
static unsigned days;
static int fade_seconds;
static char playlist[PETALARM_PLAYLIST_MAX + 1];
static long last_day;
static int last_minutes;

// All of this lives on the GUI thread. The wall clock can be set by hand or
// NTP, and the timezone can change without the clock changing at all. A
// backwards step or a different TZ invalidates the cached deadline; a normal
// forward step is handled by take_due's bounded lateness rule.
static time_t next_at;
static time_t last_now;
static bool observed;
static char observed_tz[160];

static void reset_schedule(void) {
	next_at = 0;
	last_now = 0;
	observed = false;
	observed_tz[0] = '\0';
}

static bool fade_valid(int value) { return value == 0 || value == 10 || value == 30 || value == 60; }

static int hex_value(char c) {
	if (c >= '0' && c <= '9') return c - '0';
	if (c >= 'a' && c <= 'f') return c - 'a' + 10;
	if (c >= 'A' && c <= 'F') return c - 'A' + 10;
	return -1;
}

// Config values are 127 bytes, shorter than a valid playlist name. Hex in
// four short values preserves all 200 bytes, including UTF-8 and spaces at a
// split boundary (the INI reader trims whitespace from individual values).
static void load_playlist(void) {
	size_t used = 0;
	for (int part = 0; part < NAME_PARTS; part++) {
		char key[24];
		snprintf(key, sizeof(key), "playlist_%d", part);
		const char *encoded = config_get(SECTION, key, "");
		size_t len = strlen(encoded);
		if ((len & 1) || len > NAME_PART_BYTES * 2 || used + len / 2 > PETALARM_PLAYLIST_MAX) {
			playlist[0] = '\0';
			return;
		}
		for (size_t i = 0; i < len; i += 2) {
			int hi = hex_value(encoded[i]);
			int lo = hex_value(encoded[i + 1]);
			if (hi < 0 || lo < 0 || (hi == 0 && lo == 0)) {
				playlist[0] = '\0';
				return;
			}
			playlist[used++] = (char)((hi << 4) | lo);
		}
	}
	playlist[used] = '\0';
}

void petalarm_init(void) {
	enabled = config_get_bool(SECTION, "enabled", false);
	long value = config_get_int(SECTION, "minutes", 7 * 60);
	minutes = value >= 0 && value < 24 * 60 ? (int)value : 7 * 60;
	value = config_get_int(SECTION, "days", PETALARM_ALL_DAYS);
	days = value >= 0 && value <= PETALARM_ALL_DAYS ? (unsigned)value : PETALARM_ALL_DAYS;
	value = config_get_int(SECTION, "fade_seconds", 0);
	fade_seconds = fade_valid((int)value) ? (int)value : 0;
	last_day = config_get_int(SECTION, "last_day", 0);
	last_minutes = (int)config_get_int(SECTION, "last_minutes", -1);
	load_playlist();
	reset_schedule();
}

bool petalarm_enabled(void) { return enabled; }
int petalarm_minutes(void) { return minutes; }
unsigned petalarm_days(void) { return days; }
int petalarm_fade_seconds(void) { return fade_seconds; }
const char *petalarm_playlist(void) { return playlist; }

void petalarm_set_enabled(bool on) {
	if (enabled == on) return;
	enabled = on;
	config_set_bool(SECTION, "enabled", on);
	config_save();
	reset_schedule();
}

void petalarm_set_minutes(int value) {
	if (value < 0 || value >= 24 * 60 || value == minutes) return;
	minutes = value;
	config_set_int(SECTION, "minutes", value);
	config_save();
	reset_schedule();
}

void petalarm_set_days(unsigned value) {
	if ((value & ~PETALARM_ALL_DAYS) || value == days) return;
	days = value;
	config_set_int(SECTION, "days", (long)value);
	config_save();
	reset_schedule();
}

void petalarm_set_fade_seconds(int value) {
	if (!fade_valid(value) || value == fade_seconds) return;
	fade_seconds = value;
	config_set_int(SECTION, "fade_seconds", value);
	config_save();
}

bool petalarm_set_playlist(const char *name) {
	if (!name) name = "";
	size_t len = strlen(name);
	if (len > PETALARM_PLAYLIST_MAX) return false;
	if (strcmp(playlist, name) == 0) return true;
	memcpy(playlist, name, len + 1);

	static const char digits[] = "0123456789abcdef";
	for (int part = 0; part < NAME_PARTS; part++) {
		char key[24];
		char encoded[NAME_PART_BYTES * 2 + 1];
		size_t start = (size_t)part * NAME_PART_BYTES;
		size_t n = start < len ? len - start : 0;
		if (n > NAME_PART_BYTES) n = NAME_PART_BYTES;
		for (size_t i = 0; i < n; i++) {
			unsigned char c = (unsigned char)name[start + i];
			encoded[i * 2] = digits[c >> 4];
			encoded[i * 2 + 1] = digits[c & 15];
		}
		encoded[n * 2] = '\0';
		snprintf(key, sizeof(key), "playlist_%d", part);
		config_set(SECTION, key, encoded);
	}
	config_save();
	return true;
}

bool petalarm_armed(bool clock_set) { return enabled && days != 0 && clock_set; }

static long day_key(const struct tm *local) {
	return (long)(local->tm_year + 1900) * 10000L + (local->tm_mon + 1) * 100L + local->tm_mday;
}

// Search calendar days, not 24-hour increments: the latter lands an hour out
// over daylight-saving changes. Noon is used to move between dates, since
// midnight itself can be a timezone transition. mktime normalizes a skipped
// spring-forward minute to the following valid local time.
static time_t find_next(time_t now) {
	struct tm today;
	if (!localtime_r(&now, &today)) return 0;
	for (int i = 0; i <= 8; i++) {
		struct tm day = today;
		day.tm_mday += i;
		day.tm_hour = 12;
		day.tm_min = day.tm_sec = 0;
		day.tm_isdst = -1;
		time_t noon = mktime(&day);
		if (noon == (time_t)-1 || !localtime_r(&noon, &day)) continue;
		if (!(days & (1u << day.tm_wday))) continue;
		if (day_key(&day) == last_day && minutes == last_minutes) continue;
		day.tm_hour = minutes / 60;
		day.tm_min = minutes % 60;
		day.tm_sec = 0;
		day.tm_isdst = -1;
		time_t candidate = mktime(&day);
		// A setting made just before the minute can reach the first watcher
		// tick a second late. Treat the entire scheduled minute as current,
		// even on startup, but never replay a missed earlier minute.
		if (candidate != (time_t)-1 && difftime(now, candidate) < 60.0) return candidate;
	}
	return 0;
}

time_t petalarm_next(time_t now, bool clock_set) {
	if (!petalarm_armed(clock_set) || now <= 0) {
		reset_schedule();
		return 0;
	}
	const char *tz = getenv("TZ");
	if (!tz) tz = "";
	if (observed && (now < last_now || strcmp(tz, observed_tz) != 0)) {
		next_at = 0;
	}
	if (!observed || strcmp(tz, observed_tz) != 0) {
		snprintf(observed_tz, sizeof(observed_tz), "%s", tz);
	}
	observed = true;
	last_now = now;
	if (!next_at) next_at = find_next(now);
	return next_at;
}

bool petalarm_take_due(time_t now, bool clock_set) {
	time_t due = petalarm_next(now, clock_set);
	if (!due || now < due) return false;

	bool on_time = difftime(now, due) <= PETALARM_LATE_SECS;
	struct tm local;
	if (on_time && localtime_r(&due, &local) &&
		!(day_key(&local) == last_day && minutes == last_minutes)) {
		last_day = day_key(&local);
		last_minutes = minutes;
		config_set_int(SECTION, "last_day", last_day);
		config_set_int(SECTION, "last_minutes", last_minutes);
		config_save();
		next_at = find_next(now);
		return true;
	}
	// A large forward clock correction or a very late resume is not a request
	// to play an old alarm. Arm the next occurrence instead.
	next_at = find_next(now);
	return false;
}
