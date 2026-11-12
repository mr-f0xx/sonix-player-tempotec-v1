// The real PEQ page and profile engine on the V1 renderer. Check the profile
// pills in every shipped language/theme, that a Control Centre switch
// refreshes both the main page and an open band editor underneath it, and the
// keypad a band's frequency, gain and Q are typed on.
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
static lv_obj_t *chevron_hidden_on;
void back_btn_hide_on_screen(lv_obj_t *screen, bool hidden) {
	if (hidden) { chevron_hidden_on = screen; } else if (chevron_hidden_on == screen) { chevron_hidden_on = NULL; }
}
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
// ---------------------------------------------------------------------------
// Typing a value: the keypad behind the three numbers of the band editor.
// ---------------------------------------------------------------------------
static lv_obj_t *editor, *entry_layer, *entry_pad;

static lv_obj_t *key(const char *text) {
	lv_obj_t *label = find_label(entry_pad, text);
	if (!label) { fprintf(stderr, "no key \"%s\"\n", text); }
	assert(label);
	return lv_obj_get_parent(label);
}
// A string of keys, one character each, as a finger would press them.
static void type(const char *keys) {
	for (; *keys; keys++) {
		char text[2] = {*keys, '\0'};
		assert(lv_obj_is_clickable(key(text)));
		lv_obj_send_event(key(text), LV_EVENT_CLICKED, NULL);
	}
}
static void tap(lv_obj_t *obj) { lv_obj_send_event(obj, LV_EVENT_CLICKED, NULL); }
static lv_obj_t *delete_key(void) {
	for (uint32_t i = 0; i < lv_obj_get_child_count(entry_pad); i++) {
		lv_obj_t *child = lv_obj_get_child(entry_pad, i);
		if (lv_obj_check_type(lv_obj_get_child(child, 0), &lv_image_class)) { return child; }
	}
	assert(0);
	return NULL;
}
static lv_obj_t *cancel_key(void) {
	for (uint32_t i = 0; i < lv_obj_get_child_count(entry_layer); i++) {
		lv_obj_t *child = lv_obj_get_child(entry_layer, i);
		if (lv_obj_check_type(child, &lv_button_class)) { return child; }
	}
	assert(0);
	return NULL;
}
static bool shows(const char *text) {
	lv_obj_t *label = find_label(entry_layer, text);
	if (!label) { fprintf(stderr, "keypad does not show \"%s\"\n", text); }
	return label && lv_obj_get_parent(label) != entry_pad && !lv_obj_is_hidden(entry_layer);
}
// The number over a slider, by what it reads. The editor's cards come before
// the keypad in the tree, so its display saying the same thing is not found.
static lv_obj_t *chip(const char *text) {
	lv_obj_t *label = find_label(editor, text);
	if (!label) { fprintf(stderr, "no value \"%s\" in the band editor\n", text); }
	assert(label && lv_obj_is_clickable(label));
	return label;
}
static void open_keypad(const char *value) {
	assert(lv_obj_is_hidden(entry_layer));
	tap(chip(value));
	assert(!lv_obj_is_hidden(entry_layer) && chevron_hidden_on == editor);
}
static void accept(void) {
	tap(key("OK"));
	assert(lv_obj_is_hidden(entry_layer) && chevron_hidden_on == NULL);
}
static peq_band_t band0(void) {
	peq_band_t b;
	peq_get_band(0, &b);
	return b;
}
static bool inside(const lv_area_t *in, const lv_area_t *out) {
	return in->x1 >= out->x1 && in->x2 <= out->x2 && in->y1 >= out->y1 && in->y2 <= out->y2;
}
static bool apart(const lv_area_t *a, const lv_area_t *b) {
	return a->x2 < b->x1 || b->x2 < a->x1 || a->y2 < b->y1 || b->y2 < a->y1;
}
// Every key that is up: on the panel, a finger wide, clear of the others and
// holding its own caption. `keys` is how many the pad shows for this value.
static void check_keypad_layout(uint32_t keys) {
	lv_obj_update_layout(editor);
	lv_area_t screen = {0, 0, bp_screen_w() - 1, bp_screen_h() - 1};
	lv_area_t areas[16];
	uint32_t count = 0;
	for (uint32_t i = 0; i < lv_obj_get_child_count(entry_pad); i++) {
		lv_obj_t *child = lv_obj_get_child(entry_pad, i);
		if (lv_obj_is_hidden(child)) { continue; }
		lv_area_t inner;
		lv_obj_get_coords(child, &areas[count]);
		lv_obj_get_coords(lv_obj_get_child(child, 0), &inner);
		assert(inside(&areas[count], &screen) && inside(&inner, &areas[count]));
		assert(lv_obj_get_width(child) >= 48 && lv_obj_get_height(child) >= 32);
		for (uint32_t other = 0; other < count; other++) { assert(apart(&areas[count], &areas[other])); }
		count++;
	}
	assert(count == keys);
	// OK closes the last row, flush with the right-hand column above it.
	lv_area_t pad;
	lv_obj_get_coords(entry_pad, &pad);
	assert(areas[count - 1].x2 == pad.x2 && areas[count - 1].y2 == pad.y2);
	assert(areas[count - 1].x1 == (keys == 14 ? areas[1].x1 : pad.x1));

	lv_area_t cancel, heading, field;
	lv_obj_get_coords(cancel_key(), &cancel);
	lv_obj_get_coords(lv_obj_get_child(entry_layer, 0), &heading);
	assert(inside(&cancel, &screen) && lv_obj_get_width(cancel_key()) >= 36 && lv_obj_get_height(cancel_key()) >= 36);
	assert(cancel.y1 >= bp_status_bar_h() && heading.y1 >= bp_status_bar_h() && heading.x2 < cancel.x1);
	for (uint32_t i = 0; i < lv_obj_get_child_count(entry_layer); i++) {
		lv_obj_t *child = lv_obj_get_child(entry_layer, i);
		if (child == entry_pad || child == cancel_key() || child == lv_obj_get_child(entry_layer, 0)) { continue; }
		lv_area_t shown;
		lv_obj_get_coords(child, &field);
		lv_obj_get_coords(lv_obj_get_child(child, 0), &shown);
		assert(inside(&shown, &field) && field.y1 > cancel.y2 && field.y2 < pad.y1);
	}
}
// The widest thing each of the three numbers can say, in its box over the
// slider: inside the card, clear of the name and of the slider under it.
static void check_chips(void) {
	peq_band_t saved = band0(), wide = saved;
	wide.freq = PEQ_FREQ_MAX; wide.gain_tenths = PEQ_GAIN_MIN_TENTHS; wide.q_cent = PEQ_Q_MAX;
	peq_set_band(0, &wide);
	lv_obj_send_event(editor, LV_EVENT_REFRESH, NULL);
	lv_obj_update_layout(editor);
	const char *texts[] = {"20,0 kHz", "-15,0 dB", "10,00"};
	for (int i = 0; i < 3; i++) {
		lv_obj_t *value = chip(texts[i]), *card = lv_obj_get_parent(value);
		lv_area_t v, c, name;
		lv_obj_get_coords(value, &v);
		lv_obj_get_coords(card, &c);
		lv_obj_get_coords(lv_obj_get_child(card, 0), &name);
		assert(inside(&v, &c) && v.x1 > name.x2);
		for (uint32_t k = 0; k < lv_obj_get_child_count(card); k++) {
			lv_obj_t *child = lv_obj_get_child(card, k);
			if (!lv_obj_check_type(child, &lv_slider_class)) { continue; }
			lv_area_t slider;
			lv_obj_get_coords(child, &slider);
			// The knob stands proud of its track by its own padding.
			assert(v.y2 < slider.y1 - lv_obj_get_style_pad_top(child, LV_PART_KNOB));
		}
	}
	peq_set_band(0, &saved);
	lv_obj_send_event(editor, LV_EVENT_REFRESH, NULL);
}
static void check_entry(void) {
	peq_band_t b = band0();
	b.on = true; b.type = PEQ_TYPE_PEAK; b.freq = 100; b.gain_tenths = 60; b.q_cent = 100;
	peq_set_band(0, &b);
	lv_obj_send_event(lv_obj_get_parent(find_label(peqpage_screen(), "Band 1")), LV_EVENT_CLICKED, NULL);
	editor = lv_screen_active();
	assert(editor != peqpage_screen());
	entry_pad = lv_obj_get_parent(lv_obj_get_parent(find_label(editor, "7")));
	entry_layer = lv_obj_get_parent(entry_pad);
	assert(lv_obj_get_parent(entry_layer) == editor && lv_obj_is_hidden(entry_layer));
	check_chips();

	// A tap that is the end of a swipe back opens nothing.
	back_drag = true;
	tap(chip("100 Hz"));
	assert(lv_obj_is_hidden(entry_layer) && chevron_hidden_on == NULL);
	back_drag = false;

	// Frequency in Hz: the old value is replaced by the first digit, the comma
	// is dead, and what is typed is the band's frequency to the Hz -- 1250 is
	// between two steps of the slider.
	open_keypad("100 Hz");
	assert(shows("100 Hz") && shows("Frequency"));
	assert(!lv_obj_is_hidden(key("kHz")) && !lv_obj_is_clickable(key(",")));
	check_keypad_layout(14);
	type("1250");
	assert(shows("1250 Hz"));
	accept();
	assert(band0().freq == 1250);

	// It reopens in kHz, to the Hz, and the key beside OK goes to the other
	// unit and back without touching the value or making it the typed one.
	open_keypad("1,2 kHz");
	assert(shows("1,25 kHz") && lv_obj_is_clickable(key(",")));
	check_keypad_layout(14);
	tap(key("Hz"));
	assert(shows("1250 Hz") && !lv_obj_is_clickable(key(",")));
	type("47");
	assert(shows("47 Hz"));
	tap(key("kHz"));
	assert(shows("0,047 kHz"));
	tap(key("Hz"));
	assert(shows("47 Hz"));
	tap(key(",")); // not reachable by a finger; refused all the same
	assert(shows("47 Hz"));
	accept();
	assert(band0().freq == 47);

	// Typed in kHz, with trailing zeros dropped when it is shown again.
	open_keypad("47 Hz");
	tap(key("kHz"));
	assert(shows("0,047 kHz"));
	type("2,5");
	assert(shows("2,5 kHz"));
	accept();
	assert(band0().freq == 2500);
	open_keypad("2,5 kHz");
	assert(shows("2,5 kHz"));
	type(",0635"); // a fourth decimal rounds the Hz
	assert(shows("0,0635 kHz"));
	accept();
	assert(band0().freq == 64);

	// Clamped to the band's range at both ends, in either unit.
	open_keypad("64 Hz");
	type("5");
	accept();
	assert(band0().freq == PEQ_FREQ_MIN);
	open_keypad("20 Hz");
	tap(key("kHz"));
	type("9999999");
	type("9"); // one more than the display takes
	assert(shows("9999999 kHz"));
	accept();
	assert(band0().freq == PEQ_FREQ_MAX);
	open_keypad("20,0 kHz");
	assert(shows("20 kHz"));
	tap(key("Hz"));
	assert(shows("20000 Hz"));

	// The X, an emptied display and leaving the page all change nothing.
	type("555");
	tap(cancel_key());
	assert(lv_obj_is_hidden(entry_layer) && chevron_hidden_on == NULL && band0().freq == PEQ_FREQ_MAX);
	open_keypad("20,0 kHz");
	tap(delete_key());
	assert(shows("0 kHz"));
	accept();
	assert(band0().freq == PEQ_FREQ_MAX);
	open_keypad("20,0 kHz");
	type("1");
	lv_screen_load(peqpage_screen());
	assert(lv_obj_is_hidden(entry_layer) && chevron_hidden_on == NULL && band0().freq == PEQ_FREQ_MAX);
	lv_screen_load(editor);

	// Gain: the same key is the sign, and a typed value keeps the old sign.
	open_keypad("+6,0 dB");
	assert(shows("6,0 dB") && shows("Gain") && !lv_obj_is_hidden(key("+/-")));
	check_keypad_layout(14);
	tap(key("+/-"));
	assert(shows("-6,0 dB"));
	accept();
	assert(band0().gain_tenths == -60);
	open_keypad("-6,0 dB");
	type("3,7");
	assert(shows("-3,7 dB"));
	accept();
	assert(band0().gain_tenths == -37);
	open_keypad("-3,7 dB");
	tap(key("+/-"));
	type("99");
	accept();
	assert(band0().gain_tenths == PEQ_GAIN_MAX_TENTHS);

	// Q has no key beside OK, which takes the row.
	open_keypad("1,00");
	assert(shows("1,00") && lv_obj_is_hidden(key("+/-")) && lv_obj_is_clickable(key(",")));
	check_keypad_layout(13);
	type("1,41");
	accept();
	assert(band0().q_cent == 141);
	open_keypad("1,41");
	type("0");
	accept();
	assert(band0().q_cent == PEQ_Q_MIN);

	// And after Q the frequency has its unit key back.
	open_keypad("20,0 kHz");
	assert(!lv_obj_is_hidden(key("Hz")));
	check_keypad_layout(14);
	tap(cancel_key());

	// The other themes paint the same keys; none may leave one unreadable.
	for (int kind = 0; kind < THEME_KIND_COUNT; kind++) {
		theme_set_kind((theme_kind_t)kind);
		open_keypad("20,0 kHz");
		for (uint32_t i = 0; i < lv_obj_get_child_count(entry_pad); i++) {
			lv_obj_t *child = lv_obj_get_child(entry_pad, i), *caption = lv_obj_get_child(child, 0);
			if (!lv_obj_check_type(caption, &lv_label_class)) { continue; }
			assert(!lv_color_eq(lv_obj_get_style_bg_color(child, LV_PART_MAIN), lv_obj_get_style_text_color(caption, LV_PART_MAIN)));
		}
		tap(cancel_key());
	}
	theme_set_kind(THEME_KIND_DARK);
	lv_screen_load(peqpage_screen());
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
	check_entry();
	printf("peq UI: profile pills fit 11 languages / 5 themes; selection, refresh and scoped reset passed\n");
	printf("peq UI: frequency (Hz and kHz), gain and Q typed on the keypad, clamped, cancelled; keys fit the panel\n");
	lv_deinit();
	return 0;
}
