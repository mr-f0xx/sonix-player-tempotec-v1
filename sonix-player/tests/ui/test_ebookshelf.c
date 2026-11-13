/* The EPUB shelf on the real V1 layout. Covers are supplied synchronously by
 * a recording worker; the page, directory scan, shell and LVGL are production.
 * No EPUB decoder, display driver or audio service is needed. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "src/gui/ebook/ebookmarkspage.h"
#include "src/gui/ebook/ebookcovers.h"
#include "src/gui/ebook/ebookpage.h"
#include "src/gui/ebook/ebookreader.h"
#include "src/gui/ebook/ebooksettings.h"
#include "src/gui/fonts/fonts.h"
#include "src/gui/nowplaying/cover.h"
#include "src/gui/nowplaying/player.h"
#include "src/gui/shell/switcher.h"
#include "src/gui/shell/theme.h"
#include "src/system/core/config.h"
#include "src/system/core/lang.h"

lv_obj_t *ebooksettings_screen, *ebookmarkspage_screen;
lv_font_t font_ui_14, font_ui_16, font_ui_18, font_ui_20, font_ui_20_bold;
lv_font_t font_ui_22, font_ui_22_bold, font_ui_24, font_ui_24_bold;
lv_font_t font_ui_26, font_ui_28, font_ui_32;

static ebookcovers_ready_cb cover_ready;
static int worker_w, worker_h, worker_count, visibility_requests, stops, frees;
static char opened_book[800];

uint32_t ebookcovers_start(const char *dir, const char (*files)[256], int count, int box_w, int box_h,
						   ebookcovers_ready_cb cb) {
	(void)dir;
	(void)files;
	worker_count = count;
	worker_w = box_w;
	worker_h = box_h;
	cover_ready = cb;
	return 1;
}
void ebookcovers_want_visible(uint32_t token, lv_obj_t *view, lv_obj_t *const *items, int count, int32_t margin) {
	(void)token;
	(void)view;
	(void)items;
	(void)count;
	(void)margin;
	visibility_requests++;
}
void ebookcovers_stop(uint32_t token) {
	(void)token;
	stops++;
	cover_ready = NULL;
}
void cover_free(cover_image_t *image) {
	free(image->pixels);
	memset(image, 0, sizeof(*image));
	frees++;
}
void ebookreader_open(const char *path) { snprintf(opened_book, sizeof(opened_book), "%s", path); }
void ebookmarkspage_open(void) {}
void switch_screen(lv_obj_t *screen) { lv_screen_load(screen); }
void switcher_attach_back_gesture(lv_obj_t *obj) { (void)obj; }
bool switcher_back_drag_active(void) { return false; }
void player_sheet_attach_drag(lv_obj_t *obj, bool opening) { (void)obj; (void)opening; }
bool player_sheet_drag_active(void) { return false; }
void fonts_register_change(void (*cb)(void)) { (void)cb; }
void bootlogo_set(int choice, bool dark) { (void)choice; (void)dark; }
bool bootpic_present(void) { return false; }

static void init_fonts(void) {
	lv_font_t *fonts[] = {&font_ui_14, &font_ui_16, &font_ui_18, &font_ui_20, &font_ui_20_bold,
						  &font_ui_22, &font_ui_22_bold, &font_ui_24, &font_ui_24_bold,
						  &font_ui_26, &font_ui_28, &font_ui_32};
	for (size_t i = 0; i < sizeof(fonts) / sizeof(fonts[0]); i++) {
		*fonts[i] = lv_font_montserrat_14;
	}
	font_ui_18 = font_ui_20 = font_ui_20_bold = lv_font_montserrat_20;
	font_ui_24 = font_ui_26 = font_ui_28 = font_ui_32 = lv_font_montserrat_28;
}

static void snapshot(void) {
	const char *path = getenv("SONIX_EBOOK_SNAPSHOT");
	if (!path || !path[0]) {
		return;
	}
	lv_draw_buf_t *shot = lv_snapshot_take(ebookpage_screen, LV_COLOR_FORMAT_RGB888);
	assert(shot);
	FILE *file = fopen(path, "wb");
	assert(file);
	fprintf(file, "P6\n%u %u\n255\n", shot->header.w, shot->header.h);
	for (uint32_t y = 0; y < shot->header.h; y++) {
		const uint8_t *row = shot->data + y * shot->header.stride;
		for (uint32_t x = 0; x < shot->header.w; x++) {
			uint8_t rgb[] = {row[x * 3 + 2], row[x * 3 + 1], row[x * 3]};
			assert(fwrite(rgb, sizeof(rgb), 1, file) == 1);
		}
	}
	assert(fclose(file) == 0);
	lv_draw_buf_destroy(shot);
}

// The scroller holds the empty note (a childless label) plus one row per pair.
// Identify rows by structure rather than child order: a row carries tiles.
static lv_obj_t *row_obj(lv_obj_t *page, int r) {
	int seen = 0;
	uint32_t n = lv_obj_get_child_count(page);
	for (uint32_t i = 0; i < n; i++) {
		lv_obj_t *c = lv_obj_get_child(page, (int)i);
		if (lv_obj_get_child_count(c) > 0) {
			if (seen == r) {
				return c;
			}
			seen++;
		}
	}
	assert(!"row not found");
	return NULL;
}

static lv_obj_t *shelf_tile(lv_obj_t *page, int index) {
	return lv_obj_get_child(row_obj(page, index / 2), index % 2);
}

static void check_tiles(lv_obj_t *page, int count) {
	lv_obj_update_layout(ebookpage_screen);
	int rows = (count + 1) / 2;
	assert(lv_obj_get_scroll_snap_y(page) == LV_SCROLL_SNAP_CENTER);
	int width = (lv_obj_get_content_width(page) - 14) / 2;
	int row_count = 0;
	uint32_t n = lv_obj_get_child_count(page);
	for (uint32_t i = 0; i < n; i++) {
		lv_obj_t *row = lv_obj_get_child(page, (int)i);
		if (lv_obj_get_child_count(row) == 0) {
			continue; // the empty note
		}
		assert(lv_obj_is_snappable(row));
		assert(lv_obj_get_width(row) == lv_obj_get_content_width(page));
		int cols = (row_count == rows - 1 && count % 2) ? 1 : 2;
		assert(lv_obj_get_child_count(row) == (uint32_t)cols);
		for (int c = 0; c < cols; c++) {
			lv_obj_t *tile = lv_obj_get_child(row, c);
			lv_obj_t *plate = lv_obj_get_child(tile, 0);
			lv_obj_t *label = lv_obj_get_child(tile, 1);
			int expected_h = lv_obj_get_style_pad_top(tile, 0) + lv_obj_get_height(plate) +
							 lv_obj_get_style_pad_row(tile, 0) + lv_obj_get_height(label) +
							 lv_obj_get_style_pad_bottom(tile, 0);
			assert(lv_obj_get_width(tile) == width);
			// Cards wrap their cover + caption + padding, not the viewport.
			assert(lv_obj_get_height(tile) == expected_h);
			assert(lv_obj_get_height(plate) == worker_h && lv_obj_get_width(plate) == worker_w);
			assert(lv_obj_get_width(label) == width - 20);
			assert(lv_obj_get_height(label) >= lv_font_get_line_height(&font_ui_18));
			assert(abs((lv_obj_get_x(plate) + lv_obj_get_width(plate) / 2) - lv_obj_get_content_width(tile) / 2) <= 1);
			assert(lv_obj_get_y(label) == lv_obj_get_y(plate) + lv_obj_get_height(plate) + 6);
		}
		row_count++;
	}
	assert(row_count == rows);
}

// One pair fills the screen at rest: the snapped row is centred in the
// viewport and no other row peeks in above or below it.
static void check_centered(lv_obj_t *page, int row_index, int rows) {
	// Bring the pair on screen the way a swipe would, then let the snap settle
	// it. Snapping only acts on a row already in view, exactly as on release.
	lv_obj_scroll_to_view(row_obj(page, row_index), LV_ANIM_OFF);
	lv_obj_update_snap(page, LV_ANIM_OFF);
	lv_obj_update_layout(ebookpage_screen);
	lv_area_t view, row;
	lv_obj_get_content_coords(page, &view);
	lv_obj_get_coords(row_obj(page, row_index), &row);
	int view_c = (view.y1 + view.y2) / 2;
	int row_c = (row.y1 + row.y2) / 2;
	assert(abs(row_c - view_c) <= 1); // always centred, even if a caption wraps tall
	// A pair that fits the viewport sits wholly inside it with no neighbour
	// peeking. (A pathologically long caption can wrap past the viewport; it is
	// still centred, but cannot be contained — that is not the resting case.)
	if (row.y2 - row.y1 + 1 <= view.y2 - view.y1 + 1) {
		assert(row.y1 >= view.y1 && row.y2 <= view.y2);
		if (row_index > 0) {
			lv_area_t prev;
			lv_obj_get_coords(row_obj(page, row_index - 1), &prev);
			assert(prev.y2 < view.y1);
		}
		if (row_index + 1 < rows) {
			lv_area_t next;
			lv_obj_get_coords(row_obj(page, row_index + 1), &next);
			assert(next.y1 > view.y2);
		}
	}
}

int main(int argc, char **argv) {
	assert(argc == 2);
	char root[] = "/tmp/sonix-ebookshelf-XXXXXX";
	assert(mkdtemp(root));
	char path[512];
	snprintf(path, sizeof(path), "%s/device_config.ini", root);
	assert(!config_init(path)); // a fresh card starts from defaults
	assert(mkdir((snprintf(path, sizeof(path), "%s/Ebook", root), path), 0700) == 0);
	const char *names[] = {"Maelstrom.epub", "Cache-cache.epub", "Very long book name.epub", "Without cover.epub", "Last.epub"};
	for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
		snprintf(path, sizeof(path), "%s/Ebook/%s", root, names[i]);
		FILE *file = fopen(path, "wb");
		assert(file);
		assert(fclose(file) == 0);
	}

	lv_init();
	init_fonts();
	lang_init(argv[1]);
	gui_config_t cfg = {240, 320, 6, 24, root};
	lv_display_create(cfg.screen_width, cfg.screen_height);
	theme_init();
	theme_set_kind(THEME_KIND_TOKYO_NIGHT);
	ebookpage_init(&cfg);
	lv_obj_t *menu = lv_obj_create(NULL);
	lv_screen_load(menu);
	lv_screen_load(ebookpage_screen);
	lv_obj_t *page = lv_obj_get_child(ebookpage_screen, 1); // the scroller
	assert(worker_count == 5);
	snapshot();
	check_tiles(page, 5);
	check_centered(page, 0, 3); // first pair centred, second pair not peeking

	// Metadata and artwork arrive after the placeholders have been laid out.
	// A wrapped title gets the room it needs; the picture adds no empty band.
	int height = lv_obj_get_height(shelf_tile(page, 0));
	int last_height = lv_obj_get_height(shelf_tile(page, 1));
	assert(cover_ready(1, "A title much longer than the available caption width", NULL) == false);
	lv_obj_update_layout(ebookpage_screen);
	check_tiles(page, 5);
	assert(lv_obj_get_height(shelf_tile(page, 1)) > last_height);
	uint8_t *pixels = calloc((size_t)worker_w * worker_h, 4);
	assert(pixels);
	cover_image_t image = {.pixels = pixels};
	image.dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
	image.dsc.header.cf = LV_COLOR_FORMAT_XRGB8888;
	image.dsc.header.w = worker_w;
	image.dsc.header.h = worker_h;
	image.dsc.header.stride = worker_w * 4;
	image.dsc.data_size = (size_t)worker_w * worker_h * 4;
	image.dsc.data = pixels;
	assert(cover_ready(0, "Maelstrom", &image));
	lv_obj_update_layout(ebookpage_screen);
	check_tiles(page, 5);
	assert(lv_obj_get_height(shelf_tile(page, 0)) == height);
	lv_obj_t *picture = lv_obj_get_child(lv_obj_get_child(shelf_tile(page, 0), 0), 1);
	const lv_image_dsc_t *src = lv_image_get_src(picture);
	assert(!lv_obj_is_hidden(picture) && src && src->data == pixels && src->header.w == (uint32_t)worker_w);
	lv_obj_send_event(shelf_tile(page, 0), LV_EVENT_CLICKED, NULL);
	snprintf(path, sizeof(path), "%s/Ebook/Cache-cache.epub", root);
	assert(strcmp(opened_book, path) == 0);

	// Scrolling settles on the next pair, centred, with no peek of its neighbours.
	int requests = visibility_requests;
	lv_obj_scroll_to_y(page, lv_obj_get_scroll_bottom(page), LV_ANIM_OFF);
	lv_obj_update_layout(ebookpage_screen);
	assert(visibility_requests > requests);
	check_tiles(page, 5);
	check_centered(page, 2, 3); // last (lone) pair centred, previous pair not peeking
	check_centered(page, 1, 3); // a middle pair: neither neighbour peeks

	// Changing the theme while the shelf is visible must only repaint it.
	for (int kind = 0; kind < THEME_KIND_COUNT; kind++) {
		theme_set_kind((theme_kind_t)kind);
		check_tiles(page, 5);
	}
	check_centered(page, 1, 3);
	font_ui_18 = lv_font_montserrat_28;
	lv_screen_load(menu);
	assert(frees == 1);
	lv_screen_load(ebookpage_screen);
	check_tiles(page, 5);
	check_centered(page, 0, 3);
	assert(lv_obj_get_height(shelf_tile(page, 0)) > height);

	lv_screen_load(menu);
	assert(stops >= 3 && cover_ready == NULL);
	lv_deinit();
	for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
		snprintf(path, sizeof(path), "%s/Ebook/%s", root, names[i]);
		assert(unlink(path) == 0);
	}
	snprintf(path, sizeof(path), "%s/Ebook", root);
	assert(rmdir(path) == 0);
	snprintf(path, sizeof(path), "%s/device_config.ini", root);
	assert(unlink(path) == 0);
	assert(rmdir(root) == 0);
	puts("PASS EPUB shelf: content-sized cards, one centred pair per screen, async covers, snapping and themes");
	return 0;
}
