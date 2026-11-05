#include "podcastpage.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <malloc.h>

#include "src/gui/board_profile.h"
#include "src/gui/settings/screensaver.h"
#include "src/gui/fonts/fonts.h"
#include "src/system/addons/addons.h"
#include "src/system/playback/sleeptimer.h"
#include "src/gui/shell/gui.h"
#include "src/gui/shell/icons.h"
#include "src/gui/shell/keyboard.h"
#include "src/gui/nowplaying/player.h"
#include "src/gui/shell/popover.h"
#include "src/gui/shell/toast.h"
#include "src/gui/streaming/podcastsaved.h"
#include "src/gui/streaming/qobuzart.h"
#include "src/system/streaming/listcache.h"
#include "src/gui/shell/settingsrow.h"
#include "src/gui/shell/spinner.h"
#include "src/gui/shell/switcher.h"
#include "src/gui/shell/theme.h"
#include "src/gui/wireless/wifisettings.h"
#include "src/system/decode/growfile.h"
#include "src/system/playback/device_state.h"
#include "src/system/core/config.h"
#include "src/system/core/lang.h"
#include "src/system/core/region.h"
#include "src/system/playback/playlist.h"
#include "src/system/streaming/podcast.h"
#include "src/system/streaming/podcastcache.h"
#include "src/system/streaming/podcastdl.h"
#include "src/system/streaming/podcastplayed.h"
#include "src/system/streaming/podcastsubs.h"
#include "src/system/streaming/streamturn.h"
#include "src/system/device/power.h"
#include "src/system/device/system.h"
#include "src/system/core/utils.h"
#include "src/system/net/wifi.h"
#include "src/system/net/wifitransfer.h"

// ---------------------------------------------------------------------------
// Podcasts, up front.
//
// The same shape as the Qobuz and Tidal pages, deliberately: opening all three
// must not mean learning three interfaces. The same ground rule therefore
// holds -- everything that talks to the network BLOCKS, so it cannot run on the
// GUI thread: one worker, one slot for the job to do, and results handed back
// with gui_post().
//
// Three things differ here, and every odd choice follows from them:
//
//   * there is no sign-in. Podcast Index is an open catalogue: search and read,
//     that is all. No login page, no token to renew, and "my podcasts" is a
//     list kept on the card (podcastsubs.h) rather than something living on the
//     service;
//   * nothing is fetched ahead. Qobuz and Tidal download the NEXT track while
//     the current one plays, because a track is five megabytes. An episode is
//     sixty and runs an hour: fetching ahead would fill the card with material
//     that may never be heard. What is needed is fetched when it is needed;
//   * episodes come in reverse order. The catalogue lists newest first, which
//     is the order to see them in but not the order to hear them in, so the
//     queue built from a tap holds the chosen episode and the ones AFTER it in
//     the list, that is, going back in time.
// ---------------------------------------------------------------------------

#define PODCAST_LIMIT PODCAST_PAGE_LIMIT
// How many entries are held at most. The catalogue has no offset, so "another
// page" here means asking for the same list with a higher limit.
//
// A podcast's episodes go up to the catalogue's own maximum per request, 1000.
// Search results and the chart stop at four pages, like the radio stations.
// The arrays are static and sized for the larger of the two; their pages only
// take memory once entries are written into them.
#define PODCAST_MAX_HELD 1000
#define PODCAST_FEEDS_MAX (4 * PODCAST_LIMIT)

// Episodes are asked for in steps of this many: the first step when a podcast
// is opened, another each time the scroll nears the bottom. Each step repeats
// the whole request with the higher limit.
#define PODCAST_EPISODES_STEP 100

// The longest queue a tapped episode builds: it and the ones after it in the
// list. Each queued episode that is not on the card yet gets its sidecar
// written at the tap.
#define PODCAST_QUEUE_MAX (4 * PODCAST_LIMIT)

lv_obj_t *podcast_screen;
lv_obj_t *podcast_list_screen; // the same object: see podcastpage.h
#define list_screen podcast_screen
static lv_obj_t *search_screen;
static lv_obj_t *settings_screen;

static lv_obj_t *busy_layer;
static lv_obj_t *busy_label;
static lv_obj_t *search_btn;
static lv_obj_t *settings_btn;
static lv_obj_t *saved_btn; // the downloaded podcasts, which need no network
static lv_obj_t *order_btn; // newest / oldest first, on an episode list
// The corner while rows are being chosen: the action for this kind of list, and
// the way out of the mode.
static lv_obj_t *sel_confirm_btn;
static lv_obj_t *sel_close_btn;

// The two pills at the top -- "My podcasts" and "Trending" -- and the row that
// holds them. Shown only while one of the two lists they name is underneath: on
// search results or on a podcast's episodes they would not be telling the
// truth, because what is on screen is neither of them.
static lv_obj_t *pill_row;
static lv_obj_t *pill_followed;
static lv_obj_t *pill_trending;

// The notice that takes the list's place when the section cannot work: the
// keys are missing, or the Wi-Fi is.
static lv_obj_t *note_label;
static lv_obj_t *note_wifi_row; // the "Wi-Fi settings" row under the network notice
static void note_wifi_row_cb(lv_event_t *e);

// ---------------------------------------------------------------------------
// the worker
// ---------------------------------------------------------------------------

typedef enum {
	JOB_NONE = 0,
	JOB_SEARCH,	  // search podcasts by name
	JOB_TRENDING, // the current chart
	JOB_EPISODES, // one podcast's episodes
	JOB_FOLLOWED, // the followed podcasts, which live on the card
	JOB_PLAY,	  // download an episode and start it
} job_kind_t;

typedef struct {
	job_kind_t kind;
	char text[160];	 // the query, or the title the list will carry
	long long id;	 // the podcast's or the episode's id (64 bit: see podcast.h)
	int index;		 // for JOB_PLAY: which row was tapped
	int want;		 // how many entries in all; 0 means one page
	// Whether this is the SAME list continuing rather than a new one. Set only
	// by the paging at the bottom of the list, never inferred from `want`: a
	// list can open asking for more than one page, and a continuation keeps the
	// rows already drawn and appends after them.
	bool more;
} job_t;

static pthread_mutex_t job_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t job_ready = PTHREAD_COND_INITIALIZER;
static pthread_cond_t result_taken = PTHREAD_COND_INITIALIZER;
static job_t pending;
static bool have_pending;
static bool worker_started;

// The results. Two arrays and not one because a podcast and an episode do not
// share fields, and merging them into a single struct would leave half the
// fields empty in every row.
// Feeds (shows) arrive only through search, the charts and the followed list,
// each capped at PODCAST_FEEDS_MAX; only the episodes of one podcast run to
// the catalogue maximum, and they live in result_episodes below.
static podcast_feed_t result_feeds[PODCAST_FEEDS_MAX];
static podcast_episode_t result_episodes[PODCAST_MAX_HELD];
static int result_count;
static int result_from; // from which row the on-screen list is (re)drawn
static int result_want; // how many entries were asked for
static int result_raw;	// how many the catalogue SENT, before the discards: this
						// is what says whether the answer was full (an episode without
						// audio dropped mid-page must not make a full page look
						// short)
static job_kind_t result_kind;
static char result_error[256];
static char result_title[160];

// The worker waits for the GUI to have taken the result before picking up the
// next job: without that, a second job could rewrite the arrays while the first
// was still being drawn.
static bool result_pending;

// Kept for the heading, whose reserved width follows the corner buttons.
static gui_config_t *g_cfg;

static void *worker_main(void *arg);
static void refresh_page_state(void);
static bool refresh_notes(void);
static void refresh_corner_buttons(void);
static bool network_up(void);
static void paint_pill(lv_obj_t *btn, bool on);
static void row_clicked_cb(lv_event_t *e);
static bool job_is_list(job_kind_t kind);
static void list_nav_drop(void);
static void list_nav_commit(void);
static void fill_list(int from);
static void busy_hide(void);
static void prefetch_urgent(const char *wanted_path);

static void submit(const job_t *job) {
	pthread_mutex_lock(&job_lock);
	pending = *job;
	have_pending = true;
	pthread_cond_signal(&job_ready);
	pthread_mutex_unlock(&job_lock);
}

static void job_result_taken(void) {
	pthread_mutex_lock(&job_lock);
	result_pending = false;
	pthread_cond_broadcast(&result_taken);
	pthread_mutex_unlock(&job_lock);
}

// ---------------------------------------------------------------------------
// the waiting veil
// ---------------------------------------------------------------------------

static void busy_show_text(bool with_text) {
	if (!busy_layer) {
		return;
	}
	if (busy_label) {
		if (with_text) {
			lv_obj_set_hidden(busy_label, false);
		} else {
			lv_obj_set_hidden(busy_label, true);
		}
	}
	lv_obj_set_hidden(busy_layer, false);
	if (!screensaver_is_visible()) {
		lv_obj_move_foreground(busy_layer);
	}
}

// Waiting for a list: the spinner says enough, and the list being replaced is
// what the user is looking at anyway.
static void busy_show(void) { busy_show_text(false); }

static void busy_hide(void) {
	if (busy_layer) {
		lv_obj_set_hidden(busy_layer, true);
	}
}

static void build_busy_layer(void) {
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

	// Under the spinner, because a spinner alone over a list that is still
	// there reads as "the page is stuck" rather than "your episode is coming".
	busy_label = lv_label_create(busy_layer);
	lv_label_set_text(busy_label, tr("loading"));
	lv_obj_set_style_text_color(busy_label, lv_color_white(), 0);
	lv_obj_set_style_text_font(busy_label, &font_ui_22, 0);
	lv_obj_set_style_margin_top(busy_label, 16, 0);
	lv_obj_set_hidden(busy_label, true);
}

// ---------------------------------------------------------------------------
// the row covers
//
// The loader has twelve slots and there can be sixty rows: they go to the rows
// that are VISIBLE and change hands as the list scrolls. The loader is Qobuz's
// and suits fine -- it names its images after the MD5 of the address, so two
// services cannot tread on each other even by accident (see qobuzart.h).
// ---------------------------------------------------------------------------

#define ROW_THUMB bp_pick(36, 60)

static lv_obj_t *list_container;
static lv_obj_t *list_title;
static lv_obj_t *list_empty;

// What is drawn right now, which is not the same as result_kind: result_* says
// what the worker is working on, and while an episode downloads that job is
// JOB_PLAY -- but the screen still holds the episode list with its tappable
// rows. Whatever answers a tap has to look at what is visible, not at what is
// being done. (qobuzpage.c does the same, under the same name.)
static job_kind_t list_kind;

// The podcast whose episodes are on screen: the folder a download goes into.
static char list_feed_title[PODCAST_TITLE_MAX];

// Paging, as in the radio: the last request came back full to the limit, so
// there may be more -- and one request at a time.
static bool page_more;
static bool page_loading;
static int list_count;

// The rows, of which there are twelve however long the list is.
//
// Scrolling a container costs time proportional to how many children it has,
// whether or not they are on screen, so a widget tree per entry -- up to two
// hundred and forty of them, some fifteen hundred LVGL objects -- makes the
// list crawl.
//
// So the widgets are a pool that slides over the data, exactly as the file
// browser and the track list do. Twelve for the six that fit, and the arithmetic
// that makes it work is uniform pitch: row `n` sits at `n * ROW_PITCH`, the
// first one to bind is `scroll / ROW_PITCH`, and the scrollable height is
// `count * ROW_PITCH` on an otherwise empty body object.
//
// Everything ABOUT an entry -- its cover, whether that cover failed, which row
// lent it -- stays indexed by the entry, not by the widget. The widget is only
// what draws it this second.
#define ROW_HEIGHT bp_pick(50, 88)
#define ROW_GAP bp_pick(6, 12)
#define ROW_PITCH (ROW_HEIGHT + ROW_GAP)
// The two lines of a row, at the sizes the Tidal tab settled on for this panel
// -- the row is the same thing on both pages, and two lines of 22/18 on a
// 240-px screen leave room for two rows where three and a half would fit.
#define ROW_TITLE_FONT (bp_is_tempotec_v1() ? &font_ui_18 : &font_ui_22)
#define ROW_DETAIL_FONT (bp_is_tempotec_v1() ? &font_ui_14 : &font_ui_18)
#define ROW_POOL 12

typedef struct {
	lv_obj_t *button;
	lv_obj_t *thumb;
	lv_obj_t *title;
	lv_obj_t *detail;
	lv_obj_t *menu_btn;
	// The tick an episode that has been heard wears. Built on every row and
	// shown only on the episode list, like the three dots above it.
	lv_obj_t *heard;
	// The mark of a chosen row in selection mode. Its own glyph rather than the
	// heard tick's: the two can be on the same row, and one of them means "this
	// one is already dealt with" while the other means "this one is picked".
	lv_obj_t *check;
	// The entry drawn on it: -1 for a row with nothing to show, and -2 for one
	// that must be redrawn even though its number has not changed -- the list
	// reloaded underneath it to the same length.
	int index;
} row_t;

static row_t rows[ROW_POOL];

// The object the rows are positioned inside. It holds no layout of its own: its
// only job is to be as tall as the whole list so the container has something to
// scroll.
static lv_obj_t *list_body;

static char row_cover_url[PODCAST_MAX_HELD][QOBUZART_URL_MAX];
static cover_image_t row_images[PODCAST_MAX_HELD];
static bool row_has_image[PODCAST_MAX_HELD];

// Which row the picture on this one belongs to, or -1 for a row showing the
// microphone. A row that owns its picture points at itself.
//
// Every episode of a podcast carries the same artwork and the catalogue repeats
// that one address on all of them, so asked row by row it would be hundreds of
// downloads and JPEG decodes of identical pictures on a single core. The first
// row to get a picture lends it to every other row asking for the same address.
//
// The rule is the address, not the kind of list: a show that does give its
// episodes their own artwork still gets it, and a search result listing the same
// podcast twice fetches its cover once.
static int row_image_source[PODCAST_MAX_HELD];

// This row has already asked for its cover and did not get one: dead address,
// image that will not decode, host that never answers. Without this memory the
// reload pass -- which restarts at every slot that frees up -- would ask again,
// and again, forever: one broken row would keep the loader busy and the good
// covers would never arrive.
static bool row_art_failed[PODCAST_MAX_HELD];

// The token that says "these slots are mine": only its address counts, never
// its value.
static const char podcast_art_owner;

static int slot_owner[QOBUZART_SLOTS];
static lv_timer_t *art_timer;

static bool art_any_pending(void) {
	for (int i = 0; i < QOBUZART_SLOTS; i++) {
		if (slot_owner[i] >= 0) {
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

static void art_release_slot(int slot) {
	if (slot < 0 || slot >= QOBUZART_SLOTS) {
		return;
	}
	qobuzart_release(&podcast_art_owner, slot);
	slot_owner[slot] = -1;
}

static void art_request_visible(void);

// The pooled widget drawing this entry right now, or NULL when the entry is off
// screen. Twelve comparisons, which is cheaper than keeping a map up to date.
static lv_obj_t *thumb_showing(int index) {
	for (int i = 0; i < ROW_POOL; i++) {
		if (rows[i].index == index) {
			return rows[i].thumb;
		}
	}
	return NULL;
}

// Puts the picture, or the microphone when there is none, on one pooled widget.
static void paint_thumb(lv_obj_t *thumb, int index) {
	if (!thumb) {
		return;
	}
	if (index >= 0 && row_has_image[index]) {
		lv_obj_remove_style(thumb, &theme_style_icon, 0);
		lv_obj_set_style_image_recolor_opa(thumb, LV_OPA_TRANSP, 0);
		lv_obj_set_style_radius(thumb, 6, 0);
		lv_obj_set_style_clip_corner(thumb, true, 0);
		lv_image_set_src(thumb, &row_images[row_image_source[index]].dsc);
		return;
	}
	// The microphone, not the musical note: a podcast row without a cover is
	// not a track. The icon is the one supplied by the user (podcast-list.svg).
	lv_image_set_src(thumb, &icon_podcast_list);
	lv_obj_add_style(thumb, &theme_style_icon, 0);
	lv_obj_set_style_image_recolor_opa(thumb, LV_OPA_COVER, 0);
	lv_obj_set_style_clip_corner(thumb, false, 0);
	lv_obj_set_style_radius(thumb, 0, 0);
}

// Records that the picture belonging to `owner` is the one for `row`, and draws
// it if that row is on screen. The two are the same row when it has just come
// out of the decoder.
static void show_cover(int row, int owner) {
	if (row != owner) {
		// It may already have asked for a copy of its own; that copy is now
		// pointless and the slot is worth more to a row that has nothing.
		for (int slot = 0; slot < QOBUZART_SLOTS; slot++) {
			if (slot_owner[slot] == row) {
				art_release_slot(slot);
			}
		}
	}
	row_has_image[row] = true;
	row_image_source[row] = owner;
	paint_thumb(thumb_showing(row), row);
}

// The row that owns a picture, or -1 when nobody has this address yet.
static int cover_owner_for(const char *url) {
	if (!url[0]) {
		return -1;
	}
	for (int row = 0; row < PODCAST_MAX_HELD; row++) {
		if (row_has_image[row] && row_image_source[row] == row && strcmp(row_cover_url[row], url) == 0) {
			return row;
		}
	}
	return -1;
}

// A picture has just arrived: every row waiting for the same address gets it
// now, including the ones off screen, because the cost of handing over a
// pointer is nothing and it stops them from ever asking.
static void lend_cover(int owner) {
	for (int row = 0; row < PODCAST_MAX_HELD; row++) {
		if (row == owner || row >= list_count || row_has_image[row]) {
			continue;
		}
		if (strcmp(row_cover_url[row], row_cover_url[owner]) == 0) {
			show_cover(row, owner);
		}
	}
}

// And the same for a failure: an address that would not download will not
// download for the other rows either, and without this the one broken cover of
// a podcast is retried once per episode.
static void fail_cover(int owner) {
	row_art_failed[owner] = true;
	for (int row = 0; row < PODCAST_MAX_HELD; row++) {
		if (row != owner && !row_has_image[row] && row_cover_url[row][0] &&
			strcmp(row_cover_url[row], row_cover_url[owner]) == 0) {
			row_art_failed[row] = true;
		}
	}
}

static void art_poll_cb(lv_timer_t *timer) {
	bool freed = false;
	if (!art_any_pending()) {
		lv_timer_pause(timer); // nothing in flight: art_wake() starts it again
		return;
	}

	for (int slot = 0; slot < QOBUZART_SLOTS; slot++) {
		int row = slot_owner[slot];
		if (row < 0 || row >= PODCAST_MAX_HELD) {
			continue;
		}
		cover_image_t image;
		bool finished = false;
		if (!qobuzart_take(&podcast_art_owner, slot, &image, &finished)) {
			if (finished) {
				// Failed: this address is never requested again.
				fail_cover(row);
				freed = true;
				slot_owner[slot] = -1;
			}
			continue;
		}

		cover_free(&row_images[row]);
		row_images[row] = image;
		show_cover(row, row);
		lend_cover(row);

		slot_owner[slot] = -1;
		freed = true;
	}

	// A slot that frees up goes straight back out to someone.
	//
	// There are more rows than slots, so art_request_visible() hands out what
	// it can and returns; without this pass the rest would only be asked for
	// again on the next scroll, and never at all if the last scroll happened
	// while every slot was busy.
	if (freed) {
		art_request_visible();
	}

}

// The covers wanted right now, which is the same set as the rows that exist:
// the pool IS the visible band, so there is nothing left to test for
// visibility.
static void art_request_visible(void) {
	for (int i = 0; i < ROW_POOL; i++) {
		int row = rows[i].index;
		if (row < 0 || row >= list_count || row_has_image[row] || row_art_failed[row] || !row_cover_url[row][0]) {
			continue;
		}

		// Already downloaded for another row of the same podcast: hand it over
		// rather than spend a slot, a download and a decode on a copy of it.
		int owner = cover_owner_for(row_cover_url[row]);
		if (owner >= 0) {
			show_cover(row, owner);
			continue;
		}

		bool queued = false;
		for (int slot = 0; slot < QOBUZART_SLOTS && !queued; slot++) {
			queued = slot_owner[slot] == row;
		}
		if (queued) {
			continue;
		}

		int free_slot = -1;
		for (int slot = 0; slot < QOBUZART_SLOTS && free_slot < 0; slot++) {
			if (slot_owner[slot] < 0) {
				free_slot = slot;
			}
		}
		if (free_slot < 0) {
			return;
		}

		slot_owner[free_slot] = row;
		art_wake();
		qobuzart_request(&podcast_art_owner, "podcast-cache", free_slot, row_cover_url[row], ROW_THUMB);
	}
}

static void maybe_load_more(void);
static void row_detail(int index, char *out, size_t size);
static bool kind_has_row_menu(job_kind_t kind);

// ---------------------------------------------------------------------------
// which order the episodes are shown in
//
// The catalogue answers newest first and the Saved page sorts by date the same
// way. Oldest first is the same list backwards -- nothing is re-queried, and
// the queue built from a tap still walks towards older episodes, whichever way
// the list is drawn (see JOB_PLAY).
//
// Kept in the config rather than in this page: the Saved list of downloaded
// episodes reads it too.
// ---------------------------------------------------------------------------

bool podcastpage_episodes_newest_first(void) {
	return config_get_int("podcast", "newest_first", 1) != 0;
}

void podcastpage_set_episodes_newest_first(bool newest) {
	config_set_int("podcast", "newest_first", newest ? 1 : 0);
	config_save();
}

// ---------------------------------------------------------------------------
// which order the followed podcasts are shown in
//
// Most listened first is the default: the podcasts somebody keeps up with are
// the ones at the top of the list, rather than whichever was followed most
// recently -- which is what the list used to say and what a list of a hundred
// subscriptions cannot answer at a glance.
//
// "Listened" is counted per podcast (podcastsubs_count_play), not read off the
// heard marks, because those are keyed by episode and cannot say which podcast
// an episode belonged to. A count is also the only thing that ranks a podcast
// somebody listens to ten minutes of every day above one they tried once and
// finished.
// ---------------------------------------------------------------------------

podcast_sort_t podcastpage_subs_sort(void) {
	int order = config_get_int("podcast", "subs_sort", (int)PODCAST_SORT_PLAYED);
	if (order < PODCAST_SORT_PLAYED || order > PODCAST_SORT_ZA) {
		order = PODCAST_SORT_PLAYED;
	}
	return (podcast_sort_t)order;
}

void podcastpage_set_subs_sort(podcast_sort_t order) {
	if (order < PODCAST_SORT_PLAYED || order > PODCAST_SORT_ZA) {
		order = PODCAST_SORT_PLAYED;
	}
	config_set_int("podcast", "subs_sort", (int)order);
	config_save();
}

// Two subscriptions, in the order the setting asks for. Counts come from the
// list already in memory, so this costs nothing per comparison.
static int compare_feeds(const podcast_feed_t *a, const podcast_feed_t *b, podcast_sort_t order) {
	if (order == PODCAST_SORT_PLAYED) {
		int played_a = podcastsubs_plays(a->id);
		int played_b = podcastsubs_plays(b->id);
		if (played_a != played_b) {
			return played_b - played_a; // most listened first
		}
	} else if (order == PODCAST_SORT_ZA) {
		return -strcasecmp(a->title, b->title);
	} else {
		return strcasecmp(a->title, b->title);
	}
	// Level on plays: the alphabet decides, so the order is never arbitrary.
	return strcasecmp(a->title, b->title);
}

// Insertion sort: a hundred entries at most, and the list is almost always
// already in this order from the last time it was drawn.
static void sort_feeds(podcast_feed_t *list, int n) {
	podcast_sort_t order = podcastpage_subs_sort();
	for (int i = 1; i < n; i++) {
		podcast_feed_t feed = list[i];
		int j = i - 1;
		while (j >= 0 && compare_feeds(&list[j], &feed, order) > 0) {
			list[j + 1] = list[j];
			j--;
		}
		list[j + 1] = feed;
	}
}

// The followed list as the setting wants it: read from the card, put in order.
static int load_followed(podcast_feed_t *out, int max) {
	int n = podcastsubs_list(out, max);
	sort_feeds(out, n);
	return n;
}

static void reverse_episodes(podcast_episode_t *list, int n) {
	for (int i = 0, j = n - 1; i < j; i++, j--) {
		podcast_episode_t swap = list[i];
		list[i] = list[j];
		list[j] = swap;
	}
}

// ---------------------------------------------------------------------------
// choosing several rows at once
//
// The same mode the file browser and the track lists have: a long press (here,
// through the row's own menu) starts it, a tap then chooses a row or lets it
// go, letting go of the last one ends it, and the corner trades its buttons for
// the ones that act on what is chosen.
//
// What can be done in bulk is what can be done to one row, with the exceptions
// the section already has: a download is one at a time by design (podcastdl.h
// runs a single thread for the whole device), so a selection of episodes offers
// the heard mark instead -- the chore people actually have, clearing a backlog.
// ---------------------------------------------------------------------------

// The row an episode's long press was opened on. Declared here because the
// selection's own entry point -- the menu's "Select" -- is written before the
// download card that owns the variable.
static int dl_row;

static bool selecting;
// One flag per entry of the list on screen, or NULL when nothing is chosen.
// Allocated when the mode starts and freed when it ends: the list can be fifty
// entries or six hundred, and the array is dead outside the mode.
static unsigned char *chosen;
static int chosen_count;

static void refresh_corner_buttons(void);
static void paint_selection_actions(void);

static void row_update_selection(row_t *row) {
	if (!row->check) {
		return;
	}
	bool on = selecting && chosen && row->index >= 0 && row->index < list_count && chosen[row->index];
	if (row->menu_btn) {
		lv_obj_set_hidden(row->menu_btn, selecting || !kind_has_row_menu(list_kind));
	}
	if (on) {
		lv_obj_set_style_image_recolor(row->check, theme()->accent, 0);
		lv_obj_set_hidden(row->check, false);
	} else {
		lv_obj_set_hidden(row->check, true);
	}
}

static void rows_update_selection(void) {
	for (int i = 0; i < ROW_POOL; i++) {
		row_update_selection(&rows[i]);
	}
}

static void selection_stop(void) {
	if (!selecting) {
		return;
	}
	selecting = false;
	free(chosen);
	chosen = NULL;
	chosen_count = 0;
	refresh_corner_buttons();
	rows_update_selection();
}

static bool selection_start(void) {
	if (selecting) {
		return true;
	}
	if (list_count <= 0) {
		return false;
	}
	chosen = calloc((size_t)list_count, 1);
	if (!chosen) {
		return false;
	}
	selecting = true;
	chosen_count = 0;
	refresh_corner_buttons();
	rows_update_selection();
	return true;
}

// Chooses the entry, or lets it go. Letting go of the last one leaves the mode.
static void selection_toggle(int index) {
	if (!selecting || !chosen || index < 0 || index >= list_count) {
		return;
	}
	chosen[index] = !chosen[index];
	if (chosen[index]) {
		chosen_count++;
	} else if (--chosen_count == 0) {
		selection_stop();
		return;
	}
	for (int i = 0; i < ROW_POOL; i++) {
		if (rows[i].index == index) {
			row_update_selection(&rows[i]);
		}
	}
	// The corner's tick is disabled until something is chosen, so it has to be
	// told when the first row goes in and when the last one comes out.
	paint_selection_actions();
}

// Puts entry `index` on one pooled widget, or hides it when there is no entry.
// Nothing is created here: the widgets were built once and only ever change
// what they say and where they sit.
static void row_bind(row_t *row, int index) {
	if (row->index == index) {
		return;
	}

	// The slot this widget's old entry was waiting on. Letting it run would
	// fill a cover nobody is looking at while a visible row waits for a slot --
	// and if it is an address some other row also wants, that row asks for it
	// itself.
	int leaving = row->index;
	if (leaving >= 0 && !row_has_image[leaving]) {
		for (int slot = 0; slot < QOBUZART_SLOTS; slot++) {
			if (slot_owner[slot] == leaving) {
				art_release_slot(slot);
			}
		}
	}

	row->index = index;

	if (index < 0 || index >= list_count) {
		lv_obj_set_hidden(row->button, true);
		return;
	}

	lv_obj_set_hidden(row->button, false);
	lv_obj_set_y(row->button, index * ROW_PITCH);

	const char *title =
		list_kind == JOB_EPISODES ? result_episodes[index].title : result_feeds[index].title;
	lv_label_set_text(row->title, title);

	char detail[200];
	row_detail(index, detail, sizeof(detail));
	if (detail[0]) {
		lv_label_set_text(row->detail, detail);
		lv_obj_set_hidden(row->detail, false);
	} else {
		lv_obj_set_hidden(row->detail, true);
	}

	// The same pooled button serves both kinds of list: feed rows offer
	// follow/unfollow, and episode rows offer heard state and download.
	if (kind_has_row_menu(list_kind)) {
		lv_obj_set_hidden(row->menu_btn, false);
	} else {
		lv_obj_set_hidden(row->menu_btn, true);
	}

	// Heard: the tick says so, and the text steps back so a list of them reads
	// as a list of what is left. Only episodes can be heard, so the rows of a
	// podcast list are never dimmed by this.
	bool heard = list_kind == JOB_EPISODES && podcastplayed_get(result_episodes[index].id);
	lv_obj_set_hidden(row->heard, !heard);
	lv_obj_set_style_text_opa(row->title, heard ? LV_OPA_50 : LV_OPA_COVER, 0);
	lv_obj_set_style_text_opa(row->detail, heard ? LV_OPA_50 : LV_OPA_COVER, 0);

	paint_thumb(row->thumb, index);
	row_update_selection(row);
}

// Slides the pool to wherever the list has been scrolled to.
static void window_update(void) {
	if (!list_container) {
		return;
	}

	int scroll = lv_obj_get_scroll_y(list_container);
	if (scroll < 0) {
		scroll = 0; // the elastic overscroll at the top
	}

	// One row of overdraw above, so a row arriving from the top is already
	// drawn rather than appearing as the gap reaches it.
	int first = (scroll / ROW_PITCH) - 1;
	if (first + ROW_POOL > list_count) {
		first = list_count - ROW_POOL;
	}
	if (first < 0) {
		first = 0;
	}

	// The widget for entry n is always rows[n % ROW_POOL], which is what makes
	// a one-row slide rebind one widget instead of all twelve.
	for (int i = 0; i < ROW_POOL; i++) {
		int index = first + i;
		row_bind(&rows[index % ROW_POOL], index < list_count ? index : -1);
	}

	art_request_visible();
}

static void list_scrolled_cb(lv_event_t *e) {
	(void)e;
	window_update();
	maybe_load_more();
}

// One entry changed under the rows -- heard, or heard-and-not: bind the row
// that is showing it again, and only that one. The other eleven keep theirs.
static void refresh_row(int index) {
	if (index < 0) {
		return;
	}
	for (int i = 0; i < ROW_POOL; i++) {
		if (rows[i].index == index) {
			rows[i].index = -2;
		}
	}
	window_update();
}

void podcastpage_episode_heard(long long episode_id) {
	// Only the list that draws the mark, and only while it is the one on
	// screen: coming back to a list binds every row again, so a page that is
	// not showing needs nothing done to it.
	if (episode_id <= 0 || !list_container || list_kind != JOB_EPISODES ||
		lv_screen_active() != list_screen) {
		return;
	}
	for (int i = 0; i < list_count; i++) {
		if (result_episodes[i].id == episode_id) {
			refresh_row(i);
			return;
		}
	}
}

static void art_forget_rows(void) {
	for (int slot = 0; slot < QOBUZART_SLOTS; slot++) {
		art_release_slot(slot);
	}
	// Nothing may still be pointing at the pixels about to go: the widgets stay
	// alive across a list change, so they are put back on the microphone first
	// and freed afterwards.
	for (int i = 0; i < ROW_POOL; i++) {
		if (rows[i].button) {
			lv_obj_set_hidden(rows[i].button, true);
			paint_thumb(rows[i].thumb, -1);
		}
		rows[i].index = -1;
	}
	for (int row = 0; row < PODCAST_MAX_HELD; row++) {
		row_cover_url[row][0] = '\0';
		row_has_image[row] = false;
		row_art_failed[row] = false;
		row_image_source[row] = -1;
		// Only an owner has pixels of its own; a row that borrowed one holds a
		// zeroed image, and freeing that is nothing.
		cover_free(&row_images[row]);
	}
}

// Gives the decoded thumbnails back without taking the list apart: the entries
// stay, so coming back to the page finds the list where it was left and only
// the pictures have to be fetched again.
//
// row_art_failed is deliberately NOT cleared: a cover that would not download
// still will not, and forgetting that would send the same failing requests
// again at every visit.
static void art_drop_pictures(void) {
	for (int slot = 0; slot < QOBUZART_SLOTS; slot++) {
		art_release_slot(slot);
	}
	for (int row = 0; row < PODCAST_MAX_HELD; row++) {
		row_has_image[row] = false;
		row_image_source[row] = -1;
		cover_free(&row_images[row]);
	}
	// Back to the microphone, after the state says there is no picture and
	// before anything can draw the freed pixels.
	for (int i = 0; i < ROW_POOL; i++) {
		paint_thumb(rows[i].thumb, rows[i].index);
	}
}

// Two hundred and forty rows of 60x60 is 1.7 MB, held until the list is
// rebuilt, so the pictures are given back a while after the page is left. Not
// the moment it is left: tapping an episode goes to the player and coming
// straight back is the ordinary way to use this page, and freeing on the way out
// would re-read and re-decode a dozen pictures every time something is played.
#define ART_DROP_DELAY_MS 20000

static lv_timer_t *art_drop_timer;

static void art_drop_cb(lv_timer_t *timer) {
	lv_timer_pause(timer);
	art_drop_pictures();

	// And back to the kernel, not merely back to the arena. A thumbnail is
	// 7200 bytes, well under the 96 kB mmap threshold main.c sets, so the
	// thumbnails come out of the heap rather than out of their own mappings
	// and freeing them alone leaves the arena holding the space.
	malloc_trim(0);
}

static void list_loaded_cb(lv_event_t *e) {
	(void)e;
	if (art_drop_timer) {
		lv_timer_pause(art_drop_timer); // back before the wait ran out
	}
	if (!list_container) {
		return; // the screen exists before the page inside it does
	}
	// Whatever was given back while the page was away is asked for again. Free
	// when nothing was dropped: every row still holding its picture is skipped.
	art_wake();
	// Every row is bound again, not only the ones whose number moved: an
	// episode played while the player was up has become one heard, and coming
	// back to the list has to show that. Twelve binds are nothing.
	for (int i = 0; i < ROW_POOL; i++) {
		rows[i].index = -2;
	}
	window_update();
}

static void list_unloaded_cb(lv_event_t *e) {
	(void)e;
	if (art_drop_timer) {
		lv_timer_reset(art_drop_timer);
		lv_timer_resume(art_drop_timer);
	}
}

// ---------------------------------------------------------------------------
// the rows
// ---------------------------------------------------------------------------

// The three-dot menu carries the single-row actions for feeds and episodes.
static bool kind_has_row_menu(job_kind_t kind) {
	return kind == JOB_SEARCH || kind == JOB_TRENDING || kind == JOB_FOLLOWED || kind == JOB_EPISODES;
}

static void row_menu_cb(lv_event_t *e);
static void episode_menu_cb(lv_event_t *e);
static void row_long_cb(lv_event_t *e);

// One pooled button serves both kinds of list, so it needs one callback that
// knows which kind is on screen. It used to carry the episode menu only: on a
// list of podcasts the dots were drawn, lit, and did nothing at all.
static void row_menu_dispatch_cb(lv_event_t *e) {
	if (!kind_has_row_menu(list_kind)) {
		return;
	}
	if (list_kind == JOB_EPISODES) {
		episode_menu_cb(e);
	} else {
		row_menu_cb(e);
	}
}

// The twelve rows, built once. Everything that can differ between one entry and
// the next is created here and shown or hidden at bind time: a recycled widget
// cannot grow a subtitle it was not born with.
static void build_rows(int width) {
	for (int i = 0; i < ROW_POOL; i++) {
		row_t *row = &rows[i];

		row->button = lv_btn_create(list_body);
		lv_obj_set_size(row->button, width, ROW_HEIGHT);
		lv_obj_set_x(row->button, 0);
		lv_obj_add_style(row->button, &theme_style_card, 0);
		lv_obj_add_style(row->button, &theme_style_card_pressed, LV_STATE_PRESSED);
		lv_obj_set_style_radius(row->button, bp_pick(10, 12), 0);
		lv_obj_set_style_border_width(row->button, 0, 0);
		lv_obj_set_style_shadow_width(row->button, 0, 0);
		lv_obj_set_style_pad_hor(row->button, bp_pick(6, 16), 0);
		lv_obj_set_style_pad_ver(row->button, 0, 0);
		lv_obj_set_style_pad_column(row->button, bp_pick(6, 8), 0);
		lv_obj_set_scrollable(row->button, false);
		lv_obj_set_hidden(row->button, true);
		lv_obj_set_flex_flow(row->button, LV_FLEX_FLOW_ROW);
		lv_obj_set_flex_align(row->button, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
		// Gestures (back, and pulling the player in) often start on a row, and a
		// button keeps the press to itself.
		lv_obj_set_event_bubble(row->button, true);
		lv_obj_add_event_cb(row->button, row_clicked_cb, LV_EVENT_CLICKED, NULL);
		lv_obj_add_event_cb(row->button, row_long_cb, LV_EVENT_LONG_PRESSED, NULL);

		// The thumbnail is always there, even when the catalogue gives no
		// address: a row without it comes up with nothing on the left, text
		// against the edge and out of line with every other row. With no
		// address the grey microphone stays, which is the honest answer --
		// there is no cover.
		row->thumb = lv_image_create(row->button);
		lv_obj_set_size(row->thumb, ROW_THUMB, ROW_THUMB);
		lv_image_set_inner_align(row->thumb, LV_IMAGE_ALIGN_CENTER);
		lv_obj_set_event_bubble(row->thumb, true);
		paint_thumb(row->thumb, -1);

		lv_obj_t *texts = lv_obj_create(row->button);
		lv_obj_set_flex_grow(texts, 1);
		lv_obj_set_height(texts, LV_SIZE_CONTENT);
		lv_obj_set_style_bg_opa(texts, 0, 0);
		lv_obj_set_style_border_width(texts, 0, 0);
		lv_obj_set_style_pad_all(texts, 0, 0);
		lv_obj_set_style_pad_row(texts, bp_pick(2, 6), 0);
		lv_obj_set_scrollable(texts, false);
		lv_obj_set_event_bubble(texts, true);
		lv_obj_set_flex_flow(texts, LV_FLEX_FLOW_COLUMN);
		lv_obj_set_flex_align(texts, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

		row->title = lv_label_create(texts);
		lv_label_set_long_mode(row->title, LV_LABEL_LONG_DOT);
		lv_obj_set_width(row->title, lv_pct(100));
		lv_obj_set_height(row->title, lv_font_get_line_height(ROW_TITLE_FONT));
		lv_obj_add_style(row->title, &theme_style_text, 0);
		lv_obj_set_style_text_font(row->title, ROW_TITLE_FONT, 0);

		row->detail = lv_label_create(texts);
		lv_label_set_long_mode(row->detail, LV_LABEL_LONG_DOT);
		lv_obj_set_width(row->detail, lv_pct(100));
		lv_obj_set_height(row->detail, lv_font_get_line_height(ROW_DETAIL_FONT));
		lv_obj_add_style(row->detail, &theme_style_text_dim, 0);
		lv_obj_set_style_text_font(row->detail, ROW_DETAIL_FONT, 0);

		// The tick of an episode that has been heard. Between the text and the
		// three dots, because a podcast row shows no dots and an episode row
		// shows no dots either: whichever list this row is bound to, the tick
		// has the end of the row to itself.
		row->heard = lv_image_create(row->button);
		lv_obj_set_size(row->heard, 32, 32);
		lv_image_set_src(row->heard, &icon_check);
		lv_obj_add_style(row->heard, &theme_style_icon, 0);
		// Half strength, and only the opacity: the colour is the theme's own
		// and follows a theme change, which a colour taken here would not.
		lv_obj_set_style_image_recolor_opa(row->heard, LV_OPA_50, 0);
		lv_image_set_scale(row->heard, 156); // 36 px source -> about 22 px
		lv_obj_set_event_bubble(row->heard, true);
		lv_obj_set_hidden(row->heard, true);

		// The selection mark, at the same end of the row as the tick and the
		// dots: it is the last thing a finger passes on its way off the row.
		row->check = lv_image_create(row->button);
		lv_obj_set_size(row->check, 32, 32);
		// The plain tick the file browser puts on a chosen row: the circled one
		// this used to show was a 36 px ring around its own tick, and at 22 px
		// the two ran into each other and read as a smudge.
		lv_image_set_src(row->check, &icon_check);
		lv_obj_add_style(row->check, &theme_style_icon, 0);
		lv_obj_set_style_image_recolor_opa(row->check, LV_OPA_COVER, 0);
		lv_image_set_scale(row->check, 156); // 36 px source -> about 22 px
		lv_obj_set_event_bubble(row->check, true);
		lv_obj_set_hidden(row->check, true);

		row->menu_btn = lv_btn_create(row->button);
		lv_obj_set_size(row->menu_btn, bp_pick(30, 44), bp_pick(30, 44));
		lv_obj_set_style_bg_opa(row->menu_btn, LV_OPA_TRANSP, 0);
		lv_obj_set_style_border_width(row->menu_btn, 0, 0);
		lv_obj_set_style_shadow_width(row->menu_btn, 0, 0);
		lv_obj_set_style_pad_all(row->menu_btn, 0, 0);
		lv_obj_set_hidden(row->menu_btn, true);
		// Two dots are a narrow target inside a row that plays something when
		// it is tapped: the grace around the button is what keeps a tap that
		// lands a pixel off from turning into an episode starting.
		lv_obj_set_ext_click_area(row->menu_btn, bp_pick(7, 10));
		lv_obj_add_event_cb(row->menu_btn, row_menu_dispatch_cb, LV_EVENT_CLICKED, NULL);

		lv_obj_t *dots = lv_image_create(row->menu_btn);
		lv_image_set_src(dots, &icon_ellipsis_vertical);
		lv_obj_add_style(dots, &theme_style_icon, 0);
		lv_obj_set_style_image_recolor_opa(dots, LV_OPA_COVER, 0);
		lv_image_set_scale(dots, (uint32_t)(LV_SCALE_NONE * 20 / 36));
		lv_obj_center(dots);

		row->index = -1;
	}
}

// Which entry a pressed widget is showing. The number cannot be carried in the
// callback's user data: the widget that is entry 3 one moment is entry 51 the
// next.
static int row_index_of(const lv_obj_t *button) {
	for (int i = 0; i < ROW_POOL; i++) {
		if (rows[i].button == button) {
			return rows[i].index;
		}
	}
	return -1;
}

static int menu_index_of(const lv_obj_t *menu_btn) {
	for (int i = 0; i < ROW_POOL; i++) {
		if (rows[i].menu_btn == menu_btn) {
			return rows[i].index;
		}
	}
	return -1;
}

// "1 h 12 min", "43 min", "" when the catalogue does not say.
static void format_duration(int seconds, char *out, size_t size) {
	if (seconds <= 0) {
		out[0] = '\0';
		return;
	}
	int minutes = (seconds + 30) / 60;
	if (minutes < 60) {
		snprintf(out, size, "%d min", minutes);
	} else {
		snprintf(out, size, "%d h %02d min", minutes / 60, minutes % 60);
	}
}

// "12 March 2026". Written by hand rather than with strftime because strftime
// follows the SYSTEM language, which on this device never changes, while the
// interface language is the user's own choice.
static void format_date(long unix_secs, char *out, size_t size) {
	static const char *const PODCAST_MONTHS[12] = {"podcast_january", "podcast_february", "podcast_march",	   "podcast_april",   "podcast_may",   "podcast_june",
										   "podcast_july",  "podcast_august",	  "podcast_september", "podcast_october",  "podcast_november", "podcast_december"};
	if (unix_secs <= 0) {
		out[0] = '\0';
		return;
	}
	time_t t = (time_t)unix_secs;
	struct tm tm_when;
	if (!localtime_r(&t, &tm_when)) {
		out[0] = '\0';
		return;
	}
	snprintf(out, size, "%d %s %d", tm_when.tm_mday, tr(PODCAST_MONTHS[tm_when.tm_mon % 12]), tm_when.tm_year + 1900);
}

// The line under an entry's name: the date and length of an episode, the author
// and episode count of a podcast. Built at bind time rather than kept for every
// entry -- it is twelve short strings on screen, not two hundred and forty in
// memory.
static void row_detail(int index, char *out, size_t size) {
	out[0] = '\0';
	if (index < 0 || index >= list_count) {
		return;
	}
	if (list_kind == JOB_EPISODES) {
		const podcast_episode_t *e = &result_episodes[index];
		char when[48];
		char duration[32];
		format_date(e->published, when, sizeof(when));
		format_duration(e->duration_secs, duration, sizeof(duration));
		if (when[0] && duration[0]) {
			snprintf(out, size, "%s \xC2\xB7 %s", when, duration);
		} else {
			snprintf(out, size, "%s%s", when, duration);
		}
		return;
	}
	const podcast_feed_t *f = &result_feeds[index];
	if (f->episode_count > 0 && f->author[0]) {
		snprintf(out, size, tr("podcast_episodes"), f->author, f->episode_count);
	} else if (f->episode_count > 0) {
		snprintf(out, size, tr("podcast_episodes_2"), f->episode_count);
	} else {
		snprintf(out, size, "%s", f->author);
	}
}

// `from` is the first NEW entry: 0 is a different list, a higher value is
// another page of the same one appended at the end. No widget is created or
// destroyed either way -- the list is only as long as the body object says it
// is, and the pool slides over it.
static void fill_list(int from) {
	list_kind = result_kind;
	list_count = result_count;

	// The flags describe a list that is being replaced: entry 3 of this one is
	// not entry 3 of the next. A list that merely grows keeps them.
	if (from == 0 && selecting) {
		selection_stop();
	}

	// Oldest first puts a new page at the FRONT: the episodes a longer answer
	// added are the older ones. The rows were drawn from the end of the array
	// before, so the whole pool is bound again -- and the view is moved down by
	// as many rows as went in above it, or the reader would be thrown back to
	// the beginning of the list they had scrolled through.
	int prepended = 0;
	if (from > 0 && list_kind == JOB_EPISODES && !podcastpage_episodes_newest_first()) {
		prepended = list_count - from;
		from = 0;
	}
	int was_scrolled = prepended > 0 ? (int)lv_obj_get_scroll_y(list_container) : 0;

	if (from == 0) {
		art_forget_rows();
	}
	if (list_kind == JOB_EPISODES) {
		snprintf(list_feed_title, sizeof(list_feed_title), "%s", result_title);
	}

	// The addresses of every entry, on screen or not, because the lending looks
	// for a row that already holds this address and the row that holds it is
	// usually one nobody can see.
	for (int i = from; i < result_count && i < PODCAST_MAX_HELD; i++) {
		const char *url = list_kind == JOB_EPISODES ? result_episodes[i].image : result_feeds[i].image;
		snprintf(row_cover_url[i], sizeof(row_cover_url[0]), "%s", url ? url : "");
	}

	// The entries under the pool have changed even where their numbers have
	// not: a search that returns the same count as the last one would otherwise
	// leave every row saying what it said before.
	for (int i = 0; i < ROW_POOL; i++) {
		rows[i].index = -2;
	}
	lv_obj_set_height(list_body, list_count > 0 ? list_count * ROW_PITCH : ROW_PITCH);

	if (from == 0) {
		lv_obj_scroll_to_y(list_container, 0, LV_ANIM_OFF);
	}
	window_update();
	if (prepended > 0) {
		lv_obj_scroll_to_y(list_container, was_scrolled + prepended * ROW_PITCH, LV_ANIM_OFF);
		window_update();
	}

	// The pills only on the two lists they name. The title says "Podcasts"
	// there, because the lit pill already says which of the two is on screen;
	// elsewhere it says what is on screen, which is the query or the podcast's
	// name.
	bool home_view = list_kind == JOB_FOLLOWED || list_kind == JOB_TRENDING;
	if (pill_row) {
		if (home_view) {
			lv_obj_set_hidden(pill_row, false);
		} else {
			lv_obj_set_hidden(pill_row, true);
		}
	}
	// The magnifier and the gear belong to the SECTION, not to what is inside
	// it: on search results and on a podcast's episodes they go away together
	// with the pills.
	refresh_corner_buttons();
	paint_pill(pill_followed, list_kind == JOB_FOLLOWED);
	paint_pill(pill_trending, list_kind == JOB_TRENDING);
	if (list_title) {
		lv_label_set_text(list_title, home_view ? tr("podcasts") : result_title);
		settingsrow_title_realign(list_title);
	}

	if (list_count == 0) {
		// The empty notice says what to do when there is something to do. It is
		// where the hint lives: under the notice it appears only to someone
		// whose list really is empty, rather than as a subtitle that stays over
		// a full one.
		lv_label_set_text(list_empty,
						  list_kind == JOB_FOLLOWED ? tr("podcast_none_followed") : tr("nothing_to_show"));
		lv_obj_set_hidden(list_empty, false);
	} else {
		lv_obj_set_hidden(list_empty, true);
	}

	// A result can land after a notice has taken the page over -- the request
	// was already on the wire when the Wi-Fi went. The list is filled anyway,
	// so the page is whole once the notice goes, but it stays out of sight
	// underneath it instead of half showing through.
	if (note_label && !lv_obj_is_hidden(note_label)) {
		lv_obj_set_hidden(pill_row, true);
		lv_obj_set_hidden(list_container, true);
		lv_obj_set_hidden(list_empty, true);
	}
}

// ---------------------------------------------------------------------------
// the list history
//
// Every list lives on ONE screen, so the global screen manager has nothing to
// stack: this page keeps the stack itself. Taken from tidalpage.c: a step is
// committed only once the result has really arrived, since committing it at
// request time would leave the history pointing at a page nobody ever saw.
// ---------------------------------------------------------------------------

#define LIST_HISTORY_DEPTH 8
static job_t list_history[LIST_HISTORY_DEPTH];
static int list_history_len;
static job_t current_list_job;
static bool have_current_list;

static job_t inflight_job;
static bool inflight_valid;
static bool inflight_is_back; // a back step consumes a rung instead of adding one
static bool inflight_is_root; // a pill: the stack restarts from there

static bool job_is_list(job_kind_t kind) { return kind != JOB_PLAY && kind != JOB_NONE; }

static bool same_list(const job_t *a, const job_t *b) {
	return a->kind == b->kind && a->id == b->id && strcmp(a->text, b->text) == 0;
}

static void list_nav_drop(void) { inflight_valid = false; }

static void list_nav_commit(void) {
	if (!inflight_valid) {
		return;
	}
	inflight_valid = false;

	if (inflight_is_back) {
		if (list_history_len > 0) {
			list_history_len--;
		}
		current_list_job = inflight_job;
		have_current_list = true;
		return;
	}

	// A pill is the section's root, not a step into something. Going from "My
	// podcasts" to "Trending" and then back must leave the section, not bounce
	// between the two.
	if (inflight_is_root) {
		list_history_len = 0;
		current_list_job = inflight_job;
		have_current_list = true;
		return;
	}

	// Entering from outside: the stack restarts here.
	//
	// The search screen is NOT "outside": it is a keyboard sitting on top of
	// this page, and the results it brings are a step INSIDE the list that was
	// being looked at. Without this second condition a search would clear the
	// stack, and the chevron from the results would leave the section instead
	// of returning to the pills.
	lv_obj_t *active = lv_screen_active();
	if (active != list_screen && active != search_screen) {
		list_history_len = 0;
		current_list_job = inflight_job;
		have_current_list = true;
		return;
	}

	// The same list reloaded is not a step forward.
	if (have_current_list && same_list(&current_list_job, &inflight_job)) {
		return;
	}

	if (have_current_list && list_history_len < LIST_HISTORY_DEPTH) {
		list_history[list_history_len++] = current_list_job;
	}
	current_list_job = inflight_job;
	have_current_list = true;
}

static void run(const job_t *job);
static void start_worker(void);
static void busy_show(void);

// The back step is prepared by hand rather than through run(), so that
// inflight_is_back stays raised until list_nav_commit() reads it. run() only
// hands the job to the worker; the commit happens much later, when the result
// comes back from the network, and a flag lowered straight after run() would
// have the back step stacked as a step forward. tidalpage.c does it the same
// way.
// The guard's peek, for the back swipe: is there a rung to consume inside the
// page? It moves nothing -- it only keeps another screen's page from being
// drawn under the finger.
static bool list_back_has_step(void) { return list_history_len > 0; }

static bool list_back_guard(void) {
	if (list_history_len == 0) {
		have_current_list = false;
		return false; // the stack is empty: leave the page
	}
	// The top step is PEEKED at and redone; the stack only shortens once the
	// result has arrived (list_nav_commit).
	job_t previous = list_history[list_history_len - 1];
	inflight_job = previous;
	inflight_valid = true;
	inflight_is_back = true;
	inflight_is_root = false;
	page_more = false;
	page_loading = false;
	start_worker();
	if (previous.kind != JOB_FOLLOWED) {
		busy_show();
	}
	submit(&previous);
	return true;
}

// ---------------------------------------------------------------------------
// starting a job
// ---------------------------------------------------------------------------

static void start_worker(void) {
	if (worker_started) {
		return;
	}
	pthread_t thread;
	if (pthread_create(&thread, NULL, worker_main, NULL) == 0) {
		pthread_detach(thread);
		worker_started = true;
	}
}

static void run_ex(const job_t *job, bool root) {
	if (job_is_list(job->kind)) {
		inflight_job = *job;
		inflight_valid = true;
		inflight_is_back = false;
		inflight_is_root = root;
		// A new list is starting: the old one's paging must not fire while it is
		// awaited.
		page_more = false;
		page_loading = false;
	}
	start_worker();
	// No veil for "My podcasts": that list is on the card, comes back the
	// instant it is asked for, and a veil that appears and vanishes in the same
	// frame is only a black flash at every entry into the section.
	//
	// The veil DOES cover the wait before an episode starts, and it stays up
	// until job_done_cb takes it down, which is the moment playback has really
	// begun -- a timed toast would leave before the download it is about.
	if (job->kind == JOB_PLAY) {
		player_pause_for_loading();
	}
	if (job->kind != JOB_FOLLOWED) {
		busy_show_text(job->kind == JOB_PLAY);
	}
	submit(job);
}

static void run(const job_t *job) { run_ex(job, false); }

// A higher limit when the scroll comes near the bottom: like the radio, except
// the catalogue has no offset, so the same request is made again asking for one
// more page. No veil and no history -- it is the SAME list continuing (inflight
// deliberately stays off), and the rows already there stay usable meanwhile.
static void maybe_load_more(void) {
	if (page_loading || !page_more) {
		return;
	}
	// A page arriving would rewrite the arrays the chosen rows are numbered
	// against, under the selection.
	if (selecting) {
		return;
	}
	if (lv_screen_active() != list_screen || !have_current_list || !job_is_list(current_list_job.kind) ||
		current_list_job.kind == JOB_FOLLOWED) {
		return;
	}
	// A sideways drag (back, or pulling the player in) moves this list too:
	// that is not a request for more rows.
	if (player_sheet_drag_active() || switcher_back_drag_active()) {
		return;
	}
	// Within one screen of the bottom is enough: the request takes a moment,
	// and reaching the last row only to find the list stops there is exactly
	// what this replaces.
	if (lv_obj_get_scroll_bottom(list_container) > lv_obj_get_height(list_container)) {
		return;
	}
	page_loading = true;
	job_t job = current_list_job;
	// From the LIMIT asked for last time, not from the cleaned-up count: the
	// discards (episodes with no audio) must not shift the step of the next
	// request.
	job.want = result_want + (current_list_job.kind == JOB_EPISODES ? PODCAST_EPISODES_STEP : PODCAST_LIMIT);
	job.more = true;
	start_worker();
	submit(&job);
}

// ---------------------------------------------------------------------------
// waiting for an episode to come down
//
// Much simpler than on Qobuz and Tidal, for the reason given at the top of the
// file: nothing is fetched ahead. There is one request at a time, and it is
// always "the episode needed NOW".
// ---------------------------------------------------------------------------

static pthread_mutex_t fetch_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t fetch_ready = PTHREAD_COND_INITIALIZER;
static long long fetch_want_id;
static bool fetch_started;
static bool waiting_for_episode;

// The episodes that are known: they are what the address of the one the queue
// asks for is found from. Filled when an episode list is started.
static podcast_episode_t queued_episodes[PODCAST_MAX_HELD];
// Whether that array is the whole feed rather than a page of it. Asking its
// length is not the same question: a podcast with twenty episodes has twenty
// and no more, and testing the length against the cap would send the player
// back to the catalogue for the two hundred that do not exist -- once per tap,
// in front of the sound.
static bool queued_complete;
static int queued_count;

static void downloading_note_async(void *user) {
	(void)user;
	gui_notify_popup("downloading");
}

static bool episode_is_playable(const char *path) {
	long long id = podcastcache_episode_id(path);
	if (podcastcache_find(id, NULL, 0)) {
		return true;
	}
	// Or it is coming down right NOW: the decoder can read a growing file. Two
	// questions and not one, for the same reason as the other two services: the
	// comparison by path and the one by id can disagree, and the id cannot be
	// wrong.
	return growfile_is_growing(path) || (id != 0 && podcastcache_downloading_id() == id);
}

static void resume_if_waiting_async(void *user) {
	(void)user;
	if (!waiting_for_episode) {
		return;
	}
	// The Wi-Fi transfer is a file server running as root on the card this
	// episode was just written to. Nothing starts playing under it.
	if (wifitransfer_get_enabled()) {
		return;
	}
	int index = playlist_current_index();
	char path[512];
	if (index < 0 || !playlist_path_at(index, path, sizeof(path))) {
		return;
	}
	if (podcastcache_owns(path) && !episode_is_playable(path)) {
		fprintf(stderr, "podcast: something arrived but the queue is waiting for '%s', which is not ready\n", path);
		return;
	}
	waiting_for_episode = false;
	fprintf(stderr, "podcast: restarting the queue from slot %d\n", index);
	device_state_play_queue_index(index);
	player_refresh_now_playing();
}

// Called by device_state an instant before a file is started.
static bool podcastpage_prepare_track(char *path, size_t path_size) {
	(void)path_size; // the cache writes the episode under the name the queue holds
	if (!podcastcache_owns(path)) {
		waiting_for_episode = false;
		return true; // not a podcast episode
	}
	if (episode_is_playable(path)) {
		waiting_for_episode = false;
		return true;
	}

	fprintf(stderr, "podcast: '%s' not ready (id=%lld): stopping and requesting it\n", path, podcastcache_episode_id(path));

	// Stop what was playing FIRST: the previous file is still coming down and is
	// exactly what prefetch_urgent is about to abandon. Pulling it out from
	// under the audio thread makes it report "track finished", and the queue
	// advances by itself onto another missing episode.
	podcastcache_set_network_wanted(true);
	waiting_for_episode = true;
	player_pause_for_loading();
	device_state_stop();
	prefetch_urgent(path);
	gui_post(downloading_note_async, NULL);
	return false;
}

static void prefetch_urgent(const char *wanted_path) {
	long long wanted = podcastcache_episode_id(wanted_path ? wanted_path : "");
	if (wanted == 0) {
		return;
	}

	// Is this one already coming down? Asked both ways, and if the answer is yes
	// it is NOT abandoned: throwing away a wait already under way means starting
	// from zero and never reaching the end.
	char stem[512];
	snprintf(stem, sizeof(stem), "%s", wanted_path ? wanted_path : "");
	char *dot = strrchr(stem, '.');
	if (dot) {
		*(dot + 1) = '\0';
	}
	bool already_ours = (stem[0] && growfile_prefix_is_growing(stem)) || podcastcache_downloading_id() == wanted;
	if (!already_ours) {
		streamturn_abandon_all();
	}

	podcastcache_set_network_wanted(true);

	pthread_mutex_lock(&fetch_lock);
	fetch_want_id = wanted;
	pthread_cond_broadcast(&fetch_ready);
	pthread_mutex_unlock(&fetch_lock);
}

// The author of the podcast being browsed. It does not arrive with the episodes
// -- the catalogue's /episodes/byfeedid answer carries the podcast's TITLE and
// not its author -- but the sidecar files need it, because that is where the
// player's star takes what it needs to follow the podcast without asking the
// network. Taken when the podcast's row is tapped, where the whole record is
// there.
static char browsing_author[PODCAST_AUTHOR_MAX];
static char browsing_image[PODCAST_URL_MAX];

// Downloads an episode and writes the sidecars, with the local path in `out`.
static bool fetch_episode(const podcast_episode_t *e, char *out, size_t size) {
	if (!podcastcache_start(e->id, e->enclosure, e->mime, e->duration_secs, out, size)) {
		return false;
	}
	const char *ftitle = e->feed_title[0] ? e->feed_title : (list_feed_title[0] ? list_feed_title : NULL);
	const char *fauthor = browsing_author[0] ? browsing_author : NULL;
	podcastcache_write_sidecars(e->id, e->mime, e->title, ftitle, e->feed_id,
								fauthor, browsing_image[0] ? browsing_image : e->image, e->image, true);
	return true;
}

static void *fetch_main(void *arg) {
	(void)arg;
	thread_be_background("podcast fetch");

	for (;;) {
		pthread_mutex_lock(&fetch_lock);
		while (fetch_want_id == 0) {
			pthread_cond_wait(&fetch_ready, &fetch_lock);
		}
		long long id = fetch_want_id;
		pthread_mutex_unlock(&fetch_lock);

		podcast_episode_t episode;
		bool known = false;
		for (int i = 0; i < queued_count && !known; i++) {
			if (queued_episodes[i].id == id) {
				episode = queued_episodes[i];
				known = true;
			}
		}

		// Not known does not mean lost: ask the catalogue.
		//
		// `queued_episodes` is the last episode list that was looked at, and the
		// queue outlives it: opening another podcast, or returning to a queue
		// resumed after a shutdown, is enough for the episode coming up not to be
		// in that array any more, and stopping here would leave playback stuck
		// on the next episode. One catalogue request by id settles it.
		if (!known) {
			fprintf(stderr, "podcast: %lld is not in the list we know, asking the catalogue\n", id);
			if (!podcast_episode(id, &episode)) {
				fprintf(stderr, "podcast: %lld not found there either: %s\n", id, podcast_last_error());
				pthread_mutex_lock(&fetch_lock);
				if (fetch_want_id == id) {
					fetch_want_id = 0;
				}
				pthread_mutex_unlock(&fetch_lock);
				continue;
			}
		}

		podcastcache_wait_idle();

		// The request may have changed while waiting for a turn.
		pthread_mutex_lock(&fetch_lock);
		bool moved_on = fetch_want_id != id;
		pthread_mutex_unlock(&fetch_lock);
		if (moved_on) {
			continue;
		}

		char path[512];
		bool ok = fetch_episode(&episode, path, sizeof(path));
		if (!ok) {
			fprintf(stderr, "podcast: %lld does not download: %s\n", id, podcastcache_last_error());
		}

		pthread_mutex_lock(&fetch_lock);
		if (fetch_want_id == id) {
			fetch_want_id = 0;
		}
		bool superseded = fetch_want_id != 0 && fetch_want_id != id;
		pthread_mutex_unlock(&fetch_lock);

		if (ok && !superseded) {
			gui_post(resume_if_waiting_async, NULL);
		}
	}
	return NULL;
}

static void fetch_start_thread(void) {
	pthread_mutex_lock(&fetch_lock);
	if (!fetch_started) {
		pthread_t thread;
		if (pthread_create(&thread, NULL, fetch_main, NULL) == 0) {
			pthread_detach(thread);
			fetch_started = true;
		}
	}
	pthread_mutex_unlock(&fetch_lock);
}

// ---------------------------------------------------------------------------
// results coming back, on the GUI thread
// ---------------------------------------------------------------------------

static void play_started_async(void *user) {
	(void)user;
	player_refresh_now_playing();
	switch_screen(player_screen);
}

static void job_done_cb(void *user) {
	(void)user;
	busy_hide();

	if (result_error[0]) {
		list_nav_drop();
		gui_notify_popup(result_error);
		page_loading = false;
		job_result_taken();
		refresh_notes();
		return;
	}



	if (result_kind == JOB_PLAY) {
		job_result_taken();
		return;
	}

	fill_list(result_from);
	// The request came back full to the limit asked for: there is probably more.
	// Come back short, the list has ended and nothing more is asked for. "My
	// podcasts" is entirely on the card and never pages.
	page_loading = false;
	page_more = result_kind != JOB_FOLLOWED && result_raw >= result_want &&
				result_count < (result_kind == JOB_EPISODES ? PODCAST_MAX_HELD : PODCAST_FEEDS_MAX);
	list_nav_commit();

	// The page only changes if the viewer is still where it expects to be.
	//
	// A job can finish after the user has gone elsewhere -- the real case is
	// opening the magnifier while the list is still loading: the result arrived
	// and forced the list back up, with the keyboard vanishing under the
	// fingers. The search screen counts as "inside the section", because the
	// results are exactly what was asked for from there.
	lv_obj_t *active = lv_screen_active();
	if (active == list_screen || active == search_screen) {
		switch_screen(list_screen);
	}
	job_result_taken();
}

// ---------------------------------------------------------------------------
// the thread
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// the lists kept on the card
//
// The chart and a podcast's episodes were fetched again every time the tab was
// opened -- a request, a spinner and the same names as last time. The first
// page of each is written to the card and read back, so opening a tab shows its
// rows at once; the catalogue is asked again only when the file is older than
// six hours or the list is continued (the next page is always a request). Six
// hours rather than minutes: a podcast's back catalogue does not change within a
// listening session, and a feed that has published something new can wait for
// the next visit after the file ages out rather than costing every visit the
// whole download again.
// ---------------------------------------------------------------------------

#define PODCAST_CACHE_TTL_SECS (6 * 60 * 60)

static void podcast_cache_key(const job_t *job, char *out, size_t size) {
	out[0] = '\0';
	if (job->kind == JOB_TRENDING) {
		// One file per region: the chart of a language somebody chose is a
		// different list from another's, and reading the wrong one back would
		// show a chart the settings page says is not the one being drawn.
		snprintf(out, size, "%s", podcast_trending_cache_key());
	} else if (job->kind == JOB_EPISODES && job->id > 0) {
		snprintf(out, size, "pod_feed_%lld", job->id);
	}
}

static void *worker_main(void *arg) {
	(void)arg;
	thread_be_background("podcast worker");

	for (;;) {
		pthread_mutex_lock(&job_lock);
		while (!have_pending) {
			pthread_cond_wait(&job_ready, &job_lock);
		}
		// The previous result must have been taken, or it would be rewritten
		// under whoever is drawing it. With a cap: if the GUI has fallen behind
		// through a fault of its own, an overwritten list beats a worker stuck
		// forever.
		struct timespec deadline;
		clock_gettime(CLOCK_REALTIME, &deadline);
		deadline.tv_sec += 2;
		while (result_pending) {
			if (pthread_cond_timedwait(&result_taken, &job_lock, &deadline) != 0) {
				break;
			}
		}

		job_t job = pending;
		have_pending = false;
		pthread_mutex_unlock(&job_lock);

		// What is on screen stays there if the job fails: without this snapshot,
		// an error emptied the list being looked at.
		int kept_count = result_count;
		job_kind_t kept_kind = result_kind;
		char kept_title[160];
		snprintf(kept_title, sizeof(kept_title), "%s", result_title);

		result_error[0] = '\0';

		// The order this answer is put in, read once: the rows that land on
		// screen are drawn by the same rule, and a toggle mid-request must not
		// leave half the list one way and half the other.
		bool newest_first = podcastpage_episodes_newest_first();

		// Playback does not clear the list, and that is not a detail: what is
		// playing is an episode of THIS list, and the list stays on screen with
		// its tappable rows. Cleared, a second tap on another episode finds
		// result_count at zero and returns in silence. Same rule as
		// qobuzpage.c.
		result_kind = job.kind;

		// How many entries in all. The catalogue does not page with an offset, so
		// continuing a list is the same request with a higher limit: the arrays
		// are rewritten whole, and the GUI draws only the new rows (from
		// result_from on).
		int want = job.want;
		if (want <= 0) {
			want = PODCAST_LIMIT;
		}
		int cap = job.kind == JOB_EPISODES || job.kind == JOB_PLAY ? PODCAST_MAX_HELD : PODCAST_FEEDS_MAX;
		if (want > cap) {
			want = cap;
		}
		bool grow = job.more;
		int kept = result_count;

		// What this answer is, before anything is read for it. The reset comes
		// HERE and not after the card has been asked: the cached list is an
		// answer too, and zeroing the count behind it left every list read from
		// the card at zero -- a tab that showed its rows once and said there was
		// nothing to show on the next visit.
		if (job.kind != JOB_PLAY && !grow) {
			result_count = 0;
			snprintf(result_title, sizeof(result_title), "%s", job.text);
		}
		if (job.kind != JOB_PLAY) {
			result_from = grow ? kept : 0;
			result_want = want;
		}

		// A first page that is still young is read from the card instead of
		// from the catalogue: the chart and a podcast's episodes do not change
		// between two looks at the same tab, and on a slow connection that is
		// the difference between a tab and a wait for the same names.
		char cache_key[64];
		podcast_cache_key(&job, cache_key, sizeof(cache_key));
		bool from_cache = false;
		if (!grow && job.kind != JOB_PLAY && cache_key[0]) {
			int age = listcache_age(cache_key);
			if (age >= 0 && age <= PODCAST_CACHE_TTL_SECS) {
				int cached = job.kind == JOB_TRENDING
								 ? listcache_load(cache_key, result_feeds, sizeof(result_feeds[0]), want)
								 : listcache_load(cache_key, result_episodes, sizeof(result_episodes[0]), want);
				if (cached > 0) {
					from_cache = true;
					result_count = cached;
					// The file holds the first page and nothing else, so "was the
					// answer full?" is answered with the limit that was asked for:
					// the next page is a request, and scrolling to the bottom of a
					// cached list has to be able to make it.
					result_raw = want;
					if (job.kind == JOB_EPISODES) {
						if (!newest_first) {
							reverse_episodes(result_episodes, cached);
						}
						// Playback looks its episodes up here, so a list read
						// from the card has to fill it too -- and say it holds
						// only the first page, so the downloader asks for the
						// rest when the queue reaches them.
						memcpy(queued_episodes, result_episodes, sizeof(result_episodes[0]) * (size_t)cached);
						queued_count = cached;
						queued_complete = false;
					}
				}
			}
		}

		switch (job.kind) {
		case JOB_SEARCH: {
			int n = podcast_search(job.text, result_feeds, want);
			if (n < 0) {
				snprintf(result_error, sizeof(result_error), "%s", podcast_last_error());
			} else {
				result_count = n;
				result_raw = podcast_last_raw_count();
			}
			break;
		}

		case JOB_TRENDING: {
			if (from_cache) {
				break;
			}
			int n = podcast_trending(result_feeds, want);
			if (n < 0) {
				snprintf(result_error, sizeof(result_error), "%s", podcast_last_error());
			} else {
				result_count = n;
				result_raw = podcast_last_raw_count();
				if (n > 0) {
					listcache_save(cache_key, result_feeds, sizeof(result_feeds[0]), n);
				}
			}
			break;
		}

		case JOB_EPISODES: {
			if (from_cache) {
				break;
			}
			int n = podcast_episodes(job.id, result_episodes, want);
			if (n < 0) {
				snprintf(result_error, sizeof(result_error), "%s", podcast_last_error());
			} else {
				result_count = n;
				result_raw = podcast_last_raw_count();
				// Written as the catalogue sent it, newest first, and reversed
				// on the way out of the file the same way it is here: the order
				// on the card must not depend on the setting in force when it
				// was written.
				if (n > 0) {
					listcache_save(cache_key, result_episodes, sizeof(result_episodes[0]), n);
				}
				if (!newest_first) {
					reverse_episodes(result_episodes, n);
				}
				// The episodes that just arrived become the ones the downloader
				// can look up: this is where it takes the address from when the
				// queue reaches one that is not there yet.
				memcpy(queued_episodes, result_episodes, sizeof(result_episodes[0]) * (size_t)n);
				queued_count = n;
				// Short of the limit asked for, or at the most the arrays hold:
				// what came back is the whole feed.
				queued_complete = want >= PODCAST_MAX_HELD || result_raw < want;
			}
			break;
		}

		case JOB_FOLLOWED:
			// No network: the list is on the card, all of it at once.
			result_count = load_followed(result_feeds, PODCAST_MAX_HELD);
			result_raw = result_count;
			break;

		case JOB_PLAY: {
			int index = job.index;
			if (index < 0 || index >= queued_count) {
				snprintf(result_error, sizeof(result_error), "%s", tr("podcast_that_episode_is_gone"));
				break;
			}

			// The queue is the rest of the podcast, and the list it is built
			// from already holds its first step: JOB_EPISODES asks for
			// PODCAST_EPISODES_STEP episodes when the podcast is opened.
			//
			// This is the fallback for the one case where it is not -- a queue
			// resumed after a restart, or an episode reached from somewhere
			// that never loaded the list. Between the tap and the first sound
			// there is nothing here to wait for otherwise, which is the whole
			// point: the request costs a round trip and the user is watching.
			//
			// The tapped episode is found again by its id rather than kept at
			// its index: the longer list is the same list with more of it, but
			// the id is what makes that certain.
			if (!queued_complete && queued_count < PODCAST_EPISODES_STEP && queued_episodes[index].feed_id > 0) {
				long long wanted_id = queued_episodes[index].id;
				long long feed = queued_episodes[index].feed_id;
				int n = podcast_episodes(feed, result_episodes, PODCAST_EPISODES_STEP);
				if (n > 0 && !newest_first) {
					reverse_episodes(result_episodes, n);
				}
				if (n > queued_count) {
					memcpy(queued_episodes, result_episodes, sizeof(result_episodes[0]) * (size_t)n);
					queued_count = n;
					queued_complete = true;
					// The list on screen has grown too: it is the same list.
					result_count = n;
					result_from = 0;
					result_want = PODCAST_EPISODES_STEP;

					int found = -1;
					for (int i = 0; i < n; i++) {
						if (queued_episodes[i].id == wanted_id) {
							found = i;
							break;
						}
					}
					if (found < 0) {
						snprintf(result_error, sizeof(result_error), "%s", tr("podcast_that_episode_is_gone"));
						break;
					}
					index = found;
				}
			}

			// The subscription this episode belongs to has been listened to once
			// more, which is what "most listened" sorts by. Counted here, at the
			// start, and not when the episode reaches its end: a podcast is
			// listened to in pieces, and the piece somebody keeps coming back to
			// is the one that ranks.
			podcastsubs_count_play(queued_episodes[index].feed_id);

			// Drop whatever was coming down: this is the user's order, not a
			// fetch ahead.
			streamturn_abandon_all();
			podcastcache_set_network_wanted(true);

			char path[512];
			if (!fetch_episode(&queued_episodes[index], path, sizeof(path))) {
				snprintf(result_error, sizeof(result_error), "%s", podcastcache_last_error());
				break;
			}

			// The queue: this episode and the ones AFTER it in time, that is, going
			// back. Which way that is in the array depends on the order the list is
			// drawn in -- newest first walks up the array, oldest first walks down
			// it -- and the tapped one comes first either way.
			//
			// The others are not downloaded now: they come down when their turn
			// arrives (see podcastpage_prepare_track).
			static char paths[PODCAST_QUEUE_MAX][512];
			static const char *path_ptr[PODCAST_QUEUE_MAX];
			int count = 0;
			int step = newest_first ? 1 : -1;
			for (int i = index; i >= 0 && i < queued_count && count < PODCAST_QUEUE_MAX; i += step) {
				if (i == index) {
					snprintf(paths[count], sizeof(paths[0]), "%s", path);
				} else if (!podcastcache_find(queued_episodes[i].id, paths[count], sizeof(paths[0]))) {
					podcastcache_path(queued_episodes[i].id, queued_episodes[i].mime, paths[count],
									  sizeof(paths[0]));

					// The tags for the whole queue at once, as Qobuz does and
					// for the same reason: they are text files of a few dozen
					// bytes and cost nothing, and without them the Queue page
					// would show the file name, which is the id, for every
					// episode not yet downloaded. Not the cover: that is an
					// image to download, and it is fetched when the episode's
					// turn comes.
					const podcast_episode_t *q = &queued_episodes[i];
					const char *ftitle = q->feed_title[0] ? q->feed_title : (list_feed_title[0] ? list_feed_title : NULL);
					const char *fauthor = browsing_author[0] ? browsing_author : NULL;
					podcastcache_write_sidecars(q->id, q->mime, q->title,
												ftitle, q->feed_id,
												fauthor, browsing_image, q->image, false);
				}
				path_ptr[count] = paths[count];
				count++;
			}

			// _ordered: a podcast queue already has its order, newest first
			// going back. Plain device_state_play_list would shuffle the
			// episodes whenever the play mode is random.
			device_state_play_list_ordered(path_ptr, count, 0);
			gui_post(play_started_async, NULL);
			break;
		}

		case JOB_NONE:
		default:
			break;
		}

		// The job failed: put back what was there.
		if (result_error[0]) {
			result_count = kept_count;
			result_kind = kept_kind;
			snprintf(result_title, sizeof(result_title), "%s", kept_title);
		}

		// Repeating with a higher limit can -- rarely -- bring back FEWER entries
		// than before (the chart has moved): redraw from where the list really
		// ends, never past it.
		if (result_from > result_count) {
			result_from = result_count;
		}

		pthread_mutex_lock(&job_lock);
		result_pending = true;
		pthread_mutex_unlock(&job_lock);
		gui_post(job_done_cb, NULL);
	}
	return NULL;
}

// ---------------------------------------------------------------------------
// tapping a row
// ---------------------------------------------------------------------------

static void row_clicked_cb(lv_event_t *e) {
	if (switcher_back_drag_active() || player_sheet_drag_active()) {
		return;
	}
	int index = row_index_of(lv_event_get_current_target(e));
	if (index < 0 || index >= list_count) {
		return;
	}

	// In selection mode a tap picks the row and nothing else: opening a podcast
	// or starting an episode would be a different thing from what the tick
	// said, and the list is being used as a set for a moment.
	if (selecting) {
		selection_toggle(index);
		return;
	}

	job_t job = {0};
	if (list_kind == JOB_EPISODES) {
		// No toast: a timed one would leave while the download it is about
		// carries on. run_ex() puts the veil up instead, with the same words on
		// it, and takes it down when the episode actually starts.
		job.kind = JOB_PLAY;
		job.index = index;
		job.id = result_episodes[index].id;
		run(&job);
		return;
	}

	// The author is taken HERE, where the podcast's whole record is available:
	// the episodes answer does not carry it, and without it the sidecars would
	// not be enough for the player's star.
	snprintf(browsing_author, sizeof(browsing_author), "%s", result_feeds[index].author);
	snprintf(browsing_image, sizeof(browsing_image), "%s", result_feeds[index].image);

	job.kind = JOB_EPISODES;
	job.id = result_feeds[index].id;
	// The first step; the rest comes as the list is scrolled.
	job.want = PODCAST_EPISODES_STEP;
	snprintf(job.text, sizeof(job.text), "%s", result_feeds[index].title);
	run(&job);
}

// ---------------------------------------------------------------------------
// the three-dot menus: follow a feed, or act on an episode
// ---------------------------------------------------------------------------

static int menu_row_index;

static void follow_cb(void *user) {
	(void)user;
	if (menu_row_index < 0 || menu_row_index >= list_count) {
		return;
	}
	// Already followed: the menu says Unfollow in that case, so this is a tap
	// that raced a follow made elsewhere. Nothing to write, and no second
	// "now following" notice over a podcast that was already there.
	if (podcastsubs_is_followed(result_feeds[menu_row_index].id)) {
		return;
	}
	podcastsubs_follow(&result_feeds[menu_row_index]);
	gui_notify_popup(tr("podcast_now_following"));
}

static void unfollow_cb(void *user) {
	(void)user;
	if (menu_row_index < 0 || menu_row_index >= list_count) {
		return;
	}
	podcastsubs_unfollow(result_feeds[menu_row_index].id);
	gui_notify_popup(tr("podcast_no_longer_following"));

	// If the followed list itself is on screen, the row has to go at once:
	// leaving it there would mean a list that is no longer true.
	if (list_kind == JOB_FOLLOWED) {
		result_kind = JOB_FOLLOWED;
		result_count = load_followed(result_feeds, PODCAST_MAX_HELD);
		fill_list(0);
	}
}

static void download_cb(void *user);
static void mark_heard_cb(void *user);
static void mark_not_heard_cb(void *user);

// Starts the selection with this row already chosen. The row is read from
// whichever variable the menu that called this belongs to: the feed menu keeps
// its row in menu_row_index, an episode's long press in dl_row.
static void row_menu_cb(lv_event_t *e) {
	lv_event_stop_bubbling(e);
	menu_row_index = menu_index_of(lv_event_get_current_target(e));
	if (menu_row_index < 0 || menu_row_index >= list_count) {
		return;
	}

	static const popover_item_t FOLLOW[] = {{"podcast_follow", follow_cb, NULL, false}};
	static const popover_item_t UNFOLLOW[] = {{"podcast_unfollow", unfollow_cb, NULL, false}};

	bool followed = podcastsubs_is_followed(result_feeds[menu_row_index].id);
	popover_show(lv_event_get_current_target(e), followed ? UNFOLLOW : FOLLOW, 1);
}

static void episode_menu_cb(lv_event_t *e) {
	lv_event_stop_bubbling(e);
	int index = menu_index_of(lv_event_get_current_target(e));
	if (index < 0 || index >= list_count || list_kind != JOB_EPISODES) {
		return;
	}
	dl_row = index;
	static const popover_item_t NOT_HEARD[] = {{"podcast_mark_listened", mark_heard_cb, NULL, false},
												 {"podcast_download", download_cb, NULL, false}};
	static const popover_item_t HEARD[] = {{"podcast_mark_not_listened", mark_not_heard_cb, NULL, false},
									   {"podcast_download", download_cb, NULL, false}};
	bool heard = podcastplayed_get(result_episodes[index].id);
	popover_show(lv_event_get_current_target(e), heard ? HEARD : NOT_HEARD, 2);
}

// ---------------------------------------------------------------------------
// keeping an episode: the long press, and the card that follows the download
// ---------------------------------------------------------------------------

#define DL_POLL_MS 250

static lv_obj_t *dl_veil;
static lv_obj_t *dl_name;
static lv_obj_t *dl_bar;
static lv_obj_t *dl_amount;
static lv_timer_t *dl_timer;
static int dl_row = -1;
static int bulk_dl_rows[PODCAST_MAX_HELD];
static int bulk_dl_count;
static int bulk_dl_at;
static bool bulk_dl_active;
static bool bulk_dl_failed;

static void bulk_download_next(void);

static void dl_close(void) {
	lv_timer_pause(dl_timer);
	lv_obj_set_hidden(dl_veil, true);
	power_hold_screen_on(false);
}

// "12.4 / 58.0 MB", or the bytes alone when the server gave no length.
static void dl_paint(long long done, long long total) {
	double done_mb = (double)done / (1024.0 * 1024.0);
	if (total > 0) {
		int permille = (int)(done * 1000 / total);
		lv_bar_set_value(dl_bar, permille, LV_ANIM_OFF);
		lv_label_set_text_fmt(dl_amount, "%.1f / %.1f MB \xC2\xB7 %d%%", done_mb, (double)total / (1024.0 * 1024.0),
							  permille / 10);
	} else {
		lv_bar_set_value(dl_bar, 0, LV_ANIM_OFF);
		lv_label_set_text_fmt(dl_amount, "%.1f MB", done_mb);
	}
}

static void dl_poll_cb(lv_timer_t *timer) {
	(void)timer;
	long long done = 0, total = 0;
	podcastdl_state_t state = podcastdl_state(&done, &total);
	dl_paint(done, total);
	if (state == PODCASTDL_RUNNING) {
		return;
	}
	dl_close();
	podcastdl_acknowledge();
	if (bulk_dl_active) {
		if (state != PODCASTDL_DONE) {
			bulk_dl_failed = true;
		}
		bulk_download_next();
	} else if (state == PODCASTDL_DONE) {
		toast_success(tr("podcast_download_done"));
	} else if (state == PODCASTDL_FAILED) {
		toast_error(tr("podcast_download_failed"));
	}
}

static void dl_cancel_cb(lv_event_t *e) {
	(void)e;
	bulk_dl_active = false;
	bulk_dl_count = 0;
	podcastdl_cancel();
	lv_label_set_text(dl_amount, tr("podcast_download_cancelling"));
}

static void dl_build(gui_config_t *cfg) {
	dl_veil = lv_obj_create(lv_layer_top());
	lv_obj_set_size(dl_veil, lv_pct(100), lv_pct(100));
	lv_obj_set_style_bg_color(dl_veil, lv_color_black(), 0);
	lv_obj_set_style_bg_opa(dl_veil, LV_OPA_60, 0);
	lv_obj_set_style_border_width(dl_veil, 0, 0);
	lv_obj_set_style_radius(dl_veil, 0, 0);
	lv_obj_set_scrollable(dl_veil, false);
	lv_obj_set_clickable(dl_veil, true);
	lv_obj_set_hidden(dl_veil, true);

	lv_obj_t *card = lv_obj_create(dl_veil);
	lv_obj_set_size(card, cfg->screen_width - 2 * cfg->padding, LV_SIZE_CONTENT);
	lv_obj_add_style(card, &theme_style_card, 0);
	lv_obj_set_style_radius(card, 16, 0);
	lv_obj_set_style_border_width(card, 0, 0);
	lv_obj_set_style_shadow_width(card, 0, 0);
	lv_obj_set_style_pad_all(card, 20, 0);
	lv_obj_set_style_pad_row(card, 14, 0);
	lv_obj_set_scrollable(card, false);
	lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
	lv_obj_align(card, LV_ALIGN_CENTER, 0, cfg->top_bar_height / 2);

	lv_obj_t *title = lv_label_create(card);
	lv_label_set_text(title, tr("podcast_downloading"));
	lv_obj_add_style(title, &theme_style_text, 0);
	lv_obj_set_style_text_font(title, &font_ui_24, 0);

	dl_name = lv_label_create(card);
	lv_label_set_long_mode(dl_name, LV_LABEL_LONG_DOT);
	lv_obj_set_width(dl_name, lv_pct(100));
	lv_obj_set_style_max_height(dl_name, 2 * lv_font_get_line_height(&font_ui_20), 0);
	lv_obj_add_style(dl_name, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(dl_name, &font_ui_20, 0);

	dl_bar = lv_bar_create(card);
	lv_obj_set_size(dl_bar, lv_pct(100), 12);
	lv_bar_set_range(dl_bar, 0, 1000);
	lv_obj_set_style_bg_color(dl_bar, theme()->surface_pressed, LV_PART_MAIN);
	lv_obj_set_style_bg_opa(dl_bar, LV_OPA_COVER, LV_PART_MAIN);
	lv_obj_set_style_bg_color(dl_bar, theme()->accent, LV_PART_INDICATOR);
	lv_obj_set_style_radius(dl_bar, LV_RADIUS_CIRCLE, LV_PART_MAIN);
	lv_obj_set_style_radius(dl_bar, LV_RADIUS_CIRCLE, LV_PART_INDICATOR);

	dl_amount = lv_label_create(card);
	lv_obj_add_style(dl_amount, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(dl_amount, &font_ui_18, 0);

	lv_obj_t *cancel = lv_btn_create(card);
	lv_obj_set_size(cancel, lv_pct(100), 56);
	lv_obj_set_style_radius(cancel, LV_RADIUS_CIRCLE, 0);
	lv_obj_set_style_shadow_width(cancel, 0, 0);
	lv_obj_set_style_border_width(cancel, 0, 0);
	lv_obj_set_style_bg_color(cancel, theme()->surface_pressed, 0);
	lv_obj_add_event_cb(cancel, dl_cancel_cb, LV_EVENT_CLICKED, NULL);
	lv_obj_t *cancel_label = lv_label_create(cancel);
	lv_label_set_text(cancel_label, tr("cancel"));
	lv_obj_set_style_text_font(cancel_label, &font_ui_22, 0);
	lv_obj_set_style_text_color(cancel_label, theme()->text_primary, 0);
	lv_obj_center(cancel_label);

	dl_timer = lv_timer_create(dl_poll_cb, DL_POLL_MS, NULL);
	lv_timer_pause(dl_timer);
}

static bool download_start(int index) {
	if (index < 0 || index >= list_count || list_kind != JOB_EPISODES) {
		return false;
	}
	dl_row = index;
	const podcast_episode_t *episode = &result_episodes[index];
	if (podcastdl_exists(episode, list_feed_title)) {
		return false;
	}
	if (!podcastdl_start(episode, list_feed_title, browsing_author, browsing_image)) {
		return false;
	}

	// Theme colours are read at every opening: the card is built once.
	lv_obj_set_style_bg_color(dl_bar, theme()->accent, LV_PART_INDICATOR);
	lv_label_set_text(dl_name, episode->title);
	dl_paint(0, 0);
	lv_obj_set_hidden(dl_veil, false);
	if (!screensaver_is_visible()) {
		lv_obj_move_foreground(dl_veil);
	}
	power_hold_screen_on(true);
	lv_timer_reset(dl_timer);
	lv_timer_resume(dl_timer);
	return true;
}

static void download_cb(void *user) {
	(void)user;
	if (dl_row < 0 || dl_row >= list_count || list_kind != JOB_EPISODES) {
		return;
	}
	if (!download_start(dl_row)) {
		toast_plain(tr("podcast_already_downloaded"));
	}
}

static void bulk_download_next(void) {
	while (bulk_dl_active && bulk_dl_at < bulk_dl_count) {
		int index = bulk_dl_rows[bulk_dl_at++];
		if (podcastdl_exists(&result_episodes[index], list_feed_title)) {
			continue;
		}
		if (download_start(index)) {
			return;
		}
		bulk_dl_failed = true;
	}
	if (bulk_dl_active) {
		bulk_dl_active = false;
		if (bulk_dl_failed) {
			toast_error(tr("podcast_download_failed"));
		} else {
			toast_success(tr("podcast_download_done"));
		}
	}
}

// The tick, set or taken away by hand. A menu that says "Mark as listened" is
// the only way to retire an episode one has already heard somewhere else -- or
// to put one back that was marked by mistake.
static void mark_heard_cb(void *user) {
	(void)user;
	if (dl_row < 0 || dl_row >= list_count || list_kind != JOB_EPISODES) {
		return;
	}
	podcastplayed_set(result_episodes[dl_row].id, true);
	refresh_row(dl_row);
}

static void mark_not_heard_cb(void *user) {
	(void)user;
	if (dl_row < 0 || dl_row >= list_count || list_kind != JOB_EPISODES) {
		return;
	}
	podcastplayed_set(result_episodes[dl_row].id, false);
	refresh_row(dl_row);
}

static void row_long_cb(lv_event_t *e) {
	if (switcher_back_drag_active() || player_sheet_drag_active()) {
		return;
	}

	int index = row_index_of(lv_event_get_current_target(e));
	if (index < 0 || index >= list_count) {
		return;
	}
	lv_indev_t *indev = lv_indev_active();
	if (indev) {
		lv_indev_wait_release(indev);
	}
	if (!selection_start()) {
		return;
	}
	if (!chosen[index]) {
		selection_toggle(index);
	}
}

// ---------------------------------------------------------------------------
// the page: pills on top, list below
//
// ONE screen. "My podcasts" and "Trending" are not different places: they are
// two ways of looking at the same thing, exactly like All / Recent / Finished in
// the audiobooks. That is what pills are for -- they change what is underneath
// without navigating away -- so the list sits on the same screen.
//
// The pills go away when a search or a podcast's episodes are underneath: there
// they would no longer describe what is on screen, and a lit pill over a list
// that is not its own is a lie.
// ---------------------------------------------------------------------------

static void pill_clicked_cb(lv_event_t *e);

static lv_obj_t *make_pill(lv_obj_t *parent, const char *text, job_kind_t kind, bool compact) {
	lv_obj_t *btn = lv_btn_create(parent);
	lv_obj_set_size(btn, LV_SIZE_CONTENT, compact ? 36 : 56);
	// The regular 20 px insets make the two English labels wider than the
	// V1's entire 240 px panel, even before Large text is selected. Match the
	// compact choice pills used by settingsrow: the labels still keep a clear
	// touch target, while both sections fit at every V1 text-size setting.
	lv_obj_set_style_pad_hor(btn, compact ? 8 : 20, 0);
	lv_obj_set_style_radius(btn, LV_RADIUS_CIRCLE, 0); // Adwaita-style pill
	lv_obj_set_style_shadow_width(btn, 0, 0);
	lv_obj_set_style_border_width(btn, 0, 0);
	lv_obj_add_event_cb(btn, pill_clicked_cb, LV_EVENT_CLICKED, (void *)(intptr_t)kind);

	lv_obj_t *label = lv_label_create(btn);
	lv_label_set_text(label, tr(text));
	lv_obj_set_style_text_font(label, &font_ui_22, 0);
	lv_obj_center(label);

	return btn;
}

static void paint_pill(lv_obj_t *btn, bool on) {
	if (!btn) {
		return;
	}
	lv_obj_set_style_bg_color(btn, on ? theme()->accent : theme()->surface_pressed, 0);
	lv_obj_set_style_text_color(lv_obj_get_child(btn, 0), on ? theme_on_accent() : theme()->text_primary, 0);
	lv_obj_set_style_radius(btn, LV_RADIUS_CIRCLE, 0);
}

// The order button wears the order the list is in: the same two glyphs the
// track lists use for their date sorting.
static void paint_order_button(void) {
	if (!order_btn) {
		return;
	}
	lv_obj_t *icon = lv_obj_get_child(order_btn, 0);
	if (icon) {
		lv_image_set_src(icon, podcastpage_episodes_newest_first() ? &icon_sort_date_new : &icon_sort_date_old);
		settingsrow_scale_corner_icon(icon, g_cfg);
	}
}

// Turning a list round: the same episodes, the other way. The array the rows
// read and the copy a tap walks (queued_episodes) both go, so the queue still
// runs towards the older episodes whichever way they are drawn.
static void order_cb(lv_event_t *e) {
	(void)e;
	if (switcher_back_drag_active() || player_sheet_drag_active()) {
		return;
	}
	podcastpage_set_episodes_newest_first(!podcastpage_episodes_newest_first());
	if (list_kind == JOB_EPISODES && result_count > 0) {
		reverse_episodes(result_episodes, result_count);
		reverse_episodes(queued_episodes, queued_count);
		fill_list(0);
	}
	paint_order_button();
}

// The corner buttons' visibility has a single owner, because two owners of one
// flag means whichever runs last wins. The magnifier needs the keys, a network
// and a home view (the two pills). The gear and the downloads need none of
// that and stay up over the notices too: the podcast settings apply to the
// downloaded episodes as well. The order belongs to the episodes view alone.
//
// Slots are handed out from the right to whatever is showing, rather than fixed
// at build time: the order button and the magnifier never share a list, so
// leaving a hole between them would be a hole in the heading's width.
// Every chosen podcast is already followed, so the sheet offers Unfollow and
// not Follow. False for an episode selection, which has nothing to follow.
static bool chosen_all_followed(void) {
	if (!selecting || !chosen || list_kind == JOB_EPISODES) {
		return false;
	}
	bool any = false;
	for (int i = 0; i < list_count && !any; i++) {
		any = chosen[i] != 0;
	}
	if (!any) {
		return false;
	}
	for (int i = 0; i < list_count; i++) {
		if (chosen[i] && !podcastsubs_is_followed(result_feeds[i].id)) {
			return false;
		}
	}
	return true;
}

// The corner's tick is enabled only while something is chosen: a sheet that
// cannot act on anything should not open.
static void paint_selection_actions(void) {
	if (!sel_confirm_btn) {
		return;
	}
	if (!selecting || chosen_count <= 0) {
		lv_obj_add_state(sel_confirm_btn, LV_STATE_DISABLED);
	} else {
		lv_obj_remove_state(sel_confirm_btn, LV_STATE_DISABLED);
	}
}

// Follow all of it, or leave all of it. The sheet has already said which, from
// the same question (chosen_all_followed), so the two cannot disagree.
static void bulk_follow_apply(void *user) {
	(void)user;
	if (!selecting || !chosen || list_kind == JOB_EPISODES) {
		return;
	}
	bool remove_followed = chosen_all_followed();
	bool changed = false;
	for (int i = 0; i < list_count; i++) {
		if (!chosen[i]) {
			continue;
		}
		bool followed = podcastsubs_is_followed(result_feeds[i].id);
		if (remove_followed && followed) {
			podcastsubs_unfollow(result_feeds[i].id);
			changed = true;
		} else if (!remove_followed && !followed) {
			podcastsubs_follow(&result_feeds[i]);
			changed = true;
		}
	}
	if (changed) {
		toast_plain(tr(remove_followed ? "podcast_no_longer_following" : "podcast_now_following"));
	}
	bool was_followed_list = list_kind == JOB_FOLLOWED;
	selection_stop();
	if (was_followed_list) {
		result_kind = JOB_FOLLOWED;
		result_count = load_followed(result_feeds, PODCAST_MAX_HELD);
		fill_list(0);
	}
}

// The tick for every chosen episode, or away from all of them. One write per
// episode: the heard set is a database row each and the mark must survive a
// power cut right after the tap.
static void bulk_mark_heard_cb(void *user) {
	(void)user;
	if (!selecting || !chosen || list_kind != JOB_EPISODES) {
		return;
	}
	for (int i = 0; i < list_count; i++) {
		if (!chosen[i]) {
			continue;
		}
		podcastplayed_set(result_episodes[i].id, true);
	}
	selection_stop();
	for (int i = 0; i < ROW_POOL; i++) {
		if (rows[i].index >= 0) {
			refresh_row(rows[i].index);
		}
	}
}

static void bulk_mark_unheard_cb(void *user) {
	(void)user;
	if (!selecting || !chosen || list_kind != JOB_EPISODES) {
		return;
	}
	for (int i = 0; i < list_count; i++) {
		if (chosen[i]) {
			podcastplayed_set(result_episodes[i].id, false);
		}
	}
	selection_stop();
	for (int i = 0; i < ROW_POOL; i++) {
		if (rows[i].index >= 0) {
			refresh_row(rows[i].index);
		}
	}
}

static void bulk_download_selected_cb(void *user) {
	(void)user;
	if (!selecting || !chosen || list_kind != JOB_EPISODES || bulk_dl_active) {
		return;
	}
	bulk_dl_count = 0;
	for (int i = 0; i < list_count && bulk_dl_count < PODCAST_MAX_HELD; i++) {
		if (chosen[i]) {
			bulk_dl_rows[bulk_dl_count++] = i;
		}
	}
	selection_stop();
	if (bulk_dl_count == 0) {
		return;
	}
	bulk_dl_at = 0;
	bulk_dl_failed = false;
	bulk_dl_active = true;
	bulk_download_next();
}

static void bulk_episode_sheet(lv_obj_t *anchor) {
	if (!selecting || !chosen || list_kind != JOB_EPISODES) {
		return;
	}
	bool has_heard = false;
	bool has_unheard = false;
	for (int i = 0; i < list_count; i++) {
		if (chosen[i]) {
			if (podcastplayed_get(result_episodes[i].id)) {
				has_heard = true;
			} else {
				has_unheard = true;
			}
		}
	}
	popover_item_t actions[3];
	int count = 0;
	if (has_unheard) {
		actions[count++] = (popover_item_t){"podcast_mark_listened", bulk_mark_heard_cb, NULL, false};
	}
	if (has_heard) {
		actions[count++] = (popover_item_t){"podcast_mark_not_listened", bulk_mark_unheard_cb, NULL, false};
	}
	actions[count++] = (popover_item_t){"podcast_download", bulk_download_selected_cb, NULL, false};
	popover_show(anchor, actions, count);
}

// The tick in the corner: the same sheet the episode corner used to open
// directly, and for a selection of podcasts the one action that list has --
// follow, or unfollow when every chosen row is already in My Podcasts.
static void bulk_sheet_cb(lv_event_t *e) {
	lv_event_stop_bubbling(e);
	if (!selecting || !chosen || chosen_count <= 0) {
		return;
	}
	lv_obj_t *anchor = lv_event_get_current_target(e);
	if (list_kind == JOB_EPISODES) {
		bulk_episode_sheet(anchor);
		return;
	}
	popover_item_t action;
	memset(&action, 0, sizeof(action));
	bool unfollow = chosen_all_followed();
	action.label = unfollow ? "podcast_unfollow" : "podcast_follow";
	action.action = bulk_follow_apply;
	popover_show(anchor, &action, 1);
}

static void bulk_close_cb(lv_event_t *e) {
	(void)e;
	selection_stop();
}

static void refresh_corner_buttons(void) {
	bool home_view = list_kind == JOB_FOLLOWED || list_kind == JOB_TRENDING || list_kind == JOB_NONE;
	bool online = podcast_configured() && network_up();
	bool search = online && home_view;
	bool local = home_view || !online;
	bool order = list_kind == JOB_EPISODES;

	// While rows are being chosen the corner belongs to the choice: the tick that
	// confirms it and the way out, the same pair on both kinds of list.
	lv_obj_t *const buttons[] = {settings_btn, saved_btn, search_btn, order_btn, sel_confirm_btn, sel_close_btn};
	const bool shown[] = {local && !selecting, local && !selecting, search && !selecting, order && !selecting,
						  selecting, selecting};
	int slots = 0;
	for (int i = 0; i < 6; i++) {
		if (!buttons[i]) {
			continue;
		}
		if (shown[i]) {
			if (g_cfg) {
				settingsrow_place_corner_button(buttons[i], g_cfg, slots);
			}
			slots++;
			lv_obj_set_hidden(buttons[i], false);
		} else {
			lv_obj_set_hidden(buttons[i], true);
		}
	}
	if (list_title && g_cfg) {
		// The heading gets the width the buttons are not using.
		settingsrow_title_corner_slots(list_title, g_cfg, slots);
	}
	paint_order_button();
	paint_selection_actions();
}

// The pill colours are set by hand, so a theme change does not touch them on
// its own: without this repaint the lit pill keeps the previous accent.
static void refresh_pills(void) {
	paint_pill(pill_followed, list_kind == JOB_FOLLOWED);
	paint_pill(pill_trending, list_kind == JOB_TRENDING);
}

static void build_page(gui_config_t *cfg) {
	g_cfg = cfg;
	const bool compact = cfg->screen_width < 320;
	podcast_screen = lv_obj_create(NULL);
	lv_obj_add_event_cb(podcast_screen, list_loaded_cb, LV_EVENT_SCREEN_LOADED, NULL);
	lv_obj_add_event_cb(podcast_screen, list_unloaded_cb, LV_EVENT_SCREEN_UNLOADED, NULL);
	podcast_list_screen = podcast_screen;
	lv_obj_add_style(podcast_screen, &theme_style_screen, 0);

	// The title follows what is on screen: "Podcasts" on the two home lists, the
	// query on a search, the podcast's name on episodes.
	list_title = settingsrow_title(podcast_screen, cfg, "podcasts");
	settingsrow_title_corner_slots(list_title, cfg, 3);

	int content_top = settingsrow_content_top(cfg);

	// The column: the pills and the list, one above the other. Not
	// settingsrow_page(), because that gives a single container and fill_list()
	// starts it with an lv_obj_clean() -- which would take the pills with it at
	// every fill.
	lv_obj_t *column = lv_obj_create(podcast_screen);
	lv_obj_set_size(column, lv_pct(100), cfg->screen_height - content_top);
	lv_obj_align(column, LV_ALIGN_TOP_LEFT, 0, content_top);
	lv_obj_set_style_bg_opa(column, 0, 0);
	lv_obj_set_style_border_width(column, 0, 0);
	lv_obj_set_style_radius(column, 0, 0);
	lv_obj_set_style_pad_all(column, 0, 0);
	lv_obj_set_style_pad_gap(column, compact ? 6 : 12, 0);
	lv_obj_set_scrollable(column, false);
	lv_obj_set_flex_flow(column, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(column, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

	pill_row = lv_obj_create(column);
	lv_obj_set_size(pill_row, lv_pct(100), LV_SIZE_CONTENT);
	lv_obj_set_style_bg_opa(pill_row, 0, 0);
	lv_obj_set_style_border_width(pill_row, 0, 0);
	lv_obj_set_style_pad_ver(pill_row, 0, 0);
	lv_obj_set_style_pad_hor(pill_row, cfg->padding, 0);
	lv_obj_set_style_pad_gap(pill_row, compact ? 6 : 10, 0);
	// Compact insets keep the shipped translations on one row. A card font can
	// still be unusually wide, however, so retain horizontal scrolling as a
	// fallback instead of clipping the trailing edge of Trending. Larger
	// panels preserve the original non-scrolling selector.
	lv_obj_set_scrollable(pill_row, compact);
	lv_obj_set_scroll_dir(pill_row, LV_DIR_HOR);
	lv_obj_set_scrollbar_mode(pill_row, LV_SCROLLBAR_MODE_OFF);
	lv_obj_set_flex_flow(pill_row, LV_FLEX_FLOW_ROW);
	// Presses that miss a pill belong to the column below, which carries the
	// swipe-back and the player sheet.
	lv_obj_set_event_bubble(pill_row, true);

	pill_followed = make_pill(pill_row, "podcast_my_podcasts", JOB_FOLLOWED, compact);
	pill_trending = make_pill(pill_row, "podcast_trending", JOB_TRENDING, compact);
	refresh_pills();
	theme_register_refresh(refresh_pills);

	list_container = lv_obj_create(column);
	lv_obj_set_width(list_container, lv_pct(100));
	lv_obj_set_flex_grow(list_container, 1);
	lv_obj_set_style_bg_opa(list_container, 0, 0);
	lv_obj_set_style_border_width(list_container, 0, 0);
	lv_obj_set_style_radius(list_container, 0, 0);
	lv_obj_set_style_pad_hor(list_container, cfg->padding, 0);
	lv_obj_set_style_pad_ver(list_container, 0, 0);
	lv_obj_set_scroll_dir(list_container, LV_DIR_VER);

	// The body: no layout, no background, and exactly as tall as the whole list.
	// It is the only child the container has, and the twelve rows are placed
	// inside it by hand -- flex and lv_obj_set_y() cannot both decide where a
	// row goes.
	int row_width = cfg->screen_width - 2 * cfg->padding;
	list_body = lv_obj_create(list_container);
	lv_obj_set_width(list_body, row_width);
	lv_obj_set_height(list_body, ROW_PITCH);
	lv_obj_set_pos(list_body, 0, 0);
	lv_obj_set_style_bg_opa(list_body, 0, 0);
	lv_obj_set_style_border_width(list_body, 0, 0);
	lv_obj_set_style_pad_all(list_body, 0, 0);
	lv_obj_set_scrollable(list_body, false);
	// Presses go row -> body -> container; without this they stop here and the
	// swipes never see a gesture that began on a row.
	lv_obj_set_event_bubble(list_body, true);
	build_rows(row_width);

	// On the screen and not inside the container: it has to sit in the middle of
	// the page, and inside the body it would have to be positioned against a
	// height that changes with the length of a list that, in this case, has no
	// entries at all.
	list_empty = lv_label_create(podcast_screen);
	lv_label_set_text(list_empty, tr("nothing_to_show"));
	lv_label_set_long_mode(list_empty, LV_LABEL_LONG_WRAP);
	lv_obj_set_width(list_empty, cfg->screen_width - 4 * cfg->padding);
	lv_obj_set_style_text_align(list_empty, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_add_style(list_empty, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(list_empty, &font_ui_24, 0);
	// Centred on the panel, with the position left to LVGL. A fixed offset below
	// the heading put a two-line notice low enough to read as a footnote, and on
	// the V1's 320 px it sat against the bottom edge -- which is not what
	// "nothing here yet" should look like.
	lv_obj_align(list_empty, LV_ALIGN_CENTER, 0, 0);
	lv_obj_set_hidden(list_empty, true);

	// The same spot, for when the section cannot work at all.
	note_label = lv_label_create(podcast_screen);
	lv_label_set_text(note_label, "");
	lv_label_set_long_mode(note_label, LV_LABEL_LONG_WRAP);
	lv_obj_set_width(note_label, cfg->screen_width - 2 * cfg->padding);
	lv_obj_add_style(note_label, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(note_label, &font_ui_22, 0);
	lv_obj_set_style_text_align(note_label, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_align(note_label, LV_ALIGN_TOP_LEFT, cfg->padding, content_top + 14);
	lv_obj_set_hidden(note_label, true);

	// The "Wi-Fi settings" row under the network notice, as on the transfer
	// page. Created hidden; show_note puts it under the notice.
	note_wifi_row = settingsrow_add(podcast_screen, "wi_fi_settings", NULL, note_wifi_row_cb, NULL);
	lv_obj_set_width(note_wifi_row, cfg->screen_width - 2 * cfg->padding);
	lv_obj_set_hidden(note_wifi_row, true);

	lv_obj_add_event_cb(list_container, list_scrolled_cb, LV_EVENT_SCROLL, NULL);
	switcher_attach_back_gesture(podcast_screen);
	switcher_attach_back_gesture(list_container);
	// The column covers the whole page and is clickable, as every LVGL object
	// is: without its own handlers it swallows both gestures, and with the
	// notice up -- where the list is hidden and the column is all there is --
	// the swipe back would do nothing at all.
	switcher_attach_back_gesture(column);
	player_sheet_attach_drag(column, true);
	switcher_set_back_guard(podcast_screen, list_back_guard);
	switcher_set_back_guard_peek(podcast_screen, list_back_has_step);
	player_sheet_attach_drag(podcast_screen, true);
	player_sheet_attach_drag(list_container, true);
}

// ---------------------------------------------------------------------------
// the search
// ---------------------------------------------------------------------------

static lv_obj_t *search_field;
static lv_obj_t *search_clear_btn;
static keyboard_t *search_keyboard;
static lv_obj_t *search_history_list;
static lv_obj_t *search_history_empty;

// The last few queries, newest first -- Tidal's history, in the podcast
// section's own part of the configuration file. Eight, because a history is
// for picking up a search from the day before and not for keeping a log.
#define SEARCH_HISTORY_MAX 8
#define SEARCH_HISTORY_LEN 96
#define SEARCH_HISTORY_FONT (bp_is_tempotec_v1() ? &font_ui_18 : &font_ui_24)

static char history_terms[SEARCH_HISTORY_MAX][SEARCH_HISTORY_LEN];
static int history_count;
static bool history_loaded;

static void search_history_rebuild(void);

// Rebuilding deletes the rows, and one of them may be the widget whose tap
// asked for it: the work is queued, so it runs after the event has returned.
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

	int stored = (int)config_get_int("podcast", "search_history_count", 0);
	if (stored < 0) {
		stored = 0;
	}
	if (stored > SEARCH_HISTORY_MAX) {
		stored = SEARCH_HISTORY_MAX;
	}
	for (int i = 0; i < stored; i++) {
		char key[24];
		snprintf(key, sizeof(key), "search_history_%d", i);
		const char *term = config_get("podcast", key, "");
		if (term && term[0]) {
			snprintf(history_terms[history_count], SEARCH_HISTORY_LEN, "%s", term);
			history_count++;
		}
	}
}

static void history_save(void) {
	config_set_int("podcast", "search_history_count", history_count);
	// Past the end the keys are emptied rather than left where they were: a
	// cleared history that a later read finds again is worse than none.
	for (int i = 0; i < SEARCH_HISTORY_MAX; i++) {
		char key[24];
		snprintf(key, sizeof(key), "search_history_%d", i);
		config_set("podcast", key, i < history_count ? history_terms[i] : "");
	}
	config_save();
}

// Puts the term on top, once: a search already in the list moves up instead of
// filling it with the same word repeated.
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

// Typing needs the keyboard, and the keyboard and the history cannot both be on
// the V1's screen: one replaces the other, as on Tidal's search page.
static void search_keyboard_show(bool visible) {
	if (!search_keyboard) {
		return;
	}
	keyboard_set_visible(search_keyboard, visible);
	if (visible) {
		lv_obj_set_hidden(search_history_list, true);
		lv_obj_set_hidden(search_history_empty, true);
	} else {
		search_history_rebuild();
	}
}

static void refresh_search_clear(void) {
	if (!search_clear_btn) {
		return;
	}
	const char *text = lv_textarea_get_text(search_field);
	if (text && text[0]) {
		lv_obj_set_hidden(search_clear_btn, false);
	} else {
		lv_obj_set_hidden(search_clear_btn, true);
	}
}

static void search_field_changed_cb(lv_event_t *e) {
	(void)e;
	refresh_search_clear();
}

static void search_clear_cb(lv_event_t *e) {
	(void)e;
	lv_textarea_set_text(search_field, "");
	keyboard_reset(search_keyboard);
	lv_obj_add_state(search_field, LV_STATE_FOCUSED);
	lv_obj_send_event(search_field, LV_EVENT_FOCUSED, NULL);
	refresh_search_clear();
}

static void search_accept_cb(lv_event_t *e) {
	(void)e;
	const char *text = lv_textarea_get_text(search_field);
	if (!text || !text[0]) {
		return;
	}
	history_remember(text);
	job_t job = {0};
	job.kind = JOB_SEARCH;
	snprintf(job.text, sizeof(job.text), "%s", text);
	// The results get the whole page, as on Tidal: the keyboard would
	// otherwise stay up over a list nobody asked to see with it.
	search_keyboard_show(false);
	run(&job);
}

// Tapping the field brings the keyboard, and the history steps aside for it.
static void search_field_clicked_cb(lv_event_t *e) {
	(void)e;
	if (switcher_back_drag_active()) {
		return;
	}
	search_keyboard_show(true);
	lv_obj_add_state(search_field, LV_STATE_FOCUSED);
	lv_obj_send_event(search_field, LV_EVENT_FOCUSED, NULL);
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

static void history_pick_cb(lv_event_t *e) {
	if (switcher_back_drag_active() || player_sheet_drag_active()) {
		return;
	}
	int index = (int)(intptr_t)lv_event_get_user_data(e);
	if (index < 0 || index >= history_count) {
		return;
	}
	lv_textarea_set_text(search_field, history_terms[index]);
	refresh_search_clear();
	search_accept_cb(e);
}

// Draws the history, in place of the keyboard while nothing is being typed.
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
		lv_obj_set_style_text_font(label, SEARCH_HISTORY_FONT, 0);
		lv_obj_set_flex_grow(label, 1);
		lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
		lv_obj_set_height(label, lv_font_get_line_height(SEARCH_HISTORY_FONT));

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

static void search_loaded_cb(lv_event_t *e) {
	(void)e;
	refresh_search_clear();
	// Back from the results, the keyboard that was sent away stays away and the
	// history takes the space it left.
	if (!keyboard_is_visible(search_keyboard)) {
		search_history_rebuild();
	}
}

// Copied line for line from Tidal's, deliberately: search in this player has
// one shape -- the field with 12 radius, the x on the right edge and the
// shared regular/compact keyboard below. A page that invents another is
// instantly not of this house.
static void build_search_page(gui_config_t *cfg) {
	const bool compact = cfg->screen_width < 320;
	const int field_h = compact ? 44 : 62;
	const int gap = compact ? 6 : 10;
	const int clear_side = compact ? 40 : 56;

	search_screen = lv_obj_create(NULL);
	lv_obj_add_style(search_screen, &theme_style_screen, 0);
	lv_obj_add_event_cb(search_screen, search_loaded_cb, LV_EVENT_SCREEN_LOADED, NULL);

	settingsrow_title(search_screen, cfg, "podcast_search_for_a_podcast");
	int top = settingsrow_content_top(cfg);

	search_field = lv_textarea_create(search_screen);
	lv_textarea_set_one_line(search_field, true);
	lv_textarea_set_placeholder_text(search_field, tr("podcast_name"));
	lv_obj_set_size(search_field, cfg->screen_width - 2 * cfg->padding, field_h);
	lv_obj_set_scrollbar_mode(search_field, LV_SCROLLBAR_MODE_OFF);
	lv_obj_align(search_field, LV_ALIGN_TOP_LEFT, cfg->padding, top);
	lv_obj_add_style(search_field, &theme_style_card, 0);
	lv_obj_set_style_radius(search_field, compact ? 10 : 12, 0);
	lv_obj_set_style_border_width(search_field, 0, 0);
	lv_obj_set_style_shadow_width(search_field, 0, 0);
	lv_obj_set_style_pad_all(search_field, compact ? 8 : 14, 0);
	lv_obj_set_style_text_font(search_field, compact ? &font_ui_18 : &font_ui_24, 0);
	keyboard_style_caret(search_field);
	lv_obj_add_event_cb(search_field, search_field_changed_cb, LV_EVENT_VALUE_CHANGED, NULL);
	// Tapping it brings the keyboard, and the history steps aside for it.
	lv_obj_add_event_cb(search_field, search_field_clicked_cb, LV_EVENT_CLICKED, NULL);

	// The x on the field's right edge: clears the query with one tap. Hidden
	// while the field is empty.
	search_clear_btn = lv_btn_create(search_screen);
	lv_obj_set_size(search_clear_btn, clear_side, clear_side);
	lv_obj_align(search_clear_btn, LV_ALIGN_TOP_RIGHT, -cfg->padding - (compact ? 0 : 4),
				 top + (field_h - clear_side) / 2);
	lv_obj_set_style_bg_opa(search_clear_btn, LV_OPA_TRANSP, 0);
	lv_obj_set_style_shadow_width(search_clear_btn, 0, 0);
	lv_obj_set_style_border_width(search_clear_btn, 0, 0);
	lv_obj_set_style_pad_all(search_clear_btn, 0, 0);
	lv_obj_set_hidden(search_clear_btn, true);
	lv_obj_add_event_cb(search_clear_btn, search_clear_cb, LV_EVENT_CLICKED, NULL);

	lv_obj_t *search_clear_icon = lv_image_create(search_clear_btn);
	lv_image_set_src(search_clear_icon, &icon_clear);
	lv_obj_add_style(search_clear_icon, &theme_style_icon, 0);
	lv_obj_set_style_image_opa(search_clear_icon, LV_OPA_70, 0);
	lv_obj_center(search_clear_icon);

	// Room for it, so a long query does not run underneath.
	lv_obj_set_style_pad_right(search_field, compact ? 42 : 60, 0);

	// No search-type pills, unlike Qobuz and Tidal: there the search runs over
	// tracks, albums and artists, here there is one thing only to search for.
	// Their place under the field is taken by the history.
	const int history_top = top + field_h + gap;
	int history_h = cfg->screen_height - history_top - cfg->padding;
	if (history_h < 40) {
		history_h = 40;
	}

	search_history_list = lv_obj_create(search_screen);
	lv_obj_set_size(search_history_list, cfg->screen_width - 2 * cfg->padding, history_h);
	lv_obj_align(search_history_list, LV_ALIGN_TOP_LEFT, cfg->padding, history_top);
	lv_obj_set_style_bg_opa(search_history_list, LV_OPA_TRANSP, 0);
	lv_obj_set_style_border_width(search_history_list, 0, 0);
	lv_obj_set_style_radius(search_history_list, 0, 0);
	lv_obj_set_style_pad_all(search_history_list, 0, 0);
	lv_obj_set_style_pad_gap(search_history_list, compact ? 4 : 8, 0);
	lv_obj_set_scroll_dir(search_history_list, LV_DIR_VER);
	lv_obj_set_scrollbar_mode(search_history_list, LV_SCROLLBAR_MODE_AUTO);
	lv_obj_set_event_bubble(search_history_list, true);
	lv_obj_set_flex_flow(search_history_list, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(search_history_list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

	search_history_empty = lv_label_create(search_screen);
	lv_label_set_text(search_history_empty, tr("no_searches_yet"));
	lv_obj_add_style(search_history_empty, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(search_history_empty, &font_ui_18, 0);
	lv_obj_set_width(search_history_empty, cfg->screen_width - 2 * cfg->padding);
	lv_label_set_long_mode(search_history_empty, LV_LABEL_LONG_WRAP);
	// Level with where the first history row would be, not a gap under the
	// field: with nothing searched yet this label is the whole history.
	lv_obj_align(search_history_empty, LV_ALIGN_TOP_LEFT, cfg->padding, history_top + 2);
	lv_obj_set_hidden(search_history_empty, true);

	search_keyboard =
		keyboard_create(search_screen, cfg->screen_width, compact ? 144 : 316, search_field, NULL, "ok", search_accept_cb, NULL);

	// The history is what the page opens on; the keyboard arrives with the
	// first tap in the field.
	search_keyboard_show(false);

	switcher_attach_back_gesture(search_screen);
}

// ---------------------------------------------------------------------------
// the pills, and the page state
// ---------------------------------------------------------------------------

// "My podcasts" does NOT go through the worker.
//
// The followed list is in memory (podcastsubs.h reads it from the card once):
// queueing it on a thread means a veil that flashes, a screen change when the
// result comes back, and -- since the page refreshes it at every entry -- a loop
// that bites its own tail: the result changed screen, the screen change started
// the refresh again. There is nothing to wait for here: fill and be done.
//
// Not done while the worker is working: the result arrays are the same ones, and
// rewriting them under it is the one way to get this wrong.
static bool show_followed_now(void) {
	pthread_mutex_lock(&job_lock);
	bool busy = have_pending || result_pending;
	pthread_mutex_unlock(&job_lock);
	if (busy || inflight_valid) {
		return false;
	}

	result_error[0] = '\0';
	result_kind = JOB_FOLLOWED;
	result_count = load_followed(result_feeds, PODCAST_MAX_HELD);
	snprintf(result_title, sizeof(result_title), "%s", tr("podcast_my_podcasts"));
	fill_list(0);

	// This is the section's root: the list stack restarts here.
	job_t root = {0};
	root.kind = JOB_FOLLOWED;
	snprintf(root.text, sizeof(root.text), "%s", result_title);
	current_list_job = root;
	have_current_list = true;
	list_history_len = 0;
	return true;
}

static void run_home_view(job_kind_t kind) {
	if (kind == JOB_FOLLOWED && show_followed_now()) {
		return;
	}
	job_t job = {0};
	job.kind = kind;
	switch (kind) {
	case JOB_TRENDING:
		snprintf(job.text, sizeof(job.text), "%s", tr("podcast_trending"));
		break;
	case JOB_FOLLOWED:
		snprintf(job.text, sizeof(job.text), "%s", tr("podcast_my_podcasts"));
		break;
	default:
		return;
	}
	run_ex(&job, true); // a pill is the root: it clears the stack
}

static void pill_clicked_cb(lv_event_t *e) {
	if (switcher_back_drag_active() || player_sheet_drag_active()) {
		return;
	}
	// If a translation or card font needed the compact row's scrolling
	// fallback, settle the chosen section fully inside the panel.
	lv_obj_scroll_to_view(lv_event_get_current_target(e), LV_ANIM_ON);
	run_home_view((job_kind_t)(intptr_t)lv_event_get_user_data(e));
}

static void open_search_cb(lv_event_t *e) {
	(void)e;
	lv_textarea_set_text(search_field, "");
	keyboard_reset(search_keyboard);
	refresh_search_clear();
	// The history first, unless there is none to show: then the keyboard is
	// what the page opens on, and the first tap has something to do -- the same
	// rule as Tidal's search page.
	history_load();
	search_keyboard_show(history_count == 0);
	switch_screen(search_screen);
}

static void open_settings_cb(lv_event_t *e) {
	(void)e;
	switch_screen(settings_screen);
}

static void open_saved_cb(lv_event_t *e) {
	(void)e;
	switch_screen(podcastsaved_screen);
}

static lv_obj_t *corner_button(gui_config_t *cfg, int slot, const lv_image_dsc_t *glyph, lv_event_cb_t cb) {
	lv_obj_t *button = lv_btn_create(podcast_screen);
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

// Written out rather than borrowed: the function of this name in tidalpage.c is
// a static of that file. Calling it from here compiles on the host, where the
// preprocessor removes the branch that uses it, and does not link on the
// device.
static bool network_up(void) {
#ifdef HOST_BUILD
	return true; // the simulator runs on a machine with a network and no wlan0
#else
	wifi_status_t status;
	wifi_get_status(&status);
	return status.state == WIFI_STATE_CONNECTED && status.ip[0] != '\0';
#endif
}

// The row under the network notice: it leads to the Wi-Fi settings.
static void note_wifi_row_cb(lv_event_t *e) {
	(void)e;
	switch_screen(wifisettings_screen);
}

// Shows the notice in place of everything else, or takes it away. The keys and
// the Wi-Fi can change while the user is elsewhere, so this is rechecked at
// every entry into the page.
static void show_note(const char *text) {
	if (!note_label) {
		return;
	}
	if (text) {
		bool wifi_note = strcmp(text, "enable_wi_fi_first") == 0;
		lv_label_set_text(note_label, tr(text));
		// The Wi-Fi notice uses the NORMAL text colour (as on the other pages);
		// the other notices stay quiet.
		if (wifi_note) {
			lv_obj_set_style_text_color(note_label, theme()->text_primary, 0);
		} else {
			lv_obj_remove_local_style_prop(note_label, LV_STYLE_TEXT_COLOR, 0);
		}
		lv_obj_set_hidden(note_label, false);
		lv_obj_set_hidden(pill_row, true);
		lv_obj_set_hidden(list_container, true);
		lv_obj_set_hidden(list_empty, true);
		// The NETWORK notice brings the "Wi-Fi settings" row with it, as on the
		// transfer page: it says what is missing AND where it is fixed. The other
		// notices (the keys) do not.
		if (note_wifi_row) {
			if (wifi_note) {
				lv_obj_set_hidden(note_wifi_row, false);
				lv_obj_update_layout(note_label); // the notice's real height
				lv_obj_align_to(note_wifi_row, note_label, LV_ALIGN_OUT_BOTTOM_MID, 0, 16);
			} else {
				lv_obj_set_hidden(note_wifi_row, true);
			}
		}
	} else {
		lv_obj_set_hidden(note_label, true);
		if (note_wifi_row) {
			lv_obj_set_hidden(note_wifi_row, true);
		}
		lv_obj_set_hidden(list_container, false);
	}
}

// The network state the page is currently drawn for, so the watcher below can
// tell a change from a poll that found nothing new.
static bool note_network_up;

// Only the notices and the corner buttons: it starts nothing. It is also called
// from the return of a failed job, where starting a job would be an endless
// loop.
static bool refresh_notes(void) {
	if (!note_label) {
		return false;
	}

	// Recorded before anything can return: the watcher below compares against
	// it, and leaving it behind on the path where the keys are missing would
	// have it find a difference on every tick.
	note_network_up = network_up();

	refresh_corner_buttons();

	if (!podcast_configured()) {
		show_note("api_keys_unavailable");
		return false;
	}

	// With no network, the Wi-Fi notice in place of the page: the pills and the
	// lists live off the catalogue, and a page that looks whole but does not
	// answer is worse than a notice saying what is missing.
	if (!note_network_up) {
		show_note("enable_wi_fi_first");
		return false;
	}

	show_note(NULL);
	return true;
}

// A region was chosen, so the chart on screen is the old region's. Re-entering
// the section asks for the new one instead of leaving the old list under a pill
// that now means somewhere else.
static bool trending_stale;

static void refresh_page_state(void) {
	if (!refresh_notes()) {
		return;
	}

	if (!have_current_list) {
		// First entry: start from the followed list, which asks nothing of the
		// network and is there anyway.
		run_home_view(JOB_FOLLOWED);
		return;
	}

	// Re-entering the section rereads the followed list: a podcast may have been
	// followed, or unfollowed, from the player's star while the user was
	// elsewhere.
	if (current_list_job.kind == JOB_FOLLOWED) {
		run_home_view(JOB_FOLLOWED);
		return;
	}

	if (current_list_job.kind == JOB_TRENDING && trending_stale) {
		trending_stale = false;
		run_home_view(JOB_TRENDING);
	}
}

// Where the podcast page keeps its things: the picture cache, the list cache,
// the podcasts' own database. All of it lives on the card, and the card can be
// inserted after the player starts -- a root that was empty at startup used to
// stay empty for the session, which is why the covers were downloaded again at
// every visit. Read again (and only again: these setters reopen a database)
// whenever the card behind them has changed.
static char podcast_root[256];

static void podcast_roots_use(const char *root) {
	if (!root) {
		root = "";
	}
	if (strcmp(podcast_root, root) == 0) {
		return;
	}
	snprintf(podcast_root, sizeof(podcast_root), "%s", root);
	qobuzart_set_root(root);
	listcache_set_root(root);
	podcastcache_set_root(root);
	podcastsubs_set_root(root);
	podcastplayed_set_root(root);
}

static void page_loaded_cb(lv_event_t *e) {
	(void)e;
	podcast_roots_use(storage_sd_root());
	refresh_page_state();
}

// The Wi-Fi can be switched on while this page is already open, so the page
// follows it once a second, as the radio hub does; otherwise the notice stays
// up until the section is left and entered again.
static void wifi_watch_cb(lv_timer_t *timer) {
	(void)timer;
	if (lv_screen_active() != podcast_screen) {
		return;
	}
	bool up = network_up();
	if (up == note_network_up) {
		return;
	}

	if (!up) {
		refresh_notes(); // the network went: the notice takes the page
		return;
	}
	if (!refresh_notes()) {
		return; // the network is back but something else is still missing
	}

	// The network is back, so the list the notice replaced has to be loaded
	// again and not merely uncovered: the notice took the pills down with it,
	// and only filling a list puts them back. Whichever of the two home views
	// was showing is the one to return to; anything deeper was a step whose
	// result the drop threw away.
	run_home_view(current_list_job.kind == JOB_TRENDING ? JOB_TRENDING : JOB_FOLLOWED);
}

// ---------------------------------------------------------------------------
// the podcast settings
//
// One page, with the two skip amounts. SEPARATE from the audiobooks' ones and
// not a link to their page: the two are alike but are not listened to the same
// way, and wanting -10/+30 on a podcast and -30/+10 on a book must not be a
// choice between them. See podcast.h.
// ---------------------------------------------------------------------------

#define SKIP_CHOICES 3

static lv_obj_t *skip_back_choice[SKIP_CHOICES];
static lv_obj_t *skip_forward_choice[SKIP_CHOICES];
static const int SKIP_VALUES[SKIP_CHOICES] = {PODCAST_SKIP_SHORT, PODCAST_SKIP_LONG, PODCAST_SKIP_HUGE};

static void refresh_skip_buttons(void) {
	int back = podcast_skip_back();
	int forward = podcast_skip_forward();
	for (int i = 0; i < SKIP_CHOICES; i++) {
		paint_pill(skip_back_choice[i], SKIP_VALUES[i] == back);
		paint_pill(skip_forward_choice[i], SKIP_VALUES[i] == forward);
	}
}

static void pick_skip_back_cb(lv_event_t *e) {
	if (switcher_back_drag_active()) {
		return;
	}
	podcast_set_skip_back((int)(intptr_t)lv_event_get_user_data(e));
	refresh_skip_buttons();
	player_refresh_now_playing(); // the button it names changes its glyph
}

static void pick_skip_forward_cb(lv_event_t *e) {
	if (switcher_back_drag_active()) {
		return;
	}
	podcast_set_skip_forward((int)(intptr_t)lv_event_get_user_data(e));
	refresh_skip_buttons();
	player_refresh_now_playing();
}

static lv_obj_t *make_skip_choice(lv_obj_t *parent, const char *text, int seconds, lv_event_cb_t cb) {
	lv_obj_t *btn = lv_btn_create(parent);
	// Two cards of three pills, and the V1's 320 px has to hold both without a
	// scrollbar: the pills are sized to the panel there.
	lv_obj_set_size(btn, LV_SIZE_CONTENT, bp_pick(44, 64));
	lv_obj_set_style_pad_hor(btn, bp_pick(14, 22), 0);
	lv_obj_set_style_radius(btn, LV_RADIUS_CIRCLE, 0);
	lv_obj_set_style_shadow_width(btn, 0, 0);
	lv_obj_set_style_border_width(btn, 0, 0);
	lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, (void *)(intptr_t)seconds);

	lv_obj_t *label = lv_label_create(btn);
	lv_label_set_text(label, tr(text));
	lv_obj_set_style_text_font(label, &font_ui_24, 0);
	lv_obj_center(label);
	return btn;
}

// One card per button: its name, then its three pills. The same shape as the
// audiobooks' "Change controls" page, because it is the same choice.
static lv_obj_t *make_skip_card(lv_obj_t *parent, const char *title) {
	lv_obj_t *card = lv_obj_create(parent);
	lv_obj_set_width(card, lv_pct(100));
	lv_obj_set_height(card, LV_SIZE_CONTENT);
	lv_obj_add_style(card, &theme_style_card, 0);
	lv_obj_set_style_radius(card, bp_pick(10, 12), 0);
	lv_obj_set_style_border_width(card, 0, 0);
	lv_obj_set_style_shadow_width(card, 0, 0);
	lv_obj_set_style_pad_all(card, bp_pick(10, 20), 0);
	lv_obj_set_style_pad_gap(card, bp_pick(8, 18), 0);
	lv_obj_set_scrollable(card, false);
	lv_obj_set_event_bubble(card, true);
	lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(card, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

	lv_obj_t *name = lv_label_create(card);
	lv_label_set_text(name, tr(title));
	lv_obj_add_style(name, &theme_style_text, 0);
	lv_obj_set_style_text_font(name, &font_ui_24, 0);

	lv_obj_t *row = lv_obj_create(card);
	lv_obj_set_size(row, lv_pct(100), LV_SIZE_CONTENT);
	lv_obj_set_style_bg_opa(row, 0, 0);
	lv_obj_set_style_border_width(row, 0, 0);
	lv_obj_set_style_pad_all(row, 0, 0);
	lv_obj_set_style_pad_gap(row, 14, 0);
	lv_obj_set_scrollable(row, false);
	lv_obj_set_event_bubble(row, true);
	lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

	return row;
}

static void settings_loaded_cb(lv_event_t *e) {
	(void)e;
	refresh_skip_buttons();
}

// The two skip buttons, on a page of their own. Same arrangement as the
// audiobooks': the settings page lists what the section does, and how the two
// side buttons are laid out is one thing among those rather than the first
// thing anybody reads.
static lv_obj_t *controls_screen;

static void build_controls_page(gui_config_t *cfg) {
	controls_screen = lv_obj_create(NULL);
	lv_obj_t *container = settingsrow_page(controls_screen, cfg, "change_controls");

	lv_obj_t *back_row = make_skip_card(container, "back_button");
	skip_back_choice[0] = make_skip_choice(back_row, "-10", PODCAST_SKIP_SHORT, pick_skip_back_cb);
	skip_back_choice[1] = make_skip_choice(back_row, "-30", PODCAST_SKIP_LONG, pick_skip_back_cb);
	skip_back_choice[2] = make_skip_choice(back_row, "-60", PODCAST_SKIP_HUGE, pick_skip_back_cb);

	lv_obj_t *forward_row = make_skip_card(container, "forward_button");
	skip_forward_choice[0] = make_skip_choice(forward_row, "+10", PODCAST_SKIP_SHORT, pick_skip_forward_cb);
	skip_forward_choice[1] = make_skip_choice(forward_row, "+30", PODCAST_SKIP_LONG, pick_skip_forward_cb);
	skip_forward_choice[2] = make_skip_choice(forward_row, "+60", PODCAST_SKIP_HUGE, pick_skip_forward_cb);

	refresh_skip_buttons();
	theme_register_refresh(refresh_skip_buttons);
	lv_obj_add_event_cb(controls_screen, settings_loaded_cb, LV_EVENT_SCREEN_LOADED, NULL);
	switcher_attach_back_gesture(controls_screen);
}

// The order the followed list is drawn in. Three pills, like the skip amounts:
// the choice is a property of the reader and it is made once, so it belongs on
// the settings page rather than in a corner of the list -- where the V1's
// heading has room for three buttons and no room for a fourth.
static lv_obj_t *order_choice[3];

static void refresh_order_buttons(void) {
	podcast_sort_t order = podcastpage_subs_sort();
	for (int i = 0; i < 3; i++) {
		paint_pill(order_choice[i], (int)order == i);
	}
}

static void pick_order_cb(lv_event_t *e) {
	if (switcher_back_drag_active()) {
		return;
	}
	podcastpage_set_subs_sort((podcast_sort_t)(intptr_t)lv_event_get_user_data(e));
	refresh_order_buttons();
	// The list itself is sorted on the way back: entering the section re-reads
	// it (list_loaded_cb), and reading it is what puts it in this order.
}

// The three order pills, and they are the same pills every other settings page
// uses. They were their own thing -- taller, with a bigger face and their own
// paddings -- which on a page of shared cards read as one card drawn by
// somebody else. The shared pill passes its value as the event's user data,
// which is what the old callback read.
// The text is a translation key, so it has to go through settingsrow_pill(),
// which translates it -- settingsrow_pill_text() is the variant for literal
// labels ("MP3", "320", "feat.") and would leave the tags on screen.
static lv_obj_t *make_order_pill(lv_obj_t *parent, const char *text, podcast_sort_t order) {
	return settingsrow_pill(parent, text, (int)order, pick_order_cb);
}

// The card the three pills sit in. The row wraps, unlike the skip amounts':
// three pills at the V1's sizes are wider than its 200 usable pixels, and a row
// that could not wrap would push the last one off the card.
static void build_order_card(lv_obj_t *parent) {
	lv_obj_t *card = lv_obj_create(parent);
	lv_obj_set_width(card, lv_pct(100));
	lv_obj_set_height(card, LV_SIZE_CONTENT);
	lv_obj_add_style(card, &theme_style_card, 0);
	lv_obj_set_style_radius(card, 12, 0);
	lv_obj_set_style_border_width(card, 0, 0);
	lv_obj_set_style_shadow_width(card, 0, 0);
	lv_obj_set_style_pad_all(card, 20, 0);
	lv_obj_set_style_pad_gap(card, bp_pick(8, 18), 0);
	lv_obj_set_scrollable(card, false);
	lv_obj_set_event_bubble(card, true);
	lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(card, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

	lv_obj_t *name = lv_label_create(card);
	lv_label_set_text(name, tr("podcast_order"));
	lv_obj_add_style(name, &theme_style_text, 0);
	lv_obj_set_style_text_font(name, &font_ui_24, 0);

	lv_obj_t *row = lv_obj_create(card);
	lv_obj_set_size(row, lv_pct(100), LV_SIZE_CONTENT);
	lv_obj_set_style_bg_opa(row, 0, 0);
	lv_obj_set_style_border_width(row, 0, 0);
	lv_obj_set_style_pad_all(row, 0, 0);
	lv_obj_set_style_pad_gap(row, bp_pick(6, 14), 0);
	lv_obj_set_style_pad_row(row, bp_pick(6, 14), 0);
	lv_obj_set_scrollable(row, false);
	lv_obj_set_event_bubble(row, true);
	lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW_WRAP);
	lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

	order_choice[PODCAST_SORT_PLAYED] = make_order_pill(row, "podcast_order_played", PODCAST_SORT_PLAYED);
	order_choice[PODCAST_SORT_AZ] = make_order_pill(row, "podcast_order_az", PODCAST_SORT_AZ);
	order_choice[PODCAST_SORT_ZA] = make_order_pill(row, "podcast_order_za", PODCAST_SORT_ZA);
}

// ---------------------------------------------------------------------------
// where the chart comes from
//
// The catalogue's trending list takes one filter that means "somewhere": a
// language. There is no country parameter to ask for -- that was checked against
// its documentation rather than assumed -- so the region offered here is its
// language, with Worldwide first: every region page in the player -- this one,
// Radio, YouTube Music and the sort order here -- writes the same shared choice
// in system/core/region.h, and reads it back from it, so there is one list and
// one default instead of five copies. Choosing one changes which chart the
// Trending pill shows, and the copy on the card is dropped with it.
// ---------------------------------------------------------------------------

// The shared list, read here on every rebuild rather than cached: the choice
// may have been made on another page of the same session.
static region_t region_rows[REGION_LIST_MAX];
static int region_count;

static lv_obj_t *region_screen;
static lv_obj_t *region_container;
static lv_obj_t *region_value; // the row on the settings page, showing the choice

// A row's value keeps the box it was given when the row was first laid out; a
// language chosen later is drawn inside that box. Anchoring the label to the
// right edge after every change is what keeps "Polski" against the row's edge
// instead of leaving it at the left of a box sized for a longer name.
static void region_value_set(const char *label) {
	if (!region_value) {
		return;
	}
	lv_label_set_text(region_value, label);
	lv_obj_set_style_text_align(region_value, LV_TEXT_ALIGN_RIGHT, 0);
	lv_obj_align(region_value, LV_ALIGN_RIGHT_MID, 0, 0);
}

static void region_pick_cb(lv_event_t *e);

// The row whose language this is. Nobody's choice reads as the first row,
// which region.h keeps as Worldwide.
static int region_index_of(const char *code) {
	for (int i = 0; i < region_count; i++) {
		if (strcmp(region_rows[i].language, code ? code : "") == 0) {
			return i;
		}
	}
	return 0;
}

// A built-in region names itself in its own language -- Polski, not Polish --
// and arrives from region.h already final; the rest are translation keys.
static const char *region_row_label(int i) {
	return region_rows[i].translated ? region_rows[i].label : tr(region_rows[i].label);
}

static void region_rows_rebuild(void) {
	if (!region_container) {
		return;
	}
	int keep_scroll = (int)lv_obj_get_scroll_y(region_container);
	lv_obj_clean(region_container);

	region_count = region_list(region_rows, REGION_LIST_MAX);
	int active = region_index_of(podcast_trending_lang());
	for (int i = 0; i < region_count; i++) {
		lv_obj_t *value = NULL;
		lv_obj_t *row = settingsrow_add(region_container, region_row_label(i), &value, region_pick_cb,
										(void *)(intptr_t)i);
		if (i == active) {
			// The chosen one, and only it: a tick says which row the chart is
			// coming from without a second word on every row.
			settingsrow_add_icon(row, &icon_check, THEME_SEMANTIC_GREEN);
		}
	}
	lv_obj_update_layout(region_container);
	lv_obj_scroll_to_y(region_container, keep_scroll, LV_ANIM_OFF);
}

static void region_pick_cb(lv_event_t *e) {
	if (switcher_back_drag_active()) {
		return;
	}
	int index = (int)(intptr_t)lv_event_get_user_data(e);
	if (index < 0 || index >= region_count) {
		return;
	}
	// The shared choice, so Radio, YouTube Music and the sort order here all
	// move together; the podcast chart drops its copy with it.
	podcast_set_trending_lang(region_rows[index].language);
	listcache_drop(podcast_trending_cache_key());
	trending_stale = true;
	region_value_set(region_row_label(index));
	region_rows_rebuild();
}

static void region_loaded_cb(lv_event_t *e) {
	(void)e;
	region_rows_rebuild();
}

static void open_region_cb(lv_event_t *e) {
	(void)e;
	switch_screen(region_screen);
}

static void build_region_page(gui_config_t *cfg) {
	region_screen = lv_obj_create(NULL);
	region_container = settingsrow_page(region_screen, cfg, "podcast_region");
	lv_obj_add_event_cb(region_screen, region_loaded_cb, LV_EVENT_SCREEN_LOADED, NULL);
	switcher_attach_back_gesture(region_screen);
}

static lv_obj_t *stop_episode_switch;
static settingsrow_duration_t sleep_row;

static void refresh_settings_rows(void) {
	region_count = region_list(region_rows, REGION_LIST_MAX);
	region_value_set(region_row_label(region_index_of(podcast_trending_lang())));
	if (stop_episode_switch) {
		if (podcast_stop_at_episode_end()) {
			lv_obj_add_state(stop_episode_switch, LV_STATE_CHECKED);
		} else {
			lv_obj_remove_state(stop_episode_switch, LV_STATE_CHECKED);
		}
	}
	// The control centre switches this timer too, so the switch and the
	// wheels are read again rather than trusted from the last visit.
	settingsrow_duration_set_minutes(&sleep_row, sleeptimer_minutes(SLEEPTIMER_PODCAST));
	settingsrow_duration_expanded(&sleep_row, sleeptimer_enabled(SLEEPTIMER_PODCAST));
	settingsrow_duration_repaint(&sleep_row);
}

static void settings_rows_loaded_cb(lv_event_t *e) {
	(void)e;
	refresh_settings_rows();
	refresh_order_buttons();
}

static void stop_episode_cb(lv_event_t *e) {
	podcast_set_stop_at_episode_end(lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED));
}

static void sleep_toggle_cb(lv_event_t *e) {
	(void)e;
	sleeptimer_set_enabled(SLEEPTIMER_PODCAST, lv_obj_has_state(sleep_row.toggle, LV_STATE_CHECKED));
	refresh_settings_rows();
}

static void sleep_wheel_cb(lv_event_t *e) {
	(void)e;
	sleeptimer_set_minutes(SLEEPTIMER_PODCAST, settingsrow_duration_minutes(&sleep_row));
}

static void build_settings_page(gui_config_t *cfg) {
	build_controls_page(cfg);
	build_region_page(cfg);

	settings_screen = lv_obj_create(NULL);
	lv_obj_t *container = settingsrow_page(settings_screen, cfg, "podcast_settings");

	settingsrow_add(container, "change_controls", NULL, switch_screen_cb, controls_screen);
	settingsrow_add(container, "podcast_region", &region_value, open_region_cb, NULL);
	build_order_card(container);
	settingsrow_toggle(container, "podcast_stop_at_end_of_episode", &stop_episode_switch, stop_episode_cb);
	// Its own, and not the music one: a stretch set for falling asleep to a
	// podcast should not be counting down over an album the next morning. The
	// module switched off in Plugins takes the row away from every page.
	if (addons_enabled("sleep_timer")) {
		settingsrow_toggle_duration(container, "sleep_timer", sleep_toggle_cb, sleep_wheel_cb, &sleep_row);
	}
	settingsrow_duration_set_minutes(&sleep_row, sleeptimer_minutes(SLEEPTIMER_PODCAST));

	refresh_settings_rows();
	refresh_order_buttons();
	theme_register_refresh(refresh_settings_rows);
	theme_register_refresh(refresh_order_buttons);
	lv_obj_add_event_cb(settings_screen, settings_rows_loaded_cb, LV_EVENT_SCREEN_LOADED, NULL);
	// And when the control centre switches the timer with the page under it.
	lv_obj_add_event_cb(settings_screen, settings_rows_loaded_cb, LV_EVENT_REFRESH, NULL);
	switcher_attach_back_gesture(settings_screen);
}

// ---------------------------------------------------------------------------
// from outside
// ---------------------------------------------------------------------------

lv_obj_t *podcastpage_settings_screen(void) { return settings_screen; }

bool podcastpage_open_feed(long long feed_id, const char *title) {
	if (feed_id <= 0 || !podcast_configured()) {
		return false;
	}
	// Arriving from the player's "Show podcast", neither the author nor the
	// feed's cover is known: they are cleared rather than carried over from the
	// last podcast browsed, which would belong to another.
	browsing_author[0] = '\0';
	browsing_image[0] = '\0';

	job_t job = {0};
	job.kind = JOB_EPISODES;
	job.id = feed_id;
	job.want = PODCAST_EPISODES_STEP;
	snprintf(job.text, sizeof(job.text), "%s", title && title[0] ? title : tr("podcasts"));
	run(&job);
	return true;
}

// Once a second: protects from pruning the episodes the queue still has ahead,
// and clears the waiting flag when the queue has moved on to something that is
// not a podcast.
static void queue_watch_cb(lv_timer_t *timer) {
	(void)timer;
	// A queue that is a library list holds no cache files, and walking it here
	// would be a database read per entry, once a second, for nothing. The
	// bookkeeping below still runs: a download already in flight keeps the
	// radio awake whether or not the queue moved off it.
	bool library_queue = playlist_is_library_backed();
	if (library_queue) {
		podcastcache_set_protected(NULL, 0);
		waiting_for_episode = false;
	}

	int count = playlist_count();
	int current = playlist_current_index();

	static char queued[PODCASTCACHE_PROTECTED_MAX][512];
	static const char *queued_ptr[PODCASTCACHE_PROTECTED_MAX];
	int protect = 0;
	for (int i = current; !library_queue && i >= 0 && i < count && protect < PODCASTCACHE_PROTECTED_MAX; i++) {
		if (playlist_path_at(i, queued[protect], sizeof(queued[0]))) {
			queued_ptr[protect] = queued[protect];
			protect++;
		}
	}
	if (!library_queue) {
		podcastcache_set_protected(queued_ptr, protect);
	}

	if (waiting_for_episode) {
		char wanted[512];
		if (current < 0 || !playlist_path_at(current, wanted, sizeof(wanted)) || !podcastcache_owns(wanted)) {
			waiting_for_episode = false;
		}
	}

	// Nothing further is asked of the network while the Wi-Fi transfer is on:
	// the episode would land on a card a server outside this process is
	// writing, and the wait it feeds is a wait to start playing, which is what
	// switching the transfer on put a stop to. The queued files stay protected
	// above either way -- they are still the queue.
	pthread_mutex_lock(&fetch_lock);
	if (wifitransfer_get_enabled()) {
		waiting_for_episode = false;
		fetch_want_id = 0;
	}
	bool downloading = fetch_want_id != 0;
	pthread_mutex_unlock(&fetch_lock);
	podcastcache_set_network_wanted(downloading || waiting_for_episode);
}

void podcastpage_init(gui_config_t *cfg) {
	for (int i = 0; i < QOBUZART_SLOTS; i++) {
		slot_owner[i] = -1;
	}
	for (int i = 0; i < PODCAST_MAX_HELD; i++) {
		row_image_source[i] = -1;
	}

	build_page(cfg);

	// Slots are counted from the edge leftwards: the gear against it, the
	// downloads beside it, the magnifier last.
	settings_btn = corner_button(cfg, 0, &icon_music_settings, open_settings_cb);
	saved_btn = corner_button(cfg, 1, &icon_import, open_saved_cb);
	search_btn = corner_button(cfg, 2, &icon_search, open_search_cb);
	// Its slot is decided by refresh_corner_buttons() on every list, since it
	// is the only button that shares its list with nothing.
	order_btn = corner_button(cfg, 3, &icon_sort_date_new, order_cb);
	// The tick confirms the selection and opens the sheet of what can be done
	// with it; the x abandons it. The plain check and not the circled one: at
	// the corner's size the ring fills the button and reads as a stray disc.
	sel_close_btn = corner_button(cfg, 0, &icon_close, bulk_close_cb);
	sel_confirm_btn = corner_button(cfg, 1, &icon_check, bulk_sheet_cb);
	lv_obj_set_hidden(sel_confirm_btn, true);
	lv_obj_set_hidden(sel_close_btn, true);

	build_search_page(cfg);
	build_settings_page(cfg);
	podcastsaved_init(cfg);
	dl_build(cfg);
	build_busy_layer();
	podcastcache_set_slow_start_cb(NULL);

	device_state_add_prepare_cb(podcastpage_prepare_track);
	fetch_start_thread();

	lv_timer_create(queue_watch_cb, 1000, NULL);
	lv_timer_create(wifi_watch_cb, 1000, NULL);
	art_drop_timer = lv_timer_create(art_drop_cb, ART_DROP_DELAY_MS, NULL);
	lv_timer_pause(art_drop_timer);
	art_timer = lv_timer_create(art_poll_cb, 120, NULL);
	lv_timer_pause(art_timer);
	podcast_roots_use(storage_sd_root());

	// Notices only: the first list is asked for by the page when it really
	// opens, not at player startup -- at startup the card may not have been read
	// yet and the Wi-Fi may not be up.
	refresh_notes();

	lv_obj_add_event_cb(podcast_screen, page_loaded_cb, LV_EVENT_SCREEN_LOADED, NULL);
}
