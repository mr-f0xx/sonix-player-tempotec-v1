#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static char test_cfg[256], test_platform[256], test_journal[256], test_phy[256];
static long long fake_now;
static int fail_phy_writes;
static bool fail_tcs_write, fail_directory_sync;
static bool reserve_allowed = true, restore_safe = true, reserved;

static int fake_fsync(int fd) {
	struct stat st;
	if (fail_directory_sync && fstat(fd, &st) == 0 && S_ISDIR(st.st_mode)) {
		errno = EIO;
		return -1;
	}
	return fsync(fd);
}

static int fake_clock_gettime(clockid_t clock, struct timespec *ts) {
	assert(clock == CLOCK_MONOTONIC);
	ts->tv_sec = fake_now;
	ts->tv_nsec = 0;
	return 0;
}

static FILE *fake_fopen(const char *path, const char *mode) {
	if (fail_tcs_write && strcmp(path, test_cfg) == 0 && strcmp(mode, "w") == 0) {
		errno = EACCES;
		return NULL;
	}
	if (fail_phy_writes && strcmp(path, test_phy) == 0 && strcmp(mode, "w") == 0) {
		if (fail_phy_writes > 0) fail_phy_writes--;
		errno = EACCES;
		return NULL;
	}
	return fopen(path, mode);
}

#define USBHOST_TCS_CFG test_cfg
#define USBHOST_PLATFORM_DEVICES test_platform
#define USBHOST_JOURNAL test_journal
#define clock_gettime fake_clock_gettime
#define fopen fake_fopen
#define fsync fake_fsync
#include "../src/system/audio/usbhostprobe.c"
#undef fsync
#undef fopen
#undef clock_gettime

bool usb_external_host_reserve(void) {
	if (!reserve_allowed || reserved) return false;
	reserved = true;
	return true;
}
void usb_external_host_release(void) { reserved = false; }
void usb_external_host_block_recovery(void) { reserved = true; }
bool usb_external_host_reserved(void) { return reserved; }
bool usb_external_host_restore_safe(void) { return restore_safe; }

static void pathcat(char *out, size_t size, const char *a, const char *b) {
	assert(strlen(a) + strlen(b) + 1 <= size);
	strcpy(out, a);
	strcat(out, b);
}

static void put_bytes(const char *path, const void *data, size_t n) {
	FILE *f = fopen(path, "wb");
	assert(f);
	assert(fwrite(data, 1, n, f) == n);
	assert(fclose(f) == 0);
}
static void put(const char *path, const char *text) { put_bytes(path, text, strlen(text)); }
static void expect(const char *path, const char *text) {
	char out[256];
	FILE *f = fopen(path, "r");
	assert(f && fgets(out, sizeof(out), f));
	assert(fclose(f) == 0);
	assert(strcmp(out, text) == 0);
}
static void make_dir(const char *path) { assert(mkdir(path, 0700) == 0); }

// Simulate a player process dying while the journal and sysfs persist. The
// mutex is left alone; only process-owned state and its reservation disappear.
static void simulate_player_restart(void) {
	assert(pthread_mutex_lock(&probe.lock) == 0);
	atomic_store(&probe.active, false);
	probe.stopping = probe.had_card = probe.saw_interface = probe.phy_cleared = probe.role_restored = probe.reported_failure = false;
	probe.started = probe.absent_since = probe.retry_at = 0;
	probe.role[0] = probe.phy[0] = '\0';
	reserved = false;
	assert(pthread_mutex_unlock(&probe.lock) == 0);
}

int main(void) {
	char root[] = "/tmp/sonix-usbhost-XXXXXX";
	assert(mkdtemp(root));
	pathcat(test_cfg, sizeof(test_cfg), root, "/tcs_cfg");
	pathcat(test_platform, sizeof(test_platform), root, "/platform");
	pathcat(test_journal, sizeof(test_journal), root, "/journal");
	make_dir(test_platform);
	put(test_cfg, "NormalDRP\n");

	// A missing journal directory is not proof that the journal is absent.
	char journal_path[256];
	snprintf(journal_path, sizeof(journal_path), "%s", test_journal);
	pathcat(test_journal, sizeof(test_journal), root, "/missing/journal");
	usbhostprobe_recover();
	assert(usbhostprobe_active() && usbhostprobe_restore_pending() && reserved);
	assert(!usbhostprobe_cancel());
	simulate_player_restart();
	snprintf(test_journal, sizeof(test_journal), "%s", journal_path);

	// An old/unsupported kernel must never receive an invented host write.
	assert(usbhostprobe_begin() == USBHOST_PROBE_UNSUPPORTED);
	assert(!reserved && access(test_journal, F_OK) != 0);
	expect(test_cfg, "NormalDRP\n");

	char dev[256], ofnode[256], compatible[256];
	pathcat(dev, sizeof(dev), test_platform, "/phy0");
	pathcat(ofnode, sizeof(ofnode), dev, "/of_node");
	pathcat(compatible, sizeof(compatible), ofnode, "/compatible");
	pathcat(test_phy, sizeof(test_phy), dev, "/sw_switch_hsotg");
	make_dir(dev);
	make_dir(ofnode);
	static const char compat[] = "other,phy\0ingenic,usbphy-x1600\0";
	put_bytes(compatible, compat, sizeof(compat) - 1);
	put(test_phy, "none\n");

	// Two compatible PHYs are ambiguous: never choose an arbitrary device.
	char dev2[256], ofnode2[256], compatible2[256], phy2[256];
	pathcat(dev2, sizeof(dev2), test_platform, "/phy1");
	pathcat(ofnode2, sizeof(ofnode2), dev2, "/of_node");
	pathcat(compatible2, sizeof(compatible2), ofnode2, "/compatible");
	pathcat(phy2, sizeof(phy2), dev2, "/sw_switch_hsotg");
	make_dir(dev2); make_dir(ofnode2);
	put_bytes(compatible2, compat, sizeof(compat) - 1); put(phy2, "none\n");
	assert(usbhostprobe_begin() == USBHOST_PROBE_UNSUPPORTED);
	expect(test_cfg, "NormalDRP\n");
	assert(unlink(phy2) == 0);
	assert(usbhostprobe_begin() == USBHOST_PROBE_UNSUPPORTED); // second PHY still makes it ambiguous
	assert(unlink(compatible2) == 0 && rmdir(ofnode2) == 0 && rmdir(dev2) == 0);

	reserve_allowed = false; // represents active ADB, a PC or an unknown UDC
	assert(usbhostprobe_begin() == USBHOST_PROBE_BUSY);
	assert(!reserved && access(test_journal, F_OK) != 0);
	expect(test_cfg, "NormalDRP\n");
	expect(test_phy, "none\n");
	reserve_allowed = true;

	// No role change unless the journal and its directory entry are durable.
	fail_directory_sync = true;
	assert(usbhostprobe_begin() == USBHOST_PROBE_FAILED);
	assert(!reserved && !usbhostprobe_active() && access(test_journal, F_OK) != 0);
	expect(test_cfg, "NormalDRP\n");
	expect(test_phy, "none\n");
	fail_directory_sync = false;

	// A failed PHY request rolls back the Type-C mode and releases ownership.
	fail_phy_writes = 1;
	assert(usbhostprobe_begin() == USBHOST_PROBE_FAILED);
	assert(!usbhostprobe_active() && !reserved && access(test_journal, F_OK) != 0);
	expect(test_cfg, "NormalDRP\n");
	expect(test_phy, "none\n");

	// No sound card: the time-limited try returns both controls to their
	// original state, without interpreting VBUS as a device.
	fake_now = 10;
	assert(usbhostprobe_begin() == USBHOST_PROBE_STARTED);
	assert(usbhostprobe_active() && !usbhostprobe_restore_pending() && reserved && access(test_journal, F_OK) == 0);
	expect(test_cfg, "Source\n");
	expect(test_phy, "host\n");
	fake_now += USBHOST_TRIAL_SECONDS - 1;
	assert(usbhostprobe_poll(false, false) == USBHOST_EVENT_NONE);
	fake_now++;
	assert(usbhostprobe_poll(false, false) == USBHOST_EVENT_TIMEOUT);
	assert(!usbhostprobe_active() && !reserved && access(test_journal, F_OK) != 0);
	expect(test_cfg, "NormalDRP\n");
	expect(test_phy, "none\n");

	// An audio interface gives ALSA a longer but still bounded probe window.
	assert(usbhostprobe_begin() == USBHOST_PROBE_STARTED);
	fake_now += USBHOST_TRIAL_SECONDS;
	assert(usbhostprobe_poll(false, true) == USBHOST_EVENT_NONE);
	// An interface disappearing briefly does not erase the longer grace period.
	fake_now++;
	assert(usbhostprobe_poll(false, false) == USBHOST_EVENT_NONE);
	fake_now += USBHOST_AUDIO_PROBE_SECONDS - USBHOST_TRIAL_SECONDS - 1;
	assert(usbhostprobe_poll(false, false) == USBHOST_EVENT_TIMEOUT);

	// Once a card exists, keep the override; a brief missing-card gap and an
	// audio interface still being probed do not count as an unplug.
	put(test_cfg, "StrongDRP\n");
	assert(usbhostprobe_begin() == USBHOST_PROBE_STARTED);
	assert(usbhostprobe_poll(true, true) == USBHOST_EVENT_CARD);
	fake_now += 1000;
	assert(usbhostprobe_poll(true, true) == USBHOST_EVENT_NONE);
	assert(usbhostprobe_poll(false, false) == USBHOST_EVENT_NONE);
	fake_now += 2;
	assert(usbhostprobe_poll(true, true) == USBHOST_EVENT_NONE);
	assert(usbhostprobe_poll(false, true) == USBHOST_EVENT_NONE);
	fake_now += 10;
	assert(usbhostprobe_poll(false, true) == USBHOST_EVENT_NONE);
	assert(usbhostprobe_poll(false, false) == USBHOST_EVENT_NONE);
	fake_now += USBHOST_UNPLUG_SECONDS;
	assert(usbhostprobe_poll(false, false) == USBHOST_EVENT_REMOVED);
	expect(test_cfg, "StrongDRP\n");
	expect(test_phy, "none\n");
	assert(!reserved);

	// A failed restore must not hand the controller to ADB/storage. A later
	// successful retry clears the override and releases the reservation.
	put(test_cfg, "Sink\n");
	assert(usbhostprobe_begin() == USBHOST_PROBE_STARTED);
	fail_phy_writes = -1;
	assert(!usbhostprobe_cancel());
	assert(usbhostprobe_active() && usbhostprobe_restore_pending() && reserved && access(test_journal, F_OK) == 0);
	expect(test_cfg, "Source\n");
	fail_phy_writes = 0;
	fake_now++;
	assert(usbhostprobe_poll(false, false) == USBHOST_EVENT_RESTORED);
	assert(!usbhostprobe_restore_pending());
	expect(test_cfg, "Sink\n");
	expect(test_phy, "none\n");
	assert(!reserved);

	// A Type-C restore failure occurs after the PHY was already cleared.
	// Retrying must not need to reapply the PHY command.
	put(test_cfg, "NormalDRP\n");
	assert(usbhostprobe_begin() == USBHOST_PROBE_STARTED);
	fail_tcs_write = true;
	assert(!usbhostprobe_cancel());
	assert(usbhostprobe_active() && reserved);
	expect(test_phy, "none\n");
	fail_tcs_write = false;
	fail_phy_writes = -1; // would fail if the already-cleared PHY were rewritten
	fake_now++;
	assert(usbhostprobe_poll(false, false) == USBHOST_EVENT_RESTORED);
	assert(!reserved);
	fail_phy_writes = 0;
	expect(test_cfg, "NormalDRP\n");

	// An unlinked journal is not released until the directory sync succeeds.
	assert(usbhostprobe_begin() == USBHOST_PROBE_STARTED);
	fail_directory_sync = true;
	assert(!usbhostprobe_cancel());
	assert(usbhostprobe_active() && reserved);
	expect(test_cfg, "NormalDRP\n");
	expect(test_phy, "none\n");
	fail_directory_sync = false;
	fake_now++;
	assert(usbhostprobe_poll(false, false) == USBHOST_EVENT_RESTORED);
	assert(!reserved && access(test_journal, F_OK) != 0);

	if (geteuid() != 0) { // root can unlink regardless of directory mode
		assert(usbhostprobe_begin() == USBHOST_PROBE_STARTED);
		assert(chmod(root, 0500) == 0);
		assert(!usbhostprobe_cancel());
		assert(usbhostprobe_active() && reserved && access(test_journal, F_OK) == 0);
		assert(chmod(root, 0700) == 0);
		fail_tcs_write = true; // role is already restored; only unlink remains
		fake_now++;
		assert(usbhostprobe_poll(false, false) == USBHOST_EVENT_RESTORED);
		fail_tcs_write = false;
		assert(!reserved && access(test_journal, F_OK) != 0);
	}

	// The journal recovers an interrupted trial before any gadget may bind.
	put(test_cfg, "NormalDRP\n");
	assert(usbhostprobe_begin() == USBHOST_PROBE_STARTED);
	simulate_player_restart();
	restore_safe = false; // a PC or unknown gadget claimed the port after the crash
	usbhostprobe_recover();
	assert(usbhostprobe_active() && reserved && access(test_journal, F_OK) == 0);
	expect(test_cfg, "Source\n");
	expect(test_phy, "host\n");
	restore_safe = true;
	fake_now++;
	assert(usbhostprobe_poll(false, false) == USBHOST_EVENT_RESTORED);
	assert(!usbhostprobe_active() && !reserved && access(test_journal, F_OK) != 0);
	expect(test_cfg, "NormalDRP\n");
	expect(test_phy, "none\n");

	// Corrupt journals never become a role or a PHY command; recovery fails
	// closed instead of handing an unknown forced controller to a gadget.
	put(test_cfg, "NormalDRP\n");
	put(test_phy, "unchanged\n");
	put(test_journal, "bad\nSource\n");
	usbhostprobe_recover();
	assert(usbhostprobe_active() && reserved);
	assert(!usbhostprobe_cancel());
	expect(test_cfg, "NormalDRP\n");
	expect(test_phy, "unchanged\n");
	simulate_player_restart();
	assert(unlink(test_journal) == 0);

	// A dangling journal symlink must not look like an absent journal.
	assert(symlink("/nonexistent/sonix-test-journal", test_journal) == 0);
	usbhostprobe_recover();
	assert(usbhostprobe_active() && reserved && !usbhostprobe_cancel());
	expect(test_cfg, "NormalDRP\n");
	simulate_player_restart();
	assert(unlink(test_journal) == 0);

	// Refuse an existing symlink rather than overwrite its target.
	char victim[256];
	pathcat(victim, sizeof(victim), root, "/victim");
	put(victim, "keep\n");
	assert(symlink(victim, test_journal) == 0);
	assert(usbhostprobe_begin() == USBHOST_PROBE_FAILED);
	expect(victim, "keep\n");
	expect(test_cfg, "NormalDRP\n");
	assert(!reserved);
	assert(unlink(test_journal) == 0);

	assert(unlink(victim) == 0);
	assert(unlink(test_phy) == 0);
	assert(unlink(compatible) == 0);
	assert(rmdir(ofnode) == 0);
	assert(rmdir(dev) == 0);
	assert(rmdir(test_platform) == 0);
	assert(unlink(test_cfg) == 0);
	assert(rmdir(root) == 0);
	puts("PASS usbhostprobe (guarded role trial, timeout, rollback and crash recovery)");
	return 0;
}
