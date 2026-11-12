#include "subsonicdlpage.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "src/gui/board_profile.h"
#include "src/gui/fonts/fonts.h"
#include "src/gui/nowplaying/player.h"
#include "src/gui/shell/confirm.h"
#include "src/gui/shell/icons.h"
#include "src/gui/shell/popover.h"
#include "src/gui/shell/settingsrow.h"
#include "src/gui/shell/switcher.h"
#include "src/gui/shell/theme.h"
#include "src/gui/shell/toast.h"
#include "src/system/core/lang.h"
#include "src/system/device/system.h"
#include "src/system/playback/device_state.h"
#include "src/system/playback/playlist.h"
#include "src/system/streaming/subsonicdl.h"

// The downloads page: what a download is doing right now, then everything
// already on the card, newest first. Rows are ordinary files -- playing one is
// ordinary local playback -- so the page is mostly a list, with the running
// job's progress card on top of it.
//
// It rebuilds itself only when something changed: a tick at a human pace
// repaints the progress bar in place, and the list is read again when a
// download finishes or a file is deleted. Rebuilding the rows under a finger
// that is scrolling them would fight the finger.

lv_obj_t *subsonicdl_screen;
static gui_config_t *config;
static lv_obj_t *dl_container;
static lv_obj_t *progress_card, *progress_title, *progress_amount, *progress_bar, *cancel_button;
static lv_obj_t *empty_label;
static subsonicdl_entry_t *entries;
static int entry_count;
static bool drawn_busy;
static int drawn_completed;
static lv_timer_t *poll;

// ---------------------------------------------------------------------------
// starting a download
// ---------------------------------------------------------------------------

void subsonicdlpage_download(const subsonic_entry_t *tracks, int count, int index, bool page) {
	if (!tracks || count <= 0 || index < 0 || index >= count) {
		return;
	}
	subsonicdl_request_t request = {
		.tracks = tracks,
		.count = page ? count : index + 1,
		.start = index,
	};
	if (!subsonicdl_start(&request)) {
		// The reason is one of a few, and each has its own sentence: no card,
		// no account, a Raw codec this player cannot read, or a download
		// already running.
		if (subsonicdl_busy()) {
			toast_plain("subsonic_download_already_running");
		} else if (!storage_card_attached() || !storage_sd_writable()) {
			toast_error("subsonic_no_card");
		} else if (!subsonic_configured()) {
			toast_error("subsonic_not_configured");
		} else if (subsonic_quality().format == SUBSONIC_FORMAT_RAW) {
			toast_error("subsonic_raw_unsupported");
		} else {
			toast_error("subsonic_download_cannot_start");
		}
		return;
	}
	subsonicdlpage_open();
}

bool subsonicdlpage_present(const subsonic_entry_t *entry) {
	return entry && entry->kind == SUBSONIC_TRACK && subsonicdl_exists(entry, subsonic_quality());
}

// ---------------------------------------------------------------------------
// the running download
// ---------------------------------------------------------------------------

static void cancel_cb(lv_event_t *e) {
	(void)e;
	if (!subsonicdl_busy()) {
		return;
	}
	subsonicdl_cancel();
	toast_plain("subsonic_download_cancelling");
}

static void paint_progress(const subsonicdl_progress_t *p) {
	if (!progress_card) {
		return;
	}
	lv_label_set_text(progress_title, p->title[0] ? p->title : tr("subsonic_downloading"));
	int file_percent = -1;
	if (p->bytes_total > 0) {
		file_percent = (int)(100.0 * (double)p->bytes_done / (double)p->bytes_total);
		if (file_percent > 100) {
			file_percent = 100;
		}
	}
	int done = p->completed + p->failed;
	if (p->total_tracks > 1) {
		// The bar is the whole job: finished files plus how far into this one
		// the transfer is. The ratio is an estimate because a server may
		// transcode at its own length, but it never goes backwards.
		int overall = 0;
		if (file_percent >= 0) {
			overall = (int)(100.0 * (done + file_percent / 100.0) / p->total_tracks);
		} else {
			overall = (int)(100.0 * done / p->total_tracks);
		}
		if (overall > 100) {
			overall = 100;
		}
		if (file_percent >= 0) {
			lv_label_set_text_fmt(progress_amount, "%d / %d \xC2\xB7 %d%%", done + 1, p->total_tracks, file_percent);
		} else {
			lv_label_set_text_fmt(progress_amount, "%d / %d", done + 1, p->total_tracks);
		}
		lv_bar_set_value(progress_bar, overall, LV_ANIM_OFF);
	} else {
		lv_label_set_text(progress_amount, file_percent >= 0 ? "" : tr("subsonic_download_starting"));
		if (file_percent >= 0) {
			lv_label_set_text_fmt(progress_amount, "%d%%", file_percent);
		}
		lv_bar_set_value(progress_bar, file_percent < 0 ? 0 : file_percent, LV_ANIM_OFF);
	}
}

static void build_progress(void) {
	progress_card = lv_obj_create(dl_container);
	lv_obj_set_width(progress_card, lv_pct(100));
	lv_obj_set_height(progress_card, LV_SIZE_CONTENT);
	lv_obj_add_style(progress_card, &theme_style_card, 0);
	lv_obj_set_style_pad_all(progress_card, 12, 0);
	lv_obj_set_style_pad_row(progress_card, 8, 0);
	lv_obj_set_scrollable(progress_card, false);
	lv_obj_set_flex_flow(progress_card, LV_FLEX_FLOW_COLUMN);

	progress_title = lv_label_create(progress_card);
	lv_obj_set_width(progress_title, lv_pct(100));
	lv_label_set_long_mode(progress_title, LV_LABEL_LONG_DOT);
	lv_obj_add_style(progress_title, &theme_style_text, 0);
	lv_obj_set_style_text_font(progress_title, &font_ui_20, 0);

	progress_bar = lv_bar_create(progress_card);
	lv_obj_set_size(progress_bar, lv_pct(100), 12);
	lv_bar_set_range(progress_bar, 0, 100);
	lv_obj_set_style_bg_color(progress_bar, theme()->surface_pressed, LV_PART_MAIN);
	lv_obj_set_style_bg_opa(progress_bar, LV_OPA_COVER, LV_PART_MAIN);
	lv_obj_set_style_bg_color(progress_bar, theme()->accent, LV_PART_INDICATOR);
	lv_obj_set_style_radius(progress_bar, LV_RADIUS_CIRCLE, LV_PART_MAIN);
	lv_obj_set_style_radius(progress_bar, LV_RADIUS_CIRCLE, LV_PART_INDICATOR);

	progress_amount = lv_label_create(progress_card);
	lv_obj_set_width(progress_amount, lv_pct(100));
	lv_label_set_long_mode(progress_amount, LV_LABEL_LONG_WRAP);
	lv_obj_add_style(progress_amount, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(progress_amount, &font_ui_18, 0);

	// The download is not modal and the card is the only place it can be
	// stopped from: leaving the page, playing something else and locking the
	// screen all leave it running.
	cancel_button = lv_btn_create(progress_card);
	lv_obj_set_size(cancel_button, lv_pct(100), bp_pick(34, 52));
	lv_obj_add_style(cancel_button, &theme_style_card_pressed, 0);
	lv_obj_set_style_bg_color(cancel_button, theme()->surface_pressed, 0);
	lv_obj_set_style_border_width(cancel_button, 0, 0);
	lv_obj_set_style_shadow_width(cancel_button, 0, 0);
	lv_obj_add_event_cb(cancel_button, cancel_cb, LV_EVENT_CLICKED, NULL);
	lv_obj_t *label = lv_label_create(cancel_button);
	lv_obj_add_style(label, &theme_style_text, 0);
	lv_obj_set_style_text_font(label, &font_ui_18, 0);
	lv_label_set_text(label, tr("subsonic_download_cancel"));
	lv_obj_center(label);
}

// ---------------------------------------------------------------------------
// what is on the card
// ---------------------------------------------------------------------------

static void play_row(void *user) {
	int index = (int)(intptr_t)user;
	if (index < 0 || index >= entry_count) {
		return;
	}
	const char *path = entries[index].path;
	if (!playlist_is_playable_file(path)) {
		toast_error("subsonic_download_gone");
		subsonicdlpage_repaint();
		return;
	}
	// Ordinary local playback: nothing here is a stream, and the queue is the
	// one file the row stands for.
	const char *paths[1] = {path};
	device_state_play_list_ordered(paths, 1, 0);
	player_refresh_now_playing();
	player_sheet_open(true);
}

static void play_row_cb(lv_event_t *e) {
	if (switcher_back_drag_active() || player_sheet_drag_active()) {
		return;
	}
	play_row(lv_event_get_user_data(e));
}

static void delete_confirmed(void *user) {
	char *path = user;
	if (path && subsonicdl_delete(path)) {
		toast_success("subsonic_download_deleted");
	} else {
		toast_error("subsonic_download_delete_failed");
	}
	free(path);
	subsonicdlpage_repaint();
}

static void delete_row(void *user) {
	int index = (int)(intptr_t)user;
	if (index < 0 || index >= entry_count) {
		return;
	}
	char *path = strdup(entries[index].path);
	if (!path) {
		return;
	}
	confirm_show("subsonic_download_delete", "subsonic_download_delete_note", "delete", delete_confirmed, path);
}

static void menu_row_cb(lv_event_t *e) {
	lv_event_stop_bubbling(e); // the tap belongs to the menu, not to the row
	if (switcher_back_drag_active() || player_sheet_drag_active()) {
		return;
	}
	int index = (int)(intptr_t)lv_event_get_user_data(e);
	if (index < 0 || index >= entry_count) {
		return;
	}
	popover_item_t items[] = {
		{.label = "subsonic_download_play", .action = play_row, .user = (void *)(intptr_t)index},
		{.label = "subsonic_download_delete", .action = delete_row, .user = (void *)(intptr_t)index},
	};
	popover_show(lv_event_get_current_target(e), items, 2);
}

// ---------------------------------------------------------------------------
// the page
// ---------------------------------------------------------------------------

// Reads the card again and draws it. Called on load, when a download ends and
// after a delete; not on the tick.
void subsonicdlpage_repaint(void) {
	if (!dl_container) {
		return;
	}
	lv_obj_clean(dl_container);
	progress_card = progress_title = progress_amount = progress_bar = cancel_button = NULL;
	empty_label = NULL;
	subsonicdl_progress_t p;
	subsonicdl_progress(&p);
	drawn_busy = p.running;
	drawn_completed = p.completed + p.failed;
	if (p.running) {
		build_progress();
		paint_progress(&p);
	}
	if (!entries) {
		entries = calloc(SUBSONICDL_HISTORY_MAX, sizeof(*entries));
	}
	entry_count = entries ? subsonicdl_history(entries, SUBSONICDL_HISTORY_MAX) : 0;
	if (!entry_count && !p.running) {
		empty_label = lv_label_create(dl_container);
		lv_obj_add_style(empty_label, &theme_style_text_dim, 0);
		lv_obj_set_width(empty_label, lv_pct(100));
		lv_label_set_long_mode(empty_label, LV_LABEL_LONG_WRAP);
		lv_label_set_text(empty_label, tr("subsonic_downloads_empty"));
	}
	if (entry_count) {
		lv_obj_t *header = settingsrow_add(dl_container, "subsonic_downloads_on_card", NULL, NULL, NULL);
		settingsrow_add_icon(header, &icon_download, THEME_SEMANTIC_GREEN);
	}
	for (int i = 0; i < entry_count; i++) {
		const subsonicdl_entry_t *entry = &entries[i];
		lv_obj_t *row = settingsrow_add(dl_container, "", NULL, play_row_cb, (void *)(intptr_t)i);
		lv_label_set_text(settingsrow_name_label(row), entry->title); // data, not a tag
		settingsrow_name_lines(row, 2);
		if (entry->artist[0] || entry->album[0]) {
			char detail[352];
			if (entry->artist[0] && entry->album[0]) {
				snprintf(detail, sizeof(detail), "%.127s \xC2\xB7 %.199s", entry->artist, entry->album);
			} else {
				snprintf(detail, sizeof(detail), "%.350s", entry->artist[0] ? entry->artist : entry->album);
			}
			settingsrow_add_caption(row, detail);
		}
		settingsrow_add_icon(row, &icon_music_note, THEME_SEMANTIC_CYAN);
		// The row's own menu: play, or delete and free the space again.
		lv_obj_t *dots = lv_btn_create(row);
		lv_obj_set_size(dots, bp_pick(28, 40), bp_pick(28, 40));
		lv_obj_set_style_bg_opa(dots, LV_OPA_TRANSP, 0);
		lv_obj_set_style_border_width(dots, 0, 0);
		lv_obj_set_style_shadow_width(dots, 0, 0);
		lv_obj_set_style_pad_all(dots, 0, 0);
		lv_obj_align(dots, LV_ALIGN_RIGHT_MID, 0, 0);
		lv_obj_set_ext_click_area(dots, 10);
		lv_obj_add_event_cb(dots, menu_row_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
		lv_obj_t *dots_label = lv_label_create(dots);
		lv_obj_add_style(dots_label, &theme_style_text_dim, 0);
		lv_label_set_text(dots_label, "\xE2\x8B\xAE");
		lv_obj_center(dots_label);
	}
}

static void loaded_cb(lv_event_t *e);
static void unloaded_cb(lv_event_t *e);

// Tells the user how the last job ended, once. A job is reported whether or not
// this page is the one on screen: a download started from an album and left to
// run while the user browses elsewhere is exactly the case that needs the
// toast. `subsonicdl_acknowledge()` then silences the state until the next job.
static void report_finished(void) {
	subsonicdl_progress_t p;
	subsonicdl_progress(&p);
	if (p.state == SUBSONICDL_IDLE) {
		return;
	}
	subsonicdl_acknowledge();
	if (p.state == SUBSONICDL_DONE) {
		toast_success("subsonic_download_complete");
	} else if (p.state == SUBSONICDL_CANCELLED) {
		toast_plain("subsonic_download_cancelled");
	} else if (p.failed > 0 && p.completed > 0) {
		toast_error("subsonic_download_some_failed");
	} else {
		toast_error(p.error[0] ? p.error : "subsonic_download_failed");
	}
}

static void poll_cb(lv_timer_t *timer) {
	(void)timer;
	// Not on screen: nothing to paint, and the toast is the Subsonic page's
	// timer's business, so that one job is reported exactly once.
	if (lv_screen_active() != subsonicdl_screen) {
		return;
	}
	subsonicdl_progress_t p;
	subsonicdl_progress(&p);
	if (p.running != drawn_busy) {
		subsonicdlpage_repaint(); // a job started or ended: the list changed
	} else if (p.running) {
		paint_progress(&p);
	}
	if (!p.running) {
		report_finished();
		subsonicdlpage_repaint();
	}
}

// Called from the Subsonic page's own tick, which runs whatever is on screen:
// a job that ends while the user is browsing or playing somewhere else still
// says how it went. It does no I/O beyond reading the worker's counters, and
// does nothing at all between jobs.
void subsonicdlpage_watch(void) {
	subsonicdl_progress_t p;
	subsonicdl_progress(&p);
	if (!p.running) {
		report_finished();
	}
}

void subsonicdlpage_open(void) {
	if (!subsonicdl_screen) {
		return;
	}
	subsonicdlpage_repaint();
	switch_screen(subsonicdl_screen);
}

void subsonicdlpage_init(gui_config_t *cfg) {
	config = cfg;
	subsonicdl_screen = lv_obj_create(NULL);
	lv_obj_add_style(subsonicdl_screen, &theme_style_screen, 0);
	switcher_attach_back_gesture(subsonicdl_screen);
	dl_container = settingsrow_page(subsonicdl_screen, cfg, "subsonic_downloads");
	player_sheet_attach_drag(dl_container, true);
	subsonicdlpage_repaint();
	// Two hertz, and only while the page is on screen: the painted numbers are
	// the only thing this touches.
	poll = lv_timer_create(poll_cb, 500, NULL);
	lv_timer_pause(poll);
	lv_obj_add_event_cb(subsonicdl_screen, loaded_cb, LV_EVENT_SCREEN_LOADED, NULL);
	lv_obj_add_event_cb(subsonicdl_screen, unloaded_cb, LV_EVENT_SCREEN_UNLOADED, NULL);
}

static void loaded_cb(lv_event_t *e) {
	(void)e;
	subsonicdlpage_repaint();
	lv_timer_resume(poll);
}

static void unloaded_cb(lv_event_t *e) {
	(void)e;
	lv_timer_pause(poll);
}
