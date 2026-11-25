#ifndef BATTERYHISTORY_H
#define BATTERYHISTORY_H

#include <stdbool.h>
#include <stdint.h>

// A small, persistent account of a charge/discharge cycle. It deliberately
// counts CLOCK_BOOTTIME while the player is running, not calendar time, so time
// spent switched off can never be mistaken for battery runtime.
//
// Two durations are kept since the charge point: the total powered-on time
// (on_seconds), which includes standby and suspend, and the active time
// (active_seconds), which counts only while the screen is on or audio is
// playing. The active figure is what lets two battery runs be compared
// without overnight suspend inflating the number.
typedef enum {
	BATTERYHISTORY_MARK_NONE = 0,
	BATTERYHISTORY_MARK_CHARGED_TO_TARGET,
	BATTERYHISTORY_MARK_UNPLUGGED,
} batteryhistory_mark_t;

typedef struct {
	batteryhistory_mark_t mark;
	int charge_target;
	int64_t marked_at;       // Unix seconds, or zero when the clock was unset
	uint64_t on_seconds;     // completed, powered-on time before this run
	uint64_t active_seconds; // completed active time before this run
} batteryhistory_saved_t;

typedef struct {
	batteryhistory_saved_t saved;
	uint64_t session_started_boottime;
	uint64_t active_started_boottime; // start of the current active interval, or 0
	bool active;                      // whether the player is currently active
	bool cable_known;
	bool cable_present;
	bool target_seen_this_cable;
	int target_seen;
} batteryhistory_state_t;

// Pure state-machine helpers. Keeping the clock and config out of this part
// gives the history the same direct, hardware-free test coverage as the low
// battery guard.
void batteryhistory_state_restore(batteryhistory_state_t *state, const batteryhistory_saved_t *saved,
								  uint64_t boottime_seconds);
bool batteryhistory_state_step(batteryhistory_state_t *state, int battery_percent, bool cable_present,
							   int charge_target, int64_t wall_seconds, uint64_t boottime_seconds, bool active);
uint64_t batteryhistory_state_on_seconds(const batteryhistory_state_t *state, uint64_t boottime_seconds);
uint64_t batteryhistory_state_active_seconds(const batteryhistory_state_t *state, uint64_t boottime_seconds);
void batteryhistory_state_checkpoint(batteryhistory_state_t *state, uint64_t boottime_seconds);

// The player-owned wrapper. It restores and checkpoints the values beside the
// rest of the settings, while power.c supplies the latest gauge and cable
// state. Call init once after config_init(), tick from the power timer, and
// flush before a deliberate shutdown.
void batteryhistory_init(void);
void batteryhistory_tick(int battery_percent, bool cable_present, int charge_target, bool active);
void batteryhistory_flush(void);
bool batteryhistory_snapshot(batteryhistory_saved_t *out);

#endif /* BATTERYHISTORY_H */
