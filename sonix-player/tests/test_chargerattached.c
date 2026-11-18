/* The charger-presence probe the charging screen stands on.
 *
 * The boot that decides between the library and the battery screen asks
 * `charger_attached()` before anything else exists, so the answer has to come
 * out of sysfs alone and has to mean "the cable is in", not "a charge is
 * going in" -- the 80% charge limit makes the quiet case the common one. Real:
 * the production enumeration over a temporary directory of fake supplies. No
 * hardware, no LVGL, no clock. */
#include <assert.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

#include "src/system/device/chargerattached.h"

// One fake supply: `dir`/`name`/<node>=text. `text` NULL leaves the node out.
static void supply(const char *root, const char *name,
			   const char *type, const char *status, const char *online, const char *voltage_now) {
	char dir[512];
	snprintf(dir, sizeof(dir), "%s/%s", root, name);
	mkdir(dir, 0700);
	const char *nodes[] = {"type", "status", "online", "voltage_now"};
	const char *texts[] = {type, status, online, voltage_now};
	for (size_t i = 0; i < sizeof(nodes) / sizeof(nodes[0]); i++) {
		char path[640];
		snprintf(path, sizeof(path), "%s/%s", dir, nodes[i]);
		if (texts[i]) {
			FILE *f = fopen(path, "w");
			assert(f && fputs(texts[i], f) >= 0 && fclose(f) == 0);
		} else {
			unlink(path);
		}
	}
}

static void rmrf(const char *root) {
	DIR *dir = opendir(root);
	if (!dir) {
		return;
	}
	struct dirent *de;
	while ((de = readdir(dir)) != NULL) {
		if (de->d_name[0] == '.') {
			continue;
		}
		char dir_path[640];
		snprintf(dir_path, sizeof(dir_path), "%s/%s", root, de->d_name);
		DIR *sub = opendir(dir_path);
		if (!sub) {
			unlink(dir_path);
			continue;
		}
		struct dirent *child;
		while ((child = readdir(sub)) != NULL) {
			if (child->d_name[0] == '.') {
				continue;
			}
			unlinkat(dirfd(sub), child->d_name, 0);
		}
		closedir(sub);
		rmdir(dir_path);
	}
	closedir(dir);
	rmdir(root);
}

static const char *fixture(void) {
	static char roots[16][64];
	static unsigned count;
	snprintf(roots[count % 16], sizeof(roots[0]), "/tmp/sonix-chargerattached-%06lx-%u", (unsigned long)getpid(), count);
	char *root = roots[count % 16];
	count++;
	assert(mkdir(root, 0700) == 0);
	return root;
}

int main(void) {
	// A missing power-supply class says nothing, and nothing is the safe answer.
	assert(!charger_attached_at("/tmp/sonix-chargerattached-no-such-dir"));
	assert(!charger_attached_at(NULL));
	assert(!charger_attached_at(""));

	// An empty class: no cable.
	{
		const char *root = fixture();
		assert(!charger_attached_at(root));
		rmrf(root);
	}

	// A battery on its own, discharging: no cable.
	{
		const char *root = fixture();
		supply(root, "axp_battery", "Battery", "Discharging\n", NULL, NULL);
		assert(!charger_attached_at(root));
		rmrf(root);
	}

	// The battery saying anything else means the cable is in -- this is the
	// case the whole probe exists for: the cell held at its configured limit
	// reads "Full" (or "Not charging") while the charger does nothing, and a
	// probe built on "is a charge going in" cannot tell that from a charger
	// that is gone.
	{
		const char *root = fixture();
		supply(root, "axp_battery", "Battery", "Full\n", NULL, NULL);
		assert(charger_attached_at(root));
		rmrf(root);
	}
	{
		const char *root = fixture();
		supply(root, "axp_battery", "Battery", "Not charging\n", NULL, NULL);
		assert(charger_attached_at(root));
		rmrf(root);
	}
	{
		const char *root = fixture();
		supply(root, "axp_battery", "Battery", "Charging\n", NULL, NULL);
		assert(charger_attached_at(root));
		rmrf(root);
	}
	{
		const char *root = fixture();
		supply(root, "axp_battery", "Battery", "Unknown\n", NULL, NULL);
		assert(!charger_attached_at(root));
		rmrf(root);
	}

	// A supply that cannot be asked anything at all is skipped, not guessed.
	{
		const char *root = fixture();
		supply(root, "mystery", NULL, NULL, NULL, NULL);
		assert(!charger_attached_at(root));
		rmrf(root);
	}

	// The charger's own node reporting online: the classic answer, for either
	// of the names this hardware ships with.
	{
		const char *root = fixture();
		supply(root, "axp_battery", "Battery", "Discharging\n", NULL, NULL);
		supply(root, "usb", "USB", NULL, "1\n", NULL);
		assert(charger_attached_at(root));
		rmrf(root);
	}
	{
		const char *root = fixture();
		supply(root, "mp2731-charger", "Mains", NULL, "1\n", NULL);
		assert(charger_attached_at(root));
		rmrf(root);
	}

	// online reads 0 (or is missing) and there is no voltage to ask: no cable.
	{
		const char *root = fixture();
		supply(root, "usb", "USB", NULL, "0\n", NULL);
		assert(!charger_attached_at(root));
		rmrf(root);
	}
	{
		const char *root = fixture();
		supply(root, "usb", "USB", NULL, NULL, NULL);
		assert(!charger_attached_at(root));
		rmrf(root);
	}

	// online reads 0 with a USB-sized voltage behind it: the cable is in and
	// the online attribute is the one bit of this PMIC that wobbles around the
	// panel's rails.
	{
		const char *root = fixture();
		supply(root, "usb", "USB", NULL, "0\n", "5000000\n");
		assert(charger_attached_at(root));
		rmrf(root);
	}
	{
		const char *root = fixture();
		supply(root, "usb", "USB", NULL, NULL, "4712345\n");
		assert(charger_attached_at(root));
		rmrf(root);
	}
	// The boundary itself: 4.00 V counts, anything below is not a USB source.
	{
		const char *root = fixture();
		supply(root, "usb", "USB", NULL, NULL, "4000000\n");
		assert(charger_attached_at(root));
		rmrf(root);
	}
	{
		const char *root = fixture();
		supply(root, "usb", "USB", NULL, NULL, "3999999\n");
		assert(!charger_attached_at(root));
		rmrf(root);
	}

	// A battery near its top (4.2 V) must NOT read as a USB source, even if its
	// driver publishes voltage_now: only non-battery supplies are asked.
	{
		const char *root = fixture();
		supply(root, "axp_battery", "Battery", "Full\n", NULL, "4200000\n");
		assert(charger_attached_at(root)); // via status, never via voltage
		supply(root, "axp_battery", "Battery", "Discharging\n", NULL, "4200000\n");
		assert(!charger_attached_at(root));
		rmrf(root);
	}

	// Garbage in the nodes is skipped, not obeyed.
	{
		const char *root = fixture();
		supply(root, "usb", "USB", NULL, "yes\n", NULL);
		assert(!charger_attached_at(root));
		rmrf(root);
	}
	{
		const char *root = fixture();
		supply(root, "usb", "USB", NULL, NULL, "not a number\n");
		assert(!charger_attached_at(root));
		rmrf(root);
	}

	fprintf(stderr, "chargerattached: all tests passed\n");
	return 0;
}
