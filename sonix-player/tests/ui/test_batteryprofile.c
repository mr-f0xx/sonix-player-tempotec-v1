// The production profiler on the V1's LVGL panel. Feed it the stock driver's
// units without touching host sysfs, and run both the sampler and the UI.
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "src/gui/fonts/fonts.h"
#include "src/gui/settings/batteryprofilepage.h"
#include "src/gui/shell/settingsrow.h"
#include "src/gui/shell/theme.h"
#include "src/system/audio/alsa-controls.h"
#include "src/system/audio/audio.h"
#include "src/system/core/lang.h"
#include "src/system/device/batteryhistory.h"
#include "src/system/device/batteryprofile.h"
#include "src/system/net/wifi.h"

lv_font_t font_ui_14, font_ui_16, font_ui_18, font_ui_20, font_ui_22, font_ui_24, font_ui_26, font_ui_28, font_ui_32;
lv_font_t font_ui_20_bold, font_ui_22_bold, font_ui_24_bold;
void fonts_register_change(void (*cb)(void)) { (void)cb; }
int fonts_text_size(void) { return FONTS_TEXT_NORMAL; }
void bootlogo_set(int choice, bool dark) { (void)choice; (void)dark; }
bool bootpic_present(void) { return false; }
bool player_sheet_drag_active(void) { return false; }
bool switcher_back_drag_active(void) { return false; }
void switcher_attach_back_gesture(lv_obj_t *obj) { (void)obj; }
void switcher_mark_fresh_start(lv_obj_t *obj) { (void)obj; }

static int seconds, percent = 50, rrt_minutes = 300, pmic_mv = 3950, charge_limit = 500;
static unsigned samples, voltage_reads, limit_reads, history_reads;
static batteryhistory_saved_t history;
static bool cable, screen_on = true, heavy;
static lv_timer_t *sample_timer;

int __wrap_clock_gettime(clockid_t clock, struct timespec *time) {
	(void)clock;
	*time = (struct timespec){.tv_sec = seconds};
	return 0;
}

batteryprofile_reading_t __wrap_batteryprofile_read(const char *root, int cached, bool voltage) {
	assert(root && strcmp(root, "/sys/class/power_supply") == 0 && cached == percent);
	samples++;
	voltage_reads += voltage;
	return (batteryprofile_reading_t){
		.percent = cached, .cw2015_percent = true, .gauge_minutes = rrt_minutes,
		.pmic_mv = voltage ? pmic_mv : -1,
	};
}

bool batteryhistory_snapshot(batteryhistory_saved_t *out) {
	history_reads++;
	*out = history;
	return history.mark != BATTERYHISTORY_MARK_NONE;
}

int axpcharge_current_setting_ma(void) { limit_reads++; return charge_limit; }
char *read_battery_percent(void) {
	static char level[12];
	snprintf(level, sizeof(level), "%d", percent);
	return level;
}
bool read_battery_charging(void) { return false; } // the charge cap can be holding, even on USB
bool usb_vbus_present(void) { return cable; }
bool power_screen_is_on(void) { return screen_on; }
long power_get_brightness(void) { return 100; }
void power_slow_in_standby(lv_timer_t *timer, uint32_t slow_ms) {
	assert(!sample_timer && slow_ms == 5000);
	sample_timer = timer;
}
volume_output_t volume_profile_output(void) { return heavy ? VOLUME_OUTPUT_BALANCED : VOLUME_OUTPUT_PHONES; }
int get_volume_percent(void) { return 35; }
int get_high_gain(void) { return heavy; }
bool peq_get_enabled(void) { return heavy; }
bool eq_get_enabled(void) { return false; }
bool lineout_is_active(void) { return false; }
audio_status_t audio_get_status(void) { return AUDIO_STATUS_PLAYING; }
bool audio_external_is_active(void) { return false; }
int audio_get_dsd_multiple(void) { return 0; }
void audio_get_stream_info(int *rate, int *channels) { if (rate) *rate = 44100; if (channels) *channels = 2; }
void wifi_get_status(wifi_status_t *out) { *out = (wifi_status_t){.state = heavy ? WIFI_STATE_CONNECTED : WIFI_STATE_OFF}; }

static lv_obj_t *find_label(lv_obj_t *obj, const char *text) {
	if (lv_obj_check_type(obj, &lv_label_class) && strcmp(lv_label_get_text(obj), text) == 0) {
		return obj;
	}
	for (uint32_t i = 0; i < lv_obj_get_child_count(obj); i++) {
		lv_obj_t *found = find_label(lv_obj_get_child(obj, (int32_t)i), text);
		if (found) {
			return found;
		}
	}
	return NULL;
}

static lv_obj_t *named(lv_obj_t *page, const char *tag) {
	lv_obj_t *found = find_label(page, tr(tag));
	assert(found);
	return found;
}

static void check_metric(lv_obj_t *page, const char *tag, const char *value) {
	lv_obj_t *name = named(page, tag);
	lv_obj_t *row = lv_obj_get_parent(name);
	lv_obj_t *shown = find_label(row, value);
	assert(shown);
	lv_obj_update_layout(page);
	lv_area_t a, b, area;
	lv_obj_get_coords(name, &a);
	lv_obj_get_coords(shown, &b);
	lv_obj_get_coords(row, &area);
	assert(a.x1 >= area.x1 && a.x2 < b.x1 && b.x2 <= area.x2);
	assert(a.y1 >= area.y1 && b.y1 >= area.y1 && a.y2 <= area.y2 && b.y2 <= area.y2);
}

static void advance(int elapsed) {
	for (int i = 0; i < elapsed; i++) {
		seconds++;
		lv_tick_inc(1000);
		lv_timer_handler();
	}
}

static void flush_cb(lv_display_t *display, const lv_area_t *area, uint8_t *pixels) {
	(void)area;
	(void)pixels;
	lv_display_flush_ready(display);
}

int main(int argc, char **argv) {
	assert(argc == 2);
	assert(setenv("TZ", "UTC0", 1) == 0);
	tzset();
	lv_init();
	font_ui_14 = font_ui_18 = lv_font_montserrat_14;
	font_ui_16 = lv_font_montserrat_16;
	font_ui_20 = font_ui_22 = font_ui_24 = lv_font_montserrat_20;
	font_ui_20_bold = font_ui_22_bold = font_ui_24_bold = font_ui_20;
	font_ui_26 = font_ui_28 = font_ui_32 = lv_font_montserrat_28;
	lv_display_t *display = lv_display_create(240, 320);
	static uint8_t framebuffer[240 * 320 * 4];
	lv_display_set_buffers(display, framebuffer, NULL, sizeof(framebuffer), LV_DISPLAY_RENDER_MODE_DIRECT);
	lv_display_set_flush_cb(display, flush_cb);
	lang_init(argv[1]);
	assert(strcmp(lang_current(), "English") == 0);
	theme_init();
	gui_config_t cfg = {.screen_width = 240, .screen_height = 320, .padding = 6, .top_bar_height = 24};
	lv_obj_t *other = lv_screen_active();
	lv_obj_t *page = batteryprofilepage_init(&cfg);
	advance(4);
	assert(samples == 0 && history_reads == 0); // no probing on other pages
	lv_screen_load(page);
	assert(samples == 1);
	lv_obj_t *save = lv_obj_get_parent(named(page, "battery_profile_save"));
	assert(lv_obj_has_state(save, LV_STATE_DISABLED));
	check_metric(page, "battery_profile_runtime", "—");
	check_metric(page, "battery_profile_voltage", "3950 mV");
	check_metric(page, "battery_profile_charge_current", "—");
	check_metric(page, "battery_profile_charge_limit", "500 mA");
	check_metric(page, "system_battery_level", "50%");
	check_metric(page, "system_battery_last_charge", tr("system_battery_not_recorded"));
	check_metric(page, "system_battery_charge_point", "—");
	check_metric(page, "system_battery_on_time", "—");

	history = (batteryhistory_saved_t){
		.mark = BATTERYHISTORY_MARK_CHARGED_TO_TARGET,
		.charge_target = 80,
		.marked_at = 1704067200,
		.on_seconds = 62 * 60,
	};
	advance(10);
	check_metric(page, "system_battery_last_charge", "2024-01-01 00:00");
	check_metric(page, "system_battery_charge_point", "Charged to 80%");
	check_metric(page, "system_battery_on_time", "1h 02m");
	assert(!lv_obj_has_state(save, LV_STATE_DISABLED));
	check_metric(page, "battery_profile_runtime", "4.9 h");
	check_metric(page, "battery_profile_draw", "140 mA");
	lv_obj_send_event(save, LV_EVENT_CLICKED, NULL);
	check_metric(page, "battery_profile_baseline", "140 mA");
	check_metric(page, "battery_profile_delta", "+0 mA (+0%)");

	heavy = true;
	rrt_minutes = 210;
	advance(2);
	assert(lv_obj_has_state(save, LV_STATE_DISABLED));
	check_metric(page, "battery_profile_draw", "—");
	check_metric(page, "battery_profile_delta", "—");
	advance(10);
	check_metric(page, "battery_profile_draw", "200 mA");
	check_metric(page, "battery_profile_delta", "+60 mA (+43%)");
	assert(find_label(page, "4.4 mm · High gain · 35%\nPEQ: On · Wi-Fi: On\nPlaying"));

	// Screen-off tracking uses the same sample but skips painted/PMIC fields.
	unsigned before = samples, raw = voltage_reads, limits = limit_reads, histories = history_reads;
	screen_on = false;
	history.on_seconds += 60;
	lv_timer_set_period(sample_timer, 5000);
	advance(15);
	assert(samples > before && voltage_reads == raw && limit_reads == limits && history_reads == histories);
	screen_on = true;
	lv_timer_set_period(sample_timer, 2000);
	lv_timer_ready(sample_timer);
	advance(1);
	check_metric(page, "battery_profile_delta", "+60 mA (+43%)");
	check_metric(page, "system_battery_on_time", "1h 03m");

	cable = true;
	advance(2);
	named(page, "battery_profile_unplug");
	check_metric(page, "battery_profile_runtime", "—");
	check_metric(page, "battery_profile_delta", "—");
	assert(lv_obj_has_state(save, LV_STATE_DISABLED));
	cable = false;
	advance(12);
	lv_obj_t *resample = lv_obj_get_parent(named(page, "battery_profile_resample"));
	lv_obj_scroll_to_view(resample, LV_ANIM_OFF);
	lv_obj_update_layout(page);
	lv_area_t a;
	lv_obj_get_coords(resample, &a);
	assert(a.y1 >= settingsrow_content_top(&cfg) && a.y2 < 320 && a.x1 >= 0 && a.x2 < 240);
	lv_obj_send_event(resample, LV_EVENT_CLICKED, NULL);
	assert(lv_obj_has_state(save, LV_STATE_DISABLED));
	check_metric(page, "battery_profile_draw", "—");
	check_metric(page, "battery_profile_baseline", "140 mA");

	lv_screen_load(other);
	before = samples;
	histories = history_reads;
	history = (batteryhistory_saved_t){.mark = BATTERYHISTORY_MARK_UNPLUGGED, .on_seconds = 25 * 3600};
	advance(8);
	assert(samples == before && history_reads == histories);
	lv_screen_load(page);
	check_metric(page, "system_battery_last_charge", tr("system_battery_time_not_set"));
	check_metric(page, "system_battery_charge_point", tr("system_battery_unplugged"));
	check_metric(page, "system_battery_on_time", "1d 01h");
	advance(10);
	check_metric(page, "battery_profile_delta", "+60 mA (+43%)");
	pmic_mv = charge_limit = rrt_minutes = -1;
	advance(2);
	check_metric(page, "battery_profile_voltage", "—");
	check_metric(page, "battery_profile_charge_limit", "—");
	check_metric(page, "battery_profile_draw", "—");
	percent = -1;
	advance(2);
	named(page, "battery_profile_no_gauge");

	// Allow a new run to accumulate a real SOC slope. The existing gauge
	// baseline cannot be compared with this estimate, even with valid RRT.
	percent = 50;
	rrt_minutes = 210;
	advance(12);
	percent = 49;
	advance(2);
	advance(298);
	percent = 48;
	advance(2);
	advance(298);
	percent = 47;
	advance(2);
	named(page, "battery_profile_trend");
	check_metric(page, "battery_profile_draw", "168 mA");
	check_metric(page, "battery_profile_delta", tr("battery_profile_different_source"));
	lv_obj_send_event(save, LV_EVENT_CLICKED, NULL);
	check_metric(page, "battery_profile_baseline", "168 mA");
	check_metric(page, "battery_profile_delta", "+0 mA (+0%)");

	// Rendering uses test fonts, but the labels, wrapping, translations and
	// layout are the production ones. Test wider metrics as well as English.
	const char *languages[16];
	int count = lang_list(languages, 16);
	assert(count == 11);
	font_ui_14 = font_ui_16 = lv_font_montserrat_20;
	lv_obj_report_style_change(NULL);
	for (int i = 0; i < count; i++) {
		assert(lang_set(languages[i]));
		advance(2);
		check_metric(page, "battery_profile_runtime", "3.8 h");
		check_metric(page, "battery_profile_draw", "168 mA");
		check_metric(page, "battery_profile_delta", "+0 mA (+0%)");
		check_metric(page, "battery_profile_voltage", "—");
		check_metric(page, "battery_profile_charge_limit", "—");
		check_metric(page, "system_battery_last_charge", tr("system_battery_time_not_set"));
		check_metric(page, "system_battery_charge_point", tr("system_battery_unplugged"));
		check_metric(page, "system_battery_on_time", "1d 01h");
		named(page, "battery_profile_save");
		named(page, "battery_profile_resample");
	}
	puts("PASS battery profiler UI (240x320, 11 languages, charge history, baseline, load changes, charger, resample and navigation)");
	return 0;
}
