/*
 * The Stats page on the real V1 renderer.
 *
 * What is checked is the page's SHAPE, not its arithmetic: that the range pills
 * sit across the title row and the chosen one is the only one lit, that the two
 * summary cards share a row and neither runs off the panel, that the chart's
 * seven bars and the seven weekday letters under them line up, and that the two
 * top lists draw their rank, cover, name and plays inside a row of the height
 * the panel has for them. The numbers themselves are listeningstats.c's and are
 * covered by test_listeningstats.c against the real database.
 *
 * Real: LVGL and its software renderer, the shell's row and pill builders, the
 * theme, and the page's own code. The listening history is a stub that hands
 * back the same handful of rows every time, so a layout fault is never an
 * artefact of what happened to be in a database.
 */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "src/gui/board_profile.h"
#include "src/gui/fonts/fonts.h"
#include "src/gui/library/stats.h"
#include "src/gui/shell/gui.h"
#include "src/gui/shell/icons.h"
#include "src/gui/shell/settingsrow.h"
#include "src/gui/shell/switcher.h"
#include "src/gui/shell/theme.h"
#include "src/system/core/config.h"
#include "src/system/core/lang.h"
#include "src/system/library/listeningstats.h"

#define W 240
#define H 320

lv_font_t font_ui_14, font_ui_16, font_ui_18, font_ui_20, font_ui_20_bold;
lv_font_t font_ui_22, font_ui_22_bold, font_ui_24, font_ui_24_bold;
lv_font_t font_ui_26, font_ui_28, font_ui_32;

static uint32_t framebuffer[W * H];
static lv_point_t pointer_point;
static lv_indev_state_t pointer_state;
static void read_pointer(lv_indev_t *indev, lv_indev_data_t *data) {
	(void)indev;
	data->point = pointer_point;
	data->state = pointer_state;
}
static void pointer_at(lv_indev_t *indev, int x, int y, lv_indev_state_t state) {
	pointer_point.x = x;
	pointer_point.y = y;
	pointer_state = state;
	lv_tick_inc(20);
	lv_indev_read(indev);
}


// ---------------------------------------------------------------------------
// What the page asks the world for
// ---------------------------------------------------------------------------

// Set by the empty-list section at the end: a history with nothing in it, the
// state the "nothing played yet" line is the whole page for.
static bool lists_empty;
static int stub_month_days = 30;
static int stub_month_number = 11;
static int stub_today_day = 23;
static int stub_year_count = 3;
static bool stub_no_history;
static int locale_case;
static int stub_today = 20780; // Monday 2026-11-23

// The history the page is drawn from, held here so the test decides the shape
// of the answer: four artists, three records, all with a file behind them (so a
// cover is asked for) and all with plays.
static void stub_entry(stats_entry_t *out, const char *name, const char *value, int plays) {
	memset(out, 0, sizeof(*out));
	snprintf(out->name, sizeof(out->name), "%s", name);
	snprintf(out->value, sizeof(out->value), "%s", value);
	snprintf(out->artist, sizeof(out->artist), "%s", "An Artist");
	out->plays = plays;
}

void stats_daily(int days, stats_day_t *out, int count) {
	// The last day of the window is today; one day in the middle of the week
	// has an hour on it and one has a minute, so the chart has more than one
	// height of bar to lay out.
	for (int i = 0; i < count; i++) {
		out[i].day = stub_today - (days - 1) + i;
		out[i].seconds = 0;
		out[i].tracks = 0;
	}
	if (count >= 7) {
		out[0].seconds = 120;
		out[count - 1].seconds = 3600;
		out[count - 1].tracks = 12;
		out[count - 3].seconds = 60;
		out[count - 3].tracks = 1;
	}
}

int stats_calendar_month(stats_day_t out[STATS_CHART_MONTH_DAYS], int *month, int *year, int *today_day) {
	*month = stub_month_number;
	*year = 2026;
	*today_day = stub_today_day;
	for (int i = 0; i < stub_month_days; i++) {
		out[i].day = 20758 + i;
		out[i].seconds = i == 0 ? 120 : i == 22 ? 3600 : 0;
		out[i].tracks = 0;
	}
	return stub_month_days;
}

void stats_calendar_year(int64_t out[12], int *year) {
	*year = 2026;
	for (int i = 0; i < 12; i++) out[i] = 0;
	out[0] = 60;
	out[10] = 3600;
}

int64_t *stats_all_years(int *first_year, int *count) {
	*first_year = 2024;
	*count = stub_year_count;
	int64_t *values = calloc((size_t)*count, sizeof(*values));
	assert(values);
	if (!stub_no_history) {
		values[0] = 60;
		values[*count - 1] = 3600;
	}
	return values;
}

void stats_summary(stats_range_t range, stats_summary_t *out) {
	memset(out, 0, sizeof(*out));
	out->seconds = range == STATS_RANGE_WEEK ? 3660 : range == STATS_RANGE_ALL ? 200 * 3600 : 12 * 3600 + 34 * 60;
	out->plays = range == STATS_RANGE_WEEK ? 13 : range == STATS_RANGE_ALL ? 900 : 195;
	out->tracks = 4;
	out->albums = 3;
	out->artists = 4;
}

int stats_top(stats_top_t kind, stats_range_t range, int offset, int count, stats_entry_t *out) {
	(void)range;
	if (lists_empty) {
		return 0;
	}
	static const char *const ARTISTS[] = {"Slowspace", "Alyzea", "Nightjar", "Kessler"};
	static const char *const ALBUMS[] = {"Red", "Blue", "Green"};
	static const char *const FILES[] = {"/card/a/01.flac", "/card/b/01.flac", "/card/c/01.flac", "/card/d/01.flac"};
	int total = kind == STATS_TOP_ARTISTS ? 4 : kind == STATS_TOP_ALBUMS ? 3 : 4;
	int written = 0;
	for (int i = offset; i < total && written < count; i++, written++) {
		if (kind == STATS_TOP_ARTISTS) {
			stub_entry(&out[written], ARTISTS[i], FILES[i % 4], 31 - i * 3);
		} else if (kind == STATS_TOP_ALBUMS) {
			stub_entry(&out[written], ALBUMS[i], FILES[i % 4], 22 - i * 5);
		} else {
			stub_entry(&out[written], "A Track", FILES[i % 4], 9 - i);
		}
	}
	return written;
}

int stats_top_count(stats_top_t kind, stats_range_t range) {
	(void)range;
	if (lists_empty) {
		return 0;
	}
	return kind == STATS_TOP_ALBUMS ? 3 : 4;
}

void stats_format_duration(long seconds, char *out, size_t size) {
	snprintf(out, size, "%ldh %02ldm", seconds / 3600, (seconds % 3600) / 60);
}

// The cover loader is asked for a picture per row and never answers: the test
// is about where the picture would go.
void coverloader_request(int slot, const char *path, int size) {
	(void)slot;
	(void)path;
	(void)size;
}
bool coverloader_take(int slot, void *out, bool *finished) {
	(void)slot;
	(void)out;
	if (finished) {
		*finished = false;
	}
	return false;
}
void coverloader_release(int slot) { (void)slot; }

// Translation and the theme's two boot-screen questions: the page asks for
// words, and theme_init() asks for the boot picture it should remember.
// Every other string the page draws is a plain tag, and the tag is what the
// test looks for. Three of them come back as the English they stand for,
// because a tag would hide what the test is about: the two count lines are
// format strings and a tag there would swallow the number, and the empty-list
// note is only worth checking as a sentence: its length is the whole problem,
// and "stats_nothing_yet" fits on a line of its own.
const char *tr(const char *key) {
	if (locale_case) {
		if (strcmp(key, "stats_top_artists") == 0) return locale_case == 1 ? "Artistes les plus écoutés" : "Top-Interpreten";
		if (strcmp(key, "stats_top_albums") == 0) return locale_case == 1 ? "Albums les plus écoutés" : "Top-Alben";
		if (strcmp(key, "stats_see_all") == 0) return locale_case == 1 ? "Tout voir" : "Alle anzeigen";
		if (strcmp(key, "stats_week") == 0) return locale_case == 1 ? "Semaine" : "Woche";
		if (strcmp(key, "stats_month") == 0) return locale_case == 1 ? "Mois" : "Monat";
		if (strcmp(key, "stats_year") == 0) return locale_case == 1 ? "Année" : "Jahr";
		if (strcmp(key, "stats_all_time") == 0) return locale_case == 1 ? "Tout" : "Gesamt";
	}
	static const char *keys[] = {"stats_month_jan", "stats_month_feb", "stats_month_mar", "stats_month_apr",
		"stats_month_may", "stats_month_jun", "stats_month_jul", "stats_month_aug", "stats_month_sep",
		"stats_month_oct", "stats_month_nov", "stats_month_dec"};
	static const char *letters[] = {"J", "F", "M", "A", "M", "J", "J", "A", "S", "O", "N", "D"};
	for (int i = 0; i < 12; i++) if (strcmp(key, keys[i]) == 0) return letters[i];
	static const char *name_keys[] = {"stats_month_name_jan", "stats_month_name_feb", "stats_month_name_mar",
		"stats_month_name_apr", "stats_month_name_may", "stats_month_name_jun", "stats_month_name_jul",
		"stats_month_name_aug", "stats_month_name_sep", "stats_month_name_oct", "stats_month_name_nov",
		"stats_month_name_dec"};
	static const char *names[] = {"January", "February", "March", "April", "May", "June", "July", "August",
		"September", "October", "November", "December"};
	for (int i = 0; i < 12; i++) if (strcmp(key, name_keys[i]) == 0) return names[i];
	if (strcmp(key, "stats_plays") == 0) {
		return "%d plays";
	}
	if (strcmp(key, "stats_play_one") == 0) {
		return "%d play";
	}
	if (strcmp(key, "stats_nothing_yet") == 0) {
		return "Nothing played yet. Play something and come back.";
	}
	return key;
}
void bootlogo_set(int choice, bool dark) { (void)choice; (void)dark; }
bool bootpic_present(void) { return false; }

// The two glyphs the page's list rows draw before their covers arrive. The
// real ones are 30 MB of bitmaps (icons.c) that this test has no opinion
// about; what matters is that a row has an image in its cover slot.
static const uint8_t GLYPH_BYTES[4] = {0xff, 0xff, 0xff, 0xff};
lv_image_dsc_t icon_stats_artists = {.header = {.magic = LV_IMAGE_HEADER_MAGIC, .cf = LV_COLOR_FORMAT_A8,
													  .w = 2, .h = 2, .stride = 2},
										   .data = GLYPH_BYTES, .data_size = sizeof(GLYPH_BYTES)};
lv_image_dsc_t icon_stats_albums = {.header = {.magic = LV_IMAGE_HEADER_MAGIC, .cf = LV_COLOR_FORMAT_A8,
													 .w = 2, .h = 2, .stride = 2},
										  .data = GLYPH_BYTES, .data_size = sizeof(GLYPH_BYTES)};
lv_image_dsc_t icon_chevron_right = {.header = {.magic = LV_IMAGE_HEADER_MAGIC, .cf = LV_COLOR_FORMAT_A8,
													  .w = 2, .h = 2, .stride = 2},
										   .data = GLYPH_BYTES, .data_size = sizeof(GLYPH_BYTES)};

// The shell bits the page leans on.
void fonts_register_change(void (*cb)(void)) { (void)cb; }
int fonts_text_size(void) { return FONTS_TEXT_NORMAL; }
bool player_sheet_drag_active(void) { return false; }
bool switcher_back_drag_active(void) { return false; }
void switcher_attach_back_gesture(lv_obj_t *screen) { (void)screen; }
void switcher_mark_fresh_start(lv_obj_t *screen) { (void)screen; }
void switch_screen(lv_obj_t *target) {
	if (target) {
		lv_screen_load(target);
	}
}
bool medialist_album_view(void) { return true; }
bool medialist_go_to_current(void) { return false; }
bool medialist_quality_badges(void) { return false; }

// ---------------------------------------------------------------------------

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

static void flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map) {
	(void)area;
	(void)px_map;
	lv_display_flush_ready(disp);
}

// Deepest-bottom edge of any object in the tree, so a page that runs off the
// panel is caught however it is built.
static int deepest_bottom(lv_obj_t *obj) {
	int bottom = 0;
	lv_area_t a;
	uint32_t count = lv_obj_get_child_count(obj);
	for (uint32_t i = 0; i < count; i++) {
		lv_obj_t *child = lv_obj_get_child(obj, (int32_t)i);
		lv_obj_get_coords(child, &a);
		if (a.y2 > bottom) {
			bottom = a.y2;
		}
		int inner = deepest_bottom(child);
		if (inner > bottom) {
			bottom = inner;
		}
	}
	return bottom;
}

// Finds the first descendant label whose text is `text`.
static lv_obj_t *find_label(lv_obj_t *obj, const char *text) {
	uint32_t count = lv_obj_get_child_count(obj);
	for (uint32_t i = 0; i < count; i++) {
		lv_obj_t *child = lv_obj_get_child(obj, (int32_t)i);
		if (lv_obj_check_type(child, &lv_label_class) && strcmp(lv_label_get_text(child), text) == 0) {
			return child;
		}
		lv_obj_t *found = find_label(child, text);
		if (found) {
			return found;
		}
	}
	return NULL;
}

// Finds the first descendant label whose text begins with `prefix`. Rows
// ellipsize their names, so "Slowspace" may be drawn as "Slowsp...".
// A row's name is drawn with LV_LABEL_LONG_DOT, so a long one comes back as
// "Slowsp...". Compare what is there, minus the dots, against the start of the
// name.
static bool label_starts_with(const char *text, const char *prefix) {
	size_t available = strlen(text);
	size_t wanted = strlen(prefix);
	if (available >= 3 && strcmp(text + available - 3, "...") == 0) {
		available -= 3;
	}
	if (available > wanted) {
		available = wanted;
	}
	return available >= (wanted < 3 ? wanted : 3) && strncmp(text, prefix, available) == 0;
}

static lv_obj_t *find_label_prefix(lv_obj_t *obj, const char *prefix) {
	uint32_t count = lv_obj_get_child_count(obj);
	for (uint32_t i = 0; i < count; i++) {
		lv_obj_t *child = lv_obj_get_child(obj, (int32_t)i);
		if (lv_obj_check_type(child, &lv_label_class) && label_starts_with(lv_label_get_text(child), prefix)) {
			return child;
		}
		lv_obj_t *found = find_label_prefix(child, prefix);
		if (found) {
			return found;
		}
	}
	return NULL;
}

static void dump_snapshot(lv_obj_t *screen, const char *tag) {
	const char *prefix = getenv("SONIX_STATS_SNAPSHOTS");
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

int main(void) {
	char dir[] = "/tmp/sonix-stats-ui-XXXXXX";
	assert(mkdtemp(dir));
	char path[256];
	snprintf(path, sizeof(path), "%s/device_config.ini", dir);
	config_init(path);

	init_fonts();
	theme_init();

	lv_init();
	lv_display_t *disp = lv_display_create(W, H);
	lv_display_set_color_format(disp, LV_COLOR_FORMAT_XRGB8888);
	lv_display_set_buffers(disp, framebuffer, NULL, sizeof(framebuffer), LV_DISPLAY_RENDER_MODE_DIRECT);
	lv_display_set_flush_cb(disp, flush_cb);
	lv_tick_inc(10);

	gui_config_t cfg = {
		.screen_width = W,
		.screen_height = H,
		.padding = 6,
		.top_bar_height = 24,
		.sd_root_path = dir,
	};

	// The whole shell the page expects to be built on, minus the rest of the
	// player: a title row, a swiped-back screen, and nothing else.
	stats_init(&cfg);
	assert(stats_screen);
	assert(stats_artists_screen);
	assert(stats_albums_screen);

	lv_screen_load(stats_screen);
	lv_obj_update_layout(stats_screen);

	dump_snapshot(stats_screen, "all");

	// --- the title and the pills ------------------------------------------------
	lv_obj_t *title = settingsrow_page_title(stats_screen);
	assert(title);
	assert(strcmp(lv_label_get_text(title), "stats") == 0);
	// The heading clears the strip of pills that follows it.
	lv_area_t title_area;
	lv_obj_get_coords(title, &title_area);
	assert(title_area.x2 < W);

	// The three range pills, in the order the pill row was built, on one row.
	lv_obj_t *week = find_label(stats_screen, "stats_week");
	lv_obj_t *month = find_label(stats_screen, "stats_month");
	lv_obj_t *year = find_label(stats_screen, "stats_year");
	lv_obj_t *all = find_label(stats_screen, "stats_all_time");
	assert(week && month && year && all);
	// Four translated labels may wrap to a second row on the V1. Every pill
	// must remain within the row, readable and separate from its neighbours.
	lv_obj_t *pill_labels[] = {week, month, year, all};
	for (int i = 0; i < 4; i++) {
		lv_area_t a;
		lv_obj_get_coords(lv_obj_get_parent(pill_labels[i]), &a);
		assert(a.x1 >= 0 && a.x2 < W);
		for (int j = 0; j < i; j++) {
			lv_area_t b;
			lv_obj_get_coords(lv_obj_get_parent(pill_labels[j]), &b);
			assert(a.x1 > b.x2 || a.x2 < b.x1 || a.y1 > b.y2 || a.y2 < b.y1);
		}
	}

	lv_obj_send_event(lv_obj_get_parent(year), LV_EVENT_CLICKED, NULL);
	lv_obj_update_layout(stats_screen);

	// --- the summary cards ------------------------------------------------------
	lv_obj_t *listening = find_label(stats_screen, "stats_listening_time");
	lv_obj_t *tracks = find_label(stats_screen, "stats_tracks_played");
	assert(listening && tracks);
	lv_area_t la, ta;
	lv_obj_get_coords(listening, &la);
	lv_obj_get_coords(tracks, &ta);
	// Side by side, the same row, the label above its own figure.
	assert(la.x1 < ta.x1);
	assert(la.y1 == ta.y1);
	// The figure under the first caption is the formatted listening time; the
	// one under the second is the play count. Year is the stub's twelve and
	// a half hours and 195 plays.
	lv_obj_t *time_value = find_label(stats_screen, "12h 34m");
	lv_obj_t *plays_value = find_label(stats_screen, "195");
	assert(time_value);
	assert(plays_value);
	lv_area_t tva, pva;
	lv_obj_get_coords(time_value, &tva);
	lv_obj_get_coords(plays_value, &pva);
	assert(tva.y1 > la.y1 && tva.x1 >= la.x1 - 4 && tva.x1 < ta.x1);
	assert(pva.y1 > ta.y1 && pva.x1 >= ta.x1 - 4);

	// --- the chart --------------------------------------------------------------
	lv_obj_t *chart_title = find_label(stats_screen, "2026");
	assert(chart_title);
	lv_obj_t *chart_card = lv_obj_get_parent(chart_title);
	lv_obj_t *viewport = lv_obj_get_child(chart_card, 1);
	lv_obj_t *content = lv_obj_get_child(viewport, 0);
	lv_obj_t *chart = lv_obj_get_child(content, 0);
	lv_obj_t *letters_row = lv_obj_get_child(content, 1);
	assert(lv_obj_check_type(chart, &lv_chart_class));
	assert(lv_chart_get_point_count(chart) == 12);
	assert(!lv_obj_is_scrollable(viewport));
	lv_chart_series_t *series = lv_chart_get_series_next(chart, NULL);
	int32_t *values = lv_chart_get_series_y_array(chart, series);
	assert(values[0] == 1 && values[10] == 60 && values[11] == 0);
	static const char *months[] = {"J", "F", "M", "A", "M", "J", "J", "A", "S", "O", "N", "D"};
	assert(lv_obj_get_child_count(letters_row) == 12);
	for (int i = 0; i < 12; i++) {
		lv_obj_t *mark = lv_obj_get_child(letters_row, i);
		assert(strcmp(lv_label_get_text(mark), months[i]) == 0);
		lv_area_t area;
		lv_obj_get_coords(mark, &area);
		assert(area.x1 >= 0 && area.x2 < W);
	}

	lv_obj_send_event(lv_obj_get_parent(week), LV_EVENT_CLICKED, NULL);
	lv_obj_update_layout(stats_screen);
	assert(lv_chart_get_point_count(chart) == 7);
	assert(!lv_obj_is_scrollable(viewport));
	values = lv_chart_get_series_y_array(chart, series);
	// Monday is today, so the newest bar belongs under Monday, not Sunday.
	assert(values[0] == 60 && values[1] == 2 && values[5] == 1);
	chart_title = find_label(stats_screen, "stats_last_7_days");
	assert(chart_title);
	// Seven weekday letters under the chart, one per bucket: read straight off
	// the row's children, because several buckets legitimately end on the same
	// weekday and a search by text would find the first "S" seven times.
	lv_obj_t *marks_row = lv_obj_get_parent(chart_title); // the chart card
	(void)marks_row;
	lv_obj_t *first_letter = find_label(stats_screen, "stats_day_sun");
	assert(first_letter);
	assert(letters_row == lv_obj_get_parent(first_letter));
	assert(letters_row);
	assert(lv_obj_get_child_count(letters_row) == 7);
	lv_area_t prev = {0};
	for (int i = 0; i < 7; i++) {
		lv_obj_t *letter = lv_obj_get_child(letters_row, i);
		assert(lv_obj_check_type(letter, &lv_label_class));
		// Every label is in Monday-to-Sunday order.
		static const char *const expected[] = {"stats_day_mon", "stats_day_tue", "stats_day_wed", "stats_day_thu",
			"stats_day_fri", "stats_day_sat", "stats_day_sun"};
		assert(strcmp(lv_label_get_text(letter), expected[i]) == 0);
		// Every one of them is one of the weekday tags.
		const char *text = lv_label_get_text(letter);
		assert(strncmp(text, "stats_day_", 10) == 0);
		lv_area_t a;
		lv_obj_get_coords(letter, &a);
		if (i > 0) {
			// Left to right, on one line, each a slot of its own.
			assert(a.x1 >= prev.x2);
			assert(a.y1 == prev.y1);
			int w = a.x2 - a.x1 + 1;
			int pw = prev.x2 - prev.x1 + 1;
			assert(w - pw >= -1 && w - pw <= 1); // equal widths (within 1 px rounding)
		}
		assert(a.x2 <= W);
		// Inside the card that holds them.
		lv_area_t card_area;
		lv_obj_get_coords(letters_row, &card_area);
		assert(a.x1 >= card_area.x1 - 1 && a.x2 <= card_area.x2 + 1);
		prev = a;
	}

	lv_obj_send_event(lv_obj_get_parent(month), LV_EVENT_CLICKED, NULL);
	lv_obj_update_layout(stats_screen);
	assert(strcmp(lv_label_get_text(chart_title), "November") == 0);
	static const char *month_names[] = {"January", "February", "March", "April", "May", "June", "July", "August",
		"September", "October", "November", "December"};
	for (int i = 0; i < 12; i++) {
		stub_month_number = i + 1;
		stats_refresh();
		assert(strcmp(lv_label_get_text(chart_title), month_names[i]) == 0);
	}
	stub_month_number = 11;
	stats_refresh();
	lv_obj_update_layout(stats_screen);
	assert(lv_chart_get_point_count(chart) == 30);
	values = lv_chart_get_series_y_array(chart, series);
	assert(values[0] == 2 && values[22] == 60 && values[29] == 0);
	assert(lv_obj_is_scrollable(viewport));
	assert(lv_obj_get_scroll_dir(viewport) == LV_DIR_HOR);
	assert(lv_obj_get_scroll_right(viewport) > 0);
	// Drag the actual LVGL pointer across the visible chart, in both
	// directions, and verify a vertical swipe still scrolls the page.
	lv_obj_t *page = lv_obj_get_parent(chart_card);
	lv_obj_scroll_to_view(chart_card, LV_ANIM_OFF);
	lv_obj_update_layout(stats_screen);
	lv_area_t touch_area;
	lv_obj_get_coords(viewport, &touch_area);
	int tx = (touch_area.x1 + touch_area.x2) / 2;
	int ty = (touch_area.y1 + touch_area.y2) / 2;
	assert(ty >= 0 && ty < H);
	lv_indev_t *pointer = lv_indev_create();
	lv_indev_set_type(pointer, LV_INDEV_TYPE_POINTER);
	lv_indev_set_read_cb(pointer, read_pointer);
	pointer_at(pointer, tx + 45, ty, LV_INDEV_STATE_PRESSED);
	pointer_at(pointer, tx + 10, ty, LV_INDEV_STATE_PRESSED);
	pointer_at(pointer, tx - 40, ty, LV_INDEV_STATE_PRESSED);
	assert(lv_indev_get_scroll_obj(pointer) == viewport);
	int scroll_after_left = lv_obj_get_scroll_x(viewport);
	assert(scroll_after_left > 0);
	pointer_at(pointer, tx - 40, ty, LV_INDEV_STATE_RELEASED);
	// Avoid inertial motion while checking the reverse drag.
	lv_obj_set_scroll_momentum(viewport, false);
	pointer_at(pointer, tx - 40, ty, LV_INDEV_STATE_PRESSED);
	pointer_at(pointer, tx, ty, LV_INDEV_STATE_PRESSED);
	pointer_at(pointer, tx + 40, ty, LV_INDEV_STATE_PRESSED);
	assert(lv_obj_get_scroll_x(viewport) < scroll_after_left);
	pointer_at(pointer, tx + 40, ty, LV_INDEV_STATE_RELEASED);
	lv_obj_set_scroll_momentum(viewport, true);
	int page_before = lv_obj_get_scroll_y(page);
	pointer_at(pointer, tx, ty + 25, LV_INDEV_STATE_PRESSED);
	pointer_at(pointer, tx, ty, LV_INDEV_STATE_PRESSED);
	pointer_at(pointer, tx, ty - 25, LV_INDEV_STATE_PRESSED);
	assert(lv_indev_get_scroll_obj(pointer) == page);
	assert(lv_obj_get_scroll_y(page) > page_before);
	pointer_at(pointer, tx, ty - 25, LV_INDEV_STATE_RELEASED);
	lv_indev_delete(pointer);
	lv_obj_scroll_to_y(page, 0, LV_ANIM_OFF);
	lv_obj_scroll_to_x(viewport, 0, LV_ANIM_OFF);
	lv_obj_update_layout(stats_screen);
	assert(lv_obj_get_child_count(letters_row) == 30);
	for (int i = 0; i < 30; i++) {
		lv_color_t actual = lv_obj_get_style_text_color(lv_obj_get_child(letters_row, i), 0);
		assert(lv_color_eq(actual, i == 22 ? theme()->accent : theme()->text_secondary));
	}
	stub_today_day = 1;
	stats_refresh();
	assert(lv_color_eq(lv_obj_get_style_text_color(lv_obj_get_child(letters_row, 0), 0), theme()->accent));
	assert(lv_color_eq(lv_obj_get_style_text_color(lv_obj_get_child(letters_row, 22), 0), theme()->text_secondary));
	stub_today_day = 23;
	stats_refresh();
	lv_obj_update_layout(stats_screen);
	for (int i = 0; i < 30; i++) {
		char expected[4];
		snprintf(expected, sizeof(expected), "%d", i + 1);
		lv_obj_t *label = lv_obj_get_child(letters_row, i);
		assert(strcmp(lv_label_get_text(label), expected) == 0);
		lv_area_t area;
		lv_obj_get_coords(label, &area);
		assert(area.x2 - area.x1 + 1 >= 24);
	}
	// Scroll to both edges; bars and labels share the same moving content.
	lv_obj_scroll_to_x(viewport, lv_obj_get_scroll_right(viewport), LV_ANIM_OFF);
	lv_obj_update_layout(stats_screen);
	assert(lv_obj_get_scroll_x(viewport) > 0);
	assert(lv_obj_get_scroll_right(viewport) == 0);
	lv_area_t last_area, viewport_area;
	lv_obj_get_coords(lv_obj_get_child(letters_row, 29), &last_area);
	lv_obj_get_coords(viewport, &viewport_area);
	assert(last_area.x1 >= viewport_area.x1 && last_area.x2 <= viewport_area.x2);
	lv_obj_scroll_to_x(viewport, 0, LV_ANIM_OFF);
	assert(lv_obj_get_scroll_x(viewport) == 0);
	stub_month_days = 31;
	stats_refresh();
	lv_obj_update_layout(stats_screen);
	assert(lv_chart_get_point_count(chart) == 31);
	assert(strcmp(lv_label_get_text(lv_obj_get_child(letters_row, 30)), "31") == 0);
	stub_month_days = 28;
	stats_refresh();
	assert(lv_chart_get_point_count(chart) == 28);
	stub_month_days = 29;
	stats_refresh();
	assert(lv_chart_get_point_count(chart) == 29);
	lv_obj_send_event(lv_obj_get_parent(year), LV_EVENT_CLICKED, NULL);
	lv_obj_update_layout(stats_screen);
	assert(lv_chart_get_point_count(chart) == 12);
	assert(!lv_obj_is_scrollable(viewport));
	assert(lv_obj_get_scroll_x(viewport) == 0);
	values = lv_chart_get_series_y_array(chart, series);
	assert(values[0] == 1 && values[10] == 60 && values[11] == 0);
	lv_draw_buf_t *year_shot = lv_snapshot_take(chart, LV_COLOR_FORMAT_RGB888);
	assert(year_shot);
	lv_draw_buf_destroy(year_shot);

	lv_obj_send_event(lv_obj_get_parent(all), LV_EVENT_CLICKED, NULL);
	lv_obj_update_layout(stats_screen);
	assert(strcmp(lv_label_get_text(chart_title), "stats_all_time") == 0);
	assert(find_label(stats_screen, "200h 00m") && find_label(stats_screen, "900"));
	assert(lv_chart_get_point_count(chart) == 3);
	assert(!lv_obj_is_scrollable(viewport));
	values = lv_chart_get_series_y_array(chart, series);
	assert(values[0] == 1 && values[1] == 0 && values[2] == 60);
	assert(strcmp(lv_label_get_text(lv_obj_get_child(letters_row, 0)), "2024") == 0);
	assert(strcmp(lv_label_get_text(lv_obj_get_child(letters_row, 2)), "2026") == 0);
	stub_year_count = 6;
	stats_refresh();
	lv_obj_update_layout(stats_screen);
	assert(lv_chart_get_point_count(chart) == 6 && lv_obj_get_child_count(letters_row) == 2);
	assert(strcmp(lv_label_get_text(lv_obj_get_child(letters_row, 1)), "2029") == 0);
	values = lv_chart_get_series_y_array(chart, series);
	assert(values[0] == 1 && values[5] == 60);
	stub_year_count = 1;
	stats_refresh();
	lv_obj_update_layout(stats_screen);
	assert(lv_chart_get_point_count(chart) == 1 && lv_obj_get_child_count(letters_row) == 1);
	// One year is a compact centered bar with room for its full label. In
	// particular it must not fill the entire card like the original bug.
	lv_area_t chart_area, content_area, view_area, mark_area;
	lv_obj_get_coords(chart, &chart_area);
	lv_obj_get_coords(content, &content_area);
	lv_obj_get_coords(viewport, &view_area);
	lv_obj_get_coords(lv_obj_get_child(letters_row, 0), &mark_area);
	assert(lv_obj_get_width(chart) == 32);
	assert(lv_obj_get_width(content) == 44);
	assert(lv_obj_get_width(chart) < lv_obj_get_width(viewport) / 3);
	assert(abs((chart_area.x1 + chart_area.x2) - (view_area.x1 + view_area.x2)) <= 2);
	assert(abs((chart_area.x1 + chart_area.x2) - (mark_area.x1 + mark_area.x2)) <= 2);
	assert(mark_area.x1 == content_area.x1 && mark_area.x2 == content_area.x2);
	assert(strcmp(lv_label_get_text(lv_obj_get_child(letters_row, 0)), "2024") == 0);
	// The actual LVGL-rendered pixels show a bar with headroom, not a solid
	// block at the chart's top. The snapshot is RGB888 in BGR byte order.
	lv_draw_buf_t *shot = lv_snapshot_take(chart, LV_COLOR_FORMAT_RGB888);
	assert(shot);
	lv_color_t accent = theme()->accent;
	int colored = 0, first_y = (int)shot->header.h;
	for (uint32_t y = 0; y < shot->header.h; y++) {
		const uint8_t *row = shot->data + y * shot->header.stride;
		for (uint32_t x = 0; x < shot->header.w; x++) {
			const uint8_t *pixel = row + 3 * x;
			if (pixel[0] == accent.blue && pixel[1] == accent.green && pixel[2] == accent.red) {
				colored++;
				if ((int)y < first_y) first_y = (int)y;
			}
		}
	}
	assert(colored > 0 && first_y >= 12 && first_y < (int)shot->header.h / 2);
	lv_draw_buf_destroy(shot);
	stub_no_history = true;
	stats_refresh();
	assert(lv_chart_get_series_y_array(chart, series)[0] == 0);
	shot = lv_snapshot_take(chart, LV_COLOR_FORMAT_RGB888);
	assert(shot);
	colored = 0;
	for (uint32_t y = 0; y < shot->header.h; y++) {
		const uint8_t *row = shot->data + y * shot->header.stride;
		for (uint32_t x = 0; x < shot->header.w; x++) {
			const uint8_t *pixel = row + 3 * x;
			if (pixel[0] == accent.blue && pixel[1] == accent.green && pixel[2] == accent.red) colored++;
		}
	}
	assert(colored == 0);
	lv_draw_buf_destroy(shot);
	stub_no_history = false;
	// Several years have one centered label beneath each separated bar.
	for (int count = 2; count <= 4; count++) {
		stub_year_count = count;
		stats_refresh();
		lv_obj_update_layout(stats_screen);
		assert(lv_obj_get_width(content) == count * 44);
		assert(lv_chart_get_point_count(chart) == (uint32_t)count);
		assert(lv_obj_get_child_count(letters_row) == (uint32_t)count);
		lv_obj_get_coords(chart, &chart_area);
		for (int i = 0; i < count; i++) {
			lv_point_t p;
			lv_chart_get_point_pos_by_id(chart, series, (uint32_t)i, &p);
			lv_obj_get_coords(lv_obj_get_child(letters_row, i), &mark_area);
			assert(abs(2 * (chart_area.x1 + p.x) - (mark_area.x1 + mark_area.x2)) <= 3);
		}
	}
	stub_year_count = 6;
	stats_refresh();
	lv_obj_update_layout(stats_screen);
	assert(lv_obj_get_width(content) == lv_obj_get_content_width(viewport));
	assert(lv_obj_get_style_pad_column(chart, LV_PART_MAIN) == 1);
	assert(lv_obj_get_style_pad_column(letters_row, 0) == 0);
	assert(!lv_obj_is_scrollable(viewport));
	// Switching to month restores left alignment and its horizontal scroll.
	lv_obj_send_event(lv_obj_get_parent(month), LV_EVENT_CLICKED, NULL);
	lv_obj_update_layout(stats_screen);
	lv_obj_get_coords(viewport, &view_area);
	lv_obj_get_coords(content, &content_area);
	assert(content_area.x1 == view_area.x1);
	assert(lv_obj_get_width(content) == 29 * 24);
	assert(lv_obj_is_scrollable(viewport) && lv_obj_get_scroll_right(viewport) > 0);
	lv_obj_scroll_to_x(viewport, lv_obj_get_scroll_right(viewport), LV_ANIM_OFF);
	stub_year_count = 3;
	lv_obj_send_event(lv_obj_get_parent(year), LV_EVENT_CLICKED, NULL);
	lv_obj_update_layout(stats_screen);
	assert(lv_obj_get_scroll_x(viewport) == 0 && !lv_obj_is_scrollable(viewport));
	assert(lv_obj_get_width(content) == lv_obj_get_content_width(viewport));

	// --- the two top lists ------------------------------------------------------
	lv_obj_t *top_artists = find_label(stats_screen, "stats_top_artists");
	lv_obj_t *top_albums = find_label(stats_screen, "stats_top_albums");
	assert(top_artists && top_albums);
	// Each card's heading carries a "See all" of its own, to the right of the
	// title and on the same line.
	lv_obj_t *see_all = find_label(lv_obj_get_parent(top_artists), "stats_see_all");
	assert(see_all);
	lv_area_t sa, ha;
	lv_obj_get_coords(see_all, &sa);
	lv_obj_get_coords(top_artists, &ha);
	lv_area_t button_area;
	lv_obj_get_coords(lv_obj_get_parent(see_all), &button_area);
	assert(ha.x2 < button_area.x1 && sa.x2 <= W);
	assert(ha.y1 <= button_area.y2 && button_area.y1 <= ha.y2);
	// The rows themselves: two per card, in the order the stub gave them.
	lv_obj_t *first_artist = find_label_prefix(stats_screen, "Slowspace");
	lv_obj_t *second_artist = find_label_prefix(stats_screen, "Alyzea");
	lv_obj_t *third_artist = find_label_prefix(stats_screen, "Nightjar");
	assert(first_artist && second_artist);
	// The card shows two and no more.
	assert(!third_artist);

	lv_obj_t *first_album = find_label_prefix(stats_screen, "Red");
	lv_obj_t *second_album = find_label_prefix(stats_screen, "Blue");
	lv_obj_t *third_album = find_label_prefix(stats_screen, "Green");
	assert(first_album && second_album);
	assert(!third_album);

	// The two cards are stacked, artists above albums, and each row is below its
	// heading and inside the panel.
	lv_area_t a1, a2, b1;
	lv_obj_get_coords(top_artists, &a1);
	lv_obj_get_coords(first_artist, &a2);
	lv_obj_get_coords(top_albums, &b1);
	assert(a2.y1 > a1.y1);
	assert(b1.y1 > a2.y1);
	assert(a2.x2 <= W);

	// The ranks and the play counts are on the row: "1" beside the first name
	// horizontally, and the plays at the right edge of the same row.
	lv_obj_t *rank = find_label(lv_obj_get_parent(first_artist), "1");
	assert(rank);
	lv_area_t ra, na;
	lv_obj_get_coords(rank, &ra);
	lv_obj_get_coords(first_artist, &na);
	assert(ra.x1 < na.x1);
	lv_obj_t *plays = find_label(lv_obj_get_parent(first_artist), "31 plays");
	assert(plays);
	lv_area_t pa;
	lv_obj_get_coords(plays, &pa);
	assert(pa.x1 > na.x1);
	assert(pa.x2 <= W);

	// --- the whole page ---------------------------------------------------------
	// Everything on it is inside the 240 px panel, and the page is a scrolling
	// column: its content runs past the bottom of the viewport rather than
	// being clipped flat.
	lv_obj_t *container = lv_obj_get_child(stats_screen, 1);
	assert(container);
	int content_bottom = deepest_bottom(container);
	assert(content_bottom > H);
	lv_area_t ca;
	lv_obj_get_coords(container, &ca);
	assert(ca.x2 < W);
	assert(ca.y2 <= H);

	// --- the range pills actually change the page -------------------------------
	// "Week" is tapped: the same two cards now carry the stub's week, an hour
	// and a minute and thirteen plays, and nothing of the all-time figures is
	// left on them.
	lv_obj_t *week_pill = lv_obj_get_parent(week);
	lv_obj_send_event(week_pill, LV_EVENT_CLICKED, NULL);
	lv_obj_update_layout(stats_screen);
	lv_obj_t *week_time = find_label(stats_screen, "1h 01m");
	assert(week_time);
	lv_obj_t *week_plays = find_label(stats_screen, "13");
	assert(week_plays);
	assert(!find_label(stats_screen, "12h 34m"));
	assert(!find_label(stats_screen, "195"));
	// A Sunday and a Tuesday still map every last-seven-days value to its
	// weekday, including across the preceding week.
	stub_today = 20779; // Sunday
	stats_refresh();
	values = lv_chart_get_series_y_array(chart, series);
	assert(values[0] == 2 && values[4] == 1 && values[6] == 60);
	stub_today = 20781; // Tuesday
	stats_refresh();
	values = lv_chart_get_series_y_array(chart, series);
	assert(values[1] == 60 && values[2] == 2 && values[6] == 1);
	stub_today = 20780;


	// --- the See all pages ------------------------------------------------------
	// The artist list is a screen of its own, headed like the page it came from,
	// and it holds the whole list rather than the two the card shows.
	lv_screen_load(stats_artists_screen);
	lv_obj_update_layout(stats_artists_screen);
	assert(settingsrow_page_title(stats_artists_screen));
	dump_snapshot(stats_artists_screen, "artists");
	lv_obj_t *all_four = find_label_prefix(stats_artists_screen, "Kessler");
	assert(all_four); // the fourth artist, which the card did not have room for
	lv_screen_load(stats_albums_screen);
	lv_obj_update_layout(stats_albums_screen);
	lv_obj_t *all_three = find_label_prefix(stats_albums_screen, "Green");
	assert(all_three);

	// --- the page with nothing on it --------------------------------------------
	// Neither list has a row, so the note saying so is all either page has to
	// say -- and it is a sentence longer than the panel. A label measured at its
	// natural width is one unbroken line, and in a centred column that line
	// overhangs the panel on both sides at once: the first words and the last
	// ones both disappear, which is what the empty Top artists and Top albums
	// pages showed. Wrapped into the page it stays legible end to end, on as
	// many lines as the language needs.
	//
	// The English is spelled out rather than looked up by its tag for the same
	// reason the plays lines are: the tag is shorter than the panel, so a page
	// that cut the sentence off would still pass a test that only knew the tag.
	static const char *const NOTHING_YET = "Nothing played yet. Play something and come back.";

	lists_empty = true;
	lv_obj_send_event(lv_obj_get_parent(see_all), LV_EVENT_CLICKED, NULL); // the artists card's "See all"
	lv_obj_update_layout(stats_artists_screen);
	lv_obj_t *note = find_label(stats_artists_screen, NOTHING_YET);
	assert(note);
	assert(!lv_obj_is_hidden(note));
	assert(!find_label_prefix(stats_artists_screen, "Slowspace")); // the list itself stayed empty
	lv_area_t note_area;
	lv_obj_get_coords(note, &note_area);
	// All of it inside the panel, on both edges and at the bottom.
	assert(note_area.x1 >= 0 && note_area.x2 < W);
	assert(note_area.y1 >= 0 && note_area.y2 < H);
	// And over more than one line, which is how it fits: a single line of that
	// sentence is exactly what used to hang over both edges.
	assert(note_area.y2 - note_area.y1 + 1 > lv_font_get_line_height(&font_ui_18) + 2);

	// The albums page is this page built a second time, so it is read the same
	// way.
	lv_obj_t *album_see_all = find_label(lv_obj_get_parent(top_albums), "stats_see_all");
	assert(album_see_all);
	lv_obj_send_event(lv_obj_get_parent(album_see_all), LV_EVENT_CLICKED, NULL);
	lv_obj_update_layout(stats_albums_screen);
	lv_obj_t *album_note = find_label(stats_albums_screen, NOTHING_YET);
	assert(album_note);
	assert(!lv_obj_is_hidden(album_note));
	lv_area_t album_note_area;
	lv_obj_get_coords(album_note, &album_note_area);
	assert(album_note_area.x1 >= 0 && album_note_area.x2 < W);
	assert(album_note_area.y2 - album_note_area.y1 + 1 > lv_font_get_line_height(&font_ui_18) + 2);

	// And the note steps aside as soon as there is something to show, since the
	// page is refilled on every entry rather than rebuilt.
	lists_empty = false;
	lv_obj_send_event(lv_obj_get_parent(see_all), LV_EVENT_CLICKED, NULL);
	lv_obj_update_layout(stats_artists_screen);
	assert(lv_obj_is_hidden(note));
	assert(find_label_prefix(stats_artists_screen, "Slowspace"));

	// Real long French/German titles and the actual See-all button share
	// the strip without overlap, also with larger V1 font metrics.
	font_ui_18 = lv_font_montserrat_20;
	for (locale_case = 1; locale_case <= 2; locale_case++) {
		stats_init(&cfg);
		lv_screen_load(stats_screen);
		lv_obj_update_layout(stats_screen);
		const char *headings[] = {tr("stats_top_artists"), tr("stats_top_albums")};
		for (int i = 0; i < 2; i++) {
			lv_obj_t *head = find_label(stats_screen, headings[i]);
			assert(head && lv_label_get_long_mode(head) == LV_LABEL_LONG_WRAP);
			lv_obj_t *strip = lv_obj_get_parent(head);
			lv_obj_t *button_label = find_label(strip, tr("stats_see_all"));
			assert(button_label);
			lv_obj_t *button = lv_obj_get_parent(button_label);
			lv_area_t h, b, row;
			lv_obj_get_coords(head, &h);
			lv_obj_get_coords(button, &b);
			lv_obj_get_coords(strip, &row);
			assert(h.x1 >= row.x1 && h.x2 < b.x1);
			assert(b.x2 <= row.x2 && row.x2 < W);
			assert(h.y1 >= row.y1 && h.y2 <= row.y2);
			assert(b.y1 >= row.y1 && b.y2 <= row.y2);
		}
		const char *pill_text[] = {tr("stats_week"), tr("stats_month"), tr("stats_year"), tr("stats_all_time")};
		for (int i = 0; i < 4; i++) {
			lv_obj_t *label = find_label(stats_screen, pill_text[i]);
			assert(label);
			lv_area_t a;
			lv_obj_get_coords(lv_obj_get_parent(label), &a);
			assert(a.x1 >= 0 && a.x2 < W);
		}
	}

	printf("stats: the pills, the cards, the chart and the two top lists fit the V1's panel\n");
	return 0;
}
