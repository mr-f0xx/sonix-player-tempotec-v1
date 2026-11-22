/* SD icon sets: a set loads, swaps the pixels behind the built-in glyphs in
 * place, colours its roles from palette.ini, and puts everything back on
 * unload. The folders are written here -- no card, no reader, one solid red
 * PNG standing in for the artwork (any <id>.png would do; the loader only
 * knows ids from icons.h).
 */

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "lvgl/lvgl.h"

#include "src/gui/shell/iconset.h"
#include "src/gui/shell/icons.h"
#include "src/gui/shell/theme.h"

/* A 64x64 solid red PNG. The loader box-averages it down to each
 * descriptor's size, so every swapped icon comes out flat red. */
static const unsigned char red_png[] = {
	0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0x00, 0x00, 0x00, 0x0d,
	0x49, 0x48, 0x44, 0x52, 0x00, 0x00, 0x00, 0x40, 0x00, 0x00, 0x00, 0x40,
	0x08, 0x06, 0x00, 0x00, 0x00, 0xaa, 0x69, 0x71, 0xde, 0x00, 0x00, 0x00,
	0x95, 0x49, 0x44, 0x41, 0x54, 0x78, 0x9c, 0xed, 0xd0, 0x41, 0x11, 0x00,
	0x20, 0x0c, 0xc0, 0xb0, 0x81, 0x7f, 0xcf, 0x43, 0x46, 0x1e, 0x34, 0x0a,
	0x7a, 0x3d, 0x3b, 0xb3, 0xf3, 0xb1, 0xab, 0x03, 0xb4, 0x06, 0xe8, 0x00,
	0xad, 0x01, 0x3a, 0x40, 0x6b, 0x80, 0x0e, 0xd0, 0x1a, 0xa0, 0x03, 0xb4,
	0x06, 0xe8, 0x00, 0xad, 0x01, 0x3a, 0x40, 0x6b, 0x80, 0x0e, 0xd0, 0x1a,
	0xa0, 0x03, 0xb4, 0x06, 0xe8, 0x00, 0xad, 0x01, 0x3a, 0x40, 0x6b, 0x80,
	0x0e, 0xd0, 0x1a, 0xa0, 0x03, 0xb4, 0x06, 0xe8, 0x00, 0xad, 0x01, 0x3a,
	0x40, 0x6b, 0x80, 0x0e, 0xd0, 0x1a, 0xa0, 0x03, 0xb4, 0x06, 0xe8, 0x00,
	0xad, 0x01, 0x3a, 0x40, 0x6b, 0x80, 0x0e, 0xd0, 0x1a, 0xa0, 0x03, 0xb4,
	0x06, 0xe8, 0x00, 0xad, 0x01, 0x3a, 0x40, 0x6b, 0x80, 0x0e, 0xd0, 0x1a,
	0xa0, 0x03, 0xb4, 0x06, 0xe8, 0x00, 0xad, 0x01, 0x3a, 0x40, 0x6b, 0x80,
	0x0e, 0xd0, 0x1a, 0xa0, 0x03, 0xb4, 0x06, 0xe8, 0x00, 0xad, 0x01, 0x3a,
	0x40, 0x6b, 0x80, 0x0e, 0xd0, 0x1e, 0x53, 0x35, 0x02, 0x7e, 0xe0, 0xd9,
	0x51, 0x46, 0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4e, 0x44, 0xae, 0x42,
	0x60, 0x82,
};

static void write_file(const char *path, const void *data, size_t size) {
	FILE *f = fopen(path, "wb");
	assert(f);
	assert(fwrite(data, 1, size, f) == size);
	assert(fclose(f) == 0);
}

int main(void) {
	char root[] = "/tmp/sonix-iconset-XXXXXX";
	assert(mkdtemp(root));
	iconset_set_card_root(root);

	// What iconset_init() publishes to the theme.
	theme_set_icon_sources(iconset_active, iconset_semantic_color);

	assert(!iconset_active());
	assert(strcmp(iconset_active_name(), "") == 0);
	assert(iconset_choices(NULL, 0) == 0);

	const void *music_before = icon_music2.data;
	uint32_t music_before_size = icon_music2.data_size;
	const void *play_before = icon_play.data;

	// A set that isn't there changes nothing.
	assert(!iconset_select("missing"));
	assert(!iconset_active());

	char icons_dir[300];
	snprintf(icons_dir, sizeof(icons_dir), "%s/Icons", root);
	assert(mkdir(icons_dir, 0700) == 0);
	char set_dir[320];
	snprintf(set_dir, sizeof(set_dir), "%s/demo", icons_dir);
	assert(mkdir(set_dir, 0700) == 0);

	char path[340];
	snprintf(path, sizeof(path), "%s/music2.png", set_dir);
	write_file(path, red_png, sizeof(red_png));
	snprintf(path, sizeof(path), "%s/palette.ini", set_dir);
	write_file(path, "green = #00ff00\n", 16);

	const char *names[4];
	int choices = iconset_choices(names, 4);
	assert(choices == 1);
	assert(strcmp(names[0], "demo") == 0);

	// Loading swaps the pixels behind icon_music2 in place; icon_play, which
	// the set doesn't ship, keeps its built-in bitmap.
	assert(iconset_select("demo"));
	assert(iconset_active());
	assert(strcmp(iconset_active_name(), "demo") == 0);
	assert(icon_music2.data != music_before);
	assert(icon_music2.data_size == icon_music2.header.w * icon_music2.header.h * 4);
	assert(icon_music2.header.w == 32 && icon_music2.header.h == 32);
	const unsigned char *px = icon_music2.data;
	assert(px[0] == 0 && px[1] == 0 && px[2] == 255 && px[3] == 255); // BGRA red
	assert(icon_play.data == play_before);
	assert(music_before_size == icon_music2.header.w * icon_music2.header.h * 4);

	// The theme answers in the set's colours now, in its own glyph mode.
	assert(theme_uses_function_glyphs());
	lv_color_t green;
	assert(iconset_semantic_color(THEME_SEMANTIC_GREEN, &green));
	assert(lv_color_eq(green, lv_color_make(0, 255, 0)));
	assert(lv_color_eq(theme_semantic_color(THEME_SEMANTIC_GREEN), lv_color_make(0, 255, 0)));
	// A role the palette doesn't define falls back to the theme's own colour.
	lv_color_t blue;
	assert(!iconset_semantic_color(THEME_SEMANTIC_BLUE, &blue));
	assert(lv_color_eq(theme_semantic_color(THEME_SEMANTIC_BLUE), theme()->accent));

	// A folder with no icons the firmware knows is a failed load, and the
	// loaded set stays loaded.
	char empty_dir[340];
	snprintf(empty_dir, sizeof(empty_dir), "%s/empty", icons_dir);
	assert(mkdir(empty_dir, 0700) == 0);
	assert(!iconset_select("empty"));
	assert(iconset_active());
	assert(strcmp(iconset_active_name(), "demo") == 0);

	// Unloading puts the built-ins back, byte for byte.
	assert(iconset_select(""));
	assert(!iconset_active());
	assert(icon_music2.data == music_before);
	assert(icon_music2.data_size == music_before_size);
	assert(!theme_uses_function_glyphs());
	assert(lv_color_eq(theme_semantic_color(THEME_SEMANTIC_GREEN), theme()->accent));

	// And a set can be loaded again after that.
	assert(iconset_select("demo"));
	assert(iconset_active());
	assert(icon_music2.data != music_before);
	assert(iconset_select(""));

	printf("PASS iconset swap, palette and restore\n");
	return 0;
}
