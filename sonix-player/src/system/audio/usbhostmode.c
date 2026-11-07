#include "usbhostmode.h"

#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "src/system/core/config.h"
#include "src/system/device/usb.h"

// The setting. Under [usb] beside DAC mode's own keys, because it is the port's
// behaviour rather than a developer experiment -- developer options is only
// where it is switched on, next to the trial it grew out of.
#define HOSTMODE_SECTION "usb"
#define HOSTMODE_KEY "host_dac"

// How long to leave the controller alone after it was found busy. Long enough
// that a player sitting in DAC mode or with ADB on is not asking once a second
// for the rest of the night, short enough that switching the other owner off is
// followed within a few seconds.
#define RETRY_MS 3000

// Everything below is guarded by `lock` rather than being atomic: these answers
// travel together, and a reader that saw one without the other would describe a
// port that never existed.
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static bool retrying;      // the setting is on, the controller was busy
static bool unsupported;   // this kernel has no role to force: do not ask again
static long long retry_at; // monotonic ms, when asking again is allowed
static const char *status_key;

static long long now_ms(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

bool usbhostmode_wanted(void) {
	return config_get_bool(HOSTMODE_SECTION, HOSTMODE_KEY, false);
}

void usbhostmode_set_wanted(bool on) {
	// A deliberate switch-on clears what the last one learned: the user is
	// asking again, not resuming an argument the player already lost.
	if (on) {
		pthread_mutex_lock(&lock);
		retrying = false;
		unsupported = false;
		retry_at = 0;
		status_key = NULL;
		pthread_mutex_unlock(&lock);
	}
	config_set_bool(HOSTMODE_SECTION, HOSTMODE_KEY, on);
	config_save();
}

bool usbhostmode_active(void) { return usbhostprobe_held(); }

bool usbhostmode_retrying(void) {
	pthread_mutex_lock(&lock);
	bool waiting = retrying;
	pthread_mutex_unlock(&lock);
	return waiting;
}

const char *usbhostmode_status_key(void) {
	pthread_mutex_lock(&lock);
	const char *key = status_key;
	pthread_mutex_unlock(&lock);
	return key ? key : "";
}

// Takes the port, or says why it could not. `why` names the caller in the log,
// so a setting that is on but not in force reads differently from one that
// never tried.
static void start_if_clear(const char *why) {
	usbhost_probe_result_t result = usbhostprobe_begin_held();
	const char *key = result == USBHOST_PROBE_STARTED ? "usbhost_held" :
		result == USBHOST_PROBE_BUSY ? "usbhost_busy" :
		result == USBHOST_PROBE_UNSUPPORTED ? "usbhost_unsupported" : "usbhost_failed";

	pthread_mutex_lock(&lock);
	bool changed = !status_key || strcmp(status_key, key) != 0;
	status_key = key;
	retrying = result == USBHOST_PROBE_BUSY;
	unsupported = result == USBHOST_PROBE_UNSUPPORTED;
	// A refusal that will pass (another owner) is retried on a slow beat; one
	// that will not (no role attribute on this kernel) is not retried at all,
	// because nothing about it can change while the player runs.
	retry_at = retrying ? now_ms() + RETRY_MS : 0;
	pthread_mutex_unlock(&lock);

	if (changed) {
		fprintf(stderr, "usbhost: %s: %s\n", why,
				result == USBHOST_PROBE_STARTED ? "the port is now held as host (external USB audio)" :
				result == USBHOST_PROBE_BUSY ? "the controller is in use; will ask again" :
				result == USBHOST_PROBE_UNSUPPORTED ? "this kernel exposes no role that may be forced" :
				"could not request host mode");
	}
}

// Gives the port back and tells the interface. Returns whether the role really
// went back; a false here means the probe is still holding it and retrying, and
// the user is warned rather than left with a port nobody accounts for.
static bool stop(void) {
	if (!usbhostprobe_active()) {
		return true;
	}
	bool ok = usbhostprobe_cancel();
	pthread_mutex_lock(&lock);
	retrying = false;
	retry_at = 0;
	pthread_mutex_unlock(&lock);
	usbhostmode_event(ok ? USBHOST_EVENT_RESTORED : USBHOST_EVENT_RESTORE_FAILED);
	return ok;
}

void usbhostmode_tick(void) {
	// Another owner took the controller and gave the reservation back: ADB, DAC
	// mode, a storage export, suspend. A held role cannot survive that -- the
	// Source/host override is exactly what they had to undo -- so this is an
	// unplug from the player's side rather than a switch the user flipped. The
	// setting stays where the user left it and the port is taken again when the
	// other owner is finished.
	// Defensive: every path that clears the reservation restores the role
	// first, but a held role that outlived its reservation would be a forced
	// Source nobody accounts for, which is the one state this must never leave.
	if (usbhostprobe_held() && !usb_external_host_reserved()) {
		fprintf(stderr, "usbhost: the reservation is gone; releasing the held host role\n");
		bool ok = usbhostprobe_cancel();
		pthread_mutex_lock(&lock);
		retrying = usbhostmode_wanted();
		unsupported = false; // the other owner's release says the role exists
		retry_at = retrying ? now_ms() + RETRY_MS : 0;
		status_key = retrying ? "usbhost_waiting" : NULL;
		pthread_mutex_unlock(&lock);
		usbhostmode_event(ok ? USBHOST_EVENT_REMOVED : USBHOST_EVENT_RESTORE_FAILED);
		return;
	}

	if (!usbhostmode_wanted()) {
		if (usbhostprobe_held()) {
			// Switched off in the interface, or cleared by whatever owns the
			// setting (a factory reset, a firmware update). Give the port back
			// before anything else can see it.
			stop();
			pthread_mutex_lock(&lock);
			status_key = NULL;
			pthread_mutex_unlock(&lock);
		}
		return;
	}

	if (usbhostprobe_active()) {
		// Held, or the trial from developer options is running (which owns the
		// port for its twenty seconds; the setting waits it out).
		return;
	}

	pthread_mutex_lock(&lock);
	bool wait = unsupported || (retrying && now_ms() < retry_at);
	pthread_mutex_unlock(&lock);
	if (wait) {
		return;
	}
	start_if_clear("external USB DAC setting");
}

void usbhostmode_event(usbhost_probe_event_t event) {
	pthread_mutex_lock(&lock);
	switch (event) {
	case USBHOST_EVENT_CARD:
		status_key = "usbhost_held";
		break;
	case USBHOST_EVENT_TIMEOUT:
		// Only the timed trial ends this way, and the trial is not this
		// module's: a held role has no deadline to run out.
		break;
	case USBHOST_EVENT_REMOVED:
		// Another owner took the controller (the trial's own unplug never
		// reaches here), and the setting waits for it.
		status_key = usbhostmode_wanted() ? "usbhost_waiting" : NULL;
		if (usbhostmode_wanted()) {
			retrying = true;
			retry_at = now_ms() + RETRY_MS;
		}
		break;
	case USBHOST_EVENT_RESTORE_FAILED:
		status_key = "usbhost_restore_failed";
		break;
	case USBHOST_EVENT_RESTORED:
		status_key = usbhostmode_wanted() ? "usbhost_waiting" : NULL;
		break;
	case USBHOST_EVENT_NONE:
		break;
	}
	pthread_mutex_unlock(&lock);
}

bool usbhostmode_release(bool clear_setting) {
	if (clear_setting) {
		config_set_bool(HOSTMODE_SECTION, HOSTMODE_KEY, false);
		config_save();
	}
	bool ok = stop();
	pthread_mutex_lock(&lock);
	if (ok) {
		unsupported = false;
		status_key = clear_setting ? NULL : "usbhost_waiting";
	}
	pthread_mutex_unlock(&lock);
	return ok;
}

void usbhostmode_log_state(const char *why) {
	fprintf(stderr, "usbhost: %s: setting %s, port %s, status '%s', asking again %s\n", why,
			usbhostmode_wanted() ? "on" : "off",
			usbhostprobe_active() ? (usbhostprobe_held() ? "held as host" : "in the timed trial") : "not host",
			usbhostmode_status_key(), usbhostmode_retrying() ? "yes" : "no");
}
