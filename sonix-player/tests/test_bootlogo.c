// The boot marker is the only bridge between a setting made in the player and
// the splash drawn before any config file is mounted. Check its exact shape:
// the stock script reads the first seven bytes, the packer's block reads the
// picture selection at byte 8, and the brightness it applies lives at byte 15.
// Direct marker construction, no NAND and no worker thread.

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "../src/system/device/bootlogo.c"

// Only used by the marker writer, which is not exercised here.
void thread_be_background(const char *name) { (void)name; }

static void expect(int choice, bool dark, int light, const char *want) {
	char marker[MARKER_LENGTH + 1];
	config_set_int("ui", "boot_brightness", light);
	marker_for(choice, dark, marker);
	assert(strlen(marker) == MARKER_LENGTH);
	assert(strncmp(marker, want, strlen(want)) == 0);
	for (size_t i = strlen(want); i < MARKER_LENGTH; i++) {
		assert(marker[i] == ' '); // padded, so the equality check is stable
	}
	assert(strncmp(marker, "theme:", 6) == 0);
	assert(strncmp(marker + MARKER_SECOND_FIELD, "logo:", 5) == 0);
}

int main(void) {
	// The stock picture follows the palette; the added pictures ask for the
	// stock fallback and say what they really are in the second field.
	expect(BOOTLOGO_STOCK, false, 60, "theme:1 logo:0 light:60");
	expect(BOOTLOGO_STOCK, true, 60, "theme:2 logo:0 light:60");
	expect(BOOTLOGO_RETROSPACE, true, 60, "theme:3 logo:1 light:60");
	expect(BOOTLOGO_SPACE, false, 60, "theme:3 logo:2 light:60");
	expect(BOOTLOGO_TRAVELLING, false, 60, "theme:3 logo:3 light:60");
	expect(BOOTLOGO_CUSTOM, false, 60, "theme:3 logo:4 light:60");

	// The default is the stock script's own level, and a saved value is
	// snapped to the five-percent steps the setting offers, all the way down
	// to 5%. Out-of-range values come from a marker written by newer
	// firmware, not from this page.
	expect(BOOTLOGO_SPACE, false, BOOTLOGO_LIGHT_MIN, "theme:3 logo:2 light:5");
	expect(BOOTLOGO_SPACE, false, 6, "theme:3 logo:2 light:5");
	expect(BOOTLOGO_SPACE, false, 35, "theme:3 logo:2 light:35");
	expect(BOOTLOGO_SPACE, false, BOOTLOGO_LIGHT_MAX, "theme:3 logo:2 light:100");
	expect(BOOTLOGO_SPACE, false, 0, "theme:3 logo:2 light:60");
	expect(BOOTLOGO_SPACE, false, 130, "theme:3 logo:2 light:60");

	puts("ok  boot marker carries the picture, the palette and the splash brightness");
	return 0;
}
