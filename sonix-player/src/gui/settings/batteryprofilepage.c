#include "batteryprofilepage.h"

#include "src/gui/board_profile.h"
#include "src/gui/fonts/fonts.h"
#include "src/gui/shell/settingsrow.h"
#include "src/gui/shell/switcher.h"
#include "src/system/audio/alsa-controls.h"
#include "src/system/audio/audio.h"
#include "src/system/audio/eq.h"
#include "src/system/core/lang.h"
#include "src/system/device/axpcharge.h"
#include "src/system/device/batteryguard.h"
#include "src/system/device/batteryhistory.h"
#include "src/system/device/batteryprofile.h"
#include "src/system/device/power.h"
#include "src/system/device/system.h"
#include "src/system/device/usb.h"
#include "src/system/net/wifi.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define POLL_MS 2000
#define DARK_POLL_MS 5000
#define NO_VALUE "\xE2\x80\x94"

#ifdef HOST_BUILD
static const char *const supply_dir = NULL;
#else
static const char *const supply_dir = "/sys/class/power_supply";
#endif

// These are load settings and playback state, not a table of guessed costs.
// Shifts in the tracked settings start a new sample, but the saved baseline
// stays put. Automatic screen blanking is allowed during both runs.
typedef struct {
	volume_output_t output;
	int volume, sample_rate, dsd_multiple;
	long brightness;
	wifi_state_t wifi;
	bool playing, high_gain, peq, geq, lineout;
} load_t;

static lv_obj_t *screen;
static lv_timer_t *poll;
static lv_obj_t *level_value, *voltage_value, *charge_limit_value;
static lv_obj_t *last_charge_value, *charge_point_value, *on_time_value, *active_time_value;
static lv_obj_t *runtime_value, *draw_value, *estimate_note;
static lv_obj_t *load_value, *baseline_value, *baseline_note, *baseline_load_value, *delta_value, *save_row;
static batteryprofile_t sample;
static batteryprofile_estimate_t current, baseline;
static load_t load, baseline_load;

static load_t read_load(void) {
	wifi_status_t wifi;
	wifi_get_status(&wifi);
	load_t out = {
		.output = volume_profile_output(),
		.volume = get_volume_percent(),
		.brightness = power_get_brightness(),
		.wifi = wifi.state,
		.playing = audio_get_status() == AUDIO_STATUS_PLAYING || audio_external_is_active(),
		.high_gain = get_high_gain() != 0,
		.peq = peq_get_enabled(),
		.geq = eq_get_enabled(),
		.lineout = lineout_is_active(),
	};
	if (out.playing) {
		audio_get_stream_info(&out.sample_rate, NULL);
		out.dsd_multiple = audio_get_dsd_multiple();
	}
	return out;
}

static bool same_load(load_t a, load_t b) {
	return a.output == b.output && a.volume == b.volume && a.brightness == b.brightness && a.wifi == b.wifi &&
		   a.playing == b.playing && a.sample_rate == b.sample_rate && a.dsd_multiple == b.dsd_multiple &&
		   a.high_gain == b.high_gain && a.peq == b.peq && a.geq == b.geq && a.lineout == b.lineout;
}

static uint32_t boottime_ms(void) {
	struct timespec ts = {0};
#ifdef CLOCK_BOOTTIME
	if (clock_gettime(CLOCK_BOOTTIME, &ts) == 0) {
		return (uint32_t)ts.tv_sec * 1000u + (uint32_t)ts.tv_nsec / 1000000u;
	}
#endif
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint32_t)ts.tv_sec * 1000u + (uint32_t)ts.tv_nsec / 1000000u;
}

static void set_text(lv_obj_t *label, const char *text) {
	if (strcmp(lv_label_get_text(label), text) != 0) {
		lv_label_set_text(label, text);
	}
}

static void set_reading(lv_obj_t *label, int n, const char *unit) {
	char text[24];
	if (n < 0) {
		set_text(label, NO_VALUE);
	} else {
		snprintf(text, sizeof(text), "%d%s", n, unit);
		set_text(label, text);
	}
}

static void format_duration(char *text, size_t size, uint64_t seconds) {
	unsigned long long minutes = seconds / 60;
	unsigned long long hours = minutes / 60;
	if (hours >= 24) {
		snprintf(text, size, "%llud %02lluh", hours / 24, hours % 24);
	} else if (hours) {
		snprintf(text, size, "%lluh %02llum", hours, minutes % 60);
	} else {
		snprintf(text, size, "%llum", minutes);
	}
}

static void refresh_history(void) {
	// The power manager tracks charge history even when the profiler is closed.
	batteryhistory_saved_t history;
	if (!batteryhistory_snapshot(&history)) {
		set_text(last_charge_value, tr("system_battery_not_recorded"));
		set_text(charge_point_value, NO_VALUE);
		set_text(on_time_value, NO_VALUE);
		set_text(active_time_value, NO_VALUE);
		return;
	}

	char text[64];
	time_t when = (time_t)history.marked_at;
	struct tm local;
	bool dated = when > 0 && (int64_t)when == history.marked_at && localtime_r(&when, &local) &&
				 strftime(text, sizeof(text), "%Y-%m-%d %H:%M", &local) != 0;
	set_text(last_charge_value, dated ? text : tr("system_battery_time_not_set"));
	if (history.mark == BATTERYHISTORY_MARK_CHARGED_TO_TARGET) {
		snprintf(text, sizeof(text), tr("system_battery_charged_to"), history.charge_target);
		set_text(charge_point_value, text);
	} else {
		set_text(charge_point_value, tr("system_battery_unplugged"));
	}

	format_duration(text, sizeof(text), history.on_seconds);
	set_text(on_time_value, text);
	format_duration(text, sizeof(text), history.active_seconds);
	set_text(active_time_value, text);
}

static void set_load(lv_obj_t *label, load_t value) {
	// Stock's one switch does not identify both jacks. Say "Wired" unless
	// routing actually identifies 4.4 mm; never guess 3.5 mm from that switch.
	const char *output = tr("battery_profile_wired");
	if (value.output == VOLUME_OUTPUT_BALANCED) {
		output = "4.4 mm";
	} else if (value.output == VOLUME_OUTPUT_USB) {
		output = "USB";
	} else if (value.output == VOLUME_OUTPUT_BLUETOOTH) {
		output = tr("bluetooth");
	}
	char text[256];
	snprintf(text, sizeof(text), "%s%s%s · %s · %d%%\n%s: %s · %s: %s\n%s",
			 output, value.lineout ? " / " : "", value.lineout ? tr("quickpanel_line_out") : "",
			 tr(value.high_gain ? "musicsettings_high_gain" : "battery_profile_low_gain"), value.volume,
			 tr("peq"), tr(value.peq ? "on" : "off"), tr("wi_fi"), tr(value.wifi != WIFI_STATE_OFF ? "on" : "off"),
			 tr(value.playing ? "battery_profile_playing" : "battery_profile_idle"));
	set_text(label, text);
}

static const char *source_label(batteryprofile_source_t source) {
	return source == BATTERYPROFILE_TREND ? "battery_profile_trend" : "battery_profile_gauge";
}

static void refresh_baseline(void) {
	bool saved = baseline.source != BATTERYPROFILE_NONE;
	set_reading(baseline_value, saved ? baseline.draw_ma : -1, " mA");
	lv_obj_set_hidden(baseline_note, !saved);
	lv_obj_set_hidden(baseline_load_value, !saved);
	if (saved) {
		set_text(baseline_note, tr(source_label(baseline.source)));
		set_load(baseline_load_value, baseline_load);
	}
	int ma, percent;
	if (batteryprofile_delta(current, baseline, &ma, &percent)) {
		char text[32];
		snprintf(text, sizeof(text), "%+d mA (%+d%%)", ma, percent);
		set_text(delta_value, text);
	} else if (saved && current.source != BATTERYPROFILE_NONE) {
		set_text(delta_value, tr("battery_profile_different_source"));
	} else {
		set_text(delta_value, NO_VALUE);
	}
}

static void refresh(void) {
	bool lit = power_screen_is_on();
	const char *level = read_battery_percent();
	int cached = level && level[0] >= '0' && level[0] <= '9' ? atoi(level) : -1;
	bool external_power = usb_vbus_present() || read_battery_charging();
	batteryprofile_reading_t reading = batteryprofile_read(supply_dir, cached, lit);
	load_t observed = read_load();
	if (!same_load(load, observed)) {
		sample = (batteryprofile_t){0};
		load = observed;
	}
	if (supply_dir) {
		current = batteryprofile_step(&sample, boottime_ms(), reading, external_power);
	}
	if (!lit) {
		return; // no painting or extra PMIC transactions in the dark
	}

	set_reading(level_value, reading.percent, "%");
	set_reading(voltage_value, reading.pmic_mv, " mV");
	set_reading(charge_limit_value, axpcharge_current_setting_ma(), " mA");
	refresh_history();
	set_load(load_value, load);

	bool available = current.source != BATTERYPROFILE_NONE;
	set_reading(draw_value, available ? current.draw_ma : -1, " mA");
	if (available) {
		char text[24];
		int tenths = (current.minutes_left + 3) / 6;
		if (tenths == 0) {
			set_text(runtime_value, "<0.1 h");
		} else {
			snprintf(text, sizeof(text), "%d.%d h", tenths / 10, tenths % 10);
			set_text(runtime_value, text);
		}
	} else {
		set_text(runtime_value, NO_VALUE);
	}

	const char *note = "battery_profile_sampling";
	if (!supply_dir) {
		note = "battery_profile_simulator";
	} else if (external_power) {
		note = "battery_profile_unplug";
	} else if (reading.percent < 0) {
		note = "battery_profile_no_gauge";
	} else if (reading.percent <= BATTERYGUARD_OFF_PERCENT) {
		note = "battery_low_warning";
	} else if (available) {
		note = source_label(current.source);
	}
	set_text(estimate_note, tr(note));
	lv_obj_set_state(save_row, LV_STATE_DISABLED, !available);
	refresh_baseline();
}

static void timer_cb(lv_timer_t *timer) {
	(void)timer;
	if (lv_screen_active() == screen) {
		refresh();
	}
}

static void opened_cb(lv_event_t *e) {
	(void)e;
	sample = (batteryprofile_t){0};
	lv_timer_resume(poll);
	lv_timer_reset(poll);
	refresh();
}

static void closed_cb(lv_event_t *e) {
	(void)e;
	lv_timer_pause(poll);
}

static void resample_cb(lv_event_t *e) {
	(void)e;
	sample = (batteryprofile_t){0};
	refresh();
}

static void save_cb(lv_event_t *e) {
	(void)e;
	refresh(); // a cable or load change may have landed before the next tick
	if (current.source != BATTERYPROFILE_NONE) {
		baseline = current;
		baseline_load = load;
		refresh_baseline();
	}
}

static lv_obj_t *label(lv_obj_t *parent, const char *text, bool dim) {
	lv_obj_t *obj = lv_label_create(parent);
	lv_obj_add_style(obj, dim ? &theme_style_text_dim : &theme_style_text, 0);
	lv_obj_set_style_text_font(obj, dim ? &font_ui_14 : &font_ui_16, 0);
	lv_label_set_text(obj, tr(text));
	return obj;
}

static lv_obj_t *paragraph(lv_obj_t *parent, const char *text, bool dim) {
	lv_obj_t *obj = label(parent, text, dim);
	lv_obj_set_width(obj, lv_pct(100));
	return obj;
}

static lv_obj_t *card(lv_obj_t *parent) {
	lv_obj_t *obj = lv_obj_create(parent);
	lv_obj_add_style(obj, &theme_style_card, 0);
	lv_obj_set_size(obj, lv_pct(100), LV_SIZE_CONTENT);
	lv_obj_set_style_radius(obj, bp_tile_radius(), 0);
	lv_obj_set_style_border_width(obj, 0, 0);
	lv_obj_set_style_pad_all(obj, 8, 0);
	lv_obj_set_style_pad_gap(obj, 4, 0);
	lv_obj_set_flex_flow(obj, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_scrollable(obj, false);
	lv_obj_set_clickable(obj, false);
	return obj;
}

static lv_obj_t *metric(lv_obj_t *parent, const char *name) {
	lv_obj_t *row = lv_obj_create(parent);
	lv_obj_remove_style_all(row);
	lv_obj_set_size(row, lv_pct(100), LV_SIZE_CONTENT);
	lv_obj_set_style_pad_gap(row, 6, 0);
	lv_obj_set_scrollable(row, false);
	lv_obj_set_clickable(row, false);
	lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_flex_grow(label(row, name, true), 1);
	lv_obj_t *value = label(row, NO_VALUE, false);
	lv_obj_set_style_max_width(value, lv_pct(60), 0);
	lv_obj_set_style_text_align(value, LV_TEXT_ALIGN_RIGHT, 0);
	return value;
}

lv_obj_t *batteryprofilepage_init(gui_config_t *cfg) {
	screen = lv_obj_create(NULL);
	lv_obj_t *container = settingsrow_page(screen, cfg, "battery_profile_title");
	settingsrow_title_corner_slots(settingsrow_page_title(screen), cfg, 0);

	lv_obj_t *summary = card(container);
	runtime_value = metric(summary, "battery_profile_runtime");
	draw_value = metric(summary, "battery_profile_draw");
	estimate_note = paragraph(summary, "", true);
	delta_value = metric(summary, "battery_profile_delta");

	lv_obj_t *telemetry = card(container);
	level_value = metric(telemetry, "system_battery_level");
	voltage_value = metric(telemetry, "battery_profile_voltage");
	metric(telemetry, "battery_profile_charge_current"); // no current ADC on the V1
	charge_limit_value = metric(telemetry, "battery_profile_charge_limit");

	lv_obj_t *history = card(container);
	last_charge_value = metric(history, "system_battery_last_charge");
	charge_point_value = metric(history, "system_battery_charge_point");
	on_time_value = metric(history, "system_battery_on_time");
	active_time_value = metric(history, "system_battery_active_time");

	save_row = settingsrow_action(container, "battery_profile_save", save_cb, NULL);
	lv_obj_t *resample = settingsrow_action(container, "battery_profile_resample", resample_cb, NULL);
	lv_obj_set_state(resample, LV_STATE_DISABLED, !supply_dir);

	lv_obj_t *comparison = card(container);
	baseline_value = metric(comparison, "battery_profile_baseline");
	baseline_note = paragraph(comparison, "", true);
	baseline_load_value = paragraph(comparison, "", true);

	lv_obj_t *active = card(container);
	paragraph(active, "battery_profile_load", false);
	load_value = paragraph(active, "", true);

	lv_obj_t *details = card(container);
	paragraph(details, "battery_profile_help", true);
	paragraph(details, "battery_profile_limits_note", true);

	poll = lv_timer_create(timer_cb, POLL_MS, NULL);
	lv_timer_pause(poll);
	// Sampling can continue while the panel is dark (especially useful for a
	// percent trend). It doesn't prevent blanking or suspend, and stops off-page.
	power_slow_in_standby(poll, DARK_POLL_MS);
	lv_obj_add_event_cb(screen, opened_cb, LV_EVENT_SCREEN_LOADED, NULL);
	lv_obj_add_event_cb(screen, closed_cb, LV_EVENT_SCREEN_UNLOAD_START, NULL);
	switcher_attach_back_gesture(screen);
	return screen;
}
