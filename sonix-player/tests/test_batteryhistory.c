// The battery-history state machine is intentionally clock/config free here.
// These checks prove that its on-time is accumulated only for running sessions,
// and that it records real cable/target edges rather than making up a charge
// time when the player first boots.

#include <assert.h>
#include <stdio.h>

#include "src/system/device/batteryhistory.h"

static void first_observation_never_invents_a_charge_time(void) {
	batteryhistory_state_t state;
	batteryhistory_state_restore(&state, NULL, 1000);

	// Booting already full and on a cable tells us neither when it became full
	// nor whether it had been unplugged in the meantime.
	assert(!batteryhistory_state_step(&state, 100, true, 100, 1700000000, 1001));
	assert(state.saved.mark == BATTERYHISTORY_MARK_NONE);
	assert(batteryhistory_state_on_seconds(&state, 2000) == 0);
	puts("ok  boot on a full charger does not invent a charge timestamp");
}

static void reaches_target_then_unplugged_resets_the_cycle(void) {
	batteryhistory_state_t state;
	batteryhistory_state_restore(&state, NULL, 100);

	// First sample: off the cable. Plugging in below the target starts a new,
	// observable charge session.
	assert(!batteryhistory_state_step(&state, 60, false, 80, 1700000000, 101));
	assert(!batteryhistory_state_step(&state, 60, true, 80, 1700000100, 110));
	assert(batteryhistory_state_step(&state, 80, true, 80, 1700000200, 120));
	assert(state.saved.mark == BATTERYHISTORY_MARK_CHARGED_TO_TARGET);
	assert(state.saved.charge_target == 80);
	assert(state.saved.marked_at == 1700000200);
	assert(batteryhistory_state_on_seconds(&state, 180) == 60);

	// The held 80% limit remains visible for hours but must not keep replacing
	// the recorded time. The counter continues while the device is still on.
	assert(!batteryhistory_state_step(&state, 80, true, 80, 1700000300, 200));
	assert(state.saved.marked_at == 1700000200);
	assert(batteryhistory_state_on_seconds(&state, 200) == 80);

	// Disconnecting starts the useful "on battery" comparison at zero and says
	// plainly that the recorded point is an unplug, not a claimed full charge.
	assert(batteryhistory_state_step(&state, 80, false, 80, 1700000400, 220));
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
	};
	batteryhistory_state_t state;
	batteryhistory_state_restore(&state, &saved, 1000);
	assert(batteryhistory_state_on_seconds(&state, 1030) == 120);

	// A clean switch-off checkpoints 120 seconds. The next run starts from its
	// own BOOTTIME value: the 20,000 seconds while the player was off are absent.
	batteryhistory_state_checkpoint(&state, 1030);
	assert(state.saved.on_seconds == 120);
	batteryhistory_saved_t checkpoint = state.saved;
	batteryhistory_state_restore(&state, &checkpoint, 21030);
	assert(batteryhistory_state_on_seconds(&state, 21045) == 135);
	puts("ok  only powered-on sessions add to on-time");
}

static void unknown_gauge_cannot_claim_a_full_charge(void) {
	batteryhistory_state_t state;
	batteryhistory_state_restore(&state, NULL, 1);
	assert(!batteryhistory_state_step(&state, -1, false, 100, 1700000000, 2));
	assert(!batteryhistory_state_step(&state, -1, true, 100, 1700000001, 3));
	assert(!batteryhistory_state_step(&state, -1, true, 100, 1700000002, 4));
	assert(state.saved.mark == BATTERYHISTORY_MARK_NONE);
	puts("ok  an unreadable gauge never claims a completed charge");
}

int main(void) {
	first_observation_never_invents_a_charge_time();
	reaches_target_then_unplugged_resets_the_cycle();
	off_time_is_not_part_of_on_time();
	unknown_gauge_cannot_claim_a_full_charge();
	puts("PASS battery history (charge points and powered-on time)");
	return 0;
}
