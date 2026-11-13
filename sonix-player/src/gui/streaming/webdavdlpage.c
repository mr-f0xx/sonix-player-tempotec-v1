#include "webdavdlpage.h"

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
#include "src/system/streaming/webdavdl.h"

lv_obj_t *webdavdl_screen;
static lv_obj_t *dl_container;
static lv_obj_t *progress_card, *progress_title, *progress_amount, *progress_bar, *cancel_button;
static lv_obj_t *empty_label;
static webdavdl_entry_t *entries;
static int entry_count;
static bool drawn_busy;
static int drawn_completed;
static lv_timer_t *poll;

static void report_start_failure(const char *err) {
	if (webdavdl_busy() || (err && strcmp(err, "webdav_download_busy") == 0)) {
		toast_plain("webdav_download_already_running");
	} else if (!storage_card_attached() || !storage_sd_writable()) {
		toast_error("webdav_no_card");
	} else if (!webdav_configured()) {
		toast_error("webdav_not_configured");
	} else {
		toast_error((err && err[0]) ? err : "webdav_download_failed");
	}
}

void webdavdlpage_download_tracks(const webdav_entry_t *tracks, int count, int index) {
	char err[128] = "";
	if (!webdavdl_start_tracks(tracks, count, index, err, sizeof(err))) {
		report_start_failure(err);
		return;
	}
	webdavdlpage_open();
}

void webdavdlpage_download_folder(const char *folder_rel_path, const char *folder_title) {
	char err[128] = "";
	if (!webdavdl_start_folder(folder_rel_path, folder_title, err, sizeof(err))) {
		report_start_failure(err);
		return;
	}
	webdavdlpage_open();
}

bool webdavdlpage_present(const webdav_entry_t *entry) {
	return entry && entry->kind == WEBDAV_TRACK && webdavdl_exists(entry);
}

static void cancel_cb(lv_event_t *e) {
	(void)e;
	if (!webdavdl_busy()) {
		return;
	}
	webdavdl_cancel();
	toast_plain("webdav_download_cancelling");
}

static void paint_progress(const webdavdl_progress_t *p) {
	if (!progress_card) {
		return;
	}
	lv_label_set_text(progress_title, p->title[0] ? p->title : tr("webdav_downloading"));
	int file_percent = -1;
	if (p->bytes_total > 0) {
		file_percent = (int)(100.0 * (double)p->bytes_done / (double)p->bytes_total);
		if (file_percent > 100) {
			file_percent = 100;
		}
	}
	int done = p->completed + p->failed;
	if (p->total > 1) {
		int overall = 0;
		if (file_percent >= 0) {
			overall = (int)(100.0 * (done + file_percent / 100.0) / p->total);
		} else {
			overall = (int)(100.0 * done / p->total);
		}
		if (overall > 100) {
			overall = 100;
		}
		if (file_percent >= 0) {
			lv_label_set_text_fmt(progress_amount, "%d / %d \xC2\xB7 %d%%", done + 1, p->total, file_percent);
		} else {
			lv_label_set_text_fmt(progress_amount, "%d / %d", done + 1, p->total);
		}
		lv_bar_set_value(progress_bar, overall, LV_ANIM_OFF);
	} else {
		lv_label_set_text(progress_amount, file_percent >= 0 ? "" : tr("webdav_download_starting"));
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
	lv_label_set_text(label, tr("webdav_download_cancel"));
	lv_obj_center(label);
}

static void play_row(void *user) {
	int index = (int)(intptr_t)user;
	if (index < 0 || index >= entry_count) {
		return;
	}
	const char *path = entries[index].path;
	if (!playlist_is_playable_file(path)) {
		toast_error("webdav_download_gone");
		webdavdlpage_repaint();
		return;
	}
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
	if (path && webdavdl_delete(path)) {
		toast_success("webdav_download_deleted");
	} else {
		toast_error("webdav_download_delete_failed");
	}
	free(path);
	webdavdlpage_repaint();
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
	confirm_show("webdav_download_delete", "webdav_download_delete_note", "delete", delete_confirmed, path);
}

static void menu_row_cb(lv_event_t *e) {
	lv_event_stop_bubbling(e);
	if (switcher_back_drag_active() || player_sheet_drag_active()) {
		return;
	}
	int index = (int)(intptr_t)lv_event_get_user_data(e);
	if (index < 0 || index >= entry_count) {
		return;
	}
	popover_item_t items[] = {
		{.label = "webdav_download_play", .action = play_row, .user = (void *)(intptr_t)index},
		{.label = "webdav_download_delete", .action = delete_row, .user = (void *)(intptr_t)index},
	};
	popover_show(lv_event_get_current_target(e), items, 2);
}

void webdavdlpage_repaint(void) {
	if (!dl_container) {
		return;
	}
	lv_obj_clean(dl_container);
	progress_card = progress_title = progress_amount = progress_bar = cancel_button = NULL;
	empty_label = NULL;

	webdavdl_progress_t p;
	webdavdl_progress(&p);
	bool running = (p.state == WEBDAVDL_RUNNING);
	drawn_busy = running;
	drawn_completed = p.completed + p.failed;
	if (running) {
		build_progress();
		paint_progress(&p);
	}
	if (!entries) {
		entries = calloc(WEBDAVDL_HISTORY_MAX, sizeof(*entries));
	}
	entry_count = entries ? webdavdl_history(entries, WEBDAVDL_HISTORY_MAX) : 0;
	if (!entry_count && !running) {
		empty_label = lv_label_create(dl_container);
		lv_obj_add_style(empty_label, &theme_style_text_dim, 0);
		lv_obj_set_width(empty_label, lv_pct(100));
		lv_label_set_long_mode(empty_label, LV_LABEL_LONG_WRAP);
		lv_label_set_text(empty_label, tr("webdav_downloads_empty"));
	}
	if (entry_count) {
		lv_obj_t *header = settingsrow_add(dl_container, "webdav_downloads_on_card", NULL, NULL, NULL);
		settingsrow_add_icon(header, &icon_download, THEME_SEMANTIC_GREEN);
	}
	for (int i = 0; i < entry_count; i++) {
		const webdavdl_entry_t *entry = &entries[i];
		lv_obj_t *row = settingsrow_action(dl_container, "", play_row_cb, (void *)(intptr_t)i);
		lv_label_set_text(settingsrow_name_label(row), entry->title);

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

		settingsrow_name_lines(row, 2);
		if (entry->folder[0]) {
			settingsrow_add_caption(row, entry->folder);
		}
		settingsrow_add_icon(row, &icon_music_note, THEME_SEMANTIC_CYAN);
	}
}

static void report_finished(void) {
	webdavdl_progress_t p;
	webdavdl_progress(&p);
	if (p.state == WEBDAVDL_IDLE || p.state == WEBDAVDL_RUNNING) {
		return;
	}
	webdavdl_acknowledge();
	if (p.state == WEBDAVDL_DONE && p.failed == 0) {
		toast_success("webdav_download_complete");
	} else if (p.state == WEBDAVDL_CANCELLED) {
		toast_plain("webdav_download_cancelled");
	} else if (p.failed > 0 && p.completed > 0) {
		toast_error("webdav_download_some_failed");
	} else {
		toast_error(p.error[0] ? p.error : "webdav_download_failed");
	}
}

static void poll_cb(lv_timer_t *timer) {
	(void)timer;
	if (lv_screen_active() != webdavdl_screen) {
		return;
	}
	webdavdl_progress_t p;
	webdavdl_progress(&p);
	bool running = (p.state == WEBDAVDL_RUNNING);
	if (running != drawn_busy) {
		webdavdlpage_repaint();
	} else if (running) {
		if (p.completed + p.failed != drawn_completed) {
			webdavdlpage_repaint();
		} else {
			paint_progress(&p);
		}
	}
	if (!running && p.state != WEBDAVDL_IDLE) {
		report_finished();
		webdavdlpage_repaint();
	}
}

void webdavdlpage_watch(void) {
	webdavdl_progress_t p;
	webdavdl_progress(&p);
	if (p.state != WEBDAVDL_RUNNING && p.state != WEBDAVDL_IDLE) {
		report_finished();
	}
}

void webdavdlpage_open(void) {
	if (!webdavdl_screen) {
		return;
	}
	webdavdlpage_repaint();
	switch_screen(webdavdl_screen);
}

static void loaded_cb(lv_event_t *e) {
	(void)e;
	webdavdlpage_repaint();
	lv_timer_resume(poll);
}

static void unloaded_cb(lv_event_t *e) {
	(void)e;
	lv_timer_pause(poll);
}

void webdavdlpage_init(gui_config_t *cfg) {
	webdavdl_screen = lv_obj_create(NULL);
	lv_obj_add_style(webdavdl_screen, &theme_style_screen, 0);
	switcher_attach_back_gesture(webdavdl_screen);
	dl_container = settingsrow_page(webdavdl_screen, cfg, "webdav_downloads");
	player_sheet_attach_drag(dl_container, true);
	webdavdlpage_repaint();
	poll = lv_timer_create(poll_cb, 500, NULL);
	lv_timer_pause(poll);
	lv_obj_add_event_cb(webdavdl_screen, loaded_cb, LV_EVENT_SCREEN_LOADED, NULL);
	lv_obj_add_event_cb(webdavdl_screen, unloaded_cb, LV_EVENT_SCREEN_UNLOADED, NULL);
}
