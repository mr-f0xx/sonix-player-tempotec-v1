#include "ytmusicpage.h"

#include <dirent.h>
#include <malloc.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include "src/gui/board_profile.h"
#include "src/gui/settings/screensaver.h"
#include "src/gui/library/browser.h"
#include "src/system/addons/addons.h"
#include "src/gui/library/medialist.h"
#include "src/gui/nowplaying/cover.h"
#include "src/gui/shell/icons.h"
#include "src/gui/shell/keyboard.h"
#include "src/gui/shell/popover.h"
#include "src/gui/shell/settingsrow.h"
#include "src/gui/shell/spinner.h"
#include "src/gui/shell/switcher.h"
#include "src/gui/shell/theme.h"
#include "src/gui/shell/toast.h"
#include "src/gui/nowplaying/player.h"
#include "src/gui/fonts/fonts.h"
#include "src/gui/wireless/wifisettings.h"
#include "src/gui/streaming/qobuzart.h"
#include "src/system/core/config.h"
#include "src/system/core/utils.h"
#include "src/system/core/lang.h"
#include "src/system/device/system.h"
#include "src/system/playback/sleeptimer.h"
#include "src/system/net/wifi.h"
#include "src/system/decode/growfile.h"
#include "src/system/playback/device_state.h"
#include "src/system/playback/playlist.h"
#include "src/system/streaming/streamkeep.h"
#include "src/system/streaming/ytmusic.h"
#include "src/system/streaming/ytlikes.h"
#include "src/system/streaming/ytpodcastplayed.h"

#define YTMUSIC_PAGE_RESULTS YTMUSIC_ITEMS_MAX
#define YTMUSIC_QUERY_MAX 192

// The page is a small tree: the home screen lists what can be opened, the
// Explore screen lists the store's carousels, the search screen takes a query,
// and two shared lists show what came back -- the results screen for a search
// or a chart, the tracks screen for whatever a card opens (a playlist, an
// album, a podcast show). A tap on a card loads its page into the tracks
// screen, so drilling down is one screen at a time and the chevron walks back
// out of it.
lv_obj_t *ytmusic_screen;

static gui_config_t *page_cfg;
static lv_obj_t *home_container;
static lv_obj_t *search_screen;
static lv_obj_t *search_title;
static lv_obj_t *search_field;
static keyboard_t *search_keyboard;
static lv_obj_t *results_screen;
static lv_obj_t *results_list;
static lv_obj_t *results_title;
static lv_obj_t *tracks_screen;
static lv_obj_t *tracks_list;
static lv_obj_t *tracks_title;
static lv_obj_t *podcast_order_button;
static lv_obj_t *explore_screen;
static lv_obj_t *explore_list;
static lv_obj_t *settings_screen;
static lv_obj_t *sign_out_row;
static lv_obj_t *source_screen;
static lv_obj_t *source_list;
static lv_obj_t *source_value_label;
static ytmusic_source_t source_items[YTMUSIC_SOURCES_MAX];
static int source_item_count;
static pthread_mutex_t page_lock = PTHREAD_MUTEX_INITIALIZER;
// One network job at a time: a search, an Explore load, a card's page, or the
// download-and-play of a track. They share the request lock in the system
// layer and the user can only be looking at one of them.
static bool page_busy;
static unsigned list_generation;
static ytmusic_filter_t search_filter;
static int search_kind_index; // 0 tracks, 1 albums, 2 artists
static lv_obj_t *search_pills[3];
static lv_obj_t *search_pill_row;
static ytmusic_item_t result_items[YTMUSIC_PAGE_RESULTS];
static int result_count;
static ytmusic_item_t track_items[YTMUSIC_PAGE_RESULTS];
static int track_count;
static ytmusic_section_t explore_sections[YTMUSIC_SECTIONS_MAX];
static int explore_section_count;
// True while the results screen is showing one of Explore's carousels. A card
// there is an album or a single, and its folder holds a row per track -- for a
// single, one row with nothing but a name. A tap on a card in this list plays
// it instead of opening that page; everywhere else a card still opens.
static bool explore_cards;

// The album a downloaded song lands in. A song row's own second column names the
// album where YouTube sends one; an album's own track list does not, so the
// title of the list the song was opened from stands in for it. Empty when
// neither did, and the downloader's plain "Unknown album" is the honest answer
// then.
static char list_album[YTMUSIC_TITLE_MAX];

// The show a page of episodes was opened from. YouTube names the show in the
// page's header and nowhere on its rows, so every episode of a show would be a
// title with nothing under it -- and a download would carry no creator into the
// now-playing screen. Read the same way list_album is, from the card tapped.
static char list_show[YTMUSIC_TITLE_MAX];
static char list_artist[YTMUSIC_NAME_MAX];
static char list_thumbnail[YTMUSIC_URL_MAX];

// Whether the tracks screen is showing the favourites this player keeps itself
// rather than a card opened from the store. Only that list has to be rebuilt
// when one of its rows is unliked; everywhere else a heart changes nothing that
// is on screen.
static bool tracks_are_likes;
static void podcast_rows_refresh(void);
static void fill_item_list(lv_obj_t *list, ytmusic_item_t *items, int count, lv_event_cb_t cb);
static void item_clicked_cb(lv_event_t *e);

static bool podcast_newest_first(void) {
	return config_get_bool("ytmusic", "podcast_newest_first", true);
}

static bool items_are_podcast_episodes(const ytmusic_item_t *items, int count) {
	if (!items || count <= 0) return false;
	for (int i = 0; i < count; i++) {
		if (!items[i].podcast_episode) return false;
	}
	return true;
}

static void podcast_order_paint(void) {
	if (!podcast_order_button) return;
	lv_obj_t *icon = lv_obj_get_child(podcast_order_button, 0);
	if (icon) {
		lv_image_set_src(icon, podcast_newest_first() ? &icon_sort_date_new : &icon_sort_date_old);
		settingsrow_scale_corner_icon(icon, page_cfg);
	}
}

static void podcast_order_cb(lv_event_t *e) {
	(void)e;
	if (switcher_back_drag_active() || player_sheet_drag_active() ||
		!items_are_podcast_episodes(track_items, track_count)) return;
	bool newest = podcast_newest_first();
	config_set_bool("ytmusic", "podcast_newest_first", !newest);
	if (!config_save()) {
		config_set_bool("ytmusic", "podcast_newest_first", newest);
		toast_error("podcast_save_failed");
		return;
	}
	for (int i = 0, j = track_count - 1; i < j; i++, j--) {
		ytmusic_item_t tmp = track_items[i];
		track_items[i] = track_items[j];
		track_items[j] = tmp;
	}
	podcast_order_paint();
	fill_item_list(tracks_list, track_items, track_count, item_clicked_cb);
}

// The rows of the home page: which of them are there, and in what order. Both
// are the page's own setting, kept in the config as the numbers below -- the
// order as one list, the switched-off ones as another -- so somebody who opens
// Top songs every day does not scroll past Explore to reach it, and somebody
// who never opens Explore does not see it at all.
//
// Search is not one of these rows: it has a corner button in the title bar,
// and a row here only ever offered the same page twice. An arrangement saved
// by an older build still carries its old id and numbers the rest from 1;
// yt_home_migrate() rewrites that once, and everything written afterwards is
// in the numbers below.
enum {
	YT_HOME_EXPLORE = 0,
	YT_HOME_CHARTS,
	YT_HOME_FAVOURITES,
	YT_HOME_PODCASTS,
	YT_HOME_ROWS,
};
static const char *const YT_HOME_SLUGS[YT_HOME_ROWS] = {
	"ytmusic_explore",
	"ytmusic_charts",
	"ytmusic_favourites",
	"ytmusic_podcasts",
};
static int yt_home_order[YT_HOME_ROWS];
static bool yt_home_hidden[YT_HOME_ROWS];

// The three settings this page keeps for what it plays: how good the stream
// should be, which country's catalogue it comes from, and when to stop.
static lv_obj_t *quality_pills[2];
static settingsrow_duration_t yt_sleep_row;
// One row per entry of the shared region list (system/core/region.h), which
// is Worldwide plus one per language the firmware ships: every entry has to
// have a row here, or a region chosen on the podcast page could not be shown
// as chosen on this one.
#define YT_REGION_MAX 16
static lv_obj_t *region_screen;
static lv_obj_t *region_list;
static lv_obj_t *region_value_label;
static ytmusic_region_t region_items[YT_REGION_MAX];
static int region_item_count;
static lv_obj_t *order_screen;
static lv_obj_t *order_rows;

// The corner of whichever list is on screen, while rows are chosen: the tick
// queues them onto the card and the cross leaves the mode. Both list pages
// carry a pair, hidden until a long press asks for them.
static lv_obj_t *sel_action_btn;
static lv_obj_t *sel_close_btn;
static lv_obj_t *results_sel_action;
static lv_obj_t *results_sel_close;
static lv_obj_t *tracks_sel_action;
static lv_obj_t *tracks_sel_close;

// How far a queue of chosen songs has got, for the loading card: written by the
// worker and read by the card's timer. An int each, because a percentage and a
// position are all that is shown and a byte count would need a lock.
static volatile int bulk_at;
static volatile int bulk_total;

static void home_refresh_session_rows(void);
static void home_build(void);
// The three dots a row that plays carries instead of a chevron; defined with the
// playing code below, used where the rows are built.
static lv_obj_t *row_kebab_add(lv_obj_t *row, const ytmusic_item_t *item);
static void track_from_item(const ytmusic_item_t *item, ytmusic_track_t *track);
// Fills the tracks screen from the player's own liked songs; defined with the
// Favourites row, called from the row menu and from itself.
static void favourites_refill(void);
static void favourites_refill_async(void *user);

// Whether the network is really there. Every row on this page is a request to
// YouTube, and with no IP address each one ends in a generic error that does
// not tell the user what they need to know -- so the notice goes in place of
// the rows, as on the podcast and Tidal pages.
static bool wifi_is_connected(void) {
#ifdef HOST_BUILD
	// The simulator runs on a machine with a network and no wlan0.
	return true;
#else
	if (!wifi_get_enabled()) {
		return false;
	}
	wifi_status_t status;
	wifi_get_status(&status);
	return status.state == WIFI_STATE_CONNECTED && status.ip[0] != '\0';
#endif
}

// The row under the notice: it leads where the problem is fixed.
static void open_wifi_settings_cb(lv_event_t *e) {
	(void)e;
	switch_screen(wifisettings_screen);
}

typedef enum {
	LIST_SEARCH = 0,
	LIST_BROWSE,
} list_kind_t;

// A page of rows on its way back from a worker. `to_tracks` says which of the
// two lists it fills: a card's contents go to the tracks screen, a search or a
// chart to the results screen.
typedef struct {
	list_kind_t kind;
	bool to_tracks;
	// Set when the list was asked for by a card tapped in Explore: the tracks
	// it returns are queued and started instead of being shown.
	bool autoplay;
	unsigned generation;
	ytmusic_filter_t filter;
	char query[YTMUSIC_QUERY_MAX];
	char browse_id[YTMUSIC_BROWSE_ID_MAX];
	char title[96];
	int count;
	char error[192];
	ytmusic_item_t items[YTMUSIC_PAGE_RESULTS];
} list_result_t;

typedef struct {
	unsigned generation;
	int count;
	char error[192];
	ytmusic_section_t sections[YTMUSIC_SECTIONS_MAX];
} explore_result_t;

typedef struct {
	ytmusic_track_t track;
	char path[512];
	char error[192];
	bool ok;
	// Set on the download thread the moment there is enough of the file to
	// open it: from then on the page is playing, and the completion post must
	// not play the song a second time.
	bool started;
	// Where the song sits in the queue the tap built. Playback is started on
	// the queue, not on the file: see the note above yt_queue_paths.
	int slot;
} play_result_t;

static void list_done(void *data);
static void *list_worker(void *arg);
static void explore_done(void *data);
static void item_clicked_cb(lv_event_t *e);
static void section_clicked_cb(lv_event_t *e);

static bool begin_busy(void) {
	pthread_mutex_lock(&page_lock);
	if (page_busy) {
		pthread_mutex_unlock(&page_lock);
		return false;
	}
	page_busy = true;
	pthread_mutex_unlock(&page_lock);
	return true;
}

static void end_busy(void) {
	pthread_mutex_lock(&page_lock);
	page_busy = false;
	pthread_mutex_unlock(&page_lock);
}

// A lightweight wait indicator for searches and playback startup. Downloads
// keep the separate progress card below, since that is the useful feedback
// while bytes are being saved to the card.
static lv_obj_t *busy_layer;
static lv_obj_t *busy_label;

static void busy_show(void) {
	player_pause_for_loading();
	if (!busy_layer) return;
	lv_obj_set_hidden(busy_label, false);
	lv_obj_set_hidden(busy_layer, false);
	if (!screensaver_is_visible()) {
		lv_obj_move_foreground(busy_layer);
	}
}

static void busy_hide(void) {
	if (busy_layer) lv_obj_set_hidden(busy_layer, true);
}

static void busy_build(void) {
	busy_layer = lv_obj_create(lv_layer_top());
	lv_obj_set_size(busy_layer, lv_pct(100), lv_pct(100));
	lv_obj_set_pos(busy_layer, 0, 0);
	lv_obj_set_style_bg_color(busy_layer, lv_color_black(), 0);
	lv_obj_set_style_bg_opa(busy_layer, LV_OPA_50, 0);
	lv_obj_set_style_border_width(busy_layer, 0, 0);
	lv_obj_set_style_radius(busy_layer, 0, 0);
	lv_obj_set_scrollable(busy_layer, false);
	lv_obj_set_clickable(busy_layer, true);
	lv_obj_set_hidden(busy_layer, true);
	lv_obj_set_flex_flow(busy_layer, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(busy_layer, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

	lv_obj_t *spin = spinner_create(busy_layer, &icon_loader_big);
	lv_obj_set_style_image_recolor(spin, lv_color_white(), 0);
	lv_obj_set_style_image_recolor_opa(spin, LV_OPA_COVER, 0);

	busy_label = lv_label_create(busy_layer);
	lv_label_set_text(busy_label, tr("loading"));
	lv_obj_set_style_text_color(busy_label, lv_color_white(), 0);
	lv_obj_set_style_text_font(busy_label, &font_ui_22, 0);
	lv_obj_set_style_margin_top(busy_label, 16, 0);
}

// ---------------------------------------------------------------------------
// the loading card
//
// A song starts as soon as enough of it has arrived, but "enough" still takes a
// few seconds over a public instance -- long enough that a page which says
// nothing looks like a page that did not notice the tap. The card says what is
// happening and how far along it is, and it swallows taps so a second press
// cannot start a second transfer. It comes down when the player takes over,
// which is the moment there is something to hear.
// ---------------------------------------------------------------------------

static lv_obj_t *loading_veil;
static lv_obj_t *loading_label;
static lv_obj_t *loading_bar;
static lv_timer_t *loading_timer;
// Whether the card is about a transfer somebody asked for (a download onto the
// card) or about a song starting. The first says "Downloading...", the second
// "Loading...": the same bytes are moving, but a tap on a song asked for it to
// play, not for a download.
static bool loading_transfer;

static void loading_show(void);
static void loading_hide(void);

static void loading_poll(lv_timer_t *timer) {
	(void)timer;
	int percent = -1;
	bool running = false;
	ytmusic_download_progress(&percent, &running);
	char text[96];
	int at = bulk_at;
	int total = bulk_total;
	// A queue says where it has got to; a single song has nothing to count.
	if (total > 1) {
		if (percent >= 0) {
			snprintf(text, sizeof(text), "%d / %d  %d%%", at, total, percent);
		} else {
			snprintf(text, sizeof(text), "%d / %d", at, total);
		}
	} else if (percent >= 0) {
		snprintf(text, sizeof(text), "%s  %d%%", tr(loading_transfer ? "downloading" : "loading"), percent);
	} else {
		snprintf(text, sizeof(text), "%s", tr(loading_transfer ? "downloading" : "loading"));
	}
	if (loading_label) lv_label_set_text(loading_label, text);
	if (loading_bar && percent >= 0) lv_bar_set_value(loading_bar, percent, LV_ANIM_OFF);
}

static void loading_build(void) {
	if (loading_veil) return;
	loading_veil = lv_obj_create(lv_layer_top());
	lv_obj_set_size(loading_veil, lv_pct(100), lv_pct(100));
	lv_obj_set_style_bg_color(loading_veil, lv_color_black(), 0);
	lv_obj_set_style_bg_opa(loading_veil, LV_OPA_40, 0);
	lv_obj_set_style_border_width(loading_veil, 0, 0);
	lv_obj_set_style_pad_all(loading_veil, 0, 0);
	lv_obj_add_flag(loading_veil, LV_OBJ_FLAG_CLICKABLE); // the tap stops here
	lv_obj_clear_flag(loading_veil, LV_OBJ_FLAG_SCROLLABLE);

	lv_obj_t *card = lv_obj_create(loading_veil);
	lv_obj_set_size(card, bp_pick(210, 250), LV_SIZE_CONTENT);
	lv_obj_add_style(card, &theme_style_card, 0);
	lv_obj_set_style_radius(card, bp_tile_radius(), 0);
	lv_obj_set_style_border_width(card, 0, 0);
	lv_obj_set_style_shadow_width(card, 0, 0);
	lv_obj_set_style_pad_all(card, bp_pick(12, 16), 0);
	lv_obj_set_style_pad_row(card, 10, 0);
	lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(card, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_center(card);

	spinner_create(card, &icon_loader_big);

	loading_label = lv_label_create(card);
	lv_obj_add_style(loading_label, &theme_style_text, 0);
	lv_obj_set_style_text_font(loading_label, &font_ui_20, 0);
	lv_label_set_text(loading_label, tr(loading_transfer ? "downloading" : "loading"));

	loading_bar = lv_bar_create(card);
	lv_obj_set_size(loading_bar, lv_pct(100), 6);
	lv_bar_set_range(loading_bar, 0, 100);
	lv_bar_set_value(loading_bar, 0, LV_ANIM_OFF);

	loading_timer = lv_timer_create(loading_poll, 150, NULL);
	if (loading_timer) lv_timer_pause(loading_timer);
}

static void loading_show(void) {
	player_pause_for_loading();
	loading_build();
	if (!loading_veil) return;
	lv_obj_remove_flag(loading_veil, LV_OBJ_FLAG_HIDDEN);
	if (!screensaver_is_visible()) {
		lv_obj_move_foreground(loading_veil);
	}
	if (loading_timer) lv_timer_resume(loading_timer);
	loading_poll(NULL);
}

static void loading_hide(void) {
	if (loading_timer) lv_timer_pause(loading_timer);
	if (loading_veil) lv_obj_add_flag(loading_veil, LV_OBJ_FLAG_HIDDEN);
}

// A corner button on any page of this tab: the home screen's three, and the two
// a list page shows while rows are chosen.
static lv_obj_t *page_corner_button(lv_obj_t *screen, gui_config_t *cfg, int slot,
									const lv_image_dsc_t *glyph, lv_event_cb_t cb) {
	lv_obj_t *button = lv_btn_create(screen);
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
	lv_obj_add_event_cb(button, cb, LV_EVENT_CLICKED, NULL);
	return button;
}

static lv_obj_t *ytmusic_corner_button(gui_config_t *cfg, int slot, const lv_image_dsc_t *glyph,
									   lv_event_cb_t cb) {
	return page_corner_button(ytmusic_screen, cfg, slot, glyph, cb);
}

static void open_settings_cb(lv_event_t *e) {
	(void)e;
	home_refresh_session_rows();
	switch_screen(settings_screen);
}

// ---------------------------------------------------------------------------
// the downloads page
//
// What "Download" leaves behind is the song itself, under
// <card>/Music/YouTube Music/<artist>/<album>/, with its tags and sleeve beside
// it. The Tidal page lists its own folder the same way and hands the list to
// the shared track page, so this is that shape rather than a second browser:
// a browser says "this directory does not exist" until the first download has
// made one, and then makes the user walk three folders to find a song.
// ---------------------------------------------------------------------------

#define YT_DOWNLOADS_MAX 400

typedef struct {
	char path[512];
	char title[YTMUSIC_TITLE_MAX];
	char artist[YTMUSIC_NAME_MAX];
} yt_download_entry_t;

static int yt_download_cmp(const void *a, const void *b) {
	return strcasecmp(((const yt_download_entry_t *)a)->title, ((const yt_download_entry_t *)b)->title);
}

// One key of the tags written beside a downloaded file. The downloader writes
// them next to the audio and the keep worker copies both, so a file on the card
// describes itself without being opened.
static bool yt_download_tag(const char *path, const char *key, char *out, size_t size) {
	if (!path || !key || !out || size == 0) {
		return false;
	}
	out[0] = '\0';
	char tags[800];
	if ((size_t)snprintf(tags, sizeof(tags), "%.760s.tags", path) >= sizeof(tags)) {
		return false;
	}
	FILE *file = fopen(tags, "r");
	if (!file) {
		return false;
	}
	size_t key_len = strlen(key);
	char line[512];
	bool found = false;
	while (!found && fgets(line, sizeof(line), file)) {
		if (strncmp(line, key, key_len) != 0 || line[key_len] != '=') {
			continue;
		}
		char *value = line + key_len + 1;
		char *end = value + strlen(value);
		while (end > value && (end[-1] == '\n' || end[-1] == '\r')) {
			*--end = '\0';
		}
		snprintf(out, size, "%s", value);
		found = out[0] != '\0';
	}
	fclose(file);
	return found;
}

static void yt_download_scan_dir(const char *root, yt_download_entry_t **entries, int *count, int *capacity,
								 int depth) {
	if (depth > 16 || *count >= YT_DOWNLOADS_MAX) {
		return;
	}
	DIR *dir = opendir(root);
	if (!dir) {
		return;
	}
	struct dirent *de;
	while (*count < YT_DOWNLOADS_MAX && (de = readdir(dir)) != NULL) {
		if (de->d_name[0] == '.' || strcmp(de->d_name, "..") == 0) {
			continue;
		}
		char path[768];
		int n = snprintf(path, sizeof(path), "%s/%s", root, de->d_name);
		if (n < 0 || (size_t)n >= sizeof(path)) {
			continue;
		}
		struct stat info;
		if (stat(path, &info) != 0) {
			continue;
		}
		if (S_ISDIR(info.st_mode)) {
			yt_download_scan_dir(path, entries, count, capacity, depth + 1);
			continue;
		}
		// A file still being written is not a download yet, and a folder whose
		// song is still arriving would list a title somebody cannot play.
		if (!S_ISREG(info.st_mode) || !playlist_is_playable_file(de->d_name) || growfile_is_growing(path)) {
			continue;
		}
		if (*count == *capacity) {
			int next = *capacity ? *capacity * 2 : 8;
			if (next > YT_DOWNLOADS_MAX) {
				next = YT_DOWNLOADS_MAX;
			}
			yt_download_entry_t *grown = realloc(*entries, (size_t)next * sizeof(**entries));
			if (!grown) {
				break;
			}
			*entries = grown;
			*capacity = next;
		}
		yt_download_entry_t *entry = &(*entries)[*count];
		if (snprintf(entry->path, sizeof(entry->path), "%s", path) >= (int)sizeof(entry->path)) {
			continue;
		}
		if (!yt_download_tag(path, "title", entry->title, sizeof(entry->title))) {
			snprintf(entry->title, sizeof(entry->title), "%s", de->d_name);
			char *dot = strrchr(entry->title, '.');
			if (dot) {
				*dot = '\0';
			}
		}
		yt_download_tag(path, "artist", entry->artist, sizeof(entry->artist));
		(*count)++;
	}
	closedir(dir);
}

static void open_downloads_cb(lv_event_t *e) {
	(void)e;
	const char *root = storage_sd_root();
	if (!root || !root[0]) {
		toast_error("download_no_card");
		return;
	}
	char folder[600];
	if (snprintf(folder, sizeof(folder), "%s/Music/YouTube Music", root) >= (int)sizeof(folder)) {
		return;
	}
	yt_download_entry_t *entries = NULL;
	int count = 0;
	int capacity = 0;
	yt_download_scan_dir(folder, &entries, &count, &capacity, 0);
	if (count <= 0) {
		free(entries);
		toast_error("ytmusic_no_downloads");
		return;
	}
	qsort(entries, (size_t)count, sizeof(*entries), yt_download_cmp);
	const char **paths = calloc((size_t)count, sizeof(*paths));
	const char **titles = calloc((size_t)count, sizeof(*titles));
	const char **artists = calloc((size_t)count, sizeof(*artists));
	if (!paths || !titles || !artists) {
		free(entries);
		free(paths);
		free(titles);
		free(artists);
		toast_error("out_of_memory");
		return;
	}
	for (int i = 0; i < count; i++) {
		paths[i] = entries[i].path;
		titles[i] = entries[i].title;
		artists[i] = entries[i].artist;
	}
	// medialist copies all of it and opens its own page. The downloads flavour
	// is what puts "Delete" in a row's menu: these are songs the player wrote
	// into Music/YouTube Music itself, so they are the user's to remove.
	medialist_open_downloads(tr("downloaded"), paths, titles, artists, count);
	free(entries);
	free(paths);
	free(titles);
	free(artists);
}

// The pages an account changes: the sign-out row exists only while a session is
// attached, and the import row is always there, because importing a session is
// the only way an account is ever attached.
static void home_refresh_session_rows(void) {
	if (sign_out_row) lv_obj_set_hidden(sign_out_row, !ytmusic_session_cookie_attached());
}

// ---------------------------------------------------------------------------
// rows
// ---------------------------------------------------------------------------

// A card for a playlist or album, a note for a song, the podcast glyph for a
// show: the same glyphs the Streaming page uses, so the two pages read alike.
static const lv_image_dsc_t *item_icon(const ytmusic_item_t *item) {
	if (item->kind == YTMUSIC_ITEM_TRACK) return &icon_music_note;
	if (strncmp(item->browse_id, "MPSP", 4) == 0) return &icon_podcast_list;
	return &icon_music2;
}

// ---------------------------------------------------------------------------
// row artwork
// ---------------------------------------------------------------------------
//
// A row is drawn with the picture its entry came with -- an album card's
// sleeve, a song's cover -- rather than a fixed glyph, so this tab reads like
// the Tidal one. The loader is the shared one (see qobuzart.h): twelve slots,
// the download and the decode on its own thread, and each image kept on the
// card under .local/qobuz-art named by the MD5 of its URL, so a second look at
// the same list downloads nothing.
//
// Two lists carry rows -- a search's or a chart's results, and the contents of
// a card -- and each keeps its own state, because both stay alive: coming back
// from a track finds the results list where it was left, and freeing a hidden
// list's pixels would leave its rows pointing at nothing. The twelve slots are
// shared, so they belong to the list that is on screen; leaving one hands its
// requests back, and returning asks for them again (the card's cache answers
// at once).
#define YTMUSIC_ROW_THUMB bp_pick(38, 60)
#define YTMUSIC_ROW_HEIGHT bp_pick(52, 88)
#define YTMUSIC_ROW_TITLE_FONT (bp_is_tempotec_v1() ? &font_ui_18 : &font_ui_22)
#define YTMUSIC_ROW_DETAIL_FONT (bp_is_tempotec_v1() ? &font_ui_14 : &font_ui_18)

typedef struct {
	lv_obj_t *list;        // the container the rows live in
	ytmusic_item_t *items; // what the rows were built from, in row order
	int count;
	lv_obj_t *thumbs[YTMUSIC_PAGE_RESULTS]; // the cover image of each row, NULL when the entry has none
	// The glyph each row was built with, put back while its picture is not
	// there: a row with no cover keeps the icon it would have had anyway, and
	// the same one it had before this page drew artwork at all.
	const lv_image_dsc_t *placeholders[YTMUSIC_PAGE_RESULTS];
	cover_image_t images[YTMUSIC_PAGE_RESULTS];
	bool has_image[YTMUSIC_PAGE_RESULTS];
	// A row that asked and got nothing -- a dead URL, an image that will not
	// decode, a host that does not answer -- is not asked again: a freed slot
	// restarts the pass over every row, and one broken row would otherwise keep
	// taking a slot from the rows that can be served.
	bool failed[YTMUSIC_PAGE_RESULTS];
} yt_art_list_t;

static yt_art_list_t art_results, art_tracks;
static yt_art_list_t *art_active;
// The token claiming the shared slots for this page: only its address matters,
// and it differs from the other pages'. See qobuzart.h.
static const char ytmusic_art_owner;
static int art_slot_row[QOBUZART_SLOTS]; // row of the active list in each slot, -1 = free
static lv_timer_t *art_timer;

static yt_art_list_t *art_state_for(lv_obj_t *list) {
	if (list == results_list) {
		return &art_results;
	}
	if (list == tracks_list) {
		return &art_tracks;
	}
	return NULL;
}

// Back to the glyph the row was built with. The placeholder is a themed icon,
// so the row reads the same as every other row of the page until its picture
// lands -- and again if one never does.
static void art_show_placeholder(yt_art_list_t *state, int row) {
	lv_obj_t *image = state ? state->thumbs[row] : NULL;
	if (!image) {
		return;
	}
	const lv_image_dsc_t *glyph = state->placeholders[row] ? state->placeholders[row] : &icon_music2;
	lv_image_set_src(image, glyph);
	// Back at the slot's size, not the glyph's own 128 px: the shrink the row
	// was built with (see settingsrow_add_art), set again because the picture
	// it replaces was drawn without one.
	int max_side = LV_MAX(glyph->header.w, glyph->header.h);
	if (max_side > YTMUSIC_ROW_THUMB) {
		lv_image_set_scale(image, (uint32_t)(LV_SCALE_NONE * YTMUSIC_ROW_THUMB / max_side));
	} else {
		lv_image_set_scale(image, LV_SCALE_NONE);
	}
	lv_obj_add_style(image, &theme_style_icon, 0);
	lv_obj_set_style_image_recolor_opa(image, LV_OPA_COVER, 0);
	lv_obj_set_style_radius(image, 0, 0);
	lv_obj_set_style_clip_corner(image, false, 0);
}

static void art_release_slots(void) {
	for (int slot = 0; slot < QOBUZART_SLOTS; slot++) {
		if (art_slot_row[slot] >= 0) {
			qobuzart_release(&ytmusic_art_owner, slot);
			art_slot_row[slot] = -1;
		}
	}
}

static bool art_any_pending(void) {
	for (int slot = 0; slot < QOBUZART_SLOTS; slot++) {
		if (art_slot_row[slot] >= 0) {
			return true;
		}
	}
	return false;
}

static void art_wake(void) {
	if (art_timer) {
		lv_timer_resume(art_timer);
	}
}

// The pictures of a list that is no longer on screen are given back. Its rows
// stay -- coming back finds the list where it was left -- so each one goes back
// to its glyph first and the pixels are freed with nothing pointing at them.
//
// Held only while the list showing them is up: twenty-four decoded covers are
// tens of kilobytes apiece on the card's side of the screen, and the list that
// is not being looked at has no use for them. Returning asks for them again,
// which the loader's cache on the card answers at once.
static void art_give_back_pictures(yt_art_list_t *state) {
	for (int row = 0; row < YTMUSIC_PAGE_RESULTS; row++) {
		if (state->thumbs[row] && state->has_image[row]) {
			art_show_placeholder(state, row);
		}
		state->has_image[row] = false;
		cover_free(&state->images[row]);
	}
}

static void art_request_visible(void);

// Finished covers are collected here, in the same shape as the player's own
// cover loader: the thread works, the list draws, and whatever is ready appears
// on the next pass.
static void art_poll_cb(lv_timer_t *timer) {
	if (!art_active || !art_any_pending()) {
		lv_timer_pause(timer); // nothing in flight: art_wake() resumes it
		return;
	}

	bool freed = false;
	for (int slot = 0; slot < QOBUZART_SLOTS; slot++) {
		int row = art_slot_row[slot];
		if (row < 0 || row >= YTMUSIC_PAGE_RESULTS) {
			continue;
		}
		lv_obj_t *image = art_active->thumbs[row];
		if (!image) {
			art_slot_row[slot] = -1;
			freed = true;
			continue;
		}
		cover_image_t cover;
		bool finished = false;
		if (!qobuzart_take(&ytmusic_art_owner, slot, &cover, &finished)) {
			if (finished) {
				art_active->failed[row] = true;
				art_slot_row[slot] = -1; // nothing to show: the slot goes back
				freed = true;
			}
			continue;
		}

		cover_free(&art_active->images[row]);
		art_active->images[row] = cover;
		art_active->has_image[row] = true;
		// The placeholder was recoloured by the theme; a sleeve keeps its own
		// colours, or it comes out a solid square.
		lv_obj_remove_style(image, &theme_style_icon, 0);
		lv_obj_set_style_image_recolor_opa(image, LV_OPA_TRANSP, 0);
		lv_obj_set_style_radius(image, 6, 0);
		lv_obj_set_style_clip_corner(image, true, 0);
		lv_image_set_src(image, &art_active->images[row].dsc);
		// The picture is decoded at the slot's own size, so it draws at scale
		// none: the placeholder's shrink must not shrink the sleeve with it.
		lv_image_set_scale(image, LV_SCALE_NONE);
		art_slot_row[slot] = -1;
		freed = true;
	}

	// A freed slot is handed out again at once. There are more rows than slots,
	// so a pass serves what it can and returns; without this the rows beyond the
	// twelfth would wait for the next scroll, and never be asked for at all if
	// the last scroll happened while every slot was busy.
	if (freed) {
		art_request_visible();
	}
}

// Asks for the covers of the visible rows plus a margin above and below, so a
// slow scroll finds them already there. Called on activation and on every
// scroll.
static void art_request_visible(void) {
	if (!art_active || !art_active->list) {
		return;
	}
	lv_obj_t *list = art_active->list;
	int32_t view_top = lv_obj_get_scroll_y(list);
	int32_t view_height = lv_obj_get_height(list);
	int32_t margin = view_height / 2;

	uint32_t children = lv_obj_get_child_count(list);
	for (uint32_t c = 0; c < children && c < YTMUSIC_PAGE_RESULTS; c++) {
		int row = (int)c;
		lv_obj_t *image = art_active->thumbs[row];
		if (!image || art_active->has_image[row] || art_active->failed[row]) {
			continue;
		}
		if (row >= art_active->count || !art_active->items[row].thumbnail[0]) {
			continue;
		}

		lv_obj_t *widget = lv_obj_get_child(list, row);
		int32_t y = lv_obj_get_y(widget);
		int32_t h = lv_obj_get_height(widget);
		if (y + h < view_top - margin || y > view_top + view_height + margin) {
			continue; // too far away to be worth a request
		}

		bool queued = false;
		for (int slot = 0; slot < QOBUZART_SLOTS && !queued; slot++) {
			queued = art_slot_row[slot] == row;
		}
		if (queued) {
			continue;
		}

		int free_slot = -1;
		for (int slot = 0; slot < QOBUZART_SLOTS && free_slot < 0; slot++) {
			if (art_slot_row[slot] < 0) {
				free_slot = slot;
			}
		}
		if (free_slot < 0) {
			return; // all busy: the rest on the next pass
		}

		art_slot_row[free_slot] = row;
		art_wake();
		qobuzart_request(&ytmusic_art_owner, "ytmusic-cache", free_slot, art_active->items[row].thumbnail, YTMUSIC_ROW_THUMB);
	}
}

// The list that is on screen owns the slots, and the pictures -- the one it
// replaced gives its own back.
static void art_activate(yt_art_list_t *state, lv_obj_t *list, ytmusic_item_t *items, int count) {
	if (art_active && art_active != state) {
		art_release_slots();
		art_give_back_pictures(art_active);
	}
	state->list = list;
	state->items = items;
	state->count = count;
	art_active = state;
	art_request_visible();
}

static void art_deactivate(void) {
	if (!art_active) {
		return;
	}
	art_release_slots();
	art_give_back_pictures(art_active);
	art_active = NULL;
}

// A page with no rows of its own: the covers are given back while it is up.
static void no_art_loaded_cb(lv_event_t *e) {
	(void)e;
	art_deactivate();
}

// The rows of one list are about to be rebuilt and their pictures with them.
// Called before lv_obj_clean(), which is what makes freeing the pixels safe.
static void art_forget_rows(yt_art_list_t *state) {
	if (!state) {
		return;
	}
	if (art_active == state) {
		art_release_slots();
	}
	for (int row = 0; row < YTMUSIC_PAGE_RESULTS; row++) {
		state->thumbs[row] = NULL;
		state->placeholders[row] = NULL;
		state->has_image[row] = false;
		state->failed[row] = false;
		cover_free(&state->images[row]);
	}
}

static void art_scrolled_cb(lv_event_t *e) {
	(void)e;
	art_request_visible();
}

static void results_loaded_cb(lv_event_t *e) {
	(void)e;
	art_activate(&art_results, results_list, result_items, result_count);
}

static void tracks_loaded_cb(lv_event_t *e) {
	(void)e;
	podcast_rows_refresh();
	if (podcast_order_button) {
		lv_obj_set_hidden(podcast_order_button, !items_are_podcast_episodes(track_items, track_count));
		podcast_order_paint();
	}
	art_activate(&art_tracks, tracks_list, track_items, track_count);
}

// ---------------------------------------------------------------------------
// choosing several rows
//
// The Tidal page's shape, over these rows: a long press on a song starts the
// mode, a tap then chooses or lets go of it, and the list's corner becomes the
// two things the mode can do -- queue the chosen songs onto the card, or leave.
// A selection belongs to the list it was started on; opening another list ends
// it, because row 3 of one list is not row 3 of the next.
// ---------------------------------------------------------------------------

static bool selecting;
// One flag per row of the list on screen, or NULL outside the mode.
static unsigned char *chosen;
static int chosen_count;
// The rows as they were filled, so a tick can be put on one without rebuilding
// the list -- which would lose the scroll position under the finger.
static ytmusic_item_t *sel_items;
static int sel_total;
static lv_obj_t *row_objs[YTMUSIC_PAGE_RESULTS];
static lv_obj_t *row_checks[YTMUSIC_PAGE_RESULTS];
static lv_obj_t *row_menus[YTMUSIC_PAGE_RESULTS];
static lv_obj_t *tracks_labels[YTMUSIC_PAGE_RESULTS];
static lv_obj_t *results_labels[YTMUSIC_PAGE_RESULTS];

static void selection_stop(void);
static lv_obj_t *item_row_create(lv_obj_t *list, const ytmusic_item_t *item, int index,
								 lv_event_cb_t cb, yt_art_list_t *art);

static void yt_corners_refresh(void) {
	if (sel_action_btn) lv_obj_set_hidden(sel_action_btn, !selecting);
	if (sel_close_btn) lv_obj_set_hidden(sel_close_btn, !selecting);
	if (podcast_order_button) {
		lv_obj_set_hidden(podcast_order_button,
						  selecting || !items_are_podcast_episodes(track_items, track_count));
	}
}

static void row_tick_put(int index, bool on) {
	if (index < 0 || index >= YTMUSIC_PAGE_RESULTS || !row_objs[index]) {
		return;
	}
	lv_obj_t *check = row_checks[index];
	if (!check) {
		check = lv_image_create(row_objs[index]);
		lv_obj_set_size(check, 32, 32);
		// The plain tick the file browser puts on a chosen row, at the same
		// right-hand spot the three dots occupy outside the mode.
		lv_image_set_src(check, &icon_check);
		lv_obj_add_style(check, &theme_style_icon, 0);
		lv_obj_set_style_image_recolor(check, theme()->accent, 0);
		lv_obj_set_style_image_recolor_opa(check, LV_OPA_COVER, 0);
		lv_image_set_scale(check, 156); // 36 px source -> about 22 px
		lv_obj_set_event_bubble(check, true);
		lv_obj_align(check, LV_ALIGN_RIGHT_MID, 0, 0);
		row_checks[index] = check;
	}
	lv_obj_set_hidden(check, !on);
}

static void item_row_title(const ytmusic_item_t *item, char *out, size_t size) {
	if (!item || !out || size == 0) return;
	if (item->podcast_episode && item->played) {
		snprintf(out, size, "✓ %s", item->title);
	} else {
		snprintf(out, size, "%s", item->title);
	}
}

static void podcast_rows_refresh(void) {
	for (int i = 0; i < track_count && i < YTMUSIC_PAGE_RESULTS; i++) {
		if (!track_items[i].podcast_episode) continue;
		track_items[i].played = ytpodcastplayed_get(track_items[i].id);
		if (tracks_labels[i]) {
			char title[YTMUSIC_TITLE_MAX + 8];
			item_row_title(&track_items[i], title, sizeof(title));
			lv_label_set_text(tracks_labels[i], title);
		}
	}
}

static void selection_stop(void) {
	if (!selecting) {
		return;
	}
	selecting = false;
	for (int i = 0; i < YTMUSIC_PAGE_RESULTS; i++) {
		if (row_checks[i]) {
			lv_obj_set_hidden(row_checks[i], true);
		}
		if (row_menus[i]) {
			lv_obj_set_hidden(row_menus[i], false);
		}
	}
	free(chosen);
	chosen = NULL;
	chosen_count = 0;
	yt_corners_refresh();
}

static bool selection_start(void) {
	if (selecting) {
		return true;
	}
	if (!sel_items || sel_total <= 0) {
		return false;
	}
	chosen = calloc((size_t)sel_total, 1);
	if (!chosen) {
		toast_error("ytmusic_out_of_memory");
		return false;
	}
	selecting = true;
	chosen_count = 0;
	for (int i = 0; i < sel_total; i++) {
		if (row_menus[i]) {
			lv_obj_set_hidden(row_menus[i], true);
		}
	}
	yt_corners_refresh();
	return true;
}

// Chooses the row, or lets it go. Letting go of the last one leaves the mode.
static void selection_toggle(int index) {
	if (!selecting || !chosen || index < 0 || index >= sel_total) {
		return;
	}
	chosen[index] = !chosen[index];
	if (chosen[index]) {
		chosen_count++;
	} else if (--chosen_count == 0) {
		selection_stop();
		return;
	}
	row_tick_put(index, chosen[index]);
}

static void row_long_cb(lv_event_t *e) {
	int index = (int)(intptr_t)lv_event_get_user_data(e);
	if (!selection_start()) {
		return;
	}
	selection_toggle(index);
}

// A queue of chosen songs onto the card. Each one is downloaded to the cache
// and handed to the keep worker, one at a time: the keep worker copies one file
// at a time and refuses while it is busy, so the next transfer starts when the
// last copy is done.
typedef struct {
	int count;
	int failed;
	bool ok;
	char error[192];
	ytmusic_item_t items[YTMUSIC_PAGE_RESULTS];
} bulk_result_t;

static void bulk_done_cb(void *data) {
	bulk_result_t *result = data;
	if (!result) return;
	loading_hide();
	end_busy();
	bulk_at = 0;
	bulk_total = 0;
	if (result->ok) {
		toast_success("stream_keep_done");
	} else {
		toast_error(result->error[0] ? result->error : "ytmusic_playback_failed");
	}
	free(result);
}

static void *bulk_worker(void *arg) {
	thread_be_low_priority("ytmusic bulk");
	bulk_result_t *result = arg;
	for (int i = 0; i < result->count; i++) {
		bulk_at = i + 1;
		ytmusic_track_t track;
		track_from_item(&result->items[i], &track);
		char path[512] = "";
		char error[192] = "";
		if (!ytmusic_download_for_playback(&track, path, sizeof(path), error, sizeof(error))) {
			result->failed++;
			if (!result->error[0]) {
				snprintf(result->error, sizeof(result->error), "%s",
						 error[0] ? error : "ytmusic_playback_failed");
			}
			continue;
		}
		if (!streamkeep_keep(path, false)) {
			char why[192] = "";
			result->failed++;
			if (!result->error[0]) {
				if (streamkeep_last_error(why, sizeof(why)) && why[0]) {
					snprintf(result->error, sizeof(result->error), "%s", why);
				} else {
					snprintf(result->error, sizeof(result->error), "%s", "stream_keep_failed");
				}
			}
			continue;
		}
		// The copy out of the cache is the keep worker's own thread.
		for (int tick = 0; tick < 400 && streamkeep_busy(); tick++) {
			usleep(100000);
		}
	}
	result->ok = result->failed == 0;
	if (!gui_post(bulk_done_cb, result)) {
		end_busy();
		bulk_at = 0;
		bulk_total = 0;
		free(result);
	}
	return NULL;
}

static void selection_keep_go(void) {
	if (!selecting || !chosen || chosen_count <= 0 || !sel_items) {
		return;
	}
	if (!begin_busy()) {
		return;
	}
	bulk_result_t *result = calloc(1, sizeof(*result));
	if (!result) {
		end_busy();
		toast_error("ytmusic_out_of_memory");
		return;
	}
	for (int i = 0; i < sel_total; i++) {
		if (chosen[i]) {
			result->items[result->count++] = sel_items[i];
		}
	}
	if (result->count == 0) {
		free(result);
		end_busy();
		return;
	}
	selection_stop();
	bulk_total = result->count;
	bulk_at = 0;
	loading_transfer = true;
	loading_show();
	pthread_t thread;
	if (pthread_create(&thread, NULL, bulk_worker, result) != 0) {
		loading_hide();
		end_busy();
		bulk_at = 0;
		bulk_total = 0;
		free(result);
		toast_error("ytmusic_playback_failed");
		return;
	}
	pthread_detach(thread);
}

static void selection_keep_action(void *user) {
	(void)user;
	selection_keep_go();
}

// The chosen rows hearted into the player's own list, or taken out of it when
// the list on screen is that list. The store is local, so there is no worker
// and no waiting screen -- the same act the row's own menu performs, for
// several rows at once.
static void selection_like_go(bool add) {
	if (!selecting || !chosen || chosen_count <= 0 || !sel_items) {
		return;
	}
	int changed = 0;
	for (int i = 0; i < sel_total; i++) {
		if (!chosen[i]) {
			continue;
		}
		const ytmusic_item_t *item = &sel_items[i];
		if (item->kind != YTMUSIC_ITEM_TRACK || !item->id[0]) {
			continue;
		}
		if (add) {
			ytlike_t like;
			memset(&like, 0, sizeof(like));
			snprintf(like.id, sizeof(like.id), "%s", item->id);
			snprintf(like.title, sizeof(like.title), "%s", item->title);
			snprintf(like.artist, sizeof(like.artist), "%s", item->subtitle);
			snprintf(like.album, sizeof(like.album), "%s", item->album[0] ? item->album : list_album);
			like.duration = item->duration;
			if (ytlikes_add(&like)) {
				changed++;
			}
		} else if (ytlikes_remove(item->id)) {
			changed++;
		}
	}
	selection_stop();
	if (changed > 0) {
		toast_success(add ? "added_to_favourites" : "medialist_tracks_removed");
	}
	// A row that was just unliked is gone from the favourites, and the screen
	// is that list.
	if (!add && tracks_are_likes) {
		favourites_refill();
	}
}

static void selection_like_action(void *user) { selection_like_go((bool)(intptr_t)user); }

// The corner's tick asks what to do rather than doing it, exactly as the Tidal
// pages' tick does: a selection is a deliberate act, and the sheet names the
// act before anything is kept or hearted.
static void selection_sheet_cb(lv_event_t *e) {
	lv_event_stop_bubbling(e);
	if (!selecting || !chosen || chosen_count <= 0) {
		return;
	}
	// The favourites list offers the way back out of itself; every other list
	// of songs offers the way in.
	bool on_likes = tracks_are_likes && sel_action_btn == tracks_sel_action;
	popover_item_t items[2];
	int n = 0;
	items[n++] = (popover_item_t){on_likes ? "remove_from_favourites" : "medialist_add_to_favourites",
								  selection_like_action, (void *)(intptr_t)(on_likes ? 0 : 1)};
	items[n++] = (popover_item_t){"trackmenu_keep_on_card", selection_keep_action, NULL};
	popover_show(lv_event_get_current_target(e), items, n);
}

static void selection_cancel_cb(lv_event_t *e) {
	(void)e;
	selection_stop();
}

static void fill_item_list(lv_obj_t *list, ytmusic_item_t *items, int count, lv_event_cb_t cb) {
	yt_art_list_t *art = art_state_for(list);
	lv_obj_t **labels = list == tracks_list ? tracks_labels : results_labels;
	// The flags describe rows that are about to go: entry 3 of this list is not
	// entry 3 of the next. Ended before the widgets are cleaned, so the ticks it
	// hides are still real objects.
	selection_stop();
	sel_items = (ytmusic_item_t *)items;
	sel_total = count;
	if (list == results_list) {
		sel_action_btn = results_sel_action;
		sel_close_btn = results_sel_close;
	} else if (list == tracks_list) {
		sel_action_btn = tracks_sel_action;
		sel_close_btn = tracks_sel_close;
	} else {
		sel_action_btn = NULL;
		sel_close_btn = NULL;
	}
	for (int i = 0; i < YTMUSIC_PAGE_RESULTS; i++) {
		row_objs[i] = NULL;
		row_checks[i] = NULL;
		row_menus[i] = NULL;
		labels[i] = NULL;
	}
	art_forget_rows(art);
	lv_obj_clean(list);
	if (count <= 0) {
		settingsrow_add(list, "no_results", NULL, NULL, NULL);
		return;
	}
	for (int i = 0; i < count; i++) {
		if (items[i].podcast_episode) {
			items[i].played = ytpodcastplayed_get(items[i].id);
		}
		lv_obj_t *row = item_row_create(list, &items[i], i, cb, art);
		if (i < YTMUSIC_PAGE_RESULTS) {
			row_objs[i] = row;
			lv_obj_t *texts = lv_obj_get_child(row, 1);
			labels[i] = texts ? lv_obj_get_child(texts, 0) : NULL;
		}
	}
	// A list that is already on screen -- a second search, a card opened from a
	// card -- asks for its covers here; the screen-loaded callback covers the
	// other case, where the list is about to be shown for the first time.
	if (art) {
		art->list = list;
		art->items = (ytmusic_item_t *)items;
		art->count = count;
		if (art_active == art) {
			art_request_visible();
		}
	}
}

// ---------------------------------------------------------------------------
// playing a row
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// the queue
// ---------------------------------------------------------------------------
//
// What plays after a song is the list it was tapped in, not whatever folder
// the playback cache happens to hold. A YouTube Music song has no file of its
// own until the cache writes one, but its name is known before any of it is
// downloaded -- <cache>/<id>.m4a -- so the whole list goes into the queue up
// front and each song is fetched as playback reaches it. That is the same
// arrangement Tidal uses, and it is what makes next, previous and the queue
// page stay inside YouTube Music instead of walking the card.
//
// The queue also carries names: the title, artist and album of every entry are
// written beside its cache file first (as text, no downloads), so a song that
// has not arrived yet is still a name and an artist on the player and in the
// queue page rather than eleven characters of video id.

#define YT_QUEUE_MAX YTMUSIC_QUEUE_MAX

static char yt_queue_paths[YT_QUEUE_MAX][512];
static const char *yt_queue_ptrs[YT_QUEUE_MAX];
static ytmusic_track_t yt_queue_tracks[YT_QUEUE_MAX];
static int yt_queue_count;
static int yt_queue_slot = -1;
// A transfer the queue is waiting on: the entry playback stopped at, being
// fetched. One at a time, because there is one of it to wait for.
static bool yt_fetching;

// The tapped song's list becomes the queue. Returns the slot the song sits in,
// or -1 when the list holds no playable song at all.
static int build_queue(const ytmusic_item_t *items, int count, int index) {
	yt_queue_count = 0;
	yt_queue_slot = -1;
	for (int i = 0; i < count && yt_queue_count < YT_QUEUE_MAX; i++) {
		if (items[i].kind != YTMUSIC_ITEM_TRACK || !items[i].id[0]) {
			continue;
		}
		if (!ytmusic_cache_path_for(items[i].id, yt_queue_paths[yt_queue_count],
									sizeof(yt_queue_paths[0]))) {
			continue;
		}
		if (i == index) {
			yt_queue_slot = yt_queue_count;
		}
		track_from_item(&items[i], &yt_queue_tracks[yt_queue_count]);
		ytmusic_cache_write_sidecars(&yt_queue_tracks[yt_queue_count], yt_queue_paths[yt_queue_count]);
		yt_queue_ptrs[yt_queue_count] = yt_queue_paths[yt_queue_count];
		yt_queue_count++;
	}
	return yt_queue_slot;
}

bool ytmusicpage_set_external_queue(const ytmusic_track_t *tracks, const char *const *paths, int count,
								   int start_slot) {
	if (!tracks || !paths || count <= 0 || count > YT_QUEUE_MAX || start_slot < 0 || start_slot >= count) {
		return false;
	}
	for (int i = 0; i < count; i++) {
		char id[sizeof(tracks[i].id)];
		if (!paths[i] || !ytmusic_cache_track_id(paths[i], id, sizeof(id)) ||
			strcmp(id, tracks[i].id) != 0) {
			return false;
		}
	}

	device_state_add_prepare_cb(ytmusicpage_prepare_track);
	yt_queue_count = count;
	yt_queue_slot = start_slot;
	for (int i = 0; i < count; i++) {
		yt_queue_tracks[i] = tracks[i];
		snprintf(yt_queue_paths[i], sizeof(yt_queue_paths[i]), "%s", paths[i]);
		yt_queue_ptrs[i] = yt_queue_paths[i];
	}
	return true;
}

// Hands the queue to the player at `slot`. device_state asks this page whether
// the entry is there before opening it (see ytmusicpage_prepare_track), which
// is where a song nobody has played yet starts downloading.
static void play_queued(int slot) {
	if (slot < 0 || slot >= yt_queue_count) {
		return;
	}
	device_state_play_list((const char *const *)yt_queue_ptrs, yt_queue_count, slot);
	player_refresh_now_playing();
	player_sheet_open(true);
}

// The song is on its way: the file has enough of it for the decoder to open,
// and the rest keeps arriving behind the player. Runs on the GUI thread.
static void playback_begin(void *data) {
	play_result_t *result = data;
	if (!result) return;
	busy_hide();
	end_busy();
	yt_fetching = false;
	play_queued(result->slot);
}

// Called from the download thread, once, at the byte the track can start from.
// It only queues the work: LVGL is not touched from here.
static void playback_ready(const char *path, void *user) {
	play_result_t *result = user;
	if (!result || !path) return;
	snprintf(result->path, sizeof(result->path), "%s", path);
	result->started = true;
	if (!gui_post(playback_begin, result)) {
		// Nothing to tell the interface with: fall back to the completion path,
		// which plays the song once the file is whole.
		result->started = false;
	}
}

static void playback_done(void *data) {
	play_result_t *result = data;
	if (!result) return;
	yt_fetching = false;
	if (!result->started) {
	busy_hide();
		end_busy();
		if (result->ok) {
			play_queued(result->slot);
		} else {
			toast_error(result->error[0] ? result->error : "ytmusic_playback_failed");
		}
	} else if (!result->ok) {
		// Playback began and the transfer then died: the song stops where the
		// bytes did. Saying so is better than letting it look like a short track.
		toast_error(result->error[0] ? result->error : "ytmusic_download_incomplete");
	}
	free(result);
}

static void *playback_worker(void *arg) {
	thread_be_low_priority("ytmusic play");
	play_result_t *result = arg;
	result->ok = ytmusic_stream_for_playback(&result->track, result->path, sizeof(result->path),
											 playback_ready, result,
											 result->error, sizeof(result->error));
	if (!gui_post(playback_done, result)) {
		end_busy();
		free(result);
	}
	return NULL;
}

// One queue entry, fetched on its own thread. Both callers need exactly this:
// the tap on a row, and the queue reaching a song that is not there yet.
static bool start_transfer(const ytmusic_track_t *track, int slot) {
	play_result_t *result = calloc(1, sizeof(*result));
	if (!result) {
		toast_error("ytmusic_out_of_memory");
		return false;
	}
	result->track = *track;
	result->slot = slot;
	pthread_t thread;
	if (pthread_create(&thread, NULL, playback_worker, result) != 0) {
		free(result);
		toast_error("ytmusic_playback_failed");
		return false;
	}
	pthread_detach(thread);
	return true;
}

// A song in a list was tapped: that list becomes the queue, and the song is
// fetched if the cache does not already hold it whole.
static void play_item_at(const ytmusic_item_t *items, int count, int index) {
	if (!items || index < 0 || index >= count) return;
	const ytmusic_item_t *item = &items[index];
	if (item->kind != YTMUSIC_ITEM_TRACK || !item->id[0]) return;
	int slot = build_queue(items, count, index);
	if (slot < 0) return;
	// A song played a moment ago is still in the cache, whole. Playing it again
	// is a local file: no transfer, and no card about one -- which is what a
	// prompt on the second play of the same song was.
	if (ytmusic_cache_whole(yt_queue_paths[slot])) {
		play_queued(slot);
		return;
	}
	if (!begin_busy()) return;
	player_pause_for_loading();
	yt_fetching = true;
	busy_show();
	if (!start_transfer(&yt_queue_tracks[slot], slot)) {
		yt_fetching = false;
		loading_hide();
		end_busy();
	}
	// Nothing else is said here: loading_show() is already the card with the
	// spinner, the percentage and the bar. The "downloading" popup that used to
	// be raised on top of it said the same thing twice, and the older of the two
	// covered the progress it was announcing.
}

// Which list a row belongs to. The row menu carries the item's own address and
// playing needs the list around it, because the queue is the list.
static void play_item(const ytmusic_item_t *item) {
	if (!item) return;
	for (int i = 0; i < result_count; i++) {
		if (&result_items[i] == item) {
			play_item_at(result_items, result_count, i);
			return;
		}
	}
	for (int i = 0; i < track_count; i++) {
		if (&track_items[i] == item) {
			play_item_at(track_items, track_count, i);
			return;
		}
	}
}

// device_state is about to open a queue entry: "is it there yet?".
//
// A song nobody has played has no cache file, only a name in the queue. The
// answer is "not yet", the previous track is stopped and the missing one is
// fetched; when enough of it has arrived the queue is started again at that
// entry, which this time answers "yes, arriving". That is what lets next and
// previous walk a whole album without every song having been played first.
bool ytmusicpage_prepare_track(char *path, size_t path_size) {
	(void)path_size;
	char id[16];
	if (!path || !ytmusic_cache_track_id(path, id, sizeof(id))) {
		return true; // not one of this page's files
	}
	if (ytmusic_cache_whole(path) || growfile_is_growing(path)) {
		return true; // here, or well on its way
	}
	if (yt_fetching) {
		return false; // one is already on its way, and it is this one
	}
	for (int i = 0; i < yt_queue_count; i++) {
		if (strcmp(yt_queue_paths[i], path) != 0) {
			continue;
		}
		// Stopped first, and not out of politeness: the entry before this one is
		// reading a growing file, and taking that file out from under the audio
		// thread makes it report "finished" -- the queue would then advance
		// onto another missing song and start over.
		printf("ytmusic: %.11s: queued but not downloaded; fetching it now\n", id);
		device_state_stop();
		yt_fetching = true;
		if (!start_transfer(&yt_queue_tracks[i], i)) {
			yt_fetching = false;
		}
		return false;
	}
	return true; // left over from another list: the player may try it
}

// ---------------------------------------------------------------------------
// the row menu
//
// A song's row taps straight into playing it, so there is no page to open and
// no chevron to open it with. What a tap cannot do lives behind the three dots
// instead, in a popover anchored to the row: the Tidal page's shape.
// ---------------------------------------------------------------------------

static void track_from_item(const ytmusic_item_t *item, ytmusic_track_t *track) {
	memset(track, 0, sizeof(*track));
	snprintf(track->id, sizeof(track->id), "%s", item->id);
	snprintf(track->title, sizeof(track->title), "%s", item->title);
	// A row's subtitle is the artist, or the show an episode belongs to.
	snprintf(track->artist, sizeof(track->artist), "%s", item->subtitle[0] ? item->subtitle : list_artist);
	// The album the row named, or the list it was opened from: that is what
	// makes the download land in a folder with a name instead of "Unknown
	// album". The tag file written beside the audio carries it, and the keep
	// worker reads the folder from the tag.
	snprintf(track->album, sizeof(track->album), "%s", item->album[0] ? item->album : list_album);
	// The row's cover goes with the track: the downloader fetches it beside the
	// audio, which is where the player looks for a file's artwork.
	snprintf(track->thumbnail, sizeof(track->thumbnail), "%s", item->thumbnail[0] ? item->thumbnail : list_thumbnail);
	track->duration = item->duration;
	track->podcast_episode = item->podcast_episode;
}

// Downloading without playing. The same cache file is made, and then handed to
// the keep worker instead of to the player, so it ends up in
// <card>/Music/YouTube Music/ with its tags and sleeve.
typedef struct {
	ytmusic_track_t track;
	char path[512];
	char error[192];
	bool ok;
} save_result_t;

static void save_done(void *data) {
	save_result_t *result = data;
	if (!result) return;
	loading_hide();
	end_busy();
	if (!result->ok) toast_error(result->error[0] ? result->error : "ytmusic_playback_failed");
	free(result);
}

static void *save_worker(void *arg) {
	thread_be_low_priority("ytmusic save");
	save_result_t *result = arg;
	result->ok = ytmusic_download_for_playback(&result->track, result->path, sizeof(result->path),
											   result->error, sizeof(result->error));

	if (result->ok) {
		for (int tick = 0; tick < 50 && streamkeep_busy(); tick++) {
			usleep(100 * 1000);
		}
		if (!streamkeep_keep(result->path, true)) {
			// No card, or one the computer is using: the keep worker records why
			// without a toast of its own, so the reason is read back here.
			char why[192] = "";
			result->ok = false;
			if (streamkeep_last_error(why, sizeof(why)) && why[0]) {
				snprintf(result->error, sizeof(result->error), "%s", why);
			}
		}
	}
	if (!gui_post(save_done, result)) {
		end_busy();
		free(result);
	}
	return NULL;
}

static void row_menu_save(void *user) {
	const ytmusic_item_t *item = user;
	if (!item || item->kind != YTMUSIC_ITEM_TRACK || !item->id[0]) return;
	if (!begin_busy()) return;
	save_result_t *result = calloc(1, sizeof(*result));
	if (!result) {
		end_busy();
		toast_error("ytmusic_out_of_memory");
		return;
	}
	track_from_item(item, &result->track);
	loading_transfer = true;
	loading_show();
	pthread_t thread;
	if (pthread_create(&thread, NULL, save_worker, result) != 0) {
		loading_hide();
		end_busy();
		free(result);
		toast_error("ytmusic_playback_failed");
		return;
	}
	pthread_detach(thread);
	// The loading card is the only progress there is; see play_item().
}

static void row_menu_play(void *user) {
	play_item((const ytmusic_item_t *)user);
}

static void row_menu_mark_played(void *user) {
	ytmusic_item_t *item = user;
	if (!item || !item->podcast_episode || !item->id[0]) return;
	bool played = !ytpodcastplayed_get(item->id);
	if (!ytpodcastplayed_set(item->id, played)) {
		toast_error("podcast_save_failed");
		return;
	}
	item->played = played;
	for (int i = 0; i < track_count && i < YTMUSIC_PAGE_RESULTS; i++) {
		if (&track_items[i] == item && tracks_labels[i]) {
			char title[YTMUSIC_TITLE_MAX + 8];
			item_row_title(item, title, sizeof(title));
			lv_label_set_text(tracks_labels[i], title);
			return;
		}
	}
	for (int i = 0; i < result_count && i < YTMUSIC_PAGE_RESULTS; i++) {
		if (&result_items[i] == item && results_labels[i]) {
			char title[YTMUSIC_TITLE_MAX + 8];
			item_row_title(item, title, sizeof(title));
			lv_label_set_text(results_labels[i], title);
			return;
		}
	}
}

// One row's heart, from the row itself rather than from the player: a song can
// be liked without being played, which is what a list is for. The like is the
// player's own record -- id, title, artist -- so it survives the cache being
// swept and needs no account.
static void row_menu_like(void *user) {
	const ytmusic_item_t *item = user;
	if (!item || item->kind != YTMUSIC_ITEM_TRACK || !item->id[0]) return;
	if (ytlikes_contains(item->id)) {
		ytlikes_remove(item->id);
	} else {
		ytlike_t like;
		memset(&like, 0, sizeof(like));
		snprintf(like.id, sizeof(like.id), "%s", item->id);
		snprintf(like.title, sizeof(like.title), "%s", item->title);
		snprintf(like.artist, sizeof(like.artist), "%s", item->subtitle);
		snprintf(like.album, sizeof(like.album), "%s", item->album[0] ? item->album : list_album);
		like.duration = item->duration;
		if (ytlikes_add(&like)) toast_success("added_to_favourites");
	}
	// The favourites list is the one screen that shows what just changed, and
	// rebuilding it deletes the very row this menu was opened on: the work is
	// queued so it runs once the event has returned.
	if (tracks_are_likes) gui_post(favourites_refill_async, NULL);
}

static void row_menu_cb(lv_event_t *e) {
	const ytmusic_item_t *item = lv_event_get_user_data(e);
	if (!item) return;
	popover_item_t items[4];
	int count = 0;
	if (item->podcast_episode) {
		items[count++] = (popover_item_t){
			item->played ? "podcast_mark_not_listened" : "podcast_mark_listened",
			row_menu_mark_played, (void *)item, false};
	}
	items[count++] = (popover_item_t){"ytmusic_play", row_menu_play, (void *)item, false};
	items[count++] = (popover_item_t){"trackmenu_keep_on_card", row_menu_save, (void *)item, false};
	if (item->kind == YTMUSIC_ITEM_TRACK && item->id[0]) {
		bool liked = ytlikes_contains(item->id);
		items[count++] = (popover_item_t){liked ? "remove_from_favourites" : "medialist_add_to_favourites",
										  row_menu_like, (void *)item, false};
	}
	popover_show(lv_event_get_target(e), items, count);
}

static lv_obj_t *row_kebab_add(lv_obj_t *row, const ytmusic_item_t *item) {
	lv_obj_t *button = lv_btn_create(row);
	lv_obj_set_size(button, bp_pick(26, 34), bp_pick(28, 34));
	lv_obj_set_style_bg_opa(button, LV_OPA_TRANSP, 0);
	lv_obj_set_style_border_width(button, 0, 0);
	lv_obj_set_style_shadow_width(button, 0, 0);
	lv_obj_set_style_pad_all(button, 0, 0);
	// A target two dots wide is a narrow one: a tap that lands just outside it
	// used to be a tap on the row, which started the song instead of the menu.
	lv_obj_set_ext_click_area(button, bp_pick(7, 10));
	lv_obj_add_event_cb(button, row_menu_cb, LV_EVENT_CLICKED, (void *)item);
	lv_obj_t *icon = lv_image_create(button);
	lv_image_set_src(icon, &icon_ellipsis_vertical);
	lv_obj_add_style(icon, &theme_style_icon, 0);
	// 22 px of glyph in the button: the source is 36, and unscaled it drew past
	// the button's own edges.
	lv_image_set_scale(icon, (uint32_t)(LV_SCALE_NONE * 22 / 36));
	lv_obj_center(icon);
	return button;
}

static lv_obj_t *item_row_create(lv_obj_t *list, const ytmusic_item_t *item, int index,
								 lv_event_cb_t cb, yt_art_list_t *art) {
	lv_obj_t *row = lv_btn_create(list);
	lv_obj_set_width(row, lv_pct(100));
	lv_obj_set_height(row, YTMUSIC_ROW_HEIGHT);
	lv_obj_add_style(row, &theme_style_card, 0);
	lv_obj_add_style(row, &theme_style_card_pressed, LV_STATE_PRESSED);
	lv_obj_set_style_radius(row, bp_pick(10, 12), 0);
	lv_obj_set_style_border_width(row, 0, 0);
	lv_obj_set_style_shadow_width(row, 0, 0);
	lv_obj_set_style_pad_hor(row, bp_pick(5, 16), 0);
	lv_obj_set_style_pad_ver(row, 0, 0);
	lv_obj_set_style_pad_column(row, bp_pick(6, 8), 0);
	lv_obj_set_scrollable(row, false);
	lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_event_bubble(row, true);
	lv_obj_add_event_cb(row, cb, LV_EVENT_CLICKED, (void *)(intptr_t)index);

	const lv_image_dsc_t *placeholder = item_icon(item);
	lv_obj_t *thumb = lv_image_create(row);
	lv_obj_set_size(thumb, YTMUSIC_ROW_THUMB, YTMUSIC_ROW_THUMB);
	lv_image_set_inner_align(thumb, LV_IMAGE_ALIGN_CENTER);
	lv_image_set_src(thumb, placeholder);
	lv_obj_add_style(thumb, &theme_style_icon, 0);
	lv_obj_set_style_image_recolor_opa(thumb, LV_OPA_COVER, 0);
	lv_obj_set_style_radius(thumb, 0, 0);
	lv_obj_set_style_clip_corner(thumb, false, 0);
	lv_obj_set_clickable(thumb, false);
	lv_obj_set_scrollable(thumb, false);
	lv_obj_set_event_bubble(thumb, true);
	int max_side = LV_MAX(placeholder->header.w, placeholder->header.h);
	if (max_side > YTMUSIC_ROW_THUMB) {
		lv_image_set_scale(thumb, (uint32_t)(LV_SCALE_NONE * YTMUSIC_ROW_THUMB / max_side));
	}

	lv_obj_t *texts = lv_obj_create(row);
	lv_obj_set_flex_grow(texts, 1);
	lv_obj_set_height(texts, LV_SIZE_CONTENT);
	lv_obj_set_style_bg_opa(texts, 0, 0);
	lv_obj_set_style_border_width(texts, 0, 0);
	lv_obj_set_style_pad_all(texts, 0, 0);
	lv_obj_set_style_pad_row(texts, bp_pick(2, 6), 0);
	lv_obj_set_scrollable(texts, false);
	lv_obj_set_clickable(texts, false);
	lv_obj_set_event_bubble(texts, true);
	lv_obj_set_flex_flow(texts, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(texts, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

	lv_obj_t *title = lv_label_create(texts);
	char title_text[YTMUSIC_TITLE_MAX + 8];
	item_row_title(item, title_text, sizeof(title_text));
	lv_label_set_text(title, title_text);
	lv_label_set_long_mode(title, LV_LABEL_LONG_DOT);
	lv_obj_set_width(title, lv_pct(100));
	lv_obj_set_height(title, lv_font_get_line_height(YTMUSIC_ROW_TITLE_FONT));
	lv_obj_add_style(title, &theme_style_text, 0);
	lv_obj_set_style_text_font(title, YTMUSIC_ROW_TITLE_FONT, 0);
	lv_obj_set_clickable(title, false);

	if (item->subtitle[0]) {
		lv_obj_t *detail = lv_label_create(texts);
		lv_label_set_text(detail, item->subtitle);
		lv_label_set_long_mode(detail, LV_LABEL_LONG_DOT);
		lv_obj_set_width(detail, lv_pct(100));
		lv_obj_set_height(detail, lv_font_get_line_height(YTMUSIC_ROW_DETAIL_FONT));
		lv_obj_add_style(detail, &theme_style_text_dim, 0);
		lv_obj_set_style_text_font(detail, YTMUSIC_ROW_DETAIL_FONT, 0);
	lv_obj_set_clickable(detail, false);
	}

	if (item->kind == YTMUSIC_ITEM_TRACK) {
		if (index < YTMUSIC_PAGE_RESULTS) {
			row_menus[index] = row_kebab_add(row, item);
		}
		lv_obj_add_event_cb(row, row_long_cb, LV_EVENT_LONG_PRESSED, (void *)(intptr_t)index);
	} else {
		lv_obj_t *chevron = lv_image_create(row);
		lv_image_set_src(chevron, &icon_chevron_right);
		lv_obj_add_style(chevron, &theme_style_icon, 0);
		lv_obj_set_style_image_opa(chevron, LV_OPA_60, 0);
		lv_image_set_scale(chevron, (uint32_t)(LV_SCALE_NONE * 22 / 36));
		lv_obj_set_clickable(chevron, false);
		lv_obj_set_event_bubble(chevron, true);
	}

	if (art && index < YTMUSIC_PAGE_RESULTS) {
		art->placeholders[index] = placeholder;
		art->thumbs[index] = thumb;
	}
	return row;
}

static bool start_list_request(list_kind_t kind, bool to_tracks, ytmusic_filter_t filter,
							   const char *query, const char *browse_id, const char *title,
							   bool autoplay) {
	// A search, the charts, the liked songs or Explore starts a new context: the
	// album folder of a downloaded song comes from the list it was opened in, and
	// a card's title is only meaningful for that card's own tracks.
	if (kind == LIST_SEARCH || !browse_id || strcmp(browse_id, "FEmusic_charts") == 0 ||
		strcmp(browse_id, "VLLM") == 0 || strncmp(browse_id, "FEmusic_", 8) == 0) {
		list_album[0] = '\0';
		list_show[0] = '\0';
		list_artist[0] = '\0';
		list_thumbnail[0] = '\0';
	}
	if (!begin_busy()) return false;
	list_result_t *result = calloc(1, sizeof(*result));
	if (!result) {
		end_busy();
		toast_error("ytmusic_out_of_memory");
		return false;
	}
	result->kind = kind;
	result->to_tracks = to_tracks;
	result->autoplay = autoplay;
	result->filter = filter;
	if (query) snprintf(result->query, sizeof(result->query), "%s", query);
	if (browse_id) snprintf(result->browse_id, sizeof(result->browse_id), "%s", browse_id);
	snprintf(result->title, sizeof(result->title), "%s", title ? title : "");
	pthread_mutex_lock(&page_lock);
	result->generation = ++list_generation;
	pthread_mutex_unlock(&page_lock);

	pthread_t thread;
	if (pthread_create(&thread, NULL, list_worker, result) != 0) {
		end_busy();
		free(result);
		toast_error(kind == LIST_SEARCH ? "ytmusic_search_failed" : "ytmusic_browse_failed");
		return false;
	}
	pthread_detach(thread);
	busy_show();
	return true;
}

static void *list_worker(void *arg) {
	thread_be_low_priority("ytmusic list");
	list_result_t *result = arg;
	if (result->kind == LIST_SEARCH) {
		result->count = ytmusic_search_items(result->query, result->filter, result->items,
											 YTMUSIC_PAGE_RESULTS, result->error, sizeof(result->error));
	} else {
		result->count = ytmusic_browse_items(result->browse_id, result->items,
											 YTMUSIC_PAGE_RESULTS, result->error, sizeof(result->error));
	}
	if (!gui_post(list_done, result)) {
		end_busy();
		free(result);
	}
	return NULL;
}

static void list_done(void *data) {
	list_result_t *result = data;
	if (!result) return;
	busy_hide();
	end_busy();
	pthread_mutex_lock(&page_lock);
	bool current = result->generation == list_generation;
	pthread_mutex_unlock(&page_lock);
	if (!current) {
		free(result);
		return;
	}
	if (result->count < 0) {
		toast_error(result->error[0] ? result->error : "ytmusic_search_failed");
		free(result);
		return;
	}
	int count = result->count;
	if (count > YTMUSIC_PAGE_RESULTS) count = YTMUSIC_PAGE_RESULTS;
	for (int i = 0; i < count; i++) {
		if (result->items[i].kind == YTMUSIC_ITEM_TRACK) {
			if (!result->items[i].subtitle[0] && list_artist[0]) {
				snprintf(result->items[i].subtitle, sizeof(result->items[i].subtitle), "%s", list_artist);
			}
			if (!result->items[i].album[0] && list_album[0]) {
				snprintf(result->items[i].album, sizeof(result->items[i].album), "%s", list_album);
			}
			if (!result->items[i].thumbnail[0] && list_thumbnail[0]) {
				snprintf(result->items[i].thumbnail, sizeof(result->items[i].thumbnail), "%s", list_thumbnail);
			}
		}
	}
	// A card tapped in Explore: the tracks it was hiding are the point of the
	// tap, not the page they would fill. They are queued straight from the
	// fetched list -- play_item_at copies everything it keeps -- so the folder
	// is never drawn and the carousel stays where the tap was.
	if (result->autoplay && count > 0 && result->items[0].kind == YTMUSIC_ITEM_TRACK) {
		play_item_at(result->items, count, 0);
		free(result);
		return;
	}
	if (result->to_tracks) {
		// Whatever a card opened, the tracks screen is a page of the store
		// again: only the Favourites row sets this.
		tracks_are_likes = false;
		track_count = count;
		memcpy(track_items, result->items, sizeof(track_items[0]) * (size_t)count);
		bool is_podcast_container = (strncmp(result->browse_id, "MPSP", 4) == 0) ||
		                            (result->filter == YTMUSIC_FILTER_PODCASTS);
		if (is_podcast_container) {
			for (int i = 0; i < track_count; i++) {
				if (track_items[i].kind == YTMUSIC_ITEM_TRACK) {
					track_items[i].podcast_episode = true;
				}
			}
		}
		// Episodes of the show just opened say whose they are. The subtitle a
		// show page puts on its rows is a date, and the name of the show is
		// worth more than that: it is what the listener recognises the show by,
		// and the name the download carries into the player.
		if (list_show[0]) {
			for (int i = 0; i < track_count; i++) {
				if (track_items[i].podcast_episode) {
					snprintf(track_items[i].subtitle, sizeof(track_items[0].subtitle), "%s", list_show);
				}
			}
		}
		if (!podcast_newest_first() && items_are_podcast_episodes(track_items, track_count)) {
			for (int i = 0, j = track_count - 1; i < j; i++, j--) {
				ytmusic_item_t tmp = track_items[i];
				track_items[i] = track_items[j];
				track_items[j] = tmp;
			}
		}
		lv_label_set_text(tracks_title, tr(result->title[0] ? result->title : "ytmusic_browse"));
		fill_item_list(tracks_list, track_items, track_count, item_clicked_cb);
		if (podcast_order_button) {
			lv_obj_set_hidden(podcast_order_button, !items_are_podcast_episodes(track_items, track_count));
			podcast_order_paint();
		}
		switch_screen(tracks_screen);
	} else {
		explore_cards = false;
		result_count = count;
		memcpy(result_items, result->items, sizeof(result_items[0]) * (size_t)count);
		lv_label_set_text(results_title, tr(result->title[0] ? result->title : "ytmusic_search_results"));
		fill_item_list(results_list, result_items, result_count, item_clicked_cb);
		switch_screen(results_screen);
	}
	free(result);
	// A page of results is a few hundred kilobytes of JSON read and thrown away
	// on the worker thread. Its arena keeps the high-water mark otherwise, and
	// an afternoon of searching is a few megabytes that never go back.
	(void)malloc_trim(0);
}

// A tap in either list: a song plays, a card opens its own page in the tracks
// list. Both lists keep their rows in an array the handler indexes.
static void item_clicked_cb(lv_event_t *e) {
	if (player_sheet_drag_active() || switcher_back_drag_active()) {
		return;
	}
	int index = (int)(intptr_t)lv_event_get_user_data(e);
	lv_obj_t *target_row = lv_event_get_current_target(e);
	const ytmusic_item_t *item = NULL;
	// The list a row was drawn in, kept so that playing it can queue the rest
	// of that same list: a tap is "play this and what follows", not "play one
	// file and fall back to whatever was queued before".
	const ytmusic_item_t *list_items = NULL;
	int list_count = 0;
	if (lv_obj_get_parent(target_row) == results_list) {
		if (index < 0 || index >= result_count) return;
		item = &result_items[index];
		list_items = result_items;
		list_count = result_count;
	} else {
		if (index < 0 || index >= track_count) return;
		item = &track_items[index];
		list_items = track_items;
		list_count = track_count;
	}
	if (selecting) {
		// A tap chooses in this mode. A card is not a song, and has nothing to
		// download -- it is not offered a tick either.
		if (item->kind == YTMUSIC_ITEM_TRACK) {
			selection_toggle(index);
		}
		return;
	}
	if (item->kind == YTMUSIC_ITEM_CONTAINER) {
		// Everything opened from here belongs to this list: an album's own
		// track list does not name the album on its rows.
		snprintf(list_album, sizeof(list_album), "%s", item->title);
		snprintf(list_show, sizeof(list_show), "%s", item->title);
		snprintf(list_artist, sizeof(list_artist), "%s", item->subtitle);
		snprintf(list_thumbnail, sizeof(list_thumbnail), "%s", item->thumbnail);
		bool from_explore = lv_obj_get_parent(target_row) == results_list && explore_cards;
		start_list_request(LIST_BROWSE, true, YTMUSIC_FILTER_SONGS, NULL, item->browse_id, item->title,
						   from_explore);
	} else {
		play_item_at(list_items, list_count, index);
	}
}

// ---------------------------------------------------------------------------
// explore
// ---------------------------------------------------------------------------

static void *explore_worker(void *arg) {
	thread_be_low_priority("ytmusic explore");
	explore_result_t *result = arg;
	result->count = ytmusic_explore_sections(result->sections, YTMUSIC_SECTIONS_MAX,
											  result->error, sizeof(result->error));
	if (!gui_post(explore_done, result)) {
		end_busy();
		free(result);
	}
	return NULL;
}

static void explore_done(void *data) {
	explore_result_t *result = data;
	if (!result) return;
	busy_hide();
	end_busy();
	pthread_mutex_lock(&page_lock);
	bool current = result->generation == list_generation;
	pthread_mutex_unlock(&page_lock);
	if (!current) {
		free(result);
		return;
	}
	if (result->count <= 0) {
		toast_error(result->error[0] ? result->error : "ytmusic_explore_empty");
		free(result);
		return;
	}
	explore_section_count = result->count;
	memcpy(explore_sections, result->sections, sizeof(explore_sections[0]) * (size_t)explore_section_count);
	lv_obj_clean(explore_list);
	for (int i = 0; i < explore_section_count; i++) {
		lv_obj_t *value = NULL;
		lv_obj_t *row = settingsrow_add(explore_list,
			explore_sections[i].title[0] ? explore_sections[i].title : "ytmusic_explore",
			&value, section_clicked_cb, (void *)(intptr_t)i);
		settingsrow_name_lines(row, 2);
		settingsrow_add_icon(row, &icon_music2, THEME_SEMANTIC_PURPLE);
		if (value) {
			char text[16];
			snprintf(text, sizeof(text), "%d", explore_sections[i].count);
			lv_label_set_text(value, text);
		}
	}
	switch_screen(explore_screen);
	free(result);
	(void)malloc_trim(0); // the carousels' JSON, for the reason above
}

static void explore_start_cb(lv_event_t *e) {
	(void)e;
	if (!begin_busy()) return;
	explore_result_t *result = calloc(1, sizeof(*result));
	if (!result) {
		end_busy();
		toast_error("ytmusic_out_of_memory");
		return;
	}
	pthread_mutex_lock(&page_lock);
	result->generation = ++list_generation;
	pthread_mutex_unlock(&page_lock);
	pthread_t thread;
	if (pthread_create(&thread, NULL, explore_worker, result) != 0) {
		end_busy();
		free(result);
		toast_error("ytmusic_explore_failed");
		return;
	}
	pthread_detach(thread);
	busy_show();
}

// One carousel, listed with the cards it holds. Its items came with the page,
// so this opens without another request.
static void section_clicked_cb(lv_event_t *e) {
	int index = (int)(intptr_t)lv_event_get_user_data(e);
	if (index < 0 || index >= explore_section_count) return;
	const ytmusic_section_t *section = &explore_sections[index];
	tracks_are_likes = false;
	explore_cards = true;
	result_count = section->count;
	memcpy(result_items, section->items, sizeof(result_items[0]) * (size_t)result_count);
	lv_label_set_text(results_title, tr(section->title[0] ? section->title : "ytmusic_explore"));
	fill_item_list(results_list, result_items, result_count, item_clicked_cb);
	switch_screen(results_screen);
}

// ---------------------------------------------------------------------------
// home
// ---------------------------------------------------------------------------

static void search_keyboard_show(bool visible);

// ---------------------------------------------------------------------------
// search history
//
// The last few queries, newest first, in the config the way Tidal keeps its
// own: a history is for picking up yesterday's search, not for keeping a log.
// It takes the keyboard's place whenever nothing is being typed, so the page
// opens on something to tap rather than on an empty field.
// ---------------------------------------------------------------------------
static lv_obj_t *search_history_list;
static lv_obj_t *search_history_empty;

#define SEARCH_HISTORY_MAX 8
#define SEARCH_HISTORY_LEN 96

static char history_terms[SEARCH_HISTORY_MAX][SEARCH_HISTORY_LEN];
static int history_count;
static bool history_loaded;

static void search_history_rebuild(void);

// Rebuilding deletes the rows, and one of them may be the very widget whose
// tap asked for the rebuild: the work is queued so it runs once the event has
// returned.
static void search_history_rebuild_async(void *user) {
	(void)user;
	search_history_rebuild();
}

static void history_load(void) {
	if (history_loaded) {
		return;
	}
	history_loaded = true;
	history_count = 0;
	int stored = (int)config_get_int("ytmusic", "search_history_count", 0);
	if (stored < 0) {
		stored = 0;
	}
	if (stored > SEARCH_HISTORY_MAX) {
		stored = SEARCH_HISTORY_MAX;
	}
	for (int i = 0; i < stored; i++) {
		char key[24];
		snprintf(key, sizeof(key), "search_history_%d", i);
		const char *term = config_get("ytmusic", key, "");
		if (term && term[0]) {
			snprintf(history_terms[history_count], SEARCH_HISTORY_LEN, "%s", term);
			history_count++;
		}
	}
}

static void history_save(void) {
	config_set_int("ytmusic", "search_history_count", history_count);
	// Past the end the keys are emptied rather than left where they were: a
	// cleared history that a later read finds again is worse than none.
	for (int i = 0; i < SEARCH_HISTORY_MAX; i++) {
		char key[24];
		snprintf(key, sizeof(key), "search_history_%d", i);
		config_set("ytmusic", key, i < history_count ? history_terms[i] : "");
	}
	config_save();
}

// Puts the term on top, once: a search already in the list moves up instead of
// filling the list with the same word repeated.
static void history_remember(const char *term) {
	history_load();
	while (term && *term == ' ') {
		term++;
	}
	if (!term || !*term) {
		return;
	}
	char kept[SEARCH_HISTORY_MAX][SEARCH_HISTORY_LEN];
	int kept_count = 0;
	for (int i = 0; i < history_count && kept_count < SEARCH_HISTORY_MAX - 1; i++) {
		if (strcmp(history_terms[i], term) == 0) {
			continue;
		}
		snprintf(kept[kept_count], SEARCH_HISTORY_LEN, "%s", history_terms[i]);
		kept_count++;
	}
	snprintf(history_terms[0], SEARCH_HISTORY_LEN, "%s", term);
	for (int i = 0; i < kept_count; i++) {
		snprintf(history_terms[i + 1], SEARCH_HISTORY_LEN, "%s", kept[i]);
	}
	history_count = kept_count + 1;
	history_save();
}

static void paint_search_pills(void) {
	for (int i = 0; i < 3; i++) {
		if (!search_pills[i]) continue;
		bool on = i == search_kind_index;
		lv_obj_set_style_bg_color(search_pills[i], on ? theme()->accent : theme()->surface_pressed, 0);
		lv_obj_set_style_text_color(lv_obj_get_child(search_pills[i], 0), on ? theme_on_accent() : theme()->text_primary, 0);
	}
}

static void search_pill_cb(lv_event_t *e) {
	if (switcher_back_drag_active() || player_sheet_drag_active()) return;
	search_kind_index = (int)(intptr_t)lv_event_get_user_data(e);
	if (search_filter != YTMUSIC_FILTER_PODCASTS) {
		search_filter = search_kind_index == 1 ? YTMUSIC_FILTER_ALBUMS : (search_kind_index == 2 ? YTMUSIC_FILTER_ARTISTS : YTMUSIC_FILTER_SONGS);
	}
	paint_search_pills();
}

// Runs what is in the field, and remembers it.
static void search_run(void) {
	if (search_filter != YTMUSIC_FILTER_PODCASTS) {
		search_filter = search_kind_index == 1 ? YTMUSIC_FILTER_ALBUMS : (search_kind_index == 2 ? YTMUSIC_FILTER_ARTISTS : YTMUSIC_FILTER_SONGS);
	}
	const char *text = lv_textarea_get_text(search_field);
	if (!text || !text[0]) {
		return;
	}
	history_remember(text);
	char query[YTMUSIC_QUERY_MAX];
	snprintf(query, sizeof(query), "%s", text);
	// The results take the page: a keyboard left up covers a list nobody asked
	// for, and hides what came back.
	search_keyboard_show(false);
	start_list_request(LIST_SEARCH, false, search_filter, query, NULL, "ytmusic_search_results", false);
}

// The keyboard and the history cannot both have the space under the field --
// one replaces the other, as on the Tidal page.
static void search_keyboard_show(bool visible) {
	if (!search_keyboard) {
		return;
	}
	keyboard_set_visible(search_keyboard, visible);
	if (visible) {
		if (search_history_list) {
			lv_obj_set_hidden(search_history_list, true);
		}
		if (search_history_empty) {
			lv_obj_set_hidden(search_history_empty, true);
		}
	} else {
		search_history_rebuild();
	}
}

// One row of the history: the query, and an x to forget just that one.
static void history_forget_cb(lv_event_t *e) {
	lv_event_stop_bubbling(e);
	if (switcher_back_drag_active() || player_sheet_drag_active()) {
		return;
	}
	int index = (int)(intptr_t)lv_event_get_user_data(e);
	if (index < 0 || index >= history_count) {
		return;
	}
	for (int i = index; i + 1 < history_count; i++) {
		snprintf(history_terms[i], SEARCH_HISTORY_LEN, "%s", history_terms[i + 1]);
	}
	history_count--;
	history_save();
	gui_post(search_history_rebuild_async, NULL);
}

// Tapping a query runs it again, exactly as typing it would have.
static void history_pick_cb(lv_event_t *e) {
	if (switcher_back_drag_active() || player_sheet_drag_active()) {
		return;
	}
	int index = (int)(intptr_t)lv_event_get_user_data(e);
	if (index < 0 || index >= history_count) {
		return;
	}
	lv_textarea_set_text(search_field, history_terms[index]);
	search_run();
}

// Draws the history, in the space the keyboard leaves free.
static void search_history_rebuild(void) {
	if (!search_history_list) {
		return;
	}
	history_load();
	lv_obj_clean(search_history_list);

	bool any = history_count > 0;
	lv_obj_set_hidden(search_history_list, !any);
	if (search_history_empty) {
		lv_obj_set_hidden(search_history_empty, any);
	}
	if (!any) {
		return;
	}

	const lv_font_t *font = page_cfg && page_cfg->screen_width < 320 ? &font_ui_18 : &font_ui_24;
	for (int i = 0; i < history_count; i++) {
		lv_obj_t *row = lv_btn_create(search_history_list);
		lv_obj_set_width(row, lv_pct(100));
		lv_obj_set_height(row, bp_pick(40, 68));
		lv_obj_add_style(row, &theme_style_card, 0);
		lv_obj_add_style(row, &theme_style_card_pressed, LV_STATE_PRESSED);
		lv_obj_set_style_radius(row, bp_pick(10, 12), 0);
		lv_obj_set_style_border_width(row, 0, 0);
		lv_obj_set_style_shadow_width(row, 0, 0);
		lv_obj_set_style_pad_hor(row, bp_pick(8, 20), 0);
		lv_obj_set_style_pad_column(row, bp_pick(4, 10), 0);
		lv_obj_set_scrollable(row, false);
		lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
		lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
		lv_obj_add_event_cb(row, history_pick_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);

		lv_obj_t *label = lv_label_create(row);
		lv_label_set_text(label, history_terms[i]);
		lv_obj_add_style(label, &theme_style_text, 0);
		lv_obj_set_style_text_font(label, font, 0);
		lv_obj_set_flex_grow(label, 1);
		lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
		lv_obj_set_height(label, lv_font_get_line_height(font));

		lv_obj_t *x = lv_btn_create(row);
		lv_obj_set_size(x, bp_pick(30, 44), bp_pick(30, 44));
		lv_obj_set_style_bg_opa(x, LV_OPA_TRANSP, 0);
		lv_obj_set_style_border_width(x, 0, 0);
		lv_obj_set_style_shadow_width(x, 0, 0);
		lv_obj_set_style_pad_all(x, 0, 0);
		lv_obj_add_event_cb(x, history_forget_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);

		lv_obj_t *glyph = lv_image_create(x);
		lv_image_set_src(glyph, &icon_clear);
		lv_obj_add_style(glyph, &theme_style_icon, 0);
		lv_obj_set_style_image_opa(glyph, LV_OPA_60, 0);
		lv_obj_center(glyph);
	}
}

// From the settings page: forget every query at once.
static void clear_search_history_cb(lv_event_t *e) {
	(void)e;
	if (switcher_back_drag_active()) {
		return;
	}
	history_load();
	history_count = 0;
	history_save();
	search_history_rebuild();
	toast_success("search_history_cleared");
}

// Every return from the results lands here with the history, not the
// keyboard: the query has already been run.
static void search_loaded_cb(lv_event_t *e) {
	no_art_loaded_cb(e);
	if (search_keyboard && !keyboard_is_visible(search_keyboard)) {
		search_history_rebuild();
	}
}

static void show_search(ytmusic_filter_t filter) {
	search_filter = filter;
	if (search_pill_row) {
		lv_obj_set_hidden(search_pill_row, filter == YTMUSIC_FILTER_PODCASTS);
	}
	if (filter != YTMUSIC_FILTER_PODCASTS) {
		search_filter = search_kind_index == 1 ? YTMUSIC_FILTER_ALBUMS : (search_kind_index == 2 ? YTMUSIC_FILTER_ARTISTS : YTMUSIC_FILTER_SONGS);
		paint_search_pills();
	}
	lv_label_set_text(search_title, tr(filter == YTMUSIC_FILTER_PODCASTS ? "ytmusic_podcasts" : "ytmusic_search"));
	lv_textarea_set_text(search_field, "");
	keyboard_reset(search_keyboard);
	// The history first, unless there is none to show: then the keyboard is
	// what the page opens on, and the first tap has something to do.
	history_load();
	search_keyboard_show(history_count == 0);
	lv_obj_add_state(search_field, LV_STATE_FOCUSED);
	lv_obj_send_event(search_field, LV_EVENT_FOCUSED, NULL);
	switch_screen(search_screen);
}

static void search_accept_cb(lv_event_t *e) {
	(void)e;
	search_run();
}

static void search_field_cb(lv_event_t *e) {
	(void)e;
	keyboard_set_field(search_keyboard, search_field);
	// Tapping the field is what asks for the keyboard, and the history steps
	// aside for it -- neither has the room to show at the same time.
	search_keyboard_show(true);
}

static void search_cb(lv_event_t *e) {
	(void)e;
	if (!ytmusic_configured() || !wifi_is_connected()) {
		return;
	}
	show_search(YTMUSIC_FILTER_SONGS);
}

static void podcasts_cb(lv_event_t *e) {
	(void)e;
	show_search(YTMUSIC_FILTER_PODCASTS);
}

// "Top songs" is the charts page: the store answers it with the chart rows and
// with the playlists that hold them, and both are worth showing.
static void charts_cb(lv_event_t *e) {
	(void)e;
	start_list_request(LIST_BROWSE, false, YTMUSIC_FILTER_SONGS, NULL, "FEmusic_charts", "ytmusic_charts", false);
}

// The rows of the player's own list, in the shape the lists draw.
//
// The sleeve is derived from the video id rather than stored: YouTube serves
// the same frame for every id at i.ytimg.com, so a song liked months ago still
// has its artwork after the cache was swept.
static void favourites_fill_items(void) {
	track_count = 0;
	int count = ytlikes_count();
	for (int i = 0; i < count && track_count < YTMUSIC_PAGE_RESULTS; i++) {
		const ytlike_t *like = ytlikes_at(i);
		if (!like || !like->id[0]) continue;
		ytmusic_item_t *item = &track_items[track_count++];
		memset(item, 0, sizeof(*item));
		item->kind = YTMUSIC_ITEM_TRACK;
		snprintf(item->id, sizeof(item->id), "%s", like->id);
		snprintf(item->title, sizeof(item->title), "%s", like->title[0] ? like->title : like->id);
		snprintf(item->subtitle, sizeof(item->subtitle), "%s", like->artist);
		snprintf(item->album, sizeof(item->album), "%s", like->album);
		snprintf(item->thumbnail, sizeof(item->thumbnail), "https://i.ytimg.com/vi/%s/hqdefault.jpg",
				 like->id);
		item->duration = like->duration;
	}
}

// Repaints the list after a heart changed it. A row that was just unliked is
// gone here, which is the point: the screen is the list.
static void favourites_refill(void) {
	favourites_fill_items();
	lv_label_set_text(tracks_title, tr("ytmusic_favourites"));
	fill_item_list(tracks_list, track_items, track_count, item_clicked_cb);
}

static void favourites_refill_async(void *user) {
	(void)user;
	if (tracks_are_likes) favourites_refill();
}

// Favourites, the two ways there are.
//
// With a browser session imported, this is the account's own liked songs -- the
// only credential Google's music endpoints accept for that list. Without one it
// is the player's own list of what was hearted here, which is why a heart now
// works with no account at all: it used to write a cache path into the music
// database and disappear when the cache was swept.
static void favourites_cb(lv_event_t *e) {
	(void)e;
	if (ytmusic_session_cookie_attached()) {
		tracks_are_likes = false;
		start_list_request(LIST_BROWSE, false, YTMUSIC_FILTER_SONGS, NULL, "VLLM", "ytmusic_favourites", false);
		return;
	}
	list_album[0] = '\0';
	list_artist[0] = '\0';
	list_thumbnail[0] = '\0';
	favourites_fill_items();
	if (track_count <= 0) {
		toast_error("ytmusic_favourites_empty");
		return;
	}
	tracks_are_likes = true;
	favourites_refill();
	switch_screen(tracks_screen);
}

static void logout_cb(lv_event_t *e) {
	(void)e;
	if (ytmusic_logout()) {
		home_refresh_session_rows();
		toast_success("ytmusic_signed_out");
	}
	else toast_error("ytmusic_session_remove_failed");
}

// The session file is the only sign-in InnerTube accepts, so it has a row of
// its own rather than being a hidden feature of the card.
static void import_session_cb(lv_event_t *e) {
	(void)e;
	char error[192] = "";
	if (ytmusic_import_session_file(page_cfg ? page_cfg->sd_root_path : NULL, error, sizeof(error))) {
		home_refresh_session_rows();
		toast_success("ytmusic_session_imported");
	} else {
		toast_error(error[0] ? error : "ytmusic_session_file_missing");
	}
}

static void build_search_page(void) {
	search_screen = lv_obj_create(NULL);
	lv_obj_add_style(search_screen, &theme_style_screen, 0);
	lv_obj_set_scrollable(search_screen, false);
	search_title = settingsrow_title(search_screen, page_cfg, "ytmusic_search");
	search_field = lv_textarea_create(search_screen);
	lv_textarea_set_one_line(search_field, true);
	lv_textarea_set_placeholder_text(search_field, tr("artist_album_or_track"));
	const bool compact = page_cfg->screen_width < 320;
	const int top = settingsrow_content_top(page_cfg);
	const int field_h = compact ? 38 : 62;
	const int pill_h = compact ? 30 : 42;
	const int pill_gap = compact ? 4 : 8;
	lv_obj_set_size(search_field, page_cfg->screen_width - 2 * page_cfg->padding, field_h);
	lv_obj_align(search_field, LV_ALIGN_TOP_LEFT, page_cfg->padding, settingsrow_content_top(page_cfg));
	lv_obj_add_style(search_field, &theme_style_card, 0);
	lv_obj_set_scrollbar_mode(search_field, LV_SCROLLBAR_MODE_OFF);
	lv_obj_set_style_radius(search_field, compact ? 10 : 12, 0);
	lv_obj_set_style_border_width(search_field, 0, 0);
	lv_obj_set_style_shadow_width(search_field, 0, 0);
	lv_obj_set_style_pad_all(search_field, compact ? 8 : 14, 0);
	lv_obj_set_style_text_font(search_field, compact ? &font_ui_18 : &font_ui_24, 0);
	keyboard_style_caret(search_field);
	lv_obj_add_event_cb(search_field, search_field_cb, LV_EVENT_CLICKED, NULL);
	// Under the field: the history while nothing is typed, the keyboard while
	// something is. The two take the same space, so only one of them is ever
	// shown (see search_keyboard_show).
	search_pill_row = lv_obj_create(search_screen);
	lv_obj_set_size(search_pill_row, page_cfg->screen_width - 2 * page_cfg->padding, pill_h);
	lv_obj_align(search_pill_row, LV_ALIGN_TOP_LEFT, page_cfg->padding, top + field_h + pill_gap);
	lv_obj_set_style_bg_opa(search_pill_row, LV_OPA_TRANSP, 0);
	lv_obj_set_style_border_width(search_pill_row, 0, 0);
	lv_obj_set_style_pad_all(search_pill_row, 0, 0);
	lv_obj_set_style_pad_column(search_pill_row, compact ? 6 : 10, 0);
	lv_obj_set_scroll_dir(search_pill_row, LV_DIR_HOR);
	lv_obj_set_scrollbar_mode(search_pill_row, LV_SCROLLBAR_MODE_OFF);
	lv_obj_set_event_bubble(search_pill_row, true);
	lv_obj_set_flex_flow(search_pill_row, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(search_pill_row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

	static const char *const KINDS[] = {"search_tracks_2", "albums", "artists"};
	for (int i = 0; i < 3; i++) {
		lv_obj_t *pill = lv_btn_create(search_pill_row);
		lv_obj_set_size(pill, LV_SIZE_CONTENT, pill_h);
		lv_obj_set_style_pad_hor(pill, compact ? 12 : 22, 0);
		lv_obj_set_style_radius(pill, LV_RADIUS_CIRCLE, 0);
		lv_obj_set_style_shadow_width(pill, 0, 0);
		lv_obj_set_style_border_width(pill, 0, 0);
		lv_obj_add_event_cb(pill, search_pill_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);

		lv_obj_t *label = lv_label_create(pill);
		lv_label_set_text(label, tr(KINDS[i]));
		lv_obj_set_style_text_font(label, compact ? &font_ui_16 : &font_ui_20, 0);
		lv_obj_center(label);
		search_pills[i] = pill;
	}
	paint_search_pills();

	int history_top = top + field_h + pill_gap + pill_h + pill_gap;
	int history_h = page_cfg->screen_height - history_top - page_cfg->padding;
	if (history_h < 40) {
		history_h = 40;
	}

	search_history_list = lv_obj_create(search_screen);
	lv_obj_set_size(search_history_list, page_cfg->screen_width - 2 * page_cfg->padding, history_h);
	lv_obj_align(search_history_list, LV_ALIGN_TOP_LEFT, page_cfg->padding, history_top);
	lv_obj_set_style_bg_opa(search_history_list, LV_OPA_TRANSP, 0);
	lv_obj_set_style_border_width(search_history_list, 0, 0);
	lv_obj_set_style_radius(search_history_list, 0, 0);
	lv_obj_set_style_pad_all(search_history_list, 0, 0);
	lv_obj_set_style_pad_gap(search_history_list, bp_pick(4, 8), 0);
	lv_obj_set_scroll_dir(search_history_list, LV_DIR_VER);
	lv_obj_set_scrollbar_mode(search_history_list, LV_SCROLLBAR_MODE_AUTO);
	lv_obj_set_event_bubble(search_history_list, true);
	lv_obj_set_flex_flow(search_history_list, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(search_history_list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
	lv_obj_set_hidden(search_history_list, true);

	search_history_empty = lv_label_create(search_screen);
	lv_label_set_text(search_history_empty, tr("no_searches_yet"));
	lv_obj_add_style(search_history_empty, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(search_history_empty, page_cfg->screen_width < 320 ? &font_ui_18 : &font_ui_24, 0);
	lv_obj_set_width(search_history_empty, page_cfg->screen_width - 2 * page_cfg->padding);
	lv_label_set_long_mode(search_history_empty, LV_LABEL_LONG_WRAP);
	// Level with where the first history row would be: with nothing searched
	// yet this label is the whole history.
	lv_obj_align(search_history_empty, LV_ALIGN_TOP_LEFT, page_cfg->padding, history_top + 2);
	lv_obj_set_hidden(search_history_empty, true);

	search_keyboard = keyboard_create(search_screen, page_cfg->screen_width,
		page_cfg->screen_width < 320 ? 144 : 316, search_field, NULL, "ok", search_accept_cb, NULL);
	// The history is what the page opens on; the keyboard arrives with the
	// first tap in the field.
	search_keyboard_show(false);
	lv_obj_add_event_cb(search_screen, search_loaded_cb, LV_EVENT_SCREEN_LOADED, NULL);
	switcher_attach_back_gesture(search_screen);
	theme_register_refresh(paint_search_pills);

	results_screen = lv_obj_create(NULL);
	lv_obj_add_style(results_screen, &theme_style_screen, 0);
	results_list = settingsrow_page(results_screen, page_cfg, "YouTube Music");
	results_title = settingsrow_page_title(results_screen);
	// The corner is empty until rows are chosen: the cross leaves the mode and
	// the tick opens the action sheet, in the same order as other list pages.
	settingsrow_title_corner_slots(results_title, page_cfg, 2);
	results_sel_close = page_corner_button(results_screen, page_cfg, 0, &icon_close, selection_cancel_cb);
	results_sel_action = page_corner_button(results_screen, page_cfg, 1, &icon_check, selection_sheet_cb);
	lv_obj_set_hidden(results_sel_action, true);
	lv_obj_set_hidden(results_sel_close, true);
	player_sheet_attach_drag(results_list, true);
	// The covers belong to the list that is on screen, and these two carry rows.
	lv_obj_add_event_cb(results_screen, results_loaded_cb, LV_EVENT_SCREEN_LOADED, NULL);
	lv_obj_add_event_cb(results_list, art_scrolled_cb, LV_EVENT_SCROLL, NULL);
	switcher_attach_back_gesture(results_screen);

	tracks_screen = lv_obj_create(NULL);
	lv_obj_add_style(tracks_screen, &theme_style_screen, 0);
	tracks_list = settingsrow_page(tracks_screen, page_cfg, "YouTube Music");
	tracks_title = settingsrow_page_title(tracks_screen);
	settingsrow_title_corner_slots(tracks_title, page_cfg, 3);
	podcast_order_button = page_corner_button(tracks_screen, page_cfg, 0,
											 podcast_newest_first() ? &icon_sort_date_new : &icon_sort_date_old,
											 podcast_order_cb);
	tracks_sel_close = page_corner_button(tracks_screen, page_cfg, 1, &icon_close, selection_cancel_cb);
	tracks_sel_action = page_corner_button(tracks_screen, page_cfg, 2, &icon_check, selection_sheet_cb);
	lv_obj_set_hidden(podcast_order_button, true);
	lv_obj_set_hidden(tracks_sel_action, true);
	lv_obj_set_hidden(tracks_sel_close, true);
	player_sheet_attach_drag(tracks_list, true);
	lv_obj_add_event_cb(tracks_screen, tracks_loaded_cb, LV_EVENT_SCREEN_LOADED, NULL);
	lv_obj_add_event_cb(tracks_list, art_scrolled_cb, LV_EVENT_SCROLL, NULL);
	switcher_attach_back_gesture(tracks_screen);

	explore_screen = lv_obj_create(NULL);
	lv_obj_add_style(explore_screen, &theme_style_screen, 0);
	explore_list = settingsrow_page(explore_screen, page_cfg, "ytmusic_explore");
	player_sheet_attach_drag(explore_list, true);
	// Its rows are carousel names, not entries with artwork: whoever was
	// holding the slots gives them back while this page is up.
	lv_obj_add_event_cb(explore_screen, no_art_loaded_cb, LV_EVENT_SCREEN_LOADED, NULL);
	switcher_attach_back_gesture(explore_screen);
}

// The value beside the source row, and whether it is the chosen one.
static void source_refresh(void) {
	if (!source_value_label) return;
	char active[YTMUSIC_SOURCE_MAX] = "";
	ytmusic_source_active(active, sizeof(active));
	const char *shown = active;
	for (int i = 0; i < source_item_count; i++) {
		if (strcmp(source_items[i].value, active) == 0) {
			shown = tr(source_items[i].label);
			break;
		}
	}
	// An instance pinned by hand and no longer in the list still says what it
	// is, without the scheme: the row's value is a host name.
	const char *host = strstr(shown, "://");
	lv_label_set_text(source_value_label, host ? host + 3 : shown);
}

static void source_row_build(void);

static void source_pick_cb(lv_event_t *e) {
	int index = (int)(intptr_t)lv_event_get_user_data(e);
	if (index < 0 || index >= source_item_count) return;
	if (!ytmusic_source_select(source_items[index].value)) {
		toast_error("ytmusic_source_invalid");
		return;
	}
	toast_success("ytmusic_source_saved");
	source_refresh();
	if (source_list) lv_obj_clean(source_list);
	source_item_count = ytmusic_sources(source_items, YTMUSIC_SOURCES_MAX);
	source_row_build();
}

static void source_row_build(void) {
	if (!source_list) return;
	char active[YTMUSIC_SOURCE_MAX] = "";
	ytmusic_source_active(active, sizeof(active));
	for (int i = 0; i < source_item_count; i++) {
		lv_obj_t *value = NULL;
		settingsrow_add(source_list, tr(source_items[i].label), &value, source_pick_cb,
						(void *)(intptr_t)i);
		if (value && strcmp(source_items[i].value, active) == 0) {
			lv_label_set_text(value, tr("on"));
		}
	}
}

static void source_loaded_cb(lv_event_t *e) {
	(void)e;
	if (source_list) lv_obj_clean(source_list);
	source_item_count = ytmusic_sources(source_items, YTMUSIC_SOURCES_MAX);
	source_row_build();
}

static void open_source_cb(lv_event_t *e) {
	(void)e;
	switch_screen(source_screen);
}

static void build_source_page(gui_config_t *cfg) {
	source_screen = lv_obj_create(NULL);
	lv_obj_add_style(source_screen, &theme_style_screen, 0);
	source_list = settingsrow_page(source_screen, cfg, "ytmusic_source");
	player_sheet_attach_drag(source_list, true);
	lv_obj_add_event_cb(source_screen, source_loaded_cb, LV_EVENT_SCREEN_LOADED, NULL);
	switcher_attach_back_gesture(source_screen);
}

// ---------------------------------------------------------------------------
// the arrangement of the home page
// ---------------------------------------------------------------------------

// A number list, as both settings store it: "2,0,4".
static int home_parse_id_list(const char *text, int *out, int max) {
	int count = 0;
	const char *p = text ? text : "";
	while (*p && count < max) {
		while (*p == ',' || *p == ' ') p++;
		if (!*p) break;
		out[count++] = atoi(p);
		while (*p >= '0' && *p <= '9') p++;
	}
	return count;
}

// An arrangement written before Search left the list: those ids ran 0 for
// Search through 4 for Podcasts, so everything after Search shifts down by one
// and Search itself is dropped. The version key is written on the first read,
// so the shift happens exactly once and an arrangement already written in the
// new numbers is left alone.
static void yt_home_migrate(void) {
	if ((int)config_get_int("ytmusic", "home_rows_version", 0) >= 1) {
		return;
	}

	int old[YT_HOME_ROWS + 1];
	int n = home_parse_id_list(config_get("ytmusic", "home_order", ""), old, YT_HOME_ROWS + 1);
	if (n > 0) {
		char order[64] = "";
		for (int i = 0; i < n; i++) {
			if (old[i] <= 0 || old[i] > YT_HOME_ROWS) {
				continue; // Search, or a number no build ever wrote
			}
			size_t used = strlen(order);
			snprintf(order + used, sizeof(order) - used, "%s%d", used ? "," : "", old[i] - 1);
		}
		config_set("ytmusic", "home_order", order);
	}

	n = home_parse_id_list(config_get("ytmusic", "home_hidden", ""), old, YT_HOME_ROWS + 1);
	if (n > 0) {
		char hidden[64] = "";
		for (int i = 0; i < n; i++) {
			if (old[i] <= 0 || old[i] > YT_HOME_ROWS) {
				continue;
			}
			size_t used = strlen(hidden);
			snprintf(hidden + used, sizeof(hidden) - used, "%s%d", used ? "," : "", old[i] - 1);
		}
		config_set("ytmusic", "home_hidden", hidden);
	}

	config_set_int("ytmusic", "home_rows_version", 1);
	config_save();
}

// Read again whenever the home page is built: a change made on the arrange page
// is then what the page shows the next time it is looked at, without a restart.
static void yt_home_order_load(void) {
	yt_home_migrate();
	for (int i = 0; i < YT_HOME_ROWS; i++) {
		yt_home_order[i] = i;
		yt_home_hidden[i] = false;
	}
	const char *saved = config_get("ytmusic", "home_order", "");
	if (saved[0]) {
		bool seen[YT_HOME_ROWS] = {false};
		int at = 0;
		const char *p = saved;
		while (*p && at < YT_HOME_ROWS) {
			while (*p == ',' || *p == ' ') p++;
			if (!*p) break;
			int id = atoi(p);
			while (*p >= '0' && *p <= '9') p++;
			if (id >= 0 && id < YT_HOME_ROWS && !seen[id]) {
				seen[id] = true;
				yt_home_order[at++] = id;
			}
		}
		// A row added by a later build has no place in the saved order yet.
		for (int id = 0; id < YT_HOME_ROWS; id++) {
			if (!seen[id] && at < YT_HOME_ROWS) yt_home_order[at++] = id;
		}
	}
	const char *hidden = config_get("ytmusic", "home_hidden", "");
	const char *p = hidden;
	while (*p) {
		while (*p == ',' || *p == ' ') p++;
		if (!*p) break;
		int id = atoi(p);
		while (*p >= '0' && *p <= '9') p++;
		if (id >= 0 && id < YT_HOME_ROWS) yt_home_hidden[id] = true;
	}
}

static void yt_home_order_save(void) {
	char order[64] = "", hidden[64] = "";
	for (int slot = 0; slot < YT_HOME_ROWS; slot++) {
		size_t used = strlen(order);
		snprintf(order + used, sizeof(order) - used, "%s%d", used ? "," : "", yt_home_order[slot]);
	}
	bool any = false;
	for (int id = 0; id < YT_HOME_ROWS; id++) {
		if (!yt_home_hidden[id]) continue;
		size_t used = strlen(hidden);
		snprintf(hidden + used, sizeof(hidden) - used, "%s%d", any ? "," : "", id);
		any = true;
	}
	config_set("ytmusic", "home_order", order);
	config_set("ytmusic", "home_hidden", hidden);
	config_save();
}

static void yt_order_move_cb(lv_event_t *e);
static void yt_order_toggle_cb(lv_event_t *e);
static void region_pick_cb(lv_event_t *e);

static void yt_order_rebuild(void) {
	if (!order_rows) return;
	lv_obj_clean(order_rows);
	for (int pos = 0; pos < YT_HOME_ROWS; pos++) {
		int id = yt_home_order[pos];
		lv_obj_t *row = lv_obj_create(order_rows);
		lv_obj_set_size(row, lv_pct(100), bp_pick(40, 56));
		lv_obj_add_style(row, &theme_style_card, 0);
		lv_obj_set_style_radius(row, bp_tile_radius(), 0);
		lv_obj_set_style_border_width(row, 0, 0);
		lv_obj_set_style_shadow_width(row, 0, 0);
		lv_obj_set_style_pad_hor(row, bp_pick(6, 12), 0);
		lv_obj_set_style_pad_gap(row, bp_pick(1, 5), 0);
		lv_obj_set_scrollable(row, false);
		lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
		lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

		lv_obj_t *name = lv_label_create(row);
		lv_label_set_text(name, tr(YT_HOME_SLUGS[id]));
		lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
		lv_obj_set_flex_grow(name, 1);
		lv_obj_add_style(name, &theme_style_text, 0);
		lv_obj_set_style_text_font(name, bp_is_tempotec_v1() ? &font_ui_16 : &font_ui_22, 0);

		// Up and down, in the list's own order: a row moves one place at a time
		// and the switch beside it is whether the home page shows it at all.
		for (int dir = 0; dir < 2; dir++) {
			lv_obj_t *button = lv_btn_create(row);
			lv_obj_set_size(button, bp_pick(28, 40), bp_pick(30, 40));
			lv_obj_set_style_bg_opa(button, LV_OPA_TRANSP, 0);
			lv_obj_set_style_border_width(button, 0, 0);
			lv_obj_set_style_shadow_width(button, 0, 0);
			lv_obj_set_style_pad_all(button, 0, 0);
			lv_obj_add_event_cb(button, yt_order_move_cb, LV_EVENT_CLICKED,
								(void *)(intptr_t)((dir ? 0 : 0x100) | pos));
			lv_obj_t *icon = lv_image_create(button);
			lv_image_set_src(icon, &icon_chevron_up);
			lv_image_set_rotation(icon, dir ? 1800 : 0);
			lv_obj_add_style(icon, &theme_style_icon, 0);
			lv_obj_center(icon);
			if ((dir == 0 && pos == 0) || (dir == 1 && pos == YT_HOME_ROWS - 1)) {
				lv_obj_add_state(button, LV_STATE_DISABLED);
			}
		}

		lv_obj_t *toggle = lv_switch_create(row);
		lv_obj_set_size(toggle, bp_pick(34, 48), bp_pick(20, 28));
		if (!yt_home_hidden[id]) {
			lv_obj_add_state(toggle, LV_STATE_CHECKED);
		}
		lv_obj_add_event_cb(toggle, yt_order_toggle_cb, LV_EVENT_VALUE_CHANGED,
							(void *)(intptr_t)id);
	}
	lv_obj_update_layout(order_rows);
}

static void yt_order_move_cb(lv_event_t *e) {
	int encoded = (int)(intptr_t)lv_event_get_user_data(e);
	int pos = encoded & 0xff;
	int target = pos + ((encoded & 0x100) ? -1 : 1);
	if (pos < 0 || pos >= YT_HOME_ROWS || target < 0 || target >= YT_HOME_ROWS) return;
	int swap = yt_home_order[pos];
	yt_home_order[pos] = yt_home_order[target];
	yt_home_order[target] = swap;
	yt_home_order_save();
	yt_order_rebuild();
	home_build();
}

static void yt_order_toggle_cb(lv_event_t *e) {
	int id = (int)(intptr_t)lv_event_get_user_data(e);
	if (id < 0 || id >= YT_HOME_ROWS) return;
	yt_home_hidden[id] = !lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
	yt_home_order_save();
	home_build();
}

static void order_loaded_cb(lv_event_t *e) {
	(void)e;
	yt_home_order_load();
	yt_order_rebuild();
}

static void open_order_cb(lv_event_t *e) {
	(void)e;
	switch_screen(order_screen);
}

static void build_order_page(gui_config_t *cfg) {
	order_screen = lv_obj_create(NULL);
	lv_obj_add_style(order_screen, &theme_style_screen, 0);
	order_rows = settingsrow_page(order_screen, cfg, "ytmusic_arrange");
	player_sheet_attach_drag(order_rows, true);
	lv_obj_add_event_cb(order_screen, order_loaded_cb, LV_EVENT_SCREEN_LOADED, NULL);
	switcher_attach_back_gesture(order_screen);
}

// ---------------------------------------------------------------------------
// the region page
// ---------------------------------------------------------------------------

static void region_refresh(void) {
	if (!region_value_label) return;
	char region[YTMUSIC_REGION_MAX], language[YTMUSIC_REGION_MAX];
	ytmusic_region_active(region, sizeof(region), language, sizeof(language));
	for (int i = 0; i < region_item_count; i++) {
		if (strcmp(region_items[i].region, region) == 0 &&
			strcmp(region_items[i].language, language) == 0) {
			lv_label_set_text(region_value_label, tr(region_items[i].label));
			return;
		}
	}
	// A pair written into the config by hand still says what it is.
	char both[24];
	snprintf(both, sizeof(both), "%s · %s", region, language);
	lv_label_set_text(region_value_label, both);
}

static void region_row_build(void) {
	if (!region_list) return;
	lv_obj_clean(region_list);
	char region[YTMUSIC_REGION_MAX], language[YTMUSIC_REGION_MAX];
	ytmusic_region_active(region, sizeof(region), language, sizeof(language));
	for (int i = 0; i < region_item_count; i++) {
		lv_obj_t *value = NULL;
		settingsrow_add(region_list, tr(region_items[i].label), &value, region_pick_cb,
						(void *)(intptr_t)i);
		if (value && strcmp(region_items[i].region, region) == 0 &&
			strcmp(region_items[i].language, language) == 0) {
			lv_label_set_text(value, tr("on"));
		}
	}
}

static void region_pick_cb(lv_event_t *e) {
	int index = (int)(intptr_t)lv_event_get_user_data(e);
	if (index < 0 || index >= region_item_count) return;
	if (!ytmusic_region_select(region_items[index].region, region_items[index].language)) {
		toast_error("ytmusic_region_invalid");
		return;
	}
	toast_success("ytmusic_region_saved");
	region_refresh();
	region_row_build();
}

static void region_loaded_cb(lv_event_t *e) {
	(void)e;
	region_item_count = ytmusic_regions(region_items, YT_REGION_MAX);
	region_row_build();
}

static void open_region_cb(lv_event_t *e) {
	(void)e;
	switch_screen(region_screen);
}

static void build_region_page(gui_config_t *cfg) {
	region_screen = lv_obj_create(NULL);
	lv_obj_add_style(region_screen, &theme_style_screen, 0);
	region_list = settingsrow_page(region_screen, cfg, "ytmusic_region");
	player_sheet_attach_drag(region_list, true);
	lv_obj_add_event_cb(region_screen, region_loaded_cb, LV_EVENT_SCREEN_LOADED, NULL);
	switcher_attach_back_gesture(region_screen);
}

// ---------------------------------------------------------------------------
// quality and the sleep timer
// ---------------------------------------------------------------------------

static void yt_quality_refresh(void) {
	if (quality_pills[0]) settingsrow_pill_active(quality_pills[0], ytmusic_quality_high());
	if (quality_pills[1]) settingsrow_pill_active(quality_pills[1], !ytmusic_quality_high());
}

static void yt_quality_pick_cb(lv_event_t *e) {
	ytmusic_quality_set((int)(intptr_t)lv_event_get_user_data(e) != 0);
	yt_quality_refresh();
}

static void yt_sleep_refresh(void) {
	settingsrow_duration_set_minutes(&yt_sleep_row, sleeptimer_minutes(SLEEPTIMER_MUSIC));
	settingsrow_duration_expanded(&yt_sleep_row, sleeptimer_enabled(SLEEPTIMER_MUSIC));
	settingsrow_duration_repaint(&yt_sleep_row);
}

// The music transport's own timer, the one the card and Tidal use: a YouTube
// Music song plays through the same player, so it is the same stretch counting
// down and the same switch that stops it.
static void yt_sleep_toggle_cb(lv_event_t *e) {
	(void)e;
	sleeptimer_set_enabled(SLEEPTIMER_MUSIC, lv_obj_has_state(yt_sleep_row.toggle, LV_STATE_CHECKED));
	yt_sleep_refresh();
}

static void yt_sleep_wheel_cb(lv_event_t *e) {
	(void)e;
	sleeptimer_set_minutes(SLEEPTIMER_MUSIC, settingsrow_duration_minutes(&yt_sleep_row));
}

static void build_settings_page(gui_config_t *cfg) {
	build_source_page(cfg);
	build_region_page(cfg);
	build_order_page(cfg);
	settings_screen = lv_obj_create(NULL);
	lv_obj_add_style(settings_screen, &theme_style_screen, 0);
	lv_obj_t *container = settingsrow_page(settings_screen, cfg, "ytmusic_settings");

	// What this page plays, before what it fetches through. The sleep timer is
	// a module of its own (Plugins): off, the row is not drawn here either.
	if (addons_enabled("sleep_timer")) {
		settingsrow_toggle_duration(container, "sleep_timer", yt_sleep_toggle_cb, yt_sleep_wheel_cb,
								&yt_sleep_row);
		yt_sleep_refresh();
	}
	lv_obj_t *pill_box = NULL;
	settingsrow_pills(container, "ytmusic_quality", &pill_box);
	quality_pills[0] = settingsrow_pill(pill_box, "ytmusic_quality_high", 1, yt_quality_pick_cb);
	quality_pills[1] = settingsrow_pill(pill_box, "ytmusic_quality_standard", 0, yt_quality_pick_cb);
	yt_quality_refresh();
	lv_obj_t *region_row =
		settingsrow_add(container, "ytmusic_region", &region_value_label, open_region_cb, NULL);
	settingsrow_add_caption(region_row, "ytmusic_region_note");
	region_item_count = ytmusic_regions(region_items, YT_REGION_MAX);
	region_refresh();
	lv_obj_t *order_row = settingsrow_add(container, "ytmusic_arrange", NULL, open_order_cb, NULL);
	settingsrow_add_caption(order_row, "ytmusic_arrange_note");

	lv_obj_t *source_row =
		settingsrow_add(container, "ytmusic_source", &source_value_label, open_source_cb, NULL);
	settingsrow_add_caption(source_row, "ytmusic_source_note");
	source_item_count = ytmusic_sources(source_items, YTMUSIC_SOURCES_MAX);
	source_refresh();
	sign_out_row = settingsrow_action(container, "ytmusic_sign_out", logout_cb, NULL);
	settingsrow_add_icon(sign_out_row, &icon_log_out, THEME_SEMANTIC_RED);
	lv_obj_t *import_row = settingsrow_action(container, "ytmusic_import_session", import_session_cb, NULL);
	settingsrow_add_icon(import_row, &icon_import, THEME_SEMANTIC_PURPLE);
	settingsrow_add_caption(import_row, "ytmusic_import_session_note");
	// The queries the search page keeps, cleared from here as Tidal's are
	// cleared from its own settings.
	settingsrow_action(container, "clear_search_history", clear_search_history_cb, NULL);
	home_refresh_session_rows();
	player_sheet_attach_drag(container, true);
	switcher_attach_back_gesture(settings_screen);
}

static lv_obj_t *home_settings_btn;
static lv_obj_t *home_downloads_btn;
static lv_obj_t *home_search_btn;

static void update_home_corner_buttons(void) {
	bool configured = ytmusic_configured();
	bool search = configured && wifi_is_connected();
	bool downloads = true;
	bool settings = true;
	int slot = 0;
	if (home_settings_btn) {
		lv_obj_set_hidden(home_settings_btn, !settings);
		if (settings) {
			settingsrow_place_corner_button(home_settings_btn, page_cfg, slot++);
		}
	}
	if (home_downloads_btn) {
		lv_obj_set_hidden(home_downloads_btn, !downloads);
		if (downloads) {
			settingsrow_place_corner_button(home_downloads_btn, page_cfg, slot++);
		}
	}
	if (home_search_btn) {
		lv_obj_set_hidden(home_search_btn, !search);
		if (search) {
			settingsrow_place_corner_button(home_search_btn, page_cfg, slot++);
		}
	}
	if (ytmusic_screen && page_cfg) {
		settingsrow_title_corner_slots(settingsrow_page_title(ytmusic_screen), page_cfg, slot);
	}
}

// The rows, or the no-network notice in their place. Rebuilt on every entry
// into the page, because the Wi-Fi can have been turned on or off while the
// user was in the settings that did it.
static void home_build(void) {
	if (!home_container) {
		return;
	}
	lv_obj_clean(home_container);
	update_home_corner_buttons();

	if (!ytmusic_configured()) {
		lv_obj_t *note = lv_label_create(home_container);
		lv_label_set_long_mode(note, LV_LABEL_LONG_WRAP);
		lv_obj_set_width(note, lv_pct(100));
		lv_obj_add_style(note, &theme_style_text_dim, 0);
		lv_obj_set_style_text_font(note, &font_ui_20, 0);
		lv_label_set_text(note, tr("api_keys_unavailable"));
		return;
	}

	if (!wifi_is_connected()) {
		// The same notice as the podcast and Tidal pages: what is missing, on
		// the page that needs it, with the row that leads where it is fixed.
		lv_obj_t *note = lv_label_create(home_container);
		lv_label_set_long_mode(note, LV_LABEL_LONG_WRAP);
		lv_obj_set_width(note, lv_pct(100));
		lv_obj_add_style(note, &theme_style_text, 0); // normal colour: an instruction
		lv_obj_set_style_text_font(note, &font_ui_22, 0);
		lv_obj_set_style_text_align(note, LV_TEXT_ALIGN_CENTER, 0);
		lv_obj_set_style_pad_hor(note, 4, 0);
		lv_obj_set_style_pad_top(note, 14, 0);
		lv_label_set_text(note, tr("enable_wi_fi_first"));
		lv_obj_t *row = settingsrow_add(home_container, "wi_fi_settings", NULL, open_wifi_settings_cb, NULL);
		lv_obj_set_style_margin_top(row, 16, 0);
		return;
	}

	// The rows in the order the user arranged them, with the switched-off ones
	// left out -- see the arrange page behind the settings' "Home rows" row.
	static void (*const handlers[YT_HOME_ROWS])(lv_event_t *) = {
		explore_start_cb, charts_cb, favourites_cb, podcasts_cb,
	};
	static const lv_image_dsc_t *const icons[YT_HOME_ROWS] = {
		&icon_music2, &icon_music2, &icon_music_note, &icon_podcast_list,
	};
	static const theme_semantic_t tones[YT_HOME_ROWS] = {
		THEME_SEMANTIC_PURPLE,
		THEME_SEMANTIC_RED,
		THEME_SEMANTIC_RED,
		THEME_SEMANTIC_CYAN,
	};
	yt_home_order_load();
	for (int slot = 0; slot < YT_HOME_ROWS; slot++) {
		int id = yt_home_order[slot];
		if (yt_home_hidden[id]) continue;
		lv_obj_t *row = settingsrow_add(home_container, YT_HOME_SLUGS[id], NULL, handlers[id], NULL);
		settingsrow_add_icon(row, icons[id], tones[id]);
	}
}

static void home_loaded_cb(lv_event_t *e) {
	(void)e;
	art_deactivate();
	home_build();
}

void ytmusicpage_init(gui_config_t *cfg) {
	page_cfg = cfg;
	busy_build();
	// Where a downloaded cover is kept. Called here as much as for the shared
	// loader's benefit: with no card named it has nowhere to write and every
	// list keeps its placeholders, which is the honest answer.
	qobuzart_set_root(storage_sd_root());
	// And where the song being streamed is written. On the card, not in /tmp:
	// /tmp is tmpfs here, so a track kept there is a track kept in the heap the
	// decoder and a Bluetooth codec are also using. Without a card the cache
	// stays where it was, because a player with no card should still play.
	ytmusic_set_cache_root(storage_sd_root());
	ytmusic_screen = lv_obj_create(NULL);
	lv_obj_add_style(ytmusic_screen, &theme_style_screen, 0);
	home_container = settingsrow_page(ytmusic_screen, cfg, "YouTube Music");
	// The Tidal page's corner, in the order the music tab wears it: the gear on
	// the right, downloads beside it, the magnifier furthest left. Slots count
	// from the right edge, so slot 2 is the leftmost of the three.
	settingsrow_title_corner_slots(settingsrow_page_title(ytmusic_screen), cfg, 3);
	home_settings_btn = ytmusic_corner_button(cfg, 0, &icon_music_settings, open_settings_cb);
	home_downloads_btn = ytmusic_corner_button(cfg, 1, &icon_import, open_downloads_cb);
	home_search_btn = ytmusic_corner_button(cfg, 2, &icon_search, search_cb);
	home_build();
	lv_obj_add_event_cb(ytmusic_screen, home_loaded_cb, LV_EVENT_SCREEN_LOADED, NULL);
	player_sheet_attach_drag(home_container, true);
	build_search_page();
	build_settings_page(cfg);
	// Created paused: the first cover request resumes it, and it pauses itself
	// again when the last one lands or fails.
	art_timer = lv_timer_create(art_poll_cb, 120, NULL);
	if (art_timer) {
		lv_timer_pause(art_timer);
	}
	// "Is this queued song there yet?", asked by the player each time the
	// queue moves. The Tidal page registers the same kind of callback; it is
	// what keeps a queue of songs that have never been played from emptying
	// itself into silence.
	device_state_add_prepare_cb(ytmusicpage_prepare_track);
	switcher_attach_back_gesture(ytmusic_screen);
}
