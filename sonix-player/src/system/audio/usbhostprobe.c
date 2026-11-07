#include "usbhostprobe.h"

#include "src/system/device/usb.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#ifndef USBHOST_TCS_CFG
#define USBHOST_TCS_CFG "/sys/devices/platform/tcs1421/tcs1421_cfg"
#endif
#ifndef USBHOST_PLATFORM_DEVICES
#define USBHOST_PLATFORM_DEVICES "/sys/bus/platform/devices"
#endif
#ifndef USBHOST_JOURNAL
// The settings partition survives a reboot; /tmp may not. A failed or
// interrupted restore must remain discoverable even after a power cycle.
#ifdef HOST_BUILD
#define USBHOST_JOURNAL "/tmp/sonix-simulator-usb-host-probe"
#else
#define USBHOST_JOURNAL "/usr/data/" USBHOST_JOURNAL_NAME
#endif
#endif

// The V1 1.3 firmware's x1600 PHY exposes a write-only sw_switch_hsotg. Its
// driver accepts "host" to force ID low and "none" to return control to the
// pin. Do not guess another attribute or write an override on other PHYs.
#define USBHOST_PHY_COMPATIBLE "ingenic,usbphy-x1600"
#define USBHOST_TRIAL_SECONDS 20
#define USBHOST_AUDIO_PROBE_SECONDS 45 // an interface appeared, ALSA may still be probing
#define USBHOST_UNPLUG_SECONDS 3
#define USBHOST_RETRY_SECONDS 1
#define USBHOST_MAGIC "sonix-usb-host-probe-v1\n"

// Where the devices on the host bus are looked for, by the held-role check
// below. Overridable for the same reason as the two paths above: the tests run
// against a directory of their own.
#ifndef USBHOST_USB_BUS
#define USBHOST_USB_BUS "/sys/bus/usb/devices"
#endif

#define STR_(x) #x
#define STR(x) STR_(x)

static struct {
	pthread_mutex_t lock;
	atomic_bool active, stopping;
	bool had_card, saw_interface, phy_cleared, role_restored, reported_failure;
	// Held rather than tried: the role stays forced for as long as the user
	// asks for, instead of the diagnostic window in begin().
	bool held;
	// A held role is checked as well as written -- see keep_role_locked().
	long long checked_at;	 // when both controls were last read back
	int checks;		 // consecutive reads that found them wrong
	long long rewritten_at;	 // the last rewrite of both
	int rewrites;		 // rewrites since the setting last took the port
	long long started, absent_since, retry_at;
	char role[32];
	char phy[PATH_MAX];
} probe = {.lock = PTHREAD_MUTEX_INITIALIZER};

static long long monotonic_seconds(void) {
	struct timespec ts;
	return clock_gettime(CLOCK_MONOTONIC, &ts) == 0 ? (long long)ts.tv_sec : -1;
}

static bool read_line(const char *path, char *out, size_t size) {
	if (!size) return false;
	out[0] = '\0';
	FILE *f = fopen(path, "r");
	if (!f) return false;
	bool ok = fgets(out, (int)size, f) != NULL && !ferror(f);
	if (fclose(f) != 0) ok = false;
	if (!ok) return false;
	out[strcspn(out, "\r\n")] = '\0';
	return true;
}

static bool saved_role_valid(const char *role) {
	return strcmp(role, "NormalDRP") == 0 || strcmp(role, "StrongDRP") == 0 || strcmp(role, "Sink") == 0;
}

// Whether something is on the host bus: a device under the controller's own
// directory, "1-1" or "1-1.2", and not an interface ("1-1:1.0") and not the
// root hub ("usb1").
//
// This is the only positive evidence the port has that the far end survived a
// role write. The Type-C mode can be written and read back as "Source" with
// nothing attached at all; a device sitting on the bus cannot.
static bool host_bus_has_device(void) {
	DIR *dir = opendir(USBHOST_USB_BUS);
	if (!dir) return false;
	bool found = false;
	struct dirent *de;
	while (!found && (de = readdir(dir)) != NULL) {
		const char *name = de->d_name;
		if (name[0] < '0' || name[0] > '9' || !strchr(name, '-') || strchr(name, ':')) continue;
		found = true;
	}
	closedir(dir);
	return found;
}

static bool write_attr(const char *path, const char *value) {
	FILE *f = fopen(path, "w");
	if (!f) return false;
	bool ok = fprintf(f, "%s\n", value) >= 0;
	if (fclose(f) != 0) ok = false;
	return ok;
}

static bool write_role(const char *role) {
	char actual[32];
	return write_attr(USBHOST_TCS_CFG, role) && read_line(USBHOST_TCS_CFG, actual, sizeof(actual)) &&
		strcmp(actual, role) == 0;
}

// compatible is a NUL-separated device-tree string list, not a text line.
static bool compatible_phy(const char *path) {
	FILE *f = fopen(path, "rb");
	if (!f) return false;
	char data[512];
	size_t n = fread(data, 1, sizeof(data), f);
	bool ok = !ferror(f) && feof(f);
	fclose(f);
	if (!ok) return false;
	for (size_t at = 0; at < n;) {
		size_t len = strnlen(data + at, n - at);
		if (len == n - at) return false;
		if (strcmp(data + at, USBHOST_PHY_COMPATIBLE) == 0) return true;
		at += len + 1;
	}
	return false;
}

// Use the device-tree identity, not the first attribute with this generic
// name. Refuse an ambiguous or inaccessible layout rather than write to a
// different controller. Platform-device entries are sysfs symlinks.
static bool find_phy(char *out, size_t size) {
	DIR *dir = opendir(USBHOST_PLATFORM_DEVICES);
	if (!dir) return false;
	bool found = false, ambiguous = false;
	int compatible = 0;
	struct dirent *de;
	for (;;) {
		errno = 0;
		de = readdir(dir);
		if (!de) { if (errno) ambiguous = true; break; }
		if (de->d_name[0] == '.') continue;
		char path[PATH_MAX];
		int n = snprintf(path, sizeof(path), "%s/%s/of_node/compatible", USBHOST_PLATFORM_DEVICES, de->d_name);
		if (n < 0 || (size_t)n >= sizeof(path) || !compatible_phy(path)) continue;
		if (++compatible > 1) { ambiguous = true; break; }
		n = snprintf(path, sizeof(path), "%s/%s/sw_switch_hsotg", USBHOST_PLATFORM_DEVICES, de->d_name);
		if (n < 0 || (size_t)n >= sizeof(path) || access(path, W_OK) != 0) continue;
		if (strlen(path) >= size) { ambiguous = true; break; }
		snprintf(out, size, "%s", path);
		found = true;
	}
	if (closedir(dir) != 0) ambiguous = true;
	return found && !ambiguous;
}

static int open_journal_parent(void) {
	char parent[PATH_MAX];
	size_t len = strlen(USBHOST_JOURNAL);
	if (len >= sizeof(parent)) { errno = ENAMETOOLONG; return -1; }
	memcpy(parent, USBHOST_JOURNAL, len + 1);
	char *slash = strrchr(parent, '/');
	if (!slash) strcpy(parent, ".");
	else if (slash == parent) slash[1] = '\0';
	else *slash = '\0';
	return open(parent, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
}

// Persist the directory entry as well as the journal contents before changing
// the port. Otherwise a power loss can discard the name of a synced file.
static bool sync_journal_parent(void) {
	int fd = open_journal_parent();
	if (fd < 0) return false;
	bool ok = fsync(fd) == 0;
	int error = errno;
	if (close(fd) != 0 && ok) return false;
	if (!ok) errno = error;
	return ok;
}

static bool make_journal(const char *role) {
	int fd = open(USBHOST_JOURNAL, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
	if (fd < 0) return false;
	char text[96];
	int n = snprintf(text, sizeof(text), USBHOST_MAGIC "%s\n", role);
	bool ok = n > 0 && (size_t)n < sizeof(text);
	size_t at = 0;
	while (ok && at < (size_t)n) {
		ssize_t written = write(fd, text + at, (size_t)n - at);
		if (written < 0 && errno == EINTR) continue;
		if (written <= 0) { ok = false; break; }
		at += (size_t)written;
	}
	if (ok && fsync(fd) != 0) ok = false;
	if (close(fd) != 0) ok = false;
	if (ok && !sync_journal_parent()) ok = false;
	if (!ok) {
		unlink(USBHOST_JOURNAL);
		(void)sync_journal_parent(); // best effort; no role has been changed
	}
	return ok;
}

// Validates the complete journal, rather than applying arbitrary text as a
// role. O_NOFOLLOW also prevents a stale/malicious symlink being followed.
static bool read_journal(char *role, size_t size) {
	int fd = open(USBHOST_JOURNAL, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
	if (fd < 0) return false;
	char text[96];
	struct stat st;
	bool regular = fstat(fd, &st) == 0 && S_ISREG(st.st_mode);
	ssize_t n = read(fd, text, sizeof(text) - 1);
	char extra;
	ssize_t tail = n >= 0 ? read(fd, &extra, 1) : -1;
	int closed = close(fd);
	bool ok = regular && n > 0 && tail == 0 && closed == 0;
	if (!ok) return false;
	text[n] = '\0';
	size_t magic = sizeof(USBHOST_MAGIC) - 1;
	if ((size_t)n <= magic || memcmp(text, USBHOST_MAGIC, magic) != 0 || text[n - 1] != '\n' ||
		memchr(text, '\0', (size_t)n) != NULL) return false;
	text[n - 1] = '\0';
	size_t role_len = strlen(text + magic);
	if (!saved_role_valid(text + magic) || role_len >= size) return false;
	memcpy(role, text + magic, role_len + 1);
	return true;
}

bool usbhostprobe_active(void) { return atomic_load(&probe.active); }
bool usbhostprobe_restore_pending(void) {
	return atomic_load(&probe.active) && atomic_load(&probe.stopping);
}

// Caller holds probe.lock. Never give the gadget controller back with a known
// forced host override or an un-restored Type-C role. If a sysfs write fails,
// the journal and reservation remain in place for a retry (and a restart).
static bool restore_locked(void) {
	probe.stopping = true;
	if (!saved_role_valid(probe.role)) return false; // never apply a corrupt journal
	// In particular, recovery after a crash must not change a controller
	// another process has since given to a PC or an unknown gadget.
	if (!usb_external_host_restore_safe()) return false;
	if (!probe.phy_cleared) {
		if (!probe.phy[0] && !find_phy(probe.phy, sizeof(probe.phy))) return false;
		if (!write_attr(probe.phy, "none")) {
			probe.phy[0] = '\0'; // the platform device may have been rebound
			return false;
		}
		probe.phy_cleared = true;
	}
	if (!probe.role_restored) {
		if (!write_role(probe.role)) return false;
		probe.role_restored = true;
	}
	if (unlink(USBHOST_JOURNAL) != 0 && errno != ENOENT) return false;
	// Also retry this sync if a previous unlink succeeded but its sync failed.
	if (!sync_journal_parent()) return false;
	fprintf(stderr, "usbhost: PHY override cleared; Type-C mode restored to %s\n", probe.role);
	probe.stopping = false;
	probe.had_card = false;
	probe.reported_failure = false;
	probe.phy[0] = '\0';
	atomic_store(&probe.active, false);
	usb_external_host_release();
	return true;
}

static bool request_host_locked(const char *why) {
	// Advertise a source on the Type-C controller before asking the SoC to be
	// a host. A successful write is a request, not proof of enumeration.
	bool requested = write_role("Source");
	if (requested) {
		probe.phy_cleared = false; // even a short/failed write may have taken
		requested = write_attr(probe.phy, "host");
	}
	if (!requested) {
		fprintf(stderr, "usbhost: could not request host mode; restoring the previous role\n");
		if (!restore_locked()) fprintf(stderr, "usbhost: restoration failed; the controller remains reserved for retry\n");
		return false;
	}
	fprintf(stderr, "usbhost: %s; Type-C Source and PHY host requested (%s)\n", why, probe.phy);
	return true;
}

// How often a held role is read back, and how many reads in a row have to find
// it wrong before it is written again.
//
// A write to the TCS1421 is a request to the controller's own state machine, not
// a setting that stays. The driver re-runs its role decision on every attach and
// detach, from the mode it started in (NormalDRP), and whatever is written to
// the attribute does not survive that -- the port is then dual while the SoC is
// still meant to be the host, which is a combination nothing here asked for and
// nothing recovers from on its own. Two consecutive bad reads a second apart is
// the threshold: one is a port in the middle of a role transition, two is a
// port that has decided.
#define ROLE_CHECK_SECONDS 1
#define ROLE_CHECK_MISSES 2
// And how soon after a rewrite the port may be rewritten again if that did not
// take either. A re-enumerating dongle comes back within a few seconds; this is
// the space between attempts, not the attempt's own timeout.
#define ROLE_REWRITE_SECONDS 5

// Reads the two controls back and writes them again when they are not what was
// asked for. Caller holds probe.lock. Returns whether the role was written
// again, which is the only thing the poll has to know.
static bool keep_role_locked(long long now) {
	if (now < 0) return false;
	if (probe.checked_at && now - probe.checked_at < ROLE_CHECK_SECONDS) return false;
	probe.checked_at = now;

	char mode[32];
	bool mode_ok = read_line(USBHOST_TCS_CFG, mode, sizeof(mode)) && strcmp(mode, "Source") == 0;
	// A bus device is the answer the kernel gives to "is the far end still
	// there": with one enumerated, the controller's role is doing its job
	// whatever the attribute says, and the Type-C mode is a value the driver
	// is entitled to report in its own terms. Without one, the attribute is all
	// there is, and both controls have to say host.
	if (host_bus_has_device()) {
		probe.checks = 0;
		return false;
	}
	if (mode_ok) {
		probe.checks = 0;
		return false;
	}

	if (++probe.checks < ROLE_CHECK_MISSES) return false;
	probe.checks = 0;
	if (probe.rewritten_at && now - probe.rewritten_at < ROLE_REWRITE_SECONDS) return false;
	probe.rewritten_at = now;

	// The Type-C mode first, as on the way in. The PHY override is written
	// again with it: whether it was the mode or the override that went, the
	// pair is what the far end needs, and a PHY already in host is not harmed
	// by being told so again.
	bool ok = write_role("Source");
	if (ok) {
		probe.phy_cleared = false;
		ok = write_attr(probe.phy, "host");
	}
	probe.rewrites++;
	if (!ok) {
		fprintf(stderr, "usbhost: the port did not take Source again; the next attempt is in %d s\n",
				ROLE_REWRITE_SECONDS);
		return false;
	}
	// Said on the first one and then once a minute, because an empty port is
	// rewritten on this same 5-second beat for as long as the setting is on --
	// that is the setting working, not a fault, and a line per attempt would
	// bury whatever the log was opened for. A device that keeps dropping off
	// the bus reads differently: it is the same line, one a minute.
	if (probe.rewrites == 1 || probe.rewrites % 12 == 0) {
		char was[32];
		snprintf(was, sizeof(was), "%s", mode[0] ? mode : "?");
		fprintf(stderr, "usbhost: the port had gone back to %s with no device on the bus; Source and PHY host "
						"written again (rewrite %d)\n",
				was, probe.rewrites);
	}
	return true;
}

// Shared by the timed trial and the held mode. `held` is the whole difference:
// a held role has no deadline, and a DAC that goes away does not take it down
// either (see usbhostprobe_poll()).
static usbhost_probe_result_t begin_locked(bool held, const char *why) {
	pthread_mutex_lock(&probe.lock);
	if (atomic_load(&probe.active)) {
		pthread_mutex_unlock(&probe.lock);
		return USBHOST_PROBE_BUSY;
	}
	long long now = monotonic_seconds();
	char role[32], phy[PATH_MAX];
	if (now < 0 || !read_line(USBHOST_TCS_CFG, role, sizeof(role)) || !saved_role_valid(role) || !find_phy(phy, sizeof(phy))) {
		pthread_mutex_unlock(&probe.lock);
		return USBHOST_PROBE_UNSUPPORTED;
	}
	// The reservation checks that DAC mode, storage, ADB and a connected or
	// unknown UDC are not in use, and keeps gadget housekeeping off the port.
	if (!usb_external_host_reserve()) {
		pthread_mutex_unlock(&probe.lock);
		return USBHOST_PROBE_BUSY;
	}
	if (!make_journal(role)) {
		usb_external_host_release();
		pthread_mutex_unlock(&probe.lock);
		return USBHOST_PROBE_FAILED;
	}
	snprintf(probe.role, sizeof(probe.role), "%s", role);
	snprintf(probe.phy, sizeof(probe.phy), "%s", phy);
	probe.started = now;
	probe.absent_since = -1;
	probe.retry_at = 0;
	probe.checked_at = 0;
	probe.checks = 0;
	probe.rewritten_at = 0;
	probe.rewrites = 0;
	probe.had_card = probe.saw_interface = probe.stopping = probe.reported_failure = false;
	probe.held = held;
	probe.role_restored = false;
	probe.phy_cleared = true; // the PHY has not been touched yet
	atomic_store(&probe.active, true);

	if (!request_host_locked(why)) {
		pthread_mutex_unlock(&probe.lock);
		return USBHOST_PROBE_FAILED;
	}
	pthread_mutex_unlock(&probe.lock);
	return USBHOST_PROBE_STARTED;
}

usbhost_probe_result_t usbhostprobe_begin(void) {
	return begin_locked(false, "user requested a " STR(USBHOST_TRIAL_SECONDS) "-second external DAC trial");
}

usbhost_probe_result_t usbhostprobe_begin_held(void) {
	return begin_locked(true, "external USB DAC mode requested: the port is held as host until it is switched off");
}

bool usbhostprobe_held(void) {
	pthread_mutex_lock(&probe.lock);
	bool held = atomic_load(&probe.active) && probe.held && !atomic_load(&probe.stopping);
	pthread_mutex_unlock(&probe.lock);
	return held;
}

bool usbhostprobe_cancel(void) {
	pthread_mutex_lock(&probe.lock);
	bool ok = !atomic_load(&probe.active) || restore_locked();
	if (!ok) fprintf(stderr, "usbhost: restoration failed; the controller remains reserved for retry\n");
	pthread_mutex_unlock(&probe.lock);
	return ok;
}

usbhost_probe_event_t usbhostprobe_poll(bool audio_card, bool audio_interface) {
	pthread_mutex_lock(&probe.lock);
	if (!atomic_load(&probe.active)) {
		pthread_mutex_unlock(&probe.lock);
		return USBHOST_EVENT_NONE;
	}
	long long now = monotonic_seconds();
	usbhost_probe_event_t event = USBHOST_EVENT_NONE;
	if (audio_interface) probe.saw_interface = true;
	if (probe.stopping) {
		if (now < 0 || now >= probe.retry_at) {
			probe.retry_at = now < 0 ? 0 : now + USBHOST_RETRY_SECONDS;
			if (restore_locked()) event = USBHOST_EVENT_RESTORED;
			else if (!probe.reported_failure) {
				probe.reported_failure = true;
				fprintf(stderr, "usbhost: cannot restore the port yet; retrying\n");
				event = USBHOST_EVENT_RESTORE_FAILED;
			}
		}
	} else if (now < 0) {
		fprintf(stderr, "usbhost: the monotonic clock failed; restoring the port\n");
		event = restore_locked() ? USBHOST_EVENT_TIMEOUT : USBHOST_EVENT_RESTORE_FAILED;
	} else if (audio_card) {
		probe.absent_since = -1;
		if (!probe.had_card) {
			probe.had_card = true;
			fprintf(stderr, "usbhost: an ALSA USB card appeared during the trial\n");
			event = USBHOST_EVENT_CARD;
		}
	} else if (probe.held) {
		// A held role has no deadline and no unplug rule: it stays forced until
		// the user ends it, or until another owner of the controller ends it
		// through usbhostmode_tick().
		// A dongle that re-enumerates, or is plugged into the next morning,
		// finds the port still supplying it -- which is the whole point.
		if (probe.had_card && !audio_interface && probe.absent_since < 0) {
			probe.absent_since = now;
			fprintf(stderr, "usbhost: the audio device is no longer on the bus; the port stays host\n");
		}
		// And the one write a held role cannot do without: keeping itself
		// written. The driver re-decides the port's role on its own, and the
		// role it decides is the one it started in -- NormalDRP -- so a held
		// Source is not a setting, it is a state the far end has to agree to
		// every time the port changes. See keep_role_locked().
		keep_role_locked(now);
	} else if (probe.had_card) {
		// A momentary USB re-enumeration must not tear down a working DAC.
		if (audio_interface) probe.absent_since = -1;
		else if (probe.absent_since < 0) probe.absent_since = now;
		else if (now - probe.absent_since >= USBHOST_UNPLUG_SECONDS) {
			fprintf(stderr, "usbhost: the audio device left; restoring the port\n");
			event = restore_locked() ? USBHOST_EVENT_REMOVED : USBHOST_EVENT_RESTORE_FAILED;
		}
	} else if (now - probe.started >= (probe.saw_interface ? USBHOST_AUDIO_PROBE_SECONDS : USBHOST_TRIAL_SECONDS)) {
		fprintf(stderr, "usbhost: no ALSA USB card appeared%s; restoring the port\n",
			probe.saw_interface ? " (an audio interface was seen)" : "");
		event = restore_locked() ? USBHOST_EVENT_TIMEOUT : USBHOST_EVENT_RESTORE_FAILED;
	}
	if (event == USBHOST_EVENT_RESTORE_FAILED) probe.reported_failure = true;
	pthread_mutex_unlock(&probe.lock);
	return event;
}

void usbhostprobe_recover(void) {
	pthread_mutex_lock(&probe.lock);
	if (atomic_load(&probe.active)) {
		pthread_mutex_unlock(&probe.lock);
		return;
	}
	struct stat journal;
	if (lstat(USBHOST_JOURNAL, &journal) != 0) {
		if (errno == ENOENT) {
			// ENOENT also means the settings directory is missing. That is not
			// evidence that a persistent journal doesn't exist on the device.
			int parent = open_journal_parent();
			if (parent >= 0 && close(parent) == 0) {
				pthread_mutex_unlock(&probe.lock);
				return;
			}
		}
		// An inaccessible journal is not evidence that no recovery is needed.
		fprintf(stderr, "usbhost: cannot inspect the recovery journal; blocking USB gadget use\n");
		usb_external_host_block_recovery();
		atomic_store(&probe.active, true);
		probe.role[0] = '\0';
		probe.stopping = true;
		probe.reported_failure = true;
		pthread_mutex_unlock(&probe.lock);
		return;
	}
	probe.role[0] = '\0';
	if (!read_journal(probe.role, sizeof(probe.role))) {
		// Unknown bytes must never become a Type-C setting. Block gadget use
		// rather than guessing; reboot/reset behavior is firmware-dependent.
		fprintf(stderr, "usbhost: invalid recovery journal; refusing to change the port\n");
		usb_external_host_block_recovery();
		atomic_store(&probe.active, true);
		probe.stopping = true;
		probe.reported_failure = true;
		pthread_mutex_unlock(&probe.lock);
		return;
	}
	probe.phy[0] = '\0';
	probe.phy_cleared = false;
	probe.role_restored = false;
	probe.stopping = true;
	probe.reported_failure = false;
	probe.retry_at = 0;
	usb_external_host_block_recovery();
	atomic_store(&probe.active, true);
	fprintf(stderr, "usbhost: recovering an interrupted host trial before starting a gadget\n");
	if (!restore_locked()) fprintf(stderr, "usbhost: recovery is not complete; will retry\n");
	pthread_mutex_unlock(&probe.lock);
}
