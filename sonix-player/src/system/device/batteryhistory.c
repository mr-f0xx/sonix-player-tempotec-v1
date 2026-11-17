#include "batteryhistory.h"

#include "src/system/core/config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

// The player rewrites device_config.ini atomically. Five minutes is short
// enough that an unexpected battery cut loses very little measured on-time,
// while avoiding a flash write for every power-manager tick.
#define HISTORY_CHECKPOINT_SECONDS (5u * 60u)
#define EARLIEST_REAL_UNIX_TIME 946684800LL // 2000-01-01; RTC defaults are not dates

#define HISTORY_SECTION "battery_history"
#define KEY_MARK "mark"
#define KEY_TARGET "target"
#define KEY_AT "at"
#define KEY_ON_SECONDS "on_seconds"

static batteryhistory_state_t g_history;
static bool g_history_ready;
static uint64_t g_last_checkpoint_boottime;

static int clamp_target(int target) {
	if (target < 80) {
		return 80;
	}
	if (target > 100) {
		return 100;
	}
	return target;
}

static uint64_t elapsed_since(uint64_t started, uint64_t now) {
	return now > started ? now - started : 0;
}

static bool saved_mark_is_valid(batteryhistory_mark_t mark) {
	return mark == BATTERYHISTORY_MARK_CHARGED_TO_TARGET || mark == BATTERYHISTORY_MARK_UNPLUGGED;
}

void batteryhistory_state_restore(batteryhistory_state_t *state, const batteryhistory_saved_t *saved,
							  uint64_t boottime_seconds) {
	if (!state) {
		return;
	}
	memset(state, 0, sizeof(*state));
	if (saved && saved_mark_is_valid(saved->mark)) {
		state->saved = *saved;
		state->saved.charge_target = clamp_target(state->saved.charge_target);
	} else {
		state->saved.mark = BATTERYHISTORY_MARK_NONE;
		state->saved.charge_target = 100;
	}
	state->session_started_boottime = boottime_seconds;
}

uint64_t batteryhistory_state_on_seconds(const batteryhistory_state_t *state, uint64_t boottime_seconds) {
	if (!state || !saved_mark_is_valid(state->saved.mark)) {
		return 0;
	}
	return state->saved.on_seconds + elapsed_since(state->session_started_boottime, boottime_seconds);
}

void batteryhistory_state_checkpoint(batteryhistory_state_t *state, uint64_t boottime_seconds) {
	if (!state) {
		return;
	}
	if (saved_mark_is_valid(state->saved.mark)) {
		state->saved.on_seconds = batteryhistory_state_on_seconds(state, boottime_seconds);
	}
	state->session_started_boottime = boottime_seconds;
}

static bool mark_charge_point(batteryhistory_state_t *state, batteryhistory_mark_t mark, int target,
					  int64_t wall_seconds, uint64_t boottime_seconds) {
	if (!state) {
		return false;
	}
	state->saved.mark = mark;
	state->saved.charge_target = clamp_target(target);
	state->saved.marked_at = wall_seconds >= EARLIEST_REAL_UNIX_TIME ? wall_seconds : 0;
	state->saved.on_seconds = 0;
	state->session_started_boottime = boottime_seconds;
	return true;
}

bool batteryhistory_state_step(batteryhistory_state_t *state, int battery_percent, bool cable_present,
						   int charge_target, int64_t wall_seconds, uint64_t boottime_seconds) {
	if (!state) {
		return false;
	}

	charge_target = clamp_target(charge_target);
	if (!state->cable_known) {
		// A player can boot while already on a charger. The first observation
		// cannot say when it reached the target, so it starts the edge detector
		// without inventing a charge time. The next cable session is exact.
		state->cable_known = true;
		state->cable_present = cable_present;
		state->target_seen_this_cable = cable_present && battery_percent >= charge_target;
		state->target_seen = charge_target;
		return false;
	}

	bool changed = false;
	if (state->cable_present && !cable_present) {
		// Unplugging is the start of the ordinary discharge users compare. It is
		// kept even after a partial top-up: the screen says exactly which kind
		// of charge point was recorded.
		changed = mark_charge_point(state, BATTERYHISTORY_MARK_UNPLUGGED, charge_target, wall_seconds, boottime_seconds);
		state->target_seen_this_cable = false;
	}

	if (!state->cable_present && cable_present) {
		state->target_seen_this_cable = false;
		state->target_seen = charge_target;
	}

	state->cable_present = cable_present;
	if (!cable_present) {
		return changed;
	}

	// Changing an 80/100% limit during a cable session creates a new meaningful
	// target. It is eligible to be recorded once, just like a newly plugged
	// cable, and never restamps the same full battery on every timer tick.
	if (state->target_seen != charge_target) {
		state->target_seen = charge_target;
		state->target_seen_this_cable = false;
	}
	if (battery_percent >= charge_target && !state->target_seen_this_cable) {
		changed |= mark_charge_point(state, BATTERYHISTORY_MARK_CHARGED_TO_TARGET, charge_target,
								 wall_seconds, boottime_seconds);
		state->target_seen_this_cable = true;
	}
	return changed;
}

static uint64_t boottime_seconds(void) {
	struct timespec ts;
#ifdef CLOCK_BOOTTIME
	if (clock_gettime(CLOCK_BOOTTIME, &ts) == 0 && ts.tv_sec >= 0) {
		return (uint64_t)ts.tv_sec;
	}
#endif
	if (clock_gettime(CLOCK_MONOTONIC, &ts) == 0 && ts.tv_sec >= 0) {
		return (uint64_t)ts.tv_sec;
	}
	return 0;
}

static int64_t wall_seconds(void) {
	time_t now = time(NULL);
	return now < 0 ? 0 : (int64_t)now;
}

static int64_t config_i64(const char *key, int64_t fallback) {
	const char *text = config_get(HISTORY_SECTION, key, NULL);
	if (!text || !text[0]) {
		return fallback;
	}
	char *end = NULL;
	long long value = strtoll(text, &end, 10);
	return end && end != text ? (int64_t)value : fallback;
}

static uint64_t config_u64(const char *key, uint64_t fallback) {
	const char *text = config_get(HISTORY_SECTION, key, NULL);
	if (!text || !text[0]) {
		return fallback;
	}
	char *end = NULL;
	unsigned long long value = strtoull(text, &end, 10);
	return end && end != text ? (uint64_t)value : fallback;
}

static void config_set_i64(const char *key, int64_t value) {
	char text[32];
	snprintf(text, sizeof(text), "%lld", (long long)value);
	config_set(HISTORY_SECTION, key, text);
}

static void config_set_u64(const char *key, uint64_t value) {
	char text[32];
	snprintf(text, sizeof(text), "%llu", (unsigned long long)value);
	config_set(HISTORY_SECTION, key, text);
}

static void persist(uint64_t now) {
	if (!g_history_ready) {
		return;
	}
	batteryhistory_state_checkpoint(&g_history, now);
	config_set_int(HISTORY_SECTION, KEY_MARK, g_history.saved.mark);
	config_set_int(HISTORY_SECTION, KEY_TARGET, g_history.saved.charge_target);
	config_set_i64(KEY_AT, g_history.saved.marked_at);
	config_set_u64(KEY_ON_SECONDS, g_history.saved.on_seconds);
	config_save();
	g_last_checkpoint_boottime = now;
}

void batteryhistory_init(void) {
	uint64_t now = boottime_seconds();
	batteryhistory_saved_t saved = {
		.mark = (batteryhistory_mark_t)config_get_int(HISTORY_SECTION, KEY_MARK, BATTERYHISTORY_MARK_NONE),
		.charge_target = (int)config_get_int(HISTORY_SECTION, KEY_TARGET, 100),
		.marked_at = config_i64(KEY_AT, 0),
		.on_seconds = config_u64(KEY_ON_SECONDS, 0),
	};
	batteryhistory_state_restore(&g_history, &saved, now);
	g_last_checkpoint_boottime = now;
	g_history_ready = true;
}

void batteryhistory_tick(int battery_percent, bool cable_present, int charge_target) {
	if (!g_history_ready) {
		return;
	}
	uint64_t now = boottime_seconds();
	bool charge_point_changed = batteryhistory_state_step(&g_history, battery_percent, cable_present, charge_target,
										 wall_seconds(), now);
	if (charge_point_changed || elapsed_since(g_last_checkpoint_boottime, now) >= HISTORY_CHECKPOINT_SECONDS) {
		persist(now);
	}
}

void batteryhistory_flush(void) {
	if (g_history_ready) {
		persist(boottime_seconds());
	}
}

bool batteryhistory_snapshot(batteryhistory_saved_t *out) {
	if (!g_history_ready || !out) {
		return false;
	}
	*out = g_history.saved;
	out->on_seconds = batteryhistory_state_on_seconds(&g_history, boottime_seconds());
	return saved_mark_is_valid(out->mark);
}
