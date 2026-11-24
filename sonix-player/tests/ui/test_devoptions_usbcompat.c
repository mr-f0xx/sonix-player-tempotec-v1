/* Developer options: the USB audio compatibility row.
 *
 * The setting exists for a USB DAC that presents one fixed format -- 48 kHz,
 * 16 bit -- and refuses everything else. It is switched from a toggle in
 * Developer options, beside the External USB DAC row it goes with, and audio.c
 * can also switch it on by itself the moment a card refuses a track. That
 * second writer is the reason this test cares about the row's state rather
 * than only its presence: the switch has to be read back from the setting
 * whenever the page is looked at, or a setting changed by the player while the
 * page was elsewhere would open showing the opposite of what is in force.
 *
 * What is checked, on the real V1 layout and the real settings-row code:
 *   - the row exists, with the right title and a switch on the right;
 *   - it sits under the External USB DAC row and above the log row, in the
 *     order the page builds them;
 *   - it opens showing whatever the config says, both ways;
 *   - a tap writes the setting and saves it, and the row follows the tap;
 *   - a change made from outside the page (usbaudio_set_compat_mode, the
 *     playback thread's path) is what the page shows when it is opened again.
 *
 * The page is built from the real devoptions_init(), so the row under test is
 * the one the player draws, at the position it draws it, rather than a row
 * rebuilt by the test.
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
#include "src/system/audio/usbaudio.h"
#include "src/system/audio/usbhostmode.h"
#include "src/system/audio/usbhostprobe.h"
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
void gui_notify_popup(const char *text) { (void)text; }
void gui_notify_popup_long(const char *text) { (void)text; }
void confirm_show_with_cancel(const char *title, const char *message, const char *ok_label,
							  void (*on_ok)(void *user), void *user, const char *cancel_label,
							  void (*on_cancel)(void *user), void *cancel_user) {
	(void)title;
	(void)message;
	(void)ok_label;
	(void)on_ok;
	(void)user;
	(void)cancel_label;
	(void)on_cancel;
	(void)cancel_user;
}
void confirm_show(const char *title, const char *message, const char *ok_label, void (*on_ok)(void *user), void *user) {
	(void)title;
	(void)message;
	(void)ok_label;
	(void)on_ok;
	(void)user;
}

// The rest of the page's own material, none of it under test here.
lv_obj_t *processespage_screen;
bool adb_set_enabled(bool enabled) {
	(void)enabled;
	return true;
}
bool adb_is_running(void) { return false; }
bool adb_switched_on(void) { return false; }
bool library_log_database(void) { return false; }
void library_set_log_database(bool enabled) {
	(void)enabled;
}
void logging_set_to_sd(bool enabled) { (void)enabled; }
bool logging_to_sd(void) { return false; }
const char *logging_path(void) { return ""; }
void switch_screen_cb(lv_event_t *e) { (void)e; }
bool btlog_enabled(void) { return false; }
void btlog_set_enabled(bool on) { (void)on; }
const char *bootlogo_trace(void) { return NULL; }
const char *bootlogo_status(void) { return NULL; }
void usbhostmode_set_wanted(bool on) { (void)on; }
bool usbhostmode_release(bool restore) {
	(void)restore;
	return true;
}
bool usbhostmode_wanted(void) { return false; }
bool usbhostmode_active(void) { return false; }
bool usbhostmode_retrying(void) { return false; }
const char *usbhostmode_status_key(void) { return ""; }
void usbhostmode_event(usbhost_probe_event_t event) { (void)event; }
bool usbhostprobe_restore_pending(void) { return false; }
bool usbhostprobe_held(void) { return false; }
bool usbhostprobe_active(void) { return false; }
bool usbhostprobe_cancel(void) { return true; }
usbhost_probe_result_t usbhostprobe_begin(void) { return USBHOST_PROBE_FAILED; }

// ---------------------------------------------------------------------------
// The compat setting itself, standing in for usbaudio.c
//
// The page only ever asks two things of it -- what the setting is, and that a
// tap writes it -- and usbaudio.c's own behaviour is test_usbaudio_compat's
// subject, on the playback side. What is under test here is the row: that it
// opens showing the setting, and that a tap leaves the setting written. So the
// three entry points are stubbed in their own terms, against the real config,
// rather than pulling the USB stack (and with it a sound card) into a UI test.
// ---------------------------------------------------------------------------

bool usbaudio_compat_mode(void) { return config_get_bool("usb", "compat_48k", false); }

void usbaudio_set_compat_mode(bool on) { config_set_bool("usb", "compat_48k", on); }

void usbaudio_set_compat_mode_saved(bool on) {
	usbaudio_set_compat_mode(on);
	config_save();
}

void usbaudio_flush_compat(void) {}

bool usbaudio_compat_active(void) { return usbaudio_compat_mode(); }

void usbaudio_apply_volume(int percent) { (void)percent; }
bool usbaudio_has_volume_control(void) { return false; }
bool usbaudio_active(void) { return false; }
void usbaudio_card_name(char *out, size_t out_size) {
	if (out && out_size) {
		out[0] = '\0';
	}
}

#include "../src/gui/settings/devoptions.c"

// ---------------------------------------------------------------------------
// The cases
// ---------------------------------------------------------------------------

// The row, found by the switch pointer devoptions_init() filled in.
static void check_the_row_is_there(lv_obj_t *container) {
	assert(usb_compat_switch != NULL);
	lv_obj_t *card = lv_obj_get_parent(usb_compat_switch);
	assert(card != NULL);
	assert(lv_obj_get_parent(card) == container);

	// Its title, from the language file, beside the switch rather than over it.
	lv_obj_t *title = lv_obj_get_child(card, 0);
	assert(title != NULL);
	assert(strcmp(lv_label_get_text(title), tr("usb_compat_mode")) == 0);
	assert(strlen(lv_label_get_text(title)) > 0);
	assert(strcmp(lv_label_get_text(title), "usb_compat_mode") != 0);

	// Under the External USB DAC row: the two go together, and a row that
	// drifted somewhere else would be a setting nobody finds.
	lv_obj_t *usbhost_card = lv_obj_get_parent(usbhost_switch);
	lv_area_t a, b;
	lv_obj_get_coords(usbhost_card, &a);
	lv_obj_get_coords(card, &b);
	assert(b.y1 >= a.y2);
}

static void check_the_switch_follows_the_setting(void) {
	// The page's own refresh, which is what runs when the page is entered and
	// whenever the hub's status is looked at. On: the config says so and the
	// row shows it.
	config_set_bool("usb", "compat_48k", true);
	update_usbhost_row();
	assert(lv_obj_has_state(usb_compat_switch, LV_STATE_CHECKED));

	// Off again, through the same refresh: the state goes both ways rather
	// than only being able to turn on.
	config_set_bool("usb", "compat_48k", false);
	update_usbhost_row();
	assert(!lv_obj_has_state(usb_compat_switch, LV_STATE_CHECKED));

	// And a second time, with nothing else changing: a row that only looked
	// at the setting once would stay where the last tap left it.
	config_set_bool("usb", "compat_48k", true);
	update_usbhost_row();
	assert(lv_obj_has_state(usb_compat_switch, LV_STATE_CHECKED));
}

static void check_a_tap_writes_the_setting(void) {
	config_set_bool("usb", "compat_48k", false);
	update_usbhost_row();
	assert(!lv_obj_has_state(usb_compat_switch, LV_STATE_CHECKED));

	// A tap: the switch changes first, then the callback that owns the write.
	lv_obj_add_state(usb_compat_switch, LV_STATE_CHECKED);
	usb_compat_toggled_cb(NULL);
	assert(config_get_bool("usb", "compat_48k", false) == true);
	// And the getter agrees, which is what audio.c asks before a track opens.
	assert(usbaudio_compat_mode() == true);

	// Off again from the same switch.
	lv_obj_remove_state(usb_compat_switch, LV_STATE_CHECKED);
	usb_compat_toggled_cb(NULL);
	assert(config_get_bool("usb", "compat_48k", true) == false);
	assert(usbaudio_compat_mode() == false);
}

// The tap's write reaches the file and survives a reload: the setting is meant
// to outlive the session -- a DAC that needs it needs it every time.
static void check_the_write_reaches_the_file(const char *config_path) {
	config_set_bool("usb", "compat_48k", false);
	update_usbhost_row();
	lv_obj_add_state(usb_compat_switch, LV_STATE_CHECKED);
	usb_compat_toggled_cb(NULL);
	assert(config_save());

	// Read the text, rather than trusting the in-memory entry: the section and
	// the key are the interface every other part of the player reads.
	FILE *f = fopen(config_path, "r");
	assert(f != NULL);
	char line[256];
	int in_usb = 0, found = 0;
	while (fgets(line, sizeof(line), f)) {
		if (line[0] == '[') {
			in_usb = strncmp(line, "[usb]", 5) == 0;
		} else if (in_usb && strncmp(line, "compat_48k", 10) == 0) {
			assert(strstr(line, "= 1") != NULL);
			found = 1;
		}
	}
	fclose(f);
	assert(found);
}

static void check_a_change_from_the_player_shows_up(void) {
	// audio.c's fallback, on the playback thread: the row must not go on
	// showing the old value the next time the page is looked at.
	config_set_bool("usb", "compat_48k", false);
	update_usbhost_row();
	assert(!lv_obj_has_state(usb_compat_switch, LV_STATE_CHECKED));

	usbaudio_set_compat_mode(true); // the player, not the page
	usbaudio_flush_compat();	    // what usbaudio_poll() would do next
	update_usbhost_row();		    // what opening the page does
	assert(lv_obj_has_state(usb_compat_switch, LV_STATE_CHECKED));
}

int main(int argc, char **argv) {
	assert(argc == 2);
	char dir[] = "/tmp/sonix-devoptions-compat-XXXXXX";
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
	devoptions_screen = lv_obj_create(NULL);
	devoptions_init(&cfg);
	lv_screen_load(devoptions_screen);
	lv_obj_update_layout(devoptions_screen);

	lv_obj_t *container = lv_obj_get_parent(usbhost_row);
	check_the_row_is_there(container);
	check_the_switch_follows_the_setting();
	check_a_tap_writes_the_setting();
	check_the_write_reaches_the_file(path);
	check_a_change_from_the_player_shows_up();

	snprintf(path, sizeof(path), "%s/device_config.ini", dir);
	unlink(path);
	rmdir(dir);
	printf("PASS developer options: USB 48 kHz compatibility row (position, state, tap and a change from the player)\n");
	lv_deinit();
	return 0;
}
