#include "stats.h"

#include <stdio.h>
#include <string.h>

#include "src/gui/board_profile.h"
#include "src/gui/fonts/fonts.h"
#include "src/gui/library/medialist.h"
#include "src/gui/nowplaying/coverloader.h"
#include "src/gui/shell/icons.h"
#include "src/gui/shell/settingsrow.h"
#include "src/gui/shell/switcher.h"
#include "src/gui/shell/theme.h"
#include "src/system/core/lang.h"
#include "src/system/library/listeningstats.h"

// The Stats page, drawn from the listening history the player keeps.
//
// The page is a scrolling column of cards, built once and re-filled whenever
// it is shown: the numbers are a handful of GROUP BYs over two small tables,
// so reading them again on every entry costs less than keeping a copy of them
// in RAM would. Nothing here counts anything -- listeningstats.c does that
// while the music plays.

lv_obj_t *stats_screen;
lv_obj_t *stats_artists_screen;
lv_obj_t *stats_albums_screen;

static gui_config_t *stats_cfg;

// Which pill is chosen, and what the page is currently showing. The range
// survives leaving the page -- coming back to the statistics you were reading
// is the ordinary thing to want -- but not a reboot: it is a view, not a
// setting.
static stats_range_t stats_range = STATS_RANGE_ALL;

// The page's parts, so a fill can find them again without walking the tree.
static lv_obj_t *range_pills[STATS_RANGE_COUNT];
static lv_obj_t *summary_time_value;
static lv_obj_t *summary_plays_value;
static lv_obj_t *chart_card;
static lv_obj_t *chart_title;
static lv_obj_t *chart_obj;
static lv_chart_series_t *chart_series;
static lv_obj_t *chart_marks;
static lv_obj_t *artists_card;
static lv_obj_t *artists_rows;
static lv_obj_t *albums_card;
static lv_obj_t *albums_rows;

// The full lists, and the scroll panels they draw into.
static lv_obj_t *artists_full_rows;
static lv_obj_t *albums_full_rows;
static lv_obj_t *artists_empty;
static lv_obj_t *albums_empty;

// The chart's own data. LVGL keeps the points, so this array is the series:
// it is written by stats_fill_chart() and never moved afterwards.
static int32_t chart_week_values[STATS_CHART_WEEK_DAYS];
static int32_t chart_month_values[STATS_CHART_MONTH_DAYS];
static int32_t *chart_values = chart_week_values;
static int chart_points = STATS_CHART_WEEK_DAYS;
static int chart_days = STATS_CHART_WEEK_DAYS;

// How many rows a "Top" card on the page shows before "See all" takes over.
#define STATS_PREVIEW_ROWS 2
// And how many the full lists show at once, a row at a time.
#define STATS_FULL_ROWS 8

// The row and list title font: a step smaller on the V1, where these lines
// share a 44 px row with the cover and the plays.
static const lv_font_t *title_font(void) { return bp_is_tempotec_v1() ? &font_ui_18 : &font_ui_22; }

#define STATS_CARD_GAP 8
#define STATS_CARD_PAD 10
#define STATS_ROW_HEIGHT (bp_is_tempotec_v1() ? 44 : 68)
#define STATS_THUMB (bp_is_tempotec_v1() ? 32 : 52)

// The slot pools the lists' covers use. The pages' own pools live in
// coverloader.h; these three are the ones no other page asks for.
#define STATS_SLOT_ARTISTS 36
#define STATS_SLOT_ALBUMS 48
#define STATS_SLOT_FULL_ARTISTS 60
#define STATS_SLOT_FULL_ALBUMS 72

// ---------------------------------------------------------------------------
// Text
// ---------------------------------------------------------------------------

// "1 play" / "12 plays", "1 track played" / "12 tracks played": the same rule
// the rest of the interface follows for counts, and a key of its own for each
// of the two singular forms so a translator can wave them off in a language
// that does not inflect.
static void plays_text(int plays, char *out, size_t size) {
	snprintf(out, size, tr(plays == 1 ? "stats_play_one" : "stats_plays"), plays);
}


// ---------------------------------------------------------------------------
// Small builders
// ---------------------------------------------------------------------------

static lv_obj_t *card(lv_obj_t *parent, int width, int height) {
	lv_obj_t *box = lv_obj_create(parent);
	lv_obj_set_size(box, width, height);
	lv_obj_add_style(box, &theme_style_card, 0);
	lv_obj_set_style_radius(box, bp_pick(10, 12), 0);
	lv_obj_set_style_border_width(box, 0, 0);
	lv_obj_set_style_shadow_width(box, 0, 0);
	lv_obj_set_style_pad_all(box, STATS_CARD_PAD, 0);
	lv_obj_set_scrollable(box, false);
	lv_obj_set_clickable(box, false);
	return box;
}

static lv_obj_t *text(lv_obj_t *parent, const char *body, const lv_font_t *font, lv_color_t color) {
	lv_obj_t *label = lv_label_create(parent);
	lv_label_set_text(label, body);
	lv_obj_set_style_text_font(label, font, 0);
	lv_obj_set_style_text_color(label, color, 0);
	return label;
}

// The dim caption a card wears: "Listening time", "Last 7 days".
static lv_obj_t *caption(lv_obj_t *parent, const char *key) { return text(parent, tr(key), &font_ui_14, theme()->text_secondary); }

// A sentence, as opposed to a line: the whole width of the page, broken over as
// many lines as it needs.
//
// A label left at its natural width is measured as one unbroken line -- LVGL
// hands the wrap a width of LV_COORD_MAX when the label's own width is
// LV_SIZE_CONTENT -- and in a centred column like the shell's page such a line
// overhangs the panel on both sides at once, so the first words and the last
// ones are both lost. That is what the empty "Top artists" list showed on a V1:
// a fragment of a sentence with both its ends cut off.
static lv_obj_t *sentence(lv_obj_t *parent, const char *body, const lv_font_t *font, lv_color_t color) {
	lv_obj_t *label = text(parent, body, font, color);
	lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
	lv_obj_set_width(label, LV_PCT(100));
	lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
	return label;
}

// A card's own heading with "See all" on its right. The whole strip is the
// button, so the arrow is as easy to hit as the words are to read.
static lv_obj_t *heading_strip(lv_obj_t *parent, const char *key, lv_event_cb_t cb, void *user) {
	lv_obj_t *strip = lv_obj_create(parent);
	lv_obj_set_width(strip, LV_PCT(100));
	lv_obj_set_height(strip, LV_SIZE_CONTENT);
	lv_obj_set_style_bg_opa(strip, 0, 0);
	lv_obj_set_style_border_width(strip, 0, 0);
	lv_obj_set_style_pad_all(strip, 0, 0);
	lv_obj_set_scrollable(strip, false);
	lv_obj_set_flex_flow(strip, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(strip, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_event_bubble(strip, true);

	text(strip, tr(key), bp_is_tempotec_v1() ? &font_ui_18 : &font_ui_24, theme()->text_primary);

	if (cb) {
		lv_obj_t *see = lv_btn_create(strip);
		lv_obj_set_height(see, LV_SIZE_CONTENT);
		lv_obj_set_width(see, LV_SIZE_CONTENT);
		lv_obj_add_style(see, &theme_style_card, 0);
		lv_obj_set_style_bg_opa(see, 0, 0);
		lv_obj_set_style_border_width(see, 0, 0);
		lv_obj_set_style_shadow_width(see, 0, 0);
		lv_obj_set_style_pad_all(see, 4, 0);
		lv_obj_set_style_pad_column(see, 3, 0);
		lv_obj_set_flex_flow(see, LV_FLEX_FLOW_ROW);
		lv_obj_set_flex_align(see, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
		lv_obj_add_event_cb(see, cb, LV_EVENT_CLICKED, user);

		lv_obj_t *label = text(see, tr("stats_see_all"), &font_ui_14, theme()->accent);
		lv_obj_set_style_text_color(label, theme()->accent, 0);

		lv_obj_t *chevron = lv_image_create(see);
		lv_image_set_src(chevron, &icon_chevron_right);
		lv_obj_set_style_image_recolor(chevron, theme()->accent, 0);
		lv_obj_set_style_image_recolor_opa(chevron, LV_OPA_COVER, 0);
		lv_obj_set_style_image_opa(chevron, LV_OPA_COVER, 0);
		settingsrow_scale_corner_icon(chevron, stats_cfg);
	}
	return strip;
}

// ---------------------------------------------------------------------------
// The pills
// ---------------------------------------------------------------------------

static void range_paint(void) {
	for (int i = 0; i < STATS_RANGE_COUNT; i++) {
		if (range_pills[i]) {
			settingsrow_pill_active(range_pills[i], (stats_range_t)i == stats_range);
		}
	}
}

// ---------------------------------------------------------------------------
// The two summary cards
// ---------------------------------------------------------------------------

static void range_pick_cb(lv_event_t *e) {
	stats_range = (stats_range_t)(int)(intptr_t)lv_event_get_user_data(e);
	range_paint();
	stats_refresh();
}

// ---------------------------------------------------------------------------
// The chart
// ---------------------------------------------------------------------------

// Seven bars on a week, thirty on a month -- but a month of thirty bars on a
// 228 px card is a 5 px bar and a 2 px gap, which reads as a grey smudge. The
// month is drawn as a week would be, seven days to a bar: the last four weeks
// and the two days before them, which is the same thirty days the pill counts
// and the chart the user can actually read.
#define STATS_CHART_BUCKETS 7

static void chart_build(lv_obj_t *parent) {
	int height = bp_pick(96, 150);

	chart_obj = lv_chart_create(parent);
	lv_obj_set_size(chart_obj, LV_PCT(100), height);
	lv_obj_set_style_bg_opa(chart_obj, 0, 0);
	lv_obj_set_style_border_width(chart_obj, 0, 0);
	lv_obj_set_style_pad_all(chart_obj, 0, 0);
	lv_obj_set_clickable(chart_obj, false);
	lv_chart_set_type(chart_obj, LV_CHART_TYPE_BAR);
	lv_chart_set_point_count(chart_obj, chart_points);
	lv_chart_set_div_line_count(chart_obj, 0, 0);
	// A bar's slot is a seventh of the card, and the bar fills it: the chart's
	// own default leaves a gap wide enough that seven bars would not line up
	// with the seven letters under them.
	lv_obj_set_style_pad_column(chart_obj, 0, LV_PART_MAIN);
	lv_obj_set_style_pad_column(chart_obj, 0, LV_PART_ITEMS);
	// A chart with nothing in it yet still needs a range, or every value is a
	// zero-height bar floating on the axis.
	lv_chart_set_range(chart_obj, LV_CHART_AXIS_PRIMARY_Y, 0, 60);
	chart_series = lv_chart_add_series(chart_obj, theme()->accent, LV_CHART_AXIS_PRIMARY_Y);
	lv_chart_set_series_ext_y_array(chart_obj, chart_series, chart_values);

	// The bar's own look: rounded, the accent colour, and no point markers --
	// the bars ARE the marks.
	lv_obj_set_style_bg_color(chart_obj, theme()->accent, LV_PART_ITEMS);
	lv_obj_set_style_bg_opa(chart_obj, LV_OPA_COVER, LV_PART_ITEMS);
	lv_obj_set_style_radius(chart_obj, bp_pick(3, 4), LV_PART_ITEMS);
	lv_obj_set_style_border_width(chart_obj, 0, LV_PART_ITEMS);
	lv_obj_set_style_size(chart_obj, 0, 0, LV_PART_INDICATOR);

	// The day letters under the bars, in the card's own row rather than as
	// chart ticks: the chart draws its labels in the theme font and at a size
	// that does not fit a 228 px card.
	chart_marks = lv_obj_create(parent);
	lv_obj_set_size(chart_marks, LV_PCT(100), LV_SIZE_CONTENT);
	lv_obj_set_style_bg_opa(chart_marks, 0, 0);
	lv_obj_set_style_border_width(chart_marks, 0, 0);
	lv_obj_set_style_pad_all(chart_marks, 0, 0);
	lv_obj_set_scrollable(chart_marks, false);
	lv_obj_set_clickable(chart_marks, false);
	lv_obj_set_flex_flow(chart_marks, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(chart_marks, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
	lv_obj_set_style_pad_column(chart_marks, 0, 0);
	lv_obj_set_height(chart_marks, lv_font_get_line_height(&font_ui_14));
}

// The letters under the bars: the weekday initial of each bucket's last day,
// which is what makes the chart readable -- a bar is "the days up to Sunday",
// so the letter is the day it ends on.
static void chart_marks_fill(const stats_day_t *days, int count) {
	while (lv_obj_get_child_count(chart_marks) > 0) {
		lv_obj_delete(lv_obj_get_child(chart_marks, 0));
	}
	static const char *const WEEKDAY_KEYS[7] = {"stats_day_sun", "stats_day_mon", "stats_day_tue", "stats_day_wed",
												"stats_day_thu", "stats_day_fri", "stats_day_sat"};
	// 1970-01-01 was a Thursday, and the day number is days since then.
	int per_bucket = count / STATS_CHART_BUCKETS;
	if (per_bucket < 1) {
		per_bucket = 1;
	}
	for (int i = 0; i < STATS_CHART_BUCKETS; i++) {
		int last = (i + 1) * per_bucket - 1;
		if (last >= count) {
			last = count - 1;
		}
		int weekday = last >= 0 ? (days[last].day + 4) % 7 : 0; // 0 = Sunday
		lv_obj_t *mark = text(chart_marks, tr(WEEKDAY_KEYS[weekday]), &font_ui_14, theme()->text_secondary);
		// A box of its own, one seventh of the card: the letter sits centred
		// under the middle of the bucket, and flex_grow divides the row
		// evenly even before the first layout pass has run.
		lv_obj_set_width(mark, 0);
		lv_obj_set_flex_grow(mark, 1);
		lv_obj_set_height(mark, lv_font_get_line_height(&font_ui_14));
		lv_obj_set_style_text_align(mark, LV_TEXT_ALIGN_CENTER, 0);
		lv_obj_set_style_pad_all(mark, 0, 0);
	}
}

static void chart_fill(void) {
	stats_day_t days[STATS_CHART_MONTH_DAYS];
	for (int i = 0; i < STATS_CHART_MONTH_DAYS; i++) {
		days[i].day = 0;
		days[i].seconds = 0;
		days[i].tracks = 0;
	}
	stats_daily(chart_days, days, chart_days);

	// Seconds per bucket, in whole minutes: a bar is "minutes listened", which
	// is what the summary card above it is in and what the eye reads. A day
	// with anything at all on it gets at least a minute, so a single track is
	// not a bar of zero height.
	int per_bucket = chart_days / chart_points;
	if (per_bucket < 1) {
		per_bucket = 1;
	}
	int32_t peak = 0;
	for (int i = 0; i < chart_points; i++) {
		long seconds = 0;
		for (int j = 0; j < per_bucket; j++) {
			int index = i * per_bucket + j;
			if (index < chart_days) {
				seconds += days[index].seconds;
			}
		}
		int32_t minutes = (int32_t)((seconds + 30) / 60);
		if (seconds > 0 && minutes <= 0) {
			minutes = 1;
		}
		chart_values[i] = minutes;
		if (minutes > peak) {
			peak = minutes;
		}
	}
	// A quiet week still gets a scale: two minutes at the top, so one bar of
	// one minute is a bar of half the card and not the whole of it.
	int32_t top = peak > 2 ? peak : 2;
	lv_chart_set_range(chart_obj, LV_CHART_AXIS_PRIMARY_Y, 0, top);
	lv_chart_refresh(chart_obj);

	chart_marks_fill(days, chart_days);
}

// ---------------------------------------------------------------------------
// The top lists
// ---------------------------------------------------------------------------

// One row of a top list: the rank, the cover, the name and the two lines under
// it ("N tracks played"), and the plays on the right.
static lv_obj_t *top_row(lv_obj_t *parent, const stats_entry_t *entry, int rank, int slot, const lv_image_dsc_t *glyph) {
	lv_obj_t *row = lv_obj_create(parent);
	lv_obj_set_size(row, LV_PCT(100), STATS_ROW_HEIGHT);
	lv_obj_set_style_bg_opa(row, 0, 0);
	lv_obj_set_style_border_width(row, 0, 0);
	lv_obj_set_style_pad_all(row, 0, 0);
	lv_obj_set_style_pad_column(row, bp_pick(6, 10), 0);
	lv_obj_set_scrollable(row, false);
	lv_obj_set_clickable(row, false);
	lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

	// The rank: a narrow column of its own so the covers line up whatever the
	// number is.
	lv_obj_t *rank_label = lv_label_create(row);
	char rank_text[8];
	snprintf(rank_text, sizeof(rank_text), "%d", rank);
	lv_label_set_text(rank_label, rank_text);
	lv_obj_set_style_text_font(rank_label, &font_ui_14, 0);
	lv_obj_set_style_text_color(rank_label, theme()->text_secondary, 0);
	lv_obj_set_width(rank_label, bp_pick(14, 22));
	lv_obj_set_style_text_align(rank_label, LV_TEXT_ALIGN_CENTER, 0);

	lv_obj_t *cover = lv_image_create(row);
	lv_obj_set_size(cover, STATS_THUMB, STATS_THUMB);
	lv_obj_set_style_radius(cover, 6, 0);
	lv_obj_set_style_clip_corner(cover, true, 0);
	lv_image_set_inner_align(cover, LV_IMAGE_ALIGN_CENTER);
	lv_image_set_src(cover, glyph);
	lv_obj_set_style_image_recolor(cover, theme()->text_secondary, 0);
	lv_obj_set_style_image_recolor_opa(cover, LV_OPA_COVER, 0);

	// The cover's artwork, if the row has a file behind it to read it from.
	if (entry->value[0]) {
		coverloader_request(slot, entry->value, STATS_THUMB);
	}

	// The name, and only the name: the count is already on the right, so a
	// second line under it saying the same number again would only eat the
	// width the name needs, and "a hundred and twelve plays" is not something
	// the eye reads twice.
	lv_obj_t *name = lv_label_create(row);
	lv_obj_set_flex_grow(name, 1);
	lv_label_set_text(name, entry->name);
	lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
	lv_obj_set_height(name, lv_font_get_line_height(title_font()));
	lv_obj_add_style(name, &theme_style_text, 0);
	lv_obj_set_style_text_font(name, title_font(), 0);

	// The plays themselves, at the right-hand edge of the row.
	char plays[64];
	plays_text(entry->plays, plays, sizeof(plays));
	lv_obj_t *count = text(row, plays, &font_ui_14, theme()->text_secondary);
	lv_obj_set_style_text_align(count, LV_TEXT_ALIGN_RIGHT, 0);

	return row;
}

// A list of `count` rows out of the database, starting at row 0. Only the
// first `limit` are drawn; the page's cards want two, the full lists want a
// screenful.
static void top_list_fill(lv_obj_t *parent, stats_top_t kind, int limit, int slot_base, const lv_image_dsc_t *glyph) {
	// lv_obj_clean() leaves the children in place for the rest of this layout
	// pass, so a card refilled on a range change would keep the old rows until
	// something else happened to redraw it. Deleted, not cleaned.
	uint32_t stale = lv_obj_get_child_count(parent);
	for (uint32_t i = 0; i < stale; i++) {
		lv_obj_delete(lv_obj_get_child(parent, 0));
	}

	stats_entry_t rows[STATS_FULL_ROWS];
	int count = stats_top(kind, stats_range, 0, limit, rows);
	for (int i = 0; i < count; i++) {
		lv_obj_t *row = top_row(parent, &rows[i], i + 1, slot_base + i, glyph);
		if (i > 0) {
			lv_obj_set_style_pad_top(row, bp_pick(4, 8), 0);
		}
	}
}

// ---------------------------------------------------------------------------
// Filling the page
// ---------------------------------------------------------------------------

static void summary_fill(void) {
	stats_summary_t summary;
	stats_summary(stats_range, &summary);

	char time_text[24];
	stats_format_duration(summary.seconds, time_text, sizeof(time_text));
	lv_label_set_text(summary_time_value, time_text);

	char plays[24];
	snprintf(plays, sizeof(plays), "%lu", (unsigned long)summary.plays);
	lv_label_set_text(summary_plays_value, plays);
}

static void stats_fill(void) {
	if (!summary_time_value) {
		return;
	}

	range_paint();
	summary_fill();

	// The chart is the last seven days or the last thirty, whatever the range
	// pills say: it is "how have I been listening lately", and a year of bars
	// on a 228 px card is a smear. The pills govern the cards and the lists.
	if (stats_range == STATS_RANGE_MONTH) {
		chart_days = STATS_CHART_MONTH_DAYS;
	} else {
		chart_days = STATS_CHART_WEEK_DAYS;
	}
	chart_points = STATS_CHART_BUCKETS;
	if (chart_obj) {
		lv_chart_set_point_count(chart_obj, chart_points);
	}
	if (chart_days == STATS_CHART_MONTH_DAYS) {
		chart_values = chart_month_values;
		lv_label_set_text(chart_title, tr("stats_last_30_days"));
	} else {
		chart_values = chart_week_values;
		lv_label_set_text(chart_title, tr("stats_last_7_days"));
	}
	if (chart_series) {
		lv_chart_set_series_ext_y_array(chart_obj, chart_series, chart_values);
	}
	if (chart_obj) {
		chart_fill();
	}

	top_list_fill(artists_rows, STATS_TOP_ARTISTS, STATS_PREVIEW_ROWS, STATS_SLOT_ARTISTS, &icon_stats_artists);
	top_list_fill(albums_rows, STATS_TOP_ALBUMS, STATS_PREVIEW_ROWS, STATS_SLOT_ALBUMS, &icon_stats_albums);
}

void stats_refresh(void) { stats_fill(); }

static void stats_loaded_cb(lv_event_t *e) {
	(void)e;
	stats_fill();
}

// ---------------------------------------------------------------------------
// Building the page
// ---------------------------------------------------------------------------

// The two summary cards, side by side: the label, then the figure, in the size
// the whole page is read at.
static lv_obj_t *summary_card(lv_obj_t *parent, const char *key, lv_obj_t **value_out) {
	lv_obj_t *box = card(parent, 0, LV_SIZE_CONTENT);
	lv_obj_set_style_min_height(box, bp_pick(64, 92), 0);
	lv_obj_set_flex_grow(box, 1);
	lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(box, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
	lv_obj_set_style_pad_row(box, 2, 0);

	lv_obj_t *cap = caption(box, key);
	lv_obj_set_width(cap, LV_PCT(100));
	lv_label_set_long_mode(cap, LV_LABEL_LONG_DOT);
	*value_out = text(box, "0", bp_is_tempotec_v1() ? &font_ui_24_bold : &font_ui_32, theme()->text_primary);
	lv_obj_set_style_text_color(*value_out, theme()->text_primary, 0);
	return box;
}

// The two "See all" pages read the same list as the card they came from, only
// further down it, and for whatever range is chosen on the page behind them.
static void artists_full_fill(void) {
	int count = stats_top_count(STATS_TOP_ARTISTS, stats_range);
	top_list_fill(artists_full_rows, STATS_TOP_ARTISTS, STATS_FULL_ROWS, STATS_SLOT_FULL_ARTISTS, &icon_stats_artists);
	lv_obj_set_hidden(artists_empty, count > 0);
}

static void albums_full_fill(void) {
	int count = stats_top_count(STATS_TOP_ALBUMS, stats_range);
	top_list_fill(albums_full_rows, STATS_TOP_ALBUMS, STATS_FULL_ROWS, STATS_SLOT_FULL_ALBUMS, &icon_stats_albums);
	lv_obj_set_hidden(albums_empty, count > 0);
}

static void see_all_artists_cb(lv_event_t *e) {
	(void)e;
	artists_full_fill();
	switch_screen(stats_artists_screen);
}

static void see_all_albums_cb(lv_event_t *e) {
	(void)e;
	albums_full_fill();
	switch_screen(stats_albums_screen);
}

// Both pages fill themselves when they are opened rather than only when the
// card's button is the way in: the back gesture returns to the stats page, but
// nothing stops the list page being the first screen drawn -- and a page left
// with the previous range's rows would read as a page that had not noticed.
static void artists_loaded_cb(lv_event_t *e) {
	(void)e;
	if (artists_full_rows) {
		artists_full_fill();
	}
}

static void albums_loaded_cb(lv_event_t *e) {
	(void)e;
	if (albums_full_rows) {
		albums_full_fill();
	}
}

static void build_stats_page(gui_config_t *cfg) {
	stats_screen = lv_obj_create(NULL);
	lv_obj_t *container = settingsrow_page(stats_screen, cfg, "stats");
	// Three pills and nothing else on the title row: the heading narrows to
	// clear the corner buttons it does not have.
	settingsrow_title_corner_slots(settingsrow_page_title(stats_screen), cfg, 0);

	lv_obj_set_flex_flow(container, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_style_pad_row(container, STATS_CARD_GAP, 0);
	lv_obj_set_style_pad_hor(container, cfg->padding, 0);
	lv_obj_set_style_pad_ver(container, STATS_CARD_GAP, 0);
	lv_obj_set_scroll_dir(container, LV_DIR_VER);

	// The range: four pills across the top, All chosen to start with -- the
	// whole history is the answer to "how much have I listened", and the
	// narrower question is the one you ask second.
	lv_obj_t *pills = NULL;
	lv_obj_t *pills_card = settingsrow_pills(container, "stats_period", &pills);
	lv_obj_set_width(pills_card, LV_PCT(100));
	lv_obj_set_style_pad_all(pills_card, STATS_CARD_PAD, 0);
	// Four pills on a 228 px card: the shell's row wraps by default, which
	// would put "All time" on a second line of its own. They are made to share
	// one line instead, each as wide as its word.
	lv_obj_set_flex_flow(pills, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(pills, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_gap(pills, bp_pick(2, 8), 0);
	range_pills[STATS_RANGE_WEEK] = settingsrow_pill(pills, "stats_week", STATS_RANGE_WEEK, range_pick_cb);
	range_pills[STATS_RANGE_MONTH] = settingsrow_pill(pills, "stats_month", STATS_RANGE_MONTH, range_pick_cb);
	range_pills[STATS_RANGE_YEAR] = settingsrow_pill(pills, "stats_year", STATS_RANGE_YEAR, range_pick_cb);
	range_pills[STATS_RANGE_ALL] = settingsrow_pill(pills, "stats_all_time", STATS_RANGE_ALL, range_pick_cb);
	for (int i = 0; i < STATS_RANGE_COUNT; i++) {
		lv_obj_set_height(range_pills[i], bp_pick(28, 52));
		lv_obj_set_style_pad_hor(range_pills[i], bp_pick(5, 22), 0);
		lv_obj_set_style_text_font(lv_obj_get_child(range_pills[i], 0), bp_is_tempotec_v1() ? &font_ui_14 : &font_ui_20,
								   0);
	}
	range_paint();

	// The two summary cards, side by side on one row.
	lv_obj_t *summary = lv_obj_create(container);
	lv_obj_set_width(summary, LV_PCT(100));
	lv_obj_set_height(summary, LV_SIZE_CONTENT);
	lv_obj_set_style_bg_opa(summary, 0, 0);
	lv_obj_set_style_border_width(summary, 0, 0);
	lv_obj_set_style_pad_all(summary, 0, 0);
	lv_obj_set_style_pad_column(summary, STATS_CARD_GAP, 0);
	lv_obj_set_scrollable(summary, false);
	lv_obj_set_clickable(summary, false);
	lv_obj_set_flex_flow(summary, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(summary, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	summary_card(summary, "stats_listening_time", &summary_time_value);
	summary_card(summary, "stats_tracks_played", &summary_plays_value);

	// The chart.
	chart_card = card(container, LV_PCT(100), LV_SIZE_CONTENT);
	lv_obj_set_width(chart_card, LV_PCT(100));
	lv_obj_set_flex_flow(chart_card, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_style_pad_row(chart_card, 4, 0);
	chart_title = text(chart_card, tr("stats_last_7_days"), bp_is_tempotec_v1() ? &font_ui_18 : &font_ui_24,
					   theme()->text_primary);
	chart_build(chart_card);

	// Top artists, then top albums: each a card of two rows and a way in to
	// the whole list.
	artists_card = card(container, LV_PCT(100), LV_SIZE_CONTENT);
	lv_obj_set_width(artists_card, LV_PCT(100));
	lv_obj_set_flex_flow(artists_card, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_style_pad_row(artists_card, bp_pick(2, 6), 0);
	heading_strip(artists_card, "stats_top_artists", see_all_artists_cb, NULL);
	artists_rows = lv_obj_create(artists_card);
	lv_obj_set_width(artists_rows, LV_PCT(100));
	lv_obj_set_height(artists_rows, LV_SIZE_CONTENT);
	lv_obj_set_style_bg_opa(artists_rows, 0, 0);
	lv_obj_set_style_border_width(artists_rows, 0, 0);
	lv_obj_set_style_pad_all(artists_rows, 0, 0);
	lv_obj_set_scrollable(artists_rows, false);
	lv_obj_set_clickable(artists_rows, false);
	lv_obj_set_flex_flow(artists_rows, LV_FLEX_FLOW_COLUMN);

	albums_card = card(container, LV_PCT(100), LV_SIZE_CONTENT);
	lv_obj_set_width(albums_card, LV_PCT(100));
	lv_obj_set_flex_flow(albums_card, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_style_pad_row(albums_card, bp_pick(2, 6), 0);
	heading_strip(albums_card, "stats_top_albums", see_all_albums_cb, NULL);
	albums_rows = lv_obj_create(albums_card);
	lv_obj_set_width(albums_rows, LV_PCT(100));
	lv_obj_set_height(albums_rows, LV_SIZE_CONTENT);
	lv_obj_set_style_bg_opa(albums_rows, 0, 0);
	lv_obj_set_style_border_width(albums_rows, 0, 0);
	lv_obj_set_style_pad_all(albums_rows, 0, 0);
	lv_obj_set_scrollable(albums_rows, false);
	lv_obj_set_clickable(albums_rows, false);
	lv_obj_set_flex_flow(albums_rows, LV_FLEX_FLOW_COLUMN);

	switcher_attach_back_gesture(stats_screen);
}

// The two "See all" pages: the same rows, a screenful at a time, scrolled.
static lv_obj_t *build_full_page(gui_config_t *cfg, const char *title, lv_obj_t **rows_out, lv_obj_t **empty_out,
								 stats_top_t kind) {
	(void)kind;
	lv_obj_t *screen = lv_obj_create(NULL);
	lv_obj_t *container = settingsrow_page(screen, cfg, title);
	settingsrow_title_corner_slots(settingsrow_page_title(screen), cfg, 0);
	lv_obj_set_style_pad_hor(container, cfg->padding, 0);

	*rows_out = lv_obj_create(container);
	lv_obj_set_width(*rows_out, LV_PCT(100));
	lv_obj_set_height(*rows_out, LV_SIZE_CONTENT);
	lv_obj_set_style_bg_opa(*rows_out, 0, 0);
	lv_obj_set_style_border_width(*rows_out, 0, 0);
	lv_obj_set_style_pad_all(*rows_out, 0, 0);
	lv_obj_set_scrollable(*rows_out, false);
	lv_obj_set_clickable(*rows_out, false);
	lv_obj_set_flex_flow(*rows_out, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_style_pad_row(*rows_out, bp_pick(6, 10), 0);

	// Nothing to show yet: a line saying so, rather than an empty page that
	// reads like a fault. It is a sentence and not a row, so it is given the
	// page's width and told to wrap: at its natural width it runs off a 240 px
	// panel on both sides, in every language but the shortest.
	*empty_out = sentence(container, tr("stats_nothing_yet"), &font_ui_18, theme()->text_secondary);
	lv_obj_set_hidden(*empty_out, true);

	switcher_attach_back_gesture(screen);
	return screen;
}

void stats_init(gui_config_t *cfg) {
	stats_cfg = cfg;

	build_stats_page(cfg);
	stats_artists_screen =
		build_full_page(cfg, "stats_top_artists", &artists_full_rows, &artists_empty, STATS_TOP_ARTISTS);
	stats_albums_screen = build_full_page(cfg, "stats_top_albums", &albums_full_rows, &albums_empty, STATS_TOP_ALBUMS);

	// The page is refilled every time it is opened: the numbers move while the
	// music plays, and a page drawn once at boot would be yesterday's answer.
	lv_obj_add_event_cb(stats_screen, stats_loaded_cb, LV_EVENT_SCREEN_LOADED, NULL);
	lv_obj_add_event_cb(stats_artists_screen, artists_loaded_cb, LV_EVENT_SCREEN_LOADED, NULL);
	lv_obj_add_event_cb(stats_albums_screen, albums_loaded_cb, LV_EVENT_SCREEN_LOADED, NULL);

	stats_refresh();
}
