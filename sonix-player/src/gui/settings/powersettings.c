#include "powersettings.h"

#include "lvgl/lvgl.h"

#include "src/gui/fonts/fonts.h"
#include "src/gui/shell/settingsrow.h"
#include "src/gui/shell/theme.h"
#include "src/system/core/config.h"
#include "src/system/core/lang.h"
#include "src/system/device/axpcharge.h"
#include "src/system/device/led.h"
#include "src/system/device/power.h"
#include "src/system/device/system.h"

lv_obj_t *powersettings_screen;

// 80 to 100 in fives. Stopping short of full is what keeps a lithium cell
// healthy if the device mostly lives on the charger.
#define CHARGE_LIMIT_MIN 80
#define CHARGE_LIMIT_STEP 5
#define CHARGE_LIMIT_COUNT 5 // 80, 85, 90, 95, 100

// The RGB LED's brightness and fade controls, shown only where the SGM31324
// can be driven directly (no such knobs for the plain LEDs on the R1).
// The controller's ladder has ten lit levels; level 0 is off. Keep the
// default at 5 so a player updated from the five-step slider keeps the light
// it had, with five more steps above it for those who want them.
#define LED_BRIGHTNESS_MIN 1
#define LED_BRIGHTNESS_MAX 10
#define LED_BRIGHTNESS_DEFAULT 5
#define LED_BRIGHTNESS_STEPS (LED_BRIGHTNESS_MAX - LED_BRIGHTNESS_MIN + 1)
#define LED_TRANSITION_MIN_MS 20
#define LED_TRANSITION_STEP_MS 10
#define LED_TRANSITION_STEPS 199 // 20 ms to 2000 ms in 10 ms steps

static const struct {
	int minutes;
	const char *label;
} AUTO_OFF[] = {
	{15, "power_15_minutes"}, {30, "power_30_minutes"}, {60, "power_1_hour"}, {120, "power_2_hours"}, {180, "power_3_hours"}, {360, "power_6_hours"},
};
#define AUTO_OFF_COUNT ((int)(sizeof(AUTO_OFF) / sizeof(AUTO_OFF[0])))

static const struct {
	led_visualizer_style_t style;
	const char *label;
} LED_STYLES[] = {
	{LED_VISUALIZER_BASS_PULSE, "power_led_bass_pulse"},
	{LED_VISUALIZER_ENERGY_GLOW, "power_led_energy_glow"},
	{LED_VISUALIZER_RAINBOW_BREATH, "power_led_rainbow_breath"},
};
#define LED_STYLE_COUNT (sizeof(LED_STYLES) / sizeof(LED_STYLES[0]))

static lv_obj_t *charge_value, *charge_slider;
static lv_obj_t *auto_off_switch, *auto_off_value, *auto_off_slider, *auto_off_card;
static lv_obj_t *led_on_switch, *led_off_switch, *led_off_card;
static lv_obj_t *led_stealth_switch, *led_visualizer_switch, *led_visualizer_card;
static lv_obj_t *led_style_pills, *led_style_button[LED_STYLE_COUNT];
static lv_obj_t *led_bright_value, *led_bright_slider;
static lv_obj_t *led_trans_value, *led_trans_slider;
static lv_obj_t *key_lock_switch;
static lv_obj_t *hold_menu_switch;
static lv_obj_t *standby_switch;
static lv_obj_t *charge_note;
static lv_obj_t *axp_limit_switch, *axp_current_switch; // the R1's two, instead of the slider

// --- reading the saved values ---

static int charge_index(void) {
	int percent = (int)config_get_int("power", "charge_limit", 100);
	int index = (percent - CHARGE_LIMIT_MIN) / CHARGE_LIMIT_STEP;
	if (index < 0) {
		index = 0;
	}
	if (index >= CHARGE_LIMIT_COUNT) {
		index = CHARGE_LIMIT_COUNT - 1;
	}
	return index;
}

static int auto_off_index(void) {
	int minutes = (int)config_get_int("power", "auto_off_minutes", 30);
	for (int i = 0; i < AUTO_OFF_COUNT; i++) {
		if (AUTO_OFF[i].minutes == minutes) {
			return i;
		}
	}
	return 1;
}

static int led_brightness_index(void) {
	int brightness = (int)config_get_int("power", "led_brightness", LED_BRIGHTNESS_DEFAULT);
	if (brightness < LED_BRIGHTNESS_MIN) {
		brightness = LED_BRIGHTNESS_MIN;
	}
	if (brightness > LED_BRIGHTNESS_MAX) {
		brightness = LED_BRIGHTNESS_MAX;
	}
	return brightness - LED_BRIGHTNESS_MIN;
}

static int led_transition_index(void) {
	int ms = (int)config_get_int("power", "led_transition_ms", 500);
	int index = (ms - LED_TRANSITION_MIN_MS) / LED_TRANSITION_STEP_MS;
	if (index < 0) {
		index = 0;
	}
	if (index >= LED_TRANSITION_STEPS) {
		index = LED_TRANSITION_STEPS - 1;
	}
	return index;
}

// --- applying them ---

static void set_checked(lv_obj_t *sw, bool on) {
	if (on) {
		lv_obj_add_state(sw, LV_STATE_CHECKED);
	} else {
		lv_obj_remove_state(sw, LV_STATE_CHECKED);
	}
}

void powersettings_apply(void) {
	if (axpcharge_applies()) {
		bool limit_80 = config_get_bool("power", "charge_limit_80", false);
		power_set_charge_limit(limit_80 ? 80 : 100);
		axpcharge_set(limit_80, config_get_bool("power", "charge_500ma", false));
	} else {
		power_set_charge_limit(CHARGE_LIMIT_MIN + (charge_index() * CHARGE_LIMIT_STEP));
	}

	power_set_auto_off(config_get_bool("power", "auto_off", false), (uint32_t)AUTO_OFF[auto_off_index()].minutes);

	// Suspend-to-RAM standby, on by default: saves battery by suspending the
	// player shortly after the screen goes off.
	power_set_standby_enabled(config_get_bool("power", "standby_mem", true));

	led_set_stealth(config_get_bool("power", "led_stealth", false));
	led_set_enabled(config_get_bool("power", "led_on", true));
	led_set_idle_off(config_get_bool("power", "led_off_standby", true));
	led_set_brightness(LED_BRIGHTNESS_MIN + led_brightness_index());
	led_set_transition_ms(LED_TRANSITION_MIN_MS + (led_transition_index() * LED_TRANSITION_STEP_MS));
	led_set_visualizer(config_get_bool("power", "led_visualizer", false));

	input_set_key_lock(config_get_bool("power", "lock_keys_screen_off", false));

	// What a held power key does: the menu with Power off and Reboot, or the
	// power-off itself with nothing to confirm.
	input_set_power_hold_menu(config_get_bool("power", "hold_power_menu", true));
}

static void set_toggle_available(lv_obj_t *card, lv_obj_t *toggle, bool available) {
	lv_obj_set_state(toggle, LV_STATE_DISABLED, !available);
	lv_obj_set_style_opa(card, available ? LV_OPA_COVER : LV_OPA_40, 0);
}

static void refresh_labels(void) {
	if (charge_value) {
		lv_label_set_text_fmt(charge_value, "%d%%", CHARGE_LIMIT_MIN + (charge_index() * CHARGE_LIMIT_STEP));
	}
	if (axp_limit_switch) {
		set_checked(axp_limit_switch, config_get_bool("power", "charge_limit_80", false));
		set_checked(axp_current_switch, config_get_bool("power", "charge_500ma", false));
	}
	lv_label_set_text(auto_off_value, tr(AUTO_OFF[auto_off_index()].label));

	bool led_on = config_get_bool("power", "led_on", true);
	bool stealth = config_get_bool("power", "led_stealth", false);
	set_checked(led_on_switch, led_on);
	set_checked(led_stealth_switch, stealth);
	set_checked(led_off_switch, config_get_bool("power", "led_off_standby", true));
	set_toggle_available(led_off_card, led_off_switch, led_on && !stealth);
	if (led_visualizer_switch) {
		bool smart = config_get_bool("power", "led_visualizer", false);
		bool available = led_on && !stealth;
		set_checked(led_visualizer_switch, smart);
		set_toggle_available(led_visualizer_card, led_visualizer_switch, available);
		lv_obj_set_hidden(led_style_pills, !smart || !available);
		led_visualizer_style_t style = led_visualizer_style();
		for (size_t i = 0; i < LED_STYLE_COUNT; i++) {
			settingsrow_pill_active(led_style_button[i], style == LED_STYLES[i].style);
		}
	}

	if (led_bright_value) {
		lv_label_set_text_fmt(led_bright_value, "%d", LED_BRIGHTNESS_MIN + led_brightness_index());
	}
	if (led_trans_value) {
		lv_label_set_text_fmt(led_trans_value, "%d ms", LED_TRANSITION_MIN_MS + (led_transition_index() * LED_TRANSITION_STEP_MS));
	}

	set_checked(key_lock_switch, config_get_bool("power", "lock_keys_screen_off", false));
	set_checked(hold_menu_switch, config_get_bool("power", "hold_power_menu", true));

	if (config_get_bool("power", "standby_mem", true)) {
		lv_obj_add_state(standby_switch, LV_STATE_CHECKED);
	} else {
		lv_obj_remove_state(standby_switch, LV_STATE_CHECKED);
	}

	// One card: the slider is only on it while the toggle is on.
	bool auto_off = config_get_bool("power", "auto_off", false);
	if (auto_off) {
		lv_obj_add_state(auto_off_switch, LV_STATE_CHECKED);
	} else {
		lv_obj_remove_state(auto_off_switch, LV_STATE_CHECKED);
	}
	settingsrow_toggle_slider_expanded(auto_off_card, auto_off);
}

static void charge_changed_cb(lv_event_t *e) {
	(void)e;
	int index = (int)lv_slider_get_value(charge_slider);
	config_set_int("power", "charge_limit", CHARGE_LIMIT_MIN + (index * CHARGE_LIMIT_STEP));
	config_save();
	powersettings_apply();
	refresh_labels();
}

static void axp_limit_toggled_cb(lv_event_t *e) {
	(void)e;
	config_set_bool("power", "charge_limit_80", lv_obj_has_state(axp_limit_switch, LV_STATE_CHECKED));
	config_save();
	powersettings_apply();
	refresh_labels();
}

static void axp_current_toggled_cb(lv_event_t *e) {
	(void)e;
	config_set_bool("power", "charge_500ma", lv_obj_has_state(axp_current_switch, LV_STATE_CHECKED));
	config_save();
	powersettings_apply();
	refresh_labels();
}

static void auto_off_changed_cb(lv_event_t *e) {
	(void)e;
	int index = (int)lv_slider_get_value(auto_off_slider);
	config_set_int("power", "auto_off_minutes", AUTO_OFF[index].minutes);
	config_save();
	powersettings_apply();
	refresh_labels();
}

static void led_brightness_cb(lv_event_t *e) {
	(void)e;
	int brightness = LED_BRIGHTNESS_MIN + (int)lv_slider_get_value(led_bright_slider);
	config_set_int("power", "led_brightness", brightness);
	config_save();
	led_set_brightness(brightness);
	refresh_labels();
}

static void led_transition_cb(lv_event_t *e) {
	(void)e;
	int ms = LED_TRANSITION_MIN_MS + ((int)lv_slider_get_value(led_trans_slider) * LED_TRANSITION_STEP_MS);
	config_set_int("power", "led_transition_ms", ms);
	config_save();
	led_set_transition_ms(ms);
	refresh_labels();
}

static void led_stealth_cb(lv_event_t *e) {
	(void)e;
	bool enabled = lv_obj_has_state(led_stealth_switch, LV_STATE_CHECKED);
	led_set_stealth(enabled);
	config_set_bool("power", "led_stealth", enabled);
	config_save();
	refresh_labels();
}

static void led_visualizer_cb(lv_event_t *e) {
	(void)e;
	bool enabled = lv_obj_has_state(led_visualizer_switch, LV_STATE_CHECKED);
	led_set_visualizer(enabled);
	config_set_bool("power", "led_visualizer", enabled);
	config_save();
	refresh_labels();
}

static void led_visualizer_style_cb(lv_event_t *e) {
	led_visualizer_style_t style = (led_visualizer_style_t)(intptr_t)lv_event_get_user_data(e);
	led_set_visualizer_style(style);
	config_set_int("power", "led_visualizer_style", style);
	config_save();
	refresh_labels();
}

static void led_off_toggled_cb(lv_event_t *e) {
	(void)e;
	config_set_bool("power", "led_off_standby", lv_obj_has_state(led_off_switch, LV_STATE_CHECKED));
	config_save();
	powersettings_apply();
	refresh_labels();
}

// The master switch. With the LED off the standby option has nothing left to
// do, so it greys out -- and the charger still lights the LED regardless, so
// a charge in progress is never invisible.
static void led_on_toggled_cb(lv_event_t *e) {
	(void)e;
	config_set_bool("power", "led_on", lv_obj_has_state(led_on_switch, LV_STATE_CHECKED));
	config_save();
	powersettings_apply();
	refresh_labels();
}

static void key_lock_toggled_cb(lv_event_t *e) {
	(void)e;
	config_set_bool("power", "lock_keys_screen_off", lv_obj_has_state(key_lock_switch, LV_STATE_CHECKED));
	config_save();
	powersettings_apply();
	refresh_labels();
}

// The button thread reads the option at the moment a hold reaches its
// deadline, so this is in force from the next hold of the power key onwards.
static void hold_menu_toggled_cb(lv_event_t *e) {
	(void)e;
	config_set_bool("power", "hold_power_menu", lv_obj_has_state(hold_menu_switch, LV_STATE_CHECKED));
	config_save();
	powersettings_apply();
	refresh_labels();
}

static void auto_off_toggled_cb(lv_event_t *e) {
	(void)e;
	config_set_bool("power", "auto_off", lv_obj_has_state(auto_off_switch, LV_STATE_CHECKED));
	config_save();
	powersettings_apply();
	refresh_labels();
}

static void standby_toggled_cb(lv_event_t *e) {
	(void)e;
	config_set_bool("power", "standby_mem", lv_obj_has_state(standby_switch, LV_STATE_CHECKED));
	config_save();
	powersettings_apply();
	refresh_labels();
}

// The control centre switches the LED and the key lock from outside, so the
// switches are read again each time the page comes up.
static void power_loaded_cb(lv_event_t *e) {
	(void)e;
	refresh_labels();
}

static void led_note(lv_obj_t *container, const char *key) {
	lv_obj_t *label = lv_label_create(container);
	lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
	lv_obj_set_width(label, lv_pct(100));
	lv_obj_add_style(label, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(label, &font_ui_14, 0);
	lv_label_set_text(label, tr(key));
}

void powersettings_init(gui_config_t *cfg) {
	lv_obj_t *container = settingsrow_page(powersettings_screen, cfg, "power");

	// Suspend-to-RAM standby: suspends the player shortly after the screen goes
	// off to save battery, and playback resumes on wake. On by default. What it
	// waits for is the screen going dark, and that timeout lives on the Screen
	// page, beside the brightness it belongs with.
	settingsrow_toggle(container, "power_standby_mem", &standby_switch, standby_toggled_cb);
	lv_obj_t *standby_note = lv_label_create(container);
	lv_label_set_long_mode(standby_note, LV_LABEL_LONG_WRAP);
	lv_obj_set_width(standby_note, lv_pct(100));
	lv_obj_add_style(standby_note, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(standby_note, &font_ui_22, 0);
	lv_label_set_text(standby_note, tr("power_standby_mem_note"));

	// The V1's AXP2101 offers voltage/current controls in addition to the
	// percentage cutoff: two switches here, the configurable slider elsewhere.
	bool pmic = axpcharge_applies();
	if (pmic) {
		settingsrow_toggle(container, "power_charge_limit_80", &axp_limit_switch, axp_limit_toggled_cb);
		lv_obj_t *limit_note = lv_label_create(container);
		lv_label_set_long_mode(limit_note, LV_LABEL_LONG_WRAP);
		lv_obj_set_width(limit_note, lv_pct(100));
		lv_obj_add_style(limit_note, &theme_style_text_dim, 0);
		lv_obj_set_style_text_font(limit_note, &font_ui_22, 0);
		lv_label_set_text(limit_note, tr("power_charge_limit_80_note"));
		settingsrow_toggle(container, "power_charge_500ma", &axp_current_switch, axp_current_toggled_cb);
	} else {
		settingsrow_slider(container, "power_charge_limit", CHARGE_LIMIT_COUNT, &charge_value, &charge_slider,
						   charge_changed_cb);
		lv_slider_set_value(charge_slider, charge_index(), LV_ANIM_OFF);
	}

	settingsrow_toggle(container, "power_led_on", &led_on_switch, led_on_toggled_cb);
	settingsrow_toggle(container, "power_led_stealth", &led_stealth_switch, led_stealth_cb);
	led_note(container, "power_led_stealth_note");

	// The visualizer, brightness and fades need the direct RGB controller.
	if (led_effects_available()) {
		led_visualizer_card = settingsrow_toggle_pills(container, "power_led_visualizer", led_visualizer_cb,
													 &led_visualizer_switch, &led_style_pills);
		for (size_t i = 0; i < LED_STYLE_COUNT; i++) {
			led_style_button[i] = settingsrow_pill(led_style_pills, LED_STYLES[i].label, LED_STYLES[i].style,
														 led_visualizer_style_cb);
			// Keep the normal font and let long labels wrap on the V1.
			lv_obj_set_style_pad_hor(led_style_button[i], 6, 0);
		}
		led_note(container, "power_led_visualizer_note");

		settingsrow_slider(container, "power_led_brightness", LED_BRIGHTNESS_STEPS, &led_bright_value, &led_bright_slider,
						   led_brightness_cb);
		lv_slider_set_value(led_bright_slider, led_brightness_index(), LV_ANIM_OFF);

		settingsrow_slider(container, "power_led_transition", LED_TRANSITION_STEPS, &led_trans_value, &led_trans_slider,
						   led_transition_cb);
		lv_slider_set_value(led_trans_slider, led_transition_index(), LV_ANIM_OFF);
	}

	led_off_card = settingsrow_toggle(container, "power_led_off_in_standby", &led_off_switch, led_off_toggled_cb);
	// No "turn Wi-Fi off in standby" entry: parking the radio is automatic, and
	// wifi_in_use() in power.c knows on its own when not to turn it off --
	// Qobuz, Tidal, podcasts, radio, AirPlay, DLNA, transfers.

	// What a held power key does (system.c): the menu with its two slides, or
	// the power-off itself. Next to the key lock because both are about the
	// buttons rather than about the battery.
	settingsrow_toggle(container, "power_hold_for_menu", &hold_menu_switch, hold_menu_toggled_cb);
	lv_obj_t *hold_menu_note = lv_label_create(container);
	lv_label_set_long_mode(hold_menu_note, LV_LABEL_LONG_WRAP);
	lv_obj_set_width(hold_menu_note, lv_pct(100));
	lv_obj_add_style(hold_menu_note, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(hold_menu_note, &font_ui_22, 0);
	lv_label_set_text(hold_menu_note, tr("power_hold_for_menu_note"));

	// The lock gesture for the buttons on the case (system.c).
	settingsrow_toggle(container, "power_lock_keys_screen_off", &key_lock_switch, key_lock_toggled_cb);
	lv_obj_t *key_lock_note = lv_label_create(container);
	lv_label_set_long_mode(key_lock_note, LV_LABEL_LONG_WRAP);
	lv_obj_set_width(key_lock_note, lv_pct(100));
	lv_obj_add_style(key_lock_note, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(key_lock_note, &font_ui_22, 0);
	lv_label_set_text(key_lock_note, tr("power_lock_keys_screen_off_note"));

	// Toggle and "after how long" on one card: the slider is only there while
	// the option is on.
	auto_off_card = settingsrow_toggle_slider(container, "power_auto_power_off", AUTO_OFF_COUNT,
											  &auto_off_switch, &auto_off_value, &auto_off_slider,
											  auto_off_toggled_cb, auto_off_changed_cb);
	lv_slider_set_value(auto_off_slider, auto_off_index(), LV_ANIM_OFF);

	// If the charger driver has no control node there is nothing to enforce
	// the limit with, and saying so is better than a setting that silently
	// does nothing.
	charge_note = lv_label_create(container);
	lv_label_set_long_mode(charge_note, LV_LABEL_LONG_WRAP);
	lv_obj_set_width(charge_note, lv_pct(100));
	lv_obj_add_style(charge_note, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(charge_note, &font_ui_22, 0);
	lv_label_set_text(charge_note, pmic || power_charge_limit_supported() ? tr("power_auto_off_note")
																	   : tr("power_charge_limit_unavailable"));

	// Everything saved becomes effective now, at startup -- including the LED
	// standby option, which lives in led.c rather than power.c.
	powersettings_apply();
	refresh_labels();
	lv_obj_add_event_cb(powersettings_screen, power_loaded_cb, LV_EVENT_SCREEN_LOADED, NULL);
}
