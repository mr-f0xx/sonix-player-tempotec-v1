/* Exercise the real RTC/automatic-shutdown policy with fake sysfs and clocks.
 * The fake RTC deliberately disagrees with wall time; no system node is opened. */
#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "src/system/core/config.h"
#include "src/system/pet/petalarm.h"

static time_t wall_now, rtc_now, rtc_alarm;
static uint32_t boot_ms;
static bool clock_ready;
static int open_count, fail_open_at, fail_write_at, write_count;
static char written[8][64];

typedef struct { int number; char data[64]; size_t length; } wake_file_t;
static ssize_t fake_write(void *cookie, const char *data, size_t length) {
	wake_file_t *file = cookie;
	if (file->number == fail_write_at) { errno = EIO; return -1; }
	assert(file->length + length < sizeof(file->data));
	memcpy(file->data + file->length, data, length);
	file->length += length;
	file->data[file->length] = '\0';
	return (ssize_t)length;
}
static int fake_close(void *cookie) {
	wake_file_t *file = cookie;
	if (file->length) {
		assert(write_count < 8);
		snprintf(written[write_count++], sizeof(written[0]), "%s", file->data);
		if (!strcmp(file->data, "0")) rtc_alarm = 0;
		else if (file->data[0] == '+') {
			// Linux's relative wakealarm form uses the RTC's own clock.
			rtc_alarm = rtc_now + (time_t)strtol(file->data + 1, NULL, 10);
		} else {
			// The existing automatic-shutdown path uses an absolute epoch.
			rtc_alarm = (time_t)strtoll(file->data, NULL, 10);
		}
	}
	free(file);
	return 0;
}
static FILE *fake_fopen(const char *path, const char *mode) {
	assert(!strcmp(path, "/sys/class/rtc/rtc0/wakealarm") && !strcmp(mode, "w"));
	open_count++;
	if (open_count == fail_open_at) { errno = EACCES; return NULL; }
	wake_file_t *file = calloc(1, sizeof(*file));
	assert(file);
	file->number = open_count;
	cookie_io_functions_t io = {.write = fake_write, .close = fake_close};
	FILE *stream = fopencookie(file, mode, io);
	assert(stream);
	return stream;
}
static time_t fake_time(time_t *out) { if (out) *out = wall_now; return wall_now; }
static int fake_clock_gettime(clockid_t id, struct timespec *out) {
	(void)id;
	out->tv_sec = boot_ms / 1000;
	out->tv_nsec = (long)(boot_ms % 1000) * 1000000L;
	return 0;
}
bool clock_is_set(void) { return clock_ready; }

#define fopen fake_fopen
#define time fake_time
#define clock_gettime fake_clock_gettime
#include "src/system/device/power.c"
#undef fopen
#undef time
#undef clock_gettime

static char test_path[] = "/tmp/sonix-alarm-power-XXXXXX";
static const time_t due = 1773039600; /* 2026-03-09 07:00 UTC */
static void reset_fixture(void) {
	unlink(test_path);
	(void)config_init(test_path);
	petalarm_init();
	wall_now = due - 3600;
	rtc_now = wall_now + 900; // the RTC is 15 minutes fast
	rtc_alarm = 0;
	boot_ms = 100000;
	g_last_active_boot_ms = 0;
	g_auto_off_enabled = true;
	g_auto_off_ms = 600000;
	g_rtc_alarm_armed = g_rtc_alarm_for_pet = false;
	clock_ready = true;
	open_count = fail_open_at = fail_write_at = write_count = 0;
}

static void test_auto_off_and_pet_wake(void) {
	reset_fixture();
	assert(!auto_off_due(599999));
	assert(auto_off_due(600000));
	assert(rtc_alarm_arm_for_sleep());
	assert(g_rtc_alarm_armed && !g_rtc_alarm_for_pet);
	assert(write_count == 2 && !strcmp(written[0], "0"));
	assert((time_t)strtoll(written[1], NULL, 10) == wall_now + 505);
	assert(rtc_alarm == wall_now + 505 && g_rtc_alarm_boot_at == 605000);
	assert(!rtc_alarm_is_auto_off_wake(true, false));
	boot_ms = 605000;
	assert(rtc_alarm_is_auto_off_wake(true, false));
	assert(!rtc_alarm_is_auto_off_wake(true, true));
	assert(!rtc_alarm_is_auto_off_wake(false, false));
	rtc_alarm_disarm();
	assert(!g_rtc_alarm_armed && rtc_alarm == 0);

	reset_fixture();
	petalarm_set_enabled(true);
	assert(!auto_off_due(600000));
	assert(rtc_alarm_arm_for_sleep());
	assert(g_rtc_alarm_armed && g_rtc_alarm_for_pet && g_rtc_alarm_at == due);
	assert(write_count == 2 && !strcmp(written[0], "0") && !strcmp(written[1], "+3540"));
	assert(rtc_alarm == rtc_now + 3540); // not the divergent wall-clock epoch
	boot_ms += 3540 * 1000;
	assert(!rtc_alarm_is_auto_off_wake(true, false));
	// The early wake stays dark: the watcher rings when wall time reaches 7.
	assert(!petalarm_take_due(due - 60, true));
	assert(petalarm_take_due(due, true));
	assert(!petalarm_take_due(due + 1, true));
	rtc_alarm_disarm();
	assert(!g_rtc_alarm_for_pet && rtc_alarm == 0);
}

static void test_stay_awake_and_errors(void) {
	reset_fixture();
	petalarm_set_enabled(true);
	wall_now = due - 90; // early wake would leave too little time for teardown
	assert(!rtc_alarm_arm_for_sleep());
	assert(!g_rtc_alarm_armed && write_count == 1 && !strcmp(written[0], "0"));
	assert(petalarm_take_due(due, true));

	reset_fixture();
	petalarm_set_enabled(true);
	fail_open_at = 1;
	assert(!rtc_alarm_arm_for_sleep());
	assert(!g_rtc_alarm_armed && !g_rtc_alarm_for_pet);
	assert(open_count == 2); // failed clear, then best-effort clear

	reset_fixture();
	petalarm_set_enabled(true);
	fail_write_at = 2; // the clear works; programming the actual wake fails
	assert(!rtc_alarm_arm_for_sleep());
	assert(!g_rtc_alarm_armed && rtc_alarm == 0);
	assert(open_count == 3 && write_count == 2);
	assert(!strcmp(written[0], "0") && !strcmp(written[1], "0"));

	// An unarmed/clockless alarm does not disable the ordinary shutdown timer.
	reset_fixture();
	petalarm_set_enabled(true);
	clock_ready = false;
	assert(auto_off_due(600000));
	assert(rtc_alarm_arm_for_sleep());
	assert(g_rtc_alarm_armed && !g_rtc_alarm_for_pet);

	reset_fixture();
	petalarm_set_enabled(true);
	petalarm_set_days(0);
	assert(auto_off_due(600000));
	assert(rtc_alarm_arm_for_sleep());
	assert(g_rtc_alarm_armed && !g_rtc_alarm_for_pet);

	reset_fixture();
	g_auto_off_enabled = false;
	rtc_alarm = rtc_now + 100;
	assert(rtc_alarm_arm_for_sleep());
	assert(!g_rtc_alarm_armed && rtc_alarm == 0); // stale wake is cleared

	reset_fixture();
	g_last_active_boot_ms = 0;
	boot_ms = 600000; // shutdown expired while preparing to sleep
	assert(!rtc_alarm_arm_for_sleep());
	assert(!g_rtc_alarm_armed && auto_off_due(boot_ms));

	reset_fixture();
	petalarm_set_enabled(true);
	assert(rtc_alarm_arm_for_sleep());
	wall_now = due - 90; // a long sync/audio teardown approaches the alarm
	assert(!rtc_alarm_arm_for_sleep());
	assert(!g_rtc_alarm_armed && rtc_alarm == 0);
	assert(petalarm_take_due(due, true));
}

int main(void) {
	assert(setenv("TZ", "UTC0", 1) == 0);
	tzset();
	int fd = mkstemp(test_path);
	assert(fd >= 0);
	close(fd);
	test_auto_off_and_pet_wake();
	test_stay_awake_and_errors();
	unlink(test_path);
	puts("pet alarm RTC and power policy: all checks passed");
	return 0;
}
