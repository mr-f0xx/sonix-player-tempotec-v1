#include "podcastsaved.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "src/gui/board_profile.h"
#include "src/gui/fonts/fonts.h"
#include "src/gui/nowplaying/cover.h"
#include "src/gui/nowplaying/player.h"
#include "src/gui/streaming/podcastpage.h"
#include "src/gui/shell/confirm.h"
#include "src/gui/shell/icons.h"
#include "src/gui/shell/popover.h"
#include "src/gui/shell/settingsrow.h"
#include "src/gui/shell/switcher.h"
#include "src/gui/shell/theme.h"
#include "src/gui/shell/toast.h"
#include "src/system/core/lang.h"
#include "src/system/library/metadata.h"
#include "src/system/library/tagdate.h"
#include "src/system/playback/device_state.h"
#include "src/system/playback/playlist.h"
#include "src/system/streaming/podcastcache.h"
#include "src/system/streaming/podcastdl.h"
#include "src/system/streaming/podcastplayed.h"

#define SAVED_ROW_HEIGHT bp_pick(60, 88)
#define SAVED_THUMB bp_pick(44, 60)
#define SAVED_MAX_SHOWS 128
#define SAVED_MAX_EPISODES 400

lv_obj_t *podcastsaved_screen;
static lv_obj_t *episodes_screen;

static gui_config_t *g_cfg;
static lv_obj_t *shows_list;
static lv_obj_t *shows_empty;
static lv_obj_t *episodes_list;
static lv_obj_t *episodes_title;
static lv_obj_t *order_btn;

typedef struct {
	char name[256];
	int episodes;
	cover_image_t thumb;
} show_t;

typedef struct {
	char *path;
	// The title and date the row shows. They are resolved once per read of the
	// card, because the answer comes from different places: the downloader's
	// sidecar when there is one, else the file's own tags, else the file's
	// name and the file's mtime. See episodes_read().
	char title[256];
	tagdate_t date; // what the date came to, at the precision it had
	time_t when;	// the same date as one moment: what the list sorts by
	long long episode_id; // 0 when the file names no episode (copied by hand)
	bool heard;			  // the mark the episode list shows
} episode_t;

static show_t *shows;
static int show_count;
static episode_t episodes[SAVED_MAX_EPISODES];
static int episode_count;
static char open_show[256];
static cover_image_t episode_thumb;
static int menu_episode = -1;

// ---------------------------------------------------------------------------
// reading the card
// ---------------------------------------------------------------------------

static int count_episodes(const char *folder) {
	DIR *dir = opendir(folder);
	if (!dir) {
		return 0;
	}
	int n = 0;
	struct dirent *de;
	while ((de = readdir(dir)) != NULL) {
		if (de->d_name[0] != '.' && playlist_is_playable_file(de->d_name)) {
			n++;
		}
	}
	closedir(dir);
	return n;
}

static int show_cmp(const void *a, const void *b) {
	return strcasecmp(((const show_t *)a)->name, ((const show_t *)b)->name);
}

static void shows_free(void) {
	for (int i = 0; i < show_count; i++) {
		cover_free(&shows[i].thumb);
	}
	show_count = 0;
}

// Every folder under Podcast holding at least one episode, alphabetically.
static void shows_read(void) {
	shows_free();
	if (!shows) {
		shows = calloc(SAVED_MAX_SHOWS, sizeof(*shows));
		if (!shows) {
			return;
		}
	}
	const char *root = podcastdl_root();
	DIR *dir = root[0] ? opendir(root) : NULL;
	if (!dir) {
		return;
	}
	struct dirent *de;
	while ((de = readdir(dir)) != NULL && show_count < SAVED_MAX_SHOWS) {
		if (de->d_name[0] == '.' || strlen(de->d_name) >= sizeof(shows[0].name)) {
			continue;
		}
		char path[800];
		snprintf(path, sizeof(path), "%s/%s", root, de->d_name);
		struct stat st;
		if (stat(path, &st) != 0 || !S_ISDIR(st.st_mode)) {
			continue;
		}
		int n = count_episodes(path);
		if (n == 0) {
			continue;
		}
		show_t *s = &shows[show_count++];
		memset(s, 0, sizeof(*s));
		snprintf(s->name, sizeof(s->name), "%s", de->d_name);
		s->episodes = n;
	}
	closedir(dir);
	qsort(shows, (size_t)show_count, sizeof(*shows), show_cmp);
}

static int episode_cmp(const void *a, const void *b) {
	time_t x = ((const episode_t *)a)->when, y = ((const episode_t *)b)->when;
	return x < y ? 1 : x > y ? -1 : 0;
}

static void episodes_free(void) {
	for (int i = 0; i < episode_count; i++) {
		free(episodes[i].path);
	}
	episode_count = 0;
	cover_free(&episode_thumb);
}

// The list read the other way round. A swap of whole entries: the path each
// one owns travels with it, so nothing is freed or copied.
static void episodes_reverse(void) {
	for (int i = 0, j = episode_count - 1; i < j; i++, j--) {
		episode_t swap = episodes[i];
		episodes[i] = episodes[j];
		episodes[j] = swap;
	}
}

// The file name without its folder and extension: the episode's title when
// nothing better is on the card.
static void episode_name(const char *path, char *out, size_t size) {
	const char *slash = strrchr(path, '/');
	snprintf(out, size, "%s", slash ? slash + 1 : path);
	char *dot = strrchr(out, '.');
	if (dot && dot != out) {
		*dot = '\0';
	}
}

// The title and date an episode's row shows, resolved from whatever the card
// carries.
//
// A downloaded episode has the sidecar the downloader wrote beside it: that
// is the catalogue speaking, and it wins -- a podcast enclosure is often an
// MP3 carrying another episode's tags, or no tags at all -- and the file's
// mtime is its publication date, stamped there by podcastdl.c when the
// download landed. The sidecar is asked only for the title; the date is the
// mtime, full and reliable.
//
// A file put on the card by hand has no sidecar, and its own tags speak: the
// title tag, and the date tag at whatever precision it carries ("2026",
// "2026-03-12", "2026-03-12T08:00:00"). What the tags leave out falls back to
// the file's name and mtime -- the copy's date is better than no date, but
// worse than the one the file was published with.
static void episode_row_read(const char *path, const struct stat *st, episode_t *ep) {
	// Which episode this is, and whether it has been heard. The id comes from
	// the sidecar for an episode kept for good, and from the file name in the
	// cache; a file copied onto the card by hand names no episode, and so has
	// no mark either.
	ep->episode_id = podcastcache_episode_id(path);
	ep->heard = podcastplayed_get(ep->episode_id);

	char sidecar[1200];
	snprintf(sidecar, sizeof(sidecar), "%.1100s.tags", path);
	struct stat side_st;
	if (stat(sidecar, &side_st) == 0) {
		if (!podcastcache_tag(path, "title", ep->title, sizeof(ep->title)) || !ep->title[0]) {
			episode_name(path, ep->title, sizeof(ep->title));
		}
		ep->when = st->st_mtime;
		struct tm tm_when;
		if (localtime_r(&ep->when, &tm_when)) {
			ep->date.year = tm_when.tm_year + 1900;
			ep->date.month = tm_when.tm_mon + 1;
			ep->date.day = tm_when.tm_mday;
		}
		return;
	}

	song_metadata_t meta;
	metadata_read(path, &meta);
	if (meta.title[0]) {
		snprintf(ep->title, sizeof(ep->title), "%s", meta.title);
	} else {
		episode_name(path, ep->title, sizeof(ep->title));
	}
	if (meta.date[0] && tagdate_parse(meta.date, &ep->date)) {
		ep->when = tagdate_to_time(&ep->date);
	}
	if (ep->when <= 0) {
		// No date tag, or one that names no moment (a year past 2037 does
		// not fit the device's 32-bit time_t). The mtime sorts the row; the
		// tag's date, when there was one, still shows.
		ep->when = st->st_mtime;
		if (!ep->date.year) {
			struct tm tm_when;
			if (localtime_r(&ep->when, &tm_when)) {
				ep->date.year = tm_when.tm_year + 1900;
				ep->date.month = tm_when.tm_mon + 1;
				ep->date.day = tm_when.tm_mday;
			}
		}
	}
}

// The episodes of the open podcast, newest first: podcastdl.c dates each file
// to the episode's publication, and a hand-copied file is dated by its tags.
static void episodes_read(void) {
	episodes_free();
	char folder[800];
	snprintf(folder, sizeof(folder), "%s/%s", podcastdl_root(), open_show);
	DIR *dir = opendir(folder);
	if (!dir) {
		return;
	}
	struct dirent *de;
	while ((de = readdir(dir)) != NULL && episode_count < SAVED_MAX_EPISODES) {
		if (de->d_name[0] == '.' || !playlist_is_playable_file(de->d_name)) {
			continue;
		}
		char path[1100];
		snprintf(path, sizeof(path), "%s/%s", folder, de->d_name);
		struct stat st;
		if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) {
			continue;
		}
		char *copy = strdup(path);
		if (!copy) {
			break;
		}
		episode_t *ep = &episodes[episode_count];
		memset(ep, 0, sizeof(*ep)); // a fresh row: no stale title from the last fill
		ep->path = copy;
		episode_row_read(path, &st, ep);
		episode_count++;
	}
	closedir(dir);
	qsort(episodes, (size_t)episode_count, sizeof(*episodes), episode_cmp);
	// episode_cmp() puts the newest on top, which is the catalogue's own order
	// and the one the page has always had. Oldest first is that list read the
	// other way round: no second compare, no second read of the card.
	if (!podcastpage_episodes_newest_first()) {
		episodes_reverse();
	}
	cover_thumb_load(folder, true, SAVED_THUMB, &episode_thumb);
}

// ---------------------------------------------------------------------------
// the rows
// ---------------------------------------------------------------------------

// "12 March 2026", "March 2026" or "2026", in the interface language (see
// podcastpage.c): a date tag may name only the year, and printing "1 January
// 2026" would say more than the file does. The dates taken from an mtime are
// whole days and read as they always did.
static void format_date(const tagdate_t *date, char *out, size_t size) {
	static const char *const MONTHS[12] = {"podcast_january", "podcast_february", "podcast_march",
										   "podcast_april",   "podcast_may",	  "podcast_june",
										   "podcast_july",   "podcast_august",  "podcast_september",
										   "podcast_october", "podcast_november", "podcast_december"};
	if (!date->year) {
		out[0] = '\0';
		return;
	}
	if (!date->month) {
		snprintf(out, size, "%d", date->year);
		return;
	}
	const char *month = tr(MONTHS[(date->month - 1) % 12]);
	if (!date->day) {
		snprintf(out, size, "%s %d", month, date->year);
		return;
	}
	snprintf(out, size, "%d %s %d", date->day, month, date->year);
}

static void paint_thumb(lv_obj_t *thumb, const cover_image_t *image) {
	if (image && image->pixels) {
		lv_image_set_src(thumb, &image->dsc);
		lv_obj_set_style_radius(thumb, 6, 0);
		lv_obj_set_style_clip_corner(thumb, true, 0);
		return;
	}
	lv_image_set_src(thumb, &icon_podcast_list);
	lv_obj_add_style(thumb, &theme_style_icon, 0);
	lv_obj_set_style_image_recolor_opa(thumb, LV_OPA_COVER, 0);
}

static lv_obj_t *make_row(lv_obj_t *parent, const cover_image_t *image, const char *title, const char *detail,
						  lv_event_cb_t cb, lv_event_cb_t long_cb, int index, bool heard) {
	lv_obj_t *row = lv_btn_create(parent);
	lv_obj_set_size(row, lv_pct(100), SAVED_ROW_HEIGHT);
	lv_obj_add_style(row, &theme_style_card, 0);
	lv_obj_add_style(row, &theme_style_card_pressed, LV_STATE_PRESSED);
	lv_obj_set_style_radius(row, bp_pick(10, 12), 0);
	lv_obj_set_style_border_width(row, 0, 0);
	lv_obj_set_style_shadow_width(row, 0, 0);
	lv_obj_set_style_pad_hor(row, bp_pick(8, 16), 0);
	lv_obj_set_style_pad_ver(row, 0, 0);
	lv_obj_set_style_pad_column(row, bp_pick(8, 12), 0);
	lv_obj_set_scrollable(row, false);
	lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_event_bubble(row, true);
	lv_obj_add_event_cb(row, cb, LV_EVENT_CLICKED, (void *)(intptr_t)index);
	if (long_cb) {
		lv_obj_add_event_cb(row, long_cb, LV_EVENT_LONG_PRESSED, (void *)(intptr_t)index);
	}

	lv_obj_t *thumb = lv_image_create(row);
	lv_obj_set_size(thumb, SAVED_THUMB, SAVED_THUMB);
	lv_image_set_inner_align(thumb, LV_IMAGE_ALIGN_CENTER);
	lv_obj_set_event_bubble(thumb, true);
	paint_thumb(thumb, image);

	lv_obj_t *texts = lv_obj_create(row);
	lv_obj_remove_style_all(texts);
	lv_obj_set_flex_grow(texts, 1);
	lv_obj_set_height(texts, LV_SIZE_CONTENT);
	lv_obj_set_style_pad_row(texts, 6, 0);
	lv_obj_set_scrollable(texts, false);
	lv_obj_set_event_bubble(texts, true);
	lv_obj_set_flex_flow(texts, LV_FLEX_FLOW_COLUMN);

	lv_obj_t *name = lv_label_create(texts);
	lv_label_set_text(name, title);
	lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
	lv_obj_set_width(name, lv_pct(100));
	lv_obj_set_height(name, lv_font_get_line_height(&font_ui_22));
	lv_obj_add_style(name, &theme_style_text, 0);
	lv_obj_set_style_text_font(name, &font_ui_22, 0);

	lv_obj_t *sub = NULL;
	if (detail && detail[0]) {
		sub = lv_label_create(texts);
		lv_label_set_text(sub, detail);
		lv_label_set_long_mode(sub, LV_LABEL_LONG_DOT);
		lv_obj_set_width(sub, lv_pct(100));
		lv_obj_set_height(sub, lv_font_get_line_height(&font_ui_18));
		lv_obj_add_style(sub, &theme_style_text_dim, 0);
		lv_obj_set_style_text_font(sub, &font_ui_18, 0);
	}

	// An episode heard to its end wears the tick the online list gives it, and
	// its two lines go dim: the same mark on both lists, so a podcast heard
	// halfway through the day reads the same whether it is on the card or not.
	if (heard) {
		lv_obj_t *tick = lv_image_create(row);
		lv_obj_set_size(tick, bp_pick(22, 30), bp_pick(22, 30));
		lv_image_set_src(tick, &icon_check);
		lv_obj_add_style(tick, &theme_style_icon, 0);
		lv_obj_set_event_bubble(tick, true);
		lv_obj_set_style_text_opa(name, LV_OPA_50, 0);
		if (sub) {
			lv_obj_set_style_text_opa(sub, LV_OPA_50, 0);
		}
	}
	return row;
}

// ---------------------------------------------------------------------------
// the podcasts
// ---------------------------------------------------------------------------

static void show_clicked_cb(lv_event_t *e) {
	if (switcher_back_drag_active() || player_sheet_drag_active()) {
		return;
	}
	int i = (int)(intptr_t)lv_event_get_user_data(e);
	if (i < 0 || i >= show_count) {
		return;
	}
	snprintf(open_show, sizeof(open_show), "%s", shows[i].name);
	switch_screen(episodes_screen);
}

static void shows_fill(void) {
	lv_obj_clean(shows_list);
	shows_read();
	for (int i = 0; i < show_count; i++) {
		char folder[800];
		snprintf(folder, sizeof(folder), "%s/%s", podcastdl_root(), shows[i].name);
		cover_thumb_load(folder, true, SAVED_THUMB, &shows[i].thumb);
		char detail[64];
		if (shows[i].episodes == 1) {
			snprintf(detail, sizeof(detail), "%s", tr("podcast_one_episode"));
		} else {
			snprintf(detail, sizeof(detail), tr("podcast_episodes_2"), shows[i].episodes);
		}
		make_row(shows_list, &shows[i].thumb, shows[i].name, detail, show_clicked_cb, NULL, i, false);
	}
	if (show_count == 0) {
		lv_obj_set_hidden(shows_empty, false);
	} else {
		lv_obj_set_hidden(shows_empty, true);
	}
}

static void shows_loaded_cb(lv_event_t *e) {
	(void)e;
	shows_fill();
}

// The rows go and the thumbnails with them: the page is read again on return.
static void shows_unloaded_cb(lv_event_t *e) {
	(void)e;
	lv_obj_clean(shows_list);
	shows_free();
}

// ---------------------------------------------------------------------------
// the episodes of one podcast
// ---------------------------------------------------------------------------

static void episodes_fill(void);
static void paint_order_button(void);
static void episode_long_cb(lv_event_t *e);

// The episodes in the order shown, from the one tapped onwards into the past,
// as the online list plays them.
static void episode_clicked_cb(lv_event_t *e) {
	if (switcher_back_drag_active() || player_sheet_drag_active()) {
		return;
	}
	int i = (int)(intptr_t)lv_event_get_user_data(e);
	if (i < 0 || i >= episode_count) {
		return;
	}
	const char *paths[SAVED_MAX_EPISODES];
	for (int k = 0; k < episode_count; k++) {
		paths[k] = episodes[k].path;
	}
	device_state_play_list_ordered(paths, episode_count, i);
	player_refresh_now_playing();
	switch_screen(player_screen);
}

// A folder left with nothing but its artwork goes too.
static void remove_folder_if_empty(const char *folder) {
	if (count_episodes(folder) > 0) {
		return;
	}
	char cover[900];
	snprintf(cover, sizeof(cover), "%s/cover.jpg", folder);
	remove(cover);
	snprintf(cover, sizeof(cover), "%s/cover.png", folder);
	remove(cover);
	rmdir(folder);
}

static void delete_confirmed(void *user) {
	(void)user;
	if (menu_episode < 0 || menu_episode >= episode_count) {
		return;
	}
	if (remove(episodes[menu_episode].path) != 0) {
		toast_error(tr("card_write_failed"));
		return;
	}
	char tags[1200];
	snprintf(tags, sizeof(tags), "%s.tags", episodes[menu_episode].path);
	remove(tags);
	sync();
	char folder[800];
	snprintf(folder, sizeof(folder), "%s/%s", podcastdl_root(), open_show);
	remove_folder_if_empty(folder);
	menu_episode = -1;
	episodes_fill();
	if (episode_count == 0) {
		switch_screen_return_to(podcastsaved_screen);
	}
}

static void delete_cb(void *user) {
	(void)user;
	if (menu_episode < 0 || menu_episode >= episode_count) {
		return;
	}
	confirm_show("podcast_delete_download", episodes[menu_episode].title, "delete", delete_confirmed, NULL);
}

// ---------------------------------------------------------------------------
// heard to the end, or by hand
// ---------------------------------------------------------------------------

// The rows only, no card: a mark changes one entry, and re-reading the folder
// for it would also throw away the covers a scroll just fetched.
static void episodes_rows(void) {
	lv_obj_clean(episodes_list);
	for (int i = 0; i < episode_count; i++) {
		char when[48];
		format_date(&episodes[i].date, when, sizeof(when));
		make_row(episodes_list, &episode_thumb, episodes[i].title, when, episode_clicked_cb, episode_long_cb, i,
				 episodes[i].heard);
	}
}

static void mark_heard_cb(void *user) {
	(void)user;
	if (menu_episode < 0 || menu_episode >= episode_count) {
		return;
	}
	podcastplayed_set(episodes[menu_episode].episode_id, true);
	episodes[menu_episode].heard = podcastplayed_get(episodes[menu_episode].episode_id);
	episodes_rows();
}

static void mark_not_heard_cb(void *user) {
	(void)user;
	if (menu_episode < 0 || menu_episode >= episode_count) {
		return;
	}
	podcastplayed_set(episodes[menu_episode].episode_id, false);
	episodes[menu_episode].heard = podcastplayed_get(episodes[menu_episode].episode_id);
	episodes_rows();
}

static void episode_long_cb(lv_event_t *e) {
	int i = (int)(intptr_t)lv_event_get_user_data(e);
	if (i < 0 || i >= episode_count) {
		return;
	}
	menu_episode = i;
	// The lift that ends this press would otherwise arrive as a click and
	// start the episode.
	lv_indev_t *indev = lv_indev_active();
	if (indev) {
		lv_indev_wait_release(indev);
	}
	static const popover_item_t HEARD[] = {{"podcast_mark_not_listened", mark_not_heard_cb, NULL},
										  {"delete", delete_cb, NULL}};
	static const popover_item_t NOT_HEARD[] = {{"podcast_mark_listened", mark_heard_cb, NULL},
										  {"delete", delete_cb, NULL}};
	// A file copied onto the card by hand names no episode, so there is no id to
	// keep the mark under and the menu offers only the one thing left to do.
	static const popover_item_t DELETE_ONLY[] = {{"delete", delete_cb, NULL}};
	if (episodes[i].episode_id <= 0) {
		popover_show(lv_event_get_current_target(e), DELETE_ONLY, 1);
	} else {
		popover_show(lv_event_get_current_target(e), episodes[i].heard ? HEARD : NOT_HEARD, 2);
	}
}

static void episodes_fill(void) {
	episodes_read();
	episodes_rows();
	lv_label_set_text(episodes_title, open_show);
	settingsrow_title_corner_slots(episodes_title, g_cfg, 1);
	paint_order_button();
}

static void episodes_loaded_cb(lv_event_t *e) {
	(void)e;
	episodes_fill();
}

static void episodes_unloaded_cb(lv_event_t *e) {
	(void)e;
	lv_obj_clean(episodes_list);
	episodes_free();
}

// ---------------------------------------------------------------------------
// the order of the list
//
// The same two glyphs, and the same setting, as the online episode list: the
// order is a property of the user's reading, not of one screen, so turning it
// round on a podcast's page finds the downloads the same way round.
// ---------------------------------------------------------------------------

static void paint_order_button(void) {
	if (!order_btn) {
		return;
	}
	lv_obj_t *icon = lv_obj_get_child(order_btn, 0);
	if (icon) {
		lv_image_set_src(icon, podcastpage_episodes_newest_first() ? &icon_sort_date_new : &icon_sort_date_old);
	}
}

static void order_cb(lv_event_t *e) {
	(void)e;
	if (switcher_back_drag_active() || player_sheet_drag_active()) {
		return;
	}
	podcastpage_set_episodes_newest_first(!podcastpage_episodes_newest_first());
	// The list is already read: turn it and draw it, rather than going back to
	// the card for the same files in the other order.
	if (episode_count > 1) {
		episodes_reverse();
		episodes_rows();
	}
	paint_order_button();
}

static lv_obj_t *corner_button(gui_config_t *cfg, lv_obj_t *parent, int slot, const lv_image_dsc_t *glyph,
							 lv_event_cb_t cb) {
	lv_obj_t *button = lv_btn_create(parent);
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

// ---------------------------------------------------------------------------
// building
// ---------------------------------------------------------------------------

void podcastsaved_init(gui_config_t *cfg) {
	g_cfg = cfg;

	podcastsaved_screen = lv_obj_create(NULL);
	shows_list = settingsrow_page(podcastsaved_screen, cfg, "podcast_on_card");
	lv_obj_set_flex_align(shows_list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
	player_sheet_attach_drag(shows_list, true);
	switcher_attach_back_gesture(podcastsaved_screen);
	lv_obj_add_event_cb(podcastsaved_screen, shows_loaded_cb, LV_EVENT_SCREEN_LOADED, NULL);
	lv_obj_add_event_cb(podcastsaved_screen, shows_unloaded_cb, LV_EVENT_SCREEN_UNLOADED, NULL);

	shows_empty = lv_label_create(podcastsaved_screen);
	lv_label_set_text(shows_empty, tr("podcast_on_card_empty"));
	lv_label_set_long_mode(shows_empty, LV_LABEL_LONG_WRAP);
	lv_obj_set_width(shows_empty, cfg->screen_width - 4 * cfg->padding);
	lv_obj_set_style_text_align(shows_empty, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_add_style(shows_empty, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(shows_empty, &font_ui_24, 0);
	lv_obj_align(shows_empty, LV_ALIGN_TOP_MID, 0, settingsrow_content_top(cfg) + 140);
	lv_obj_set_hidden(shows_empty, true);

	episodes_screen = lv_obj_create(NULL);
	episodes_list = settingsrow_page(episodes_screen, cfg, "podcasts");
	lv_obj_set_flex_align(episodes_list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
	episodes_title = settingsrow_page_title(episodes_screen);
	// The order button sits in the heading of the download list too: a podcast
	// kept on the card is read the same way round as one streamed.
	settingsrow_title_corner_slots(episodes_title, cfg, 1);
	order_btn = corner_button(cfg, episodes_screen, 0, &icon_sort_date_new, order_cb);
	player_sheet_attach_drag(episodes_list, true);
	switcher_attach_back_gesture(episodes_screen);
	lv_obj_add_event_cb(episodes_screen, episodes_loaded_cb, LV_EVENT_SCREEN_LOADED, NULL);
	lv_obj_add_event_cb(episodes_screen, episodes_unloaded_cb, LV_EVENT_SCREEN_UNLOADED, NULL);
}
