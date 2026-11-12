#include "petalarm_page.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "src/gui/fonts/fonts.h"
#include "src/gui/shell/icons.h"
#include "src/gui/shell/settingsrow.h"
#include "src/gui/shell/switcher.h"
#include "src/gui/shell/theme.h"
#include "src/gui/shell/toast.h"
#include "src/system/core/lang.h"
#include "src/system/device/clock.h"
#include "src/system/library/playlists.h"
#include "src/system/pet/petalarm.h"

static lv_obj_t *parent_screen;
static gui_config_t *page_cfg;
static void (*changed_cb)(void);
static lv_obj_t *alarm_screen;
static lv_obj_t *picker_screen;
static lv_obj_t *picker_column;
static lv_obj_t *source_value;
static lv_obj_t *clock_notice;
static settingsrow_duration_t time_row;
static lv_obj_t *daily_pill;
static lv_obj_t *day_pills[7];
static lv_obj_t *fade_pills[4];
static bool alarm_visible;
static bool picker_visible;

static const int FADE_VALUES[] = {0, 10, 30, 60};
static const char *const DAY_TAGS[] = {
	"pet_alarm_sun", "pet_alarm_mon", "pet_alarm_tue", "pet_alarm_wed",
	"pet_alarm_thu", "pet_alarm_fri", "pet_alarm_sat",
};

static void refresh(void) {
	if (!alarm_screen) return;
	settingsrow_duration_set_minutes(&time_row, petalarm_minutes());
	settingsrow_duration_expanded(&time_row, petalarm_enabled());
	settingsrow_duration_repaint(&time_row);
	unsigned mask = petalarm_days();
	settingsrow_pill_active(daily_pill, mask == PETALARM_ALL_DAYS);
	for (int i = 0; i < 7; i++) {
		settingsrow_pill_active(day_pills[i], mask != PETALARM_ALL_DAYS && (mask & (1u << i)));
	}
	for (int i = 0; i < 4; i++) {
		settingsrow_pill_active(fade_pills[i], petalarm_fade_seconds() == FADE_VALUES[i]);
	}
	lv_label_set_text(source_value, petalarm_playlist()[0] ? petalarm_playlist() : tr("pet_alarm_resume"));
	lv_obj_set_hidden(clock_notice, clock_is_set());
	if (changed_cb) changed_cb();
}

static void toggle_cb(lv_event_t *e) {
	(void)e;
	bool on = lv_obj_has_state(time_row.toggle, LV_STATE_CHECKED);
	petalarm_set_enabled(on);
	refresh();
	if (on && !clock_is_set()) toast_error("pet_alarm_no_clock");
	else if (on && !petalarm_days()) toast_error("pet_alarm_no_days");
}

static void time_cb(lv_event_t *e) {
	(void)e;
	petalarm_set_minutes(settingsrow_duration_minutes(&time_row));
	if (changed_cb) changed_cb();
}

static void day_cb(lv_event_t *e) {
	int day = (int)(intptr_t)lv_event_get_user_data(e);
	unsigned mask = petalarm_days();
	if (day < 0) mask = PETALARM_ALL_DAYS;
	else if (day < 7) mask = mask == PETALARM_ALL_DAYS ? (1u << day) : mask ^ (1u << day);
	petalarm_set_days(mask);
	refresh();
	if (!mask && petalarm_enabled()) toast_error("pet_alarm_no_days");
}

static void fade_cb(lv_event_t *e) {
	int seconds = (int)(intptr_t)lv_event_get_user_data(e);
	petalarm_set_fade_seconds(seconds);
	refresh();
}

static void close_cb(lv_event_t *e) {
	(void)e;
	(void)petalarm_page_back();
}

static lv_obj_t *sheet(lv_obj_t *parent, const char *title, lv_obj_t **column) {
	lv_obj_t *screen = lv_obj_create(parent);
	lv_obj_set_size(screen, page_cfg->screen_width, page_cfg->screen_height);
	lv_obj_set_pos(screen, 0, 0);
	lv_obj_set_style_pad_all(screen, 0, 0);
	lv_obj_set_style_border_width(screen, 0, 0);
	lv_obj_set_scrollable(screen, false);
	lv_obj_set_hidden(screen, true);
	*column = settingsrow_page(screen, page_cfg, title);

	lv_obj_t *close = lv_btn_create(screen);
	settingsrow_place_corner_button(close, page_cfg, 0);
	lv_obj_set_style_bg_opa(close, LV_OPA_TRANSP, 0);
	lv_obj_set_style_border_width(close, 0, 0);
	lv_obj_set_style_shadow_width(close, 0, 0);
	lv_obj_set_style_pad_all(close, 0, 0);
	lv_obj_add_event_cb(close, close_cb, LV_EVENT_CLICKED, NULL);
	lv_obj_t *icon = lv_image_create(close);
	lv_image_set_src(icon, &icon_close);
	int size = settingsrow_corner_button_size(page_cfg) * 5 / 9;
	lv_image_set_scale(icon, (uint32_t)((int64_t)LV_SCALE_NONE * size / icon_close.header.h));
	lv_obj_set_size(icon, size, size);
	lv_image_set_inner_align(icon, LV_IMAGE_ALIGN_CENTER);
	lv_obj_add_style(icon, &theme_style_icon, 0);
	lv_obj_center(icon);
	return screen;
}

static void picker_row_deleted(lv_event_t *e) { free(lv_event_get_user_data(e)); }

static void picker_select(lv_event_t *e) {
	if (switcher_back_drag_active()) return;
	const char *name = lv_event_get_user_data(e);
	if (!petalarm_set_playlist(name)) {
		toast_error("pet_alarm_playlist_unavailable");
		return;
	}
	lv_obj_set_hidden(picker_screen, true);
	picker_visible = false;
	refresh();
}

// Picker rows are rebuilt on each visit so changes to the library are seen.
// Own the one name per row and release it on deletion; settingsrow_add's
// long-lived action records are deliberately not used for these transient rows.
static bool picker_add(const char *name, void *unused) {
	(void)unused;
	char *copy = name ? strdup(name) : NULL;
	if (name && !copy) return false;
	lv_obj_t *row = lv_btn_create(picker_column);
	lv_obj_set_width(row, lv_pct(100));
	lv_obj_set_height(row, page_cfg->screen_width < 320 ? 56 : 100);
	lv_obj_add_style(row, &theme_style_card, 0);
	lv_obj_add_style(row, &theme_style_card_pressed, LV_STATE_PRESSED);
	lv_obj_set_style_radius(row, 12, 0);
	lv_obj_set_style_shadow_width(row, 0, 0);
	lv_obj_set_style_pad_hor(row, 12, 0);
	lv_obj_set_event_bubble(row, true);
	lv_obj_add_event_cb(row, picker_select, LV_EVENT_CLICKED, copy);
	lv_obj_add_event_cb(row, picker_row_deleted, LV_EVENT_DELETE, copy);
	lv_obj_t *label = lv_label_create(row);
	lv_obj_add_style(label, &theme_style_text, 0);
	lv_obj_set_style_text_font(label, &font_ui_18, 0);
	lv_obj_set_width(label, lv_pct(100));
	lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
	lv_label_set_text(label, name ? name : tr("pet_alarm_resume"));
	lv_obj_align(label, LV_ALIGN_LEFT_MID, 0, 0);
	return true;
}

static void source_cb(lv_event_t *e) {
	(void)e;
	if (!picker_screen) picker_screen = sheet(alarm_screen, "pet_alarm_local_playlists", &picker_column);
	lv_obj_clean(picker_column);
	picker_add(NULL, NULL);
	playlists_for_each(picker_add, NULL);
	lv_obj_scroll_to_y(picker_column, 0, LV_ANIM_OFF);
	lv_obj_set_hidden(picker_screen, false);
	lv_obj_move_foreground(picker_screen);
	picker_visible = true;
}

static void add_note(lv_obj_t *column, const char *tag, lv_obj_t **out) {
	lv_obj_t *label = lv_label_create(column);
	lv_obj_add_style(label, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(label, &font_ui_14, 0);
	lv_obj_set_width(label, lv_pct(100));
	lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
	lv_label_set_text(label, tr(tag));
	if (out) *out = label;
}

static void build(void) {
	if (alarm_screen || !parent_screen || !page_cfg) return;
	lv_obj_t *column;
	alarm_screen = sheet(parent_screen, "pet_alarm", &column);

	settingsrow_toggle_duration(column, "pet_alarm_time", toggle_cb, time_cb, &time_row);
	// The common duration wheels also serve as a 24-hour time picker. Narrow
	// their widths for the 240px panel so both units remain inside the card.
	if (page_cfg->screen_width < 320) {
		lv_obj_set_width(time_row.hours, 50);
		lv_obj_set_width(time_row.minutes, 60);
		lv_obj_set_style_pad_gap(time_row.wheels, 5, 0);
	}
	add_note(column, "pet_alarm_no_clock", &clock_notice);
	add_note(column, "pet_alarm_note", NULL);

	lv_obj_t *day_row;
	settingsrow_pills(column, "pet_alarm_days", &day_row);
	daily_pill = settingsrow_pill(day_row, "pet_alarm_daily", -1, day_cb);
	for (int i = 1; i <= 7; i++) {
		int day = i % 7; // Monday through Sunday in the interface
		day_pills[day] = settingsrow_pill(day_row, DAY_TAGS[day], day, day_cb);
	}

	lv_obj_t *fade_row;
	settingsrow_pills(column, "pet_alarm_fade", &fade_row);
	fade_pills[0] = settingsrow_pill(fade_row, "off", 0, fade_cb);
	fade_pills[1] = settingsrow_pill_text(fade_row, "10 s", 10, fade_cb);
	fade_pills[2] = settingsrow_pill_text(fade_row, "30 s", 30, fade_cb);
	fade_pills[3] = settingsrow_pill_text(fade_row, "60 s", 60, fade_cb);
	settingsrow_add(column, "pet_alarm_source", &source_value, source_cb, NULL);
	add_note(column, "pet_alarm_volume_note", NULL);
	refresh();
}

void petalarm_page_init(lv_obj_t *parent, gui_config_t *cfg, void (*changed)(void)) {
	parent_screen = parent;
	page_cfg = cfg;
	changed_cb = changed;
	theme_register_refresh(refresh);
}

void petalarm_page_open(void) {
	build();
	if (!alarm_screen) return;
	refresh();
	lv_obj_set_hidden(alarm_screen, false);
	lv_obj_move_foreground(alarm_screen);
	alarm_visible = true;
}

bool petalarm_page_back(void) {
	if (picker_visible) {
		lv_obj_set_hidden(picker_screen, true);
		picker_visible = false;
		return true;
	}
	if (alarm_visible) {
		lv_obj_set_hidden(alarm_screen, true);
		alarm_visible = false;
		if (changed_cb) changed_cb();
		return true;
	}
	return false;
}

void petalarm_page_hide(void) {
	if (picker_screen) lv_obj_set_hidden(picker_screen, true);
	if (alarm_screen) lv_obj_set_hidden(alarm_screen, true);
	picker_visible = alarm_visible = false;
	if (picker_column) lv_obj_clean(picker_column);
}
