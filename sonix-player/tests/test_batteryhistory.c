// The battery-history state machine is intentionally clock/config free here.
// These checks prove that its on-time is accumulated only for running sessions,
// that it records real cable/target edges rather than making up a charge
// time when the player first boots, and that the active time counts only
// while the player is awake (screen on or playing), never while suspended.

#include <assert.h>
#include <stdio.h>

#include "src/system/device/batteryhistory.h"

static void first_observation_never_invents_a_charge_time(void) {
	batteryhistory_state_t state;
	batteryhistory_state_restore(&state, NULL, 1000);

	// Booting already full and on a cable tells us neither when it became full
	// nor whether it had been unplugged in the meantime.
	assert(!batteryhistory_state_step(&state, 100, true, 100, 1700000000, 1001, true));
	assert(state.saved.mark == BATTERYHISTORY_MARK_NONE);
	assert(batteryhistory_state_on_seconds(&state, 2000) == 0);
	assert(batteryhistory_state_active_seconds(&state, 2000) == 0);
	puts("ok  boot on a full charger does not invent a charge timestamp");
}

static void reaches_target_then_unplugged_resets_the_cycle(void) {
	batteryhistory_state_t state;
	batteryhistory_state_restore(&state, NULL, 100);

	// First sample: off the cable. Plugging in below the target starts a new,
	// observable charge session.
	assert(!batteryhistory_state_step(&state, 60, false, 80, 1700000000, 101, true));
	assert(!batteryhistory_state_step(&state, 60, true, 80, 1700000100, 110, true));
	assert(batteryhistory_state_step(&state, 80, true, 80, 1700000200, 120, true));
	assert(state.saved.mark == BATTERYHISTORY_MARK_CHARGED_TO_TARGET);
	assert(state.saved.charge_target == 80);
	assert(state.saved.marked_at == 1700000200);
	assert(batteryhistory_state_on_seconds(&state, 180) == 60);

	// The held 80% limit remains visible for hours but must not keep replacing
	// the recorded time. The counter continues while the device is still on.
	assert(!batteryhistory_state_step(&state, 80, true, 80, 1700000300, 200, true));
	assert(state.saved.marked_at == 1700000200);
	assert(batteryhistory_state_on_seconds(&state, 200) == 80);

	// Disconnecting starts the useful "on battery" comparison at zero and says
	// plainly that the recorded point is an unplug, not a claimed full charge.
	assert(batteryhistory_state_step(&state, 80, false, 80, 1700000400, 220, true));
	assert(state.saved.mark == BATTERYHISTORY_MARK_UNPLUGGED);
	assert(state.saved.marked_at == 1700000400);
	assert(batteryhistory_state_on_seconds(&state, 245) == 25);
	puts("ok  target and unplug edges reset one charge cycle each");
}

static void off_time_is_not_part_of_on_time(void) {
	batteryhistory_saved_t saved = {
		.mark = BATTERYHISTORY_MARK_UNPLUGGED,
		.charge_target = 100,
		.marked_at = 1700000000,
		.on_seconds = 90,
		.active_seconds = 30,
	};
	batteryhistory_state_t state;
	batteryhistory_state_restore(&state, &saved, 1000);
	assert(batteryhistory_state_on_seconds(&state, 1030) == 120);
	assert(batteryhistory_state_active_seconds(&state, 1030) == 30);

	// A clean switch-off checkpoints 120 seconds. The next run starts from its
	// own BOOTTIME value: the 20,000 seconds while the player was off are absent.
	batteryhistory_state_checkpoint(&state, 1030);
	assert(state.saved.on_seconds == 120);
	assert(state.saved.active_seconds == 30);
	batteryhistory_saved_t checkpoint = state.saved;
	batteryhistory_state_restore(&state, &checkpoint, 21030);
	assert(batteryhistory_state_on_seconds(&state, 21045) == 135);
	assert(batteryhistory_state_active_seconds(&state, 21045) == 30);
	puts("ok  only powered-on sessions add to on-time");
}

static void unknown_gauge_cannot_claim_a_full_charge(void) {
	batteryhistory_state_t state;
	batteryhistory_state_restore(&state, NULL, 1);
	assert(!batteryhistory_state_step(&state, -1, false, 100, 1700000000, 2, true));
	assert(!batteryhistory_state_step(&state, -1, true, 100, 1700000001, 3, true));
	assert(!batteryhistory_state_step(&state, -1, true, 100, 1700000002, 4, true));
	assert(state.saved.mark == BATTERYHISTORY_MARK_NONE);
	puts("ok  an unreadable gauge never claims a completed charge");
}

static void active_time_counts_only_while_awake(void) {
	batteryhistory_state_t state;
	batteryhistory_state_restore(&state, NULL, 100);

	// Establish a charge point: unplug at boottime 200 while active.
	assert(!batteryhistory_state_step(&state, 90, true, 100, 1700000000, 101, true));
	assert(batteryhistory_state_step(&state, 90, false, 100, 1700000100, 200, true));
	assert(state.saved.mark == BATTERYHISTORY_MARK_UNPLUGGED);
	assert(batteryhistory_state_active_seconds(&state, 200) == 0);

	// Awake for 100 seconds (screen on): both clocks advance together.
	assert(!batteryhistory_state_step(&state, 89, false, 100, 1700000200, 300, true));
	assert(batteryhistory_state_on_seconds(&state, 300) == 100);
	assert(batteryhistory_state_active_seconds(&state, 300) == 100);

	// Screen goes dark, nothing playing: the total keeps counting, the
	// active clock stops. This is the standby/suspend window.
	assert(!batteryhistory_state_step(&state, 89, false, 100, 1700000300, 310, false));
	assert(batteryhistory_state_on_seconds(&state, 400) == 200);
	assert(batteryhistory_state_active_seconds(&state, 400) == 110);

	// Music starts in a pocket (screen off, playing): active again.
	assert(!batteryhistory_state_step(&state, 88, false, 100, 1700000400, 410, true));
	assert(batteryhistory_state_on_seconds(&state, 500) == 300);
	assert(batteryhistory_state_active_seconds(&state, 500) == 200);

	// Pause: active stops again, total does not.
	assert(!batteryhistory_state_step(&state, 88, false, 100, 1700000500, 510, false));
	assert(batteryhistory_state_on_seconds(&state, 600) == 400);
	assert(batteryhistory_state_active_seconds(&state, 600) == 210);

	// A checkpoint folds the open interval into the saved total without
	// losing or double-counting it.
	assert(!batteryhistory_state_step(&state, 87, false, 100, 1700000600, 610, true));
	batteryhistory_state_checkpoint(&state, 700);
	assert(state.saved.active_seconds == 300);
	assert(batteryhistory_state_active_seconds(&state, 700) == 300);
	assert(batteryhistory_state_active_seconds(&state, 760) == 360);
	puts("ok  active time counts only while awake, suspended time stays in the total");
}

static void suspend_jump_never_enters_active_time(void) {
	batteryhistory_state_t state;
	batteryhistory_state_restore(&state, NULL, 100);

	assert(!batteryhistory_state_step(&state, 90, true, 100, 1700000000, 101, true));
	assert(batteryhistory_state_step(&state, 90, false, 100, 1700000100, 200, true));

	// Awake briefly, then the screen goes dark and the SoC suspends. The
	// BOOTTIME clock jumps 6 hours across the suspend, but the active
	// interval was closed before the sleep, so none of it is active time.
	assert(!batteryhistory_state_step(&state, 89, false, 100, 1700000200, 260, true));
	assert(!batteryhistory_state_step(&state, 89, false, 100, 1700000300, 270, false));
	assert(batteryhistory_state_active_seconds(&state, 270) == 70);
	// Wake 6 hours later, still dark and idle.
	assert(!batteryhistory_state_step(&state, 88, false, 100, 1700000300, 21870, false));
	assert(batteryhistory_state_on_seconds(&state, 21870) == 21670);
	assert(batteryhistory_state_active_seconds(&state, 21870) == 70);
	puts("ok  a suspend jump adds to the total but never to the active time");
}

static void charge_point_resets_active_time(void) {
	batteryhistory_state_t state;
	batteryhistory_state_restore(&state, NULL, 100);

	assert(!batteryhistory_state_step(&state, 90, true, 100, 1700000000, 101, true));
	assert(batteryhistory_state_step(&state, 90, false, 100, 1700000100, 200, true));
	assert(!batteryhistory_state_step(&state, 89, false, 100, 1700000200, 300, true));
	assert(batteryhistory_state_active_seconds(&state, 300) == 100);

	// A top-up charge point starts a fresh cycle: both clocks reset, and
	// the still-open active interval restarts from the charge point.
	assert(!batteryhistory_state_step(&state, 90, true, 100, 1700000300, 310, true));
	assert(batteryhistory_state_step(&state, 100, true, 100, 1700000400, 320, true));
	assert(state.saved.mark == BATTERYHISTORY_MARK_CHARGED_TO_TARGET);
	assert(batteryhistory_state_on_seconds(&state, 320) == 0);
	assert(batteryhistory_state_active_seconds(&state, 320) == 0);
	assert(batteryhistory_state_active_seconds(&state, 380) == 60);
	puts("ok  a new charge point resets the active time too");
}

int main(void) {
	first_observation_never_invents_a_charge_time();
	reaches_target_then_unplugged_resets_the_cycle();
	off_time_is_not_part_of_on_time();
	unknown_gauge_cannot_claim_a_full_charge();
	active_time_counts_only_while_awake();
	suspend_jump_never_enters_active_time();
	charge_point_resets_active_time();
	puts("PASS battery history (charge points, powered-on time and active time)");
	return 0;
}
