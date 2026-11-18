#ifndef BATTERYPROFILE_H
#define BATTERYPROFILE_H

#include <stdbool.h>
#include <stdint.h>

// TempoTec's rated cell capacity, not a measurement of this cell's health.
#define BATTERYPROFILE_DESIGN_MAH 1400

typedef struct {
	int percent;          // current state of charge, -1 if unknown
	int pmic_mv;          // AXP2101 battery voltage, -1 if unavailable/not requested
	int gauge_minutes;    // CW2015 time-to-empty, -1 if unavailable
	bool cw2015_percent;  // ensures time/rate calculations use a single gauge
} batteryprofile_reading_t;

// Read PMIC voltage from `axp_battery` and the stock cw201x gauge from
// `battery`. Without the stock gauge, use cached_percent for trends.
// Pass NULL for supply_dir in the simulator, which has no PMIC or fuel gauge.
batteryprofile_reading_t batteryprofile_read(const char *supply_dir, int cached_percent, bool read_voltage);

typedef enum {
	BATTERYPROFILE_NONE = 0,
	BATTERYPROFILE_GAUGE,
	BATTERYPROFILE_TREND,
} batteryprofile_source_t;

typedef struct {
	batteryprofile_source_t source;
	int draw_ma;         // equivalent draw from nominal capacity, never measured current
	int minutes_left;    // time to the software shutdown threshold at this rate
} batteryprofile_estimate_t;

// Sample state, initially zero. Clear it when switching loads. No clock or
// hardware access in the estimator: callers supply boot-time samples so a
// suspend gap cannot look like continuous playback.
typedef struct {
	bool active, cw2015_percent, have_edge;
	int last_percent;
	uint32_t started_ms, last_ms, edge_ms;
	uint32_t previous_interval_ms, interval_ms;
} batteryprofile_t;

batteryprofile_estimate_t batteryprofile_step(batteryprofile_t *state, uint32_t now_ms,
											  batteryprofile_reading_t reading, bool external_power);

// Compare two draws, not their remaining times (which also depend on SoC),
// and only when both were obtained by the same method.
bool batteryprofile_delta(batteryprofile_estimate_t current, batteryprofile_estimate_t baseline,
						  int *delta_ma, int *delta_percent);

#endif /* BATTERYPROFILE_H */
