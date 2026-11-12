#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "src/system/core/config.h"
#include "src/system/pet/petalarm.h"

static char path[] = "/tmp/sonix-alarm-test-XXXXXX";

static time_t local_time(int year, int month, int day, int hour, int minute, int second) {
	struct tm tm = {.tm_year = year - 1900, .tm_mon = month - 1, .tm_mday = day,
					.tm_hour = hour, .tm_min = minute, .tm_sec = second, .tm_isdst = -1};
	time_t result = mktime(&tm);
	assert(result != (time_t)-1);
	return result;
}

static void zone(const char *tz) {
	assert(setenv("TZ", tz, 1) == 0);
	tzset();
}

static void fresh(void) {
	unlink(path);
	(void)config_init(path);
	petalarm_init();
}

static void test_settings(void) {
	fresh();
	assert(!petalarm_enabled() && petalarm_minutes() == 420);
	assert(petalarm_days() == PETALARM_ALL_DAYS && petalarm_fade_seconds() == 0);
	assert(!petalarm_playlist()[0]);
	petalarm_set_enabled(true);
	petalarm_set_minutes(0); // midnight is not "unset"
	petalarm_set_days((1u << 1) | (1u << 5));
	petalarm_set_fade_seconds(30);
	assert(!petalarm_armed(false));
	assert(petalarm_next(1773000000, false) == 0);
	assert(petalarm_armed(true));
	petalarm_set_minutes(-1);
	petalarm_set_minutes(1440);
	petalarm_set_days(128);
	petalarm_set_fade_seconds(13);
	assert(petalarm_minutes() == 0 && petalarm_days() == ((1u << 1) | (1u << 5)));
	assert(petalarm_fade_seconds() == 30);

	// Names may be 200 UTF-8 bytes, longer than a config value; spaces on a
	// split boundary and '=' are safe too.
	char name[201];
	memset(name, 'x', 200);
	name[49] = name[50] = ' ';
	name[100] = '=';
	name[148] = (char)0xc3;
	name[149] = (char)0xa9;
	name[200] = '\0';
	assert(petalarm_set_playlist(name));
	char too_long[202];
	memset(too_long, 'a', 201);
	too_long[201] = '\0';
	assert(!petalarm_set_playlist(too_long));
	assert(!strcmp(petalarm_playlist(), name));
	assert(config_init(path));
	petalarm_init();
	assert(petalarm_enabled() && petalarm_minutes() == 0 && petalarm_fade_seconds() == 30);
	assert(petalarm_days() == ((1u << 1) | (1u << 5)));
	assert(!strcmp(petalarm_playlist(), name));
	assert(petalarm_set_playlist(NULL));
	assert(config_init(path));
	petalarm_init();
	assert(!petalarm_playlist()[0]);

	petalarm_set_days(0);
	assert(!petalarm_armed(true));
	assert(petalarm_next(1773000000, true) == 0);
	petalarm_set_enabled(false);
	assert(!petalarm_armed(true));
}

static void test_daily_and_weekly(void) {
	zone("UTC0");
	fresh();
	petalarm_set_enabled(true);
	time_t mon = local_time(2026, 3, 9, 6, 59, 50);
	time_t due = local_time(2026, 3, 9, 7, 0, 0);
	assert(petalarm_next(mon, true) == due);
	assert(!petalarm_take_due(due - 1, true));
	assert(petalarm_take_due(due + 3, true)); // a delayed tick / RTC wake
	assert(!petalarm_take_due(due + 4, true));
	assert(petalarm_next(due + 4, true) == local_time(2026, 3, 10, 7, 0, 0));
	assert(config_init(path));
	petalarm_init();
	assert(petalarm_next(due + 5, true) == local_time(2026, 3, 10, 7, 0, 0));
	assert(!petalarm_take_due(due + 5, true));

	// A changed time is a new occurrence, even on the same day.
	petalarm_set_minutes(8 * 60);
	assert(petalarm_next(due + 5, true) == local_time(2026, 3, 9, 8, 0, 0));
	assert(petalarm_take_due(local_time(2026, 3, 9, 8, 0, 0), true));

	petalarm_set_days(1u << 5); // Friday only
	time_t friday = local_time(2026, 3, 13, 8, 0, 0);
	assert(petalarm_next(local_time(2026, 3, 9, 9, 0, 0), true) == friday);
	assert(petalarm_take_due(friday, true));
	assert(petalarm_next(friday, true) == local_time(2026, 3, 20, 8, 0, 0));
	petalarm_set_enabled(false);
	assert(!petalarm_take_due(local_time(2026, 3, 20, 8, 0, 0), true));
}

static void test_current_minute(void) {
	zone("UTC0");
	time_t due = local_time(2026, 3, 9, 7, 0, 0);

	// Enabling just before the boundary must not miss an alarm merely because
	// its first one-second watcher tick happens after that boundary.
	fresh();
	petalarm_set_enabled(true);
	assert(petalarm_take_due(due, true));
	assert(!petalarm_take_due(due + 1, true));

	fresh();
	petalarm_set_enabled(true);
	assert(petalarm_take_due(due + 59, true));
	assert(!petalarm_take_due(due + 59, true));

	// A player started after the minute has passed does not play yesterday's
	// or this morning's alarm unexpectedly.
	fresh();
	petalarm_set_enabled(true);
	assert(!petalarm_take_due(due + 60, true));
	assert(petalarm_next(due + 60, true) == local_time(2026, 3, 10, 7, 0, 0));
}

static void test_clock_changes(void) {
	zone("UTC0");
	fresh();
	petalarm_set_enabled(true);
	time_t six = local_time(2026, 3, 9, 6, 0, 0);
	time_t seven = local_time(2026, 3, 9, 7, 0, 0);
	assert(petalarm_next(six, true) == seven);
	assert(!petalarm_take_due(local_time(2026, 3, 9, 9, 0, 0), true)); // big jump: no stale alarm
	assert(petalarm_next(local_time(2026, 3, 9, 9, 0, 0), true) == local_time(2026, 3, 10, 7, 0, 0));
	assert(petalarm_next(six, true) == seven); // backwards: recompute
	assert(petalarm_take_due(seven + PETALARM_LATE_SECS, true));
	assert(!petalarm_take_due(seven - 1800, true)); // backwards cannot repeat today
	assert(petalarm_next(seven - 1800, true) == local_time(2026, 3, 10, 7, 0, 0));

	fresh();
	petalarm_set_enabled(true);
	assert(petalarm_next(six, true) == seven);
	zone("UTC-2"); // same epoch, different local hour
	assert(petalarm_next(six, true) == local_time(2026, 3, 10, 7, 0, 0));
	assert(!petalarm_take_due(seven, true));
}

static void test_dst(void) {
	zone("CET-1CEST,M3.5.0/2,M10.5.0/3");
	fresh();
	petalarm_set_minutes(2 * 60 + 30);
	petalarm_set_days(1u << 0); // Sunday
	petalarm_set_enabled(true);
	time_t before = local_time(2026, 3, 29, 1, 30, 0);
	time_t next = petalarm_next(before, true);
	struct tm tm;
	assert(localtime_r(&next, &tm));
	assert(tm.tm_mday == 29 && tm.tm_hour == 3 && tm.tm_min == 30);
	assert(petalarm_take_due(next, true));

	fresh();
	petalarm_set_minutes(2 * 60 + 30);
	petalarm_set_days(1u << 0);
	petalarm_set_enabled(true);
	before = local_time(2026, 10, 25, 0, 30, 0);
	next = petalarm_next(before, true);
	assert(localtime_r(&next, &tm));
	assert(tm.tm_mday == 25 && tm.tm_hour == 2 && tm.tm_min == 30);
	assert(petalarm_take_due(next, true));
	// Either copy of the repeated hour, and even a restart/backward setting,
	// must not ring the same local occurrence twice.
	assert(config_init(path));
	petalarm_init();
	assert(!petalarm_take_due(next - 1800, true));
	time_t following = petalarm_next(next - 1800, true);
	assert(localtime_r(&following, &tm) && tm.tm_mday == 1 && tm.tm_mon == 10);
}

int main(void) {
	int fd = mkstemp(path);
	assert(fd >= 0);
	close(fd);
	test_settings();
	test_daily_and_weekly();
	test_current_minute();
	test_clock_changes();
	test_dst();
	unlink(path);
	puts("pet alarm schedule and settings: all checks passed");
	return 0;
}
