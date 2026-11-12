#include "streaming.h"

#include "lvgl/lvgl.h"

#include "src/gui/board_profile.h"
#include "src/gui/shell/gridpage.h"
#include "src/gui/shell/icons.h"
#include "src/gui/shell/settingsrow.h"
#include "src/gui/shell/switcher.h"
#include "src/gui/shell/theme.h"
#include "src/gui/nowplaying/player.h"
#include "src/gui/streaming/podcastpage.h"
#include "src/gui/streaming/qobuzpage.h"
#include "src/gui/streaming/radiopage.h"
#include "src/gui/streaming/soundcloud_icon.h"
#include "src/gui/streaming/soundcloudpage.h"
#include "src/gui/streaming/tidalpage.h"
#include "src/gui/streaming/subsonicpage.h"
#include "src/system/addons/addons.h"

lv_obj_t *streaming_screen;

static lv_obj_t *streaming_grid;
static lv_obj_t *streaming_list;

typedef struct {
	const char *label;
	const lv_image_dsc_t *icon;
	theme_semantic_t tone;
	lv_obj_t **target;
	const char *addon; // the Plugins switch that owns this service
} streaming_item_t;

// A service switched off in Plugins gets no tile, and its pages are never built
// (see streaming_init), so nothing leads to an empty screen.
static const streaming_item_t tokyo_items[] = {
	{"radio", &icon_radio_player, THEME_SEMANTIC_PURPLE, &radiopage_screen, "radio"},
	{"podcasts", &icon_podcast_list, THEME_SEMANTIC_CYAN, &podcast_screen, "podcasts"},
	{"soundcloud", &icon_soundcloud_mark, THEME_SEMANTIC_AMBER, &soundcloud_screen, "soundcloud"},
	{"tidal", &icon_tidal_mark, THEME_SEMANTIC_CYAN, &tidal_screen, "tidal"},
	{"qobuz", &icon_qobuz_mark, THEME_SEMANTIC_PURPLE, &qobuz_screen, "qobuz"},
	{"subsonic", &icon_music_note, THEME_SEMANTIC_BLUE, &subsonic_screen, "subsonic"},
};

static void streaming_list_build(gui_config_t *cfg) {
	streaming_list = settingsrow_page(streaming_screen, cfg, "streaming");
	player_sheet_attach_drag(streaming_list, true);

	for (unsigned int i = 0; i < sizeof(tokyo_items) / sizeof(tokyo_items[0]); i++) {
		const streaming_item_t *item = &tokyo_items[i];
		if (!addons_enabled(item->addon)) {
			continue;
		}
		lv_obj_t *row = settingsrow_add(streaming_list, item->label, NULL, switch_screen_cb, *item->target);
		settingsrow_add_icon(row, item->icon, item->tone);
	}
}

static void streaming_refresh_theme(void) {
	bool tokyo_v1 = bp_is_tempotec_v1() && theme_uses_function_glyphs();
	if (streaming_grid) {
		lv_obj_set_hidden(streaming_grid, tokyo_v1);
	}
	if (streaming_list) {
		lv_obj_set_hidden(streaming_list, !tokyo_v1);
	}
}

void streaming_init(gui_config_t *cfg) {
	// Each service's pages are built only while its Plugins switch is on.
	if (addons_enabled("qobuz")) {
		qobuzpage_init(cfg);
	}
	if (addons_enabled("tidal")) {
		tidalpage_init(cfg);
	}
	if (addons_enabled("podcasts")) {
		podcastpage_init(cfg);
	}
	if (addons_enabled("subsonic")) {
		subsonicpage_init(cfg);
	}
	if (addons_enabled("soundcloud")) {
		soundcloudpage_init(cfg);
	}

	const grid_entry_t all_entries[] = {
		{"soundcloud", &icon_menu_soundcloud, &soundcloud_screen, NULL},
		{"tidal", &icon_menu_tidal, &tidal_screen, NULL},
		{"qobuz", &icon_menu_qobuz, &qobuz_screen, NULL},
		{"radio", &icon_menu_radio, &radiopage_screen, NULL},
		{"podcasts", &icon_menu_podcast, &podcast_screen, NULL},
		{"subsonic", &icon_menu_streaming, &subsonic_screen, NULL},
	};
	const char *const all_addons[] = {"soundcloud", "tidal", "qobuz", "radio", "podcasts", "subsonic"};
	grid_entry_t entries[sizeof(all_entries) / sizeof(all_entries[0])];
	int count = 0;
	for (unsigned int i = 0; i < sizeof(all_entries) / sizeof(all_entries[0]); i++) {
		if (addons_enabled(all_addons[i])) {
			entries[count++] = all_entries[i];
		}
	}

	// Dark and Light keep their established two-column grid. Tokyo Night on the
	// V1 gets the same destinations as a compact, icon-led list.
	streaming_grid = gridpage_build(streaming_screen, cfg, entries, count, 2, 3, true);
	if (bp_is_tempotec_v1()) {
		streaming_list_build(cfg);
	} else {
		settingsrow_title(streaming_screen, cfg, "streaming");
	}
	streaming_refresh_theme();
	theme_register_refresh(streaming_refresh_theme);
}
