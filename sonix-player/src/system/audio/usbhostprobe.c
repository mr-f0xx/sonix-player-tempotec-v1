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

static struct {
	pthread_mutex_t lock;
	atomic_bool active, stopping;
	bool had_card, saw_interface, phy_cleared, role_restored, reported_failure;
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

usbhost_probe_result_t usbhostprobe_begin(void) {
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
	probe.had_card = probe.saw_interface = probe.stopping = probe.reported_failure = false;
	probe.role_restored = false;
	probe.phy_cleared = true; // the PHY has not been touched yet
	atomic_store(&probe.active, true);

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
		pthread_mutex_unlock(&probe.lock);
		return USBHOST_PROBE_FAILED;
	}
	fprintf(stderr, "usbhost: user requested a %d-second external DAC trial; Type-C Source and PHY host requested (%s)\n",
		USBHOST_TRIAL_SECONDS, probe.phy);
	pthread_mutex_unlock(&probe.lock);
	return USBHOST_PROBE_STARTED;
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
