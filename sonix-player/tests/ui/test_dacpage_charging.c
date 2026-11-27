/* DAC page: pass-through charging (issue #504).
 *
 * In DAC mode the player can run off the cable with the battery left out of it
 * entirely -- nothing goes in, nothing comes out -- so a long session on the
 * desk costs the cell no cycles and no heat. The choice is the user's, off by
 * default, and remembered across boots; on the page it is a named row with an
 * explanation under it, and the bolt in the header is the same setting as a
 * quick toggle while DAC mode is running.
 *
 * What is checked, on the real V1 layout and the real settings-row code:
 *   - the row exists, titled from the language file, with a switch on the right,
 *     under the Low latency row, and says what it does underneath;
 *   - it opens showing whatever the setting says, both ways;
 *   - a tap on the row writes and saves the setting, and the header bolt follows
 *     it -- one setting, two controls, never disagreeing;
 *   - the bolt drives the row the same way, and still refuses with an
 *     explanation while DAC mode is off, where the row does not;
 *   - the write reaches the config file, which is where the next boot reads it.
 *
 * The page is built from the real dacpage_init(), so the row under test is the
 * one the player draws, at the position it draws it. usbdac.c is stood in for
 * against the real config -- the charger itself is test_usbdac_charging's
 * subject, and pulling the USB gadget into a UI test would test neither.
 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "src/gui/board_profile.h"
#include "src/gui/fonts/fonts.h"
#include "src/gui/shell/icons.h"
#include "src/gui/shell/settingsrow.h"
#include "src/gui/shell/theme.h"
#include "src/system/audio/usbdac.h"
#include "src/system/core/config.h"
#include "src/system/core/lang.h"

lv_font_t font_ui_14, font_ui_16, font_ui_18, font_ui_20, font_ui_20_bold;
lv_font_t font_ui_22, font_ui_22_bold, font_ui_24, font_ui_24_bold;
lv_font_t font_ui_26, font_ui_28, font_ui_32;

static void init_fonts(void) {
	lv_font_t *all[] = {&font_ui_14, &font_ui_16, &font_ui_18, &font_ui_20, &font_ui_20_bold,
						&font_ui_22, &font_ui_22_bold, &font_ui_24, &font_ui_24_bold, &font_ui_26,
						&font_ui_28, &font_ui_32};
	for (size_t i = 0; i < sizeof(all) / sizeof(all[0]); i++) {
		*all[i] = lv_font_montserrat_14;
	}
}

void fonts_register_change(void (*cb)(void)) { (void)cb; }
int fonts_text_size(void) { return FONTS_TEXT_NORMAL; }
void bootlogo_set(int choice, bool dark) { (void)choice; (void)dark; }
bool bootpic_present(void) { return false; }
bool player_sheet_drag_active(void) { return false; }
bool switcher_back_drag_active(void) { return false; }
void switcher_attach_back_gesture(lv_obj_t *screen) { (void)screen; }
void switcher_mark_fresh_start(lv_obj_t *screen) { (void)screen; }
void switcher_set_back_guard(lv_obj_t *screen, bool (*guard)(void)) { (void)screen; (void)guard; }
void switch_screen(lv_obj_t *target) { (void)target; }
void back_btn_cb(lv_event_t *e) { (void)e; }

static const char *last_popup;
void gui_notify_popup(const char *text) { last_popup = text; }
void gui_notify_popup_long(const char *text) { last_popup = text; }
void confirm_show(const char *title, const char *message, const char *ok_label, void (*on_ok)(void *user), void *user) {
	(void)title;
	(void)message;
	(void)ok_label;
	(void)on_ok;
	(void)user;
}

// ---------------------------------------------------------------------------
// usbdac.c, in its own terms against the real config
// ---------------------------------------------------------------------------

static bool stub_dac_active;
static int stub_charge_calls;

bool usbdac_is_active(void) { return stub_dac_active; }

void usbdac_get_state(usbdac_state_t *out) {
	memset(out, 0, sizeof(*out));
	out->active = stub_dac_active;
}

unsigned usbdac_serial(void) { return stub_dac_active ? 1u : 0u; }

bool usbdac_start(void) {
	stub_dac_active = true;
	return true;
}

void usbdac_stop(void) { stub_dac_active = false; }

bool usbdac_low_latency_enabled(void) { return config_get_bool("usb", "dac_low_latency", false); }

void usbdac_set_low_latency(bool enabled) { config_set_bool("usb", "dac_low_latency", enabled); }

// The real one caches the answer and applies it to the charger while the mode is
// running; what the page needs from it is the setting and the write.
bool usbdac_charging_enabled(void) { return !config_get_bool("usb", "dac_charge_disable", false); }

void usbdac_set_charging(bool enabled) {
	stub_charge_calls++;
	config_set_bool("usb", "dac_charge_disable", !enabled);
	config_save();
}

#include "../src/gui/audio/dacpage.c"

// ---------------------------------------------------------------------------
// The cases
// ---------------------------------------------------------------------------

static void check_the_row_is_there(lv_obj_t *container) {
	assert(usb_power_switch != NULL);
	lv_obj_t *card = lv_obj_get_parent(usb_power_switch);
	assert(card != NULL);
	assert(lv_obj_get_parent(card) == container);

	// Its title, out of the language file, beside the switch rather than over
	// it. A bare tag here is a translation missing from the file.
	lv_obj_t *title = lv_obj_get_child(card, 0);
	assert(title != NULL);
	assert(strcmp(lv_label_get_text(title), tr("dac_usb_power_only")) == 0);
	assert(strcmp(lv_label_get_text(title), "dac_usb_power_only") != 0);

	// Under DAC mode and Low latency: it is a DAC setting and belongs with the
	// other two, not somewhere the page happens to have room.
	lv_obj_t *latency_card = lv_obj_get_parent(low_latency_switch);
	lv_area_t a, b;
	lv_obj_get_coords(latency_card, &a);
	lv_obj_get_coords(card, &b);
	assert(b.y1 >= a.y2);

	// And what it means, under the row: the two bolts in the header differ by
	// one thin stroke, which says nothing about the battery.
	lv_obj_t *note = lv_obj_get_child(container, (int32_t)(lv_obj_get_index(card) + 1));
	assert(note != NULL);
	assert(strcmp(lv_label_get_text(note), tr("dac_usb_power_only_note")) == 0);
	assert(strcmp(lv_label_get_text(note), "dac_usb_power_only_note") != 0);
}

static void check_the_switch_follows_the_setting(void) {
	// Charging allowed, which is the default: the switch is off.
	config_set_bool("usb", "dac_charge_disable", false);
	refresh();
	assert(!lv_obj_has_state(usb_power_switch, LV_STATE_CHECKED));

	// Pass-through on, read back by the page's own refresh -- the state goes
	// both ways rather than only being able to turn on.
	config_set_bool("usb", "dac_charge_disable", true);
	refresh();
	assert(lv_obj_has_state(usb_power_switch, LV_STATE_CHECKED));
}

// A tap on the row: the setting written, and the header bolt showing the same
// thing. The bolt is the live control while DAC mode runs, so it has to follow a
// change made here rather than keep showing the glyph it was built with.
static void check_a_tap_writes_the_setting_and_moves_the_bolt(void) {
	config_set_bool("usb", "dac_charge_disable", false);
	refresh();
	assert(lv_image_get_src(charge_glyph) == &icon_zap);
	int before = stub_charge_calls;

	usb_power_row_cb(NULL); // the whole row is the target, as elsewhere

	assert(config_get_bool("usb", "dac_charge_disable", false) == true);
	assert(stub_charge_calls == before + 1);
	assert(lv_obj_has_state(usb_power_switch, LV_STATE_CHECKED));
	assert(lv_image_get_src(charge_glyph) == &icon_zap_off);

	// And back again from the same row.
	usb_power_row_cb(NULL);
	assert(config_get_bool("usb", "dac_charge_disable", true) == false);
	assert(!lv_obj_has_state(usb_power_switch, LV_STATE_CHECKED));
	assert(lv_image_get_src(charge_glyph) == &icon_zap);
}

// The bolt is the same setting from the other end: it toggles while DAC mode is
// running, says so in words, and the row has to show what it did.
static void check_the_bolt_drives_the_row(void) {
	stub_dac_active = true;
	config_set_bool("usb", "dac_charge_disable", false);
	refresh();
	assert(!lv_obj_has_state(usb_power_switch, LV_STATE_CHECKED));

	last_popup = NULL;
	charge_cb(NULL);
	assert(config_get_bool("usb", "dac_charge_disable", false) == true);
	assert(lv_obj_has_state(usb_power_switch, LV_STATE_CHECKED));
	assert(lv_image_get_src(charge_glyph) == &icon_zap_off);
	assert(last_popup != NULL && strcmp(last_popup, "dac_usb_charging_off") == 0);

	last_popup = NULL;
	charge_cb(NULL);
	assert(config_get_bool("usb", "dac_charge_disable", true) == false);
	assert(!lv_obj_has_state(usb_power_switch, LV_STATE_CHECKED));
	assert(last_popup != NULL && strcmp(last_popup, "dac_usb_charging_on") == 0);

	// With the mode off the bolt explains itself instead of writing a setting
	// that would not be applied -- and the row still works, because the choice
	// is remembered and applied whenever DAC mode starts.
	stub_dac_active = false;
	last_popup = NULL;
	int before = stub_charge_calls;
	charge_cb(NULL);
	assert(stub_charge_calls == before);
	assert(last_popup != NULL && strcmp(last_popup, "dac_turn_dac_mode_on_first") == 0);

	last_popup = NULL;
	usb_power_row_cb(NULL);
	assert(stub_charge_calls == before + 1);
	assert(config_get_bool("usb", "dac_charge_disable", false) == true);
	assert(last_popup == NULL); // the note under the row is the explanation
}

// The tap reaches the file: a player set to run from the cable has to come back
// from a reboot doing it.
static void check_the_write_reaches_the_file(const char *config_path) {
	config_set_bool("usb", "dac_charge_disable", false);
	refresh();
	usb_power_row_cb(NULL);

	FILE *f = fopen(config_path, "r");
	assert(f != NULL);
	char line[256];
	int in_usb = 0, found = 0;
	while (fgets(line, sizeof(line), f)) {
		if (line[0] == '[') {
			in_usb = strncmp(line, "[usb]", 5) == 0;
		} else if (in_usb && strncmp(line, "dac_charge_disable", 18) == 0) {
			assert(strstr(line, "= 1") != NULL);
			found = 1;
		}
	}
	fclose(f);
	assert(found);
}

int main(int argc, char **argv) {
	assert(argc == 2);
	char dir[] = "/tmp/sonix-dacpage-charging-XXXXXX";
	assert(mkdtemp(dir));
	char path[256];
	snprintf(path, sizeof(path), "%s/device_config.ini", dir);
	config_init(path);
	config_set("ui", "language", "English");
	lang_init(argv[1]);
	init_fonts();
	lv_init();
	lv_display_create(bp_screen_w(), bp_screen_h());
	theme_init();

	gui_config_t cfg = {bp_screen_w(), bp_screen_h(), bp_padding(), bp_status_bar_h(), dir};
	dacpage_screen = lv_obj_create(NULL);
	dacpage_init(&cfg);
	lv_screen_load(dacpage_screen);
	lv_obj_update_layout(dacpage_screen);

	// Off by default, before anything is tapped: the feature must not change
	// what a player that has never seen the row does.
	assert(usbdac_charging_enabled());
	assert(lv_image_get_src(charge_glyph) == &icon_zap);

	// The page's own container: the switch's parent is its card, and the card's
	// parent is what settingsrow_page() built the rows into.
	lv_obj_t *container = lv_obj_get_parent(lv_obj_get_parent(mode_switch));
	check_the_row_is_there(container);
	check_the_switch_follows_the_setting();
	check_a_tap_writes_the_setting_and_moves_the_bolt();
	check_the_bolt_drives_the_row();
	check_the_write_reaches_the_file(path);

	unlink(path);
	rmdir(dir);
	printf("PASS DAC page: pass-through charging row (position, note, state, tap, bolt and the written setting)\n");
	lv_deinit();
	return 0;
}
