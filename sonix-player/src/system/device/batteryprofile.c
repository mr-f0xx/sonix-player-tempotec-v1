#include "batteryprofile.h"

#include "batteryguard.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define MS_PER_MINUTE 60000u
#define SETTLE_MS 10000u
#define MAX_SAMPLE_GAP_MS 15000u // panel-off sampling normally runs every 5 s
#define MIN_POINT_MS 45000u      // reject a gauge jump, not a measured load
#define MAX_POINT_MS (90u * MS_PER_MINUTE)

static int read_number(const char *dir, const char *node, int min, int max) {
	char path[512];
	int n = snprintf(path, sizeof(path), "%s/%s", dir, node);
	if (n < 0 || (size_t)n >= sizeof(path)) {
		return -1;
	}
	FILE *f = fopen(path, "r");
	if (!f) {
		return -1;
	}
	char text[32];
	bool ok = fgets(text, sizeof(text), f) && (feof(f) || strchr(text, '\n'));
	fclose(f);
	if (!ok) {
		return -1;
	}
	char *end;
	errno = 0;
	long value = strtol(text, &end, 10);
	if (end == text || errno == ERANGE || value < min || value > max) {
		return -1;
	}
	while (isspace((unsigned char)*end)) {
		end++;
	}
	return *end ? -1 : (int)value;
}

static bool stock_gauge(const char *dir) {
	char path[512], target[256];
	int n = snprintf(path, sizeof(path), "%s/battery/device/driver", dir);
	if (n < 0 || (size_t)n >= sizeof(path)) {
		return false;
	}
	ssize_t length = readlink(path, target, sizeof(target) - 1);
	if (length < 1 || (size_t)length >= sizeof(target) - 1) {
		return false;
	}
	target[length] = '\0';
	const char *name = strrchr(target, '/');
	return strcmp(name ? name + 1 : target, "cw201x") == 0;
}

batteryprofile_reading_t batteryprofile_read(const char *supply_dir, int cached_percent, bool read_voltage) {
	batteryprofile_reading_t reading = {
		.percent = cached_percent >= 0 && cached_percent <= 100 ? cached_percent : -1,
		.pmic_mv = -1,
		.gauge_minutes = -1,
	};
	if (!supply_dir) {
		return reading;
	}

	// The V1's stock CW2015 driver (cw201x) returns the 13-bit RRT register
	// in minutes. Other drivers use the Linux time_to_empty_now ABI (seconds).
	// Check the driver, not just the generic supply name, before using it.
	if (stock_gauge(supply_dir)) {
		int level = read_number(supply_dir, "battery/capacity", 0, 100);
		if (level >= 0) {
			reading.percent = level;
			reading.cw2015_percent = true;
			// 0 is not usable for a rate calculation, and 0x1fff is saturated.
			reading.gauge_minutes = read_number(supply_dir, "battery/time_to_empty_now", 1, 0x1ffe);
		}
	}
	// Only expose the ADC voltage. axp_battery/current_now in stock 1.3 is
	// a constant 200 mA, not a measurement, and health is also hard-coded.
	if (read_voltage && read_number(supply_dir, "axp_battery/present", 0, 1) != 0) {
		int uv = read_number(supply_dir, "axp_battery/voltage_now", 2000000, 5000000);
		if (uv >= 0) {
			reading.pmic_mv = (uv + 500) / 1000;
		}
	}
	return reading;
}

static batteryprofile_estimate_t estimate(batteryprofile_source_t source, int level, uint32_t ms_per_point) {
	const uint32_t scale = BATTERYPROFILE_DESIGN_MAH * (3600000u / 100u);
	return (batteryprofile_estimate_t){
		.source = source,
		.draw_ma = (int)((scale + ms_per_point / 2) / ms_per_point),
		.minutes_left = (int)((ms_per_point * (uint32_t)(level - BATTERYGUARD_OFF_PERCENT) + MS_PER_MINUTE / 2) /
							  MS_PER_MINUTE),
	};
}

batteryprofile_estimate_t batteryprofile_step(batteryprofile_t *state, uint32_t now_ms,
											  batteryprofile_reading_t reading, bool external_power) {
	const batteryprofile_estimate_t unavailable = {0};
	int level = reading.percent;
	if (external_power || level <= BATTERYGUARD_OFF_PERCENT || level > 100) {
		*state = (batteryprofile_t){0};
		return unavailable;
	}

	// Unsigned elapsed time also handles the 32-bit clock wrapping. After
	// a real suspend, or an unexpected charge jump, start a fresh window.
	if (!state->active || now_ms - state->last_ms > MAX_SAMPLE_GAP_MS ||
		reading.cw2015_percent != state->cw2015_percent || level > state->last_percent ||
		state->last_percent - level > 1) {
		*state = (batteryprofile_t){
			.active = true,
			.cw2015_percent = reading.cw2015_percent,
			.last_percent = level,
			.started_ms = now_ms,
			.last_ms = now_ms,
		};
		return unavailable;
	}

	bool falling = level < state->last_percent;
	state->last_percent = level;
	state->last_ms = now_ms;
	if (now_ms - state->started_ms < SETTLE_MS) {
		return unavailable;
	}

	// Prefer the rate seen under this load to the gauge's own forecast.
	// Use the last two *complete* 1% intervals, never the partial interval
	// between opening the page and the first falling edge.
	uint32_t since_edge = now_ms - state->edge_ms;
	if (falling) {
		if (state->have_edge && since_edge >= MIN_POINT_MS && since_edge <= MAX_POINT_MS) {
			state->previous_interval_ms = state->interval_ms;
			state->interval_ms = since_edge;
		} else {
			state->previous_interval_ms = state->interval_ms = 0;
		}
		state->edge_ms = now_ms;
		state->have_edge = true;
	} else if (state->have_edge && (since_edge > MAX_POINT_MS ||
			   (state->interval_ms && since_edge > 3u * state->interval_ms))) {
		// The gauge got stuck or the load no longer looks like this sample.
		state->have_edge = false;
		state->previous_interval_ms = state->interval_ms = 0;
	}

	if (state->previous_interval_ms) {
		return estimate(BATTERYPROFILE_TREND, level, (state->previous_interval_ms + state->interval_ms) / 2);
	}
	if (reading.cw2015_percent && reading.gauge_minutes > 0 && reading.gauge_minutes < 0x1fff) {
		uint32_t ms_per_point = ((uint32_t)reading.gauge_minutes * MS_PER_MINUTE) / (uint32_t)level;
		if (ms_per_point >= MIN_POINT_MS && ms_per_point <= MAX_POINT_MS) {
			return estimate(BATTERYPROFILE_GAUGE, level, ms_per_point);
		}
	}
	return unavailable;
}

bool batteryprofile_delta(batteryprofile_estimate_t current, batteryprofile_estimate_t baseline,
						  int *delta_ma, int *delta_percent) {
	if (current.source == BATTERYPROFILE_NONE || current.source != baseline.source ||
		current.draw_ma <= 0 || baseline.draw_ma <= 0) {
		return false;
	}
	int delta = current.draw_ma - baseline.draw_ma;
	int change = (abs(delta) * 100 + baseline.draw_ma / 2) / baseline.draw_ma;
	*delta_ma = delta;
	*delta_percent = delta < 0 ? -change : change;
	return true;
}
