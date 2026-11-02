#include "metronome.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "lvgl/lvgl.h"

#include "src/gui/board_profile.h"
#include "src/gui/fonts/fonts.h"
#include "src/gui/metronome/metronomeclick.h"
#include "src/gui/nowplaying/coverflow.h"
#include "src/gui/nowplaying/player.h"
#include "src/gui/shell/gui.h"
#include "src/gui/shell/settingsrow.h"
#include "src/gui/shell/switcher.h"
#include "src/gui/shell/theme.h"
#include "src/system/core/config.h"
#include "src/system/core/lang.h"
#include "src/system/device/power.h"

lv_obj_t *metronome_screen;

// The range the page offers. 30 to 250 is a metronome's own: below 30 there is
// nothing anyone plays along with that a clock would not do as well, and above
// 250 the count is a subdivision rather than a tempo.
#define BPM_MIN 30
#define BPM_MAX 250
#define BEATS_MIN 1
#define BEATS_MAX 7

// How many taps the tempo is averaged over, and how long a silence means the
// next tap starts a new count rather than closing a very slow one.
#define TAP_MAX 5
#define TAP_RESET_MS 2000

// How often the beat the click thread has reached is read back. A fortieth of
// a second: fast enough that the flash is where the ear says it should be, and
// cheap enough to be nothing on a page that is otherwise still.
#define BEAT_POLL_MS 25

static lv_obj_t *bpm_label;
static lv_obj_t *dots[BEATS_MAX];
static lv_obj_t *slider;
static lv_obj_t *start_label;
static lv_obj_t *start_btn;
static lv_obj_t *tap_btn;
static lv_obj_t *beat_btns[BEATS_MAX];
static lv_obj_t *tempo_card;
static lv_obj_t *beats_card;

static int bpm = 120;
static int beats = 4;
static bool running;
// Set while the page is pushing a value into the slider, so its own handler
// does not read it back as a finger.
static bool syncing;
// Which beat of the bar is lit, or -1 when nothing is running.
static int lit_beat = -1;
// The click count the last flash was drawn for, so a poll that arrives between
// two clicks does not repaint anything.
static unsigned beats_seen;
static lv_timer_t *beat_timer;
static uint32_t tap_times[TAP_MAX];
static int tap_count;
// What is already on the card, so a page visited and left untouched does not
// rewrite the file, and a tempo tapped out does not write it on every tap.
static int saved_bpm = -1;
static int saved_beats = -1;

// The release of a swipe is not a tap: LVGL sends CLICKED to whatever the
// finger was on when it lifted. Same guard the notes shelf and the player's
// own controls use.
static bool gesture_not_tap(void) {
	return player_sheet_drag_active() || switcher_back_drag_active() || coverflow_drag_active();
}

// ---------------------------------------------------------------------------
// Painting
//
// The colours here are hand-set rather than taken from a shared style, because
// the same objects are painted differently as the state changes: a lit dot
// against an unlit one, the start button as the accent in both transport states. They are all written in one place, called
// when the state changes and again after a theme switch.
// ---------------------------------------------------------------------------

static void paint_dots(void) {
	for (int i = 0; i < BEATS_MAX; i++) {
		lv_obj_t *dot = dots[i];
		if (!dot) {
			continue;
		}
		// The beats past the bar are not drawn at all: a bar of three is three
		// dots, not three and four empty rings.
		lv_obj_set_hidden(dot, i >= beats);
		bool lit = i == lit_beat;
		lv_obj_set_style_bg_color(dot, lit ? theme()->accent : theme()->text_secondary, 0);
		lv_obj_set_style_bg_opa(dot, lit ? LV_OPA_COVER : LV_OPA_30, 0);
		// A small ring makes the accented first beat recognizable at rest.
		lv_obj_set_style_border_color(dot, theme()->accent, 0);
		lv_obj_set_style_border_opa(dot, i == 0 && !lit ? LV_OPA_60 : LV_OPA_TRANSP, 0);
	}
}

static void paint_beats(void) {
	for (int i = 0; i < BEATS_MAX; i++) {
		lv_obj_t *btn = beat_btns[i];
		if (!btn) {
			continue;
		}
		bool active = i + 1 == beats;
		lv_obj_set_style_bg_color(btn, active ? theme()->accent : theme()->surface_pressed, 0);
		lv_obj_t *label = lv_obj_get_child(btn, 0);
		if (label) {
			lv_obj_set_style_text_color(label, active ? theme_on_accent() : theme()->text_primary, 0);
		}
	}
}

static void paint_transport(void) {
	if (!start_btn) {
		return;
	}
	// Keep the primary action prominent in both states, so Start is easy to
	// find and Stop remains easy to reach while practicing.
	lv_obj_set_style_bg_color(start_btn, theme()->accent, 0);
	lv_obj_set_style_text_color(start_label, theme_on_accent(), 0);
	lv_obj_set_style_bg_color(tap_btn, theme()->surface_pressed, 0);
	lv_obj_set_style_text_color(lv_obj_get_child(tap_btn, 0), theme()->text_primary, 0);
}

static void paint(void) {
	if (!bpm_label) {
		return;
	}
	lv_label_set_text_fmt(bpm_label, "%d", bpm);
	paint_dots();
	paint_beats();
	paint_transport();
	// Subtle outlines give the two sections definition even on OLED and light
	// palettes. Both cards wear the same quiet outline: an accent border
	// competes with the accent-coloured Start button and looks loud on the
	// bright palettes, so neither card wears one.
	lv_obj_set_style_border_color(tempo_card, theme()->text_secondary, 0);
	lv_obj_set_style_border_color(beats_card, theme()->text_secondary, 0);
	if (slider) {
		lv_obj_set_style_bg_color(slider, theme()->accent, LV_PART_INDICATOR);
		theme_apply_slider_knob(slider);
	}
}

static void theme_changed_cb(void) { paint(); }

// ---------------------------------------------------------------------------
// The tempo and the bar
// ---------------------------------------------------------------------------

// Writes the tempo down, if it is not what is written already. A metronome is
// left at one tempo and returned to for days, so this is a handful of writes a
// week -- unless a tap tempo were to save on every tap, which at 250 BPM would
// be four rewrites of the card a second.
static void settings_save(void) {
	if (bpm == saved_bpm && beats == saved_beats) {
		return;
	}
	saved_bpm = bpm;
	saved_beats = beats;
	config_set_int("metronome", "bpm", bpm);
	config_set_int("metronome", "beats", beats);
	config_save();
}

// Clamped to the range the page offers, then pushed into everything that shows
// it: the number, the slider and, while it is running, the click.
static void set_bpm(int value) {
	if (value < BPM_MIN) {
		value = BPM_MIN;
	} else if (value > BPM_MAX) {
		value = BPM_MAX;
	}
	if (value == bpm) {
		return;
	}
	bpm = value;
	lv_label_set_text_fmt(bpm_label, "%d", bpm);
	if (slider) {
		syncing = true;
		lv_slider_set_value(slider, bpm, LV_ANIM_OFF);
		syncing = false;
	}
	if (running) {
		metronomeclick_set_tempo(bpm, beats);
	}
}

static void set_beats(int value) {
	if (value < BEATS_MIN) {
		value = BEATS_MIN;
	} else if (value > BEATS_MAX) {
		value = BEATS_MAX;
	}
	if (value == beats) {
		return;
	}
	beats = value;
	lit_beat = -1;
	if (running) {
		metronomeclick_set_tempo(bpm, beats);
		// The click counts its bar from where it is; the flash starts again
		// from the next click rather than guessing which beat that was.
		beats_seen = metronomeclick_beats();
	}
	paint();
}

static void slider_changed_cb(lv_event_t *e) {
	(void)e;
	if (syncing) {
		return;
	}
	// A hand on the slider is a hand choosing a tempo, so any count in progress
	// is about a tempo that is no longer in force.
	tap_count = 0;
	set_bpm((int)lv_slider_get_value(slider));
}

static void slider_released_cb(lv_event_t *e) {
	(void)e;
	settings_save();
}

static void beat_btn_cb(lv_event_t *e) {
	if (gesture_not_tap()) {
		return;
	}
	set_beats((int)(intptr_t)lv_event_get_user_data(e));
	settings_save();
}

// ---------------------------------------------------------------------------
// Tap tempo
//
// The average of the gaps between the taps, not the last gap alone: a finger is
// not a clock, and one hurried tap would otherwise move the tempo a dozen
// beats. A gap longer than TAP_RESET_MS starts a new count, so the button can
// be used again without waiting anything out.
// ---------------------------------------------------------------------------

static void tap_cb(lv_event_t *e) {
	(void)e;
	if (gesture_not_tap()) {
		return;
	}
	uint32_t now = lv_tick_get();

	if (tap_count > 0 && now - tap_times[tap_count - 1] > TAP_RESET_MS) {
		tap_count = 0;
	}
	if (tap_count == TAP_MAX) {
		// The oldest tap falls off, so the tempo follows the finger after a
		// change of mind rather than averaging it with the tempo it left.
		memmove(tap_times, tap_times + 1, sizeof(tap_times[0]) * (TAP_MAX - 1));
		tap_count--;
	}
	tap_times[tap_count++] = now;

	if (tap_count < 2) {
		return;
	}
	uint32_t span = tap_times[tap_count - 1] - tap_times[0];
	if (span == 0) {
		return;
	}
	double avg_ms = (double)span / (double)(tap_count - 1);
	if (avg_ms < 1.0) {
		return;
	}
	// Rounded, then clamped by set_bpm(): a tap slower than 30 BPM or faster
	// than 250 leaves the tempo at the end of the scale rather than outside it.
	// Not written down here: every tap would be a rewrite of the card, and the
	// tempo is saved when the click is started or the page is left.
	set_bpm((int)(60000.0 / avg_ms + 0.5));
}

// ---------------------------------------------------------------------------
// Start and stop
// ---------------------------------------------------------------------------

static void show_beat(int index) {
	if (index == lit_beat) {
		return;
	}
	lit_beat = index;
	paint_dots();
}

static void set_stopped(void) {
	running = false;
	lit_beat = -1;
	lv_timer_pause(beat_timer);
	lv_label_set_text(start_label, tr("metronome_start"));
	paint();
}

// Reads back what the click thread has done: how many beats it has written and
// which one of the bar the last was. Nothing is timed here -- the audio thread
// owns the clock, because it owns the stream -- so the flash cannot drift from
// the click it is showing.
static void beat_poll_cb(lv_timer_t *timer) {
	(void)timer;
	if (!running) {
		lv_timer_pause(beat_timer);
		return;
	}
	if (metronomeclick_failed()) {
		// The output could not be opened. Put the page back to stopped rather
		// than leave a lit button clicking into nothing.
		metronomeclick_stop();
		set_stopped();
		gui_notify_popup("metronome_no_audio");
		return;
	}
	unsigned written = metronomeclick_beats();
	if (written != beats_seen) {
		beats_seen = written;
		show_beat(metronomeclick_beat_index());
	}
}

static void start_cb(lv_event_t *e) {
	(void)e;
	if (gesture_not_tap()) {
		return;
	}
	settings_save();
	if (running) {
		metronomeclick_stop();
		set_stopped();
		return;
	}
	if (!metronomeclick_start(bpm, beats)) {
		gui_notify_popup("metronome_no_audio");
		return;
	}
	running = true;
	lit_beat = -1;
	beats_seen = 0;
	lv_label_set_text(start_label, tr("metronome_stop"));
	paint();
	lv_timer_resume(beat_timer);
	lv_timer_reset(beat_timer);
}

// Leaving the page stops the click. It has no other stop button: a click still
// going with the page gone could be silenced only by coming back here, and on
// the way to anywhere else it would sound like a fault rather than like a
// player's accompaniment.
static void screen_loaded_cb(lv_event_t *e) {
	(void)e;
	// Everything on the page is hand-painted, so the colours of the beat that
	// is not lit this time have to be written again on the way in.
	paint();
}

static void screen_unloaded_cb(lv_event_t *e) {
	(void)e;
	settings_save();
	if (running) {
		metronomeclick_stop();
		set_stopped();
	}
}

// ---------------------------------------------------------------------------
// Building the page
// ---------------------------------------------------------------------------

// The shared card, with the padding the settings pages give theirs.
static lv_obj_t *make_card(lv_obj_t *parent, int pad, int pad_row) {
	lv_obj_t *card = lv_obj_create(parent);
	lv_obj_set_width(card, lv_pct(100));
	lv_obj_set_height(card, LV_SIZE_CONTENT);
	lv_obj_add_style(card, &theme_style_card, 0);
	lv_obj_set_style_radius(card, bp_tile_radius(), 0);
	lv_obj_set_style_border_width(card, 1, 0);
	lv_obj_set_style_border_opa(card, LV_OPA_20, 0);
	lv_obj_set_style_shadow_width(card, 0, 0);
	lv_obj_set_style_pad_all(card, pad, 0);
	lv_obj_set_style_pad_row(card, pad_row, 0);
	lv_obj_set_scrollable(card, false);
	// Presses bubble to the page container, so the swipe-back drag can start on
	// the card as it can in the gaps between rows.
	lv_obj_set_event_bubble(card, true);
	lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(card, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
	return card;
}

// A transparent row inside a card: the flex container everything sits in when
// it needs its children side by side.
static lv_obj_t *make_row(lv_obj_t *parent, int height, int gap) {
	lv_obj_t *row = lv_obj_create(parent);
	lv_obj_set_size(row, lv_pct(100), height);
	lv_obj_set_style_bg_opa(row, 0, 0);
	lv_obj_set_style_border_width(row, 0, 0);
	lv_obj_set_style_radius(row, 0, 0);
	lv_obj_set_style_pad_all(row, 0, 0);
	lv_obj_set_style_pad_column(row, gap, 0);
	lv_obj_set_scrollable(row, false);
	lv_obj_set_event_bubble(row, true);
	lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
	return row;
}

// A pill with a caption on it, the shape the rest of the interface uses for a
// choice. Takes its share of the row it is put in, so the seven beats divide
// one line on the V1's 240 pixels rather than wrapping onto a second.
static lv_obj_t *make_pill(lv_obj_t *parent, const char *text, lv_event_cb_t cb, void *user_data) {
	lv_obj_t *btn = lv_btn_create(parent);
	lv_obj_set_flex_grow(btn, 1);
	lv_obj_set_height(btn, lv_pct(100));
	lv_obj_set_style_radius(btn, LV_RADIUS_CIRCLE, 0);
	lv_obj_set_style_shadow_width(btn, 0, 0);
	lv_obj_set_style_border_width(btn, 0, 0);
	lv_obj_set_style_pad_all(btn, 0, 0);
	// Without this a pill would take the full width and leave nothing for the
	// ones beside it.
	lv_obj_set_style_min_width(btn, 0, 0);
	lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, user_data);
	// A pill under a finger answers like every other button in the interface.
	lv_obj_add_style(btn, &theme_style_card_pressed, LV_STATE_PRESSED);

	lv_obj_t *label = lv_label_create(btn);
	lv_label_set_text(label, text);
	lv_obj_set_style_text_font(label, &font_ui_22, 0);
	lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
	// A word that will not fit inside its share of the row ends at the edge
	// rather than widening the row it is in.
	lv_obj_set_width(label, lv_pct(100));
	lv_label_set_long_mode(label, LV_LABEL_LONG_CLIP);
	lv_obj_center(label);
	return btn;
}

static void build_tempo_card(lv_obj_t *parent, int pad, int pad_row, int gap) {
	lv_obj_t *card = make_card(parent, pad, pad_row);
	tempo_card = card;

	lv_obj_t *readout = make_row(card, bp_pick(40, 82), pad_row);
	lv_obj_set_flex_align(readout, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

	// The number with its unit beside it, the two sharing a baseline.
	lv_obj_t *number_group = lv_obj_create(readout);
	lv_obj_set_size(number_group, LV_SIZE_CONTENT, lv_pct(100));
	lv_obj_set_style_bg_opa(number_group, 0, 0);
	lv_obj_set_style_border_width(number_group, 0, 0);
	lv_obj_set_style_pad_all(number_group, 0, 0);
	lv_obj_set_style_pad_column(number_group, bp_pick(4, 10), 0);
	lv_obj_set_scrollable(number_group, false);
	lv_obj_set_event_bubble(number_group, true);
	lv_obj_set_flex_flow(number_group, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(number_group, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);

	bpm_label = lv_label_create(number_group);
	lv_label_set_text_fmt(bpm_label, "%d", bpm);
	lv_obj_add_style(bpm_label, &theme_style_text, 0);
	lv_obj_set_style_text_font(bpm_label, bp_is_tempotec_v1() ? &font_ui_36_bold : &font_ui_64_bold, 0);

	lv_obj_t *unit = lv_label_create(number_group);
	lv_label_set_text(unit, tr("metronome_bpm"));
	lv_obj_add_style(unit, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(unit, &font_ui_18, 0);
	lv_obj_set_style_pad_bottom(unit, bp_pick(4, 8), 0);

	// One dot per beat of the bar, the downbeat leftmost.
	lv_obj_t *dots_row = make_row(readout, lv_pct(100), bp_pick(5, 9));
	lv_obj_set_width(dots_row, LV_SIZE_CONTENT);
	lv_obj_set_flex_align(dots_row, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

	// The downbeat is drawn a size larger, the way a bar is written down: the
	// beat the click accents is the beat the eye picks out before anything is
	// running, and the one it can follow while the rest flash past.
	const int dot_size = bp_pick(9, 15);
	const int first_dot_size = dot_size + bp_pick(4, 6);
	for (int i = 0; i < BEATS_MAX; i++) {
		lv_obj_t *dot = lv_obj_create(dots_row);
		int side = i == 0 ? first_dot_size : dot_size;
		lv_obj_set_size(dot, side, side);
		lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
		lv_obj_set_style_border_width(dot, i == 0 ? 1 : 0, 0);
		lv_obj_set_style_shadow_width(dot, 0, 0);
		lv_obj_set_style_pad_all(dot, 0, 0);
		lv_obj_set_scrollable(dot, false);
		lv_obj_set_clickable(dot, false);
		dots[i] = dot;
	}

	// The tempo itself, dragged. A continuous range rather than the stepped
	// sliders of the settings pages: every beat per minute is a tempo someone
	// plays at, and the number above is the readout.
	slider = lv_slider_create(card);
	lv_obj_set_width(slider, lv_pct(100));
	lv_obj_set_height(slider, bp_pick(8, 12));
	lv_slider_set_range(slider, BPM_MIN, BPM_MAX);
	// The tempo the page was left at, before any finger touches it: a knob at
	// the end of its track beside a number that says 120 is a page lying about
	// itself.
	lv_slider_set_value(slider, bpm, LV_ANIM_OFF);
	lv_obj_add_style(slider, &theme_style_slider_track, LV_PART_MAIN);
	lv_obj_set_style_radius(slider, LV_RADIUS_CIRCLE, LV_PART_INDICATOR);
	lv_obj_add_style(slider, &theme_style_accent_bg, LV_PART_INDICATOR);
	lv_obj_set_style_radius(slider, LV_RADIUS_CIRCLE, LV_PART_KNOB);
	lv_obj_set_style_bg_color(slider, lv_color_white(), LV_PART_KNOB);
	theme_apply_slider_knob(slider);
	lv_obj_set_style_pad_all(slider, bp_pick(6, 9), LV_PART_KNOB);
	lv_obj_set_style_shadow_width(slider, 6, LV_PART_KNOB);
	lv_obj_set_style_shadow_opa(slider, LV_OPA_30, LV_PART_KNOB);
	lv_obj_set_style_shadow_color(slider, lv_color_black(), LV_PART_KNOB);
	lv_obj_set_style_shadow_offset_y(slider, 1, LV_PART_KNOB);
	// A 8 px trough is a hard target; the knob should be reachable from a
	// little way off it.
	lv_obj_set_ext_click_area(slider, bp_pick(10, 16));
	lv_obj_add_event_cb(slider, slider_changed_cb, LV_EVENT_VALUE_CHANGED, NULL);
	lv_obj_add_event_cb(slider, slider_released_cb, LV_EVENT_RELEASED, NULL);

	// Start on the left and tap beside it, each half the card wide.
	lv_obj_t *buttons = make_row(card, bp_pick(38, 56), gap);
	lv_obj_set_flex_align(buttons, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

	start_btn = make_pill(buttons, tr("metronome_start"), start_cb, NULL);
	start_label = lv_obj_get_child(start_btn, 0);
	tap_btn = make_pill(buttons, tr("metronome_tap"), tap_cb, NULL);
}

static void build_beats_card(lv_obj_t *parent, int pad, int pad_row) {
	lv_obj_t *card = make_card(parent, pad, pad_row);
	beats_card = card;

	lv_obj_t *name = lv_label_create(card);
	lv_label_set_text(name, tr("metronome_beats"));
	lv_obj_add_style(name, &theme_style_text, 0);
	lv_obj_set_style_text_font(name, &font_ui_20_bold, 0);
	lv_obj_set_style_pad_bottom(name, bp_pick(2, 6), 0);

	// One to seven, as one row of seven: the bar is a choice out of a short and
	// fixed list, which is what a segmented row is for.
	lv_obj_t *segments = make_row(card, bp_pick(38, 56), bp_pick(3, 8));
	lv_obj_set_flex_align(segments, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

	for (int i = 0; i < BEATS_MAX; i++) {
		char text[4];
		snprintf(text, sizeof(text), "%d", i + 1);
		lv_obj_t *btn = make_pill(segments, text, beat_btn_cb, (void *)(intptr_t)(i + 1));
		// A rounded rectangle rather than a pill: seven circles on one line
		// read as seven unrelated buttons, and this reads as one scale.
		lv_obj_set_style_radius(btn, bp_tile_radius(), 0);
		beat_btns[i] = btn;
	}
}

void metronome_init(gui_config_t *cfg) {
	const int pad = bp_pick(8, 20);
	const int pad_row = bp_pick(6, 14);
	const int gap = bp_pick(6, 12);

	bpm = (int)config_get_int("metronome", "bpm", 120);
	beats = (int)config_get_int("metronome", "beats", 4);
	if (bpm < BPM_MIN || bpm > BPM_MAX) {
		bpm = 120;
	}
	if (beats < BEATS_MIN || beats > BEATS_MAX) {
		beats = 4;
	}
	saved_bpm = bpm;
	saved_beats = beats;

	lv_obj_t *container = settingsrow_page(metronome_screen, cfg, "metronome");
	settingsrow_title_corner_slots(settingsrow_page_title(metronome_screen), cfg, 0);
	player_sheet_attach_drag(container, true);

	build_tempo_card(container, pad, pad_row, gap);
	build_beats_card(container, pad, pad_row);

	beat_timer = lv_timer_create(beat_poll_cb, BEAT_POLL_MS, NULL);
	lv_timer_pause(beat_timer);
	// Counted while the screen is dark is wasted work -- and stopped while it
	// is dark is wasted too, since there is nowhere to stop it from.
	power_pause_in_standby(beat_timer);

	lv_obj_add_event_cb(metronome_screen, screen_loaded_cb, LV_EVENT_SCREEN_LOADED, NULL);
	lv_obj_add_event_cb(metronome_screen, screen_unloaded_cb, LV_EVENT_SCREEN_UNLOADED, NULL);
	theme_register_refresh(theme_changed_cb);
	paint();
}

void metronome_open(void) {
	// The number and the bar are the ones the page was left holding, and the
	// slider has to agree with them in case config changed meanwhile.
	paint();
	switch_screen(metronome_screen);
}
