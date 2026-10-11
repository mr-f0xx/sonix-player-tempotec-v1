#include "morepage.h"

#include "lvgl/lvgl.h"

#include "src/gui/audio/dacpage.h"
#include "src/gui/ebook/ebookpage.h"
#include "src/gui/flappybird/flappybird.h"
#include "src/gui/library/filespage.h"
#include "src/gui/gearboy/gearboypage.h"
#include "src/gui/board_profile.h"
#include "src/gui/shell/gridpage.h"
#include "src/gui/shell/icons.h"
#include "src/gui/shell/settingsrow.h"
#include "src/gui/shell/theme.h"

lv_obj_t *morepage_screen;

static lv_obj_t *more_grid;
static gui_config_t *more_cfg;

#if GB_CORE
#define MORE_FILE_INDEX 3
#define MORE_BIRD_INDEX 4
#else
#define MORE_FILE_INDEX 2
#define MORE_BIRD_INDEX 3
#endif

// Tokyo Night keeps the final Flappy Bird card square and centered, so the
// line-drawn bird sits in the middle of its own tile rather than on a wide bar.
// The other palettes retain the original full-width final tile and artwork.
static void more_refresh_theme(void) {
	if (!more_grid || !more_cfg) {
		return;
	}

	bool tokyo_v1 = bp_is_tempotec_v1() && theme_is_tokyo_night();
	int padding = more_cfg->padding;
	int gap = bp_is_tempotec_v1() ? 6 : 14;
	int usable_width = (int)more_cfg->screen_width - 2 * padding;
	int tile_width = (usable_width - gap) / 2;
	int count = (int)lv_obj_get_child_count(more_grid);

	if (tokyo_v1 && count == 5) {
		lv_obj_set_width(lv_obj_get_child(more_grid, MORE_BIRD_INDEX), tile_width);
		lv_obj_set_flex_align(more_grid, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START);
		for (int i = 0; i < count; i++) {
			gridpage_set_tile_orientation(more_grid, i, i != MORE_BIRD_INDEX);
		}
		gridpage_set_tile_label_lines(more_grid, MORE_FILE_INDEX, 2);
	} else {
		if (count == 5) {
			lv_obj_set_width(lv_obj_get_child(more_grid, MORE_BIRD_INDEX), usable_width);
		}
		lv_obj_set_flex_align(more_grid, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START);
		for (int i = 0; i < count; i++) {
			gridpage_set_tile_orientation(more_grid, i, tokyo_v1);
		}
		gridpage_set_tile_label_lines(more_grid, MORE_FILE_INDEX, 1);
	}
	lv_obj_update_layout(more_grid);
}

void morepage_init(gui_config_t *cfg) {
	const grid_entry_t entries[] = {
		{"dac", &icon_menu_dac, &dacpage_screen, NULL},
#if GB_CORE
		// The Game Boy emulator. Present only when the binary was built with
		// GB=1 (see the Makefile).
		{"gearboy", &icon_menu_gearboy, &gearboypage_screen, NULL},
#endif
		// An action rather than a screen: the shelf reads the Ebook folder again
		// every time it is opened, so a book copied onto the card turns up
		// without a restart.
		{"books", &icon_menu_books, NULL, ebookpage_open},
		// An action for the same reason the shelf is one: the listing is read
		// when the page is entered, so a card that changed under the player
		// shows what is on it now.
		{"file_explorer", &icon_menu_file_explorer, NULL, filespage_open},
		// An action: the artwork is read from the resource tree on the way in.
		{"flappy_bird", &icon_menu_flappy_bird, NULL, flappybird_open},
	};

	// Two columns by three rows, like the Music and Wireless pages, so a tile
	// here is exactly the size of a tile there. The grid is laid out as full
	// even with fewer entries, so the only tile sits where it would with six --
	// top left -- instead of floating in the middle of a page of its own.
	more_cfg = cfg;
	more_grid = gridpage_build(morepage_screen, cfg, entries, (int)(sizeof(entries) / sizeof(entries[0])), 2, 3, true);
	settingsrow_title(morepage_screen, cfg, "more");
	more_refresh_theme();
	theme_register_refresh(more_refresh_theme);
}
