#include "chargerattached.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define POWER_SUPPLY_DIR "/sys/class/power_supply"

// Reads a one-line sysfs node into `out`, stripped of its trailing newline.
// False when the node is missing or unreadable, which is the answer the
// enumeration wants for a supply that does not have that attribute.
static bool read_sysfs_line(const char *dir, const char *name, const char *node, char *out, size_t out_size) {
	char path[512];
	snprintf(path, sizeof(path), "%s/%s/%s", dir, name, node);

	FILE *f = fopen(path, "r");
	if (!f) {
		return false;
	}
	char *line = fgets(out, (int)out_size, f);
	fclose(f);
	if (!line) {
		return false;
	}
	out[strcspn(out, "\r\n")] = '\0';
	return true;
}

bool charger_attached_at(const char *dir) {
	if (!dir || !dir[0]) {
		return false;
	}

	DIR *d = opendir(dir);
	if (!d) {
		return false;
	}

	bool found = false;
	struct dirent *de;
	while ((de = readdir(d)) != NULL && !found) {
		if (de->d_name[0] == '.') {
			continue;
		}

		char type[32];
		if (!read_sysfs_line(dir, de->d_name, "type", type, sizeof(type))) {
			continue;
		}

		if (strcmp(type, "Battery") == 0) {
			char status[32];
			if (read_sysfs_line(dir, de->d_name, "status", status, sizeof(status)) && status[0] != '\0' &&
				strcmp(status, "Discharging") != 0 && strcmp(status, "Unknown") != 0) {
				found = true; // Charging, Full, Not charging: the cable is in
			}
			continue;
		}

		char online[16];
		if (read_sysfs_line(dir, de->d_name, "online", online, sizeof(online)) && online[0] == '1') {
			found = true;
			continue;
		}

		// No (or a lying) online attribute: the voltage answers instead.
		// microvolts; VBUS is about five volts, a battery never that much.
		char uv[16];
		if (read_sysfs_line(dir, de->d_name, "voltage_now", uv, sizeof(uv))) {
			char *end;
			long microvolts = strtol(uv, &end, 10);
			if (end != uv && microvolts >= 4000000L) {
				found = true;
			}
		}
	}

	closedir(d);
	return found;
}

bool charger_attached(void) { return charger_attached_at(POWER_SUPPLY_DIR); }
