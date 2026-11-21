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

// ---------------------------------------------------------------------------
// What the page asks the world for
// ---------------------------------------------------------------------------

// Set by the empty-list section at the end: a history with nothing in it, the
// state the "nothing played yet" line is the whole page for.
static bool lists_empty;

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
		out[i].day = days - count + i;
		out[i].seconds = 0;
		out[i].tracks = 0;
	}
	if (count >= 7) {
		out[count - 1].seconds = 3600;
		out[count - 1].tracks = 12;
		out[count - 3].seconds = 60;
		out[count - 3].tracks = 1;
	}
}

void stats_summary(stats_range_t range, stats_summary_t *out) {
	memset(out, 0, sizeof(*out));
	out->seconds = range == STATS_RANGE_WEEK ? 3660 : 12 * 3600 + 34 * 60;
	out->plays = range == STATS_RANGE_WEEK ? 13 : 195;
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
const lv_image_dsc_t icon_stats_artists = {.header = {.magic = LV_IMAGE_HEADER_MAGIC, .cf = LV_COLOR_FORMAT_A8,
													  .w = 2, .h = 2, .stride = 2},
										   .data = GLYPH_BYTES, .data_size = sizeof(GLYPH_BYTES)};
const lv_image_dsc_t icon_stats_albums = {.header = {.magic = LV_IMAGE_HEADER_MAGIC, .cf = LV_COLOR_FORMAT_A8,
													 .w = 2, .h = 2, .stride = 2},
										  .data = GLYPH_BYTES, .data_size = sizeof(GLYPH_BYTES)};
const lv_image_dsc_t icon_chevron_right = {.header = {.magic = LV_IMAGE_HEADER_MAGIC, .cf = LV_COLOR_FORMAT_A8,
													  .w = 2, .h = 2, .stride = 2},
										   .data = GLYPH_BYTES, .data_size = sizeof(GLYPH_BYTES)};

// The shell bits the page leans on.
void fonts_register_change(void (*cb)(void)) { (void)cb; }
int fonts_text_size(void) { return FONTS_TEXT_NORMAL; }
bool player_sheet_drag_active(void) { return false; }
bool switcher_back_drag_active(void) { return false; }
void switcher_attach_back_gesture(lv_obj_t *screen) { (void)screen; }
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
	return available > 0 && strncmp(text, prefix, available) == 0;
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

	// The four range pills, in the order the pill row was built, on one row.
	lv_obj_t *week = find_label(stats_screen, "stats_week");
	lv_obj_t *month = find_label(stats_screen, "stats_month");
	lv_obj_t *year = find_label(stats_screen, "stats_year");
	lv_obj_t *all = find_label(stats_screen, "stats_all_time");
	assert(week && month && year && all);
	lv_area_t wa, ma, ya, aa;
	lv_obj_get_coords(week, &wa);
	lv_obj_get_coords(month, &ma);
	lv_obj_get_coords(year, &ya);
	lv_obj_get_coords(all, &aa);
	// Left to right, and no two of them on top of each other.
	assert(wa.x1 < ma.x1 && ma.x1 < ya.x1 && ya.x1 < aa.x1);
	// All four inside the panel.
	assert(wa.x1 >= 0 && aa.x2 < W);
	// On the same line: the pills are one row and not a wrapped two.
	assert(wa.y1 == ma.y1 && ma.y1 == ya.y1 && ya.y1 == aa.y1);

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
	// one under the second is the play count. The page opens on "All time",
	// which is the stub's twelve and a half hours and 195 plays.
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
	lv_obj_t *chart_title = find_label(stats_screen, "stats_last_7_days");
	assert(chart_title);
	// Seven weekday letters under the chart, one per bucket: read straight off
	// the row's children, because several buckets legitimately end on the same
	// weekday and a search by text would find the first "S" seven times.
	lv_obj_t *marks_row = lv_obj_get_parent(chart_title); // the chart card
	(void)marks_row;
	lv_obj_t *first_letter = find_label(stats_screen, "stats_day_sun");
	assert(first_letter);
	lv_obj_t *letters_row = lv_obj_get_parent(first_letter);
	assert(letters_row);
	assert(lv_obj_get_child_count(letters_row) == 7);
	lv_area_t prev = {0};
	for (int i = 0; i < 7; i++) {
		lv_obj_t *letter = lv_obj_get_child(letters_row, i);
		assert(lv_obj_check_type(letter, &lv_label_class));
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
	assert(sa.x1 > ha.x1 && sa.y1 <= ha.y1 + 2 && sa.x2 <= W);
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

	printf("stats: the pills, the cards, the chart and the two top lists fit the V1's panel\n");
	return 0;
}
