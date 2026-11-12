#include "subsonicpage.h"

#include "src/gui/fonts/fonts.h"
#include "src/gui/streaming/subsonicdlpage.h"
#include "src/gui/nowplaying/player.h"
#include "src/gui/shell/confirm.h"
#include "src/gui/shell/gridpage.h"
#include "src/gui/shell/icons.h"
#include "src/gui/shell/keyboard.h"
#include "src/gui/shell/popover.h"
#include "src/gui/shell/settingsrow.h"
#include "src/gui/shell/switcher.h"
#include "src/gui/shell/theme.h"
#include "src/gui/shell/toast.h"
#include "src/system/audio/audio.h"
#include "src/system/core/lang.h"
#include "src/system/core/utils.h"
#include "src/system/net/tls.h"
#include "src/system/playback/device_state.h"
#include "src/system/playback/playlist.h"
#include "src/system/streaming/streamturn.h"
#include "src/system/streaming/subsonic.h"
#include "src/system/streaming/subsonicdl.h"
#include "src/system/streaming/subsonicstream.h"

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

lv_obj_t *subsonic_screen;
static lv_obj_t *settings_screen, *edit_screen, *list_screen, *home_grid;
static lv_obj_t *list_container, *list_title, *server_value, *user_value, *remember_switch, *relaxed_switch;
static lv_obj_t *edit_field, *edit_title, *format_pills[3], *bitrate_pills[2];
static keyboard_t *keyboard;
static gui_config_t *config;
static subsonic_credentials_t draft;
static bool draft_remember, busy;
static int editing;

// Server settings, Test connection, Folders and Artists: a 2x2 grid that fills
// the panel under the heading, the same cards the SoundCloud home page uses.
#define SUBSONIC_HOME_TILES 4
// The size each card's picture is drawn at, matching what tools/svg_to_lvgl.py
// emits for it: HOME_TILE_SIZE and HOME_TILE_GLYPH_SIZE there.
#define SUBSONIC_TILE_ART_PX 54
#define SUBSONIC_TILE_GLYPH_PX 46

// A single mailbox in each direction. Results own their memory until the GUI
// takes it: a late worker can never overwrite the list the GUI is reading.
// LVGL interaction (including async calls/ticks) belongs only to the GUI.
typedef enum { JOB_BROWSE, JOB_PING, JOB_CONNECT } job_kind_t;
typedef struct {
	unsigned serial;
	job_kind_t kind;
	subsonic_browse_t browse;
	char id[SUBSONIC_ID_MAX];
	char title[200];
	int offset;
	subsonic_credentials_t credentials;
	bool remember;
} job_t;

typedef struct {
	job_t job;
	subsonic_entry_t *entries;
	int count;
	bool more;
	char error[192];
} result_t;

static pthread_mutex_t job_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t job_ready = PTHREAD_COND_INITIALIZER;
static bool worker_started, have_job, configuring;
static lv_obj_t *job_owner;
static unsigned serial, failed_serial;
static job_t pending;
static result_t *mail;
static subsonic_entry_t *shown;
static int shown_count;
static bool shown_more;
static job_t current_list;
#define HISTORY_DEPTH 12
static job_t history[HISTORY_DEPTH];
static int history_count;

static void free_result(result_t *result) {
	if (result) {
		free(result->entries);
		memset(&result->job.credentials, 0, sizeof(result->job.credentials));
		free(result);
	}
}

static void *worker_main(void *arg) {
	(void)arg;
	thread_be_low_priority("subsonic API");
	for (;;) {
		pthread_mutex_lock(&job_lock);
		while (!have_job) {
			pthread_cond_wait(&job_ready, &job_lock);
		}
		job_t job = pending;
		memset(&pending, 0, sizeof(pending));
		have_job = false;
		if (job.serial != serial) {
			memset(&job.credentials, 0, sizeof(job.credentials));
			pthread_mutex_unlock(&job_lock);
			continue;
		}
		configuring = job.kind == JOB_CONNECT;
		pthread_mutex_unlock(&job_lock);
		result_t *result = calloc(1, sizeof(*result));
		if (!result) {
			// An allocation failure must still clear the GUI's wait. The tiny
			// fallback result gets a chance once the larger old mail is freed.
			pthread_mutex_lock(&job_lock);
			free_result(mail);
			mail = NULL;
			pthread_mutex_unlock(&job_lock);
			result = calloc(1, sizeof(*result));
		}
		if (result) {
			result->job = job;
			memset(&result->job.credentials, 0, sizeof(result->job.credentials));
			if (job.kind == JOB_BROWSE) {
				result->entries = calloc(SUBSONIC_PAGE_SIZE, sizeof(*result->entries));
				if (result->entries) {
					result->count = subsonic_browse(job.browse, job.id, job.offset, result->entries,
												  SUBSONIC_PAGE_SIZE, &result->more, result->error, sizeof(result->error));
				} else {
					result->count = -1;
					snprintf(result->error, sizeof(result->error), "subsonic_out_of_memory");
				}
			} else {
				bool ok = job.kind != JOB_CONNECT || subsonic_configure(&job.credentials, job.remember, result->error, sizeof(result->error));
				pthread_mutex_lock(&job_lock);
				configuring = false; // a Forget may proceed while the ping finishes
				pthread_mutex_unlock(&job_lock);
				if (ok) {
					ok = subsonic_ping(result->error, sizeof(result->error));
				}
				result->count = ok ? 0 : -1;
			}
		}
		memset(&job.credentials, 0, sizeof(job.credentials));
		pthread_mutex_lock(&job_lock);
		configuring = false;
		free_result(mail);
		mail = result;
		if (!result) {
			failed_serial = job.serial;
		}
		pthread_mutex_unlock(&job_lock);
	}
	return NULL;
}

static bool submit(job_t *job) {
	if (!worker_started) {
		pthread_t worker;
		pthread_attr_t attr;
		int err = pthread_attr_init(&attr);
		if (!err) {
			err = pthread_attr_setstacksize(&attr, 256 * 1024);
			if (!err) err = pthread_create(&worker, &attr, worker_main, NULL);
			pthread_attr_destroy(&attr);
		}
		if (err) {
			toast_error("subsonic_out_of_memory");
			return false;
		}
		pthread_detach(worker);
		worker_started = true;
	}
	pthread_mutex_lock(&job_lock);
	job->serial = ++serial;
	pending = *job;
	have_job = true;
	pthread_cond_signal(&job_ready);
	pthread_mutex_unlock(&job_lock);
	busy = true;
	job_owner = job->kind == JOB_BROWSE ? list_screen : job->kind == JOB_CONNECT ? settings_screen : subsonic_screen;
	toast_busy("loading");
	return true;
}

static void invalidate_job(void) {
	pthread_mutex_lock(&job_lock);
	serial++;
	have_job = false;
	memset(&pending, 0, sizeof(pending));
	free_result(mail);
	mail = NULL;
	pthread_mutex_unlock(&job_lock);
	busy = false;
	job_owner = NULL;
	toast_busy_end();
}

static void page_left_cb(lv_event_t *e) {
	// Back/player navigation must not leave a global busy overlay behind or
	// let a late result rebuild a hidden list/show a toast on another page.
	if (busy && lv_event_get_target(e) == job_owner) {
		invalidate_job();
	}
}

static void quality_refresh(void) {
	subsonic_quality_t q = subsonic_quality();
	for (int i = 0; i < 3; i++) {
		settingsrow_pill_active(format_pills[i], i == (int)q.format);
	}
	for (int i = 0; i < 2; i++) {
		settingsrow_pill_active(bitrate_pills[i], q.max_bitrate == (i == 0 ? 256 : 320));
		if (q.format == SUBSONIC_FORMAT_RAW) {
			lv_obj_add_state(bitrate_pills[i], LV_STATE_DISABLED);
		} else {
			lv_obj_remove_state(bitrate_pills[i], LV_STATE_DISABLED);
		}
	}
}

static void settings_refresh(void) {
	lv_label_set_text(server_value, draft.server[0] ? draft.server : tr("subsonic_not_set"));
	lv_label_set_text(user_value, draft.username[0] ? draft.username : tr("subsonic_not_set"));
	if (draft_remember) {
		lv_obj_add_state(remember_switch, LV_STATE_CHECKED);
	} else {
		lv_obj_remove_state(remember_switch, LV_STATE_CHECKED);
	}
	// Read back rather than remembered here: the setting is [network]
	// tls_verify, which the developer options or an edited config file can also
	// have changed, and the switch has to agree with what is in force.
	if (tls_verify_mode() == TLS_VERIFY_RELAXED) {
		lv_obj_add_state(relaxed_switch, LV_STATE_CHECKED);
	} else {
		lv_obj_remove_state(relaxed_switch, LV_STATE_CHECKED);
	}
	quality_refresh();
}

static void open_settings_cb(lv_event_t *e) {
	(void)e;
	if (busy || switcher_back_drag_active()) {
		return;
	}
	subsonic_credentials(&draft, &draft_remember);
	settings_refresh();
	switch_screen(settings_screen);
}

static void format_cb(lv_event_t *e) {
	subsonic_quality_t q = subsonic_quality();
	q.format = (subsonic_format_t)(intptr_t)lv_event_get_user_data(e);
	subsonic_set_quality(q);
	quality_refresh();
}

static void bitrate_cb(lv_event_t *e) {
	subsonic_quality_t q = subsonic_quality();
	q.max_bitrate = (int)(intptr_t)lv_event_get_user_data(e);
	subsonic_set_quality(q);
	quality_refresh();
}

static void remember_cb(lv_event_t *e) {
	draft_remember = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
}

// The certificate switch is the one control on this page that is not about this
// server: it is [network] tls_verify, which every https:// the player opens
// reads, radio stations included. Written and applied at once -- the reason
// somebody is looking at it is a connection that just failed with a certificate
// error, and a setting that only worked after a restart would be of no use.
static void certificates_cb(lv_event_t *e) {
	(void)e;
	bool relaxed = lv_obj_has_state(relaxed_switch, LV_STATE_CHECKED);
	tls_set_verify_mode(relaxed ? TLS_VERIFY_RELAXED : TLS_VERIFY_STRICT);
}

static void edit_cb(lv_event_t *e) {
	if (busy || switcher_back_drag_active()) {
		return;
	}
	editing = (int)(intptr_t)lv_event_get_user_data(e);
	const char *keys[] = {"subsonic_server_url", "subsonic_username", "password"};
	const char *values[] = {draft.server, draft.username, draft.password};
	const size_t sizes[] = {sizeof(draft.server), sizeof(draft.username), sizeof(draft.password)};
	lv_label_set_text(edit_title, tr(keys[editing]));
	lv_textarea_set_max_length(edit_field, (uint32_t)sizes[editing] - 1);
	// Keep the password readable so it can be checked before connecting.
	lv_textarea_set_password_mode(edit_field, false);
	lv_textarea_set_text(edit_field, values[editing]);
	lv_textarea_set_placeholder_text(edit_field, editing == 0 ? "https://music.example.com" : tr(keys[editing]));
	keyboard_reset(keyboard);
	keyboard_set_visible(keyboard, true);
	switch_screen(edit_screen);
}

static void accept_edit_cb(lv_event_t *e) {
	(void)e;
	char *destinations[] = {draft.server, draft.username, draft.password};
	const size_t sizes[] = {sizeof(draft.server), sizeof(draft.username), sizeof(draft.password)};
	const char *text = lv_textarea_get_text(edit_field);
	// LVGL counts characters, but UTF-8 credentials are byte strings to the
	// protocol. Refuse an overlong value rather than truncate its identity.
	if (strlen(text) >= sizes[editing]) {
		toast_error("subsonic_invalid_credentials");
		return;
	}
	snprintf(destinations[editing], sizes[editing], "%s", text);
	lv_textarea_set_text(edit_field, "");
	settings_refresh();
	switch_screen_return_to(settings_screen);
}

static void edit_left_cb(lv_event_t *e) {
	(void)e;
	lv_textarea_set_text(edit_field, "");
}

static void reset_library(void) {
	subsonicstream_reset();
	free(shown);
	shown = NULL;
	shown_count = history_count = 0;
	shown_more = false;
	current_list = (job_t){.kind = JOB_BROWSE, .browse = SUBSONIC_BROWSE_ARTISTS};
	snprintf(current_list.title, sizeof(current_list.title), "%s", tr("artists"));
	lv_obj_clean(list_container);
	lv_label_set_text(list_title, current_list.title);
}

static void connect_cb(lv_event_t *e) {
	(void)e;
	if (busy) {
		return;
	}
	char current[512];
	audio_get_current_file(current, sizeof(current));
	if (subsonicstream_owns(current)) {
		audio_stop();
	}
	reset_library();
	job_t job = {.kind = JOB_CONNECT, .credentials = draft, .remember = draft_remember};
	submit(&job);
	memset(&job.credentials, 0, sizeof(job.credentials));
}

static void ping_cb(lv_event_t *e) {
	(void)e;
	if (busy) {
		return;
	}
	if (!subsonic_configured()) {
		open_settings_cb(NULL);
		return;
	}
	job_t job = {.kind = JOB_PING};
	submit(&job);
}

static void forget(void *user) {
	(void)user;
	invalidate_job();
	pthread_mutex_lock(&job_lock);
	bool saving = configuring;
	pthread_mutex_unlock(&job_lock);
	if (saving) {
		toast_error("subsonic_settings_busy");
		return; // an in-progress save must not re-create a just-forgotten account
	}
	char current[512];
	audio_get_current_file(current, sizeof(current));
	if (subsonicstream_owns(current)) {
		audio_stop();
	}
	reset_library();
	bool removed = subsonic_forget();
	memset(&draft, 0, sizeof(draft));
	draft_remember = false;
	settings_refresh();
	if (removed) toast_success("subsonic_forgotten");
	else toast_error("subsonic_cannot_save_credentials");
}

static void forget_cb(lv_event_t *e) {
	(void)e;
	if (!busy) {
		confirm_show("subsonic_forget_server", "subsonic_forget_note", NULL, forget, NULL);
	}
}

static void run_list(job_t job, bool push_history) {
	if (!subsonic_configured()) {
		open_settings_cb(NULL);
		return;
	}
	if (push_history && lv_screen_active() == list_screen && history_count < HISTORY_DEPTH) {
		history[history_count++] = current_list;
	} else if (lv_screen_active() != list_screen) {
		history_count = 0;
	}
	current_list = job;
	free(shown);
	shown = NULL;
	shown_count = 0;
	shown_more = false;
	lv_obj_clean(list_container);
	lv_label_set_text(list_title, job.title);
	if (lv_screen_active() != list_screen) {
		switch_screen(list_screen);
	}
	submit(&job);
}

static bool list_back_has_step(void) { return history_count > 0; }

static bool list_back(void) {
	if (history_count > 0) {
		run_list(history[--history_count], false);
		return true;
	}
	invalidate_job();
	return false;
}

static void browse(subsonic_browse_t which) {
	if (busy || switcher_back_drag_active()) {
		return;
	}
	job_t job = {.kind = JOB_BROWSE, .browse = which};
	snprintf(job.title, sizeof(job.title), "%s", tr(which == SUBSONIC_BROWSE_FOLDERS ? "subsonic_folders" : "artists"));
	run_list(job, false);
}

static void page_cb(lv_event_t *e) {
	if (busy) {
		return;
	}
	job_t job = current_list;
	job.offset += (int)(intptr_t)lv_event_get_user_data(e) * SUBSONIC_PAGE_SIZE;
	if (job.offset >= 0) {
		run_list(job, false);
	}
}

// A long press on a track, or on any row of a page that holds tracks, offers
// the two things a tap cannot: save this one, or save the rest of the page.
// Nothing else on this list is long-pressable, so a press on a folder or an
// artist does nothing at all.
static int menu_index;

static void menu_open_downloads(void *user) {
	(void)user;
	subsonicdlpage_open();
}

static void menu_download_track(void *user) {
	(void)user;
	subsonicdlpage_download(shown, shown_count, menu_index, false);
}

static void menu_download_page(void *user) {
	(void)user;
	subsonicdlpage_download(shown, shown_count, menu_index, true);
}

static void row_long_pressed_cb(lv_event_t *e) {
	if (busy || switcher_back_drag_active() || player_sheet_drag_active()) {
		return;
	}
	int index = (int)(intptr_t)lv_event_get_user_data(e);
	if (index < 0 || index >= shown_count || shown[index].kind != SUBSONIC_TRACK) {
		return;
	}
	menu_index = index;
	// The lift that ends this press would otherwise arrive as a click and start
	// playback under the menu.
	lv_indev_t *indev = lv_indev_active();
	if (indev) {
		lv_indev_wait_release(indev);
	}
	static const char *const labels[] = {"subsonic_download_track", "subsonic_download_page", "subsonic_downloads"};
	popover_item_t items[] = {
		{.label = labels[0], .action = menu_download_track},
		{.label = labels[1], .action = menu_download_page},
		{.label = labels[2], .action = menu_open_downloads},
	};
	int count = 2;
	if (!subsonicdl_busy()) {
		count = 3; // a second job is not offered while one is running
	}
	popover_show(lv_event_get_current_target(e), items, count);
}

static void row_cb(lv_event_t *e) {
	if (busy || switcher_back_drag_active() || player_sheet_drag_active()) {
		return;
	}
	int index = (int)(intptr_t)lv_event_get_user_data(e);
	if (index < 0 || index >= shown_count) {
		return;
	}
	const subsonic_entry_t *item = &shown[index];
	if (item->kind == SUBSONIC_TRACK) {
		// Paths are small, fixed and contain no credentials. The playlist takes
		// copies synchronously; the bridge retains only the track metadata.
		char (*paths)[96] = calloc(SUBSONIC_PAGE_SIZE, sizeof(*paths));
		const char **pointers = calloc(SUBSONIC_PAGE_SIZE, sizeof(*pointers));
		int count = 0, selected = 0;
		char error[192] = "";
		bool ok = paths && pointers && subsonicstream_queue(shown, shown_count, index, paths, pointers,
																   &count, &selected, error, sizeof(error));
		if (ok) {
			streamturn_abandon_all();
			device_state_play_list(pointers, count, selected);
			player_refresh_now_playing();
			player_sheet_open(true);
		} else {
			toast_error(error[0] ? error : "subsonic_out_of_memory");
		}
		free(pointers);
		free(paths);
		return;
	}
	job_t job = {.kind = JOB_BROWSE};
	switch (item->kind) {
	case SUBSONIC_MUSIC_FOLDER: job.browse = SUBSONIC_BROWSE_INDEXES; break;
	case SUBSONIC_DIRECTORY: job.browse = SUBSONIC_BROWSE_DIRECTORY; break;
	case SUBSONIC_ARTIST: job.browse = SUBSONIC_BROWSE_ARTIST; break;
	case SUBSONIC_ALBUM: job.browse = SUBSONIC_BROWSE_ALBUM; break;
	default: return;
	}
	snprintf(job.id, sizeof(job.id), "%s", item->id);
	snprintf(job.title, sizeof(job.title), "%s", item->title);
	run_list(job, true);
}

static void rebuild_list(void) {
	lv_obj_clean(list_container);
	lv_obj_scroll_to_y(list_container, 0, LV_ANIM_OFF);
	if (current_list.offset > 0) {
		settingsrow_action(list_container, "subsonic_previous_page", page_cb, (void *)(intptr_t)-1);
	}
	if (!shown_count) {
		lv_obj_t *empty = lv_label_create(list_container);
		lv_obj_add_style(empty, &theme_style_text_dim, 0);
		lv_obj_set_width(empty, lv_pct(100));
		lv_label_set_long_mode(empty, LV_LABEL_LONG_WRAP);
		lv_label_set_text(empty, tr("subsonic_empty"));
	}
	for (int i = 0; i < shown_count; i++) {
		const subsonic_entry_t *item = &shown[i];
		lv_obj_t *row = settingsrow_add(list_container, "", NULL, row_cb, (void *)(intptr_t)i);
		lv_label_set_text(settingsrow_name_label(row), item->title); // data, not a translation key
		settingsrow_name_lines(row, 2);
		const lv_image_dsc_t *icon = item->kind == SUBSONIC_TRACK ? &icon_music_note :
			item->kind == SUBSONIC_ARTIST ? &icon_artist : &icon_folder;
		settingsrow_add_icon(row, icon, THEME_SEMANTIC_CYAN);
		if (item->kind == SUBSONIC_TRACK) {
			// A track already saved to the card says so, and the long press is
			// where the other two choices (this one, the whole page) live.
			if (subsonicdlpage_present(item)) {
				settingsrow_add_accent_icon(row, &icon_check);
			}
			lv_obj_add_event_cb(row, row_long_pressed_cb, LV_EVENT_LONG_PRESSED, (void *)(intptr_t)i);
		}
	}
	if (shown_more) {
		settingsrow_action(list_container, "subsonic_next_page", page_cb, (void *)(intptr_t)1);
	}
}

static void poll_cb(lv_timer_t *timer) {
	(void)timer;
	// A save started here keeps running when the user walks away from the
	// list; this tick, which runs whatever is on screen, is what says so when
	// it ends. Cheap: two counters, no I/O between jobs.
	subsonicdlpage_watch();
	pthread_mutex_lock(&job_lock);
	result_t *result = mail;
	mail = NULL;
	unsigned latest = serial, failed = failed_serial;
	failed_serial = 0;
	pthread_mutex_unlock(&job_lock);
	if (failed && failed == latest) {
		busy = false;
		toast_busy_end();
		toast_error("subsonic_out_of_memory");
	}
	if (result) {
		if (result->job.serial == latest) {
			busy = false;
			toast_busy_end();
			if (result->count < 0) {
				toast_error(result->error[0] ? result->error : "subsonic_connection_failed");
			} else if (result->job.kind == JOB_BROWSE) {
				free(shown);
				shown = result->entries;
				result->entries = NULL;
				shown_count = result->count;
				shown_more = result->more;
				current_list = result->job;
				rebuild_list();
			} else {
				toast_success("subsonic_connected");
				if (result->job.kind == JOB_CONNECT && lv_screen_active() == settings_screen) {
					memset(&draft.password, 0, sizeof(draft.password));
					switch_screen_return_to(subsonic_screen);
				}
			}
		}
		free_result(result);
	}

	char error[192];
	if (subsonicstream_take_error(error, sizeof(error))) {
		toast_error(error);
	}
	if (audio_get_status() == AUDIO_STATUS_PLAYING) {
		char current[512], next[512];
		audio_get_current_file(current, sizeof(current));
		if (subsonicstream_owns(current)) {
			double position = 0, duration = 0;
			audio_get_progress(&position, &duration);
			if (duration > 0 && position >= duration * 0.90) {
				if (playlist_peek_auto(next, sizeof(next)) && subsonicstream_owns(next)) {
					subsonicstream_prefetch(next);
				} else {
					subsonicstream_prefetch(NULL);
				}
			}
		}
	}
}

static lv_obj_t *screen(void) {
	lv_obj_t *obj = lv_obj_create(NULL);
	lv_obj_add_style(obj, &theme_style_screen, 0);
	lv_obj_add_event_cb(obj, page_left_cb, LV_EVENT_SCREEN_UNLOADED, NULL);
	switcher_attach_back_gesture(obj);
	return obj;
}

// A paragraph of explanation under the row it belongs to. Wraps, because a
// translated note is longer than the panel is wide and an ellipsis would drop
// the half that says what the setting does.
static void add_note(lv_obj_t *container, const char *key) {
	lv_obj_t *note = lv_label_create(container);
	lv_obj_add_style(note, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(note, &font_ui_18, 0);
	lv_obj_set_width(note, lv_pct(100));
	lv_label_set_long_mode(note, LV_LABEL_LONG_WRAP);
	lv_label_set_text(note, tr(key));
}

static void build_settings(void) {
	settings_screen = screen();
	lv_obj_t *container = settingsrow_page(settings_screen, config, "subsonic_server_settings");
	lv_obj_t *row = settingsrow_add(container, "subsonic_server_url", &server_value, edit_cb, (void *)(intptr_t)0);
	settingsrow_add_icon(row, &icon_wifi_high, THEME_SEMANTIC_BLUE);
	settingsrow_add(container, "subsonic_username", &user_value, edit_cb, (void *)(intptr_t)1);
	settingsrow_add(container, "password", NULL, edit_cb, (void *)(intptr_t)2);
	settingsrow_toggle(container, "subsonic_remember", &remember_switch, remember_cb);
	// Straight under the account, because it is what a self-hosted server needs
	// before any of the above can work over https://.
	settingsrow_toggle(container, "subsonic_allow_self_signed", &relaxed_switch, certificates_cb);
	add_note(container, "subsonic_allow_self_signed_note");
	row = settingsrow_action(container, "subsonic_connect_save", connect_cb, NULL);
	settingsrow_add_icon(row, &icon_check, THEME_SEMANTIC_GREEN);
	lv_obj_t *pills;
	settingsrow_pills(container, "subsonic_format", &pills);
	format_pills[0] = settingsrow_pill_text(pills, "MP3", SUBSONIC_FORMAT_MP3, format_cb);
	format_pills[1] = settingsrow_pill_text(pills, "Opus", SUBSONIC_FORMAT_OPUS, format_cb);
	format_pills[2] = settingsrow_pill_text(pills, "Raw", SUBSONIC_FORMAT_RAW, format_cb);
	settingsrow_pills(container, "subsonic_bitrate", &pills);
	bitrate_pills[0] = settingsrow_pill_text(pills, "256", 256, bitrate_cb);
	bitrate_pills[1] = settingsrow_pill_text(pills, "320", 320, bitrate_cb);
	// Where the downloads go and what format they are saved in: the same
	// quality the stream uses, so this note sits under the pills.
	add_note(container, "subsonic_downloads_note");
	row = settingsrow_action(container, "subsonic_forget_server", forget_cb, NULL);
	settingsrow_add_icon(row, &icon_log_out, THEME_SEMANTIC_RED);
	add_note(container, "subsonic_settings_note");
	player_sheet_attach_drag(container, true);

	edit_screen = screen();
	edit_title = settingsrow_title(edit_screen, config, "subsonic_server_url");
	edit_field = lv_textarea_create(edit_screen);
	lv_textarea_set_one_line(edit_field, true);
	lv_obj_add_style(edit_field, &theme_style_card, 0);
	lv_obj_set_style_text_font(edit_field, &font_ui_24, 0);
	lv_obj_set_style_pad_all(edit_field, 8, 0);
	lv_obj_set_size(edit_field, config->screen_width - 2 * config->padding, config->screen_width < 320 ? 44 : 64);
	lv_obj_align(edit_field, LV_ALIGN_TOP_LEFT, config->padding, settingsrow_content_top(config));
	keyboard_style_caret(edit_field);
	keyboard = keyboard_create(edit_screen, config->screen_width, config->screen_width < 320 ? 144 : 316,
							   edit_field, NULL, "ok", accept_edit_cb, NULL);
	lv_obj_add_event_cb(edit_screen, edit_left_cb, LV_EVENT_SCREEN_UNLOADED, NULL);
	subsonic_credentials(&draft, &draft_remember);
	settings_refresh();
}

// Tiles call a plain action: the grid already drops a tap that was really a
// swipe, and nothing here needs the event.
static void home_settings_action(void) { open_settings_cb(NULL); }
static void home_test_action(void) { ping_cb(NULL); }
static void home_folders_action(void) { browse(SUBSONIC_BROWSE_FOLDERS); }
static void home_artists_action(void) { browse(SUBSONIC_BROWSE_ARTISTS); }
static void menu_downloads_btn_cb(lv_event_t *e) {
	(void)e;
	if (switcher_back_drag_active()) {
		return;
	}
	subsonicdlpage_open();
}

// The cards are twice a menu row's height and they are the whole page, so the
// picture is drawn large and centred with the caption underneath in every
// theme -- including the flat ones, whose menu rows would otherwise put the
// glyph beside the text. Dark and Light show the illustrations at the size the
// generator emits them; the flat themes show the glyph in their own accent.
static void home_style_tiles(void) {
	if (!home_grid) {
		return;
	}
	static const lv_image_dsc_t *const art[SUBSONIC_HOME_TILES] = {
		&icon_menu_sub_server,
		&icon_menu_sub_test,
		&icon_menu_sub_folders,
		&icon_menu_sub_artists,
	};
	static const lv_image_dsc_t *const glyphs[SUBSONIC_HOME_TILES] = {
		&icon_sub_server_glyph,
		&icon_sub_test_glyph,
		&icon_sub_folders_glyph,
		&icon_sub_artists_glyph,
	};
	static const theme_semantic_t tones[SUBSONIC_HOME_TILES] = {
		THEME_SEMANTIC_BLUE,
		THEME_SEMANTIC_GREEN,
		THEME_SEMANTIC_CYAN,
		THEME_SEMANTIC_PURPLE,
	};
	bool glyph = theme_uses_function_glyphs();
	for (int i = 0; i < SUBSONIC_HOME_TILES; i++) {
		// Both pictures are rendered at exactly the size they are drawn at, so
		// a card shows its own pixels and never a resampled copy of them.
		gridpage_set_tile_icon_style(home_grid, i, glyph ? glyphs[i] : art[i],
									 glyph ? SUBSONIC_TILE_GLYPH_PX : SUBSONIC_TILE_ART_PX, glyph,
									 theme_semantic_color(tones[i]), false);
		// Picture above, caption underneath, in every theme. Re-applying the
		// layout also measures the caption against the picture's new width.
		gridpage_set_tile_orientation(home_grid, i, false);
	}
}

void subsonicpage_init(gui_config_t *cfg) {
	config = cfg;
	subsonic_init();
	device_state_add_prepare_cb(subsonicstream_prepare);
	streamturn_register_abandon(subsonicstream_cancel);
	subsonic_screen = screen();
	settingsrow_title(subsonic_screen, cfg, "subsonic");
	// Four destinations, so two square tiles fill the panel under the heading
	// exactly: the grid is sized from the space that is left, and there is
	// never a fifth row to scroll to.
	const grid_entry_t home_entries[] = {
		{"subsonic_server_settings", &icon_menu_sub_server, NULL, home_settings_action},
		{"subsonic_test_connection", &icon_menu_sub_test, NULL, home_test_action},
		{"subsonic_folders", &icon_menu_sub_folders, NULL, home_folders_action},
		{"artists", &icon_menu_sub_artists, NULL, home_artists_action},
	};
	home_grid = gridpage_build(subsonic_screen, cfg, home_entries, SUBSONIC_HOME_TILES, 2, 2, true);
	// The captions are longer than one line in most languages ("Paramètres du
	// serveur", "Test de la connexion"), so let them use two lines instead of
	// an ellipsis.
	for (int i = 0; i < SUBSONIC_HOME_TILES; i++) {
		gridpage_set_tile_label_lines(home_grid, i, 2);
	}
	home_style_tiles();
	theme_register_refresh(home_style_tiles);
	player_sheet_attach_drag(subsonic_screen, true);
	// The downloads page is one tap from the tiles, in the corner the Music and
	// Qobuz pages keep their own corner controls: a fourth tile would make the
	// 2x2 grid three rows tall, and this page has exactly four destinations.
	lv_obj_t *downloads_btn = lv_btn_create(subsonic_screen);
	settingsrow_place_corner_button(downloads_btn, cfg, 0);
	lv_obj_set_style_bg_opa(downloads_btn, LV_OPA_TRANSP, 0);
	lv_obj_set_style_border_width(downloads_btn, 0, 0);
	lv_obj_set_style_shadow_width(downloads_btn, 0, 0);
	lv_obj_set_style_pad_all(downloads_btn, 0, 0);
	lv_obj_t *downloads_icon = lv_image_create(downloads_btn);
	lv_image_set_src(downloads_icon, &icon_download);
	lv_obj_add_style(downloads_icon, &theme_style_icon, 0);
	settingsrow_scale_corner_icon(downloads_icon, cfg);
	lv_obj_center(downloads_icon);
	lv_obj_add_event_cb(downloads_btn, menu_downloads_btn_cb, LV_EVENT_CLICKED, NULL);
	// The heading reserves the corner, exactly as a page with one corner button
	// does, or a long translation of "Subsonic" would run under the arrow.
	settingsrow_title_corner_slots(settingsrow_page_title(subsonic_screen), cfg, 1);

	subsonicdlpage_init(cfg);

	list_screen = screen();
	list_container = settingsrow_page(list_screen, cfg, "subsonic");
	list_title = settingsrow_page_title(list_screen);
	player_sheet_attach_drag(list_container, true);
	switcher_set_back_guard(list_screen, list_back);
	switcher_set_back_guard_peek(list_screen, list_back_has_step);
	build_settings();
	theme_register_refresh(quality_refresh);
	// Piggybacks on LVGL's existing event loop. Idle fetch workers themselves
	// sleep on conditions; this tiny two-Hz check does no REST/socket/file I/O.
	lv_timer_create(poll_cb, 500, NULL);
}
