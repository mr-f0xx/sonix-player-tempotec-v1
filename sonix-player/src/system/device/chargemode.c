#include "chargemode.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "src/system/core/config.h"
#include "src/system/device/chargerattached.h"

// Beside device_config.ini, in /usr/data: the only partition that is writable
// and always there, and the one the settings already prove survives a power
// cut. A dot file, so it does not turn up in a listing of what the user keeps.
#define PLAYER_RUNNING_MARKER ".sonix_player_running"

// Where the marker goes, from the settings file's own path -- so a build that
// moves the settings (SONIX_CONFIG, or a test) moves this with them. False when
// there is no settings path to hang it off: the host build without one.
static bool marker_path(char *out, size_t out_size) {
	const char *settings = config_path();
	if (!settings || !settings[0]) {
		return false;
	}

	const char *slash = strrchr(settings, '/');
	size_t dir_len = slash ? (size_t)(slash - settings) + 1 : 0;
	size_t name_len = strlen(PLAYER_RUNNING_MARKER);
	if (dir_len + name_len + 1 > out_size) {
		return false;
	}

	if (dir_len) {
		memcpy(out, settings, dir_len);
	}
	memcpy(out + dir_len, PLAYER_RUNNING_MARKER, name_len + 1);
	return true;
}

bool chargemode_wanted(void) {
#ifdef HOST_BUILD
	// A PC has no charger and no board to power on, so the simulator only runs
	// the screen when it is asked to:
	//
	//     SONIX_CHARGE_SCREEN=1 ./sonix_player_host
	//
	// with the two battery files from main.c's comment pointed at whatever
	// stands in for a charger.
	const char *forced = getenv("SONIX_CHARGE_SCREEN");
	if (!forced || !forced[0]) {
		return false;
	}
	return forced[0] == '1' || strcmp(forced, "on") == 0 || strcmp(forced, "yes") == 0;
#else
	if (!config_get_bool("power", "charge_screen", true)) {
		printf("chargemode: [power] charge_screen = 0 -- the player starts as usual\n");
		return false;
	}

	// The marker first: it is one access(), it is set on every boot the player
	// itself was running, and it is the common case by far -- an ordinary boot
	// pays nothing here and never reaches the sysfs probes below.
	char path[600];
	if (!marker_path(path, sizeof(path))) {
		printf("chargemode: no settings path to hang the marker off -- the player starts as usual\n");
		return false;
	}
	if (access(path, F_OK) == 0) {
		// The player was running when this device last had power: a reboot, a
		// crash, an update. Not a charger insertion, whichever cable is in.
		printf("chargemode: %s is there -- the player was running; starting as usual\n", path);
		return false;
	}

	// No marker: this boot is a candidate for a charger boot, and the cable is
	// the question. The panel is already up when this runs, and the supplies
	// wobble for a moment after its rails open and close at the PMIC -- see
	// the settle note in topbar.c. A read taken microseconds after the unblank
	// can say "no charger" with one plugged in (it is the same PMIC), so the
	// question is asked a couple of times before it is allowed to mean
	// anything. With the charge limit holding the cell at its ceiling the
	// charger does nothing either, which is exactly when the answer matters
	// most, so the check counts the limit's own footprints
	// (charger_attached()) rather than only an online bit.
	bool cable = false;
	for (int attempt = 0; attempt < 3 && !cable; attempt++) {
		if (attempt > 0) {
			usleep(200 * 1000);
		}
		cable = charger_attached();
	}
	if (!cable) {
		// A player switched on with the key: the marker is rewritten by
		// chargemode_player_started() a moment later, and everything is as it
		// was.
		printf("chargemode: no charger on the bus -- the player starts as usual\n");
		return false;
	}

	printf("chargemode: the charger is in and the player was switched off -- charging screen\n");
	return true;
#endif
}

void chargemode_player_started(void) {
#ifdef HOST_BUILD
	return; // no marker on a PC: wanted() answers from the environment
#else
	char path[600];
	if (!marker_path(path, sizeof(path))) {
		return;
	}

	FILE *f = fopen(path, "w");
	if (!f) {
		// Not fatal -- the player runs either way -- but the next boot with a
		// cable in will come up on the charging screen, so say so.
		fprintf(stderr, "chargemode: cannot write %s; the next charger boot will show the battery screen\n",
				path);
		return;
	}
	fputs("running\n", f);
	if (fclose(f) != 0) {
		fprintf(stderr, "chargemode: %s did not close cleanly\n", path);
	}
#endif
}

void chargemode_player_stopping(void) {
#ifdef HOST_BUILD
	return;
#else
	char path[600];
	if (!marker_path(path, sizeof(path))) {
		return;
	}
	if (unlink(path) == 0) {
		printf("chargemode: switched off -- the next cable-in comes up on the battery screen\n");
	} else {
		// Nothing else can be done from here, but a marker that survives the
		// shutdown is what sends the next cable-in to the library instead of
		// the battery screen, so the failure is not left silent.
		fprintf(stderr, "chargemode: cannot remove %s (%s); the next cable-in shows the player instead of the battery screen\n",
				path, strerror(errno));
	}
#endif
}
