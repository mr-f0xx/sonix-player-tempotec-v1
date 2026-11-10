/* The volume pill's reading on the real V1 LVGL layout.
 *
 * With Settings > Screen > Volume as voltage on, the label beside the speaker
 * glyph prints up to six characters ("39.9µV") where it has always printed
 * three ("100"). The pill is a flex row -- glyph, label, scale -- so whatever
 * the label gains in width the scale loses. What has to hold:
 *
 *   - off, nothing has moved: the label keeps the width the pill was built
 *     with and prints the index;
 *   - on, the label has ONE width for every level the DAC can be at, so the
 *     scale does not change length under a finger or a held key, and that
 *     width takes each reading on one line -- a label wraps what does not fit,
 *     and a second line has nowhere to go in a 28 px strip;
 *   - glyph, label and scale stay inside the pill, in that order, and the
 *     scale keeps a length a finger can still use;
 *   - the width is given back when the reading goes back to the number (the
 *     option off again, or the sound leaving over Bluetooth), and is measured
 *     again after a change of face or text size.
 *
 * volume_overlay.c and volumereadout.c are the production files. The audio
 * side is four functions that return what the test last set: the gain tables
 * are test_volumereadout.c's business, and here the level is walked over every
 * step of the DAC's register instead, which is every reading the label can be
 * given. The fonts are LVGL's built-in Montserrat, as in the other headless
 * suites. It has no micro sign, so a microvolt reading is measured with LVGL's
 * stand-in for a missing glyph; no width is asserted in pixels except the one
 * the pill was built with, only that what was measured is what fits.
 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "src/gui/board_profile.h"
#include "src/gui/fonts/fonts.h"
#include "src/gui/shell/theme.h"
#include "src/gui/shell/volume_overlay.h"
#include "src/gui/shell/volumereadout.h"
#include "src/system/audio/alsa-controls.h"
#include "src/system/core/config.h"

lv_font_t font_ui_14, font_ui_16, font_ui_24;

/* The player around the pill. */
static void (*fonts_changed)(void);
static int fonts_registrations;
void fonts_register_change(void (*cb)(void)) {
	fonts_changed = cb;
	fonts_registrations++;
}
void bootlogo_set(int choice, bool dark) {
	(void)choice;
	(void)dark;
}
bool bootpic_present(void) { return false; }
bool player_sheet_is_open(void) { return true; } /* the one place the pill shows */
bool screensaver_is_visible(void) { return false; }

/* The audio side: an index, the level the DAC has for it, and where the sound goes. */
static int level_index = 40;
static long level_db100;
static volume_output_t level_output = VOLUME_OUTPUT_PHONES;
int get_volume_percent(void) { return level_index; }
void set_volume_percent(int percent) { level_index = percent; }
long alsa_volume_output_db100(int percent) { return percent <= 0 ? ALSA_VOLUME_MUTE_DB100 : level_db100; }
volume_output_t volume_profile_output(void) { return level_output; }

static lv_obj_t *veil, *pill, *glyph, *label, *scale;

/* The pill as it stands: `want` on one line of a label wide enough for it, and
 * the three children side by side inside the strip. */
static void check_pill(const char *want) {
	lv_obj_update_layout(veil);
	assert(!lv_obj_is_hidden(veil));

	const char *text = lv_label_get_text(label);
	if (strcmp(text, want) != 0) {
		fprintf(stderr, "the pill reads \"%s\", want \"%s\"\n", text, want);
		abort();
	}

	const lv_font_t *font = lv_obj_get_style_text_font(label, LV_PART_MAIN);
	int32_t letter_space = lv_obj_get_style_text_letter_space(label, LV_PART_MAIN);
	lv_point_t size;
	lv_text_get_size(&size, text, font, letter_space, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
	if (size.x > lv_obj_get_content_width(label) || lv_obj_get_height(label) != lv_font_get_line_height(font)) {
		fprintf(stderr, "\"%s\" is %d px wide in a %d px label that came out %d px high (one line is %d)\n", text,
			(int)size.x, (int)lv_obj_get_content_width(label), (int)lv_obj_get_height(label),
			(int)lv_font_get_line_height(font));
		abort();
	}

	lv_area_t strip, a, b, c;
	lv_obj_get_coords(pill, &strip);
	lv_obj_get_coords(glyph, &a);
	lv_obj_get_coords(label, &b);
	lv_obj_get_coords(scale, &c);
	assert(strip.x1 < a.x1 && a.x2 < b.x1 && b.x2 < c.x1 && c.x2 < strip.x2);
	assert(strip.y1 <= a.y1 && a.y2 <= strip.y2);
	assert(strip.y1 <= b.y1 && b.y2 <= strip.y2);
	assert(strip.y1 <= c.y1 && c.y2 <= strip.y2);
}

/* The index form, 0 to 100: the number, and the label never anything but `width`. */
static void check_numbers(int32_t width, int32_t scale_length) {
	for (int percent = 0; percent <= 100; percent++) {
		char want[16];
		snprintf(want, sizeof(want), "%d", percent);
		volume_overlay_show(percent);
		check_pill(want);
		assert(lv_obj_get_width(label) == width);
		assert(lv_obj_get_width(scale) == scale_length);
	}
}

/* The voltage form, over every level the DAC's register holds and the mute.
 * Returns the one width the label had for all of them. */
static int32_t check_voltages(void) {
	assert(volumereadout_form() != VOLUMEREADOUT_FORM_INDEX);
	int32_t width = -1, scale_length = -1;
	int distinct = 0;
	char previous[VOLUMEREADOUT_TEXT_MAX] = "";
	for (int step = 0; step <= 255; step++) {
		int percent = step == 255 ? 0 : 100 - step * 99 / 254; /* 100 down to 1, then the mute */
		level_db100 = -(long)step * 50;
		char want[VOLUMEREADOUT_TEXT_MAX];
		volumereadout_text(percent, want, sizeof(want));
		assert(strchr(want, 'V'));
		if (strcmp(want, previous) != 0) {
			distinct++;
		}
		snprintf(previous, sizeof(previous), "%s", want);

		volume_overlay_show(percent);
		check_pill(want);
		if (width < 0) {
			width = lv_obj_get_width(label);
			scale_length = lv_obj_get_width(scale);
		}
		assert(lv_obj_get_width(label) == width);
		assert(lv_obj_get_width(scale) == scale_length);
	}
	assert(distinct == 256); /* the walk really was over every reading */
	return width;
}

int main(void) {
	char dir[] = "/tmp/sonix-volume-overlay-XXXXXX";
	assert(mkdtemp(dir));
	char path[256];
	snprintf(path, sizeof(path), "%s/device_config.ini", dir);
	config_init(path);

	lv_init();
	font_ui_14 = font_ui_16 = lv_font_montserrat_14;
	font_ui_24 = lv_font_montserrat_16;
	lv_display_create(bp_screen_w(), bp_screen_h());
	theme_init();

	gui_config_t cfg = {bp_screen_w(), bp_screen_h(), bp_padding(), bp_status_bar_h(), dir};
	volume_overlay_init(&cfg);

	veil = lv_obj_get_child(lv_layer_top(), -1);
	assert(veil && lv_obj_get_child_count(veil) == 1);
	pill = lv_obj_get_child(veil, 0);
	assert(lv_obj_get_child_count(pill) == 3);
	glyph = lv_obj_get_child(pill, 0);
	label = lv_obj_get_child(pill, 1);
	scale = lv_obj_get_child(pill, 2);
	assert(lv_obj_check_type(glyph, &lv_image_class));
	assert(lv_obj_check_type(label, &lv_label_class));
	assert(lv_obj_check_type(scale, &lv_slider_class));
	/* One, and no more: fonts.c has eight places and drops the ninth silently. */
	assert(fonts_registrations == 1 && fonts_changed);

	/* Off, which is how a player that has never seen the setting comes up:
	 * the number, in the 28 px the V1's strip has always given it. */
	assert(!volumereadout_enabled());
	volume_overlay_show(100);
	check_pill("100");
	const int32_t number_width = lv_obj_get_width(label);
	const int32_t number_scale = lv_obj_get_width(scale);
	assert(number_width == 28);
	check_numbers(number_width, number_scale);

	/* On, the 3.5 mm jack: a wider label, the same for every reading. */
	volumereadout_set_enabled(true);
	assert(volumereadout_form() == VOLUMEREADOUT_FORM_VOLTS_PHONES);
	const int32_t phones_width = check_voltages();
	assert(phones_width > number_width);
	/* The scale gave up exactly what the label took, and is still most of itself. */
	assert(lv_obj_get_width(scale) == number_scale - (phones_width - number_width));
	assert(lv_obj_get_width(scale) * 2 > number_scale);

	/* The 4.4 mm one: twice the figures, measured for themselves. */
	volumereadout_set_jack(VOLUMEREADOUT_JACK_BALANCED);
	assert(volumereadout_form() == VOLUMEREADOUT_FORM_VOLTS_BALANCED);
	const int32_t balanced_width = check_voltages();
	assert(lv_obj_get_width(scale) == number_scale - (balanced_width - number_width));
	assert(lv_obj_get_width(scale) * 2 > number_scale);

	/* A finger on the scale goes through the same reading as the keys. */
	level_db100 = -2400;
	lv_slider_set_value(scale, 31, LV_ANIM_OFF);
	lv_obj_send_event(scale, LV_EVENT_VALUE_CHANGED, NULL);
	assert(level_index == 31);
	check_pill("252mV"); /* 4 V, 24 dB down */
	assert(lv_obj_get_width(label) == balanced_width);

	/* Over Bluetooth the headphones set the level: the number, and its width back. */
	level_output = VOLUME_OUTPUT_BLUETOOTH;
	check_numbers(number_width, number_scale);
	level_output = VOLUME_OUTPUT_PHONES;
	assert(check_voltages() == balanced_width);

	/* Another face or text size. fonts.c rewrites the lv_font_t in place,
	 * restyles every screen and then calls back; the label must be measured
	 * again, or the readings that fitted now wrap. */
	font_ui_16 = lv_font_montserrat_20;
	lv_obj_refresh_style(lv_layer_top(), LV_PART_ANY, LV_STYLE_PROP_ANY);
	fonts_changed();
	const int32_t large_width = check_voltages();
	assert(large_width > balanced_width);
	font_ui_16 = lv_font_montserrat_14;
	lv_obj_refresh_style(lv_layer_top(), LV_PART_ANY, LV_STYLE_PROP_ANY);
	fonts_changed();
	assert(check_voltages() == balanced_width);

	/* Off again: back where it started. */
	volumereadout_set_enabled(false);
	check_numbers(number_width, number_scale);

	unlink(path);
	rmdir(dir);
	printf("PASS volume overlay: the index at the built width; 256 voltage readings per jack on one line at "
	       "one label width (%d/%d px against %d, scale %d px of %d); Bluetooth, the slider and a font change "
	       "re-measured\n",
	       (int)phones_width, (int)balanced_width, (int)number_width,
	       (int)(number_scale - (balanced_width - number_width)), (int)number_scale);
	lv_deinit();
	return 0;
}
