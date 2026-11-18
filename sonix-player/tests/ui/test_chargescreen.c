/*
 * The battery screen a charger boot opens on, on the real renderer.
 *
 * What the screen is for is a decision the player makes before any service
 * exists (chargemode.h), and what it does with the power key is a loop that
 * needs a key (chargescreen.c). Neither is checked here. What is checked is the
 * part that only exists with a display: that the battery, the percentage, the
 * state line and the hold hint all sit inside the glass at both panel sizes,
 * that the fill is the reading it was given and nothing more -- 100% must not
 * paint past the box it is drawn in -- and that a gauge which said nothing is
 * shown as exactly that rather than as an empty battery.
 *
 * Real: LVGL and its software renderer, the screen as chargescreen_run() builds
 * it, the translations and the config file. Substituted: the gauge and the
 * cable, which are the two readings the loop feeds the screen, and the theme,
 * whose kind and palette the test hands the screen one case at a time.
 */
#include <assert.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "src/gui/board_profile.h"
#include "src/gui/fonts/fonts.h"
#include "src/gui/shell/chargescreen.h"
#include "src/gui/shell/gui.h"
#include "src/system/core/config.h"
#include "src/system/core/lang.h"
#include "src/system/device/system.h"
#include "src/gui/shell/theme.h"

/* ---- fonts: every face is LVGL's built-in one ---------------------------- */

lv_font_t font_ui_14, font_ui_16, font_ui_18, font_ui_20, font_ui_20_bold;
lv_font_t font_ui_22, font_ui_22_bold, font_ui_24, font_ui_24_bold;
lv_font_t font_ui_26, font_ui_28, font_ui_32, font_ui_36_bold;
lv_font_t font_ui_64_bold, font_ui_72;

static void init_fonts(void) {
	lv_font_t *all[] = {
		&font_ui_14, &font_ui_16, &font_ui_18, &font_ui_20, &font_ui_20_bold, &font_ui_22, &font_ui_22_bold,
		&font_ui_24, &font_ui_24_bold, &font_ui_26, &font_ui_28, &font_ui_32, &font_ui_36_bold,
		&font_ui_64_bold, &font_ui_72,
	};
	for (size_t i = 0; i < sizeof(all) / sizeof(all[0]); i++) {
		*all[i] = lv_font_montserrat_14;
	}
	// The percentage is the one line on the screen that has to be readable
	// across a room, and it is drawn at 36 px on both panels.
	font_ui_36_bold = lv_font_montserrat_36;
}

/* ---- the two readings the loop would supply ----------------------------- */

int gauge_percent = 42;
bool gauge_charging = true;
bool gauge_cable = true;
int gauge_reads;

bool battery_read_now(const battery_config_t *cfg, int *percent, bool *charging) {
	(void)cfg;
	gauge_reads++;
	if (percent) {
		*percent = gauge_percent;
	}
	if (charging) {
		*charging = gauge_charging;
	}
	return gauge_percent >= 0;
}

bool charger_attached(void) { return gauge_cable; }

/* ---- the theme, a stub the test drives --------------------------------------
 * Dark and Light draw the black screen they always had; the added palettes draw
 * from the active one (see theme.c). The stub hands the screen both answers:
 * which kind is in force -- flipped to Tokyo Night halfway down this file -- and
 * a palette to draw from. */
static theme_palette_t test_palette = {
	.screen_bg = LV_COLOR_MAKE(26, 27, 38),      // Tokyo Night #1a1b26
	.surface = LV_COLOR_MAKE(36, 40, 59),         // #24283b
	.surface_pressed = LV_COLOR_MAKE(45, 53, 84), // #2d3554
	.panel = LV_COLOR_MAKE(22, 22, 30),           // #16161e
	.cover_bg = LV_COLOR_MAKE(22, 22, 30),
	.text_primary = LV_COLOR_MAKE(192, 202, 245),   // #c0caf5
	.text_secondary = LV_COLOR_MAKE(169, 177, 214), // #a9b1d6
	.accent = LV_COLOR_MAKE(122, 162, 247),        // #7aa2f7
	.dark = true,
};
static theme_kind_t test_kind = THEME_KIND_DARK;

const theme_palette_t *theme(void) { return &test_palette; }
theme_kind_t theme_get_kind(void) { return test_kind; }

/* The panel's own wake bracket, which chargescreen_build() does not use but
 * chargescreen_run() does: the loop is linked whether or not it is called. */
void display_wake_begin(lv_display_t *disp) { (void)disp; }
void display_wake_end(lv_display_t *disp) { (void)disp; }

/* ---- small helpers ------------------------------------------------------- */

static lv_obj_t *find_label(lv_obj_t *obj, const char *text) {
	if (lv_obj_check_type(obj, &lv_label_class) && strcmp(lv_label_get_text(obj), text) == 0) {
		return obj;
	}
	for (uint32_t i = 0; i < lv_obj_get_child_count(obj); i++) {
		lv_obj_t *found = find_label(lv_obj_get_child(obj, i), text);
		if (found) {
			return found;
		}
	}
	return NULL;
}

/* Nothing drawn may leave the panel at all: this screen has no list to scroll
 * and nothing that hangs over an edge. */
static void check_within_panel(lv_obj_t *obj, gui_config_t *cfg, const char *what) {
	if (lv_obj_is_hidden(obj)) {
		return;
	}
	lv_area_t area;
	lv_obj_get_coords(obj, &area);
	if (area.x1 < 0 || area.y1 < 0 || area.x2 >= (int32_t)cfg->screen_width ||
		area.y2 >= (int32_t)cfg->screen_height) {
		fprintf(stderr, "%s: an object at %d,%d..%d,%d is outside the %dx%d panel\n", what, area.x1, area.y1,
				area.x2, area.y2, (int)cfg->screen_width, (int)cfg->screen_height);
		assert(!"an object left the panel");
	}
	for (uint32_t i = 0; i < lv_obj_get_child_count(obj); i++) {
		check_within_panel(lv_obj_get_child(obj, i), cfg, what);
	}
}

/* The battery's box and the fill inside it, by the order they are built in:
 * root -> row -> body -> fill (chargescreen_build). */
static lv_obj_t *battery_body(lv_obj_t *root) {
	lv_obj_t *row = lv_obj_get_child(root, 0);
	assert(row);
	lv_obj_t *body = lv_obj_get_child(row, 0);
	assert(body);
	return body;
}

static lv_obj_t *battery_fill(lv_obj_t *root) { return lv_obj_get_child(battery_body(root), 0); }

static void dump_snapshot(lv_obj_t *screen, const char *tag) {
	const char *prefix = getenv("SONIX_PAGE_SNAPSHOTS");
	if (!prefix || !prefix[0]) {
		return;
	}
	lv_obj_update_layout(screen);
	lv_draw_buf_t *shot = lv_snapshot_take(screen, LV_COLOR_FORMAT_RGB888);
	assert(shot);
	char path[512];
	snprintf(path, sizeof(path), "%s-%s.ppm", prefix, tag);
	FILE *file = fopen(path, "wb");
	assert(file);
	fprintf(file, "P6\n%u %u\n255\n", shot->header.w, shot->header.h);
	for (uint32_t y = 0; y < shot->header.h; y++) {
		const uint8_t *row = shot->data + y * shot->header.stride;
		for (uint32_t x = 0; x < shot->header.w; x++) {
			uint8_t rgb[] = {row[x * 3 + 2], row[x * 3 + 1], row[x * 3]};
			assert(fwrite(rgb, sizeof(rgb), 1, file) == 1);
		}
	}
	assert(fclose(file) == 0);
	lv_draw_buf_destroy(shot);
}

/* ---- the strings, in every language -------------------------------------- */

static bool is_ini(const char *name) {
	size_t n = strlen(name);
	return n > 4 && strcmp(name + n - 4, ".ini") == 0;
}

/* Every language has to carry the screen's three states and the two warnings
 * that go with them: tr() falls back to the key it is given, so a missing one
 * would put "chargescreen_charging" on the glass. */
static int check_strings(const char *language_dir, char names[][32], int max_names) {
	static const char *const KEYS[] = {
		"battery_low_warning",
		"chargescreen_charging",
		"chargescreen_connected",
		"chargescreen_unplugged",
		"chargescreen_hold_to_start",
	};
	DIR *dir = opendir(language_dir);
	assert(dir);
	int languages = 0;
	struct dirent *entry;
	while ((entry = readdir(dir)) != NULL) {
		if (!is_ini(entry->d_name)) {
			continue;
		}
		char name[256];
		snprintf(name, sizeof(name), "%.*s", (int)(strlen(entry->d_name) - 4), entry->d_name);
		assert(lang_set(name));
		if (languages < max_names) {
			snprintf(names[languages], 32, "%.31s", name);
		}
		for (size_t i = 0; i < sizeof(KEYS) / sizeof(KEYS[0]); i++) {
			const char *text = tr(KEYS[i]);
			if (!text || !text[0] || strcmp(text, KEYS[i]) == 0) {
				fprintf(stderr, "%s: \"%s\" is not translated\n", name, KEYS[i]);
				assert(!"a string is missing from a language file");
			}
			// Characters, not bytes: half these languages are not one byte
			// per letter. The lines wrap, so a long translation is a matter
			// of height rather than of width -- the layout check below is
			// what proves each one still fits.
			int chars = 0;
			for (const char *c = text; *c; c++) {
				if (((unsigned char)*c & 0xC0) != 0x80) {
					chars++;
				}
			}
			assert(chars < 96);
		}
		languages++;
	}
	closedir(dir);
	assert(languages == 11);
	assert(languages <= max_names);
	assert(lang_set("English"));
	return languages;
}

/* ---- the screen ---------------------------------------------------------- */

int main(int argc, char **argv) {
	assert(argc == 2);

	char card[] = "/tmp/sonix-chargescreen-test-XXXXXX";
	assert(mkdtemp(card));
	char config[512];
	snprintf(config, sizeof(config), "%s/device_config.ini", card);
	config_init(config);
	config_set("ui", "language", "English");
	lang_init(argv[1]);
	char names[16][32];
	int languages = check_strings(argv[1], names, 16);

	init_fonts();
	lv_init();
	gui_config_t cfg = {bp_screen_w(), bp_screen_h(), bp_padding(), bp_status_bar_h(), card};
	lv_display_t *disp = lv_display_create(cfg.screen_width, cfg.screen_height);

	chargescreen_build(disp);
	lv_obj_t *root = lv_obj_get_child(lv_screen_active(), 0);
	assert(root);
	// Dark in force: the screen is the black one it has always been -- palette
	// or not, that is the contract for the two default themes.
	assert(lv_color_eq(lv_obj_get_style_bg_color(root, 0), lv_color_black()));

	/* A charge in progress at 42%: the level as a number and as a bar, the
	 * state under it, and the gesture that leaves the screen. */
	chargescreen_show(true, 42, true, true);
	lv_obj_update_layout(lv_screen_active());
	check_within_panel(lv_screen_active(), &cfg, "Charging screen");
	dump_snapshot(lv_screen_active(), bp_is_tempotec_v1() ? "v1-charging" : "hiby-charging");

	assert(find_label(root, "42%"));
	assert(find_label(root, tr("chargescreen_charging")));
	assert(find_label(root, tr("chargescreen_hold_to_start")));
	assert(!find_label(root, "--"));

	lv_obj_t *body = battery_body(root);
	lv_obj_t *fill = battery_fill(root);
	int32_t inner = lv_obj_get_content_width(body);
	assert(inner > 0);
	int32_t at_42 = lv_obj_get_width(fill);
	assert(at_42 > 0);
	// Within a pixel of the ratio: the fill is the reading, not a decoration.
	assert(at_42 >= inner * 42 / 100 - 1 && at_42 <= inner * 42 / 100 + 1);

	/* More charge, a longer bar -- monotonically, and never past the box. */
	chargescreen_show(true, 80, true, true);
	lv_obj_update_layout(lv_screen_active());
	assert(find_label(root, "80%"));
	int32_t at_80 = lv_obj_get_width(fill);
	assert(at_80 > at_42);

	chargescreen_show(true, 100, true, true);
	lv_obj_update_layout(lv_screen_active());
	check_within_panel(lv_screen_active(), &cfg, "Charging screen, full");
	int32_t at_100 = lv_obj_get_width(fill);
	lv_area_t fill_area, body_area;
	lv_obj_get_coords(fill, &fill_area);
	lv_obj_get_coords(body, &body_area);
	assert(at_100 <= inner);
	assert(fill_area.x2 <= body_area.x2 - 1); // inside the border, not over it

	/* A full battery on the cable: the charger is still there, but nothing is
	 * going in, and the line has to say so rather than "charging". */
	chargescreen_show(true, 100, false, true);
	lv_obj_update_layout(lv_screen_active());
	assert(find_label(root, tr("chargescreen_connected")));
	assert(!find_label(root, tr("chargescreen_charging")));

	/* The cable taken out. */
	chargescreen_show(true, 96, false, false);
	lv_obj_update_layout(lv_screen_active());
	assert(find_label(root, tr("chargescreen_unplugged")));

	/* A gauge that answered nothing is not a flat battery, and must not be
	 * drawn as one: no number, no bar, and no "0%". */
	chargescreen_show(false, -1, false, false);
	lv_obj_update_layout(lv_screen_active());
	assert(find_label(root, "--"));
	assert(!find_label(root, "0%"));
	assert(lv_obj_get_width(fill) == 0);

	/* Every language, every state: the three lines wrap rather than run off
	 * the glass. The states are stepped through one at a time because the
	 * screen only repaints a line whose reading has changed -- which is also
	 * why a language change cannot happen under it on a device. */
	for (int i = 0; i < languages; i++) {
		assert(lang_set(names[i]));
		if (strcmp(names[i], "English") == 0) {
			continue; // checked above, line by line
		}
		chargescreen_show(false, -1, false, false);
		lv_obj_update_layout(lv_screen_active());
		check_within_panel(lv_screen_active(), &cfg, names[i]);
		chargescreen_show(true, 41, false, true);
		lv_obj_update_layout(lv_screen_active());
		check_within_panel(lv_screen_active(), &cfg, names[i]);
		chargescreen_show(true, 42, true, true);
		lv_obj_update_layout(lv_screen_active());
		check_within_panel(lv_screen_active(), &cfg, names[i]);
		assert(find_label(root, tr("chargescreen_charging")));
		assert(find_label(root, tr("chargescreen_hold_to_start")));
	}
	assert(lang_set("English"));

	/* Low charge on the cable: the bar goes red, and the line still says the
	 * charger is there -- this screen never tells anybody to plug in what is
	 * already plugged in. */
	chargescreen_show(true, 4, true, true);
	lv_obj_update_layout(lv_screen_active());
	assert(find_label(root, "4%"));
	assert(find_label(root, tr("chargescreen_charging")));
	/* The red rung, whatever the palette: it is captured here and asked of
	 * the themed pass below. */
	lv_color_t fill_low = lv_obj_get_style_bg_color(battery_fill(root), 0);
	check_within_panel(lv_screen_active(), &cfg, "Charging screen, low");
	dump_snapshot(lv_screen_active(), bp_is_tempotec_v1() ? "v1-charging-low" : "hiby-charging-low");

	/* --- Tokyo Night stands in for the added palettes -------------------------
	 * The screen comes down and is built again under the themed stub: same
	 * content, the palette's own colours -- and, above the low-battery rungs,
	 * a fill that wears the accent instead of the semantic green. */
	{
		lv_obj_delete(root);
		test_kind = THEME_KIND_TOKYO_NIGHT;
		chargescreen_build(NULL);
		root = lv_obj_get_child(lv_screen_active(), 0);
		assert(root);
		assert(lv_color_eq(lv_obj_get_style_bg_color(root, 0), test_palette.screen_bg));
		assert(lv_color_eq(lv_obj_get_style_border_color(battery_body(root), 0), test_palette.text_primary));
		lv_obj_t *tpct = find_label(root, "--");
		assert(tpct && lv_color_eq(lv_obj_get_style_text_color(tpct, 0), test_palette.text_primary));
		lv_obj_t *tstate = find_label(root, tr("chargescreen_charging"));
		assert(tstate && lv_color_eq(lv_obj_get_style_text_color(tstate, 0), test_palette.accent));
		lv_obj_t *thint = find_label(root, tr("chargescreen_hold_to_start"));
		assert(thint && lv_color_eq(lv_obj_get_style_text_color(thint, 0), test_palette.text_secondary));

		chargescreen_show(true, 50, true, true);
		lv_obj_update_layout(lv_screen_active());
		assert(find_label(root, "50%"));
		check_within_panel(lv_screen_active(), &cfg, "Charging screen, Tokyo Night");
		int32_t tinner = lv_obj_get_content_width(battery_body(root));
		int32_t at_50 = lv_obj_get_width(battery_fill(root));
		assert(at_50 >= tinner * 50 / 100 - 1 && at_50 <= tinner * 50 / 100 + 1);
		// Palette in force, the accent wears the bar: the top stop of the
		// gradient is the accent a fingerprint lighter (lv_color_mix towards
		// white, 70 of 255 -- see chargescreen.c), the bottom is the accent
		// itself.
		lv_color_t accent_hi = lv_color_mix(lv_color_white(), test_palette.accent, 70);
		assert(lv_color_eq(lv_obj_get_style_bg_color(battery_fill(root), 0), accent_hi));
		assert(lv_color_eq(lv_obj_get_style_bg_grad_color(battery_fill(root), 0), test_palette.accent));
		// The low rungs stay semantic there as well: red, not the accent.
		chargescreen_show(true, 4, true, true);
		lv_obj_update_layout(lv_screen_active());
		assert(lv_color_eq(lv_obj_get_style_bg_color(battery_fill(root), 0), fill_low));
		chargescreen_show(true, 50, true, true);
		lv_obj_update_layout(lv_screen_active());
		dump_snapshot(lv_screen_active(), bp_is_tempotec_v1() ? "v1-charging-tokyo" : "hiby-charging-tokyo");
		test_kind = THEME_KIND_DARK;
	}

	lv_deinit();
	printf("PASS charge screen (%s)\n", bp_is_tempotec_v1() ? "240x320" : "480x720");
	return 0;
}
