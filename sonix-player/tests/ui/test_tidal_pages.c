/*
 * Tidal's "Favourite playlists" on the real renderer: the row on the main
 * page, the list behind it, what a row's three dots and a selection offer
 * there, and what reaches the account when one of them is taken.
 *
 * What Tidal is asked, and how its answer is read, is checked elsewhere
 * (test_tidal_favplaylists.c drives the real client against canned replies).
 * Here the client is the part substituted: an account with one playlist of
 * its own and a few it only keeps as favourites, which answers at once and
 * writes down everything it is asked. All that sits on top of it is real --
 * the page with its worker thread and its history, the module that writes to
 * the account on a thread of its own, the lists kept on the card, the rows,
 * the popover and the configuration file.
 *
 * Three runs, because the arrangement of the main page is read once per
 * process: with nothing saved; with what a build from before the row existed
 * had saved ("saved"); and with what this build saves ("arranged").
 */
#define _GNU_SOURCE /* nftw(), for clearing the scratch card away */
#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <ftw.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "src/gui/board_profile.h"
#include "src/gui/fonts/fonts.h"
#include "src/gui/library/medialist.h"
#include "src/gui/nowplaying/cover.h"
#include "src/gui/nowplaying/player.h"
#include "src/gui/shell/confirm.h"
#include "src/gui/shell/gui.h"
#include "src/gui/shell/icons.h"
#include "src/gui/shell/popover.h"
#include "src/gui/shell/settingsrow.h"
#include "src/gui/shell/switcher.h"
#include "src/gui/shell/theme.h"
#include "src/gui/shell/toast.h"
#include "src/gui/streaming/qobuzart.h"
#include "src/gui/streaming/stockkeys_ui.h"
#include "src/gui/streaming/tidalpage.h"
#include "src/gui/wireless/wifisettings.h"
#include "src/system/core/config.h"
#include "src/system/core/lang.h"
#include "src/system/core/utils.h"
#include "src/system/decode/growfile.h"
#include "src/system/device/power.h"
#include "src/system/device/system.h"
#include "src/system/net/wifi.h"
#include "src/system/net/wifitransfer.h"
#include "src/system/playback/device_state.h"
#include "src/system/playback/playlist.h"
#include "src/system/streaming/listcache.h"
#include "src/system/streaming/streamkeep.h"
#include "src/system/streaming/streamturn.h"
#include "src/system/streaming/tidal.h"
#include "src/system/streaming/tidalcache.h"

/* ---- fonts, as in the other page tests ---------------------------------- */

lv_font_t font_ui_14, font_ui_16, font_ui_18, font_ui_20, font_ui_20_bold;
lv_font_t font_ui_22, font_ui_22_bold, font_ui_24, font_ui_24_bold;
lv_font_t font_ui_26, font_ui_28, font_ui_32;
lv_font_t font_ui_36_bold, font_ui_64_bold, font_ui_72;

static void init_fonts(void) {
	lv_font_t *all[] = {
		&font_ui_14, &font_ui_16, &font_ui_18, &font_ui_20, &font_ui_20_bold, &font_ui_22, &font_ui_22_bold,
		&font_ui_24, &font_ui_24_bold, &font_ui_26, &font_ui_28, &font_ui_32, &font_ui_36_bold,
		&font_ui_64_bold, &font_ui_72,
	};
	for (size_t i = 0; i < sizeof(all) / sizeof(all[0]); i++) {
		*all[i] = lv_font_montserrat_14;
	}
}

/* ---- the interface thread's inbox ---------------------------------------- */

/* Both of the page's threads hand their answers over through gui_post(). The
 * player runs them from its main loop; the test runs them when it says so,
 * and waits for exactly as many as the tap it has just made must produce --
 * one too few and the wait below fails, one too many and it is still here
 * at the end. */
typedef struct {
	void (*cb)(void *);
	void *user;
} posted_t;

#define POSTED_MAX 32
static posted_t posted[POSTED_MAX];
static int posted_head;
static int posted_count;
static pthread_mutex_t posted_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t posted_arrived = PTHREAD_COND_INITIALIZER;

bool gui_post(void (*cb)(void *), void *user) {
	pthread_mutex_lock(&posted_lock);
	assert(posted_count < POSTED_MAX);
	posted[(posted_head + posted_count) % POSTED_MAX] = (posted_t){cb, user};
	posted_count++;
	pthread_cond_signal(&posted_arrived);
	pthread_mutex_unlock(&posted_lock);
	return true;
}

static void run_posted(int wanted, const char *what) {
	for (int i = 0; i < wanted; i++) {
		struct timespec until;
		clock_gettime(CLOCK_REALTIME, &until);
		until.tv_sec += 20;
		pthread_mutex_lock(&posted_lock);
		while (posted_count == 0) {
			if (pthread_cond_timedwait(&posted_arrived, &posted_lock, &until) == ETIMEDOUT) {
				fprintf(stderr, "%s: answer %d of %d never reached the interface thread\n", what, i + 1, wanted);
				assert(!"the page did not answer");
			}
		}
		posted_t next = posted[posted_head];
		posted_head = (posted_head + 1) % POSTED_MAX;
		posted_count--;
		pthread_mutex_unlock(&posted_lock);
		next.cb(next.user);
	}
	lv_obj_update_layout(lv_screen_active());
}

static int posted_waiting(void) {
	pthread_mutex_lock(&posted_lock);
	int count = posted_count;
	pthread_mutex_unlock(&posted_lock);
	return count;
}

/* ---- the shell the page leans on, substituted ----------------------------- */

/* Every notice that names a failure, kept: what the account refuses in this
 * test is all there may be of them. A single row's refusal comes as a popup,
 * a selection's as a toast. */
static int popups;
static char last_popup[256];
void gui_notify_popup(const char *text) {
	popups++;
	snprintf(last_popup, sizeof(last_popup), "%s", text ? text : "");
}
static int error_toasts;
static char last_error_toast[256];
void toast_error(const char *text) {
	error_toasts++;
	snprintf(last_error_toast, sizeof(last_error_toast), "%s", text ? text : "");
}

/* The question a destructive row asks. The real card is not built: what is
 * checked is what it would say, and that nothing is done until it is
 * answered. */
static struct {
	bool open;
	char title[64];
	char message[512];
	char ok[32];
	void (*on_ok)(void *);
	void *user;
} asked;

void confirm_show(const char *title, const char *message, const char *ok_label, void (*on_ok)(void *user),
				  void *user) {
	asked.open = true;
	snprintf(asked.title, sizeof(asked.title), "%s", title ? title : "");
	snprintf(asked.message, sizeof(asked.message), "%s", message ? message : "");
	snprintf(asked.ok, sizeof(asked.ok), "%s", ok_label ? ok_label : "");
	asked.on_ok = on_ok;
	asked.user = user;
}

static void expect_asked(const char *title, const char *message, const char *ok) {
	assert(asked.open);
	if (strcmp(asked.title, title) != 0 || strcmp(asked.message, message) != 0 || strcmp(asked.ok, ok) != 0) {
		fprintf(stderr, "asked \"%s\" / \"%s\" / \"%s\"\n  not \"%s\" / \"%s\" / \"%s\"\n", asked.title,
				asked.message, asked.ok, title, message, ok);
		assert(!"the wrong question was asked");
	}
}

static void answer(bool yes) {
	assert(asked.open);
	asked.open = false;
	if (yes && asked.on_ok) {
		asked.on_ok(asked.user);
	}
}

/* Screens, stacked as the player's own switcher stacks them, and "back" asked
 * of the page first: the list page keeps a history of its own and only lets
 * go of the screen when that has run out. */
static lv_obj_t *screen_stack[8];
static int screen_depth;

static struct {
	lv_obj_t *screen;
	bool (*handler)(void);
} back_guards[4];
static int back_guard_count;

void switch_screen(lv_obj_t *target_screen) {
	if (target_screen == lv_screen_active()) {
		return;
	}
	assert(screen_depth < 8);
	screen_stack[screen_depth++] = lv_screen_active();
	lv_screen_load(target_screen);
}

void switcher_set_back_guard(lv_obj_t *screen, bool (*handler)(void)) {
	assert(back_guard_count < 4);
	back_guards[back_guard_count].screen = screen;
	back_guards[back_guard_count].handler = handler;
	back_guard_count++;
}

/* True when the page kept the step for itself. */
static bool go_back(void) {
	for (int i = 0; i < back_guard_count; i++) {
		if (back_guards[i].screen == lv_screen_active() && back_guards[i].handler()) {
			return true;
		}
	}
	assert(screen_depth > 0);
	lv_screen_load(screen_stack[--screen_depth]);
	lv_obj_update_layout(lv_screen_active());
	return false;
}

lv_obj_t *player_screen;
lv_obj_t *wifisettings_screen;

void stockkeys_ui_populate_missing(lv_obj_t *home_container, void (*on_ready)(void)) {
	(void)home_container;
	(void)on_ready;
	assert(!"the keys are there in this test");
}

/* ---- the player under the page: a card, a network, nothing playing -------- */

static const char *card_root;
const char *storage_sd_root(void) { return card_root; }

void wifi_get_status(wifi_status_t *out) {
	memset(out, 0, sizeof(*out));
	out->state = WIFI_STATE_CONNECTED;
	snprintf(out->ssid, sizeof(out->ssid), "Home");
	snprintf(out->ip, sizeof(out->ip), "192.168.1.20");
}

void format_row_duration(int seconds, char *out, size_t size) {
	snprintf(out, size, "%d:%02d", seconds / 60, seconds % 60);
}

void device_state_get(device_state_t *out) { memset(out, 0, sizeof(*out)); }

void device_state_play_list(const char *const *list, int count, int start_index) {
	(void)list;
	(void)count;
	(void)start_index;
	assert(!"nothing is played in this test");
}

/* No pictures: every cover asked for is answered "there is none". */
bool qobuzart_take(const void *owner, int slot, cover_image_t *out, bool *finished) {
	(void)owner;
	(void)slot;
	(void)out;
	*finished = true;
	return false;
}

/* And no track on the card. Signing out empties the playback cache, which is
 * counted. */
static int cache_clears;
void tidalcache_clear(void) { cache_clears++; }
void tidalcache_progress(long *done, long *total) {
	*done = 0;
	*total = 0;
}
void tidalcache_path(long track_id, const char *mime, char *out, size_t size) {
	(void)track_id;
	(void)mime;
	if (out && size) {
		out[0] = '\0';
	}
}

/* The rest is asked and has nothing to say: no gesture under way, nothing
 * playing or queued, no picture to draw, no download running. The arguments
 * go unread on purpose. */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
void fonts_register_change(void (*cb)(void)) {}
void bootlogo_set(int choice, bool dark) {}
bool bootpic_present(void) { return false; }
void toast_success(const char *text) {}
void power_hold_screen_on(bool hold) {}
void switcher_attach_back_gesture(lv_obj_t *screen) {}
bool switcher_back_drag_active(void) { return false; }
void switcher_set_back_guard_peek(lv_obj_t *screen, bool (*peek)(void)) {}
void player_refresh_now_playing(void) {}
void player_sheet_attach_drag(lv_obj_t *obj, bool opening) {}
bool player_sheet_drag_active(void) { return false; }
void medialist_open_paths(const char *title, const char *const *paths, const char *const *names,
						  const char *const *artists, int count, const char *playlist) {}
bool wifi_get_enabled(void) { return true; }
bool wifitransfer_get_enabled(void) { return false; }
void thread_be_background(const char *name) {}
void device_state_add_prepare_cb(device_state_prepare_cb cb) {}
bool device_state_play_queue_index(int index) { return false; }
void device_state_stop(void) {}
int playlist_count(void) { return 0; }
int playlist_current_index(void) { return -1; }
bool playlist_is_library_backed(void) { return false; }
bool playlist_is_playable_file(const char *name) { return false; }
bool playlist_path_at(int index, char *out, size_t out_size) { return false; }
bool growfile_is_growing(const char *path) { return false; }
bool growfile_prefix_is_growing(const char *prefix) { return false; }
bool streamkeep_busy(void) { return false; }
bool streamkeep_keep(const char *path, bool notify_result) { return false; }
bool streamkeep_last_error(char *out, size_t out_size) { return false; }
void streamturn_abandon_all(void) {}
void cover_free(cover_image_t *img) {}
void qobuzart_set_root(const char *sd_root) {}
void qobuzart_request(const void *owner, const char *folder, int slot, const char *url, int size) {}
void qobuzart_release(const void *owner, int slot) {}
void tidalcache_set_root(const char *sd_root) {}
void tidalcache_set_slow_start_cb(void (*cb)(int seconds)) {}
void tidalcache_set_network_wanted(bool wanted) {}
void tidalcache_set_protected(const char *const *paths, int count) {}
bool tidalcache_ready(void) { return false; }
void tidalcache_abandon_all(void) {}
void tidalcache_wait_idle(void) {}
long tidalcache_downloading_id(void) { return 0; }
const char *tidalcache_last_error(void) { return ""; }
void tidalcache_note_played(const char *path) {}
bool tidalcache_owns(const char *path) { return false; }
long tidalcache_track_id(const char *path) { return 0; }
bool tidalcache_cancel_background_download(long track_id) { return false; }
bool tidalcache_download_error(long track_id, char *out, size_t out_size) { return false; }
bool tidalcache_downloading_path(long track_id, char *out, size_t out_size) { return false; }
bool tidalcache_find(long track_id, char *out, size_t size) { return false; }
bool tidalcache_start(long track_id, const tidal_stream_t *stream, int duration_secs, bool foreground, char *out,
					  size_t size) {
	return false;
}
void tidalcache_write_sidecars_at(const char *path, const char *title, const char *artist, const char *album,
								  const char *album_id, int track_number, int duration_secs,
								  const char *cover_url) {}
#pragma GCC diagnostic pop

/* ---- the account, substituted and recording ------------------------------ */

#define FAV_JAZZ "6a0e1c2d-0001-4f6b-8a11-5d2c7e9b0a01"
#define FAV_DEEP "6a0e1c2d-0002-4f6b-8a11-5d2c7e9b0a02"
#define FAV_ROAD "6a0e1c2d-0003-4f6b-8a11-5d2c7e9b0a03"
#define FAV_NIGHT "6a0e1c2d-0004-4f6b-8a11-5d2c7e9b0a04"
#define FAV_OTHER "f2b8a6d4-0005-47c1-b5e9-0a3d6c8e1f05"
#define OWN_MORNING "c41d7b90-0009-4e2a-9f33-1b6a8d5c2e09"

/* Three threads come here -- the page's worker for the lists, the account
 * module's for the writes, the test's own to look -- so everything below is
 * read and written under one lock. */
static pthread_mutex_t account_lock = PTHREAD_MUTEX_INITIALIZER;

static bool signed_in = true;
static tidal_playlist_t favourites[8];
static int favourite_count;
static tidal_playlist_t own_lists[2];
static int own_count;

static int fav_requests;   /* the favourite playlists, asked of Tidal */
static int own_requests;   /* the account's own */
static int track_requests; /* a playlist's tracks */
static char tracks_of[TIDAL_ID_MAX];

#define REMOVED_MAX 8
static char removed[REMOVED_MAX][TIDAL_ID_MAX]; /* every un-favouriting, in the order sent */
static int removals;
static const char *refusal; /* when set, the account refuses them with this */
static int other_writes;	/* anything else that changes the account */
static int sign_outs;

static void put_playlist(tidal_playlist_t *p, const char *id, const char *name, const char *owner, int tracks) {
	memset(p, 0, sizeof(*p));
	snprintf(p->id, sizeof(p->id), "%s", id);
	snprintf(p->name, sizeof(p->name), "%s", name);
	snprintf(p->owner, sizeof(p->owner), "%s", owner);
	snprintf(p->image, sizeof(p->image), "https://resources.tidal.com/images/%.8s/320x320.jpg", id);
	p->track_count = tracks;
}

/* The favourites as tidal.c hands them over: an editorial playlist under
 * Tidal's name, an artist's under the artist's, and another listener's with
 * no name at all (see read_playlist() and test_tidal_favplaylists.c). */
static void account_fill(void) {
	pthread_mutex_lock(&account_lock);
	put_playlist(&favourites[0], FAV_JAZZ, "Jazz Now", "TIDAL", 50);
	put_playlist(&favourites[1], FAV_DEEP, "Deep Cuts", "Miles D.", 1);
	put_playlist(&favourites[2], FAV_ROAD, "Road Trip", "", 12);
	put_playlist(&favourites[3], FAV_NIGHT, "Night Drive", "TIDAL", 30);
	favourite_count = 4;
	put_playlist(&own_lists[0], OWN_MORNING, "Morning", "", 7);
	own_count = 1;
	pthread_mutex_unlock(&account_lock);
}

static void account_refuses(const char *why) {
	pthread_mutex_lock(&account_lock);
	refusal = why;
	pthread_mutex_unlock(&account_lock);
}

static int counted(const int *counter) {
	pthread_mutex_lock(&account_lock);
	int value = *counter;
	pthread_mutex_unlock(&account_lock);
	return value;
}

/* The `at`-th un-favouriting sent since the test began was for this one. */
static void expect_removed(int at, const char *id) {
	pthread_mutex_lock(&account_lock);
	bool same = at < removals && strcmp(removed[at], id) == 0;
	pthread_mutex_unlock(&account_lock);
	assert(same);
}

static void expect_tracks_of(const char *id) {
	pthread_mutex_lock(&account_lock);
	bool same = strcmp(tracks_of, id) == 0;
	pthread_mutex_unlock(&account_lock);
	assert(same);
}

bool tidal_configured(void) { return true; }
bool tidal_logged_in(void) {
	pthread_mutex_lock(&account_lock);
	bool in = signed_in;
	pthread_mutex_unlock(&account_lock);
	return in;
}
const char *tidal_display_name(void) { return "listener"; }
const char *tidal_last_error(void) {
	pthread_mutex_lock(&account_lock);
	const char *why = refusal ? refusal : "";
	pthread_mutex_unlock(&account_lock);
	return why;
}
void tidal_logout(void) {
	pthread_mutex_lock(&account_lock);
	signed_in = false;
	sign_outs++;
	pthread_mutex_unlock(&account_lock);
}

static int page_of(const tidal_playlist_t *from, int have, int offset, tidal_playlist_t *out, int max) {
	int n = 0;
	for (int i = offset; i >= 0 && i < have && n < max; i++) {
		out[n++] = from[i];
	}
	return n;
}

int tidal_favorite_playlists(int offset, tidal_playlist_t *out, int max) {
	pthread_mutex_lock(&account_lock);
	fav_requests++;
	int n = page_of(favourites, favourite_count, offset, out, max);
	pthread_mutex_unlock(&account_lock);
	return n;
}

int tidal_user_playlists(int offset, tidal_playlist_t *out, int max) {
	pthread_mutex_lock(&account_lock);
	own_requests++;
	int n = page_of(own_lists, own_count, offset, out, max);
	pthread_mutex_unlock(&account_lock);
	return n;
}

/* Two tracks, whichever playlist is asked about: what matters is which one
 * was. */
int tidal_playlist_tracks(const char *playlist_id, int offset, tidal_track_t *out, int max) {
	pthread_mutex_lock(&account_lock);
	track_requests++;
	snprintf(tracks_of, sizeof(tracks_of), "%s", playlist_id ? playlist_id : "");
	pthread_mutex_unlock(&account_lock);
	if (offset > 0 || max < 2) {
		return 0;
	}
	memset(out, 0, 2 * sizeof(*out));
	out[0].id = 501;
	snprintf(out[0].title, sizeof(out[0].title), "So What");
	snprintf(out[0].artist, sizeof(out[0].artist), "Miles Davis");
	out[0].duration = 562;
	out[0].streamable = true;
	out[1].id = 502;
	snprintf(out[1].title, sizeof(out[1].title), "Blue in Green");
	snprintf(out[1].artist, sizeof(out[1].artist), "Miles Davis");
	out[1].duration = 337;
	out[1].streamable = true;
	return 2;
}

bool tidal_favorite_remove_playlist(const char *playlist_id) {
	pthread_mutex_lock(&account_lock);
	assert(removals < REMOVED_MAX);
	snprintf(removed[removals++], TIDAL_ID_MAX, "%s", playlist_id ? playlist_id : "");
	bool ok = refusal == NULL;
	for (int i = 0; ok && i < favourite_count; i++) {
		if (strcmp(favourites[i].id, playlist_id) == 0) {
			memmove(&favourites[i], &favourites[i + 1], (size_t)(favourite_count - i - 1) * sizeof(favourites[0]));
			favourite_count--;
			break;
		}
	}
	pthread_mutex_unlock(&account_lock);
	return ok;
}

/* Every other write the client can make. None of them belongs to anything
 * done here -- least of all the deletion, which is what a favourited playlist
 * must never be sent. */
static bool other_write(void) {
	pthread_mutex_lock(&account_lock);
	other_writes++;
	pthread_mutex_unlock(&account_lock);
	return true;
}

static int quality = TIDAL_QUALITY_LOSSLESS;
int tidal_get_quality(void) { return quality; }
void tidal_set_quality(int value) { quality = value; }
const char *tidal_expected_mime(void) { return "audio/flac"; }
int tidal_cover_px(void) { return 320; }
void tidal_cover_url(const char *uuid, int size_px, char *out, size_t out_size) {
	(void)uuid;
	(void)size_px;
	if (out && out_size) {
		out[0] = '\0';
	}
}

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
bool tidal_playlist_delete(const char *playlist_id) { return other_write(); }
bool tidal_playlist_remove_track(const char *playlist_id, int index) { return other_write(); }
bool tidal_playlist_add_track(const char *playlist_id, long track_id) { return other_write(); }
bool tidal_playlist_create(const char *name, char *out_id, size_t out_size) { return other_write(); }
bool tidal_favorite_add_track(long track_id) { return other_write(); }
bool tidal_favorite_remove_track(long track_id) { return other_write(); }
bool tidal_favorite_add_album(const char *album_id) { return other_write(); }
bool tidal_favorite_remove_album(const char *album_id) { return other_write(); }
bool tidal_favorite_add_artist(long artist_id) { return other_write(); }
bool tidal_favorite_remove_artist(long artist_id) { return other_write(); }

/* The rest of the catalogue is empty: no favourite tracks, albums or artists
 * (the page reads the last two on a visit, to know what a row's menu should
 * offer), no search results, no showcases, nothing to stream and nobody
 * signing in. */
int tidal_favorite_track_ids(long *out, int max) { return 0; }
int tidal_favorite_tracks(int offset, tidal_track_t *out, int max) { return 0; }
int tidal_favorite_albums(int offset, tidal_album_t *out, int max) { return 0; }
int tidal_favorite_artists(int offset, tidal_artist_t *out, int max) { return 0; }
int tidal_search_tracks(const char *query, int offset, tidal_track_t *out, int max) { return 0; }
int tidal_search_albums(const char *query, int offset, tidal_album_t *out, int max) { return 0; }
int tidal_search_artists(const char *query, int offset, tidal_artist_t *out, int max) { return 0; }
int tidal_album_tracks(const char *album_id, int offset, tidal_track_t *out, int max) { return 0; }
int tidal_artist_albums(long artist_id, int offset, tidal_album_t *out, int max) { return 0; }
int tidal_featured_albums(const char *type, int offset, tidal_album_t *out, int max) { return 0; }
bool tidal_track_stream(long track_id, tidal_stream_t *out) { return false; }
bool tidal_login_begin(tidal_login_t *out) { return false; }
tidal_login_state_t tidal_login_poll(const tidal_login_t *login) { return TIDAL_LOGIN_ERROR; }
#pragma GCC diagnostic pop

/* ---- reading the page as it stands --------------------------------------- */

#define FOUND_MAX 48
typedef struct {
	lv_obj_t *items[FOUND_MAX];
	int count;
} found_t;

/* Everything of one class under `obj`, itself included, in the order it was
 * built -- which for a row is the order it reads in. */
static void collect(lv_obj_t *obj, const lv_obj_class_t *cls, found_t *found) {
	if (lv_obj_check_type(obj, cls)) {
		assert(found->count < FOUND_MAX);
		found->items[found->count++] = obj;
	}
	for (uint32_t i = 0; i < lv_obj_get_child_count(obj); i++) {
		collect(lv_obj_get_child(obj, (int32_t)i), cls, found);
	}
}

/* A row's name is its first label, and what stands under the name its
 * second. */
static const char *row_text(lv_obj_t *row, int which) {
	found_t labels = {0};
	collect(row, &lv_label_class, &labels);
	return which < labels.count ? lv_label_get_text(labels.items[which]) : "";
}

/* The scrolling body of a page built by settingsrow_page(): the only child of
 * the screen that scrolls downwards and no other way. */
static lv_obj_t *page_body(lv_obj_t *screen) {
	for (uint32_t i = 0; i < lv_obj_get_child_count(screen); i++) {
		lv_obj_t *child = lv_obj_get_child(screen, (int32_t)i);
		if (lv_obj_get_scroll_dir(child) == LV_DIR_VER) {
			return child;
		}
	}
	assert(!"the page has no body");
	return NULL;
}

/* The rows of a page, top to bottom. A bare line of text between them (the
 * note at the head of the reorder page) is not one. */
static void page_rows(lv_obj_t *screen, found_t *rows) {
	lv_obj_t *body = page_body(screen);
	rows->count = 0;
	for (uint32_t i = 0; i < lv_obj_get_child_count(body); i++) {
		lv_obj_t *child = lv_obj_get_child(body, (int32_t)i);
		if (lv_obj_check_type(child, &lv_label_class)) {
			continue;
		}
		assert(rows->count < FOUND_MAX);
		rows->items[rows->count++] = child;
	}
}

static void print_names(const char *what, const char *const *names, int count) {
	fprintf(stderr, "%s:", what);
	for (int i = 0; i < count; i++) {
		fprintf(stderr, " [%s]", names[i]);
	}
	fprintf(stderr, "\n");
}

static void expect_names(const char *what, const char *const *have, int have_count, const char *const *want,
						 int want_count) {
	bool same = have_count == want_count;
	for (int i = 0; same && i < want_count; i++) {
		same = strcmp(have[i], want[i]) == 0;
	}
	if (!same) {
		fprintf(stderr, "%s\n", what);
		print_names("  there", have, have_count);
		print_names("  wanted", want, want_count);
		assert(!"not what was expected there");
	}
}

static void expect_rows(lv_obj_t *screen, const char *what, const char *const *want, int want_count) {
	lv_obj_update_layout(screen);
	found_t rows;
	page_rows(screen, &rows);
	const char *have[FOUND_MAX];
	for (int i = 0; i < rows.count; i++) {
		have[i] = row_text(rows.items[i], 0);
	}
	expect_names(what, have, rows.count, want, want_count);
}

static lv_obj_t *row_named(lv_obj_t *screen, const char *name) {
	found_t rows;
	page_rows(screen, &rows);
	for (int i = 0; i < rows.count; i++) {
		if (strcmp(row_text(rows.items[i], 0), name) == 0) {
			return rows.items[i];
		}
	}
	fprintf(stderr, "no row named \"%s\"\n", name);
	assert(!"the row is not on the page");
	return NULL;
}

static bool shows_icon(lv_obj_t *button, const void *glyph) {
	for (uint32_t i = 0; i < lv_obj_get_child_count(button); i++) {
		lv_obj_t *child = lv_obj_get_child(button, (int32_t)i);
		if (lv_obj_check_type(child, &lv_image_class) && lv_image_get_src(child) == glyph) {
			return true;
		}
	}
	return false;
}

/* A button in a page's corner, told by its glyph: they carry no text. Only
 * the screen's own children are looked at -- a chosen row wears the same tick
 * the corner does. */
static lv_obj_t *corner_button(lv_obj_t *screen, const void *glyph) {
	for (uint32_t i = 0; i < lv_obj_get_child_count(screen); i++) {
		lv_obj_t *child = lv_obj_get_child(screen, (int32_t)i);
		if (lv_obj_check_type(child, &lv_button_class) && shows_icon(child, glyph)) {
			return child;
		}
	}
	assert(!"the corner has no such button");
	return NULL;
}

/* The `nth` button with this glyph inside a row. */
static lv_obj_t *button_in(lv_obj_t *row, const void *glyph, int nth) {
	found_t buttons = {0};
	collect(row, &lv_button_class, &buttons);
	for (int i = 0; i < buttons.count; i++) {
		if (buttons.items[i] != row && shows_icon(buttons.items[i], glyph) && nth-- == 0) {
			return buttons.items[i];
		}
	}
	assert(!"the row has no such button");
	return NULL;
}

static void tap(lv_obj_t *obj) { lv_obj_send_event(obj, LV_EVENT_CLICKED, NULL); }

static void tap_dots(const char *row) {
	tap(button_in(row_named(tidal_list_screen, row), &icon_ellipsis_vertical, 0));
}

/* The popover, when it is up: the one thing showing on the layer above the
 * pages. */
static lv_obj_t *open_menu(void) {
	lv_obj_t *found = NULL;
	lv_obj_t *top = lv_layer_top();
	for (uint32_t i = 0; i < lv_obj_get_child_count(top); i++) {
		lv_obj_t *child = lv_obj_get_child(top, (int32_t)i);
		if (!lv_obj_is_hidden(child)) {
			assert(!found);
			found = child;
		}
	}
	return found;
}

static void expect_menu(const char *what, const char *const *want, int want_count) {
	lv_obj_t *menu = open_menu();
	assert(menu);
	found_t entries = {0};
	collect(menu, &lv_button_class, &entries);
	const char *have[FOUND_MAX];
	for (int i = 0; i < entries.count; i++) {
		have[i] = row_text(entries.items[i], 0);
	}
	expect_names(what, have, entries.count, want, want_count);
}

static void menu_tap(const char *entry) {
	lv_obj_t *menu = open_menu();
	assert(menu);
	found_t entries = {0};
	collect(menu, &lv_button_class, &entries);
	for (int i = 0; i < entries.count; i++) {
		if (strcmp(row_text(entries.items[i], 0), entry) == 0) {
			tap(entries.items[i]);
			assert(!open_menu());
			return;
		}
	}
	assert(!"the menu has no such entry");
}

/* A tap beside the menu, which is how one is left without choosing. */
static void menu_dismiss(void) {
	lv_obj_t *menu = open_menu();
	assert(menu);
	tap(menu);
	assert(!open_menu());
}

static const char *list_heading(void) { return lv_label_get_text(settingsrow_page_title(tidal_list_screen)); }

static bool list_says_no_results(void) {
	for (uint32_t i = 0; i < lv_obj_get_child_count(tidal_list_screen); i++) {
		lv_obj_t *child = lv_obj_get_child(tidal_list_screen, (int32_t)i);
		if (lv_obj_check_type(child, &lv_label_class) && strcmp(lv_label_get_text(child), tr("no_results")) == 0) {
			return !lv_obj_is_hidden(child);
		}
	}
	assert(!"the list page has lost its empty notice");
	return false;
}

/* Nothing drawn may leave the panel sideways; downwards only inside a
 * container that scrolls that way. */
static void check_within_panel_in(lv_obj_t *obj, gui_config_t *cfg, const char *what, bool scrolls) {
	if (lv_obj_is_hidden(obj)) {
		return;
	}
	lv_area_t area;
	lv_obj_get_coords(obj, &area);
	bool overflow_x = area.x1 < 0 || area.x2 >= (int32_t)cfg->screen_width;
	bool overflow_y = area.y1 < 0 || area.y2 >= (int32_t)cfg->screen_height;
	if (overflow_x || (overflow_y && !scrolls)) {
		fprintf(stderr, "%s: an object at %d,%d..%d,%d is outside the %dx%d panel\n", what, area.x1, area.y1, area.x2,
				area.y2, (int)cfg->screen_width, (int)cfg->screen_height);
		assert(!"an object left the panel");
	}
	if (lv_obj_get_scroll_dir(obj) & LV_DIR_VER) {
		scrolls = true;
	}
	for (uint32_t i = 0; i < lv_obj_get_child_count(obj); i++) {
		check_within_panel_in(lv_obj_get_child(obj, i), cfg, what, scrolls);
	}
}

static void dump_snapshot(lv_obj_t *root, const char *tag) {
	const char *prefix = getenv("SONIX_PAGE_SNAPSHOTS");
	if (!prefix || !prefix[0]) {
		return;
	}
	lv_obj_update_layout(root);
	lv_draw_buf_t *shot = lv_snapshot_take(root, LV_COLOR_FORMAT_RGB888);
	assert(shot);
	char path[512];
	snprintf(path, sizeof(path), "%s-%s.ppm", prefix, tag);
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

/* ---- the strings, in every language -------------------------------------- */

static bool is_ini(const char *name) {
	size_t n = strlen(name);
	return n > 4 && strcmp(name + n - 4, ".ini") == 0;
}

/* The text holds exactly one conversion and it is the one named: these are
 * handed to snprintf() as the format, with one argument behind them. */
static bool one_conversion(const char *text, char kind) {
	int found = 0;
	for (const char *p = text; *p; p++) {
		if (*p != '%') {
			continue;
		}
		p++;
		if (*p == '%') {
			continue;
		}
		if (*p != kind) {
			return false;
		}
		found++;
	}
	return found == 1;
}

/* Every language file has to carry the words the new list is made of: tr()
 * falls back to the key it is given, so a missing one shows the tag itself
 * on screen. */
static void check_strings(const char *language_dir) {
	static const char *const KEYS[] = {
		"favourite_playlists",	  "will_no_longer_be_one_of_your_playlists",
		"remove_from_favourites", "remove_from_favourites_2",
		"remove",				  "playlist_track_2",
		"playlist_tracks",
	};
	DIR *dir = opendir(language_dir);
	assert(dir);
	int languages = 0;
	struct dirent *entry;
	while ((entry = readdir(dir)) != NULL) {
		if (!is_ini(entry->d_name)) {
			continue;
		}
		char name[256];
		snprintf(name, sizeof(name), "%.*s", (int)(strlen(entry->d_name) - 4), entry->d_name);
		assert(lang_set(name));
		for (size_t i = 0; i < sizeof(KEYS) / sizeof(KEYS[0]); i++) {
			const char *text = tr(KEYS[i]);
			if (!text || !text[0] || strcmp(text, KEYS[i]) == 0) {
				fprintf(stderr, "%s: \"%s\" is not translated\n", name, KEYS[i]);
				assert(!"a string is missing from a language file");
			}
			assert(strlen(text) < 160);
		}
		if (!one_conversion(tr("will_no_longer_be_one_of_your_playlists"), 's') ||
			!one_conversion(tr("playlist_track_2"), 'd') || !one_conversion(tr("playlist_tracks"), 'd')) {
			fprintf(stderr, "%s: a format string does not take the one argument it is given\n", name);
			assert(!"a format string is malformed");
		}
		/* The length of a playlist is written into 48 bytes (fill_list). */
		assert(strlen(tr("playlist_track_2")) < 36 && strlen(tr("playlist_tracks")) < 36);
		languages++;
	}
	closedir(dir);
	// The eleven this firmware ships. A twelfth added to the folder is meant to
	// make this fail, so its texts get checked the same way.
	assert(languages == 11);
	assert(lang_set("English"));
}

/* ---- the main page -------------------------------------------------------- */

#define COUNT(a) ((int)(sizeof(a) / sizeof((a)[0])))

/* A row of the main page that leads to a list: tapped, and the list waited
 * for. */
static void open_list(const char *row_key) {
	lv_obj_update_layout(tidal_screen);
	tap(row_named(tidal_screen, tr(row_key)));
	run_posted(1, row_key);
	assert(lv_screen_active() == tidal_list_screen);
}

/* The page nobody has arranged: the new row under the account's own
 * playlists, and "My playlists" where it was -- the fourth row, the last one
 * the panel shows before the page is scrolled. */
static void check_untouched_home(gui_config_t *cfg) {
	const char *const want[] = {
		tr("favourite_tracks"),	   tr("favourite_albums"), tr("favourite_artists"),	  tr("my_playlists"),
		tr("favourite_playlists"), tr("new_releases"),	   tr("tidal_most_streamed"), tr("tidal_recommended_for_you"),
	};
	expect_rows(tidal_screen, "the main page, never arranged", want, COUNT(want));
	check_within_panel_in(tidal_screen, cfg, "Tidal", false);

	/* By its middle, where the name is written: the card's last line of
	 * pixels sits on the panel's edge and is not what is being asked about. */
	lv_area_t own;
	lv_obj_get_coords(row_named(tidal_screen, tr("my_playlists")), &own);
	assert((own.y1 + own.y2) / 2 < (int32_t)cfg->screen_height);
	dump_snapshot(tidal_screen, "v1-tidal-home");
}

/* ---- the list, and what can be done in it --------------------------------- */

static void check_favourite_playlists(gui_config_t *cfg) {
	const char *const all_four[] = {"Jazz Now", "Deep Cuts", "Road Trip", "Night Drive"};
	char want[512];

	/* The row asks for the favourites and for nothing else: not the account's
	 * own playlists, which is the only list of playlists there used to be. */
	open_list("favourite_playlists");
	assert(counted(&fav_requests) == 1 && counted(&own_requests) == 0);
	assert(strcmp(list_heading(), tr("favourite_playlists")) == 0);
	expect_rows(tidal_list_screen, "the favourite playlists", all_four, COUNT(all_four));
	check_within_panel_in(tidal_list_screen, cfg, "Favourite playlists", false);
	dump_snapshot(tidal_list_screen, "v1-tidal-favourite-playlists");

	/* Under each name, who made it and how long it is, in words; and where
	 * nobody is named, the length alone with no separator left dangling. */
	assert(strcmp(row_text(row_named(tidal_list_screen, "Jazz Now"), 1), "TIDAL \xC2\xB7 50 tracks") == 0);
	assert(strcmp(row_text(row_named(tidal_list_screen, "Deep Cuts"), 1), "Miles D. \xC2\xB7 1 track") == 0);
	assert(strcmp(row_text(row_named(tidal_list_screen, "Road Trip"), 1), "12 tracks") == 0);

	/* A second visit is read back from the card, under the same heading. */
	assert(listcache_age(LISTCACHE_TIDAL_FAV_PLAYLISTS) >= 0);
	assert(!go_back());
	assert(lv_screen_active() == tidal_screen);
	open_list("favourite_playlists");
	assert(counted(&fav_requests) == 1);
	assert(strcmp(list_heading(), tr("favourite_playlists")) == 0);
	expect_rows(tidal_list_screen, "the favourite playlists, from the card", all_four, COUNT(all_four));

	/* A favourited playlist opens like one of the account's own -- the same
	 * request, by its UUID -- but is somebody else's: a track in it can be
	 * favourited and kept on the card, and cannot be taken out. */
	const char *const tracks[] = {"So What", "Blue in Green"};
	const char *const on_a_track[] = {tr("medialist_add_to_favourites"), tr("trackmenu_keep_on_card")};
	tap(row_named(tidal_list_screen, "Deep Cuts"));
	run_posted(1, "a favourited playlist");
	assert(counted(&track_requests) == 1);
	expect_tracks_of(FAV_DEEP);
	assert(strcmp(list_heading(), "Deep Cuts") == 0);
	expect_rows(tidal_list_screen, "a favourited playlist's tracks", tracks, COUNT(tracks));
	assert(strcmp(row_text(row_named(tidal_list_screen, "So What"), 1), "Miles Davis \xC2\xB7 9:22") == 0);
	tap_dots("So What");
	expect_menu("a track of a favourited playlist", on_a_track, COUNT(on_a_track));
	menu_dismiss();

	/* The same two things for several of them at once. */
	lv_obj_t *tick = corner_button(tidal_list_screen, &icon_check);
	assert(lv_obj_is_hidden(tick));
	lv_obj_send_event(row_named(tidal_list_screen, "So What"), LV_EVENT_LONG_PRESSED, NULL);
	assert(!lv_obj_is_hidden(tick));
	tap(tick);
	expect_menu("chosen tracks of a favourited playlist", on_a_track, COUNT(on_a_track));
	menu_dismiss();
	assert(go_back()); /* out of the selection */
	assert(lv_obj_is_hidden(tick));

	/* Its rows are on the card now, under the name any playlist's are. */
	char rows_key[64];
	listcache_tidal_playlist_key(FAV_DEEP, rows_key, sizeof(rows_key));
	assert(listcache_age(rows_key) >= 0);

	/* And back leads to the favourites again, not out of the page. */
	assert(go_back());
	run_posted(1, "back to the favourites");
	assert(lv_screen_active() == tidal_list_screen);
	assert(strcmp(list_heading(), tr("favourite_playlists")) == 0);
	expect_rows(tidal_list_screen, "the favourite playlists, come back to", all_four, COUNT(all_four));

	/* The three dots on a favourite offer to take it out of the favourites.
	 * Not to delete it: it is not the account's to delete. */
	const char *const on_a_favourite[] = {tr("remove_from_favourites")};
	tap_dots("Deep Cuts");
	expect_menu("a favourite playlist", on_a_favourite, COUNT(on_a_favourite));
	dump_snapshot(lv_layer_top(), "v1-tidal-favourite-playlists-menu");

	/* It asks first, by name, and "no" sends nothing. */
	menu_tap(tr("remove_from_favourites"));
	snprintf(want, sizeof(want), tr("will_no_longer_be_one_of_your_playlists"), "Deep Cuts");
	assert(strstr(want, "Deep Cuts"));
	expect_asked("remove_from_favourites_2", want, "remove");
	answer(false);
	assert(counted(&removals) == 0 && posted_waiting() == 0);

	/* "Yes" takes that playlist, and only it, out of the favourites; the
	 * list is then asked of Tidal again instead of being read off the card,
	 * and the playlist's own rows are gone from the card with it. */
	tap_dots("Deep Cuts");
	menu_tap(tr("remove_from_favourites"));
	answer(true);
	run_posted(2, "a favourite removed");
	assert(counted(&removals) == 1);
	expect_removed(0, FAV_DEEP);
	assert(counted(&fav_requests) == 2);
	const char *const three_left[] = {"Jazz Now", "Road Trip", "Night Drive"};
	expect_rows(tidal_list_screen, "the favourites, one removed", three_left, COUNT(three_left));
	assert(lv_screen_active() == tidal_list_screen);
	assert(listcache_age(rows_key) < 0);
	assert(popups == 0 && error_toasts == 0);

	/* A refusal -- which is what an account signed in with a read-only key
	 * can expect -- is shown in Tidal's own words, and the list stays as it
	 * is. From a single row it comes as a popup... */
	account_refuses("This client may only read.");
	tap_dots("Road Trip");
	menu_tap(tr("remove_from_favourites"));
	answer(true);
	run_posted(1, "a refused removal");
	assert(counted(&removals) == 2);
	expect_removed(1, FAV_ROAD);
	assert(popups == 1 && strcmp(last_popup, "This client may only read.") == 0);
	expect_rows(tidal_list_screen, "the favourites, after a refusal", three_left, COUNT(three_left));

	/* ...and from a selection as the toast every refused selection ends in,
	 * once the rows chosen have all been tried. */
	lv_obj_send_event(row_named(tidal_list_screen, "Night Drive"), LV_EVENT_LONG_PRESSED, NULL);
	tap(tick);
	menu_tap(tr("remove_from_favourites"));
	answer(true);
	run_posted(2, "a refused selection");
	assert(counted(&removals) == 3);
	expect_removed(2, FAV_NIGHT);
	assert(error_toasts == 1 && strcmp(last_error_toast, "This client may only read.") == 0);
	expect_rows(tidal_list_screen, "the favourites, after a refused selection", three_left, COUNT(three_left));
	assert(lv_obj_is_hidden(tick));
	account_refuses(NULL);

	/* Several at once: a long press chooses the first, a tap the next, and
	 * the corner's tick offers the one thing this list can do to them. That
	 * asks too -- nothing here can put a playlist back among the favourites
	 * -- and "no" leaves the rows chosen and the account alone. */
	const char *const on_favourites[] = {tr("remove_from_favourites")};
	lv_obj_send_event(row_named(tidal_list_screen, "Road Trip"), LV_EVENT_LONG_PRESSED, NULL);
	tap(row_named(tidal_list_screen, "Night Drive"));
	assert(!lv_obj_is_hidden(tick));
	tap(tick);
	expect_menu("chosen favourite playlists", on_favourites, COUNT(on_favourites));
	menu_tap(tr("remove_from_favourites"));
	expect_asked("remove_from_favourites_2", "", "remove");
	answer(false);
	assert(counted(&removals) == 3 && posted_waiting() == 0);
	assert(!lv_obj_is_hidden(tick));

	/* "Yes" sends them one after the other, in the order they stand on the
	 * page, and reads the list again when the last has answered. */
	tap(tick);
	menu_tap(tr("remove_from_favourites"));
	answer(true);
	run_posted(3, "two favourites removed");
	assert(counted(&removals) == 5);
	expect_removed(3, FAV_ROAD);
	expect_removed(4, FAV_NIGHT);
	const char *const one_left[] = {"Jazz Now"};
	expect_rows(tidal_list_screen, "the favourites, three removed", one_left, COUNT(one_left));
	assert(lv_obj_is_hidden(tick));
	assert(!list_says_no_results());

	/* The last one out leaves a list that says it is empty. */
	tap_dots("Jazz Now");
	menu_tap(tr("remove_from_favourites"));
	answer(true);
	run_posted(2, "the last favourite removed");
	expect_removed(5, FAV_JAZZ);
	expect_rows(tidal_list_screen, "the favourites, all removed", NULL, 0);
	assert(list_says_no_results());
	assert(strcmp(list_heading(), tr("favourite_playlists")) == 0);

	assert(!go_back());
	assert(lv_screen_active() == tidal_screen);
}

/* The account's own playlists, for what they must keep and for what the two
 * lists must not share: this one deletes, and its tracks can be taken out. */
static void check_own_playlists(void) {
	const char *const own[] = {"Morning"};
	const char *const on_an_own[] = {tr("delete_playlist")};
	const char *const tracks[] = {"So What", "Blue in Green"};
	const char *const on_a_track[] = {tr("medialist_add_to_favourites"), tr("medialist_remove_from_playlist"),
									  tr("trackmenu_keep_on_card")};

	int favourites_asked = counted(&fav_requests);
	open_list("my_playlists");
	assert(counted(&own_requests) == 1 && counted(&fav_requests) == favourites_asked);
	assert(strcmp(list_heading(), tr("my_playlists")) == 0);
	expect_rows(tidal_list_screen, "the account's own playlists", own, COUNT(own));
	assert(strcmp(row_text(row_named(tidal_list_screen, "Morning"), 1), "7 tracks") == 0);
	tap_dots("Morning");
	expect_menu("one of the account's own playlists", on_an_own, COUNT(on_an_own));
	menu_dismiss();

	tap(row_named(tidal_list_screen, "Morning"));
	run_posted(1, "one of the account's own playlists");
	expect_tracks_of(OWN_MORNING);
	expect_rows(tidal_list_screen, "an own playlist's tracks", tracks, COUNT(tracks));
	tap_dots("So What");
	expect_menu("a track of an own playlist", on_a_track, COUNT(on_a_track));
	menu_dismiss();

	assert(go_back());
	run_posted(1, "back to the account's own playlists");
	assert(!go_back());
	assert(lv_screen_active() == tidal_screen);
}

/* Signing out takes the account's lists off the card with it. They are read
 * back for half a day before Tidal is asked again, which is long enough for
 * whoever signs in next to be shown them. */
static void check_sign_out(void) {
	static const char *const ACCOUNT_LISTS[] = {
		LISTCACHE_TIDAL_FAV_TRACKS,	   LISTCACHE_TIDAL_FAV_ALBUMS, LISTCACHE_TIDAL_FAV_ARTISTS,
		LISTCACHE_TIDAL_FAV_PLAYLISTS, LISTCACHE_TIDAL_PLAYLISTS,
	};
	const char *const all_four[] = {"Jazz Now", "Deep Cuts", "Road Trip", "Night Drive"};
	account_fill();
	open_list("favourite_playlists");
	expect_rows(tidal_list_screen, "the favourites, before signing out", all_four, COUNT(all_four));
	/* The two lists of playlists are on the card from the visits above. The
	 * account here has no favourite tracks, albums or artists to visit, so
	 * those three files are stood in for by a record apiece. */
	assert(listcache_save(LISTCACHE_TIDAL_FAV_TRACKS, "t", 1, 1));
	assert(listcache_save(LISTCACHE_TIDAL_FAV_ALBUMS, "a", 1, 1));
	assert(listcache_save(LISTCACHE_TIDAL_FAV_ARTISTS, "r", 1, 1));
	for (int i = 0; i < COUNT(ACCOUNT_LISTS); i++) {
		assert(listcache_age(ACCOUNT_LISTS[i]) >= 0);
	}
	assert(!go_back());

	tap(corner_button(tidal_screen, &icon_music_settings));
	lv_obj_t *settings = lv_screen_active();
	assert(settings != tidal_screen);
	lv_obj_update_layout(settings);
	tap(row_named(settings, tr("sign_out_of_tidal")));
	assert(asked.open && strcmp(asked.title, "tidal_sign_out_of_tidal") == 0 && strcmp(asked.ok, "sign_out") == 0);
	assert(counted(&sign_outs) == 0);
	answer(true);
	assert(counted(&sign_outs) == 1 && cache_clears == 1);
	for (int i = 0; i < COUNT(ACCOUNT_LISTS); i++) {
		if (listcache_age(ACCOUNT_LISTS[i]) >= 0) {
			fprintf(stderr, "\"%s\" is still on the card after signing out\n", ACCOUNT_LISTS[i]);
			assert(!"an account's list outlived its sign-out");
		}
	}

	const char *const signed_out[] = {tr("sign_in")};
	assert(!go_back());
	assert(lv_screen_active() == tidal_screen);
	expect_rows(tidal_screen, "the main page, signed out", signed_out, COUNT(signed_out));

	/* Somebody else signs in, and is shown their own favourites. */
	pthread_mutex_lock(&account_lock);
	signed_in = true;
	put_playlist(&favourites[0], FAV_OTHER, "Other One", "TIDAL", 3);
	favourite_count = 1;
	pthread_mutex_unlock(&account_lock);
	assert(!go_back());
	switch_screen(tidal_screen);
	int asked_before = counted(&fav_requests);
	open_list("favourite_playlists");
	assert(counted(&fav_requests) == asked_before + 1);
	const char *const theirs[] = {"Other One"};
	expect_rows(tidal_list_screen, "the next account's favourites", theirs, COUNT(theirs));
	assert(!go_back());
}

/* ---- an arrangement saved before the row existed -------------------------- */

/* What that build wrote: seven rows by number, the account's playlists moved
 * to the top, the showcases turned round and "most streamed" switched off.
 * The row it never heard of goes straight under "My playlists", where it
 * would have been on a page nobody had touched, and every number still means
 * the row it meant. */
#define OLD_ORDER "3,0,1,2,6,5,4"
#define OLD_HIDDEN (1L << 5)

static void check_saved_home(gui_config_t *cfg) {
	const char *const want[] = {
		tr("my_playlists"),		 tr("favourite_playlists"),		   tr("favourite_tracks"), tr("favourite_albums"),
		tr("favourite_artists"), tr("tidal_recommended_for_you"), tr("new_releases"),
	};
	expect_rows(tidal_screen, "the main page, arranged by an older build", want, COUNT(want));
	check_within_panel_in(tidal_screen, cfg, "Tidal", false);

	/* The page where the rows are arranged lists all eight, the hidden one
	 * with its switch off. */
	const char *const all[] = {
		tr("my_playlists"),		 tr("favourite_playlists"),		   tr("favourite_tracks"),	  tr("favourite_albums"),
		tr("favourite_artists"), tr("tidal_recommended_for_you"), tr("tidal_most_streamed"), tr("new_releases"),
	};
	tap(corner_button(tidal_screen, &icon_music_settings));
	lv_obj_t *settings = lv_screen_active();
	assert(settings != tidal_screen);
	lv_obj_update_layout(settings);
	tap(row_named(settings, tr("tidal_home")));
	lv_obj_t *arrange = lv_screen_active();
	assert(arrange != settings);
	expect_rows(arrange, "the rows to arrange", all, COUNT(all));
	check_within_panel_in(arrange, cfg, "Home reorder", false);
	dump_snapshot(arrange, "v1-tidal-home-reorder");

	found_t toggle = {0};
	collect(row_named(arrange, tr("tidal_most_streamed")), &lv_switch_class, &toggle);
	assert(toggle.count == 1 && !lv_obj_has_state(toggle.items[0], LV_STATE_CHECKED));
	toggle.count = 0;
	collect(row_named(arrange, tr("favourite_playlists")), &lv_switch_class, &toggle);
	assert(toggle.count == 1 && lv_obj_has_state(toggle.items[0], LV_STATE_CHECKED));

	/* Moved up one, it is written down as the eighth row, by its number. */
	tap(button_in(row_named(arrange, tr("favourite_playlists")), &icon_chevron_up, 0));
	run_posted(1, "a row moved up");
	assert(strcmp(config_get("tidal", "home_order", ""), "7,3,0,1,2,6,5,4") == 0);
	const char *const moved[] = {
		tr("favourite_playlists"), tr("my_playlists"),				   tr("favourite_tracks"),	  tr("favourite_albums"),
		tr("favourite_artists"),   tr("tidal_recommended_for_you"), tr("tidal_most_streamed"), tr("new_releases"),
	};
	expect_rows(arrange, "the rows to arrange, one moved", moved, COUNT(moved));

	/* Switched off, it leaves the main page and nothing else does. */
	toggle.count = 0;
	collect(row_named(arrange, tr("favourite_playlists")), &lv_switch_class, &toggle);
	assert(toggle.count == 1);
	lv_obj_remove_state(toggle.items[0], LV_STATE_CHECKED);
	lv_obj_send_event(toggle.items[0], LV_EVENT_VALUE_CHANGED, NULL);
	assert(config_get_int("tidal", "home_hidden", 0) == (OLD_HIDDEN | (1L << 7)));
	assert(strcmp(config_get("tidal", "home_order", ""), "7,3,0,1,2,6,5,4") == 0);

	const char *const without[] = {
		tr("my_playlists"),		 tr("favourite_tracks"),		   tr("favourite_albums"),
		tr("favourite_artists"), tr("tidal_recommended_for_you"), tr("new_releases"),
	};
	assert(!go_back());
	assert(!go_back());
	assert(lv_screen_active() == tidal_screen);
	expect_rows(tidal_screen, "the main page, the new row switched off", without, COUNT(without));
}

/* ---- an arrangement this build saved -------------------------------------- */

/* Eight numbers, the eighth among them, as the page above has just been seen
 * to write them. Read back, the row is where it was put. */
#define NEW_ORDER "7,3,0,1,2,4,5,6"

static void check_arranged_home(void) {
	const char *const want[] = {
		tr("favourite_playlists"), tr("my_playlists"), tr("favourite_tracks"),	  tr("favourite_albums"),
		tr("favourite_artists"),   tr("new_releases"), tr("tidal_most_streamed"), tr("tidal_recommended_for_you"),
	};
	expect_rows(tidal_screen, "the main page, arranged by this build", want, COUNT(want));
}

/* ---- the three runs ------------------------------------------------------- */

static int remove_entry(const char *path, const struct stat *st, int type, struct FTW *ftw) {
	(void)st;
	(void)type;
	(void)ftw;
	return remove(path);
}

int main(int argc, char **argv) {
	assert(argc == 2 || argc == 3);
	const char *run = argc == 3 ? argv[2] : "untouched";
	bool saved = strcmp(run, "saved") == 0;
	bool arranged = strcmp(run, "arranged") == 0;
	assert(argc == 2 || saved || arranged);

	char card[] = "/tmp/sonix-tidal-ui-test-XXXXXX";
	assert(mkdtemp(card));
	card_root = card;
	char config[512];
	snprintf(config, sizeof(config), "%s/device_config.ini", card);
	config_init(config);
	if (saved || arranged) {
		config_set("tidal", "home_order", saved ? OLD_ORDER : NEW_ORDER);
		config_set_int("tidal", "home_hidden", saved ? OLD_HIDDEN : 0);
		assert(config_save());
		assert(config_init(config));
	}
	config_set("ui", "language", "English");
	lang_init(argv[1]);
	check_strings(argv[1]);

	init_fonts();
	lv_init();
	gui_config_t cfg = {bp_screen_w(), bp_screen_h(), bp_padding(), bp_status_bar_h(), card};
	lv_display_create(cfg.screen_width, cfg.screen_height);
	theme_init();
	popover_init();
	player_screen = lv_obj_create(NULL);
	wifisettings_screen = lv_obj_create(NULL);

	account_fill();
	tidalpage_init(&cfg);
	/* From the Streaming grid, which here is the screen the display came up
	 * with. One round of the timers, as the player's main loop would give. */
	switch_screen(tidal_screen);
	lv_timer_handler();
	lv_obj_update_layout(tidal_screen);

	if (saved) {
		check_saved_home(&cfg);
	} else if (arranged) {
		check_arranged_home();
	} else {
		check_untouched_home(&cfg);
		check_favourite_playlists(&cfg);
		check_own_playlists();
		/* Six un-favouritings were sent, two of them refused and said so,
		 * and nothing else about the account was touched: no playlist
		 * deleted, no track taken out of one. */
		assert(counted(&removals) == 6 && counted(&other_writes) == 0);
		assert(popups == 1 && error_toasts == 1);
		check_sign_out();
		assert(counted(&other_writes) == 0 && popups == 1 && error_toasts == 1);
	}
	assert(posted_waiting() == 0 && !asked.open && !open_menu());

	/* A run that passed leaves no card behind; one that failed has stopped
	 * above, with the card still there to be looked at. */
	assert(nftw(card, remove_entry, 8, FTW_DEPTH | FTW_PHYS) == 0);

	printf("PASS tidal pages (%s)\n", run);
	return 0;
}
