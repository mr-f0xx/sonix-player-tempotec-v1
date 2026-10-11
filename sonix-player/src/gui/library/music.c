#include "music.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "lvgl/lvgl.h"

#include "src/gui/board_profile.h"
#include "src/gui/fonts/fonts.h"
#include "src/gui/library/browser.h"
#include "src/gui/library/libraryscan.h"
#include "src/gui/library/medialist.h"
#include "src/gui/library/playlistpage.h"
#include "src/gui/library/search.h"
#include "src/gui/nowplaying/coverflow.h"
#include "src/gui/nowplaying/player.h"
#include "src/gui/shell/gridpage.h"
#include "src/gui/shell/gui.h"
#include "src/gui/shell/icons.h"
#include "src/gui/shell/popover.h"
#include "src/gui/shell/settingsrow.h"
#include "src/gui/shell/switcher.h"
#include "src/gui/shell/theme.h"
#include "src/gui/settings/musicsettings.h"
#include "src/system/core/lang.h"
#include "src/system/library/audiobookdb.h"
#include "src/system/library/library.h"
#include "src/system/playback/device_state.h"
#include "src/system/playback/playlist.h"

lv_obj_t *music_screen;

#define TOKYO_RECENT_COUNT 2
#define TOKYO_BROWSE_COUNT 4

typedef struct {
	lv_obj_t *button;
	lv_obj_t *icon;
	lv_obj_t *title;
	lv_obj_t *artist;
	int queue_index;
	unsigned queue_revision;
} recent_row_t;

typedef struct {
	lv_obj_t *icon;
	theme_semantic_t tone;
} browse_icon_t;

static recent_row_t recent_rows[TOKYO_RECENT_COUNT];
static browse_icon_t browse_icons[TOKYO_BROWSE_COUNT];
static lv_obj_t *tokyo_music_panel;
static lv_obj_t *tokyo_recent_card;
static lv_obj_t *tokyo_recent_empty;
static lv_obj_t *tokyo_scan_button;
static int cached_queue_count = -1;
static int cached_queue_current = -2;
static unsigned cached_queue_revision = (unsigned)-1;
static lv_obj_t *tile_grid;
static lv_obj_t *playlists_icon;
static gui_config_t *music_cfg;
static lv_obj_t *empty_panel;

// The index tiles load their list from the database and then switch screen,
// so each gets a tiny opener instead of a target screen pointer.
static void open_all_tracks(void) { medialist_open(tr("music_all_tracks"), LIBRARY_LIST_TRACKS, LIBRARY_FILTER_NONE, NULL); }
static void open_albums(void) { medialist_open(tr("albums"), LIBRARY_LIST_ALBUMS, LIBRARY_FILTER_NONE, NULL); }
static void open_artists(void) { medialist_open(tr("artists"), LIBRARY_LIST_ARTISTS, LIBRARY_FILTER_NONE, NULL); }
static void open_album_artists(void) {
	medialist_open(tr("music_album_artists"), LIBRARY_LIST_ALBUM_ARTISTS, LIBRARY_FILTER_NONE, NULL);
}
static void open_genres(void) { medialist_open(tr("music_genres"), LIBRARY_LIST_GENRES, LIBRARY_FILTER_NONE, NULL); }

// Browse and Playlists trade places.
static void open_tile_slot(void) {
	if (musicsettings_playlists_first()) {
		playlistpage_open();
	} else {
		switch_screen(browser_screen);
	}
}

static void playlists_cb(lv_event_t *e) {
	(void)e;
	if (musicsettings_playlists_first()) {
		switch_screen(browser_screen);
	} else {
		playlistpage_open();
	}
}

static void search_cb(lv_event_t *e) {
	(void)e;
	search_open();
}

static void favourites_cb(lv_event_t *e) {
	(void)e;
	medialist_open(tr("favourites"), LIBRARY_LIST_FAVOURITES, LIBRARY_FILTER_NONE, NULL);
}

static void scan_cb(lv_event_t *e) {
	(void)e;
	libraryscan_choose_folders();
}

static void compact_playlists_action(void *unused) {
	(void)unused;
	playlists_cb(NULL);
}
static void compact_favourites_action(void *unused) {
	(void)unused;
	favourites_cb(NULL);
}
static void compact_search_action(void *unused) {
	(void)unused;
	search_cb(NULL);
}
static void compact_all_tracks_action(void *unused) {
	(void)unused;
	open_all_tracks();
}
static void compact_album_artists_action(void *unused) {
	(void)unused;
	open_album_artists();
}
static void compact_genres_action(void *unused) {
	(void)unused;
	open_genres();
}

static void compact_more_cb(lv_event_t *e) {
	popover_item_t items[] = {
		{musicsettings_playlists_first() ? "music_browse" : "playlists", compact_playlists_action, NULL, false},
		{"music_all_tracks", compact_all_tracks_action, NULL, false},
		{"music_album_artists", compact_album_artists_action, NULL, false},
		{"music_genres", compact_genres_action, NULL, false},
		{"favourites", compact_favourites_action, NULL, false},
		{"search", compact_search_action, NULL, false},
	};
	popover_show(lv_event_get_target(e), items, (int)(sizeof(items) / sizeof(items[0])));
}

static void music_recent_refresh(void);
static void music_update_visibility(void);

static void recent_track_clicked_cb(lv_event_t *e) {
	if (player_sheet_drag_active() || switcher_back_drag_active()) {
		return;
	}
	recent_row_t *row = lv_event_get_user_data(e);
	if (!row || row->queue_index < 0) {
		return;
	}
	if (row->queue_revision != playlist_revision()) {
		music_recent_refresh();
		return;
	}
	char path[512];
	if (!playlist_path_at(row->queue_index, path, sizeof(path))) {
		music_recent_refresh();
		return;
	}
	if (device_state_play_queue_index(row->queue_index)) {
		player_refresh_now_playing();
		player_sheet_open(true);
	}
}

static void label_set_wrapped_translation(lv_obj_t *label, const char *key, bool wrap);

static lv_obj_t *make_browse_button(lv_obj_t *parent, const char *label_key, const lv_image_dsc_t *source,
									 theme_semantic_t tone, bool two_lines, lv_event_cb_t cb, int index) {
	lv_obj_t *button = lv_btn_create(parent);
	int cell_width = (music_cfg->screen_width - 2 * music_cfg->padding - 14 - 4) / 2;
	lv_obj_set_size(button, cell_width, 42);
	lv_obj_set_style_bg_color(button, theme()->surface_pressed, 0);
	lv_obj_set_style_bg_opa(button, LV_OPA_40, 0);
	lv_obj_add_style(button, &theme_style_card_pressed, LV_STATE_PRESSED);
	lv_obj_set_style_border_width(button, 0, 0);
	lv_obj_set_style_radius(button, bp_tile_radius(), 0);
	lv_obj_set_style_pad_hor(button, 5, 0);
	lv_obj_set_style_pad_ver(button, 3, 0);
	lv_obj_set_style_pad_column(button, 6, 0);
	lv_obj_set_flex_flow(button, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(button, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_event_bubble(button, true);
	lv_obj_add_event_cb(button, cb, LV_EVENT_CLICKED, NULL);

	lv_obj_t *icon = lv_image_create(button);
	lv_image_set_src(icon, source);
	lv_obj_add_style(icon, &theme_style_icon, 0);
	lv_obj_set_size(icon, 17, 17);
	lv_image_set_inner_align(icon, LV_IMAGE_ALIGN_CENTER);
	int source_side = LV_MAX((int)source->header.w, (int)source->header.h);
	lv_image_set_scale(icon, source_side ? (uint32_t)(LV_SCALE_NONE * 17 / source_side) : LV_SCALE_NONE);
	lv_obj_set_style_image_recolor(icon, theme_semantic_color(tone), 0);
	lv_obj_set_style_image_recolor_opa(icon, LV_OPA_COVER, 0);
	if (index >= 0 && index < TOKYO_BROWSE_COUNT) {
		browse_icons[index] = (browse_icon_t){.icon = icon, .tone = tone};
	}

		lv_obj_t *label = lv_label_create(button);
		label_set_wrapped_translation(label, label_key, two_lines);
		lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
	lv_obj_set_flex_grow(label, 1);
	lv_obj_set_style_text_line_space(label, 0, 0);
	lv_obj_add_style(label, &theme_style_text, 0);
	lv_obj_set_style_text_font(label, &font_ui_14, 0);
	lv_obj_set_height(label, lv_font_get_line_height(&font_ui_14) * (two_lines ? 2 : 1));
	return button;
}

static void browse_artists_cb(lv_event_t *e) { (void)e; open_artists(); }
static void browse_albums_cb(lv_event_t *e) { (void)e; open_albums(); }
static void browse_album_artists_cb(lv_event_t *e) { (void)e; open_album_artists(); }
static void browse_search_cb(lv_event_t *e) { (void)e; search_cb(NULL); }
static void shortcut_all_tracks_cb(lv_event_t *e) { (void)e; open_all_tracks(); }
static void shortcut_genres_cb(lv_event_t *e) { (void)e; open_genres(); }
static void shortcut_files_cb(lv_event_t *e) { (void)e; switch_screen(browser_screen); }

static void label_set_wrapped_translation(lv_obj_t *label, const char *key, bool wrap) {
	const char *text = tr(key);
	if (!wrap) {
		lv_label_set_text(label, text);
		return;
	}
	const char *split = strchr(text, ' ');
	if (!split) {
		lv_label_set_text(label, text);
		return;
	}
	char wrapped[256];
	snprintf(wrapped, sizeof(wrapped), "%.*s\n%s", (int)(split - text), text, split + 1);
	lv_label_set_text(label, wrapped);
}

static lv_obj_t *music_shortcut_card(lv_obj_t *parent) {
	static const struct {
		const char *label;
		const lv_image_dsc_t *icon;
		theme_semantic_t tone;
		bool wrap;
		lv_event_cb_t callback;
	} rows[] = {
		{"music_all_tracks", &icon_music2, THEME_SEMANTIC_CYAN, false, shortcut_all_tracks_cb},
		{"music_album_artist", &icon_artist_album, THEME_SEMANTIC_BLUE, true, browse_album_artists_cb},
		{"music_genres", &icon_genre, THEME_SEMANTIC_AMBER, false, shortcut_genres_cb},
		{"file_explorer", &icon_folder, THEME_SEMANTIC_GREEN, true, shortcut_files_cb},
		{"favourites", &icon_star, THEME_SEMANTIC_RED, false, favourites_cb},
	};
	lv_obj_t *card = lv_obj_create(parent);
	lv_obj_set_width(card, lv_pct(100));
	lv_obj_set_height(card, LV_SIZE_CONTENT);
	lv_obj_add_style(card, &theme_style_card, 0);
	lv_obj_set_style_radius(card, bp_tile_radius(), 0);
	lv_obj_set_style_border_width(card, 0, 0);
	lv_obj_set_style_shadow_width(card, 0, 0);
	lv_obj_set_style_pad_all(card, 4, 0);
	lv_obj_set_scrollable(card, false);
	lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(card, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

	for (size_t i = 0; i < sizeof(rows) / sizeof(rows[0]); i++) {
		lv_obj_t *button = lv_btn_create(card);
		lv_obj_set_size(button, lv_pct(100), rows[i].wrap ? 42 : 38);
		lv_obj_set_style_bg_opa(button, LV_OPA_TRANSP, 0);
		lv_obj_add_style(button, &theme_style_card_pressed, LV_STATE_PRESSED);
		lv_obj_set_style_border_width(button, 0, 0);
		if (i + 1 < sizeof(rows) / sizeof(rows[0])) {
			lv_obj_set_style_border_side(button, LV_BORDER_SIDE_BOTTOM, 0);
			lv_obj_set_style_border_width(button, 1, 0);
			lv_obj_set_style_border_color(button, theme()->text_secondary, 0);
			lv_obj_set_style_border_opa(button, LV_OPA_20, 0);
		}
		lv_obj_set_style_radius(button, 0, 0);
		lv_obj_set_style_pad_hor(button, 5, 0);
		lv_obj_set_style_pad_column(button, 7, 0);
		lv_obj_set_flex_flow(button, LV_FLEX_FLOW_ROW);
		lv_obj_set_flex_align(button, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
		lv_obj_set_event_bubble(button, true);
		lv_obj_add_event_cb(button, rows[i].callback, LV_EVENT_CLICKED, NULL);

		lv_obj_t *icon = lv_image_create(button);
		lv_image_set_src(icon, rows[i].icon);
		lv_obj_add_style(icon, &theme_style_icon, 0);
		lv_obj_set_size(icon, 18, 18);
		int side = LV_MAX((int)rows[i].icon->header.w, (int)rows[i].icon->header.h);
		lv_image_set_scale(icon, side ? (uint32_t)(LV_SCALE_NONE * 18 / side) : LV_SCALE_NONE);
		lv_image_set_inner_align(icon, LV_IMAGE_ALIGN_CENTER);
		lv_obj_set_style_image_recolor(icon, theme_semantic_color(rows[i].tone), 0);
		lv_obj_set_style_image_recolor_opa(icon, LV_OPA_COVER, 0);

		lv_obj_t *label = lv_label_create(button);
		label_set_wrapped_translation(label, rows[i].label, rows[i].wrap);
		lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
		lv_obj_set_flex_grow(label, 1);
		lv_obj_add_style(label, &theme_style_text, 0);
		lv_obj_set_style_text_font(label, &font_ui_14, 0);
		lv_obj_set_height(label, lv_font_get_line_height(&font_ui_14) * (rows[i].wrap ? 2 : 1));

		lv_obj_t *chevron = lv_image_create(button);
		lv_image_set_src(chevron, &icon_chevron_right);
		lv_obj_add_style(chevron, &theme_style_icon, 0);
		lv_obj_set_size(chevron, 14, 14);
		lv_image_set_scale(chevron, (uint32_t)(LV_SCALE_NONE * 14 / 36));
		lv_image_set_inner_align(chevron, LV_IMAGE_ALIGN_CENTER);
		lv_obj_set_style_image_opa(chevron, LV_OPA_60, 0);
	}
	return card;
}

static void tokyo_music_build(gui_config_t *cfg) {
	if (!bp_is_tempotec_v1()) {
		return;
	}
	int top = settingsrow_content_top(cfg);
	tokyo_music_panel = lv_obj_create(music_screen);
	lv_obj_set_size(tokyo_music_panel, lv_pct(100), cfg->screen_height - top);
	lv_obj_align(tokyo_music_panel, LV_ALIGN_TOP_LEFT, 0, top);
	lv_obj_set_style_bg_opa(tokyo_music_panel, LV_OPA_TRANSP, 0);
	lv_obj_set_style_border_width(tokyo_music_panel, 0, 0);
	lv_obj_set_style_radius(tokyo_music_panel, 0, 0);
	lv_obj_set_style_pad_hor(tokyo_music_panel, cfg->padding, 0);
	lv_obj_set_style_pad_ver(tokyo_music_panel, 4, 0);
	lv_obj_set_style_pad_gap(tokyo_music_panel, 6, 0);
	lv_obj_set_flex_flow(tokyo_music_panel, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(tokyo_music_panel, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START);
	lv_obj_set_scroll_dir(tokyo_music_panel, LV_DIR_VER);
	lv_obj_set_scrollbar_mode(tokyo_music_panel, LV_SCROLLBAR_MODE_AUTO);
	lv_obj_set_hidden(tokyo_music_panel, true);

	lv_obj_t *recent_heading = lv_label_create(tokyo_music_panel);
	lv_label_set_text(recent_heading, tr("radio_recently_played"));
	lv_obj_add_style(recent_heading, &theme_style_text, 0);
	lv_obj_set_style_text_font(recent_heading, &font_ui_14, 0);

	tokyo_recent_card = lv_obj_create(tokyo_music_panel);
	lv_obj_set_width(tokyo_recent_card, lv_pct(100));
	lv_obj_set_height(tokyo_recent_card, LV_SIZE_CONTENT);
	lv_obj_add_style(tokyo_recent_card, &theme_style_card, 0);
	lv_obj_set_style_radius(tokyo_recent_card, bp_tile_radius(), 0);
	lv_obj_set_style_border_width(tokyo_recent_card, 0, 0);
	lv_obj_set_style_shadow_width(tokyo_recent_card, 0, 0);
	lv_obj_set_style_pad_all(tokyo_recent_card, 3, 0);
	lv_obj_set_style_pad_gap(tokyo_recent_card, 0, 0);
	lv_obj_set_scrollable(tokyo_recent_card, false);
	lv_obj_set_flex_flow(tokyo_recent_card, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(tokyo_recent_card, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

	tokyo_recent_empty = lv_label_create(tokyo_recent_card);
	lv_label_set_text(tokyo_recent_empty, tr("music_recent_queue_empty"));
	lv_label_set_long_mode(tokyo_recent_empty, LV_LABEL_LONG_DOT);
	lv_obj_set_width(tokyo_recent_empty, lv_pct(100));
	lv_obj_set_height(tokyo_recent_empty, 30);
	lv_obj_add_style(tokyo_recent_empty, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(tokyo_recent_empty, &font_ui_14, 0);
	lv_obj_set_style_text_align(tokyo_recent_empty, LV_TEXT_ALIGN_CENTER, 0);

	for (int i = 0; i < TOKYO_RECENT_COUNT; i++) {
		recent_row_t *row = &recent_rows[i];
		row->queue_index = -1;
		row->button = lv_btn_create(tokyo_recent_card);
		lv_obj_set_size(row->button, lv_pct(100), 39);
		lv_obj_set_style_bg_opa(row->button, LV_OPA_TRANSP, 0);
		lv_obj_add_style(row->button, &theme_style_card_pressed, LV_STATE_PRESSED);
		lv_obj_set_style_border_width(row->button, 0, 0);
		if (i + 1 < TOKYO_RECENT_COUNT) {
			lv_obj_set_style_border_side(row->button, LV_BORDER_SIDE_BOTTOM, 0);
			lv_obj_set_style_border_width(row->button, 1, 0);
			lv_obj_set_style_border_color(row->button, theme()->text_secondary, 0);
			lv_obj_set_style_border_opa(row->button, LV_OPA_20, 0);
		}
		lv_obj_set_style_radius(row->button, 0, 0);
		lv_obj_set_style_pad_hor(row->button, 4, 0);
		lv_obj_set_style_pad_column(row->button, 7, 0);
		lv_obj_set_flex_flow(row->button, LV_FLEX_FLOW_ROW);
		lv_obj_set_flex_align(row->button, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
		lv_obj_set_event_bubble(row->button, true);
		lv_obj_add_event_cb(row->button, recent_track_clicked_cb, LV_EVENT_CLICKED, row);

		row->icon = lv_image_create(row->button);
		lv_image_set_src(row->icon, &icon_album);
		lv_obj_set_size(row->icon, 25, 25);
		lv_image_set_inner_align(row->icon, LV_IMAGE_ALIGN_CENTER);
		lv_image_set_scale(row->icon, (uint32_t)(LV_SCALE_NONE * 25 / 32));
		lv_obj_add_style(row->icon, &theme_style_icon, 0);
		lv_obj_set_style_image_recolor(row->icon, theme_semantic_color(THEME_SEMANTIC_PURPLE), 0);
		lv_obj_set_style_image_recolor_opa(row->icon, LV_OPA_COVER, 0);

		lv_obj_t *copy = lv_obj_create(row->button);
		lv_obj_remove_style_all(copy);
		lv_obj_set_flex_grow(copy, 1);
		lv_obj_set_height(copy, LV_SIZE_CONTENT);
		lv_obj_set_style_pad_all(copy, 0, 0);
		lv_obj_set_style_pad_row(copy, 1, 0);
		lv_obj_set_scrollable(copy, false);
		lv_obj_set_flex_flow(copy, LV_FLEX_FLOW_COLUMN);
		lv_obj_set_flex_align(copy, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

		row->title = lv_label_create(copy);
		lv_label_set_long_mode(row->title, LV_LABEL_LONG_DOT);
		lv_obj_set_width(row->title, lv_pct(100));
		lv_obj_set_height(row->title, lv_font_get_line_height(&font_ui_14));
		lv_obj_add_style(row->title, &theme_style_text, 0);
		lv_obj_set_style_text_font(row->title, &font_ui_14, 0);

		row->artist = lv_label_create(copy);
		lv_label_set_long_mode(row->artist, LV_LABEL_LONG_DOT);
		lv_obj_set_width(row->artist, lv_pct(100));
		lv_obj_set_height(row->artist, lv_font_get_line_height(&font_ui_14));
		lv_obj_add_style(row->artist, &theme_style_text_dim, 0);
		lv_obj_set_style_text_font(row->artist, &font_ui_14, 0);
		lv_obj_set_hidden(row->button, true);
	}

	lv_obj_t *queue_note = lv_label_create(tokyo_music_panel);
	lv_label_set_text(queue_note, tr("music_queue_recent_note"));
	lv_label_set_long_mode(queue_note, LV_LABEL_LONG_DOT);
	lv_obj_set_width(queue_note, lv_pct(100));
	lv_obj_set_height(queue_note, lv_font_get_line_height(&font_ui_14));
	lv_obj_add_style(queue_note, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(queue_note, &font_ui_14, 0);

	lv_obj_t *browse_card = lv_obj_create(tokyo_music_panel);
	lv_obj_set_width(browse_card, lv_pct(100));
	lv_obj_set_height(browse_card, LV_SIZE_CONTENT);
	lv_obj_add_style(browse_card, &theme_style_card, 0);
	lv_obj_set_style_radius(browse_card, bp_tile_radius(), 0);
	lv_obj_set_style_border_width(browse_card, 0, 0);
	lv_obj_set_style_shadow_width(browse_card, 0, 0);
	lv_obj_set_style_pad_all(browse_card, 7, 0);
	lv_obj_set_style_pad_gap(browse_card, 5, 0);
	lv_obj_set_scrollable(browse_card, false);
	lv_obj_set_flex_flow(browse_card, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(browse_card, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

	lv_obj_t *browse_heading = lv_label_create(browse_card);
	lv_label_set_text(browse_heading, tr("music_browse"));
	lv_obj_add_style(browse_heading, &theme_style_text, 0);
	lv_obj_set_style_text_font(browse_heading, &font_ui_14, 0);

	lv_obj_t *browse_grid = lv_obj_create(browse_card);
	lv_obj_set_size(browse_grid, lv_pct(100), LV_SIZE_CONTENT);
	lv_obj_set_style_bg_opa(browse_grid, LV_OPA_TRANSP, 0);
	lv_obj_set_style_border_width(browse_grid, 0, 0);
	lv_obj_set_style_pad_all(browse_grid, 0, 0);
	lv_obj_set_style_pad_gap(browse_grid, 4, 0);
	lv_obj_set_scrollable(browse_grid, false);
	lv_obj_set_flex_flow(browse_grid, LV_FLEX_FLOW_ROW_WRAP);
	lv_obj_set_flex_align(browse_grid, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
	make_browse_button(browse_grid, "artists", &icon_artist, THEME_SEMANTIC_CYAN, false, browse_artists_cb, 0);
	make_browse_button(browse_grid, "albums", &icon_album, THEME_SEMANTIC_PURPLE, false, browse_albums_cb, 1);
	make_browse_button(browse_grid, "music_album_artist", &icon_artist_album, THEME_SEMANTIC_BLUE, true,
					   browse_album_artists_cb, 2);
	make_browse_button(browse_grid, "search", &icon_search, THEME_SEMANTIC_AMBER, false, browse_search_cb, 3);

	music_shortcut_card(tokyo_music_panel);

	tokyo_scan_button = lv_btn_create(tokyo_music_panel);
	lv_obj_set_size(tokyo_scan_button, lv_pct(100), 38);
	lv_obj_add_style(tokyo_scan_button, &theme_style_card, 0);
	lv_obj_add_style(tokyo_scan_button, &theme_style_card_pressed, LV_STATE_PRESSED);
	lv_obj_set_style_radius(tokyo_scan_button, bp_tile_radius(), 0);
	lv_obj_set_style_border_width(tokyo_scan_button, 0, 0);
	lv_obj_set_style_shadow_width(tokyo_scan_button, 0, 0);
	lv_obj_set_style_pad_hor(tokyo_scan_button, 8, 0);
	lv_obj_set_style_pad_column(tokyo_scan_button, 7, 0);
	lv_obj_set_flex_flow(tokyo_scan_button, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(tokyo_scan_button, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_event_bubble(tokyo_scan_button, true);
	lv_obj_add_event_cb(tokyo_scan_button, scan_cb, LV_EVENT_CLICKED, NULL);
	lv_obj_t *scan_icon = lv_image_create(tokyo_scan_button);
	lv_image_set_src(scan_icon, &icon_folder);
	lv_obj_add_style(scan_icon, &theme_style_icon, 0);
	lv_obj_set_size(scan_icon, 18, 18);
	lv_image_set_scale(scan_icon, (uint32_t)(LV_SCALE_NONE * 18 / 32));
	lv_image_set_inner_align(scan_icon, LV_IMAGE_ALIGN_CENTER);
	lv_obj_set_style_image_recolor(scan_icon, theme_semantic_color(THEME_SEMANTIC_CYAN), 0);
	lv_obj_set_style_image_recolor_opa(scan_icon, LV_OPA_COVER, 0);
	lv_obj_t *scan_label = lv_label_create(tokyo_scan_button);
	lv_label_set_text(scan_label, tr("music_scan_library"));
	lv_label_set_long_mode(scan_label, LV_LABEL_LONG_DOT);
	lv_obj_set_flex_grow(scan_label, 1);
	lv_obj_add_style(scan_label, &theme_style_text, 0);
	lv_obj_set_style_text_font(scan_label, &font_ui_14, 0);

	player_sheet_attach_drag(tokyo_music_panel, true);
	switcher_attach_back_gesture(tokyo_music_panel);
}

static void music_recent_refresh(void) {
	if (!tokyo_recent_card) {
		return;
	}
	int count = playlist_count();
	int current = playlist_current_index();
	unsigned revision = playlist_revision();
	cached_queue_count = count;
	cached_queue_current = current;
	cached_queue_revision = revision;

	int filled = 0;
	if (current >= 0 && current < count) {
		for (int index = current; index >= 0 && filled < TOKYO_RECENT_COUNT; index--) {
			char path[512];
			if (!playlist_path_at(index, path, sizeof(path)) || !path[0]) {
				continue;
			}
			// Audiobooks share the playback queue, but they belong in the
			// Audiobooks section rather than this music-oriented panel.
			char book_path[512];
			if (audiobookdb_book_for_file(path, book_path, sizeof(book_path))) {
				continue;
			}

			char title[256] = "";
			char artist[256] = "";
			if (!library_track_names(path, title, sizeof(title), artist, sizeof(artist))) {
				device_state_t state;
				device_state_get(&state);
				if (index != current || state.live || strcmp(state.current_file, path) != 0 || !state.metadata.title[0]) {
					continue;
				}
				snprintf(title, sizeof(title), "%s", state.metadata.title);
				snprintf(artist, sizeof(artist), "%s", state.metadata.artist);
			}
			if (!title[0]) {
				const char *slash = strrchr(path, '/');
				snprintf(title, sizeof(title), "%s", slash ? slash + 1 : path);
			}

			recent_row_t *row = &recent_rows[filled];
			row->queue_index = index;
			row->queue_revision = revision;
			lv_label_set_text(row->title, title);
			lv_label_set_text(row->artist, artist);
			lv_obj_set_hidden(row->button, false);
			filled++;
		}
	}
	for (int i = filled; i < TOKYO_RECENT_COUNT; i++) {
		recent_rows[i].queue_index = -1;
		lv_obj_set_hidden(recent_rows[i].button, true);
	}
	lv_obj_set_hidden(tokyo_recent_empty, filled != 0);
}

static void music_update_visibility(void) {
	bool tokyo_v1 = bp_is_tempotec_v1() && theme_is_tokyo_night();
	bool empty_library = !library_scan_running() && library_track_count() == 0;
	if (tokyo_v1 && tokyo_music_panel) {
		lv_obj_set_hidden(tile_grid, true);
		lv_obj_set_hidden(empty_panel, true);
		lv_obj_set_hidden(tokyo_music_panel, false);
		if (tokyo_scan_button) {
			lv_obj_set_hidden(tokyo_scan_button, !empty_library);
		}
	} else {
		if (tokyo_music_panel) {
			lv_obj_set_hidden(tokyo_music_panel, true);
		}
		gridpage_show_empty(tile_grid, empty_panel, empty_library);
	}
}

static void music_refresh_theme(void) {
	if (!tile_grid) {
		return;
	}
	music_update_visibility();
	for (int i = 0; i < TOKYO_BROWSE_COUNT; i++) {
		if (browse_icons[i].icon) {
			lv_obj_set_style_image_recolor(browse_icons[i].icon, theme_semantic_color(browse_icons[i].tone), 0);
			lv_obj_set_style_image_recolor_opa(browse_icons[i].icon, LV_OPA_COVER, 0);
		}
	}
	for (int i = 0; i < TOKYO_RECENT_COUNT; i++) {
		if (recent_rows[i].icon) {
			lv_obj_set_style_image_recolor(recent_rows[i].icon, theme_semantic_color(THEME_SEMANTIC_PURPLE), 0);
			lv_obj_set_style_image_recolor_opa(recent_rows[i].icon, LV_OPA_COVER, 0);
		}
	}
	music_recent_refresh();
}

static void screen_loaded_cb(lv_event_t *e) {
	(void)e;
	music_update_visibility();
	music_recent_refresh();
}

static void recent_timer_cb(lv_timer_t *timer) {
	(void)timer;
	if (lv_screen_active() != music_screen) {
		return;
	}
	int count = playlist_count();
	int current = playlist_current_index();
	unsigned revision = playlist_revision();
	if (count != cached_queue_count || current != cached_queue_current || revision != cached_queue_revision) {
		music_recent_refresh();
	}
	music_update_visibility();
}

// The corner buttons share everything but icon and action.
static lv_obj_t *corner_button(gui_config_t *cfg, int slot, const lv_image_dsc_t *glyph) {
	lv_obj_t *button = lv_btn_create(music_screen);
	settingsrow_place_corner_button(button, cfg, slot);
	lv_obj_set_style_bg_opa(button, LV_OPA_TRANSP, 0);
	lv_obj_set_style_border_width(button, 0, 0);
	lv_obj_set_style_shadow_width(button, 0, 0);
	lv_obj_set_style_pad_all(button, 0, 0);

	lv_obj_t *icon = lv_image_create(button);
	lv_image_set_src(icon, glyph);
	lv_obj_add_style(icon, &theme_style_icon, 0);
	settingsrow_scale_corner_icon(icon, cfg);
	lv_obj_center(icon);

	return button;
}

void music_init(gui_config_t *cfg) {
	music_cfg = cfg;
	const grid_entry_t entries[] = {
		{"music_all_tracks", &icon_menu_all, NULL, open_all_tracks},
		{"albums", &icon_menu_album, NULL, open_albums},
		{"artists", &icon_menu_artist, NULL, open_artists},
		{"music_album_artists", &icon_menu_album_artist, NULL, open_album_artists},
		{"music_genres", &icon_menu_genre, NULL, open_genres},
		// The V1's tile swaps Browse and Playlists with the music preference.
		{"music_browse", &icon_menu_explorer, NULL, open_tile_slot},
	};

	tile_grid = gridpage_build(music_screen, cfg, entries, (int)(sizeof(entries) / sizeof(entries[0])), 2, 3, true);
	coverflow_attach_edge(tile_grid, cfg);

	bool compact_header = cfg->screen_width < 320;
	settingsrow_title_corner_slots(settingsrow_title(music_screen, cfg, "music"), cfg, compact_header ? 2 : 4);

	lv_obj_t *settings_btn = corner_button(cfg, 0, &icon_music_settings);
	lv_obj_add_event_cb(settings_btn, switch_screen_cb, LV_EVENT_CLICKED, musicsettings_screen);

	if (compact_header) {
		playlists_icon = NULL;
		lv_obj_t *more_btn = corner_button(cfg, 1, &icon_list_music);
		lv_obj_add_event_cb(more_btn, compact_more_cb, LV_EVENT_CLICKED, NULL);
	} else {
		lv_obj_t *playlists_btn = corner_button(cfg, 1, &icon_list_music);
		playlists_icon = lv_obj_get_child(playlists_btn, 0);
		lv_obj_add_event_cb(playlists_btn, playlists_cb, LV_EVENT_CLICKED, NULL);

		lv_obj_t *favourites_btn = corner_button(cfg, 2, &icon_star_corner);
		lv_obj_add_event_cb(favourites_btn, favourites_cb, LV_EVENT_CLICKED, NULL);

		lv_obj_t *search_btn = corner_button(cfg, 3, &icon_search);
		lv_obj_add_event_cb(search_btn, search_cb, LV_EVENT_CLICKED, NULL);
	}

	empty_panel = gridpage_empty_panel(music_screen, cfg, &icon_music_note, "music_no_database", scan_cb);
	lv_obj_add_event_cb(music_screen, screen_loaded_cb, LV_EVENT_SCREEN_LOADED, NULL);
	tokyo_music_build(cfg);

	music_refresh_layout();
	music_refresh_theme();
	theme_register_refresh(music_refresh_theme);
	if (tokyo_music_panel) {
		lv_timer_create(recent_timer_cb, 1000, NULL);
	}
}

void music_refresh_layout(void) {
	bool playlists_first = musicsettings_playlists_first();
	gridpage_set_tile(tile_grid, 5, playlists_first ? &icon_menu_playlist : &icon_menu_explorer,
					  playlists_first ? "playlists" : "music_browse");

	if (playlists_icon) {
		lv_image_set_src(playlists_icon, playlists_first ? &icon_folder_corner : &icon_list_music);
	}
	music_refresh_theme();
}
