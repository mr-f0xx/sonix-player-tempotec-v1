/* Issue #267: 100% must survive screen-off/wake without driving full-duty PWM.
 * Real: power/charging-screen brightness selection, fades and wake sequences,
 * and LVGL. Fake: sysfs, time, audio/radios and scheduling the power tick. No
 * device nodes are opened, no service is started, and no real sleeps occur.
 */
#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "lvgl/lvgl.h"
#include "src/system/device/backlight.h"

lv_font_t font_ui_14;

#define TEST_BRIGHTNESS "/test/backlight/brightness"
#define TEST_MAX "/test/backlight/max_brightness"
#define TEST_BLANK "/test/fb/blank"

enum { BRIGHTNESS, MAX_BRIGHTNESS, BLANK, NODE_COUNT };
static struct {
	long value;
	bool available;
} nodes[NODE_COUNT];
static struct {
	int node;
	long value;
} writes[4096];
static size_t write_count;

/* A sysfs-like stream: reads return the attribute's value, writes commit on
 * close. Record every write, not just the last one, to catch transient max
 * writes in init, fades, blank/unblank and the guarded wake window.
 */
typedef struct {
	int node;
	bool writing;
	char text[64];
	size_t length, offset;
} node_file_t;

static ssize_t node_read(void *cookie, char *buf, size_t size) {
	node_file_t *file = cookie;
	size_t left = file->length - file->offset;
	if (size > left) size = left;
	memcpy(buf, file->text + file->offset, size);
	file->offset += size;
	return (ssize_t)size;
}

static ssize_t node_write(void *cookie, const char *buf, size_t size) {
	node_file_t *file = cookie;
	assert(file->length + size < sizeof(file->text));
	memcpy(file->text + file->length, buf, size);
	file->length += size;
	file->text[file->length] = '\0';
	return (ssize_t)size;
}

static int node_close(void *cookie) {
	node_file_t *file = cookie;
	if (file->writing) {
		char *end;
		long value = strtol(file->text, &end, 10);
		assert(end != file->text && *end == '\0');
		assert(write_count < sizeof(writes) / sizeof(writes[0]));
		writes[write_count].node = file->node;
		writes[write_count++].value = value;
		nodes[file->node].value = value;
	}
	free(file);
	return 0;
}

static FILE *display_fopen(const char *path, const char *mode) {
	int node = -1;
	if (!strcmp(path, TEST_BRIGHTNESS) || !strcmp(path, "/sys/class/backlight/backlight_pwm0/brightness"))
		node = BRIGHTNESS;
	if (!strcmp(path, TEST_MAX) || !strcmp(path, "/sys/class/backlight/backlight_pwm0/max_brightness"))
		node = MAX_BRIGHTNESS;
	if (!strcmp(path, TEST_BLANK) || !strcmp(path, "/sys/class/graphics/fb0/blank"))
		node = BLANK;
	if (node < 0 || !nodes[node].available) {
		errno = ENOENT;
		return NULL; // Never fall through to the host's real sysfs.
	}
	node_file_t *file = calloc(1, sizeof(*file));
	assert(file);
	file->node = node;
	file->writing = mode[0] == 'w';
	if (!file->writing)
		file->length = (size_t)snprintf(file->text, sizeof(file->text), "%ld\n", nodes[node].value);
	cookie_io_functions_t io = {.read = node_read, .write = node_write, .close = node_close};
	FILE *stream = fopencookie(file, mode, io);
	assert(stream);
	return stream;
}

static uint32_t test_ms = 1000;
static int display_clock_gettime(clockid_t clock, struct timespec *ts) {
	(void)clock;
	ts->tv_sec = test_ms / 1000;
	ts->tv_nsec = (test_ms % 1000) * 1000000L;
	return 0;
}
static int display_usleep(useconds_t us) {
	test_ms += (us + 999) / 1000;
	assert(test_ms < 60000); // Also catch a fade stuck at an unreachable max.
	return 0;
}
static uint32_t display_tick(void) { return test_ms; }

/* Only explicit transitions run in this fixture. Discard the power timer's
 * callback without retaining the unrelated shutdown/suspend/network graph.
 * sizeof still references it, so the production file compiles warning-clean.
 */
#define lv_timer_create(cb, period, data) ((void)sizeof(&(cb)), lv_timer_create(NULL, period, data))
#define fopen display_fopen
#define clock_gettime display_clock_gettime
#define usleep display_usleep
#include "src/system/device/power.c"
#undef lv_timer_create
#include "src/gui/shell/chargescreen.c"
#undef fopen
#undef clock_gettime
#undef usleep

static long saved_brightness = -1;
static long wake_hold;
static bool bt_tx, bt_rx, credible_vsync;
static bool waveform_lit, led_dark;
static bool saver_visible;

bool screensaver_is_visible(void) { return saver_visible; }
static unsigned wake_kicks;

long config_get_int(const char *section, const char *key, long fallback) {
	if (!strcmp(section, "screen")) {
		if (!strcmp(key, "brightness")) return saved_brightness;
		if (!strcmp(key, "wake_hold_level")) return wake_hold;
	}
	return fallback;
}

audio_status_t audio_get_status(void) { return bt_tx ? AUDIO_STATUS_PLAYING : AUDIO_STATUS_STOPPED; }
bool audio_output_is_bluetooth(void) { return bt_tx; }
bool btreceiver_is_streaming(void) { return bt_rx; }
bool usbaudio_active(void) { return false; }
int get_volume_percent(void) { return 50; }
void set_volume_percent(int value) { assert(value == 50); }
void wifi_set_enabled_transient(bool on) { (void)on; }
void bluetooth_set_enabled_transient(bool on) { (void)on; }
void waveform_set_screen_on(bool on) { waveform_lit = on; }
void led_set_standby(bool dark) { led_dark = dark; }
void clock_remember(void) {}
bool clock_resync(void) { return false; }
void panel_touch_flush(void) {}
void topbar_refresh_audio_outputs(void) {}
const sysinfo_model_t *sysinfo_model(void) {
	static const sysinfo_model_t model = {.tap_wake = false};
	return &model;
}
void display_wake_begin(lv_display_t *disp) {
	(void)disp;
	assert(!power_panel_is_blanked());
	if (g_cfg.brightness_path) assert(nodes[BRIGHTNESS].value <= wake_hold);
	wake_kicks++;
}
void display_wake_end(lv_display_t *disp) { (void)disp; }
bool display_wait_vsync(int frames) {
	assert(nodes[BRIGHTNESS].value <= wake_hold);
	if (credible_vsync) test_ms += (uint32_t)frames * 17;
	return credible_vsync;
}

static void flush(lv_display_t *disp, const lv_area_t *area, uint8_t *pixels) {
	(void)area;
	(void)pixels;
	lv_display_flush_ready(disp);
}

static void start_power(long saved, long current, long max, long hold, bool blank_control) {
	if (g_power_timer) lv_timer_delete(g_power_timer);
	bt_tx = bt_rx = false;
	wake_kicks = 0;
	write_count = 0;
	wake_hold = hold;
	saved_brightness = saved;
	nodes[BRIGHTNESS].value = current;
	nodes[MAX_BRIGHTNESS].value = max;
	nodes[BLANK].value = 0;
	for (int i = 0; i < NODE_COUNT; i++) nodes[i].available = true;
	if (max < 0) nodes[MAX_BRIGHTNESS].available = false;
	g_panel_held = g_bt_was_sounding = false;
	g_bt_quiet_at = 0;
	power_config_t cfg = {
		.brightness_path = TEST_BRIGHTNESS,
		.max_brightness_path = TEST_MAX,
		.blank_path = blank_control ? TEST_BLANK : NULL,
		.brightness = saved,
	};
	power_init(&cfg, lv_display_get_default());
}

static void assert_safe_writes(long max) {
	if (max <= 0) max = BACKLIGHT_MAX_DEFAULT;
	for (size_t i = 0; i < write_count; i++) {
		if (writes[i].node != BRIGHTNESS) continue;
		assert(writes[i].value >= 0);
		assert(max > 1 ? writes[i].value < max : writes[i].value <= max);
	}
}

static void assert_dark(bool blanked) {
	assert(!power_screen_is_on());
	assert(power_panel_is_blanked() == blanked);
	assert(nodes[BRIGHTNESS].value == 0 && g_hw_brightness == 0);
	assert(!waveform_lit && led_dark);
}

static void test_levels_and_init(void) {
	assert(backlight_safe_level(LONG_MIN, 100) == 0);
	assert(backlight_safe_level(0, 100) == 0);
	assert(backlight_safe_level(1, 100) == 1);
	assert(backlight_safe_level(60, 100) == 60);
	assert(backlight_safe_level(99, 100) == 99);
	assert(backlight_safe_level(100, 100) == 99);
	assert(backlight_safe_level(LONG_MAX, 100) == 99);
	assert(backlight_safe_level(100, 0) == 99);
	assert(backlight_safe_level(100, -1) == 99);
	assert(backlight_safe_level(255, 255) == 254);
	assert(backlight_safe_level(1, 1) == 1);
	assert(backlight_safe_level(2, 2) == 1);

	const struct { long saved, current, max, requested, hardware; } cases[] = {
		{100, 60, 100, 100, 99}, // An existing saved 100% setting.
		{-1, 100, 100, 100, 99}, // Adopt full brightness from the stock player.
		{-1, 0, 100, 100, 99},   // A previous dark session: fall back to max.
		{999, 60, 100, 100, 99}, // Oversized config is normalized in memory.
		{60, 100, 100, 60, 60},
		{1, 60, 100, 5, 5},     // Preserve the UI's existing startup floor.
		{100, 60, -1, 100, 99},  // Missing max_brightness attribute.
		{100, 60, 0, 100, 99},   // Invalid max_brightness attribute.
		{255, 60, 255, 255, 254},
		{1, 0, 1, 1, 1},
	};
	for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		start_power(cases[i].saved, cases[i].current, cases[i].max, 0, true);
		assert(power_get_brightness() == cases[i].requested);
		assert(nodes[BRIGHTNESS].value == cases[i].hardware);
		assert(saved_brightness == cases[i].saved); // Do not rewrite user settings.
		assert_safe_writes(cases[i].max);
	}

	start_power(100, 60, 100, 0, true);
	assert(power_get_max_brightness() == 100 && power_get_min_brightness() == 5);
	power_set_brightness(60);
	assert(power_get_brightness() == 60 && g_hw_brightness == 60);
	fade_to(100);
	assert(g_hw_brightness == 99);
	power_set_brightness(LONG_MAX);
	assert(power_get_brightness() == 100 && g_hw_brightness == 99);
	power_set_brightness(LONG_MIN);
	assert(power_get_brightness() == 1 && g_hw_brightness == 1);
	assert_safe_writes(100);
}

static void test_wake_cycles(void) {
	for (long hold = 0; hold <= 1; hold++) {
		for (int vsync = 0; vsync <= 1; vsync++) {
			credible_vsync = vsync;
			start_power(100, 60, 100, hold, true);
			for (int cycle = 0; cycle < 5; cycle++) {
				power_toggle_screen();
				assert_dark(true);
				assert(nodes[BLANK].value == FB_BLANK_POWERDOWN);
				size_t off_writes = write_count;
				power_set_brightness(100); // A dark-screen change must not light it.
				assert(write_count == off_writes);
				power_toggle_screen();
				assert(power_screen_is_on() && !power_panel_is_blanked());
				assert(nodes[BLANK].value == FB_BLANK_UNBLANK);
				assert(power_get_brightness() == 100 && g_hw_brightness == 99);
				assert(nodes[BRIGHTNESS].value == 99 && waveform_lit && !led_dark);
			}
			assert(wake_kicks == 5);
			assert_safe_writes(100);
		}
	}

	// Bluetooth TX and RX use the held-panel wake, not the blank/unblank path.
	for (int rx = 0; rx <= 1; rx++) {
		start_power(100, 60, 100, 1, true);
		bt_tx = !rx;
		bt_rx = rx;
		power_screen_off();
		assert_dark(false);
		assert(g_panel_held);
		power_screen_on();
		assert(g_hw_brightness == 99 && wake_kicks == 0);
		assert_safe_writes(100);
	}

	// A stream starting and then stopping behind a dark screen must leave the
	// backlight at zero on both panel-power transitions.
	start_power(100, 60, 100, 1, true);
	power_screen_off();
	bt_rx = true;
	panel_hold_tick(test_ms);
	assert_dark(false);
	bt_rx = false;
	panel_hold_tick(test_ms);
	test_ms += PANEL_HOLD_QUIET_MS;
	panel_hold_tick(test_ms);
	assert_dark(true);
	power_screen_on();
	assert(g_hw_brightness == 99);
	assert_safe_writes(100);

	start_power(100, 60, 100, 0, false);
	power_screen_off();
	assert_dark(false);
	power_screen_on();
	assert(g_hw_brightness == 99);
	assert_safe_writes(100);

	// The simulator has no sysfs paths and must not acquire any by accident.
	power_config_t host = {.brightness = 100};
	lv_timer_delete(g_power_timer);
	write_count = 0;
	power_init(&host, lv_display_get_default());
	power_screen_off();
	power_screen_on();
	assert(power_get_brightness() == 100 && write_count == 0);
}

static void test_screensaver_idle_timeout(void) {
	start_power(60, 60, 100, 0, true);
	saver_visible = true;

	// Simulate the pointer staying asserted against the lock screen: LVGL sees
	// fresh activity on every poll, but no new user gesture should be needed
	// for the configured screen timeout to expire.
	test_ms += 30000;
	lv_display_trigger_activity(g_disp);
	assert(lv_display_get_inactive_time(g_disp) == 0);
	assert(input_idle_ms(test_ms) == test_ms - g_last_button_activity_ms);
	assert(input_idle_ms(test_ms) >= 30000);

	// Starting the screensaver's swipe is meaningful activity and restarts the
	// independent idle clock, even though LVGL's timestamp is not consulted.
	power_notify_activity();
	assert(input_idle_ms(test_ms) == 0);

	// Ordinary pages continue to respect LVGL touch activity as before.
	saver_visible = false;
	lv_display_trigger_activity(g_disp);
	assert(input_idle_ms(test_ms) == 0);
}

static void test_charging_screen(void) {
	start_power(100, 60, 100, 0, true);
	assert(screen_brightness() == 99);
	saved_brightness = 999;
	assert(screen_brightness() == 99);
	saved_brightness = 60;
	assert(screen_brightness() == 60);
	saved_brightness = -1;
	nodes[BRIGHTNESS].value = 100;
	assert(screen_brightness() == 99);
	nodes[BRIGHTNESS].value = 0;
	assert(screen_brightness() == 99);
	nodes[MAX_BRIGHTNESS].available = false;
	assert(screen_brightness() == 99);
	nodes[MAX_BRIGHTNESS].available = true;
	nodes[MAX_BRIGHTNESS].value = 255;
	saved_brightness = 255;
	assert(screen_brightness() == 254);

	// Exercise the separate charger-only off/wake writer as well as selection.
	nodes[MAX_BRIGHTNESS].value = 100;
	saved_brightness = 100;
	display = lv_display_get_default();
	on_level = screen_brightness();
	screen_lit = true;
	panel_off();
	assert(nodes[BRIGHTNESS].value == 0 && nodes[BLANK].value == FB_BLANK_POWERDOWN);
	panel_on();
	assert(screen_lit && nodes[BRIGHTNESS].value == 99);
	assert_safe_writes(100);
}

int main(void) {
	font_ui_14 = lv_font_montserrat_14;
	lv_init();
	lv_tick_set_cb(display_tick);
	lv_display_t *disp = lv_display_create(240, 320);
	assert(disp);
	static uint16_t pixels[240 * 20];
	lv_display_set_color_format(disp, LV_COLOR_FORMAT_RGB565);
	lv_display_set_buffers(disp, pixels, NULL, sizeof(pixels), LV_DISPLAY_RENDER_MODE_PARTIAL);
	lv_display_set_flush_cb(disp, flush);
	test_levels_and_init();
	test_wake_cycles();
	test_screensaver_idle_timeout();
	test_charging_screen();
	lv_timer_delete(g_power_timer);
	lv_display_delete(disp);
	lv_deinit();
	puts("power-display: all tests passed");
	return 0;
}
