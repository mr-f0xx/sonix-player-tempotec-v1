#include "gearboypage.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lvgl/lvgl.h"

#include "src/gui/board_profile.h"
#include "src/gui/fonts/fonts.h"
#include "src/gui/gearboy/gearboyplay.h"
#include "src/gui/gearboy/gearboysettings.h"
#include "src/gui/nowplaying/cover.h"
#include "src/gui/nowplaying/coverloader.h"
#include "src/gui/nowplaying/player.h"
#include "src/gui/shell/icons.h"
#include "src/gui/shell/settingsrow.h"
#include "src/gui/shell/switcher.h"
#include "src/gui/shell/theme.h"
#include "src/system/gearboy/gbdb.h"
#include "src/system/core/lang.h"

lv_obj_t *gearboypage_screen;

#define ART_SLOTS COVERLOADER_GEARBOY_COUNT
#define ART_SIDE_MAX 216

static gui_config_t *page_cfg;
static lv_obj_t *list_box;
static lv_obj_t *status_label;
static lv_obj_t *btn_gb;
static lv_obj_t *btn_gbc;
static lv_timer_t *art_timer;

// The GB/GBC selector remains; each selection shows exactly two game cards at once.
static bool show_color;
static gbrom_t *roms;
static int rom_count;
static lv_obj_t **tiles;
static lv_obj_t **art_images;
static lv_obj_t **art_placeholders;
static cover_image_t *covers;
static bool *cover_finished;
static int slot_rom[ART_SLOTS];
static int art_side_px;

static uint32_t scan_generation;
static bool scanning;

typedef struct {
	uint32_t generation;
	gbrom_t *list;
	int count;
} scan_result_t;

static void open_rom_cb(lv_event_t *e) {
	if (switcher_back_drag_active() || player_sheet_drag_active()) {
		return;
	}
	int index = (int)(intptr_t)lv_event_get_user_data(e);
	if (index >= 0 && index < rom_count && roms) {
		gearboyplay_open(roms[index].path, roms[index].name);
	}
}

static void release_art(void) {
	for (int slot = 0; slot < ART_SLOTS; slot++) {
		coverloader_release(COVERLOADER_GEARBOY_BASE + slot);
		slot_rom[slot] = -1;
	}
	for (int i = 0; i < rom_count; i++) {
		if (covers && covers[i].pixels) {
			cover_free(&covers[i]);
			memset(&covers[i], 0, sizeof(covers[i]));
		}
		if (cover_finished) {
			cover_finished[i] = false;
		}
	}
}

static void free_grid_data(void) {
	release_art();
	free(tiles);
	free(art_images);
	free(art_placeholders);
	free(covers);
	free(cover_finished);
	tiles = NULL;
	art_images = NULL;
	art_placeholders = NULL;
	covers = NULL;
	cover_finished = NULL;
}

static void refresh_view_buttons(void) {
	lv_obj_t *const buttons[] = {btn_gb, btn_gbc};
	const bool colour[] = {false, true};
	for (size_t i = 0; i < sizeof(buttons) / sizeof(buttons[0]); i++) {
		if (!buttons[i]) {
			continue;
		}
		bool on = colour[i] == show_color;
		lv_obj_set_style_bg_color(buttons[i], on ? theme()->accent : theme()->surface_pressed, 0);
		lv_obj_set_style_text_color(lv_obj_get_child(buttons[i], 0), on ? theme_on_accent() : theme()->text_primary, 0);
	}
}

static void title_on_two_lines(const char *title, char out[GBROM_NAME_MAX + 2]) {
	size_t len = strlen(title);
	size_t target = len / 2;
	size_t split = len;
	size_t best_distance = len;
	for (size_t i = 1; i + 1 < len; i++) {
		if (title[i] != ' ') {
			continue;
		}
		size_t distance = i > target ? i - target : target - i;
		if (distance < best_distance) {
			best_distance = distance;
			split = i;
		}
	}
	if (split == len) {
		snprintf(out, GBROM_NAME_MAX + 2, "%s", title);
		return;
	}
	memcpy(out, title, split);
	out[split] = '\n';
	snprintf(out + split + 1, GBROM_NAME_MAX + 1 - split, "%s", title + split + 1);
}

static void make_tile(int index, int tile_w, int tile_h) {
	const gbrom_t *rom = &roms[index];
	char title[GBROM_NAME_MAX + 2];
	title_on_two_lines(rom->name, title);
	int art_side = art_side_px;
	lv_obj_t *tile = lv_btn_create(list_box);
	lv_obj_set_size(tile, tile_w, tile_h);
	lv_obj_add_style(tile, &theme_style_card, 0);
	lv_obj_add_style(tile, &theme_style_card_pressed, LV_STATE_PRESSED);
	lv_obj_set_style_radius(tile, 14, 0);
	lv_obj_set_style_border_width(tile, 0, 0);
	lv_obj_set_style_shadow_width(tile, 0, 0);
	lv_obj_set_style_pad_hor(tile, 12, 0);
	lv_obj_set_style_pad_top(tile, 28, 0);
	lv_obj_set_style_pad_bottom(tile, 12, 0);
	lv_obj_set_style_pad_row(tile, 14, 0);
	lv_obj_set_scrollable(tile, false);
	lv_obj_set_flex_flow(tile, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(tile, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_event_bubble(tile, true);
	lv_obj_add_event_cb(tile, open_rom_cb, LV_EVENT_CLICKED, (void *)(intptr_t)index);
	tiles[index] = tile;

	lv_obj_t *art = lv_obj_create(tile);
	lv_obj_set_size(art, art_side, art_side);
	lv_obj_set_style_bg_color(art, theme()->surface_pressed, 0);
	lv_obj_set_style_bg_opa(art, LV_OPA_COVER, 0);
	lv_obj_set_style_border_width(art, 0, 0);
	lv_obj_set_style_radius(art, 10, 0);
	lv_obj_set_style_pad_all(art, 0, 0);
	lv_obj_set_scrollable(art, false);
	lv_obj_set_event_bubble(art, true);

	lv_obj_t *image = lv_image_create(art);
	lv_obj_set_size(image, art_side, art_side);
	lv_obj_center(image);
	lv_obj_set_event_bubble(image, true);
	art_images[index] = image;

	lv_obj_t *placeholder = lv_label_create(art);
	lv_label_set_text(placeholder, rom->color ? "GBC" : "GB");
	lv_obj_set_style_text_font(placeholder, &font_ui_24, 0);
	lv_obj_add_style(placeholder, &theme_style_text_dim, 0);
	lv_obj_center(placeholder);
	lv_obj_set_event_bubble(placeholder, true);
	art_placeholders[index] = placeholder;

	lv_obj_t *name = lv_label_create(tile);
	lv_label_set_text(name, title);
	lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
	lv_obj_set_width(name, lv_pct(100));
	lv_obj_set_height(name, 2 * lv_font_get_line_height(&font_ui_22));
	lv_obj_set_style_text_align(name, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_set_style_text_font(name, &font_ui_22, 0);
	lv_obj_add_style(name, &theme_style_text, 0);
	lv_obj_set_event_bubble(name, true);
}

static void rebuild_grid(void) {
	release_art();
	lv_obj_clean(list_box);
	if (!rom_count || !roms) {
		lv_label_set_text(status_label, show_color ? tr("gearboy_empty_note") : tr("gearboy_empty_gb_note"));
		lv_obj_set_hidden(status_label, false);
		return;
	}

	int shown = 0;
	for (int i = 0; i < rom_count; i++) {
		if (roms[i].color == show_color) {
			shown++;
		}
	}
	if (!shown) {
		lv_label_set_text(status_label, show_color ? tr("gearboy_empty_note") : tr("gearboy_empty_gb_note"));
		lv_obj_set_hidden(status_label, false);
		return;
	}

	tiles = calloc((size_t)rom_count, sizeof(*tiles));
	art_images = calloc((size_t)rom_count, sizeof(*art_images));
	art_placeholders = calloc((size_t)rom_count, sizeof(*art_placeholders));
	covers = calloc((size_t)rom_count, sizeof(*covers));
	cover_finished = calloc((size_t)rom_count, sizeof(*cover_finished));
	if (!tiles || !art_images || !art_placeholders || !covers || !cover_finished) {
		free_grid_data();
		lv_label_set_text(status_label, tr("gearboy_search_failed"));
		lv_obj_set_hidden(status_label, false);
		return;
	}

	lv_obj_set_hidden(status_label, true);
	lv_obj_scroll_to_y(list_box, 0, LV_ANIM_OFF);
	// Keep the screenshot's two-column layout, but make each pair of tiles
	// fill the viewport height. That leaves exactly two games on screen rather
	// than a clipped preview of the next row. The artwork and two-line title are
	// grouped around the card center for a more balanced layout.
	lv_obj_update_layout(list_box);
	int available_w = lv_obj_get_content_width(list_box);
	int tile_w = (available_w - 12) / 2;
	int tile_h = lv_obj_get_content_height(list_box);
	int width_fit = tile_w - 24;
	int height_fit = tile_h - 28 - 12 - 14 - 2 * lv_font_get_line_height(&font_ui_22);
	art_side_px = LV_MIN(ART_SIDE_MAX, LV_MIN(width_fit, height_fit));
	if (art_side_px < 1) {
		art_side_px = 1;
	}
	for (int i = 0; i < rom_count; i++) {
		if (roms[i].color == show_color) {
			make_tile(i, tile_w, tile_h);
		}
	}
	lv_obj_update_layout(list_box);
}

static void pick_view_cb(lv_event_t *e) {
	if (switcher_back_drag_active() || player_sheet_drag_active()) {
		return;
	}
	show_color = (bool)(uintptr_t)lv_event_get_user_data(e);
	refresh_view_buttons();
	rebuild_grid();
}

static lv_obj_t *make_view_choice(lv_obj_t *parent, const char *text, bool color) {
	lv_obj_t *btn = lv_btn_create(parent);
	lv_obj_set_size(btn, LV_SIZE_CONTENT, bp_pick(40, 56));
	lv_obj_set_style_pad_hor(btn, bp_pick(16, 26), 0);
	lv_obj_set_style_radius(btn, LV_RADIUS_CIRCLE, 0);
	lv_obj_set_style_shadow_width(btn, 0, 0);
	lv_obj_set_style_border_width(btn, 0, 0);
	lv_obj_add_event_cb(btn, pick_view_cb, LV_EVENT_CLICKED, (void *)(uintptr_t)color);
	lv_obj_t *label = lv_label_create(btn);
	lv_label_set_text(label, tr(text));
	lv_obj_set_style_text_font(label, &font_ui_24, 0);
	lv_obj_center(label);
	return btn;
}

// Load only visible art (plus a little look-ahead), keeping memory bounded even
// when a card holds a large ROM collection. The shared cover worker owns all
// decoding; the GUI only swaps in finished thumbnails.
static void art_timer_cb(lv_timer_t *timer) {
	(void)timer;
	if (!roms || !tiles || lv_scr_act() != gearboypage_screen) {
		return;
	}
	lv_obj_update_layout(list_box);
	lv_area_t view;
	lv_obj_get_coords(list_box, &view);
	const int margin = art_side_px;
	int wanted[ART_SLOTS];
	int wanted_count = 0;
	for (int i = 0; i < rom_count && wanted_count < ART_SLOTS; i++) {
		if (!tiles[i]) {
			continue;
		}
		lv_area_t area;
		lv_obj_get_coords(tiles[i], &area);
		if (area.y2 < view.y1 - margin || area.y1 > view.y2 + margin) {
			continue;
		}
		wanted[wanted_count++] = i;
	}

	for (int slot = 0; slot < ART_SLOTS; slot++) {
		int next = slot < wanted_count ? wanted[slot] : -1;
		if (slot_rom[slot] == next) {
			continue;
		}
		int old = slot_rom[slot];
		coverloader_release(COVERLOADER_GEARBOY_BASE + slot);
		if (old >= 0 && old < rom_count && covers && covers[old].pixels) {
			cover_free(&covers[old]);
			memset(&covers[old], 0, sizeof(covers[old]));
			cover_finished[old] = false;
			if (art_images[old]) {
				lv_image_set_src(art_images[old], NULL);
			}
			if (art_placeholders[old]) {
				lv_obj_set_hidden(art_placeholders[old], false);
			}
		}
		slot_rom[slot] = next;
		if (next >= 0 && roms[next].cover_path[0] && !cover_finished[next]) {
			coverloader_request_image(COVERLOADER_GEARBOY_BASE + slot, roms[next].cover_path, art_side_px);
		} else if (next >= 0) {
			cover_finished[next] = true;
		}
	}

	for (int slot = 0; slot < wanted_count; slot++) {
		int index = slot_rom[slot];
		if (index < 0 || cover_finished[index] || !roms[index].cover_path[0]) {
			continue;
		}
		bool finished = false;
		cover_image_t image = {0};
		bool found = coverloader_take(COVERLOADER_GEARBOY_BASE + slot, &image, &finished);
		if (!finished) {
			continue;
		}
		cover_finished[index] = true;
		if (found) {
			covers[index] = image;
			lv_image_set_src(art_images[index], &covers[index].dsc);
			lv_obj_set_hidden(art_placeholders[index], true);
		}
	}
}

static void scan_done_cb(void *user) {
	scan_result_t *res = user;
	if (res->generation != scan_generation) {
		free(res->list);
		free(res);
		return;
	}
	free_grid_data();
	free(roms);
	roms = res->list;
	rom_count = res->count;
	scanning = false;
	rebuild_grid();
	free(res);
}

static void *scan_thread(void *arg) {
	uint32_t generation = (uint32_t)(uintptr_t)arg;
	gbrom_t *list = NULL;
	int count = gbdb_scan(&list, NULL, NULL);
	if (count < 0) {
		count = 0;
	}
	scan_result_t *res = malloc(sizeof(*res));
	if (!res) {
		free(list);
		return NULL;
	}
	res->generation = generation;
	res->list = list;
	res->count = count;
	gui_post(scan_done_cb, res);
	return NULL;
}

static void start_scan(void) {
	if (scanning) {
		return;
	}
	scanning = true;
	scan_generation++;
	lv_label_set_text(status_label, tr("gearboy_looking_for_games"));
	lv_obj_set_hidden(status_label, false);
	pthread_t t;
	if (pthread_create(&t, NULL, scan_thread, (void *)(uintptr_t)scan_generation) != 0) {
		scanning = false;
		lv_label_set_text(status_label, tr("gearboy_search_failed"));
		return;
	}
	pthread_detach(t);
}

static void screen_loaded_cb(lv_event_t *e) {
	(void)e;
	refresh_view_buttons();
	lv_obj_scroll_to_y(list_box, 0, LV_ANIM_OFF);
	start_scan();
}

static void open_settings_cb(lv_event_t *e) {
	(void)e;
	switch_screen(gearboysettings_screen);
}

void gearboypage_init(gui_config_t *cfg) {
	page_cfg = cfg;
	for (int i = 0; i < ART_SLOTS; i++) {
		slot_rom[i] = -1;
	}
	coverloader_start();
	lv_obj_add_style(gearboypage_screen, &theme_style_screen, 0);
	lv_obj_t *page_title = settingsrow_title(gearboypage_screen, cfg, "gearboy");
	settingsrow_title_corner_slots(page_title, cfg, 1);

	lv_obj_t *button = lv_btn_create(gearboypage_screen);
	settingsrow_place_corner_button(button, cfg, 0);
	lv_obj_set_style_bg_opa(button, LV_OPA_TRANSP, 0);
	lv_obj_set_style_border_width(button, 0, 0);
	lv_obj_set_style_shadow_width(button, 0, 0);
	lv_obj_set_style_pad_all(button, 0, 0);
	lv_obj_add_event_cb(button, open_settings_cb, LV_EVENT_CLICKED, NULL);
	lv_obj_t *icon = lv_image_create(button);
	lv_image_set_src(icon, &icon_music_settings);
	lv_obj_add_style(icon, &theme_style_icon, 0);
	settingsrow_scale_corner_icon(icon, cfg);
	lv_obj_center(icon);

	int content_top = settingsrow_content_top(cfg);
	lv_obj_t *container = lv_obj_create(gearboypage_screen);
	lv_obj_set_size(container, lv_pct(100), cfg->screen_height - content_top);
	lv_obj_align(container, LV_ALIGN_TOP_LEFT, 0, content_top);
	lv_obj_set_style_bg_opa(container, 0, 0);
	lv_obj_set_style_border_width(container, 0, 0);
	lv_obj_set_style_radius(container, 0, 0);
	lv_obj_set_style_pad_hor(container, cfg->padding, 0);
	lv_obj_set_style_pad_ver(container, 0, 0);
	lv_obj_set_style_pad_gap(container, 12, 0);
	lv_obj_set_scrollable(container, false);
	lv_obj_set_flex_flow(container, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(container, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
	switcher_attach_back_gesture(container);

	lv_obj_t *chooser = lv_obj_create(container);
	lv_obj_set_size(chooser, lv_pct(100), LV_SIZE_CONTENT);
	lv_obj_set_style_bg_opa(chooser, 0, 0);
	lv_obj_set_style_border_width(chooser, 0, 0);
	lv_obj_set_style_pad_all(chooser, 0, 0);
	lv_obj_set_style_pad_gap(chooser, 10, 0);
	lv_obj_set_scrollable(chooser, false);
	lv_obj_set_flex_flow(chooser, LV_FLEX_FLOW_ROW);
	btn_gb = make_view_choice(chooser, "gearboy_gb", false);
	btn_gbc = make_view_choice(chooser, "gearboy_gbc", true);

	list_box = lv_obj_create(container);
	lv_obj_set_width(list_box, lv_pct(100));
	lv_obj_set_flex_grow(list_box, 1);
	lv_obj_set_style_bg_opa(list_box, 0, 0);
	lv_obj_set_style_border_width(list_box, 0, 0);
	lv_obj_set_style_radius(list_box, 0, 0);
	lv_obj_set_style_pad_all(list_box, 0, 0);
	lv_obj_set_style_pad_bottom(list_box, cfg->padding, 0);
	lv_obj_set_style_pad_row(list_box, 12, 0);
	lv_obj_set_style_pad_column(list_box, 12, 0);
	lv_obj_set_flex_flow(list_box, LV_FLEX_FLOW_ROW_WRAP);
	lv_obj_set_flex_align(list_box, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START);
	lv_obj_set_scroll_dir(list_box, LV_DIR_VER);
	switcher_attach_back_gesture(list_box);

	status_label = lv_label_create(gearboypage_screen);
	lv_label_set_text(status_label, "");
	lv_label_set_long_mode(status_label, LV_LABEL_LONG_WRAP);
	lv_obj_set_width(status_label, lv_pct(80));
	lv_obj_set_style_text_align(status_label, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_set_style_text_font(status_label, &font_ui_20, 0);
	lv_obj_add_style(status_label, &theme_style_text_dim, 0);
	lv_obj_center(status_label);

	refresh_view_buttons();
	theme_register_refresh(refresh_view_buttons);
	lv_obj_add_event_cb(gearboypage_screen, screen_loaded_cb, LV_EVENT_SCREEN_LOADED, NULL);
	switcher_attach_back_gesture(gearboypage_screen);
	art_timer = lv_timer_create(art_timer_cb, 250, NULL);
}
