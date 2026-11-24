#include "tapedeck.h"

#include "tapesampler.h"
#include "tapewidgets.h"

#include "src/gui/shell/settingsrow.h"
#include "src/system/audio/tapepads.h"
#include "src/system/audio/taperecorder.h"
#include "src/system/core/lang.h"
#include "src/system/device/power.h"

#include <limits.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define METER_SEGMENTS 12
#define RECENT_MEMOS 12
#define POLL_MS 80
#define PROBE_MS 1000

static gui_config_t *cfg;
static lv_obj_t *screen, *body, *views[3];
static lv_obj_t *tabs[3];
enum { MEMOS, PADS, KITS };
static int view;
static lv_obj_t *rec_lamp, *rec_state, *rec_time, *source, *meter_db, *clip, *meter[METER_SEGMENTS];
static lv_obj_t *monitor_btn, *record_btn, *memo_note, *memo_rows;
static lv_timer_t *poll_timer;
static bool have_input, had_card, stopping;
static tape_input_t input;
static uint32_t last_probe;
static tape_rec_state_t prior_state, action_error;
static char memo_names[RECENT_MEMOS][TAPE_MEMO_NAME_SIZE];
static int lit = -1;

static lv_obj_t *lamp(lv_obj_t *parent, int width, int height, int radius) {
	lv_obj_t *obj = lv_obj_create(parent);
	lv_obj_remove_style_all(obj);
	lv_obj_set_size(obj, width, height);
	lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
	lv_obj_set_style_radius(obj, radius, 0);
	lv_obj_set_scrollable(obj, false);
	lv_obj_set_clickable(obj, false);
	return obj;
}

// Keep the monochrome palettes monochrome. On the classic palettes the meter
// uses the accent until the hot end, where amber and red are warnings.
static lv_color_t level_color(theme_semantic_t role) {
	if (theme_get_kind() == THEME_KIND_TOKYO_NIGHT || theme_is_monochrome()) return theme_semantic_color(role);
	if (role == THEME_SEMANTIC_RED) return lv_color_hex(theme()->dark ? 0xff6767 : 0xba2525);
	if (role == THEME_SEMANTIC_AMBER) return lv_color_hex(theme()->dark ? 0xe8b454 : 0x946014);
	return theme()->accent;
}

static void paint_meter(int segments) {
	if (segments == lit) return;
	lit = segments;
	for (int i = 0; i < METER_SEGMENTS; i++) {
		theme_semantic_t tone = i >= 11 ? THEME_SEMANTIC_RED : i >= 9 ? THEME_SEMANTIC_AMBER : THEME_SEMANTIC_GREEN;
		lv_obj_set_style_bg_color(meter[i], i < segments ? level_color(tone) : theme()->surface_pressed, 0);
	}
}

static void play_memo_cb(lv_event_t *e);

static void refresh_memos(void) {
	lv_obj_clean(memo_rows);
	int count = taperecorder_list(memo_names, RECENT_MEMOS);
	if (count <= 0) {
		note_label(memo_rows, tr(count == 0 ? "tape_empty" : "tape_memos_unavailable"));
		return;
	}
	for (int i = 0; i < count; i++) {
		// The timestamp portion is the title; the whole safe filename is kept
		// separately for the guarded path lookup when the row is pressed.
		char title[32];
		const char *name = memo_names[i];
		unsigned take = (unsigned)(name[21] - '0') * 100 + (unsigned)(name[22] - '0') * 10 + (unsigned)(name[23] - '0');
		int len = snprintf(title, sizeof(title), "%.4s-%.2s-%.2s  %.2s:%.2s:%.2s", name + 5, name + 9, name + 11,
			name + 14, name + 16, name + 18);
		if (take > 1) snprintf(title + len, sizeof(title) - (size_t)len, " #%u", take);
		key(memo_rows, title, 40, play_memo_cb, i);
	}
}

static void play_memo_cb(lv_event_t *e) {
	if (!clicked(e) || taperecorder_active()) return;
	int at = (int)(intptr_t)lv_event_get_user_data(e);
	char path[PATH_MAX];
	if (taperecorder_path(memo_names[at], path, sizeof(path))) {
		player_play_file(path);
		player_sheet_open(true);
	} else {
		refresh_memos();
	}
}

static void probe(void) {
	have_input = taperecorder_find_input(&input);
	last_probe = lv_tick_get();
	char label[72];
	if (have_input) {
		snprintf(label, sizeof(label), "%s  /  hw:%d,%d", tr(input.usb ? "tape_usb_input" : "tape_input"), input.card, input.device);
		label_text(source, label);
	} else {
		label_text(source, tr("tape_no_input"));
	}
}

static const char *hint_for(tape_rec_state_t state) {
	switch (state) {
	case TAPE_REC_NO_CARD: return "tape_card_needed";
	case TAPE_REC_INTERRUPTED: return "tape_partial";
	case TAPE_REC_INPUT_ERROR: return "tape_input_error";
	case TAPE_REC_DISK_ERROR: return "tape_disk_error";
	case TAPE_REC_BUSY:
	case TAPE_REC_START_ERROR: return "tape_start_error";
	default: break;
	}
	if (have_input) return NULL;
#ifdef HOST_BUILD
	return "tape_emulator_hint";
#else
	return "tape_input_hint";
#endif
}

static void paint_rec(void) {
	tape_rec_status_t s;
	taperecorder_status(&s);
	bool opening = s.state == TAPE_REC_OPENING;
	bool monitoring = s.state == TAPE_REC_MONITORING || (opening && !s.record);
	bool recording = s.state == TAPE_REC_RECORDING || (opening && s.record);
	bool saving = s.state == TAPE_REC_SAVING;
	bool active = monitoring || recording || saving;
	if (!active) stopping = false;
	if (s.state != prior_state) {
		prior_state = s.state;
		lv_obj_set_style_bg_color(rec_lamp, recording ? level_color(THEME_SEMANTIC_RED) :
			monitoring ? theme()->accent : theme()->text_secondary, 0);
		if (s.state == TAPE_REC_SAVED || s.state == TAPE_REC_INTERRUPTED) refresh_memos();
	}
	unsigned secs = s.frames / TAPE_REC_RATE;
	char clock[16];
	snprintf(clock, sizeof(clock), "%02u:%02u", secs / 60, secs % 60);
	label_text(rec_time, clock);
	const char *caption = saving || (recording && stopping) ? "tape_saving" : stopping ? "tape_stopping" :
		opening ? "tape_opening" : recording ? "tape_recording" : monitoring ? "tape_monitoring" :
		(s.state == TAPE_REC_SAVED || s.state == TAPE_REC_INTERRUPTED) ? "tape_saved" : "tape_idle";
	label_text(rec_state, tr(caption));

	char level[24];
	int db = s.peak ? (int)lround(20.0 * log10(s.peak / 32768.0)) : -96;
	if (db <= -48) snprintf(level, sizeof(level), "< -48 dBFS");
	else snprintf(level, sizeof(level), "%d dBFS", db);
	label_text(meter_db, level);
	int segments = db <= -48 ? 0 : (db + 51) / 4;
	paint_meter(segments);
	lv_obj_set_style_opa(clip, s.clipped ? LV_OPA_COVER : LV_OPA_30, 0);

	label_text(lv_obj_get_child(monitor_btn, 0), tr(monitoring ? "stop" : "tape_meter"));
	label_text(lv_obj_get_child(record_btn, 0), tr(recording ? "stop" : "tape_record"));
	disabled(monitor_btn, stopping || recording || saving || (!monitoring && !have_input));
	disabled(record_btn, stopping || saving || (opening && !s.record) || (!recording && !have_input));
	selected(record_btn, recording || !active);
	for (uint32_t i = 0; i < lv_obj_get_child_count(memo_rows); i++) {
		lv_obj_t *row = lv_obj_get_child(memo_rows, (int)i);
		if (lv_obj_check_type(row, &lv_button_class)) disabled(row, active);
	}
	const char *hint = hint_for(action_error != TAPE_REC_IDLE ? action_error : s.state);
	if (hint) label_text(memo_note, tr(hint));
	lv_obj_set_hidden(memo_note, !hint);
}

static void rec_cb(lv_event_t *e) {
	if (!clicked(e)) return;
	bool record = (intptr_t)lv_event_get_user_data(e) != 0;
	tape_rec_status_t s;
	taperecorder_status(&s);
	action_error = TAPE_REC_IDLE;
	if ((s.state == TAPE_REC_OPENING && s.record == record) ||
		(record ? s.state == TAPE_REC_RECORDING : s.state == TAPE_REC_MONITORING)) {
		stopping = true;
		taperecorder_request_stop();
	} else {
		probe();
		tape_rec_state_t started = taperecorder_start(have_input ? &input : NULL, record);
		if (started != TAPE_REC_OPENING) action_error = started;
	}
	paint_rec();
}

static void poll_cb(lv_timer_t *t) {
	if (lv_screen_active() != screen) {
		if (t) lv_timer_pause(t); // power wake can resume an off-page paint timer
		return;
	}
	if (view == PADS) { tapesampler_poll(); return; }
	if (view == KITS) { tapesampler_poll_kits(); return; }
	if (lv_tick_elaps(last_probe) >= PROBE_MS) {
		if (!taperecorder_active()) probe();
		bool card = gui_card_available();
		if (card != had_card) {
			had_card = card;
			refresh_memos();
		}
		last_probe = lv_tick_get();
	}
	paint_rec();
}

static void refresh_theme(void) {
	lit = -1;
	paint_meter(0);
	tapesampler_refresh_theme();
	lv_obj_set_style_text_color(clip, level_color(THEME_SEMANTIC_RED), 0);
	prior_state = (tape_rec_state_t)-1;
	if (lv_screen_active() == screen) poll_cb(NULL);
}

static void choose_tab(int which) {
	if (which != MEMOS) taperecorder_request_stop();
	if (which != PADS) tapepads_stop();
	view = which;
	lv_timer_set_period(poll_timer, view == PADS ? 40 : POLL_MS);
	for (int i = 0; i < 3; i++) {
		lv_obj_set_hidden(views[i], i != view);
		selected(tabs[i], i == view);
	}
	// A stable playing surface: swiping across percussion keys doesn't move
	// the grid. Other views scroll, including the file and assignment lists.
	lv_obj_set_scrollable(body, view != PADS);
	lv_obj_scroll_to_y(body, 0, LV_ANIM_OFF);
	if (view == PADS) tapesampler_open_pads();
	else if (view == KITS) tapesampler_open_kits();
	else {
		action_error = TAPE_REC_IDLE;
		probe();
		had_card = gui_card_available();
		refresh_memos();
		paint_rec();
	}
}

static void tab_cb(lv_event_t *e) {
	int which = (int)(intptr_t)lv_event_get_user_data(e);
	if (clicked(e) && which != view) choose_tab(which);
}

static void screen_loaded(lv_event_t *e) {
	(void)e;
	choose_tab(MEMOS);
	lv_timer_resume(poll_timer);
}

static void screen_leaving(lv_event_t *e) {
	(void)e;
	taperecorder_request_stop();
	tapepads_stop();
	lv_timer_pause(poll_timer);
}

static lv_obj_t *build_rec(void) {
	lv_obj_t *memos = stack(body, LV_FLEX_FLOW_COLUMN, LV_SIZE_CONTENT, 4, false);
	lv_obj_set_width(text_label(memos, tr("tape_deck"), &font_ui_22, false), lv_pct(100));
	lv_obj_t *card = stack(memos, LV_FLEX_FLOW_COLUMN, LV_SIZE_CONTENT, 5, true);
	lv_obj_set_style_pad_ver(card, 6, 0);
	lv_obj_t *header = stack(card, LV_FLEX_FLOW_ROW, LV_SIZE_CONTENT, 6, false);
	rec_lamp = lamp(header, 7, 7, LV_RADIUS_CIRCLE);
	rec_state = text_label(header, "", &font_ui_16, false);
	lv_obj_set_flex_grow(rec_state, 1);
	lv_obj_set_height(rec_state, lv_font_get_line_height(&font_ui_16));
	lv_label_set_long_mode(rec_state, LV_LABEL_LONG_DOT);
	rec_time = text_label(header, "00:00", &font_ui_22, false);
	source = note_label(card, "");

	lv_obj_t *bar = stack(card, LV_FLEX_FLOW_ROW, 12, 3, false);
	for (int i = 0; i < METER_SEGMENTS; i++) {
		meter[i] = lamp(bar, 0, 12, 2);
		lv_obj_set_flex_grow(meter[i], 1);
	}
	lv_obj_t *scale = stack(card, LV_FLEX_FLOW_ROW, LV_SIZE_CONTENT, 6, false);
	lv_obj_set_flex_align(scale, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	meter_db = text_label(scale, "", &font_ui_14, true);
	clip = text_label(scale, "CLIP", &font_ui_14, false);

	lv_obj_t *transport = stack(memos, LV_FLEX_FLOW_ROW, 42, 6, false);
	monitor_btn = row_key(transport, tr("tape_meter"), rec_cb, 0);
	record_btn = row_key(transport, tr("tape_record"), rec_cb, 1);
	memo_note = note_label(memos, "");
	lv_obj_t *title = text_label(memos, tr("tape_recent"), &font_ui_16, false);
	lv_obj_set_width(title, lv_pct(100));
	memo_rows = stack(memos, LV_FLEX_FLOW_COLUMN, LV_SIZE_CONTENT, 4, false);
	return memos;
}

void tapedeck_init(gui_config_t *config) { cfg = config; }

void tapedeck_open(void) {
	if (!cfg) return;
	if (!screen) {
		screen = lv_obj_create(NULL);
		body = settingsrow_page(screen, cfg, "tape_deck");
		lv_obj_set_style_pad_top(body, 0, 0);
		lv_obj_set_scrollbar_mode(body, LV_SCROLLBAR_MODE_ACTIVE);
		// Put navigation beside Back, not above the playing surface. This
		// leaves room for live FX + record without shrinking the pad hitboxes.
		lv_obj_delete(settingsrow_page_title(screen));
		int left = settingsrow_header_text_left(cfg);
		lv_obj_t *tab_row = stack(screen, LV_FLEX_FLOW_ROW, settingsrow_corner_button_size(cfg), 4, false);
		lv_obj_set_width(tab_row, cfg->screen_width - left - cfg->padding);
		lv_obj_align(tab_row, LV_ALIGN_TOP_LEFT, left, cfg->top_bar_height + cfg->padding);
		tabs[MEMOS] = compact_key(tab_row, tr("tape_memos"), tab_cb, MEMOS);
		tabs[PADS] = compact_key(tab_row, tr("tape_pads"), tab_cb, PADS);
		tabs[KITS] = compact_key(tab_row, tr("tape_kits"), tab_cb, KITS);
		views[MEMOS] = build_rec();
		tapesampler_build(body, &views[PADS], &views[KITS]);
		poll_timer = lv_timer_create(poll_cb, POLL_MS, NULL);
		lv_timer_pause(poll_timer);
		power_pause_in_standby(poll_timer);
		lv_obj_add_event_cb(screen, screen_loaded, LV_EVENT_SCREEN_LOADED, NULL);
		lv_obj_add_event_cb(screen, screen_leaving, LV_EVENT_SCREEN_UNLOAD_START, NULL);
		theme_register_refresh(refresh_theme);
		refresh_theme();
	}
	switch_screen(screen);
}
