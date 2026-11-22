// Exercise the browser's real recycled rows on LVGL without a card or worker.
#include <assert.h>
#include <stdio.h>
#include "lvgl/lvgl.h"
#include "src/gui/library/browser.c"

lv_font_t font_ui_14, font_ui_18;
lv_style_t theme_style_icon;
static theme_palette_t palette;
const theme_palette_t *theme(void) { return &palette; }
lv_color_t theme_browser_folder_color(void) { return lv_color_hex(0x123456); }
bool bp_is_tempotec_v1(void) { return true; }
int bp_pick(int compact, int regular) { (void)regular; return compact; }
const lv_font_t *bp_tile_label_font(void) { return &font_ui_14; }
int settingsrow_content_top(gui_config_t *cfg) { (void)cfg; return 72; }
static int released[COVERLOADER_SLOTS];
void coverloader_release(int slot) { assert(slot >= 0 && slot < COVERLOADER_SLOTS); released[slot]++; }
static int freed_images;
void cover_free(cover_image_t *image) {
	if (image && image->pixels) { free(image->pixels); freed_images++; }
	if (image) memset(image, 0, sizeof(*image));
}
static const uint8_t glyph[] = {255};
#define ICON(n) lv_image_dsc_t n = { .header = { .magic = LV_IMAGE_HEADER_MAGIC, .cf = LV_COLOR_FORMAT_A8, .w = 1, .h = 1, .stride = 1 }, .data = glyph, .data_size = 1 }
ICON(icon_folder); ICON(icon_music2); ICON(icon_album); ICON(icon_refresh);

int main(void) {
	// Only the actual Music tree, including mounts with a trailing slash or /.
	assert(browser_in_music("/card", "/card/Music"));
	assert(browser_in_music("/card/", "/card//Music/Artist/Album"));
	assert(browser_in_music("/", "/Music/Artist"));
	assert(!browser_in_music("/card", "/card"));
	assert(!browser_in_music("/card", "/card/Downloads"));
	assert(!browser_in_music("/card", "/card/Music Videos"));
	assert(!browser_in_music("/card", "/cards/Music"));
	assert(!browser_in_music("/card", "/card/Music/../Downloads"));
	assert(!browser_in_music("/card", "/card/music"));
	lv_init(); lv_display_create(240, 320);
	font_ui_14 = font_ui_18 = lv_font_montserrat_14;
	lv_style_init(&theme_style_icon);
	gui_config_t cfg = {.screen_width = 240, .screen_height = 320, .padding = 6};
	config = &cfg;
	lv_obj_t *screen = lv_screen_active();
	file_list = lv_obj_create(screen);
	lv_obj_set_size(file_list, 240, 248);
	lv_obj_set_style_pad_all(file_list, 0, 0);
	list_body = lv_obj_create(file_list);
	lv_obj_set_size(list_body, 228, 3000);
	lv_obj_set_style_pad_all(list_body, 0, 0);
	for (int i = 0; i < ROW_POOL; i++) {
		row_t *r = &rows[i];
		r->button = lv_button_create(list_body);
		r->icon = lv_image_create(r->button);
		r->label = lv_label_create(r->button);
		r->check = lv_image_create(r->button);
		r->playmark = lv_obj_create(r->button);
		r->index = -1;
	}
	for (int i = 0; i < 40; i++) {
		char name[32]; snprintf(name, sizeof(name), "Album %d", i);
		assert(listing_add(&shown, name, NULL, i != 1));
	}
	lv_obj_update_layout(screen);
	browse_grid = true; window_update(); lv_obj_update_layout(screen);
	albumgrid_metrics_t m; grid_metrics(&m);
	assert(m.columns == 2);
	assert(lv_obj_get_x(rows[0].button) == 0);
	assert(lv_obj_get_x(rows[1].button) == m.pitch_x);
	assert(lv_obj_get_y(rows[2].button) == m.pitch_y);
	assert(lv_obj_get_width(rows[0].icon) == m.cover);
	// Until a request completes, the refresh glyph means "loading", not
	// the no-art fallback. A completed miss gets the album glyph instead.
	assert(lv_image_get_src(rows[0].icon) == &icon_refresh);
	assert(lv_image_get_src(rows[1].icon) == &icon_refresh);
	cover_image_t fake = {0};
	fake.pixels = malloc(8); assert(fake.pixels);
	fake.dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
	fake.dsc.header.cf = LV_COLOR_FORMAT_RGB565;
	fake.dsc.header.w = fake.dsc.header.h = 2;
	fake.dsc.header.stride = 4;
	fake.dsc.data_size = 8;
	fake.dsc.data = fake.pixels;
	uint8_t *original_pixels = fake.pixels;
	row_finish_thumb(&rows[0], true, &fake);
	assert(lv_image_get_src(rows[0].icon) == &rows[0].thumb.dsc);
	row_bind(&rows[0], 16); // recycle a loaded row into the in-memory cache
	assert(lv_image_get_src(rows[0].icon) == &icon_refresh);
	row_bind(&rows[0], 0);
	assert(rows[0].thumb_settled && rows[0].thumb.pixels == original_pixels);
	assert(rows[0].thumb.dsc.data == original_pixels);
	assert(lv_image_get_src(rows[0].icon) == &rows[0].thumb.dsc);
	row_finish_thumb(&rows[1], false, NULL);
	assert(lv_image_get_src(rows[1].icon) == &icon_album);
	row_bind(&rows[1], 17);
	assert(lv_image_get_src(rows[1].icon) == &icon_refresh);
	row_bind(&rows[1], 1); // a known miss also survives recycling
	assert(rows[1].thumb_settled && lv_image_get_src(rows[1].icon) == &icon_album);
	row_t *prefetch = grid_prefetch_candidate();
	assert(prefetch && !row_is_visible(prefetch->index));
	// A neighbouring prefetch may continue while all visible rows are ready,
	// but it is abandoned as soon as a visible row needs the worker.
	for (int i = 0; i < ROW_POOL; i++) {
		if (rows[i].index >= 2 && row_is_visible(rows[i].index)) rows[i].thumb_settled = true;
	}
	prefetch->thumb_requested = true;
	int pref_slot = row_slot(prefetch), pref_before = released[pref_slot];
	cancel_offscreen_thumbs();
	assert(prefetch->thumb_requested && released[pref_slot] == pref_before);
	rows[2].thumb_settled = false;
	cancel_offscreen_thumbs();
	assert(!prefetch->thumb_requested && released[pref_slot] == pref_before + 1);
	for (int i = 2; i < ROW_POOL; i++) rows[i].thumb_settled = false;
	assert(row_is_visible(0) && row_is_visible(3));
	assert(!row_is_visible(6));
	// Scrolling cancels an off-screen pending cover even if its row remains
	// in the recycled pool; it can be requested again on the way back.
	rows[0].thumb_settled = false;
	rows[0].thumb_requested = true;
	int slot = row_slot(&rows[0]), before = released[slot];
	lv_obj_scroll_to_y(file_list, m.pitch_y, LV_ANIM_OFF);
	window_update();
	assert(rows[0].index == 0 && !rows[0].thumb_requested && released[slot] == before + 1);
	rows[0].thumb_settled = true; // retained decoded pixels still belong to this row
	lv_obj_scroll_to_y(file_list, 0, LV_ANIM_OFF); window_update();
	// Selection and the playing mark still belong to the entry in a tile.
	selecting = true; chosen = calloc(shown.count, 1); assert(chosen);
	chosen[0] = 1; row_update_selection(&rows[0]);
	assert(!lv_obj_is_hidden(rows[0].check));
	snprintf(current_path, sizeof(current_path), "/music");
	snprintf(np_path, sizeof(np_path), "/music/Album 0/song.flac");
	row_update_playmark(&rows[0]);
	assert(!lv_obj_is_hidden(rows[0].playmark));
	free(chosen); chosen = NULL; selecting = false;
	assert(browser_body_height() == albumgrid_canvas_height(40, &m));
	lv_obj_set_height(list_body, browser_body_height()); lv_obj_update_layout(screen);
	lv_obj_scroll_to_y(file_list, m.pitch_y * 8, LV_ANIM_OFF); window_update();
	int visible = albumgrid_row_at(lv_obj_get_scroll_y(file_list), &m);
	assert(rows[visible % ROW_POOL].index == visible);
	assert(rows[(visible + 3) % ROW_POOL].index == visible + 3);
	for (int i = 0; i < ROW_POOL; i++) row_bind(&rows[i], -1);
	lv_obj_scroll_to_y(file_list, 0, LV_ANIM_OFF);
	browse_grid = false; browse_covers = false; window_update(); lv_obj_update_layout(screen);
	assert(lv_obj_get_width(rows[0].button) == 228);
	assert(lv_obj_get_y(rows[1].button) == ROW_PITCH);
	assert(lv_image_get_src(rows[0].icon) == &icon_folder);
	assert(lv_image_get_src(rows[1].icon) == &icon_music2);
	for (int i = 0; i < ROW_POOL; i++) row_bind(&rows[i], -1);
	browse_covers = true; window_update();
	assert(lv_image_get_src(rows[0].icon) == &icon_refresh);
	assert(lv_image_get_src(rows[1].icon) == &icon_music2);
	grid_thumb_cache_clear();
	assert(freed_images >= 1);

	// The cache is bounded and owns its pixels. A ninth image evicts the
	// oldest; a different listing generation must never reuse an old cover.
	browse_grid = true;
	int freed_before = freed_images;
	for (int i = 0; i < GRID_THUMB_CACHE_SIZE + 1; i++) {
		row_t temp = {.index = i, .generation = 123, .thumb_settled = true, .has_thumb = true};
		temp.thumb.pixels = malloc(8); assert(temp.thumb.pixels);
		temp.thumb.dsc.data = temp.thumb.pixels;
		grid_thumb_cache_put(&temp);
		assert(!temp.has_thumb && !temp.thumb.pixels);
	}
	assert(freed_images == freed_before + 1);
	row_t lookup = {.index = 0, .generation = 123};
	assert(!grid_thumb_cache_take(&lookup)); // evicted
	lookup.index = 1; lookup.generation = 124;
	assert(!grid_thumb_cache_take(&lookup)); // another listing
	lookup.generation = 123;
	assert(grid_thumb_cache_take(&lookup) && lookup.has_thumb);
	cover_free(&lookup.thumb);
	grid_thumb_cache_clear();
	assert(freed_images == freed_before + GRID_THUMB_CACHE_SIZE + 1);
	puts("PASS browser cover grid, cache, prefetch, loading and list restore");
	return 0;
}
