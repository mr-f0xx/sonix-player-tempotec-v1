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
static lv_obj_t *rec, *mix_note, *fx_keys[3], *trim_label, *choke_label, *quality_key;
static tape_mix_state_t record_error;
static const struct { tape_pad_effect_t mask; const char *key; } fx_specs[] = {
	{TAPE_FX_FILTER, "tape_fx_filter"}, {TAPE_FX_ECHO, "tape_fx_echo"}, {TAPE_FX_DRIVE, "tape_fx_drive"},
};
static tape_pad_info_t pads[TAPE_PAD_COUNT];
static unsigned revision, last_sounding;
static int last_step = -2;
static tape_pads_state_t last_state;
static const char *const drum_keys[] = {"tape_kick", "tape_snare", "tape_hat", "tape_clap"};

static int editing;
static bool kits_browser, have_cursor;
enum { PICK_NONE, PICK_LEVELS, PICK_SLICE };
static int sample_pick = PICK_NONE;
static tape_levels_mode_t levels_mode = TAPE_LEVELS_CHROMATIC;
static char directory[TAPE_PATH_SIZE];
static tape_file_t cursor, files[FILE_PAGE + 1];
static tape_store_kind_t saves_kind;
static char saves_after[TAPE_SAVE_NAME_SIZE], saves[FILE_PAGE + 1][TAPE_SAVE_NAME_SIZE];
static bool waiting_for_take;

static void show_setup(void);
static void show_assignment(void);
static void show_presets(void);
static void show_files(void);
static void show_saves(void);

static void preset_label(int preset, char *text, size_t size) {
	snprintf(text, size, "%s %d", tr(drum_keys[preset / 4]), preset % 4 + 1);
}

static const char *pad_label(int p, char *text, size_t size, bool short_name) {
	const tape_pad_info_t *pad = &pads[p];
	if (pad->kind == TAPE_PAD_EMPTY) return short_name ? "--" : tr("tape_pad_empty");
	if (pad->kind == TAPE_PAD_WAV && pad->levels) {
		if (short_name) snprintf(text, size, "%+d", pad->semitone);
		else snprintf(text, size, "%s  %+d st", pad->name, pad->semitone);
		return text;
	}
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
		const char *flag = pads[p].long_clip ? "L" : pads[p].trimmed ? "T" : "";
		if (flag[0]) snprintf(tag, sizeof(tag), "%02d / %s", p + 1, flag);
		else snprintf(tag, sizeof(tag), "%02d", p + 1);
		label_text(number, tag);
	}
}

static void paint_mix(void) {
	tape_mix_info_t mix;
	tapemix_info(&mix);
	bool live = mix.state == TAPE_MIX_OPENING || mix.state == TAPE_MIX_RECORDING;
	bool saving = mix.state == TAPE_MIX_SAVING;
	label_text(lv_obj_get_child(rec, 0), tr(live ? "tape_end_rec" : "tape_rec_mix"));
	selected(rec, live);
	disabled(rec, saving);
	lv_color_t ink = live ? theme_semantic_color(THEME_SEMANTIC_RED) : theme()->text_secondary;
	if (!lv_color_eq(lv_obj_get_style_text_color(mix_note, 0), ink)) lv_obj_set_style_text_color(mix_note, ink, 0);
	char number[24];
	const char *text = number;
	if (live) {
		unsigned secs = mix.frames / TAPE_PAD_RATE;
		snprintf(number, sizeof(number), "%02u:%02u", secs / 60, secs % 60);
	} else if (saving) text = tr("tape_saving");
	else if (record_error == TAPE_MIX_NO_CARD) text = tr("tape_write_card");
	else if (record_error == TAPE_MIX_ERROR || mix.state == TAPE_MIX_ERROR) text = tr("tape_take_error");
	else if (mix.state == TAPE_MIX_PARTIAL) text = tr("tape_take_partial");
	else if (mix.state == TAPE_MIX_SAVED) text = tr("tape_take_saved");
	else snprintf(number, sizeof(number), "%d BPM", tapepads_bpm());
	label_text(mix_note, text);
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
	unsigned effects = tapepads_effects();
	for (unsigned i = 0; i < sizeof(fx_specs) / sizeof(*fx_specs); i++) selected(fx_keys[i], effects & fx_specs[i].mask);
	paint_mix();
	tape_pads_state_t state = tapepads_state();
	if (state != last_state) {
		if (state == TAPE_PADS_SAMPLE_ERROR) gui_notify_popup("tape_sample_error");
		if (state == TAPE_PADS_UNAVAILABLE) gui_notify_popup("tape_output_error");
		last_state = state;
	}
}

void tapesampler_open_pads(void) {
	revision = UINT_MAX;
	record_error = TAPE_MIX_IDLE;
	last_state = tapepads_state();
	tapesampler_poll();
}

void tapesampler_refresh_theme(void) { last_step = -2; }

static void pad_cb(lv_event_t *e) {
	if (clicked(e)) (void)tapepads_hit((int)(intptr_t)lv_event_get_user_data(e));
}

static void play_cb(lv_event_t *e) {
	if (!clicked(e)) return;
	if (tapepads_looping()) tapepads_stop_loop();
	else (void)tapepads_start();
	tapesampler_poll();
}

static void stop_cb(lv_event_t *e) {
	if (clicked(e)) { tapepads_stop(); tapesampler_poll(); }
}

static void dub_cb(lv_event_t *e) {
	if (clicked(e)) { tapepads_set_overdub(!tapepads_overdub()); tapesampler_poll(); }
}

static void fx_cb(lv_event_t *e) {
	if (!clicked(e)) return;
	tapepads_toggle_effect((tape_pad_effect_t)(intptr_t)lv_event_get_user_data(e));
	tapesampler_poll();
}

static void record_cb(lv_event_t *e) {
	if (!clicked(e)) return;
	tape_mix_info_t mix;
	tapemix_info(&mix);
	record_error = TAPE_MIX_IDLE;
	if (mix.state == TAPE_MIX_OPENING || mix.state == TAPE_MIX_RECORDING) tapemix_stop();
	else if (mix.state != TAPE_MIX_SAVING) {
		tape_mix_state_t result = tapepads_record();
		if (result != TAPE_MIX_OPENING) record_error = result;
	}
	tapesampler_poll();
}

static void reset_setup(void) {
	waiting_for_take = false;
	trim_label = choke_label = quality_key = NULL;
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

static const char *sequence_message(tape_seq_result_t result, bool save) {
	switch (result) {
	case TAPE_SEQ_OK: return save ? "tape_seq_saved" : "tape_seq_loaded";
	case TAPE_SEQ_NO_CARD: return save ? "tape_write_card" : "sd_card_missing";
	case TAPE_SEQ_SAMPLE_MISSING: return "tape_seq_sample_missing";
	case TAPE_SEQ_SAVE_ERROR: return "tape_seq_save_error";
	default: return "tape_seq_invalid";
	}
}

static void save_sequence_cb(lv_event_t *e) {
	if (clicked(e)) notice(sequence_message(tapepads_save_sequence(), true));
}

static void saves_cb(lv_event_t *e) {
	if (!clicked(e)) return;
	saves_kind = (tape_store_kind_t)(intptr_t)lv_event_get_user_data(e);
	saves_after[0] = '\0';
	show_saves();
}

static void save_page_cb(lv_event_t *e) {
	if (!clicked(e)) return;
	if (lv_event_get_user_data(e)) strcpy(saves_after, saves[FILE_PAGE - 1]);
	else saves_after[0] = '\0';
	show_saves();
}

static void saved_file_cb(lv_event_t *e) {
	if (!clicked(e)) return;
	const char *name = saves[(int)(intptr_t)lv_event_get_user_data(e)];
	if (saves_kind == TAPE_STORE_SEQUENCE) {
		tape_seq_result_t result = tapepads_load_sequence(name);
		if (result == TAPE_SEQ_OK) show_setup();
		notice(sequence_message(result, false));
		lv_obj_scroll_to_view(setup_notice, LV_ANIM_OFF);
	} else {
		char path[PATH_MAX];
		if (tape_store_take_path(name, path, sizeof(path))) {
			player_play_file(path);
			player_sheet_open(true);
		} else gui_notify_popup("tape_sample_error");
	}
}

static void show_saves(void) {
	reset_setup();
	bool take = saves_kind == TAPE_STORE_TAKE;
	key(setup, tr("trackmenu_back"), 40, setup_cb, 0);
	note_label(setup, tr(take ? "tape_saved_takes" : "tape_load_seq"));
	setup_notice = note_label(setup, "");
	lv_obj_set_hidden(setup_notice, true);
	if (take && tapemix_active()) {
		waiting_for_take = true;
		notice("tape_saving");
		return;
	}
	if (saves_after[0]) key(setup, tr("tape_first_page"), 40, save_page_cb, 0);
	int count = tape_store_list(saves_kind, saves_after[0] ? saves_after : NULL, saves, FILE_PAGE + 1);
	if (count <= 0) notice(count < 0 ? "sd_card_missing" : take ? "tape_no_takes" : "tape_no_seqs");
	for (int i = 0; i < count && i < FILE_PAGE; i++) {
		const char *stamp = saves[i] + (take ? sizeof("Take-") : sizeof("Sequence-")) - 1;
		char title[40];
		snprintf(title, sizeof(title), "%.4s-%.2s-%.2s  %.2s:%.2s:%.2s  #%.3s",
			stamp, stamp + 4, stamp + 6, stamp + 9, stamp + 11, stamp + 13, stamp + 16);
		key(setup, title, 40, saved_file_cb, i);
	}
	if (count > FILE_PAGE) key(setup, tr("subsonic_next_page"), 40, save_page_cb, 1);
}

void tapesampler_poll_kits(void) {
	if (waiting_for_take && !tapemix_active()) show_saves();
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
	sample_pick = PICK_NONE;
	strcpy(directory, kits_browser ? TAPE_KIT_DIR : "");
	have_cursor = false;
	show_files();
}

static void sample_browser(int pick, tape_levels_mode_t mode) {
	sample_pick = pick;
	levels_mode = mode;
	kits_browser = false;
	directory[0] = '\0';
	have_cursor = false;
	show_files();
}

static void levels_cb(lv_event_t *e) {
	if (!clicked(e)) return;
	tape_levels_mode_t mode = (tape_levels_mode_t)(intptr_t)lv_event_get_user_data(e);
	sample_browser(PICK_LEVELS, mode == TAPE_LEVELS_SPREAD ? TAPE_LEVELS_SPREAD : TAPE_LEVELS_CHROMATIC);
}

static void slice_cb(lv_event_t *e) {
	if (clicked(e)) sample_browser(PICK_SLICE, TAPE_LEVELS_CHROMATIC);
}

static void quality_cb(lv_event_t *e) {
	if (!clicked(e)) return;
	tape_interp_t next = tapepads_interpolation() == TAPE_INTERP_CUBIC ? TAPE_INTERP_LINEAR : TAPE_INTERP_CUBIC;
	tapepads_set_interpolation(next);
	show_setup();
}

static void paint_edit(void) {
	tapepads_info(pads);
	const tape_pad_info_t *pad = &pads[editing];
	if (trim_label) {
		uint32_t rate = pad->rate ? pad->rate : TAPE_PAD_RATE;
		unsigned in = (unsigned)((uint64_t)pad->trim_start * 1000 / rate);
		unsigned out = (unsigned)((uint64_t)pad->trim_end * 1000 / rate);
		char text[64];
		snprintf(text, sizeof(text), "IN %u.%02u s   OUT %u.%02u s",
			in / 1000, in / 10 % 100, out / 1000, out / 10 % 100);
		label_text(trim_label, text);
	}
	if (choke_label) {
		char text[48];
		int group = pad->choke;
		if (group > 0) snprintf(text, sizeof(text), "%s  C%d", tr("tape_choke"), group);
		else if (group < 0) snprintf(text, sizeof(text), "%s  T%d", tr("tape_choke"), -group);
		else snprintf(text, sizeof(text), "%s  OFF", tr("tape_choke"));
		label_text(choke_label, text);
	}
}

enum { TRIM_IN_BACK, TRIM_IN_FWD, TRIM_OUT_BACK, TRIM_OUT_FWD, TRIM_RESET };

static void trim_cb(lv_event_t *e) {
	if (!clicked(e)) return;
	tapepads_info(pads);
	const tape_pad_info_t *pad = &pads[editing];
	uint32_t start = pad->trim_start, end = pad->trim_end, frames = pad->frames;
	uint32_t step = pad->rate / 100;
	if (!step) step = 1;
	if (frames > 1 && step >= frames) step = frames / 2 ? frames / 2 : 1;
	int code = (int)(intptr_t)lv_event_get_user_data(e);
	switch (code) {
	case TRIM_IN_BACK: start = start > step ? start - step : 0; break;
	case TRIM_IN_FWD:
		if (end > start + 1) {
			start += step;
			if (start >= end) start = end - 1;
		}
		break;
	case TRIM_OUT_BACK: end = end > start + step + 1 ? end - step : start + 1; break;
	case TRIM_OUT_FWD:
		if (!frames) end += step;
		else if (end >= frames || step >= frames - end) end = frames;
		else end += step;
		break;
	default: start = 0; end = frames; break;
	}
	if (frames && end > frames) end = frames;
	(void)tapepads_set_trim(editing, start, end);
	paint_edit();
	refresh_labels();
}

static void choke_cb(lv_event_t *e) {
	if (!clicked(e)) return;
	int group = tapepads_choke_group(editing), next;
	if (group == 0) next = 1;
	else if (group > 0) next = -group;
	else if (-group < TAPE_CHOKE_MAX) next = -group + 1;
	else next = 0;
	(void)tapepads_set_choke_group(editing, next);
	paint_edit();
}

static void preset_cb(lv_event_t *e) {
	if (!clicked(e)) return;
	tapepads_set_preset(editing, (int)(intptr_t)lv_event_get_user_data(e));
	show_assignment();
}

static void show_setup(void) {
	reset_setup();
	sample_pick = PICK_NONE;
	kits_browser = false;
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
	lv_obj_t *seq = stack(setup, LV_FLEX_FLOW_ROW, 38, 6, false);
	compact_key(seq, tr("tape_save_seq"), save_sequence_cb, 0);
	compact_key(seq, tr("tape_load_seq"), saves_cb, TAPE_STORE_SEQUENCE);
	setup_notice = note_label(setup, "");
	lv_obj_set_hidden(setup_notice, true);
	key(setup, tr("tape_saved_takes"), 38, saves_cb, TAPE_STORE_TAKE);
	lv_obj_t *kits = stack(setup, LV_FLEX_FLOW_ROW, 40, 6, false);
	compact_key(kits, tr("tape_factory_kit"), factory_cb, 0);
	compact_key(kits, tr("tape_load_kit"), browser_cb, 1);
	note_label(setup, tr("tape_levels"));
	lv_obj_t *tools = stack(setup, LV_FLEX_FLOW_ROW, 34, 6, false);
	compact_key(tools, tr("tape_slice"), slice_cb, 0);
	compact_key(tools, tr("tape_levels_chromatic"), levels_cb, TAPE_LEVELS_CHROMATIC);
	lv_obj_t *tools2 = stack(setup, LV_FLEX_FLOW_ROW, 34, 6, false);
	compact_key(tools2, tr("tape_levels_spread"), levels_cb, TAPE_LEVELS_SPREAD);
	bool cubic = tapepads_interpolation() == TAPE_INTERP_CUBIC;
	quality_key = compact_key(tools2, tr(cubic ? "tape_interp_cubic" : "tape_interp_linear"), quality_cb, 0);
	selected(quality_key, cubic);
	note_label(setup, tr("tape_levels_hint"));
	note_label(setup, tr("tape_choke_hint"));
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
	sample_pick = PICK_NONE;
	kits_browser = false;
	key(setup, tr("trackmenu_back"), 40, setup_cb, 0);
	tapepads_info(pads);
	char name[TAPE_FILE_NAME_SIZE], title[TAPE_FILE_NAME_SIZE + 16];
	snprintf(title, sizeof(title), "%02d  %s", editing + 1, pad_label(editing, name, sizeof(name), false));
	note_label(setup, title);
	if (pads[editing].long_clip) note_label(setup, tr("tape_long_hint"));
	if (pads[editing].kind == TAPE_PAD_WAV) {
		note_label(setup, tr("tape_trim"));
		trim_label = note_label(setup, "");
		lv_obj_t *trim = stack(setup, LV_FLEX_FLOW_ROW, 34, 4, false);
		compact_key(trim, "IN -", trim_cb, TRIM_IN_BACK);
		compact_key(trim, "IN +", trim_cb, TRIM_IN_FWD);
		compact_key(trim, "OUT -", trim_cb, TRIM_OUT_BACK);
		compact_key(trim, "OUT +", trim_cb, TRIM_OUT_FWD);
		key(setup, tr("tape_trim_reset"), 36, trim_cb, TRIM_RESET);
		note_label(setup, tr("tape_trim_hint"));
	}
	if (pads[editing].kind != TAPE_PAD_EMPTY) {
		choke_label = note_label(setup, "");
		lv_obj_t *choke = stack(setup, LV_FLEX_FLOW_ROW, 34, 6, false);
		compact_key(choke, tr("tape_choke"), choke_cb, 0);
		note_label(setup, tr("tape_choke_hint"));
	}
	paint_edit();
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
	} else if (sample_pick == PICK_LEVELS || sample_pick == PICK_SLICE) {
		bool ok = sample_pick == PICK_LEVELS ?
			tapepads_levels(path, levels_mode, tapepads_interpolation()) :
			tapepads_slice(path, TAPE_PAD_COUNT, tapepads_interpolation());
		if (ok) {
			show_setup();
			notice("tape_edit_saved");
		} else gui_notify_popup("tape_wav_invalid");
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
	bool from_setup = kits_browser || sample_pick != PICK_NONE;
	key(setup, tr("trackmenu_back"), 40, from_setup ? setup_cb : assign_view_cb, 0);
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
	lv_obj_t *surface = stack(parent, LV_FLEX_FLOW_COLUMN, LV_SIZE_CONTENT, 2, false);
	lv_obj_t *transport = stack(surface, LV_FLEX_FLOW_ROW, 31, 4, false);
	play = compact_key(transport, tr("tape_loop"), play_cb, 0);
	dub = compact_key(transport, tr("tape_overdub"), dub_cb, 0);
	stop = compact_key(transport, tr("stop"), stop_cb, 0);
	rec = compact_key(transport, tr("tape_rec_mix"), record_cb, 0);
	// Give the longer captions a little more of the 216 px available.
	lv_obj_set_flex_grow(play, 42);
	lv_obj_set_flex_grow(dub, 70);
	lv_obj_set_flex_grow(stop, 43);
	lv_obj_set_flex_grow(rec, 61);
	lv_obj_t *fx_row = stack(surface, LV_FLEX_FLOW_ROW, 30, 4, false);
	for (unsigned i = 0; i < sizeof(fx_specs) / sizeof(*fx_specs); i++) {
		fx_keys[i] = row_key(fx_row, tr(fx_specs[i].key), fx_cb, fx_specs[i].mask);
	}
	lv_obj_t *status = stack(surface, LV_FLEX_FLOW_ROW, 16, 5, false);
	lv_obj_t *lights = stack(status, LV_FLEX_FLOW_ROW, 4, 2, false);
	lv_obj_set_width(lights, 103);
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
	mix_note = text_label(status, "", &font_ui_14, true);
	lv_obj_set_width(mix_note, 0);
	lv_obj_set_flex_grow(mix_note, 1);
	lv_obj_set_height(mix_note, lv_font_get_line_height(&font_ui_14));
	lv_obj_set_style_text_align(mix_note, LV_TEXT_ALIGN_RIGHT, 0);
	lv_label_set_long_mode(mix_note, LV_LABEL_LONG_CLIP);
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
