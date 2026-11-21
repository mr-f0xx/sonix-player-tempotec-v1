#include "ota_auto.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "lvgl/lvgl.h"

#include "src/gui/shell/confirm.h"
#include "src/gui/shell/gui.h"
#include "src/gui/shell/icons.h"
#include "src/gui/shell/toast.h"
#include "src/gui/shell/topbar.h"
#include "src/gui/settings/fwupdate.h"
#include "src/system/core/config.h"
#include "src/system/core/lang.h"
#include "src/system/device/led.h"
#include "src/system/device/ota.h"
#include "src/system/device/sysinfo.h"
#include "src/system/device/system.h"
#include "src/system/net/wifi.h"

// One check per this period, while the radio is connected. The GitHub
// unauthenticated API allows 60 requests an hour per IP, so even an hourly
// check is nowhere near the limit; six hours is the cadence that keeps a stale
// "update available" from sitting on screen for longer than a day.
#define PERIODIC_MS (6u * 60u * 60u * 1000u)

// The minimum gap between two connect-triggered checks, however often the
// link flaps. The poll fires every two seconds while connected.
#define CONNECT_DEBOUNCE_MS 300000u

// The notification poll rides a 250 ms beat, the same rhythm the rest of the
// interface uses for watched jobs.
#define POLL_MS 250

// ---------------------------------------------------------------------------
// state
//
// s_dismissed_tag  a tag the user has pressed "Later" for: the notification is
//                  not brought up again for it, this boot or any later one.
// s_taken_tag      the tag the user has made the update for: the notice, the
//                  LED blink and the standing dot never come back for it,
//                  this boot or any later one. Persisted as ota_taken_tag.
// s_shown_tag      the tag of the notice most recently brought up: a repeat
//                  check for that tag does not re-show it. Also set to the
//                  tag the user has just made the update for, so the notice
//                  never comes back for it.
// s_connect_last   last connect-triggered check, monotonic ms.
// s_inflight       the module started the OTA check currently running: only
//                  those results are ours to show, so the manual card and the
//                  popup for an update do not fight over one result.
// s_enabled        the Settings toggle (auto_update_notices, default on).
// ---------------------------------------------------------------------------

static char s_dismissed_tag[32];
static char s_taken_tag[32];
static char s_shown_tag[32];
static uint32_t s_connect_last;
static bool s_inflight;
static lv_timer_t *s_poll_timer;
static lv_timer_t *s_periodic_timer;
// Read from the Wi-Fi worker and written from the interface thread; a plain
// bool would race.
static bool s_enabled = true;

static bool enabled(void) {
	return __atomic_load_n(&s_enabled, __ATOMIC_RELAXED);
}
static void set_enabled(bool on) {
	__atomic_store_n(&s_enabled, on, __ATOMIC_RELAXED);
}

// The same guard the Firmware update row uses: an update that loses power
// halfway is the one failure here that cannot be undone from the UI. An
// unreadable level must not block the check: the reader returns "!!" when it
// cannot find the gauge, and refusing to check over a battery nobody can
// measure would be the opposite of useful.
static bool battery_allows_update(void) {
	char *percent_text = read_battery_percent();
	int percent = 100;
	if (percent_text && percent_text[0] >= '0' && percent_text[0] <= '9') {
		percent = atoi(percent_text);
	}
	return percent >= 30 || read_battery_charging();
}

// ---------------------------------------------------------------------------
// the check
// ---------------------------------------------------------------------------

static void try_start_check(void) {
	if (!enabled()) {
		return;
	}
	if (ota_busy()) {
		return;
	}
	if (!battery_allows_update()) {
		return;
	}
	wifi_status_t status;
	wifi_get_status(&status);
	if (status.state != WIFI_STATE_CONNECTED || !status.ip[0]) {
		return;
	}
	if (ota_check_start()) {
		s_inflight = true;
	}
}

static uint32_t monotonic_ms(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint32_t)(ts.tv_sec * 1000) + (uint32_t)(ts.tv_nsec / 1000000);
}

void ota_auto_link_online(void) {
	if (!enabled()) {
		return;
	}
	if (ota_busy()) {
		return;
	}
	uint32_t now = monotonic_ms();
	if (s_connect_last != 0 && now - s_connect_last < CONNECT_DEBOUNCE_MS) {
		return;
	}
	s_connect_last = now;
	try_start_check();
}

// ---------------------------------------------------------------------------
// the notice
// ---------------------------------------------------------------------------

// The dot is the standing reminder: it stays up until the user does the
// update or dismisses it from the popup. A dot nobody can tap into anything
// would be decor, so a tap re-brings the popup.
static void show_notice(bool led);
static void dot_tap_cb(void) {
	show_notice(false);
}

static void update_pressed_cb(void *user) {
	(void)user;
	topbar_set_update_available(false);
	if (!battery_allows_update()) {
		gui_notify_popup("system_battery_warning");
		return;
	}
	fwupdate_start_download();
}

// "Later" is the deferral: the popup does not come back for this release, the
// standing dot stays, and a transient toast says where the reminder is. The
// tag is remembered, so a later boot does not bring the popup up again.
static void later_pressed_cb(void *user) {
	(void)user;
	config_set("system", "ota_dismissed_tag", s_shown_tag);
	config_save();
	snprintf(s_dismissed_tag, sizeof(s_dismissed_tag), "%s", s_shown_tag);
	toast_glyph(&icon_download, "system_update_available");
}

static void show_notice(bool led) {
	const ota_release_t *rel = ota_release();
	if (!rel->tag[0]) {
		return;
	}
	snprintf(s_shown_tag, sizeof(s_shown_tag), "%s", rel->tag);

	if (led) {
		// A notice found by the check blinks the LED; a tap on the dot does
		// not re-blink.
		led_notify_update_ready();
	}
	topbar_set_update_available(true);

	char size_text[32], message[160];
	sysinfo_format_size((uint64_t)rel->size, size_text, sizeof(size_text));
	snprintf(message, sizeof(message), tr("system_update_version_size"), rel->tag, size_text);
	confirm_show_with_cancel("system_update_firmware", message, "system_update", update_pressed_cb, NULL, "system_later",
							  later_pressed_cb, NULL);
}

static void poll_cb(lv_timer_t *timer) {
	(void)timer;
	if (!s_inflight) {
		return; // not ours: the manual card polls this result
	}
	ota_state_t s = ota_state();
	if (s != OTA_FINISHED) {
		return;
	}
	ota_result_t r = ota_result();
	ota_acknowledge();
	s_inflight = false;
	if (r != OTA_OK) {
		return; // no network, no card-sized release, check failed: silent
	}
	const ota_release_t *rel = ota_release();
	if (s_taken_tag[0] && strcmp(rel->tag, s_taken_tag) == 0) {
		// The user made the update for this release: it must not come back
		// as a notice, a blink or the standing dot, even across a restart.
		topbar_set_update_available(false);
		return;
	}
	if (strcmp(rel->tag, s_dismissed_tag) == 0) {
		// The user deferred this release: the dot still says it is available.
		topbar_set_update_available(true);
		return;
	}
	if (strcmp(rel->tag, s_shown_tag) == 0) {
		// Already brought up, or brought up for this release this boot.
		return;
	}
	show_notice(true);
}

static void periodic_cb(lv_timer_t *timer) {
	(void)timer;
	try_start_check();
}

void ota_auto_release_taken(void) {
	const ota_release_t *rel = ota_release();
	if (rel->tag[0]) {
		snprintf(s_shown_tag, sizeof(s_shown_tag), "%s", rel->tag);
		snprintf(s_taken_tag, sizeof(s_taken_tag), "%s", rel->tag);
		// The taken release is remembered, not only this boot: after a
		// restart the check finds the same release and the notice must not
		// come back for it.
		config_set("system", "ota_taken_tag", rel->tag);
		// A remembered deferral for this release no longer applies: the
		// update is made, not deferred any more, and the standing dot must
		// not come back for it.
		if (strcmp(s_dismissed_tag, rel->tag) == 0) {
			s_dismissed_tag[0] = '\0';
			config_set("system", "ota_dismissed_tag", "");
		}
		config_save();
	}
	topbar_set_update_available(false);
}

// ---------------------------------------------------------------------------
// public API
// ---------------------------------------------------------------------------

void ota_auto_init(void) {
	if (s_periodic_timer) {
		return;
	}
	snprintf(s_dismissed_tag, sizeof(s_dismissed_tag), "%s", config_get("system", "ota_dismissed_tag", ""));
	s_dismissed_tag[sizeof(s_dismissed_tag) - 1] = '\0';
	snprintf(s_taken_tag, sizeof(s_taken_tag), "%s", config_get("system", "ota_taken_tag", ""));
	s_taken_tag[sizeof(s_taken_tag) - 1] = '\0';
	s_shown_tag[0] = '\0';
	set_enabled(config_get_bool("system", "auto_update_notices", true));

	topbar_set_update_tap_cb(dot_tap_cb);

	s_poll_timer = lv_timer_create(poll_cb, POLL_MS, NULL);
	s_periodic_timer = lv_timer_create(periodic_cb, PERIODIC_MS, NULL);
}

bool ota_auto_enabled(void) { return enabled(); }

void ota_auto_set_enabled(bool on) {
	set_enabled(on);
	config_set_bool("system", "auto_update_notices", on);
	config_save();
	if (!on) {
		topbar_set_update_available(false);
	}
}
