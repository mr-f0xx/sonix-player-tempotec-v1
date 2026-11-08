/* Music settings row geometry on the real V1 LVGL layout.
 *
 * The DSD gain card has a flex header and expandable pills, while High gain,
 * Filters and NOS use ordinary settings rows. Their glyphs and titles must
 * still line up in every theme/language, with the compensation title centred
 * when the pills are hidden. NOS must not borrow Filters' waveform bitmap.
 */
#include <assert.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "src/gui/board_profile.h"
#include "src/gui/fonts/fonts.h"
#include "src/gui/shell/icons.h"
#include "src/gui/shell/settingsrow.h"
#include "src/gui/shell/theme.h"
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
	font_ui_20 = font_ui_22 = font_ui_24 = lv_font_montserrat_16;
	font_ui_26 = font_ui_28 = font_ui_32 = lv_font_montserrat_20;
}

void fonts_register_change(void (*cb)(void)) { (void)cb; }
int fonts_text_size(void) { return FONTS_TEXT_NORMAL; }
void bootlogo_set(int choice, bool dark) { (void)choice; (void)dark; }
bool bootpic_present(void) { return false; }
bool player_sheet_drag_active(void) { return false; }
bool switcher_back_drag_active(void) { return false; }
void switcher_attach_back_gesture(lv_obj_t *screen) { (void)screen; }

static void toggle_cb(lv_event_t *e) { (void)e; }

static int center_y(lv_obj_t *obj) {
	lv_area_t a;
	lv_obj_get_coords(obj, &a);
	return a.y1 + a.y2;
}

static void check_centered(lv_obj_t *obj, lv_obj_t *card) {
	int diff = center_y(obj) - center_y(card);
	assert(diff >= -1 && diff <= 1);
}

static void check_rows(lv_obj_t *screen, lv_obj_t *gain_card, lv_obj_t *gain_label, lv_obj_t *gain_switch,
		       lv_obj_t *dsd_card, lv_obj_t *dsd_label, lv_obj_t *dsd_switch, lv_obj_t *pills,
		       lv_obj_t *nos_label, lv_obj_t *filters_label, bool expanded) {
	lv_obj_update_layout(screen);

	lv_obj_t *header = lv_obj_get_parent(dsd_label);
	lv_obj_t *dsd_icon = lv_obj_get_child(header, 0);
	lv_obj_t *gain_icon = lv_obj_get_child(gain_card, 2);
	lv_area_t gain, dsd, gain_title, gain_ctrl, dsd_title, nos_title, filters_title, dsd_ctrl;
	lv_obj_get_coords(gain_card, &gain);
	lv_obj_get_coords(dsd_card, &dsd);
	lv_obj_get_coords(gain_label, &gain_title);
	lv_obj_get_coords(dsd_label, &dsd_title);
	lv_obj_get_coords(gain_switch, &gain_ctrl);
	lv_obj_get_coords(dsd_switch, &dsd_ctrl);
	lv_obj_get_coords(nos_label, &nos_title);
	lv_obj_get_coords(filters_label, &filters_title);

	/* Same icon column and title column as the neighbouring rows. */
	assert(dsd_title.x1 == gain_title.x1 && nos_title.x1 == gain_title.x1 && filters_title.x1 == gain_title.x1);
	lv_area_t gain_glyph, dsd_glyph;
	lv_obj_get_coords(gain_icon, &gain_glyph);
	lv_obj_get_coords(dsd_icon, &dsd_glyph);
	assert(dsd_glyph.x1 == gain_glyph.x1);
	assert(dsd_glyph.x2 < dsd_title.x1);
	assert(dsd_title.x2 <= dsd_ctrl.x1);

	/* The switch keeps the same right edge and midline as ordinary rows. */
	assert(dsd_ctrl.x2 == gain_ctrl.x2);
	check_centered(dsd_switch, header);
	check_centered(dsd_icon, header);
	check_centered(dsd_label, header);
	if (!expanded) {
		check_centered(dsd_switch, dsd_card);
		check_centered(dsd_icon, dsd_card);
		check_centered(dsd_label, dsd_card);
	}
	assert(dsd.y1 >= gain.y1 && dsd.x1 == gain.x1 && dsd.x2 == gain.x2);
	assert(dsd_title.y1 >= dsd.y1 && dsd_title.y2 <= dsd.y2);

	if (!expanded) {
		assert(lv_obj_is_hidden(pills));
		assert(dsd.y2 - dsd.y1 >= gain.y2 - gain.y1);
		return;
	}

	assert(!lv_obj_is_hidden(pills));
	lv_area_t head, pill_area;
	lv_obj_get_coords(header, &head);
	lv_obj_get_coords(pills, &pill_area);
	assert(pill_area.y1 > head.y2);
	assert(pill_area.y1 > dsd_title.y2 && pill_area.y1 > dsd_ctrl.y2);
	assert(lv_obj_get_child_count(pills) > 0);
	lv_area_t first_pill;
	lv_obj_get_coords(lv_obj_get_child(pills, 0), &first_pill);
	assert(first_pill.x1 == dsd_title.x1);
	assert(pill_area.x2 <= dsd.x2);
	assert(pill_area.y2 <= dsd.y2);
	for (uint32_t i = 0; i < lv_obj_get_child_count(pills); i++) {
		lv_obj_t *pill = lv_obj_get_child(pills, i);
		lv_area_t a;
		lv_obj_get_coords(pill, &a);
		assert(a.x1 >= dsd.x1 && a.x2 <= dsd.x2);
		assert(a.y1 >= dsd.y1 && a.y2 <= dsd.y2);
	}
}

static void set_expanded(lv_obj_t *toggle, lv_obj_t *pills, bool expanded) {
	if (expanded) {
		lv_obj_add_state(toggle, LV_STATE_CHECKED);
	} else {
		lv_obj_remove_state(toggle, LV_STATE_CHECKED);
	}
	lv_obj_set_hidden(pills, !expanded);
}

static void relabel(lv_obj_t *label, const char *key) {
	lv_label_set_text(label, tr(key));
}

static void check_languages(const char *dir, lv_obj_t *screen, lv_obj_t *gain_card, lv_obj_t *gain_label,
			    lv_obj_t *gain_switch, lv_obj_t *dsd_card, lv_obj_t *dsd_label, lv_obj_t *dsd_switch,
			    lv_obj_t *pills, lv_obj_t *nos_label, lv_obj_t *filters_label) {
	DIR *d = opendir(dir);
	assert(d);
	struct dirent *ent;
	int languages = 0;
	while ((ent = readdir(d))) {
		size_t len = strlen(ent->d_name);
		if (len < 5 || strcmp(ent->d_name + len - 4, ".ini") != 0) {
			continue;
		}
		char language[128];
		snprintf(language, sizeof(language), "%.*s", (int)(len - 4), ent->d_name);
		assert(lang_set(language));
		relabel(gain_label, "musicsettings_high_gain");
		relabel(dsd_label, "musicsettings_dsd_gain");
		relabel(nos_label, "musicsettings_nos");
		relabel(filters_label, "musicsettings_filters");
		for (int kind = 0; kind < THEME_KIND_COUNT; kind++) {
			theme_set_kind((theme_kind_t)kind);
			for (int expanded = 0; expanded <= 1; expanded++) {
				set_expanded(dsd_switch, pills, expanded != 0);
				check_rows(screen, gain_card, gain_label, gain_switch, dsd_card, dsd_label, dsd_switch,
					   pills, nos_label, filters_label, expanded != 0);
			}
		}
		languages++;
	}
	closedir(d);
	assert(languages == 11);
}

int main(int argc, char **argv) {
	assert(argc == 2);
	char dir[] = "/tmp/sonix-musicsettings-rows-XXXXXX";
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
	lv_obj_t *screen = lv_obj_create(NULL);
	lv_obj_t *container = settingsrow_page(screen, &cfg, "music");
	lv_obj_t *gain_switch, *dsd_switch, *pills, *nos_switch;
	lv_obj_t *gain_card = settingsrow_toggle(container, "musicsettings_high_gain", &gain_switch, toggle_cb);
	settingsrow_add_accent_icon(gain_card, &icon_high_gain);
	lv_obj_t *dsd_card = settingsrow_toggle_pills(container, "musicsettings_dsd_gain", toggle_cb, &dsd_switch, &pills);
	settingsrow_toggle_pills_add_accent_icon(dsd_card, &icon_quality_dsd);
	for (int i = 1; i <= 6; i++) {
		char text[16];
		snprintf(text, sizeof(text), "+%d dB", i);
		settingsrow_pill_text(pills, text, i, toggle_cb);
	}
	lv_obj_t *filters_row = settingsrow_add(container, "musicsettings_filters", NULL, NULL, NULL);
	settingsrow_add_accent_icon(filters_row, &icon_audio_waveform);
	lv_obj_t *nos_row = settingsrow_toggle(container, "musicsettings_nos", &nos_switch, toggle_cb);
	settingsrow_add_accent_icon(nos_row, &icon_nos);
	set_expanded(dsd_switch, pills, false);
	lv_screen_load(screen);

	lv_obj_t *gain_label = lv_obj_get_child(gain_card, 0);
	lv_obj_t *dsd_label = lv_obj_get_child(lv_obj_get_child(dsd_card, 0), 1);
	lv_obj_t *nos_label = lv_obj_get_child(nos_row, 0);
	lv_obj_t *filters_label = lv_obj_get_child(filters_row, 0);
	lv_obj_t *nos_icon = lv_obj_get_child(nos_row, 2);
	lv_obj_t *filters_icon = lv_obj_get_child(filters_row, 2);
	assert(lv_image_get_src(nos_icon) == &icon_nos);
	assert(lv_image_get_src(filters_icon) == &icon_audio_waveform);
	assert(icon_nos.data != icon_audio_waveform.data);
	assert(icon_nos.data_size == icon_audio_waveform.data_size);
	assert(memcmp(icon_nos.data, icon_audio_waveform.data, icon_nos.data_size) != 0);

	check_languages(argv[1], screen, gain_card, gain_label, gain_switch, dsd_card, dsd_label, dsd_switch, pills,
			nos_label, filters_label);

	/* Exercise a larger wrapped title as well as the normal V1 metrics. */
	assert(lang_set("English"));
	relabel(gain_label, "musicsettings_high_gain");
	relabel(dsd_label, "musicsettings_dsd_gain");
	relabel(nos_label, "musicsettings_nos");
	relabel(filters_label, "musicsettings_filters");
	font_ui_24 = lv_font_montserrat_20;
	relabel(dsd_label, "musicsettings_dsd_gain");
	for (int kind = 0; kind < THEME_KIND_COUNT; kind++) {
		theme_set_kind((theme_kind_t)kind);
		for (int expanded = 0; expanded <= 1; expanded++) {
			set_expanded(dsd_switch, pills, expanded != 0);
			check_rows(screen, gain_card, gain_label, gain_switch, dsd_card, dsd_label, dsd_switch, pills,
				   nos_label, filters_label, expanded != 0);
		}
	}

	snprintf(path, sizeof(path), "%s/device_config.ini", dir);
	unlink(path);
	rmdir(dir);
	printf("PASS musicsettings rows: distinct NOS glyph; DSD title/icon/switch alignment in 11 languages, 5 themes, both states and large text\n");
	lv_deinit();
	return 0;
}
