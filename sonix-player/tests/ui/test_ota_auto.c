// The background update notice: check on Wi-Fi connect and every few hours,
// LED blink + topbar dot + a modal popup with an "Update now" action, and a
// transient toast for the "Later" deferral. OTA, Wi-Fi, the LED, the dot, the
// page and the config are stubbed; the real confirm card and the real timers
// are used.
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lvgl/lvgl.h"

#include "src/gui/settings/ota_auto.h"
#include "src/gui/shell/confirm.h"
#include "src/gui/shell/gui.h"
#include "src/gui/shell/icons.h"
#include "src/gui/shell/theme.h"
#include "src/system/device/ota.h"
#include "src/system/device/sysinfo.h"
#include "src/system/device/system.h"
#include "src/system/net/wifi.h"

// -- stubs -------------------------------------------------------------------

lv_font_t font_ui_14, font_ui_20, font_ui_22, font_ui_24, font_ui_26;
static theme_palette_t palette;
lv_style_t theme_style_card, theme_style_text, theme_style_text_dim, theme_style_switch;
const theme_palette_t *theme(void) { return &palette; }
lv_color_t theme_on_accent(void) { return lv_color_black(); }
void theme_register_refresh(theme_refresh_cb_t cb) { (void)cb; }
int bp_pick(int compact, int regular) { (void)regular; return compact; }

const char *tr(const char *key) {
	static const struct { const char *key, *text; } strings[] = {
		{"system_update_firmware", "Update firmware"},
		{"system_update", "Update"},
		{"system_later", "Later"},
		{"system_update_version_size", "Version %s · %s"},
		{"system_update_available", "Update available"},
		{"system_battery_warning", "Battery too low"},
		{NULL, NULL},
	};
	for (size_t i = 0; strings[i].key; i++) {
		if (strcmp(key, strings[i].key) == 0) return strings[i].text;
	}
	return key;
}

// Config: a tiny in-memory store.
struct cfg_slot { char cat[16], key[32], value[64]; };
static struct cfg_slot slots[16];
static size_t slot_count;
static int slot_find(const char *cat, const char *key) {
	for (size_t i = 0; i < slot_count; i++) {
		if (strcmp(slots[i].cat, cat) == 0 && strcmp(slots[i].key, key) == 0) return (int)i;
	}
	return -1;
}
void config_set(const char *cat, const char *key, const char *value) {
	int i = slot_find(cat, key);
	if (i < 0) {
		assert(slot_count < sizeof(slots) / sizeof(slots[0]));
		i = (int)slot_count++;
		snprintf(slots[i].cat, sizeof(slots[i].cat), "%s", cat);
		snprintf(slots[i].key, sizeof(slots[i].key), "%s", key);
	}
	snprintf(slots[i].value, sizeof(slots[i].value), "%s", value);
}
void config_set_bool(const char *cat, const char *key, bool value) {
	config_set(cat, key, value ? "true" : "false");
}
const char *config_get(const char *cat, const char *key, const char *def) {
	int i = slot_find(cat, key);
	return i >= 0 ? slots[i].value : def;
}
bool config_get_bool(const char *cat, const char *key, bool def) {
	const char *v = config_get(cat, key, NULL);
	if (!v) return def;
	return strcmp(v, "true") == 0;
}
void config_save(void) {}

// OTA: a finished release, a counter for starts, nothing else.
static ota_release_t release;
static ota_state_t state = OTA_IDLE;
static ota_result_t result;
static int check_starts;
const ota_release_t *ota_release(void) { return &release; }
bool ota_check_start(void) {
	check_starts++;
	state = OTA_FINISHED;
	result = OTA_OK;
	return true;
}
bool ota_download_start(void) { return false; }
void ota_cancel(void) { state = OTA_IDLE; }
ota_state_t ota_state(void) { return state; }
ota_result_t ota_result(void) { return result; }
void ota_acknowledge(void) { state = OTA_IDLE; }
void ota_progress(long *done, long *total) { *done = *total = 0; }
bool ota_busy(void) { return state == OTA_CHECKING || state == OTA_DOWNLOADING; }
const char *ota_installed_version(void) { return "1.6.2"; }

// Wi-Fi: connected, with an address.
static bool wifi_connected = true;
void wifi_get_status(wifi_status_t *out) {
	memset(out, 0, sizeof(*out));
	if (wifi_connected) {
		out->state = WIFI_STATE_CONNECTED;
		snprintf(out->ip, sizeof(out->ip), "192.168.1.10");
	} else {
		out->state = WIFI_STATE_OFF;
	}
}

// LED, dot, page, toast, everything outside the notice: no side effects.
static int notice_blinks;
void led_notify_update_ready(void) { notice_blinks++; }
static int fwupdate_download_starts;
void fwupdate_start_download(void) { fwupdate_download_starts++; }
void fwupdate_show(void) {}
static bool dot_visible;
void topbar_set_update_available(bool on) { dot_visible = on; }
static void (*dot_tap)(void);
void topbar_set_update_tap_cb(void (*cb)(void)) { dot_tap = cb; }
static int toasts;
void toast_glyph(const lv_image_dsc_t *glyph, const char *text) { (void)glyph; (void)text; toasts++; }
lv_image_dsc_t icon_download = {0};
void gui_notify_popup(const char *key) { (void)key; }
void sysinfo_format_size(uint64_t bytes, char *out, size_t size) {
	snprintf(out, size, "%llu MB", (unsigned long long)(bytes / 1000000));
}

// Battery: 100% by default.
static char battery_percent_text[16];
static bool battery_charging;
char *read_battery_percent(void) { return battery_percent_text; }
bool read_battery_charging(void) { return battery_charging; }
static void battery_set(int percent, bool charging) {
	snprintf(battery_percent_text, sizeof(battery_percent_text), "%d", percent);
	battery_charging = charging;
}

static lv_obj_t *find_label(lv_obj_t *obj, const char *text) {
	if (lv_obj_check_type(obj, &lv_label_class) && strcmp(lv_label_get_text(obj), text) == 0) return obj;
	for (uint32_t i = 0; i < lv_obj_get_child_count(obj); i++) {
		lv_obj_t *found = find_label(lv_obj_get_child(obj, i), text);
		if (found) return found;
	}
	return NULL;
}

static void flush_cb(lv_display_t *display, const lv_area_t *area, uint8_t *pixels) {
	(void)area;
	(void)pixels;
	lv_display_flush_ready(display);
}

static void press_button_with_label(const char *label) {
	lv_obj_t *btn_label = find_label(lv_layer_top(), label);
	assert(btn_label);
	lv_obj_send_event(lv_obj_get_parent(btn_label), LV_EVENT_CLICKED, NULL);
	lv_tick_inc(300);
	lv_timer_handler();
}

static lv_obj_t *confirm_veil_obj(void) {
	lv_obj_t *layer = lv_layer_top();
	if (!layer) return NULL;
	return lv_obj_get_child_count(layer) ? lv_obj_get_child(layer, 0) : NULL;
}

static bool confirm_showing(void) {
	lv_obj_t *v = confirm_veil_obj();
	return v != NULL && !lv_obj_is_hidden(v);
}

static void set_release(const char *tag) {
	snprintf(release.tag, sizeof(release.tag), "%s", tag);
	release.size = 52428800;
}

static void tick(void) {
	lv_tick_inc(300);
	lv_timer_handler();
}

int main(void) {
	lv_init();
	font_ui_22 = lv_font_montserrat_14;
	font_ui_24 = lv_font_montserrat_14;
	font_ui_26 = lv_font_montserrat_14;
	font_ui_14 = lv_font_montserrat_14;
	font_ui_20 = lv_font_montserrat_14;
	palette.text_primary = lv_color_white();
	palette.text_secondary = lv_color_hex(0xaaaaaa);
	palette.accent = lv_color_hex(0xff9944);
	palette.surface = lv_color_hex(0x202020);
	lv_style_init(&theme_style_card);
	lv_style_set_bg_color(&theme_style_card, palette.surface);
	lv_style_set_bg_opa(&theme_style_card, LV_OPA_COVER);
	lv_style_init(&theme_style_text);
	lv_style_set_text_color(&theme_style_text, palette.text_primary);
	lv_style_init(&theme_style_text_dim);
	lv_style_set_text_color(&theme_style_text_dim, palette.text_secondary);
	lv_style_init(&theme_style_switch);
	lv_style_set_bg_color(&theme_style_switch, lv_color_hex(0x404040));

	lv_display_t *display = lv_display_create(240, 320);
	static uint8_t buffer[240 * 320 * 4];
	lv_display_set_buffers(display, buffer, NULL, sizeof(buffer), LV_DISPLAY_RENDER_MODE_DIRECT);
	lv_display_set_flush_cb(display, flush_cb);

	gui_config_t cfg = { .screen_width = 240, .screen_height = 320, .padding = 8, .top_bar_height = 30, .sd_root_path = NULL };
	confirm_init(&cfg);

	battery_set(100, false);
	set_release("1.6.3");
	ota_auto_init();

	assert(!dot_visible);
	assert(notice_blinks == 0);
	assert(fwupdate_download_starts == 0);

	// A connect-triggered check starts the check immediately.
	ota_auto_link_online();
	assert(check_starts == 1);
	tick();
	assert(check_starts == 1); // the periodic timer is 6 h out
	assert(notice_blinks == 1);
	assert(dot_visible);
	assert(confirm_showing());
	assert(find_label(lv_layer_top(), "Update firmware"));
	assert(find_label(lv_layer_top(), "Version 1.6.3 · 52 MB"));

	// "Later": the popup goes, the dot and the tag stay, a toast says where.
	press_button_with_label("Later");
	assert(!confirm_showing());
	assert(dot_visible);
	assert(toasts == 1);
	assert(strcmp(config_get("system", "ota_dismissed_tag", ""), "1.6.3") == 0);

	// A later check for the same release brings nothing back: the tag is
	// dismissed, the popup is not re-shown, the dot stands.
	lv_tick_inc(6 * 60 * 60 * 1000);
	lv_timer_handler();
	tick();
	assert(check_starts == 2);
	assert(notice_blinks == 1);
	assert(toasts == 1);
	assert(dot_visible);
	assert(!confirm_showing());

	// A dot tap re-brings the popup, without re-showing the notice.
	dot_tap();
	tick();
	assert(confirm_showing());
	assert(dot_visible);
	assert(notice_blinks == 1);
	// ...and dismissing it sticks.
	press_button_with_label("Later");
	assert(toasts == 2);
	assert(dot_visible);

	// A new release is a new notice: the popup comes up with the new message.
	set_release("1.7.0");
	lv_tick_inc(6 * 60 * 60 * 1000 + 300);
	lv_timer_handler();
	tick();
	assert(check_starts == 3);
	assert(notice_blinks == 2);
	assert(toasts == 2);
	assert(confirm_showing());

	// "Update": the dot goes, and the download starts from the popup.
	press_button_with_label("Update");
	assert(!dot_visible);
	assert(fwupdate_download_starts == 1);

	// The toggle, off, silences the checks.
	ota_auto_set_enabled(false);
	assert(!dot_visible);
	assert(!ota_auto_enabled());
	assert(strcmp(config_get("system", "auto_update_notices", "true"), "false") == 0);
	lv_tick_inc(6 * 60 * 60 * 1000 + 300);
	lv_timer_handler();
	tick();
	assert(check_starts == 3);
	assert(notice_blinks == 2);
	assert(fwupdate_download_starts == 1);

	// The battery guard: below 30% and not charging, no check at all.
	ota_auto_set_enabled(true);
	battery_set(10, false);
	lv_tick_inc(6 * 60 * 60 * 1000 + 300);
	lv_timer_handler();
	tick();
	assert(check_starts == 3);
	assert(notice_blinks == 2);

	// Back at 100%: the check resumes. The release was already shown and
	// handled (the user updated), so nothing comes back.
	battery_set(100, false);
	lv_tick_inc(6 * 60 * 60 * 1000 + 300);
	lv_timer_handler();
	tick();
	assert(check_starts == 4);
	assert(notice_blinks == 2);
	assert(!dot_visible);

	// No Wi-Fi: nothing is started, even with the switch on.
	wifi_connected = false;
	lv_tick_inc(6 * 60 * 60 * 1000 + 300);
	lv_timer_handler();
	tick();
	assert(check_starts == 4);
	assert(notice_blinks == 2);
	wifi_connected = true;

	// The update made from the manual card: the standing dot is hidden and the
	// notice never comes back for that tag. Simulate the stale reminder the
	// card leaves behind, then the update is made.
	set_release("1.9.0");
	topbar_set_update_available(true);
	assert(dot_visible);
	ota_auto_release_taken();
	assert(!dot_visible);
	// The taken tag is remembered on disk, so a restart does not re-notify.
	assert(strcmp(config_get("system", "ota_taken_tag", ""), "1.9.0") == 0);
	lv_tick_inc(6 * 60 * 60 * 1000 + 300);
	lv_timer_handler();
	tick();
	assert(check_starts == 5);
	assert(notice_blinks == 2);
	assert(!dot_visible);
	assert(!confirm_showing());

	// A newer release is a new notice: the popup comes up again.
	set_release("1.10.0");
	lv_tick_inc(6 * 60 * 60 * 1000 + 300);
	lv_timer_handler();
	tick();
	assert(check_starts == 6);
	assert(notice_blinks == 3);
	assert(dot_visible);
	assert(confirm_showing());

	// Defer, then make the update: the dot comes back for the deferral on the
	// next check, then the update is made and it is gone for good.
	press_button_with_label("Later");
	assert(dot_visible);
	assert(strcmp(config_get("system", "ota_dismissed_tag", ""), "1.10.0") == 0);
	lv_tick_inc(6 * 60 * 60 * 1000 + 300);
	lv_timer_handler();
	tick();
	assert(check_starts == 7);
	assert(dot_visible); // the deferred release stands, as designed
	ota_auto_release_taken();
	assert(!dot_visible);
	assert(strcmp(config_get("system", "ota_dismissed_tag", ""), "") == 0); // the deferral is forgotten
	lv_tick_inc(6 * 60 * 60 * 1000 + 300);
	lv_timer_handler();
	tick();
	assert(check_starts == 8);
	assert(notice_blinks == 3); // no new blink for the handled release
	assert(!dot_visible);
	assert(!confirm_showing());

	puts("PASS auto OTA notice: check on connect and at interval, LED/dot/toast/popup, dedupe, battery guard, toggle");
	return 0;
}
