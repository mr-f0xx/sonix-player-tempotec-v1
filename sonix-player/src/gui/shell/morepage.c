#include "morepage.h"

#include <string.h>

#include "lvgl/lvgl.h"

#include "src/gui/audio/dacpage.h"
#include "src/gui/board_profile.h"
#include "src/gui/calculator/calculator.h"
#include "src/gui/ebook/ebookpage.h"
#include "src/gui/flappybird/flappybird.h"
#include "src/gui/games/dino.h"
#include "src/gui/games/game2048.h"
#include "src/gui/games/snake.h"
#include "src/gui/gearboy/gearboypage.h"
#include "src/gui/library/audiobooks.h"
#include "src/gui/library/filespage.h"
#include "src/gui/metronome/metronome.h"
#include "src/gui/notes/notes.h"
#include "src/gui/tagmanager/tagmanager.h"
#include "src/gui/pet/petpage.h"
#include "src/gui/pet/peticon.h"
#include "src/gui/nowplaying/player.h"
#include "src/gui/shell/gridpage.h"
#include "src/gui/shell/icons.h"
#include "src/gui/shell/main_menu.h"
#include "src/gui/shell/settingsrow.h"
#include "src/gui/shell/switcher.h"
#include "src/gui/shell/theme.h"

lv_obj_t *morepage_screen;

static lv_obj_t *more_grid;
static lv_obj_t *more_list;
static gui_config_t *more_cfg;

// Whether the Audiobooks row is part of More right now, so entering the page
// only rebuilds it when the home-screen choice has changed since it was built.
static bool more_audiobooks_shown;

static void more_action_cb(lv_event_t *e) {
	void (*action)(void) = (void (*)(void))lv_event_get_user_data(e);
	if (action) {
		action();
	}
}

static bool more_tokyo_icon(const char *label, const lv_image_dsc_t **icon, theme_semantic_t *tone) {
	if (!label || !icon || !tone) {
		return false;
	}
	if (strcmp(label, "dac") == 0) {
		*icon = &icon_usbaudioout;
		*tone = THEME_SEMANTIC_CYAN;
	} else if (strcmp(label, "gearboy") == 0) {
		*icon = &icon_files_game;
		*tone = THEME_SEMANTIC_GREEN;
	} else if (strcmp(label, "books") == 0) {
		*icon = &icon_files_book;
		*tone = THEME_SEMANTIC_AMBER;
	} else if (strcmp(label, "audiobooks") == 0) {
		*icon = &icon_files_audiobook;
		*tone = THEME_SEMANTIC_AMBER;
	} else if (strcmp(label, "file_explorer") == 0) {
		*icon = &icon_folder;
		*tone = THEME_SEMANTIC_BLUE;
	} else if (strcmp(label, "flappy_bird") == 0) {
		*icon = &icon_bird;
		*tone = THEME_SEMANTIC_PURPLE;
	} else if (strcmp(label, "2048") == 0) {
		*icon = &icon_game_2048;
		*tone = THEME_SEMANTIC_BLUE;
	} else if (strcmp(label, "snake") == 0) {
		*icon = &icon_game_snake;
		*tone = THEME_SEMANTIC_GREEN;
	} else if (strcmp(label, "dino") == 0) {
		*icon = &icon_game_dino;
		*tone = THEME_SEMANTIC_AMBER;
	} else if (strcmp(label, "calculator") == 0) {
		*icon = &icon_calculator;
		*tone = THEME_SEMANTIC_CYAN;
	} else if (strcmp(label, "Tag Manager") == 0) {
		*icon = &icon_folder;
		*tone = THEME_SEMANTIC_BLUE;
	} else if (strcmp(label, "notes") == 0) {
		*icon = &icon_notes;
		*tone = THEME_SEMANTIC_PURPLE;
	} else if (strcmp(label, "metronome") == 0) {
		*icon = &icon_metronome;
		*tone = THEME_SEMANTIC_GREEN;
	} else if (strcmp(label, "pet") == 0) {
		*icon = &icon_pet;
		*tone = THEME_SEMANTIC_PURPLE;
	} else {
		return false;
	}
	return true;
}

static void more_list_add_entry(const grid_entry_t *entry) {
	lv_event_cb_t callback = entry->target ? switch_screen_cb : more_action_cb;
	void *user_data = entry->target ? (void *)*entry->target : (void *)entry->action;
	lv_obj_t *row = settingsrow_add(more_list, entry->label, NULL, callback, user_data);
	const lv_image_dsc_t *icon = NULL;
	theme_semantic_t tone;
	if (more_tokyo_icon(entry->label, &icon, &tone)) {
		settingsrow_add_icon(row, icon, tone);
	}
}

// The rows in the order the page shows them: File Manager near the top on the
// short V1 screen, where the bottom of a long list otherwise falls below the
// initial viewport, then everything else in declaration order.
static void more_list_add_entries(const grid_entry_t *entries, int count) {
	static const char *const priority_labels[] = {"dac", "file_explorer"};

	for (size_t p = 0; p < sizeof(priority_labels) / sizeof(priority_labels[0]); p++) {
		for (int i = 0; i < count; i++) {
			if (strcmp(entries[i].label, priority_labels[p]) == 0) {
				more_list_add_entry(&entries[i]);
				break;
			}
		}
	}

	for (int i = 0; i < count; i++) {
		if (strcmp(entries[i].label, "dac") == 0 || strcmp(entries[i].label, "file_explorer") == 0) {
			continue;
		}
		more_list_add_entry(&entries[i]);
	}
}

// The shape of the tiled page.
//
// Every other tiled page in the player holds six destinations and is drawn as
// the two-column, three-row page the main menu uses. More is the one page that
// grew: with the calculator and the notes shelf it holds seven, and a seventh
// tile in a 2x3 grid would be laid out past the bottom of an unscrollable page
// -- visible to nobody. So the shape follows the count: six or fewer stay as
// they are, and seven or more move to a third column.
//
// Three columns are what a 240 px V1 can afford without shrinking the tiles
// below the size the rest of the interface draws them: at 72 px the 46 px
// pictogram still fits above a one-line caption, and on a 480-pixel player the
// tiles stay big enough for the 112 px menu art.
#define MORE_GRID_ROWS 3

static int more_grid_columns(int count) { return count > 2 * MORE_GRID_ROWS ? 3 : 2; }

// Everything More holds. The array stays constant; more_entries_build() copies
// it and swaps the library entry for whichever of the two libraries the home
// screen is not showing.
static const grid_entry_t MORE_ENTRIES[] = {
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
	{"Tag Manager", &icon_menu_file_explorer, NULL, tagmanager_open},
	// An action: the artwork is read from the resource tree on the way in.
	{"flappy_bird", &icon_menu_flappy_bird, NULL, flappybird_open},
	// The three games take the same route as Flappy Bird: an action rather than
	// a screen, because a run is read back from the config on the way in. 2048
	// picks up where its board was left, and the other two agree with the best
	// score on the card. Each has a drawing of what it is about, so the picture
	// says which game it is before the caption does.
	{"2048", &icon_menu_game_2048, NULL, game2048_open},
	{"snake", &icon_menu_game_snake, NULL, snake_open},
	{"dino", &icon_menu_game_dino, NULL, dino_open},
	// Both of these are actions rather than screens for the file
	// manager's reason: the calculator repaints its display from the sum
	// it was left holding, and the notes shelf reads the card.
	{"calculator", &icon_menu_calculator, NULL, calculator_open},
	{"notes", &icon_menu_notes, NULL, notes_open},
	// An action for the calculator's reason: the tempo and the bar come back
	// from the settings as they were left, and the page repaints them on the
	// way in.
	{"metronome", &icon_menu_metronome, NULL, metronome_open},
	// The pet is an action for the same reason, and one more: it is built the
	// first time it is opened, so a player who never visits it pays nothing for
	// it. What it knows about the music comes from a one-second tick that runs
	// whether or not the page is up (petwatch.c), which is why it is not simply
	// a screen with a timer of its own.
	{"pet", &icon_pet, NULL, petpage_open},
};

#define MORE_ENTRIES_MAX 13

// The page's destinations for the settings in force now.
//
// The home screen has one slot for the two libraries: Audiobooks normally, and
// Books when Settings -> Appearance hands it over. The same rule applies here,
// so the two never appear twice in the interface at once: with the option off
// this page offers Books, exactly as it always has, and with it on Books has
// moved up to the home screen and this entry becomes the Audiobooks section
// instead. The place, the count and the page's shape are the same either way.
static int more_entries_build(grid_entry_t *out, int max) {
	int count = 0;
	bool books_on_home = main_menu_books();
	int base_count = (int)(sizeof(MORE_ENTRIES) / sizeof(MORE_ENTRIES[0]));

	for (int i = 0; i < base_count && count < max; i++) {
		out[count] = MORE_ENTRIES[i];
		if (books_on_home && strcmp(MORE_ENTRIES[i].label, "books") == 0) {
			out[count].label = "audiobooks";
			out[count].icon = &icon_menu_audiobooks;
			out[count].target = NULL;
			out[count].action = audiobooks_open;
		}
		count++;
	}
	return count;
}

// Rebuilds both shapes of the page from the current settings. Used once at
// startup and again whenever the Books-on-home choice has changed under a page
// that is already built.
static void more_entries_refresh(void) {
	if (!more_cfg) {
		return;
	}

	grid_entry_t entries[MORE_ENTRIES_MAX];
	int count = more_entries_build(entries, MORE_ENTRIES_MAX);
	more_audiobooks_shown = main_menu_books();

	if (more_grid) {
		gridpage_replace_entries(more_grid, more_cfg, entries, count, more_grid_columns(count), MORE_GRID_ROWS);
	}
	if (more_list) {
		lv_obj_clean(more_list);
		more_list_add_entries(entries, count);
	}
}

// More is built once, at startup, while the home-screen choice lives on the
// Appearance page and can change later. Entering the page is the moment to
// catch up, and only when the answer has actually changed.
static void more_screen_loaded_cb(lv_event_t *e) {
	(void)e;
	if (more_audiobooks_shown != main_menu_books()) {
		more_entries_refresh();
	}
}

static void more_refresh_theme(void) {
	bool tokyo_v1 = bp_is_tempotec_v1() && theme_uses_function_glyphs();
	if (more_grid) {
		lv_obj_set_hidden(more_grid, tokyo_v1);
	}
	if (more_list) {
		lv_obj_set_hidden(more_list, !tokyo_v1);
	}
}

void morepage_init(gui_config_t *cfg) {
	more_cfg = cfg;

	grid_entry_t entries[MORE_ENTRIES_MAX];
	int count = more_entries_build(entries, MORE_ENTRIES_MAX);
	more_audiobooks_shown = main_menu_books();

	// Dark and Light retain the established two-column grid. Tokyo Night uses a
	// compact, single-column list whose rows share the Settings list styling.
	more_grid = gridpage_build(morepage_screen, cfg, entries, count, more_grid_columns(count), MORE_GRID_ROWS, true);
	if (bp_is_tempotec_v1()) {
		more_list = settingsrow_page(morepage_screen, cfg, "more");
		player_sheet_attach_drag(more_list, true);
		more_list_add_entries(entries, count);
	} else {
		settingsrow_title(morepage_screen, cfg, "more");
	}
	more_refresh_theme();
	theme_register_refresh(more_refresh_theme);
	lv_obj_add_event_cb(morepage_screen, more_screen_loaded_cb, LV_EVENT_SCREEN_LOADED, NULL);
}
