#include "libraryscan.h"

#include <dirent.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#include "lvgl/lvgl.h"

#include "src/gui/board_profile.h"
#include "src/gui/fonts/fonts.h"
#include "src/gui/library/music.h"
#include "src/gui/shell/icons.h"
#include "src/gui/shell/main_menu.h"
#include "src/gui/shell/settingsrow.h"
#include "src/gui/shell/switcher.h"
#include "src/gui/shell/theme.h"
#include "src/gui/shell/topbar.h"
#include "src/gui/shell/toast.h"
#include "src/system/library/audiobookdb.h"
#include "src/system/playback/playlist.h"
#include "src/system/streaming/podcastdl.h"
#include "src/system/core/lang.h"
#include "src/system/library/library.h"
#include "src/system/device/power.h"

// Fast enough that the number never looks stuck, slow enough that redrawing it
// costs nothing next to reading tags off a card.
#define SCAN_POLL_MS 200
#define SCAN_BUTTON_WIDTH bp_pick(184, 240)
#define SCAN_BUTTON_HEIGHT bp_pick(52, 68)
#define SCAN_BUTTON_BOTTOM bp_pick(12, 30)
// Space between the two stacked pills at the bottom of the page (Cancel/OK
// below, Continue-in-background above). Tight on the V1 panel so there is
// enough room above for the column.
#define SCAN_PILL_GAP bp_pick(8, 12)
#define SCAN_BG_BUTTON_HEIGHT bp_pick(44, 50)
// Gap above the background pill, between it and the lowest text row.
#define SCAN_CONTENT_GAP bp_pick(6, 12)
// The space between the four things in the middle of the page: the note and
// the three lines of text under it.
#define SCAN_ROW_GAP bp_pick(6, 10)
// The note is sized from what those lines leave (see scan_fit_note() below);
// these are only the bounds. Its source art is 128 px, which is what the
// 480 px players draw it at.
#define SCAN_NOTE_MAX bp_pick(96, 128)
#define SCAN_NOTE_MIN 32

lv_obj_t *libraryscan_screen;

static lv_obj_t *count_label;
static lv_obj_t *status_label;
static lv_obj_t *progress_label;
// The Smart Mix graph runs as its own phases after the music scan. Its line,
// its completed/total percentage, and its time-to-finish are separate from
// progress_label, which names the music file currently being read.
static lv_obj_t *phase_label;
static lv_obj_t *genius_progress_label;
static lv_obj_t *eta_label;
static lv_obj_t *background_button;
static lv_obj_t *ok_button;
static lv_obj_t *cancel_button;
static lv_obj_t *scan_note;
static lv_obj_t *scan_container;
static lv_timer_t *poll_timer;
static char shown_file[512];

static const char *sd_root;

static void scan_fit_note(void);

// Hide any secondary row whose text has just been cleared so that it stops
// taking a line in the flex column, then re-measure and re-shrink the note
// art around whatever rows are actually visible. Called after poll_cb and
// show_finished update the labels.
static void scan_refit(void) {
	lv_obj_set_hidden(progress_label, lv_label_get_text(progress_label)[0] == '\0');
	lv_obj_set_hidden(phase_label, lv_label_get_text(phase_label)[0] == '\0');
	lv_obj_set_hidden(genius_progress_label, lv_label_get_text(genius_progress_label)[0] == '\0');
	lv_obj_set_hidden(eta_label, lv_label_get_text(eta_label)[0] == '\0');
	scan_fit_note();
}

static void show_finished(int found) {
	// The graph's own verdict is independent of the track count: a scan that
	// changed no music still rebuilds or reuses the graph, and this line says
	// which of those it ended on.
	library_scan_progress_t progress;
	library_scan_progress(&progress);
	lv_label_set_text_fmt(count_label, "%d", found);
	lv_label_set_text(status_label, found == 1 ? tr("libraryscan_track_found") : tr("libraryscan_tracks_found"));
	lv_label_set_text(progress_label, "");
	lv_label_set_text(phase_label,
					  progress.genius_failed ? tr("libraryscan_smartmix_failed") :
					  progress.genius_published ? tr("libraryscan_smartmix_ready") : "");
	lv_label_set_text(genius_progress_label, "");
	lv_label_set_text(eta_label, "");

	lv_obj_set_hidden(cancel_button, true);
	lv_obj_set_hidden(ok_button, false);
	lv_obj_set_hidden(background_button, true);

	scan_refit();
}

static void poll_cb(lv_timer_t *timer) {
	int found = library_scan_found();
	lv_label_set_text_fmt(count_label, "%d", found);
	lv_label_set_text(status_label, found == 1 ? tr("libraryscan_track_found") : tr("libraryscan_tracks_found"));

	if (library_scan_running()) {
		// The graph phases have their own line and percentage, distinct from
		// the name of the music file the reading phase is on.
		library_scan_progress_t progress;
		library_scan_progress(&progress);
		bool building = progress.phase == LIBRARY_PHASE_GENIUS_BUILDING;
		bool publishing = progress.phase == LIBRARY_PHASE_GENIUS_PUBLISHING;
		lv_label_set_text(phase_label,
						  progress.phase == LIBRARY_PHASE_GENIUS_CHECKING ? tr("libraryscan_smartmix_checking") :
						  building ? tr("libraryscan_smartmix_indexing") :
						  publishing ? tr("libraryscan_smartmix_publishing") :
						  progress.phase == LIBRARY_PHASE_REORGANIZING ? tr("libraryscan_reorganizing") : "");
		lv_label_set_text(genius_progress_label, "");
		lv_label_set_text(eta_label, "");
		if ((building || publishing) && progress.total > 0) {
			int percent = (int)((int64_t)progress.completed * 100 / progress.total);
			// A full count before the rename means the build is done but the
			// graph is not yet published; hold one short of 100 until it is.
			if (!progress.genius_published && percent >= 100) {
				percent = 99;
			}
			lv_label_set_text_fmt(genius_progress_label, tr("libraryscan_smartmix_progress"), progress.completed,
								  progress.total, percent);
			// Only uniform ranking work contributes to this estimate. Looking
			// for unchanged tracks, reading tags and syncing are separate phases.
			if (building && progress.completed > 0 && progress.completed < progress.total &&
				progress.elapsed_ms >= 1000) {
				uint64_t seconds =
					(uint64_t)progress.elapsed_ms * (progress.total - progress.completed) / progress.completed / 1000;
				lv_label_set_text_fmt(eta_label, tr("libraryscan_smartmix_eta"), (unsigned)(seconds / 60),
									  (unsigned)(seconds % 60));
			}
		}

		char path[512];
		library_scan_current_file(path, sizeof(path));
		if (!path[0]) {
			// Before the first file is opened, or while a directory is being
			// walked, show where the scan is instead of leaving a blank line.
			library_scan_current_folder(path, sizeof(path));
		}
		const char *item = strrchr(path, '/');
		item = item ? item + 1 : path;
		if (item[0]) {
			if (strcmp(shown_file, item) != 0) {
				snprintf(shown_file, sizeof(shown_file), "%s", item);
				lv_label_set_text(progress_label, shown_file);
			}
		} else {
			// No music file is being read during a graph phase; the graph's own
			// line above carries the status instead.
			lv_label_set_text(progress_label, "");
		}

		// Update which secondary rows are visible and re-size the note art to
		// fit whatever is on screen this poll (music filename only, or phase
		// lines, or neither at the handoff between them).
		scan_refit();
		return;
	}

	lv_timer_pause(timer);
	topbar_set_library_check(TOPBAR_LIBRARY_CHECKED);
	show_finished(found);
}

// Back to the Music page, which now shows the library, with the menu as the
// only page behind it: walking back through a finished scan makes no sense.
static void back_to_music(void) {
	screen_history_reset();
	switch_screen_no_history(music_screen);
}

static void ok_cb(lv_event_t *e) {
	(void)e;
	back_to_music();
}

// Stopping is not throwing away: the scan thread winds up at the next file and
// commits everything it has already read, so the index holds exactly the
// tracks the counter was showing.
static void cancel_cb(lv_event_t *e) {
	(void)e;

	// The scan thread winds up at the next file; the poll timer is left to
	// notice and show the finished page itself if this screen is revisited.
	library_scan_stop();

	back_to_music();
}

// Leave the page without stopping the job: it keeps running in the background,
// and power.c's library_scan_running() busy guard holds the device awake.
static void background_cb(lv_event_t *e) {
	(void)e;
	back_to_music();
}

void libraryscan_begin(void) {
	lv_obj_set_hidden(ok_button, true);
	lv_obj_set_hidden(cancel_button, false);
	lv_obj_set_hidden(background_button, false);

	// Opening a running job attaches to it. Returning to this page must never
	// wipe/restart the scan.
	if (!library_scan_running() && !library_scan_start(sd_root)) {
		// Nothing to scan (no card, or a scan is somehow already going).
		lv_label_set_text(count_label, "");
		lv_label_set_text(status_label, tr("no_card_to_scan"));
		lv_label_set_text(progress_label, "");
		lv_label_set_text(phase_label, "");
		lv_label_set_text(genius_progress_label, "");
		lv_label_set_text(eta_label, "");
		lv_obj_set_hidden(cancel_button, true);
		lv_obj_set_hidden(ok_button, false);
		lv_obj_set_hidden(background_button, true);
		scan_refit();
		return;
	}

	// power.c's library_scan_running() busy guard holds the device awake for
	// every phase, even off-page or with the panel dark. No display hold here.
	shown_file[0] = '\0';
	lv_label_set_text(progress_label, "");
	lv_obj_set_hidden(progress_label, true);
	lv_timer_reset(poll_timer);
	lv_timer_resume(poll_timer);
	// Paint the current state at once rather than after the first poll period.
	poll_cb(poll_timer);
}

// ---------------------------------------------------------------------------
// Which folders: the card's top-level folders, ticked, before the scan starts
// ---------------------------------------------------------------------------

#define PICK_CHROME_H bp_pick(180, 250) // the card's title, note, buttons and padding
#define PICK_CARD_PAD bp_pick(10, 20)
#define PICK_CARD_GAP bp_pick(6, 12)
#define PICK_LIST_GAP bp_pick(4, 6)
#define PICK_ROW_HEIGHT bp_pick(44, 60)
#define PICK_ROW_PAD bp_pick(10, 16)
#define PICK_ROW_GAP bp_pick(6, 10)
#define PICK_BUTTON_HEIGHT bp_pick(44, 56)
#define PICK_BUTTON_GAP bp_pick(8, 12)
// Rows are made this many at a time, more as the list nears its end.
#define PICK_BATCH 30

static gui_config_t *pick_cfg;
static lv_obj_t *pick_veil;
static lv_obj_t *pick_list;
static lv_obj_t *pick_scan_btn;
static char **pick_names;
static bool *pick_on;
static lv_obj_t **pick_marks;
static int pick_count;
static int pick_capacity;
static int pick_built; // rows made so far, the first pick_built names

static int name_cmp(const void *a, const void *b) { return strcasecmp(*(const char *const *)a, *(const char *const *)b); }

static void pick_clear(void) {
	for (int i = 0; i < pick_count; i++) {
		free(pick_names[i]);
	}
	pick_count = 0;
	pick_built = 0;
}

static bool pick_add(const char *name) {
	if (pick_count == pick_capacity) {
		int grown = pick_capacity ? pick_capacity * 2 : 64;
		char **names = realloc(pick_names, (size_t)grown * sizeof(*names));
		if (names) {
			pick_names = names;
		}
		bool *on = realloc(pick_on, (size_t)grown * sizeof(*on));
		if (on) {
			pick_on = on;
		}
		lv_obj_t **marks = realloc(pick_marks, (size_t)grown * sizeof(*marks));
		if (marks) {
			pick_marks = marks;
		}
		if (!names || !on || !marks) {
			return false;
		}
		pick_capacity = grown;
	}
	pick_names[pick_count] = strdup(name);
	if (!pick_names[pick_count]) {
		return false;
	}
	pick_on[pick_count] = false;
	pick_marks[pick_count] = NULL;
	pick_count++;
	return true;
}

// The folders at the root of the card, alphabetically: not the hidden ones,
// not the ones a desktop leaves behind, and not Audiobooks or Podcast, which
// the music scan never reads.
static void pick_read_folders(void) {
	pick_clear();
	DIR *dir = sd_root ? opendir(sd_root) : NULL;
	if (!dir) {
		return;
	}
	struct dirent *de;
	while ((de = readdir(dir)) != NULL) {
		if (de->d_name[0] == '.' || playlist_is_junk_name(de->d_name) || strcasecmp(de->d_name, AUDIOBOOKDB_FOLDER) == 0 || strcasecmp(de->d_name, PODCASTDL_FOLDER) == 0) {
			continue;
		}
		char path[768];
		if (snprintf(path, sizeof(path), "%s/%s", sd_root, de->d_name) >= (int)sizeof(path)) {
			continue;
		}
		struct stat st;
		if (stat(path, &st) != 0 || !S_ISDIR(st.st_mode)) {
			continue;
		}
		if (!pick_add(de->d_name)) {
			break;
		}
	}
	closedir(dir);
	if (pick_count > 1) {
		qsort(pick_names, (size_t)pick_count, sizeof(pick_names[0]), name_cmp);
	}
}

// Ticked: the folders saved last time, or every folder when nothing was saved.
static void pick_load_selection(void) {
	int saved_count = 0;
	char **saved = library_scan_folders(&saved_count);
	for (int i = 0; i < pick_count; i++) {
		pick_on[i] = saved_count == 0;
		for (int j = 0; j < saved_count && !pick_on[i]; j++) {
			pick_on[i] = strcasecmp(pick_names[i], saved[j]) == 0;
		}
	}
	library_scan_folders_free(saved, saved_count);
}

static void pick_paint(void) {
	int on = 0;
	for (int i = 0; i < pick_count; i++) {
		if (i < pick_built && pick_marks[i]) {
			lv_obj_set_style_image_opa(pick_marks[i], pick_on[i] ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
		}
		on += pick_on[i];
	}
	lv_obj_set_style_opa(pick_scan_btn, (pick_count == 0 || on > 0) ? LV_OPA_COVER : LV_OPA_40, 0);
}

static void pick_row_cb(lv_event_t *e) {
	int i = (int)(intptr_t)lv_event_get_user_data(e);
	if (i >= 0 && i < pick_count) {
		pick_on[i] = !pick_on[i];
		pick_paint();
	}
}

static void pick_close(void) { lv_obj_set_hidden(pick_veil, true); }

static void pick_veil_cb(lv_event_t *e) {
	if (lv_event_get_target(e) == pick_veil) {
		pick_close();
	}
}

static void pick_cancel_cb(lv_event_t *e) {
	(void)e;
	pick_close();
}

// Every folder ticked is the whole card, the files at its root included.
static void pick_scan_cb(lv_event_t *e) {
	(void)e;
	const char **chosen = malloc((size_t)(pick_count ? pick_count : 1) * sizeof(*chosen));
	if (!chosen) {
		return;
	}
	int count = 0;
	for (int i = 0; i < pick_count; i++) {
		if (pick_on[i]) {
			chosen[count++] = pick_names[i];
		}
	}
	if (pick_count > 0 && count == 0) {
		free(chosen);
		toast_error(tr("libraryscan_choose_a_folder"));
		return;
	}
	library_scan_folders_set(chosen, count == pick_count ? 0 : count);
	free(chosen);
	pick_close();
	switch_screen(libraryscan_screen);
}

static lv_obj_t *pick_button(lv_obj_t *parent, const char *text, bool accent, lv_event_cb_t cb) {
	lv_obj_t *btn = lv_btn_create(parent);
	lv_obj_set_height(btn, PICK_BUTTON_HEIGHT);
	lv_obj_set_flex_grow(btn, 1);
	lv_obj_set_style_radius(btn, LV_RADIUS_CIRCLE, 0);
	lv_obj_set_style_shadow_width(btn, 0, 0);
	lv_obj_set_style_border_width(btn, 0, 0);
	lv_obj_set_style_bg_color(btn, accent ? theme()->accent : theme()->surface_pressed, 0);
	lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);
	lv_obj_t *label = lv_label_create(btn);
	lv_label_set_text(label, tr(text));
	lv_obj_set_style_text_font(label, bp_is_tempotec_v1() ? &font_ui_18 : &font_ui_22, 0);
	lv_obj_set_style_text_color(label, accent ? theme_on_accent() : theme()->text_primary, 0);
	lv_obj_center(label);
	return btn;
}

static void pick_scroll_cb(lv_event_t *e);

static void pick_build(void) {
	gui_config_t *cfg = pick_cfg;
	pick_veil = lv_obj_create(lv_layer_top());
	lv_obj_set_size(pick_veil, lv_pct(100), lv_pct(100));
	lv_obj_set_style_bg_color(pick_veil, lv_color_black(), 0);
	lv_obj_set_style_bg_opa(pick_veil, LV_OPA_60, 0);
	lv_obj_set_style_border_width(pick_veil, 0, 0);
	lv_obj_set_style_radius(pick_veil, 0, 0);
	lv_obj_set_style_pad_all(pick_veil, 0, 0);
	lv_obj_set_scrollable(pick_veil, false);
	lv_obj_set_clickable(pick_veil, true);
	lv_obj_set_hidden(pick_veil, true);
	lv_obj_add_event_cb(pick_veil, pick_veil_cb, LV_EVENT_CLICKED, NULL);

	lv_obj_t *card = lv_obj_create(pick_veil);
	lv_obj_set_size(card, cfg->screen_width - 2 * cfg->padding, LV_SIZE_CONTENT);
	lv_obj_set_style_max_height(card, cfg->screen_height - cfg->top_bar_height - 2 * cfg->padding, 0);
	lv_obj_add_style(card, &theme_style_card, 0);
	lv_obj_set_style_radius(card, 16, 0);
	lv_obj_set_style_border_width(card, 0, 0);
	lv_obj_set_style_shadow_width(card, 0, 0);
	lv_obj_set_style_pad_all(card, PICK_CARD_PAD, 0);
	lv_obj_set_style_pad_row(card, PICK_CARD_GAP, 0);
	lv_obj_set_scrollable(card, false);
	lv_obj_set_clickable(card, true);
	lv_obj_set_event_bubble(card, false);
	lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
	lv_obj_align(card, LV_ALIGN_CENTER, 0, cfg->top_bar_height / 2);

	lv_obj_t *title = lv_label_create(card);
	lv_label_set_text(title, tr("musicsettings_scan_the_music_library"));
	lv_label_set_long_mode(title, LV_LABEL_LONG_WRAP);
	lv_obj_set_width(title, lv_pct(100));
	lv_obj_add_style(title, &theme_style_text, 0);
	lv_obj_set_style_text_font(title, bp_is_tempotec_v1() ? &font_ui_18 : &font_ui_24, 0);

	lv_obj_t *note = lv_label_create(card);
	lv_label_set_text(note, tr("libraryscan_choose_folders_note"));
	lv_label_set_long_mode(note, LV_LABEL_LONG_WRAP);
	lv_obj_set_width(note, lv_pct(100));
	lv_obj_add_style(note, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(note, bp_is_tempotec_v1() ? &font_ui_14 : &font_ui_18, 0);

	// The folders scroll; the title and the buttons stay put.
	pick_list = lv_obj_create(card);
	lv_obj_remove_style_all(pick_list);
	lv_obj_set_width(pick_list, lv_pct(100));
	lv_obj_set_height(pick_list, LV_SIZE_CONTENT);
	// What the panel leaves once the title, the note and the buttons are in.
	lv_obj_set_style_max_height(pick_list, cfg->screen_height - cfg->top_bar_height - 2 * cfg->padding - PICK_CHROME_H,
								0);
	lv_obj_set_style_pad_row(pick_list, PICK_LIST_GAP, 0);
	lv_obj_set_flex_flow(pick_list, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_scroll_dir(pick_list, LV_DIR_VER);
	lv_obj_set_scrollbar_mode(pick_list, LV_SCROLLBAR_MODE_AUTO);
	lv_obj_add_event_cb(pick_list, pick_scroll_cb, LV_EVENT_SCROLL, NULL);

	lv_obj_t *buttons = lv_obj_create(card);
	lv_obj_remove_style_all(buttons);
	lv_obj_set_size(buttons, lv_pct(100), LV_SIZE_CONTENT);
	lv_obj_set_style_pad_column(buttons, PICK_BUTTON_GAP, 0);
	lv_obj_set_flex_flow(buttons, LV_FLEX_FLOW_ROW);
	pick_button(buttons, "cancel", false, pick_cancel_cb);
	pick_scan_btn = pick_button(buttons, "scan", true, pick_scan_cb);
}

// Makes up to `n` more rows, after the ones already there.
static void pick_append(int n) {
	int last = pick_built + n < pick_count ? pick_built + n : pick_count;
	for (int i = pick_built; i < last; i++) {
		lv_obj_t *row = lv_btn_create(pick_list);
		lv_obj_set_size(row, lv_pct(100), PICK_ROW_HEIGHT);
		lv_obj_set_style_bg_color(row, theme()->surface_pressed, 0);
		lv_obj_set_style_radius(row, 10, 0);
		lv_obj_set_style_shadow_width(row, 0, 0);
		lv_obj_set_style_border_width(row, 0, 0);
		lv_obj_set_style_pad_hor(row, PICK_ROW_PAD, 0);
		lv_obj_set_style_pad_column(row, PICK_ROW_GAP, 0);
		lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
		lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
		lv_obj_set_scroll_on_focus(row, true);
		lv_obj_add_event_cb(row, pick_row_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);

		lv_obj_t *folder = lv_image_create(row);
		lv_image_set_src(folder, &icon_folder);
		lv_obj_add_style(folder, &theme_style_icon, 0);

		lv_obj_t *label = lv_label_create(row);
		lv_label_set_text(label, pick_names[i]);
		lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
		lv_obj_set_flex_grow(label, 1);
		const lv_font_t *row_font = bp_is_tempotec_v1() ? &font_ui_16 : &font_ui_22;
		lv_obj_set_height(label, lv_font_get_line_height(row_font));
		lv_obj_add_style(label, &theme_style_text, 0);
		lv_obj_set_style_text_font(label, row_font, 0);

		pick_marks[i] = lv_image_create(row);
		lv_image_set_src(pick_marks[i], &icon_check);
		lv_obj_add_style(pick_marks[i], &theme_style_icon, 0);
		lv_obj_set_style_image_recolor(pick_marks[i], theme()->accent, 0);
		lv_obj_set_style_image_recolor_opa(pick_marks[i], LV_OPA_COVER, 0);
		lv_obj_set_style_image_opa(pick_marks[i], pick_on[i] ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
	}
	pick_built = last;
}

// Within a screenful of the last row made, the next batch.
static void pick_scroll_cb(lv_event_t *e) {
	(void)e;
	if (pick_built < pick_count && lv_obj_get_scroll_bottom(pick_list) < lv_obj_get_height(pick_list)) {
		pick_append(PICK_BATCH);
	}
}

static void pick_fill(void) {
	lv_obj_clean(pick_list);
	pick_built = 0;
	pick_append(PICK_BATCH);
}

void libraryscan_choose_folders(void) {
	// While a job is running the folder picker cannot start another one; the
	// running scan's own page is the place to watch or stop it.
	if (library_scan_running()) {
		switch_screen(libraryscan_screen);
		return;
	}
	if (!pick_veil) {
		pick_build();
	}
	pick_read_folders();
	pick_load_selection();
	pick_fill();
	lv_obj_scroll_to_y(pick_list, 0, LV_ANIM_OFF);
	pick_paint();
	lv_obj_set_hidden(pick_veil, false);
	lv_obj_move_foreground(pick_veil);
}

// Both buttons sit at the same place at the bottom of the screen; only one is
// ever visible.
static lv_obj_t *make_button(lv_obj_t *parent, const char *text, lv_color_t colour, lv_event_cb_t cb) {
	lv_obj_t *button = lv_btn_create(parent);
	lv_obj_set_size(button, SCAN_BUTTON_WIDTH, SCAN_BUTTON_HEIGHT);
	lv_obj_align(button, LV_ALIGN_BOTTOM_MID, 0, -SCAN_BUTTON_BOTTOM);
	lv_obj_add_style(button, &theme_style_card_pressed, LV_STATE_PRESSED);
	lv_obj_set_style_bg_color(button, colour, 0);
	lv_obj_set_style_bg_opa(button, LV_OPA_COVER, 0);
	lv_obj_set_style_radius(button, LV_RADIUS_CIRCLE, 0); // Adwaita pill button
	lv_obj_set_style_shadow_width(button, 0, 0);
	lv_obj_add_event_cb(button, cb, LV_EVENT_CLICKED, NULL);

	lv_obj_t *label = lv_label_create(button);
	lv_label_set_text(label, tr(text));
	lv_obj_set_style_text_font(label, &font_ui_24, 0);
	lv_obj_set_style_text_color(label, theme_ink_on(colour), 0);
	lv_obj_center(label);

	return button;
}

// ---------------------------------------------------------------------------
// Fitting the note
//
// The note is the one thing on this page that can give way: the count, the
// status line and the name of the file being read are text, and a cut-off line
// at the bottom is exactly what this page is there to report.
//
// Its source art is 128 px, nearly the whole of the gap between the page title
// and the button on the V1 -- more than the three lines of text put together.
// A flex column centres its overflow, so the note used to run up under the
// title while the file name was clipped at the other end: clipped and not
// pushed off, because this container does not scroll.
//
// So the note takes what the text leaves. Both its draw scale and its layout
// box are set: lv_image_set_scale() alone would leave the image's intrinsic
// 128 px box in the column and push the text out just the same (gridpage.c
// has the same story for the menu art). On the V1 the fit lands at about a
// quarter of the panel width, the proportion the art has on the 480 px
// players, and on those the result is the 128 px it was always drawn at.
// ---------------------------------------------------------------------------

static void scan_fit_note(void) {
	if (!scan_container || !scan_note || !count_label || !status_label || !progress_label ||
		!phase_label || !genius_progress_label || !eta_label) {
		return;
	}

	// Pin any secondary row to a single line so LV_LABEL_LONG_DOT has a
	// boundary to put the ellipsis at. A label left to size itself grows to
	// whatever its text needs and wraps onto a second line before it ever
	// dots; on this panel that second line is exactly the one that used to be
	// pushed under the buttons. Height is pinned whether the label is visible
	// or not so that the pin is already in place the moment scan_refit() shows
	// the row.
	if (bp_is_tempotec_v1()) {
		lv_obj_set_height(progress_label, lv_font_get_line_height(&font_ui_16));
		lv_obj_set_style_text_line_space(progress_label, 0, 0);
		lv_obj_set_height(phase_label, lv_font_get_line_height(&font_ui_22));
		lv_obj_set_style_text_line_space(phase_label, 0, 0);
		lv_obj_set_height(genius_progress_label, lv_font_get_line_height(&font_ui_18));
		lv_obj_set_style_text_line_space(genius_progress_label, 0, 0);
		lv_obj_set_height(eta_label, lv_font_get_line_height(&font_ui_18));
		lv_obj_set_style_text_line_space(eta_label, 0, 0);
	}

	// Hidden objects take no part in flex layout. scan_refit() has already
	// set each row's hidden flag to match whether it carries text, so the
	// layout measured here *is* the layout on screen: the note takes whatever
	// room the visible rows leave it.
	lv_obj_update_layout(scan_container);
	int text = 0;
	int text_rows = 0; // the visible labels below the note
#define MEASURE_ROW(obj)                                                                                              \
	do {                                                                                                               \
		if (!lv_obj_is_hidden(obj)) {                                                                                  \
			text += lv_obj_get_height(obj);                                                                           \
			text_rows++;                                                                                               \
		}                                                                                                              \
	} while (0)
	MEASURE_ROW(count_label);
	MEASURE_ROW(status_label);
	MEASURE_ROW(progress_label);
	MEASURE_ROW(phase_label);
	MEASURE_ROW(genius_progress_label);
	MEASURE_ROW(eta_label);
#undef MEASURE_ROW

	// Gaps between the note and the first text row, and between each pair of
	// text rows after that (N items = N-1 gaps), plus the note's own pad_bottom
	// underneath it. `size` is the side of the note as drawn; the box it
	// occupies in the column is that plus its pad_bottom, which is what
	// lv_obj_set_size() sizes.
	int gaps = text_rows > 0 ? text_rows : 0;
	int pad_bottom = lv_obj_get_style_pad_bottom(scan_note, 0);
	int size = lv_obj_get_height(scan_container) - lv_obj_get_style_pad_top(scan_container, 0) -
			   lv_obj_get_style_pad_bottom(scan_container, 0) - text - SCAN_ROW_GAP * gaps - pad_bottom;

	if (size > SCAN_NOTE_MAX) {
		size = SCAN_NOTE_MAX;
	}
	if (size < SCAN_NOTE_MIN) {
		size = SCAN_NOTE_MIN;
	}

	lv_obj_set_size(scan_note, size, size + pad_bottom);
	lv_image_set_inner_align(scan_note, LV_IMAGE_ALIGN_CENTER);
	lv_image_set_scale(scan_note, (uint32_t)((int64_t)LV_SCALE_NONE * size / (int)icon_music_note.header.h));
}

// ---------------------------------------------------------------------------
// What the user is told about the runs on the scan thread
//
// Detect changes works in the background: a glyph in the status bar while it
// looks over the card, a check mark for a few seconds when it is done, and no
// notice either way. Filing the index again after the scan options changed was
// asked for on that page, so it keeps its notice and its outcome; filed again
// on its own, at the end of a Detect changes run, it says nothing.
// ---------------------------------------------------------------------------

typedef struct {
	library_update_event_t event;
	int added;
	int removed;
	int updated;
} update_note_t;

static bool checking;	   // a Detect changes run is under way
static bool reorganizing;  // library_reorganize() put its notice up

static void update_note_cb(void *user) {
	update_note_t *note = user;

	switch (note->event) {
	case LIBRARY_UPDATE_LOOKING:
		checking = true;
		topbar_set_library_check(TOPBAR_LIBRARY_CHECKING);
		break;

	case LIBRARY_UPDATE_ADDING:
		break;

	case LIBRARY_UPDATE_GENIUS:
		// The graph phase starts after the music scan has finished, by which
		// point the poll timer may have been paused; run it again so the
		// phase line and percentage come up even off the normal scan path.
		checking = true;
		topbar_set_library_check(TOPBAR_LIBRARY_CHECKING);
		lv_timer_resume(poll_timer);
		break;

	case LIBRARY_UPDATE_FINISHED:
		checking = false;
		topbar_set_library_check(TOPBAR_LIBRARY_CHECKED);
		break;

	case LIBRARY_UPDATE_STOPPED:
		if (reorganizing) {
			reorganizing = false;
			toast_busy_end();
		} else if (checking) {
			checking = false;
			topbar_set_library_check(TOPBAR_LIBRARY_IDLE);
		}
		break;

	case LIBRARY_UPDATE_REORGANIZING:
		reorganizing = true;
		toast_busy_dismissable("libraryscan_reorganizing");
		break;

	case LIBRARY_UPDATE_REORGANIZED:
		if (reorganizing) {
			reorganizing = false;
			toast_busy_end();
			toast_success("libraryscan_reorganized");
		}
		break;

	case LIBRARY_UPDATE_SCANNED:
		// The first scan there has ever been turns Detect changes on, unless it
		// was already set either way.
		if (!library_detect_changes_chosen()) {
			library_set_detect_changes(true);
		}
		break;
	}
	free(note);
}

// On the scan thread: everything it says goes over to the interface thread.
static void update_listener(library_update_event_t event, int added, int removed, int updated) {
	update_note_t *note = malloc(sizeof(*note));
	if (!note) {
		return;
	}
	note->event = event;
	note->added = added;
	note->removed = removed;
	note->updated = updated;
	if (!gui_post(update_note_cb, note)) {
		free(note);
	}
}

// How long after the boot-time restore the card is looked over. The timer is
// created by main only after that restore has run (or was not requested), not
// while the pages are still being built: on a slow boot two independently
// created timers could otherwise make the library check run first.
#define BOOT_CHECK_DELAY_MS 5000

static lv_timer_t *boot_check_timer;

// What library_card_returned() does for a card that comes back: Detect changes
// when it is on, and no automatic work when it is off. A card mounted only
// after startup goes through it when it is attached instead (storage), and
// finds the library closed here.
static void boot_check_cb(lv_timer_t *timer) {
	(void)timer;
	boot_check_timer = NULL;
	// A library scanned before Detect changes came on by itself gets it the
	// same way, unless it was set either way since.
	if (!library_detect_changes_chosen() && library_is_open() && library_track_count() > 0) {
		library_set_detect_changes(true);
	}
	library_card_returned(sd_root);
}

void libraryscan_boot_check_schedule(void) {
	if (boot_check_timer) {
		return;
	}
	boot_check_timer = lv_timer_create(boot_check_cb, BOOT_CHECK_DELAY_MS, NULL);
	lv_timer_set_repeat_count(boot_check_timer, 1);
}

void libraryscan_init(gui_config_t *cfg) {
	sd_root = cfg->sd_root_path;
	pick_cfg = cfg;
	library_set_update_listener(update_listener);

	lv_obj_add_style(libraryscan_screen, &theme_style_screen, 0);

	// Centred: this page has no back chevron beside it to align with.
	lv_obj_t *title = settingsrow_title(libraryscan_screen, cfg, "scan_2");
	lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);

	int content_top = settingsrow_content_top(cfg);
	int content_height = cfg->screen_height - content_top;
	// Always stay above the two stacked bottom pills so the last line of text
	// never slides under the "Continue in background" action. The pills sit
	// against the bottom edge with SCAN_BUTTON_BOTTOM padding, the action pill
	// above that, with SCAN_PILL_GAP between them, and SCAN_CONTENT_GAP clear
	// above the background pill.
	content_height -= SCAN_BUTTON_HEIGHT + SCAN_BUTTON_BOTTOM + SCAN_PILL_GAP +
					  SCAN_BG_BUTTON_HEIGHT + SCAN_CONTENT_GAP;
	// Container padding already eats cfg->padding off the top and bottom via
	// lv_obj_set_style_pad_all(); don't subtract it again here.

	lv_obj_t *container = lv_obj_create(libraryscan_screen);
	lv_obj_set_size(container, lv_pct(100), content_height);
	lv_obj_align(container, LV_ALIGN_TOP_LEFT, 0, content_top);
	lv_obj_set_style_bg_opa(container, 0, 0);
	lv_obj_set_style_border_width(container, 0, 0);
	lv_obj_set_style_radius(container, 0, 0);
	lv_obj_set_style_pad_all(container, cfg->padding, 0);
	lv_obj_set_style_pad_gap(container, SCAN_ROW_GAP, 0);
	lv_obj_set_scrollable(container, false);
	lv_obj_set_flex_flow(container, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(container, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	scan_container = container;

	lv_obj_t *note = lv_image_create(container);
	lv_image_set_src(note, &icon_music_note);
	lv_obj_add_style(note, &theme_style_icon, 0);
	lv_obj_set_style_image_recolor_opa(note, LV_OPA_COVER, 0);
	lv_obj_set_style_pad_bottom(note, bp_pick(6, 16), 0);
	scan_note = note;

	// The count is the whole point of the page, so it gets the accent colour
	// and the largest type on it.
	count_label = lv_label_create(container);
	lv_label_set_text(count_label, "0");
	lv_obj_set_style_text_color(count_label, theme()->accent, 0);
	lv_obj_set_style_text_font(count_label, &font_ui_32, 0);

	status_label = lv_label_create(container);
	lv_label_set_text(status_label, tr("libraryscan_tracks_found"));
	// Wrapped inside the panel rather than run past it: a label that sizes
	// itself keeps its single line however wide it gets, and a centred flex
	// column then cuts it off at both edges -- which a translation of "tracks
	// found" is long enough to do on 240 px.
	lv_obj_set_width(status_label, lv_pct(100));
	lv_obj_set_style_text_align(status_label, LV_TEXT_ALIGN_CENTER, 0);
	lv_label_set_long_mode(status_label, LV_LABEL_LONG_WRAP);
	lv_obj_add_style(status_label, &theme_style_text, 0);
	lv_obj_set_style_text_font(status_label, &font_ui_26, 0);

	progress_label = lv_label_create(container);
	lv_label_set_text(progress_label, "");
	lv_label_set_long_mode(progress_label, LV_LABEL_LONG_DOT);
	lv_obj_set_width(progress_label, lv_pct(100));
	lv_obj_add_style(progress_label, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(progress_label, &font_ui_16, 0);
	lv_obj_set_style_text_align(progress_label, LV_TEXT_ALIGN_CENTER, 0);

	// The Smart Mix graph's own status, one line per active phase.
	phase_label = lv_label_create(container);
	lv_label_set_text(phase_label, "");
	lv_obj_set_width(phase_label, lv_pct(100));
	lv_label_set_long_mode(phase_label, LV_LABEL_LONG_DOT);
	lv_obj_set_style_text_align(phase_label, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_add_style(phase_label, &theme_style_text, 0);
	lv_obj_set_style_text_font(phase_label, &font_ui_22, 0);

	// Its completed/total count and percentage while it ranks the library.
	genius_progress_label = lv_label_create(container);
	lv_label_set_text(genius_progress_label, "");
	lv_obj_set_width(genius_progress_label, lv_pct(100));
	lv_label_set_long_mode(genius_progress_label, LV_LABEL_LONG_DOT);
	lv_obj_set_style_text_align(genius_progress_label, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_add_style(genius_progress_label, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(genius_progress_label, &font_ui_18, 0);

	// And how long the ranking work has left, when it can be estimated.
	eta_label = lv_label_create(container);
	lv_label_set_text(eta_label, "");
	lv_obj_set_width(eta_label, lv_pct(100));
	lv_label_set_long_mode(eta_label, LV_LABEL_LONG_DOT);
	lv_obj_set_style_text_align(eta_label, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_add_style(eta_label, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(eta_label, &font_ui_18, 0);

	// All labels are created before the first fit; scan_fit_note() pins any
	// single-line rows to one line height so that LV_LABEL_LONG_DOT can bring
	// the ellipsis in instead of wrapping, then sizes the note art to whatever
	// room is left. It is re-run on every poll so that phase changes (which
	// show and hide the Smart Mix rows) shrink or grow the note accordingly.
	lv_obj_set_hidden(progress_label, true);
	lv_obj_set_hidden(phase_label, true);
	lv_obj_set_hidden(genius_progress_label, true);
	lv_obj_set_hidden(eta_label, true);
	scan_fit_note();

	// Another face or another text size re-measures every line here, so the
	// note has to give way again.
	fonts_register_change(scan_fit_note);

	// The two buttons share the foot of the page: cancel while the scan runs, OK
	// once it is done.
	cancel_button = make_button(libraryscan_screen, "cancel", lv_color_make(210, 66, 58), cancel_cb);
	ok_button = make_button(libraryscan_screen, "ok", theme()->accent, ok_cb);
	lv_obj_set_hidden(ok_button, true);

	// A third, quieter action: leave the page while the job keeps running. It
	// sits above the Cancel/OK pill (the two share SCAN_PILL_GAP between them)
	// and below the content column, which reserves SCAN_CONTENT_GAP of space
	// above it.
	background_button = make_button(libraryscan_screen, "libraryscan_background", theme()->surface_pressed, background_cb);
	lv_obj_set_width(background_button, cfg->screen_width - 2 * cfg->padding);
	lv_obj_set_height(background_button, SCAN_BG_BUTTON_HEIGHT);
	lv_obj_align(background_button, LV_ALIGN_BOTTOM_MID, 0,
				 -(SCAN_BUTTON_BOTTOM + SCAN_BUTTON_HEIGHT + SCAN_PILL_GAP));
	// Quieter look than the Cancel/OK pill: slightly smaller text and the
	// secondary text colour (already applied via theme_ink_on(surface_pressed)).
	lv_obj_t *bg_label = lv_obj_get_child(background_button, 0);
	if (bg_label && bp_is_tempotec_v1()) {
		lv_obj_set_style_text_font(bg_label, &font_ui_18, 0);
	}
	lv_obj_set_hidden(background_button, true);

	poll_timer = lv_timer_create(poll_cb, SCAN_POLL_MS, NULL);
	lv_timer_pause(poll_timer);
	// No interface work while the device is in standby; the busy guard in
	// power.c is what keeps the scan itself alive with the panel dark.
	power_pause_in_standby(poll_timer);
}
