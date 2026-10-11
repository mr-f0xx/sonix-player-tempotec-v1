#include "main_menu.h"

#include <stdint.h>
#include <string.h>

#include "lvgl/lvgl.h"

#include "src/gui/board_profile.h"
#include "src/gui/fonts/fonts.h"
#include "src/gui/library/audiobooks.h"
#include "src/gui/library/music.h"
#include "src/gui/nowplaying/player.h"
#include "src/gui/shell/gridpage.h"
#include "src/gui/shell/gui.h"
#include "src/gui/shell/icons.h"
#include "src/gui/shell/morepage.h"
#include "src/gui/settings/settings.h"
#include "src/gui/shell/switcher.h"
#include "src/gui/shell/theme.h"
#include "src/gui/streaming/streaming.h"
#include "src/gui/wireless/wireless.h"
#include "src/system/core/lang.h"
#include "src/system/playback/device_state.h"

lv_obj_t *main_menu_screen;

static lv_obj_t *menu_grid;
static lv_obj_t *home_header;
static lv_obj_t *home_title;
static lv_obj_t *home_note;
static lv_obj_t *mini_player;
static lv_obj_t *mini_player_halo;
static lv_obj_t *mini_player_icon;
static lv_obj_t *mini_player_copy;
static lv_obj_t *mini_player_title;
static lv_obj_t *mini_player_subtitle;
static lv_timer_t *mini_player_timer;
static gui_config_t *menu_cfg;

static const lv_image_dsc_t *const NORMAL_MENU_ICONS[6] = {
	&icon_menu_music,
	&icon_menu_streaming,
	&icon_menu_wireless,
	&icon_menu_audiobooks,
	&icon_menu_more,
	&icon_menu_settings,
};

static const lv_image_dsc_t *const TOKYO_MENU_ICONS[6] = {
	&icon_music2,           // Music
	&icon_radio_player,     // Streaming
	&icon_wifi_high,        // Wireless
	&icon_files_audiobook,  // Audiobooks
	&icon_ellipsis_vertical, // More
	&icon_music_settings,   // Settings
};

static const theme_semantic_t TOKYO_MENU_TONES[6] = {
	THEME_SEMANTIC_CYAN,
	THEME_SEMANTIC_PURPLE,
	THEME_SEMANTIC_BLUE,
	THEME_SEMANTIC_AMBER,
	THEME_SEMANTIC_GREEN,
	THEME_SEMANTIC_MUTED,
};

// Audiobooks reads its library straight off the card, so it is the one tile
// that opens onto nothing when the card is out or at a computer. Music has the
// same rule a level deeper, where a list is asked for.
static void open_audiobooks(void) {
	if (!gui_card_available()) {
		gui_notify_no_card();
		return;
	}
	switch_screen(audiobooks_screen);
}

static void fit_menu_icon(lv_obj_t *image, const lv_image_dsc_t *source, int size) {
	if (!image || !source) {
		return;
	}
	int max_side = LV_MAX((int)source->header.w, (int)source->header.h);
	if (max_side <= 0) {
		return;
	}
	if (size <= 0) {
		size = max_side;
	}
	lv_obj_set_size(image, LV_MAX(1, (int)source->header.w * size / max_side),
					LV_MAX(1, (int)source->header.h * size / max_side));
	lv_image_set_inner_align(image, LV_IMAGE_ALIGN_CENTER);
	lv_image_set_scale(image, (uint32_t)(LV_SCALE_NONE * size / max_side));
}

static void set_label_if_changed(lv_obj_t *label, const char *text) {
	if (!label || !text) {
		return;
	}
	const char *current = lv_label_get_text(label);
	if (!current || strcmp(current, text) != 0) {
		lv_label_set_text(label, text);
	}
}

// Fit the V1's live strip around two fixed one-line labels. Explicitly sizing
// the text column from the row's content width keeps both glyphs and ellipses
// inside the card, including after a font-size or theme change.
static void main_menu_layout_tokyo_home(bool force) {
	if (!menu_grid || !menu_cfg || !mini_player || !mini_player_copy ||
			!bp_is_tempotec_v1() || !theme_is_tokyo_night()) {
		return;
	}

	int line_height = lv_font_get_line_height(&font_ui_14);
	int footer_height = LV_MAX(42, line_height * 2 + 8);
	int footer_y = (int)menu_cfg->screen_height - (int)menu_cfg->padding - footer_height;
	bool needs_reflow = force || (int)lv_obj_get_height(mini_player) != footer_height ||
			(int)lv_obj_get_y(mini_player) != footer_y;
	if (!needs_reflow) {
		return;
	}

	lv_obj_set_size(mini_player, (int)menu_cfg->screen_width - 2 * (int)menu_cfg->padding, footer_height);
	lv_obj_set_pos(mini_player, menu_cfg->padding, footer_y);
	lv_obj_update_layout(mini_player);
	lv_obj_set_height(mini_player_copy, line_height * 2);
	lv_obj_set_flex_grow(mini_player_copy, 0);
	int content_width = lv_obj_get_content_width(mini_player);
	int copy_width = content_width - lv_obj_get_width(mini_player_icon) - 14 - 2 * 7;
	lv_obj_set_width(mini_player_copy, LV_MAX(1, copy_width));
	lv_obj_set_width(mini_player_title, lv_pct(100));
	lv_obj_set_width(mini_player_subtitle, lv_pct(100));
	lv_obj_set_height(mini_player_title, line_height);
	lv_obj_set_height(mini_player_subtitle, line_height);

	int bottom = (int)menu_cfg->padding + footer_height + 1;
	gridpage_set_layout(menu_grid, menu_cfg, (int)menu_cfg->top_bar_height + 62, bottom, menu_cfg->padding, 6, 2, 3);
	for (int i = 0; i < 6; i++) {
		gridpage_set_tile_orientation(menu_grid, i, true);
	}
}

// The mini-player shows the live title and artist only. LVGL owns its label
// text after lv_label_set_text(), so the metadata may safely come from this
// local state snapshot; one-line dot mode clips long text without overlap.
static void main_menu_refresh_now_playing(void) {
	if (!mini_player || !menu_cfg) {
		return;
	}
	main_menu_layout_tokyo_home(false);

	device_state_t state;
	device_state_get(&state);
	bool loaded = state.live || state.current_file[0] != '\0';
	const char *title = NULL;
	const char *artist = "";

	if (!loaded) {
		title = tr("player_no_track_loaded");
		lv_image_set_src(mini_player_icon, &icon_music2);
		lv_obj_set_style_image_recolor(mini_player_icon, theme()->text_secondary, 0);
	} else {
		if (state.metadata.title[0]) {
			title = state.metadata.title;
		} else if (state.live) {
			title = tr("player_live");
		} else {
			const char *file = strrchr(state.current_file, '/');
			title = file ? file + 1 : state.current_file;
		}
		if (state.metadata.artist[0]) {
			artist = state.metadata.artist;
		}

		lv_image_set_src(mini_player_icon,
						 state.status == AUDIO_STATUS_PLAYING ? &icon_pause : &icon_play);
		lv_obj_set_style_image_recolor(mini_player_icon, theme_semantic_color(THEME_SEMANTIC_CYAN), 0);
	}

	const lv_image_dsc_t *mini_source = lv_image_get_src(mini_player_icon);
	fit_menu_icon(mini_player_icon, mini_source, 15);
	lv_obj_set_style_image_recolor_opa(mini_player_icon, LV_OPA_COVER, 0);
	if (mini_player_halo) {
		lv_image_set_src(mini_player_halo, mini_source);
		fit_menu_icon(mini_player_halo, mini_source, 21);
		lv_obj_set_style_image_recolor(mini_player_halo, theme_semantic_color(THEME_SEMANTIC_CYAN), 0);
		lv_obj_set_style_image_recolor_opa(mini_player_halo, LV_OPA_COVER, 0);
		lv_obj_set_style_image_opa(mini_player_halo, LV_OPA_20, 0);
		lv_obj_set_hidden(mini_player_halo, !loaded);
		lv_obj_update_layout(mini_player);
		lv_obj_align_to(mini_player_halo, mini_player_icon, LV_ALIGN_CENTER, 0, 0);
	}
	set_label_if_changed(mini_player_title, title ? title : "");
	set_label_if_changed(mini_player_subtitle, artist);
}

static void mini_player_timer_cb(lv_timer_t *timer) {
	(void)timer;
	// This is a 1-second read only while the home screen is actually active.
	// Elsewhere the strip has no pixels to update and the timer returns at once.
	if (lv_screen_active() == main_menu_screen) {
		main_menu_refresh_now_playing();
	}
}

static void main_menu_screen_loaded_cb(lv_event_t *e) {
	(void)e;
	main_menu_refresh_now_playing();
}

static void main_menu_refresh_theme(void) {
	if (!menu_grid || !menu_cfg) {
		return;
	}

	bool compact = bp_is_tempotec_v1();
	bool tokyo = theme_is_tokyo_night();
	bool prototype_layout = compact && tokyo;

	for (int i = 0; i < 6; i++) {
		if (tokyo) {
			gridpage_set_tile_icon_style(menu_grid, i, TOKYO_MENU_ICONS[i], compact ? 22 : 46, true,
										 theme_semantic_color(TOKYO_MENU_TONES[i]), prototype_layout && i < 5 && i != 1 && i != 3);
		} else {
			// The original Dark and Light menu art and its full V1 icon size are
			// retained exactly; the glyph-only treatment is scoped to Tokyo Night.
			gridpage_set_tile_icon_style(menu_grid, i, NORMAL_MENU_ICONS[i], compact ? 46 : 0, false,
										 theme()->text_primary, false);
		}

		lv_obj_t *tile = lv_obj_get_child(menu_grid, i);
		lv_obj_set_style_border_width(tile, tokyo ? 1 : 0, 0);
		if (tokyo) {
			lv_obj_set_style_border_color(tile, theme()->text_secondary, 0);
			lv_obj_set_style_border_opa(tile, LV_OPA_20, 0);
		} else {
			lv_obj_set_style_border_opa(tile, LV_OPA_COVER, 0);
		}
	}

	if (prototype_layout) {
		lv_obj_set_hidden(home_header, false);
		lv_obj_set_hidden(home_note, false);
		lv_obj_set_hidden(mini_player, false);

		// Status bar: y=0..24. The 36 px title row and single-line note lead
		// into six horizontal icon-and-label cards; the mini-player sits below.
		// The six existing tile objects are simply reflowed, not duplicated.
		lv_obj_set_style_border_width(home_header, 1, 0);
		lv_obj_set_style_border_side(home_header, LV_BORDER_SIDE_BOTTOM, 0);
		lv_obj_set_style_border_color(home_header, theme()->surface_pressed, 0);
		lv_obj_set_style_border_opa(home_header, LV_OPA_20, 0);

		lv_color_t strip_fill = lv_color_mix(theme()->accent, theme()->surface, LV_OPA_10);
		lv_color_t strip_edge = lv_color_mix(theme()->accent, theme()->surface, LV_OPA_30);
		lv_obj_set_style_bg_color(mini_player, strip_fill, 0);
		lv_obj_set_style_bg_opa(mini_player, LV_OPA_COVER, 0);
		lv_obj_set_style_border_color(mini_player, strip_edge, 0);
		lv_obj_set_style_border_opa(mini_player, LV_OPA_COVER, 0);
		lv_obj_set_style_border_width(mini_player, 1, 0);
		main_menu_layout_tokyo_home(true);
	} else {
		lv_obj_set_hidden(home_header, true);
		lv_obj_set_hidden(home_note, true);
		lv_obj_set_hidden(mini_player, true);
		gridpage_set_layout(menu_grid, menu_cfg, menu_cfg->top_bar_height, 0, menu_cfg->padding, compact ? 6 : 14, 2, 3);
		for (int i = 0; i < 6; i++) {
			gridpage_set_tile_orientation(menu_grid, i, false);
		}
		lv_obj_set_style_border_width(home_header, 0, 0);
		lv_obj_set_style_border_width(mini_player, 0, 0);
		lv_obj_remove_local_style_prop(mini_player, LV_STYLE_BG_COLOR, 0);
	}

	main_menu_refresh_now_playing();
}

void main_menu_init(gui_config_t *cfg) {
	menu_cfg = cfg;

	const grid_entry_t entries[] = {
		{"music", &icon_menu_music, &music_screen, NULL},
		{"streaming", &icon_menu_streaming, &streaming_screen, NULL},
		{"wireless", &icon_menu_wireless, &wireless_screen, NULL},
		{"audiobooks", &icon_menu_audiobooks, NULL, open_audiobooks},
		// The door to the More page, which holds the DAC and anything else
		// that does not fit. The main menu has exactly six tiles, and one of
		// them has to be able to keep growing.
		{"more", &icon_menu_more, &morepage_screen, NULL},
		{"settings", &icon_menu_settings, &settings_screen, NULL},
	};

	menu_grid = gridpage_build(main_menu_screen, cfg, entries, (int)(sizeof(entries) / sizeof(entries[0])), 2, 3, false);

	// Tokyo Night's compact home chrome: the header, note and live mini-player
	// are ordinary LVGL objects, and remain hidden in the unchanged Dark/Light
	// home menu.
	home_header = lv_obj_create(main_menu_screen);
	lv_obj_remove_style_all(home_header);
	lv_obj_set_size(home_header, cfg->screen_width, 36);
	lv_obj_set_pos(home_header, 0, cfg->top_bar_height);
	lv_obj_set_style_bg_opa(home_header, LV_OPA_TRANSP, 0);
	lv_obj_set_style_pad_all(home_header, 0, 0);
	lv_obj_set_style_pad_left(home_header, 6, 0);
	lv_obj_set_style_pad_right(home_header, 6, 0);
	lv_obj_set_style_border_width(home_header, 0, 0);
	lv_obj_set_scrollable(home_header, false);
	lv_obj_set_flex_flow(home_header, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(home_header, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

	home_title = lv_label_create(home_header);
	lv_label_set_text(home_title, "Sonix Player");
	lv_obj_add_style(home_title, &theme_style_text, 0);
	lv_obj_set_style_text_font(home_title, &font_ui_16, 0);
	lv_obj_set_flex_grow(home_title, 1);

	home_note = lv_label_create(main_menu_screen);
	lv_label_set_text(home_note, tr("main_menu_tagline"));
	lv_label_set_long_mode(home_note, LV_LABEL_LONG_DOT);
	lv_obj_set_width(home_note, cfg->screen_width - 2 * cfg->padding);
	lv_obj_set_height(home_note, lv_font_get_line_height(&font_ui_14));
	lv_obj_set_pos(home_note, cfg->padding, cfg->top_bar_height + 40);
	lv_obj_add_style(home_note, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(home_note, &font_ui_14, 0);

	mini_player = lv_btn_create(main_menu_screen);
	lv_obj_set_size(mini_player, cfg->screen_width - 2 * cfg->padding, 38);
	lv_obj_set_pos(mini_player, cfg->padding, cfg->screen_height - cfg->padding - 38);
	lv_obj_add_style(mini_player, &theme_style_card, 0);
	lv_obj_add_style(mini_player, &theme_style_card_pressed, LV_STATE_PRESSED);
	lv_obj_set_style_radius(mini_player, 8, 0);
	lv_obj_set_style_border_width(mini_player, 0, 0);
	lv_obj_set_style_pad_left(mini_player, 6, 0);
	lv_obj_set_style_pad_right(mini_player, 6, 0);
	lv_obj_set_style_pad_top(mini_player, 2, 0);
	lv_obj_set_style_pad_bottom(mini_player, 2, 0);
	lv_obj_set_style_pad_gap(mini_player, 7, 0);
	lv_obj_set_scrollable(mini_player, false);
	lv_obj_set_flex_flow(mini_player, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(mini_player, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_add_event_cb(mini_player, switch_screen_cb, LV_EVENT_CLICKED, player_screen);
	lv_obj_set_event_bubble(mini_player, true);

	mini_player_halo = lv_image_create(mini_player);
	lv_image_set_src(mini_player_halo, &icon_music2);
	fit_menu_icon(mini_player_halo, &icon_music2, 21);
	lv_obj_set_floating(mini_player_halo, true);
	lv_obj_set_clickable(mini_player_halo, false);
	lv_obj_set_scrollable(mini_player_halo, false);
	lv_obj_set_style_bg_opa(mini_player_halo, LV_OPA_TRANSP, 0);
	lv_obj_set_style_image_opa(mini_player_halo, LV_OPA_20, 0);
	lv_obj_set_hidden(mini_player_halo, true);
	lv_obj_move_to_index(mini_player_halo, 0);

	mini_player_icon = lv_image_create(mini_player);
	lv_image_set_src(mini_player_icon, &icon_music2);
	fit_menu_icon(mini_player_icon, &icon_music2, 15);
	lv_obj_add_style(mini_player_icon, &theme_style_icon, 0);

	mini_player_copy = lv_obj_create(mini_player);
	lv_obj_t *copy = mini_player_copy;
	lv_obj_remove_style_all(copy);
	lv_obj_set_width(copy, LV_SIZE_CONTENT);
	lv_obj_set_height(copy, LV_SIZE_CONTENT);
	lv_obj_set_style_bg_opa(copy, LV_OPA_TRANSP, 0);
	lv_obj_set_style_pad_all(copy, 0, 0);
	lv_obj_set_style_pad_gap(copy, 0, 0);
	lv_obj_set_scrollable(copy, false);
	lv_obj_set_flex_flow(copy, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(copy, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_flex_grow(copy, 0);

	mini_player_title = lv_label_create(copy);
	lv_label_set_long_mode(mini_player_title, LV_LABEL_LONG_DOT);
	lv_obj_set_width(mini_player_title, lv_pct(100));
	lv_obj_add_style(mini_player_title, &theme_style_text, 0);
	lv_obj_set_style_text_font(mini_player_title, &font_ui_14, 0);

	mini_player_subtitle = lv_label_create(copy);
	lv_label_set_long_mode(mini_player_subtitle, LV_LABEL_LONG_DOT);
	lv_obj_set_width(mini_player_subtitle, lv_pct(100));
	lv_obj_add_style(mini_player_subtitle, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(mini_player_subtitle, &font_ui_14, 0);

	lv_obj_t *mini_chevron = lv_image_create(mini_player);
	lv_image_set_src(mini_chevron, &icon_chevron_right);
	fit_menu_icon(mini_chevron, &icon_chevron_right, 14);
	lv_obj_add_style(mini_chevron, &theme_style_icon, 0);

	lv_obj_add_event_cb(main_menu_screen, main_menu_screen_loaded_cb, LV_EVENT_SCREEN_LOADED, NULL);
	mini_player_timer = lv_timer_create(mini_player_timer_cb, 1000, NULL);
	if (mini_player_timer) {
		lv_timer_ready(mini_player_timer);
	}

	main_menu_refresh_theme();
	theme_register_refresh(main_menu_refresh_theme);
}
