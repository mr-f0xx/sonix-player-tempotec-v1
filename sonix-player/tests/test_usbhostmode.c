#define _GNU_SOURCE
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

// The external USB DAC setting: the state machine around the role hold, not the
// sysfs work (test_usbhostprobe.c covers that). What is checked here is what
// the user and the rest of the player see -- when the port is taken, what makes
// it be given back, and that "on" is never shown as "working".

static bool fake_wanted;
static long long fake_now_ms;
static bool fake_probe_active, fake_probe_held;
static bool fake_reserved = true;
static int starts, cancels;
static bool begin_result_started = true, begin_result_busy;
static bool cancel_ok = true;

static int fake_clock_gettime(clockid_t clock, struct timespec *ts) {
	assert(clock == CLOCK_MONOTONIC);
	ts->tv_sec = fake_now_ms / 1000;
	ts->tv_nsec = (fake_now_ms % 1000) * 1000000;
	return 0;
}

#define clock_gettime fake_clock_gettime
#include "../src/system/audio/usbhostmode.c"
#undef clock_gettime

// The config, in memory: the setting is one boolean and the test is not about
// config.c (test_tags and the regression suites cover that).
bool config_get_bool(const char *section, const char *key, bool fallback) {
	assert(strcmp(section, "usb") == 0);
	assert(strcmp(key, "host_dac") == 0);
	(void)fallback;
	return fake_wanted;
}
void config_set_bool(const char *section, const char *key, bool value) {
	assert(strcmp(section, "usb") == 0 && strcmp(key, "host_dac") == 0);
	fake_wanted = value;
}
bool config_save(void) { return true; }

// usbhostprobe.c
usbhost_probe_result_t usbhostprobe_begin_held(void) {
	starts++;
	if (begin_result_busy) return USBHOST_PROBE_BUSY;
	if (!begin_result_started) return USBHOST_PROBE_UNSUPPORTED;
	fake_probe_active = fake_probe_held = true;
	fake_reserved = true;
	return USBHOST_PROBE_STARTED;
}
bool usbhostprobe_cancel(void) {
	cancels++;
	if (!cancel_ok) return false;
	fake_probe_active = fake_probe_held = false;
	fake_reserved = false;
	return true;
}
bool usbhostprobe_active(void) { return fake_probe_active; }
bool usbhostprobe_held(void) { return fake_probe_held; }
bool usbhostprobe_restore_pending(void) { return false; }

// usb.c
bool usb_external_host_reserved(void) { return fake_reserved; }

static const char *status(void) { return usbhostmode_status_key(); }

static void reset(void) {
	fake_wanted = false;
	fake_now_ms = 1000;
	fake_probe_active = fake_probe_held = false;
	fake_reserved = true;
	starts = cancels = 0;
	begin_result_started = true;
	begin_result_busy = false;
	cancel_ok = true;
	usbhostmode_set_wanted(false);
	usbhostmode_tick();
}

int main(void) {
	// Off, and nothing on the port: the setting asks for nothing.
	reset();
	assert(!usbhostmode_wanted() && !usbhostmode_active());
	usbhostmode_tick();
	assert(starts == 0 && cancels == 0);
	assert(strcmp(status(), "") == 0);

	// Switched on: the port is taken on the next tick, and the row is told so.
	usbhostmode_set_wanted(true);
	assert(usbhostmode_wanted());
	usbhostmode_tick();
	assert(starts == 1);
	assert(usbhostmode_active());
	assert(strcmp(status(), "usbhost_held") == 0);
	assert(!usbhostmode_retrying());

	// Ticking again while held does not ask twice.
	usbhostmode_tick();
	usbhostmode_tick();
	assert(starts == 1);

	// A DAC appearing and a DAC being unplugged are the same setting working:
	// no popup, no release, and the port stays host.
	usbhostmode_event(USBHOST_EVENT_CARD);
	assert(strcmp(status(), "usbhost_held") == 0);
	usbhostmode_event(USBHOST_EVENT_REMOVED);
	assert(cancels == 0 && usbhostmode_active());
	usbhostmode_tick();
	assert(starts == 1 && usbhostmode_active());

	// Another owner takes the controller and gives the reservation back: the
	// held role cannot survive it, so it is released -- and the setting stays
	// where the user left it, waiting rather than switched off.
	fake_reserved = false;
	usbhostmode_tick();
	assert(cancels == 1);
	assert(!usbhostmode_active());
	assert(usbhostmode_wanted());
	assert(usbhostmode_retrying());
	assert(strcmp(status(), "usbhost_waiting") == 0);

	// It is not asked for again on the same tick, nor immediately: the other
	// owner is still using it.
	int before = starts;
	usbhostmode_tick();
	assert(starts == before);
	fake_now_ms += RETRY_MS - 1;
	usbhostmode_tick();
	assert(starts == before);

	// Once that owner is finished, the port is taken again without the user
	// having to switch anything off and on.
	fake_reserved = true;
	fake_now_ms += 2;
	usbhostmode_tick();
	assert(starts == before + 1 && usbhostmode_active());

	// A controller in use at switch-on (DAC mode, ADB) is reported as busy, not
	// as working, and the setting is kept so that it takes over later.
	reset();
	usbhostmode_set_wanted(true);
	begin_result_busy = true;
	usbhostmode_tick();
	assert(starts == 1 && !usbhostmode_active());
	assert(usbhostmode_retrying());
	assert(strcmp(status(), "usbhost_busy") == 0);
	fake_now_ms += RETRY_MS;
	begin_result_busy = false;
	usbhostmode_tick();
	assert(starts == 2 && usbhostmode_active());
	assert(!usbhostmode_retrying());

	// A kernel with no role that may be forced says so once and is not asked
	// again: nothing can change about it while the player runs.
	reset();
	usbhostmode_set_wanted(true);
	begin_result_started = false;
	usbhostmode_tick();
	assert(!usbhostmode_active() && !usbhostmode_retrying());
	assert(strcmp(status(), "usbhost_unsupported") == 0);
	fake_now_ms += 10 * RETRY_MS;
	usbhostmode_tick();
	assert(starts == 1);

	// Switching off in the interface gives the port back and says nothing more.
	reset();
	usbhostmode_set_wanted(true);
	usbhostmode_tick();
	assert(usbhostmode_active());
	usbhostmode_tick();
	assert(strcmp(status(), "usbhost_held") == 0);
	usbhostmode_set_wanted(false);
	usbhostmode_tick();
	assert(cancels == 1 && !usbhostmode_active());
	assert(strcmp(status(), "") == 0);

	// A failed restoration is reported and never hidden: the caller is told, so
	// that a role still forced can keep the port away from ADB and DAC mode.
	reset();
	usbhostmode_set_wanted(true);
	usbhostmode_tick();
	cancel_ok = false;
	assert(!usbhostmode_release(true));
	assert(!usbhostmode_wanted()); // the setting is still cleared
	assert(strcmp(status(), "usbhost_restore_failed") == 0);
	assert(usbhostmode_active()); // still held by the probe, which keeps retrying

	// RELEASE WITHOUT CLEARING is what DAC mode and ADB use: they need the
	// controller for a while, and the port goes back to holding a DAC after.
	cancel_ok = true;
	reset();
	usbhostmode_set_wanted(true);
	usbhostmode_tick();
	assert(usbhostmode_release(false));
	assert(!usbhostmode_active());
	assert(usbhostmode_wanted());
	assert(strcmp(status(), "usbhost_waiting") == 0);

	puts("PASS external USB DAC setting (hold, yield to another owner, retry and release)");
	return 0;
}
