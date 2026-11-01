// The real PEQ page and profile engine on the V1 renderer. Check the profile
// pills in every shipped language/theme, and that a Control Centre switch
// refreshes both the main page and an open band editor underneath it.
#include <assert.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "src/gui/audio/peqpage.h"
#include "src/gui/audio/peqautoeq.h"
#include "src/gui/audio/peqsettings.h"
#include "src/gui/board_profile.h"
#include "src/gui/fonts/fonts.h"
#include "src/gui/shell/confirm.h"
#include "src/gui/shell/settingsrow.h"
#include "src/gui/shell/switcher.h"
#include "src/gui/shell/theme.h"
#include "src/system/audio/eq.h"
#include "src/system/core/config.h"
#include "src/system/core/lang.h"

lv_font_t font_ui_14, font_ui_16, font_ui_18, font_ui_20, font_ui_20_bold;
lv_font_t font_ui_22, font_ui_22_bold, font_ui_24, font_ui_24_bold;
lv_font_t font_ui_26, font_ui_28, font_ui_32, font_ui_36_bold, font_ui_64_bold, font_ui_72;
static void (*font_cb)(void);
static void init_fonts(void) {
	lv_font_t *all[] = {&font_ui_14, &font_ui_16, &font_ui_18, &font_ui_20, &font_ui_20_bold,
						&font_ui_22, &font_ui_22_bold, &font_ui_24, &font_ui_24_bold, &font_ui_26,
						&font_ui_28, &font_ui_32, &font_ui_36_bold, &font_ui_64_bold, &font_ui_72};
	for (size_t i = 0; i < sizeof(all) / sizeof(all[0]); i++) {
		*all[i] = lv_font_montserrat_14;
	}
	// Match the normal V1 raster sizes, not the nominal HiBy font names.
	font_ui_16 = font_ui_18 = lv_font_montserrat_14;
	font_ui_20 = font_ui_22 = lv_font_montserrat_16;
	font_ui_24 = lv_font_montserrat_16;
	font_ui_26 = lv_font_montserrat_20;
	font_ui_28 = lv_font_montserrat_20;
	font_ui_32 = lv_font_montserrat_20;
}
void fonts_register_change(void (*cb)(void)) { font_cb = cb; }
int fonts_text_size(void) { return FONTS_TEXT_NORMAL; }
void bootlogo_set(int choice, bool dark) { (void)choice; (void)dark; }
bool bootpic_present(void) { return false; }
void gui_notify_popup(const char *text) { fprintf(stderr, "popup: %s\n", text); assert(0); }
static bool back_drag;
bool switcher_back_drag_active(void) { return back_drag; }
bool player_sheet_drag_active(void) { return false; }
void switch_screen(lv_obj_t *screen) { lv_screen_load(screen); }
void switcher_attach_back_gesture(lv_obj_t *screen) { (void)screen; }
void back_btn_cb(lv_event_t *e) { (void)e; }
static void (*preset_reload)(void);
void peqsettings_set_reload_cb(void (*cb)(void)) { preset_reload = cb; }
lv_obj_t *peqsettings_screen(void) { return NULL; }
void peqautoeq_set_reload_cb(void (*cb)(void)) { (void)cb; }
bool peqautoeq_network_ready(void) { return true; }
bool peqautoeq_network_needed(void) { return false; }
void peqautoeq_open(void) {}
static void (*confirm_cb)(void *);
static void *confirm_user;
void confirm_show(const char *title, const char *message, const char *ok, void (*cb)(void *), void *user) {
	(void)title; (void)message; (void)ok; confirm_cb = cb; confirm_user = user;
}

static lv_obj_t *find_label(lv_obj_t *obj, const char *text) {
	if (lv_obj_check_type(obj, &lv_label_class) && strcmp(lv_label_get_text(obj), text) == 0) {
		return obj;
	}
	for (uint32_t i = 0; i < lv_obj_get_child_count(obj); i++) {
		lv_obj_t *found = find_label(lv_obj_get_child(obj, i), text);
		if (found) { return found; }
	}
	return NULL;
}
static lv_obj_t *profile_button(int profile) {
	char text[64];
	snprintf(text, sizeof(text), tr("peq_profile_name"), profile);
	lv_obj_t *label = find_label(peqpage_screen(), text);
	assert(label);
	return lv_obj_get_parent(label);
}
static void check_pills(void) {
	lv_obj_update_layout(peqpage_screen());
	for (int i = 1; i <= PEQ_PROFILE_COUNT; i++) {
		lv_obj_t *btn = profile_button(i);
		lv_obj_t *label = lv_obj_get_child(btn, 0);
		lv_area_t b, l;
		lv_obj_get_coords(btn, &b);
		lv_obj_get_coords(label, &l);
		assert(b.x1 >= 0 && b.y1 >= 0 && b.x2 < bp_screen_w() && b.y2 < bp_screen_h());
		assert(l.x1 >= b.x1 && l.x2 <= b.x2 && l.y1 >= b.y1 && l.y2 <= b.y2);
		assert(lv_obj_get_width(btn) >= 36 && lv_obj_get_height(btn) >= 36);
		lv_color_t bg = lv_obj_get_style_bg_color(btn, LV_PART_MAIN);
		lv_color_t fg = lv_obj_get_style_text_color(label, LV_PART_MAIN);
		assert(!lv_color_eq(bg, fg));
	}
}
static void check_languages(const char *dir) {
	DIR *d = opendir(dir);
	assert(d);
	struct dirent *ent;
	int languages = 0;
	while ((ent = readdir(d))) {
		size_t len = strlen(ent->d_name);
		if (len < 5 || strcmp(ent->d_name + len - 4, ".ini") != 0) { continue; }
		char language[128];
		snprintf(language, sizeof(language), "%.*s", (int)(len - 4), ent->d_name);
		assert(lang_set(language));
		assert(strcmp(tr("peq_profile"), "peq_profile") != 0);
		assert(strstr(tr("peq_profile_name"), "%d") != NULL);
		lv_obj_send_event(peqpage_screen(), LV_EVENT_REFRESH, NULL);
		for (int kind = 0; kind < THEME_KIND_COUNT; kind++) {
			theme_set_kind((theme_kind_t)kind);
			check_pills();
		}
		languages++;
	}
	closedir(d);
	assert(languages == 11);
	assert(lang_set("English"));
	lv_obj_send_event(peqpage_screen(), LV_EVENT_REFRESH, NULL);
}
static bool has_checked(lv_obj_t *obj) {
	if (lv_obj_check_type(obj, &lv_switch_class) && lv_obj_has_state(obj, LV_STATE_CHECKED)) { return true; }
	for (uint32_t i = 0; i < lv_obj_get_child_count(obj); i++) {
		if (has_checked(lv_obj_get_child(obj, i))) { return true; }
	}
	return false;
}
int main(int argc, char **argv) {
	assert(argc == 2);
	char dir[] = "/tmp/sonix-peq-ui-XXXXXX";
	assert(mkdtemp(dir));
	char path[512];
	snprintf(path, sizeof(path), "%s/device_config.ini", dir);
	config_init(path);
	config_set("ui", "language", "English");
	lang_init(argv[1]);
	eq_init();
	init_fonts();
	lv_init();
	gui_config_t cfg = {bp_screen_w(), bp_screen_h(), bp_padding(), bp_status_bar_h(), dir};
	lv_display_create(cfg.screen_width, cfg.screen_height);
	theme_init();
	peqpage_init(&cfg);
	lv_screen_load(peqpage_screen());
	check_languages(argv[1]);

	// Profile 1 on, Profile 2 off. The pills select; they do not enable PEQ.
	peq_set_enabled(true);
	peq_band_t band;
	peq_get_band(0, &band);
	band.on = true; band.freq = 100; band.gain_tenths = 60;
	peq_set_band(0, &band);
	peq_set_preamp(-60);
	lv_obj_send_event(profile_button(2), LV_EVENT_CLICKED, NULL);
	assert(peq_get_profile() == 2 && !peq_get_enabled());
	assert(!has_checked(peqpage_screen()));
	assert(peq_get_preamp() == 0);
	back_drag = true;
	lv_obj_send_event(profile_button(1), LV_EVENT_CLICKED, NULL);
	assert(peq_get_profile() == 2);
	back_drag = false;
	lv_obj_send_event(profile_button(1), LV_EVENT_CLICKED, NULL);
	assert(peq_get_profile() == 1 && peq_get_enabled() && peq_get_preamp() == -60);
	assert(has_checked(peqpage_screen()));
	assert(preset_reload);

	// Open Band 1, then change profile as the quick control would. Both pages
	// receive the shell's refresh; the editor must not keep the old frequency.
	lv_obj_t *row = find_label(peqpage_screen(), "Band 1");
	assert(row);
	lv_obj_send_event(lv_obj_get_parent(row), LV_EVENT_CLICKED, NULL);
	lv_obj_t *editor = lv_screen_active();
	assert(editor != peqpage_screen());
	assert(find_label(editor, "100 Hz"));
	peq_next_profile();
	lv_obj_send_event(peqpage_screen(), LV_EVENT_REFRESH, NULL);
	lv_obj_send_event(editor, LV_EVENT_REFRESH, NULL);
	assert(find_label(editor, "31 Hz"));
	assert(!has_checked(editor));

	// Reset is scoped to the selected profile, including its preamp.
	peq_set_preamp(-30);
	lv_screen_load(peqpage_screen());
	lv_obj_update_layout(peqpage_screen());
	// The reset action is the first corner button on the page screen.
	lv_obj_t *reset = NULL;
	for (uint32_t i = 0; i < lv_obj_get_child_count(peqpage_screen()); i++) {
		lv_obj_t *child = lv_obj_get_child(peqpage_screen(), i);
		if (lv_obj_check_type(child, &lv_button_class)) { reset = child; break; }
	}
	assert(reset);
	lv_obj_send_event(reset, LV_EVENT_CLICKED, NULL);
	assert(confirm_cb);
	confirm_cb(confirm_user);
	assert(peq_get_preamp() == 0);
	peq_select_profile(1);
	assert(peq_get_preamp() == -60);
	peq_get_band(0, &band);
	assert(band.on && band.freq == 100 && band.gain_tenths == 60);
	printf("peq UI: profile pills fit 11 languages / 5 themes; selection, refresh and scoped reset passed\n");
	lv_deinit();
	return 0;
}
