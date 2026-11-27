// Pass-through charging in DAC mode (issue #504): with the cable in, the player
// runs off VBUS and the battery is left out of it -- no charge going in, no
// current coming out. Off by default, remembered across boots, in force only
// while DAC mode has the cable, and the charger is given back on the way out.
//
// The test includes usbdac.c and records what it asks of the rest of the player:
// power.c owns the charger enable, system.c the battery icon and the LED.
#define _GNU_SOURCE 1
#include "../src/system/audio/usbdac.c"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int charge_calls; // power_set_charging_allowed()
static int last_charge_allowed = -1;
static int suppress_calls; // system_suppress_charging()
static int last_suppressed = -1;

void power_set_charging_allowed(bool allowed) {
	charge_calls++;
	last_charge_allowed = allowed ? 1 : 0;
}

void system_suppress_charging(bool suppressed) {
	suppress_calls++;
	last_suppressed = suppressed ? 1 : 0;
}

bool usb_set_dac_mode(bool on) {
	(void)on;
	return true;
}

// Never reached from below, but usbdac.c calls it and the linker wants it if
// anything on the audio path survives garbage collection.
bool audio_external_begin_latency(int sample_rate, int channels, int bits, int buffer_ms) {
	(void)sample_rate;
	(void)channels;
	(void)bits;
	(void)buffer_ms;
	return true;
}

static void reset_recorders(void) {
	charge_calls = 0;
	suppress_calls = 0;
	last_charge_allowed = -1;
	last_suppressed = -1;
}

// The production module reads the setting once and keeps it; clearing the cache
// models the next boot reading the file again.
static void restart_with(const char *path) {
	charging_cache = -1;
	config_init(path);
}

// What a successful start leaves behind, without building a gadget: the mode is
// active, so the charging choice is now a live one.
static void dac_mode_on(void) {
	pthread_mutex_lock(&lock);
	state.active = true;
	touch();
	pthread_mutex_unlock(&lock);
}

static bool file_contains(const char *path, const char *needle) {
	FILE *file = fopen(path, "r");
	if (!file) {
		return false;
	}
	char contents[4096];
	size_t size = fread(contents, 1, sizeof(contents) - 1, file);
	contents[size] = '\0';
	fclose(file);
	return strstr(contents, needle) != NULL;
}

int main(void) {
	char dir[] = "/tmp/sonix-usbdac-charging-XXXXXX";
	assert(mkdtemp(dir));
	char config_path[512];
	snprintf(config_path, sizeof(config_path), "%s/device_config.ini", dir);

	// A player that has never seen the setting charges as it always has: the
	// feature is opt-in and writes nothing until it is switched on.
	assert(!config_init(config_path));
	assert(usbdac_charging_enabled());
	assert(!file_contains(config_path, "dac_charge_disable"));

	// With DAC mode off the choice is only remembered. Nothing reaches the
	// charger, because there is no cable to hand over yet.
	reset_recorders();
	usbdac_set_charging(false);
	assert(!usbdac_charging_enabled());
	assert(charge_calls == 0 && suppress_calls == 0);
	assert(file_contains(config_path, "dac_charge_disable = 1"));

	// DAC mode starting applies it: the charger off, and the indication off
	// with it, since a cable that is in but not filling the battery must not
	// read as one that is.
	dac_mode_on();
	reset_recorders();
	apply_charging();
	assert(usbdac_is_active());
	assert(charge_calls == 1 && last_charge_allowed == 0);
	assert(suppress_calls == 1 && last_suppressed == 1);

	// The row on the page and the bolt in the header both work live, and both
	// write the same setting.
	usbdac_set_charging(true);
	assert(usbdac_charging_enabled());
	assert(charge_calls == 2 && last_charge_allowed == 1);
	assert(suppress_calls == 2 && last_suppressed == 0);
	assert(file_contains(config_path, "dac_charge_disable = 0"));

	// And back again, which is what a long session on the desk uses.
	usbdac_set_charging(false);
	assert(charge_calls == 3 && last_charge_allowed == 0);
	assert(suppress_calls == 3 && last_suppressed == 1);

	// It survives a reboot: the same file, a fresh process, the same answer.
	restart_with(config_path);
	assert(!usbdac_charging_enabled());

	// Leaving DAC mode gives the charger back and the indication with it, so a
	// player that is a player again charges off the same cable. The charger
	// keeps its enable bit across a shutdown, so this has to happen here.
	reset_recorders();
	usbdac_stop();
	assert(!usbdac_is_active());
	assert(charge_calls == 1 && last_charge_allowed == 1);
	assert(suppress_calls == 1 && last_suppressed == 0);

	// With the setting forgotten, the next DAC session charges again.
	unlink(config_path);
	restart_with(config_path);
	assert(usbdac_charging_enabled());

	unlink(config_path);
	rmdir(dir);

	puts("usbdac: pass-through charging is opt-in, remembered, live only in DAC mode and released on the way out");
	return 0;
}
