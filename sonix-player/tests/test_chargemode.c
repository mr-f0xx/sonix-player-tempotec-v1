#define _GNU_SOURCE
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "src/system/core/config.h"
#include "src/system/device/chargemode.h"

static char settings_path[512];
static char settings_dir[512];

// The chargemode module only needs these two config operations in this test.
bool config_get_bool(const char *section, const char *key, bool fallback) {
	(void)section;
	(void)key;
	return fallback;
}

const char *config_path(void) { return settings_path; }

static bool exists(const char *name) {
	char path[600];
	snprintf(path, sizeof(path), "%s/%s", settings_dir, name);
	return access(path, F_OK) == 0;
}

int main(void) {
	char temp[] = "/tmp/sonix-chargemode-XXXXXX";
	char *dir = mkdtemp(temp);
	assert(dir);
	strncpy(settings_dir, dir, sizeof(settings_dir) - 1);
	snprintf(settings_path, sizeof(settings_path), "%s/device_config.ini", dir);

	// An install/reset has no previous player state. Even with a cable
	// attached, the first boot must not be diverted into the charge-only UI.
	assert(!exists(".sonix_player_initialized"));
	assert(!exists(".sonix_player_running"));
	assert(!chargemode_wanted());

	// Once ordinary startup is reached, both the one-time sentinel and the
	// per-run marker are written. The next boot is recognized as a reboot.
	chargemode_player_started();
	assert(exists(".sonix_player_initialized"));
	assert(exists(".sonix_player_running"));
	assert(!chargemode_wanted());

	// A clean shutdown drops only the running marker. Initialization survives,
	// so later cable-only boots can still take the intended charging path.
	chargemode_player_stopping();
	assert(exists(".sonix_player_initialized"));
	assert(!exists(".sonix_player_running"));

	// A factory reset removes /usr/data, including both markers. The first
	// boot after that reset gets the same safe, normal-startup path.
	char path[600];
	snprintf(path, sizeof(path), "%s/.sonix_player_initialized", dir);
	unlink(path);
	assert(!chargemode_wanted());
	snprintf(path, sizeof(path), "%s/device_config.ini", dir);
	unlink(path);
	rmdir(dir);
	fprintf(stderr, "chargemode: first-install and marker lifecycle tests passed\n");
	return 0;
}
