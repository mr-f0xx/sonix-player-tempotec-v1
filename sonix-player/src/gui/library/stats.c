#include "stats.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
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
static stats_range_t stats_range = STATS_RANGE_WEEK;

// The page's parts, so a fill can find them again without walking the tree.
static lv_obj_t *range_pills[STATS_RANGE_COUNT];
static lv_obj_t *summary_time_value;
static lv_obj_t *summary_plays_value;
static lv_obj_t *chart_card;
static lv_obj_t *chart_title;
static lv_obj_t *chart_viewport;
static lv_obj_t *chart_content;
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

// The series uses external storage; it must stay alive until the next fill.
static int32_t chart_daily_values[STATS_CHART_MONTH_DAYS];
static int32_t *chart_all_values;
static int chart_points = STATS_CHART_WEEK_DAYS;

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
	lv_obj_set_flex_align(strip, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_column(strip, 4, 0);
	lv_obj_set_event_bubble(strip, true);

	// Give the heading only the space left by the button; long translations
	// wrap instead of drawing underneath the button.
	lv_obj_t *heading = text(strip, tr(key), bp_is_tempotec_v1() ? &font_ui_18 : &font_ui_24, theme()->text_primary);
	lv_obj_set_width(heading, 0);
	lv_obj_set_flex_grow(heading, 1);
	lv_label_set_long_mode(heading, LV_LABEL_LONG_WRAP);

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

static void chart_build(lv_obj_t *parent) {
	int height = bp_pick(96, 150);

	// Only this viewport scrolls horizontally, keeping bars and labels together.
	// Vertical gestures can still bubble to the page's vertical scroller.
	chart_viewport = lv_obj_create(parent);
	lv_obj_set_size(chart_viewport, LV_PCT(100), height + lv_font_get_line_height(&font_ui_14) + 4);
	lv_obj_set_style_bg_opa(chart_viewport, 0, 0);
	lv_obj_set_style_border_width(chart_viewport, 0, 0);
	lv_obj_set_style_pad_all(chart_viewport, 0, 0);
	lv_obj_set_scroll_dir(chart_viewport, LV_DIR_HOR);
	lv_obj_set_scroll_chain_ver(chart_viewport, true);
	lv_obj_set_scroll_chain_hor(chart_viewport, false);
	lv_obj_set_scroll_elastic(chart_viewport, false);
	lv_obj_set_scrollbar_mode(chart_viewport, LV_SCROLLBAR_MODE_AUTO);
	chart_content = lv_obj_create(chart_viewport);
	lv_obj_set_width(chart_content, LV_PCT(100));
	lv_obj_set_height(chart_content, LV_SIZE_CONTENT);
	lv_obj_set_style_bg_opa(chart_content, 0, 0);
	lv_obj_set_style_border_width(chart_content, 0, 0);
	lv_obj_set_style_pad_all(chart_content, 0, 0);
	lv_obj_set_style_pad_row(chart_content, 2, 0);
	lv_obj_set_flex_flow(chart_content, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(chart_content, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START);
	lv_obj_set_scrollable(chart_content, false);
	lv_obj_set_clickable(chart_content, false);
	parent = chart_content;

	chart_obj = lv_chart_create(parent);
	lv_obj_set_size(chart_obj, LV_PCT(100), height);
	lv_obj_set_style_bg_opa(chart_obj, 0, 0);
	lv_obj_set_style_border_width(chart_obj, 0, 0);
	lv_obj_set_style_pad_all(chart_obj, 0, 0);
	lv_obj_set_clickable(chart_obj, false);
	lv_chart_set_type(chart_obj, LV_CHART_TYPE_BAR);
	lv_chart_set_point_count(chart_obj, chart_points);
	lv_chart_set_div_line_count(chart_obj, 0, 0);
	// A one-pixel gap separates adjacent bars.
	lv_obj_set_style_pad_column(chart_obj, 1, LV_PART_MAIN);
	lv_obj_set_style_pad_column(chart_obj, 0, LV_PART_ITEMS);
	// A chart with nothing in it yet still needs a range, or every value is a
	// zero-height bar floating on the axis.
	lv_chart_set_range(chart_obj, LV_CHART_AXIS_PRIMARY_Y, 0, 60);
	chart_series = lv_chart_add_series(chart_obj, theme()->accent, LV_CHART_AXIS_PRIMARY_Y);
	lv_chart_set_series_ext_y_array(chart_obj, chart_series, chart_daily_values);

	// The bar's own look: rounded, the accent colour, and no point markers --
	// the bars ARE the marks.
	lv_obj_set_style_bg_color(chart_obj, theme()->accent, LV_PART_ITEMS);
	lv_obj_set_style_bg_opa(chart_obj, LV_OPA_COVER, LV_PART_ITEMS);
	lv_obj_set_style_radius(chart_obj, bp_pick(3, 4), LV_PART_ITEMS);
	lv_obj_set_style_border_width(chart_obj, 0, LV_PART_ITEMS);
	lv_obj_set_style_size(chart_obj, 0, 0, LV_PART_INDICATOR);

	// The labels under the bars, in the card's own row rather than as
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

// A label row independent of LVGL's chart axis, which uses the theme font.
static void chart_clear_marks(void) {
	while (lv_obj_get_child_count(chart_marks) > 0) lv_obj_delete(lv_obj_get_child(chart_marks, 0));
}

static lv_obj_t *chart_mark(const char *label, lv_text_align_t align) {
	lv_obj_t *mark = text(chart_marks, label, &font_ui_14, theme()->text_secondary);
	lv_obj_set_width(mark, 0);
	lv_obj_set_flex_grow(mark, 1);
	lv_obj_set_height(mark, lv_font_get_line_height(&font_ui_14));
	lv_obj_set_style_text_align(mark, align, 0);
	lv_obj_set_style_pad_all(mark, 0, 0);
	return mark;
}

static int weekday(int day) {
	// 1970-01-01 was Thursday; return Monday=0 through Sunday=6.
	return ((day + 3) % 7 + 7) % 7;
}

static int32_t chart_minutes(int64_t seconds) {
	if (seconds <= 0) return 0;
	int64_t minutes = seconds / 60 + (seconds % 60 >= 30);
	if (minutes < 1) minutes = 1;
	return minutes > INT32_MAX ? INT32_MAX : (int32_t)minutes;
}

static void chart_fill(void) {
	static const char *const WEEKDAY_KEYS[7] = {"stats_day_mon", "stats_day_tue", "stats_day_wed", "stats_day_thu",
		"stats_day_fri", "stats_day_sat", "stats_day_sun"};
	static const char *const MONTH_KEYS[12] = {"stats_month_jan", "stats_month_feb", "stats_month_mar", "stats_month_apr",
		"stats_month_may", "stats_month_jun", "stats_month_jul", "stats_month_aug", "stats_month_sep",
		"stats_month_oct", "stats_month_nov", "stats_month_dec"};
	static const char *const MONTH_NAME_KEYS[12] = {"stats_month_name_jan", "stats_month_name_feb", "stats_month_name_mar",
		"stats_month_name_apr", "stats_month_name_may", "stats_month_name_jun", "stats_month_name_jul",
		"stats_month_name_aug", "stats_month_name_sep", "stats_month_name_oct", "stats_month_name_nov",
		"stats_month_name_dec"};
	stats_day_t days[STATS_CHART_MONTH_DAYS];
	int points = 7, month = 0, year = 0, today_day = 0;
	int32_t *values = chart_daily_values;
	int32_t *new_all_values = NULL;
	char title[80];
	bool monthly = stats_range == STATS_RANGE_MONTH;
	chart_clear_marks();
	lv_obj_set_hidden(chart_viewport, false);

	if (monthly) {
		points = stats_calendar_month(days, &month, &year, &today_day);
		if (points < 1 || points > STATS_CHART_MONTH_DAYS || month < 1 || month > 12) return;
		snprintf(title, sizeof(title), "%s", tr(MONTH_NAME_KEYS[month - 1]));
		for (int i = 0; i < points; i++) {
			chart_daily_values[i] = chart_minutes(days[i].seconds);
			char label[4];
			snprintf(label, sizeof(label), "%d", i + 1);
			lv_obj_t *mark = chart_mark(label, LV_TEXT_ALIGN_CENTER);
			if (i + 1 == today_day) lv_obj_set_style_text_color(mark, theme()->accent, 0);
		}
	} else if (stats_range == STATS_RANGE_YEAR) {
		int64_t seconds[12] = {0};
		stats_calendar_year(seconds, &year);
		points = 12;
		snprintf(title, sizeof(title), "%d", year);
		for (int i = 0; i < points; i++) {
			chart_daily_values[i] = chart_minutes(seconds[i]);
			chart_mark(tr(MONTH_KEYS[i]), LV_TEXT_ALIGN_CENTER);
		}
	} else if (stats_range == STATS_RANGE_ALL) {
		int first = 0, count = 0;
		int64_t *seconds = stats_all_years(&first, &count);
		if (seconds && count > 0) new_all_values = calloc((size_t)count, sizeof(*new_all_values));
		if (!new_all_values) {
			free(seconds);
			lv_label_set_text(chart_title, tr("stats_all_time"));
			lv_obj_set_hidden(chart_viewport, true);
			return; // do not display a fabricated empty history on allocation failure
		}
		points = count;
		values = new_all_values;
		for (int i = 0; i < points; i++) values[i] = chart_minutes(seconds[i]);
		free(seconds);
		snprintf(title, sizeof(title), "%s", tr("stats_all_time"));
		char label[16];
		if (points <= 4) {
			for (int i = 0; i < points; i++) {
				snprintf(label, sizeof(label), "%d", first + i);
				chart_mark(label, LV_TEXT_ALIGN_CENTER);
			}
		} else {
			// Endpoint labels stay legible even as more years accumulate.
			snprintf(label, sizeof(label), "%d", first);
			chart_mark(label, LV_TEXT_ALIGN_LEFT);
			snprintf(label, sizeof(label), "%d", first + points - 1);
			chart_mark(label, LV_TEXT_ALIGN_RIGHT);
		}
	} else {
		stats_daily(points, days, points);
		snprintf(title, sizeof(title), "%s", tr("stats_last_7_days"));
		for (int i = 0; i < points; i++) chart_daily_values[weekday(days[i].day)] = chart_minutes(days[i].seconds);
		for (int i = 0; i < points; i++) {
			lv_obj_t *mark = chart_mark(tr(WEEKDAY_KEYS[i]), LV_TEXT_ALIGN_CENTER);
			if (i == weekday(days[points - 1].day)) lv_obj_set_style_text_color(mark, theme()->accent, 0);
		}
	}
	lv_label_set_text(chart_title, title);
	// Month alone scrolls. For a short all-time history, keep the year bars
	// narrow and centered instead of stretching a single year across the card.
	// The labels use the same gaps as the bars so their centers stay aligned.
	bool compact_all = stats_range == STATS_RANGE_ALL && points <= 4;
	int gap = compact_all ? bp_pick(12, 16) : 1;
	lv_obj_set_width(chart_content, monthly ? points * bp_pick(24, 32)
										  : compact_all ? points * bp_pick(44, 56) : LV_PCT(100));
	lv_obj_set_align(chart_content, compact_all ? LV_ALIGN_TOP_MID : LV_ALIGN_TOP_LEFT);
	lv_obj_set_width(chart_obj, compact_all && points == 1 ? bp_pick(32, 40) : LV_PCT(100));
	lv_obj_set_style_pad_column(chart_obj, gap, LV_PART_MAIN);
	lv_obj_set_style_pad_column(chart_marks, compact_all ? gap : 0, 0);
	lv_obj_set_scrollable(chart_viewport, monthly);
	lv_obj_set_clickable(chart_viewport, monthly);
	if (!monthly) lv_obj_scroll_to_x(chart_viewport, 0, LV_ANIM_OFF);
	int32_t peak = 0;
	for (int i = 0; i < points; i++) if (values[i] > peak) peak = values[i];
	lv_chart_set_series_ext_y_array(chart_obj, chart_series, values);
	lv_chart_set_point_count(chart_obj, points);
	free(chart_all_values);
	chart_all_values = new_all_values;
	chart_points = points;
	int32_t top = peak > 2 ? peak : 2;
	if (stats_range == STATS_RANGE_ALL && peak > 0) {
		// Leave a little headroom instead of a tallest bar filling the card.
		int64_t padded = (int64_t)peak + ((int64_t)peak + 3) / 4;
		top = padded > INT32_MAX ? INT32_MAX : (int32_t)padded;
		if (top < 2) top = 2;
	}
	lv_chart_set_range(chart_obj, LV_CHART_AXIS_PRIMARY_Y, 0, top);
	lv_chart_refresh(chart_obj);
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

	if (chart_obj) chart_fill();

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

	// Week, current calendar month, current calendar year and all time.
	lv_obj_t *pills = NULL;
	lv_obj_t *pills_card = settingsrow_pills(container, "stats_period", &pills);
	lv_obj_set_width(pills_card, LV_PCT(100));
	lv_obj_set_style_pad_all(pills_card, STATS_CARD_PAD, 0);
	lv_obj_set_flex_flow(pills, LV_FLEX_FLOW_ROW_WRAP);
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
	lv_obj_set_width(chart_title, LV_PCT(100));
	lv_label_set_long_mode(chart_title, LV_LABEL_LONG_DOT);
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
