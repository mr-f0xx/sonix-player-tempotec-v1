#include "tapesampler.h"
#include "tapewidgets.h"

#include "src/gui/shell/gui.h"
#include "src/system/audio/tapepads.h"
#include "src/system/core/lang.h"

#include <limits.h>
#include <stdio.h>

#define FILE_PAGE 16
static lv_obj_t *body, *setup, *play, *stop, *dub, *tempo_label, *setup_notice;
static lv_obj_t *pad_buttons[TAPE_PAD_COUNT], *steps[TAPE_PAD_STEPS];
static tape_pad_info_t pads[TAPE_PAD_COUNT];
static unsigned revision, last_sounding;
static int last_step = -2;
static tape_pads_state_t last_state;
static const char *const drum_keys[] = {"tape_kick", "tape_snare", "tape_hat", "tape_clap"};

static int editing;
static bool kits_browser, have_cursor;
static char directory[TAPE_PATH_SIZE];
static tape_file_t cursor, files[FILE_PAGE + 1];

static void show_setup(void);
static void show_assignment(void);
static void show_presets(void);
static void show_files(void);

static void preset_label(int preset, char *text, size_t size) {
	snprintf(text, size, "%s %d", tr(drum_keys[preset / 4]), preset % 4 + 1);
}

static const char *pad_label(int p, char *text, size_t size, bool short_name) {
	const tape_pad_info_t *pad = &pads[p];
	if (pad->kind == TAPE_PAD_EMPTY) return short_name ? "--" : tr("tape_pad_empty");
	if (pad->kind == TAPE_PAD_WAV) {
		if (!short_name) return pad->name;
		snprintf(text, size, "%.*s", (int)(strlen(pad->name) - 4), pad->name);
		return text;
	}
	if (short_name) return tr(drum_keys[pad->preset / 4]);
	preset_label(pad->preset, text, size);
	return text;
}

static void refresh_labels(void) {
	tapepads_info(pads);
	for (int p = 0; p < TAPE_PAD_COUNT; p++) {
		char name[TAPE_FILE_NAME_SIZE];
		label_text(lv_obj_get_child(pad_buttons[p], 0), pad_label(p, name, sizeof(name), true));
		disabled(pad_buttons[p], pads[p].kind == TAPE_PAD_EMPTY);
		lv_obj_t *number = lv_obj_get_child(pad_buttons[p], 1);
		char tag[12];
		snprintf(tag, sizeof(tag), pads[p].long_clip ? "%02d / L" : "%02d", p + 1);
		label_text(number, tag);
	}
}

void tapesampler_poll(void) {
	unsigned rev = tapepads_revision();
	if (rev != revision) { revision = rev; refresh_labels(); }
	unsigned active = tapepads_sounding();
	if (active != last_sounding) {
		last_sounding = active;
		for (int p = 0; p < TAPE_PAD_COUNT; p++) selected(pad_buttons[p], (active & (1u << p)) != 0);
	}
	int at = tapepads_step();
	if (at != last_step) {
		last_step = at;
		for (int i = 0; i < TAPE_PAD_STEPS; i++) {
			lv_obj_set_style_bg_color(steps[i], i == at ? theme()->accent : i % 4 ? theme()->surface_pressed : theme()->text_secondary, 0);
		}
	}
	bool loop = tapepads_looping();
	selected(play, loop);
	selected(dub, tapepads_overdub());
	disabled(dub, !loop);
	disabled(stop, !tapepads_active());
	tape_pads_state_t state = tapepads_state();
	if (state != last_state) {
		if (state == TAPE_PADS_SAMPLE_ERROR) gui_notify_popup("tape_sample_error");
		if (state == TAPE_PADS_UNAVAILABLE) gui_notify_popup("tape_output_error");
		last_state = state;
	}
}

void tapesampler_open_pads(void) {
	revision = UINT_MAX;
	last_state = tapepads_state();
	tapesampler_poll();
}

void tapesampler_refresh_theme(void) { last_step = -2; }

static void pad_cb(lv_event_t *e) {
	if (clicked(e)) (void)tapepads_hit((int)(intptr_t)lv_event_get_user_data(e));
}

static void play_cb(lv_event_t *e) {
	if (!clicked(e)) return;
	if (tapepads_looping()) tapepads_stop();
	else (void)tapepads_start();
	tapesampler_poll();
}

static void stop_cb(lv_event_t *e) {
	if (clicked(e)) { tapepads_stop(); tapesampler_poll(); }
}

static void dub_cb(lv_event_t *e) {
	if (clicked(e)) { tapepads_set_overdub(!tapepads_overdub()); tapesampler_poll(); }
}

static void reset_setup(void) {
	lv_obj_clean(setup);
	lv_obj_scroll_to_y(body, 0, LV_ANIM_OFF);
}

static void setup_cb(lv_event_t *e) { if (clicked(e)) show_setup(); }
static void assign_view_cb(lv_event_t *e) { if (clicked(e)) show_assignment(); }
static void presets_cb(lv_event_t *e) { if (clicked(e)) show_presets(); }

static void tempo_cb(lv_event_t *e) {
	if (!clicked(e)) return;
	tapepads_set_bpm(tapepads_bpm() + (int)(intptr_t)lv_event_get_user_data(e));
	char text[24];
	snprintf(text, sizeof(text), "%d BPM", tapepads_bpm());
	label_text(tempo_label, text);
}

static void notice(const char *message) {
	label_text(setup_notice, tr(message));
	lv_obj_set_hidden(setup_notice, false);
}

static void pattern_cb(lv_event_t *e) {
	if (!clicked(e)) return;
	bool demo = (intptr_t)lv_event_get_user_data(e) != 0;
	tapepads_pattern(demo);
	notice(demo ? "tape_demo_loop" : "tape_loop_cleared");
}

static void factory_cb(lv_event_t *e) {
	if (!clicked(e)) return;
	tapepads_factory();
	show_setup();
}

static void edit_cb(lv_event_t *e) {
	if (!clicked(e)) return;
	editing = (int)(intptr_t)lv_event_get_user_data(e);
	show_assignment();
}

static void browser_cb(lv_event_t *e) {
	if (!clicked(e)) return;
	kits_browser = (intptr_t)lv_event_get_user_data(e) != 0;
	strcpy(directory, kits_browser ? TAPE_KIT_DIR : "");
	have_cursor = false;
	show_files();
}

static void preset_cb(lv_event_t *e) {
	if (!clicked(e)) return;
	tapepads_set_preset(editing, (int)(intptr_t)lv_event_get_user_data(e));
	show_assignment();
}

static void show_setup(void) {
	reset_setup();
	lv_obj_t *tempo = stack(setup, LV_FLEX_FLOW_ROW, 36, 6, true);
	lv_obj_set_style_pad_all(tempo, 0, 0);
	row_key(tempo, "-", tempo_cb, -5);
	char bpm[24];
	snprintf(bpm, sizeof(bpm), "%d BPM", tapepads_bpm());
	tempo_label = text_label(tempo, bpm, &font_ui_18, false);
	row_key(tempo, "+", tempo_cb, 5);
	lv_obj_t *pattern = stack(setup, LV_FLEX_FLOW_ROW, 40, 6, false);
	row_key(pattern, tr("tape_demo_loop"), pattern_cb, 1);
	row_key(pattern, tr("tape_clear_loop"), pattern_cb, 0);
	key(setup, tr("tape_factory_kit"), 42, factory_cb, 0);
	key(setup, tr("tape_load_kit"), 42, browser_cb, 1);
	setup_notice = note_label(setup, "");
	lv_obj_set_hidden(setup_notice, true);
	note_label(setup, tr("tape_assign_hint"));
	tapepads_info(pads);
	for (int p = 0; p < TAPE_PAD_COUNT; p++) {
		char name[TAPE_FILE_NAME_SIZE], title[TAPE_FILE_NAME_SIZE + 16];
		snprintf(title, sizeof(title), "%02d  %s", p + 1, pad_label(p, name, sizeof(name), false));
		key(setup, title, 40, edit_cb, p);
	}
}

static void show_assignment(void) {
	reset_setup();
	key(setup, tr("trackmenu_back"), 40, setup_cb, 0);
	tapepads_info(pads);
	char name[TAPE_FILE_NAME_SIZE], title[TAPE_FILE_NAME_SIZE + 16];
	snprintf(title, sizeof(title), "%02d  %s", editing + 1, pad_label(editing, name, sizeof(name), false));
	note_label(setup, title);
	if (pads[editing].long_clip) note_label(setup, tr("tape_long_hint"));
	key(setup, tr("tape_load_wav"), 44, browser_cb, 0);
	key(setup, tr("tape_factory_sound"), 44, presets_cb, 0);
	key(setup, tr("tape_clear_pad"), 44, preset_cb, -1);
}

static void show_presets(void) {
	reset_setup();
	key(setup, tr("trackmenu_back"), 40, assign_view_cb, 0);
	for (int p = 0; p < TAPE_PAD_COUNT; p++) {
		char title[80];
		preset_label(p, title, sizeof(title));
		key(setup, title, 40, preset_cb, p);
	}
}

static void up_cb(lv_event_t *e) {
	if (!clicked(e)) return;
	char *parent = strrchr(directory, '/');
	if (parent) *parent = '\0';
	else directory[0] = '\0';
	have_cursor = false;
	show_files();
}

static void file_cb(lv_event_t *e) {
	if (!clicked(e)) return;
	int i = (int)(intptr_t)lv_event_get_user_data(e);
	if (kits_browser) {
		int n = tapepads_load_kit(files[i].name);
		if (n > 0) {
			show_setup();
			notice("tape_kit_loaded");
		} else gui_notify_popup(n == -1 ? "sd_card_missing" : "tape_kit_empty");
		return;
	}
	char path[TAPE_PATH_SIZE];
	int len = snprintf(path, sizeof(path), "%s%s%s", directory, directory[0] ? "/" : "", files[i].name);
	if (len < 0 || len >= (int)sizeof(path)) { gui_notify_popup("tape_sample_error"); return; }
	if (files[i].directory) {
		strcpy(directory, path);
		have_cursor = false;
		show_files();
	} else if (tapepads_assign(editing, path)) {
		show_assignment();
	} else gui_notify_popup("tape_wav_invalid");
}

static void page_cb(lv_event_t *e) {
	if (!clicked(e)) return;
	have_cursor = (intptr_t)lv_event_get_user_data(e) != 0;
	if (have_cursor) cursor = files[FILE_PAGE - 1];
	show_files();
}

static void show_files(void) {
	reset_setup();
	key(setup, tr("trackmenu_back"), 40, kits_browser ? setup_cb : assign_view_cb, 0);
	note_label(setup, directory[0] ? directory : "/");
	if (!kits_browser && directory[0]) key(setup, "..", 40, up_cb, 0);
	if (have_cursor) key(setup, tr("tape_first_page"), 40, page_cb, 0);
	int count = tape_card_list(directory, kits_browser, have_cursor ? &cursor : NULL, files, FILE_PAGE + 1);
	if (count <= 0) {
		note_label(setup, tr(count < 0 ? "sd_card_missing" : kits_browser ? "tape_kit_hint" : "tape_no_wavs"));
		return;
	}
	for (int i = 0; i < count && i < FILE_PAGE; i++) {
		char label[TAPE_FILE_NAME_SIZE + 4];
		snprintf(label, sizeof(label), "%.*s%s", TAPE_FILE_NAME_SIZE - 1, files[i].name,
			files[i].directory && !kits_browser ? "/" : "");
		key(setup, label, 40, file_cb, i);
	}
	if (count > FILE_PAGE) key(setup, tr("subsonic_next_page"), 40, page_cb, 1);
}

void tapesampler_open_kits(void) { show_setup(); }

void tapesampler_build(lv_obj_t *parent, lv_obj_t **out_pads, lv_obj_t **out_kits) {
	body = parent;
	lv_obj_t *surface = stack(parent, LV_FLEX_FLOW_COLUMN, LV_SIZE_CONTENT, 3, false);
	lv_obj_t *transport = stack(surface, LV_FLEX_FLOW_ROW, 32, 5, false);
	play = row_key(transport, tr("tape_loop"), play_cb, 0);
	dub = row_key(transport, tr("tape_overdub"), dub_cb, 0);
	stop = row_key(transport, tr("metronome_stop"), stop_cb, 0);
	lv_obj_t *lights = stack(surface, LV_FLEX_FLOW_ROW, 4, 3, false);
	for (int i = 0; i < TAPE_PAD_STEPS; i++) {
		lv_obj_t *lamp = lv_obj_create(lights);
		lv_obj_remove_style_all(lamp);
		lv_obj_set_size(lamp, 0, 4);
		lv_obj_set_style_bg_opa(lamp, LV_OPA_COVER, 0);
		lv_obj_set_style_radius(lamp, 2, 0);
		lv_obj_set_scrollable(lamp, false);
		lv_obj_set_clickable(lamp, false);
		lv_obj_set_flex_grow(lamp, 1);
		steps[i] = lamp;
	}
	for (int i = 0; i < TAPE_PAD_COUNT; i += 4) {
		lv_obj_t *row = stack(surface, LV_FLEX_FLOW_ROW, bp_pick(39, 60), 4, false);
		for (int p = i; p < i + 4; p++) {
			lv_obj_t *pad = row_key(row, "", NULL, 0);
			pad_buttons[p] = pad;
			lv_obj_add_event_cb(pad, pad_cb, LV_EVENT_PRESSED, (void *)(intptr_t)p);
			lv_obj_t *caption = lv_obj_get_child(pad, 0);
			lv_obj_set_style_text_font(caption, &font_ui_14, 0);
			lv_obj_set_height(caption, lv_font_get_line_height(&font_ui_14));
			lv_obj_align(caption, LV_ALIGN_CENTER, 0, 7);
			lv_obj_align(text_label(pad, "", &font_ui_14, true), LV_ALIGN_TOP_LEFT, 5, 3);
		}
	}
	*out_pads = surface;
	*out_kits = setup = stack(parent, LV_FLEX_FLOW_COLUMN, LV_SIZE_CONTENT, 4, false);
}
