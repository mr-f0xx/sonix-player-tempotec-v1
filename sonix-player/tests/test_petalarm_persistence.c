/* End-to-end alarm rollback with the real queue, device_state and SQLite.
 * Only the decoder, mixer and other hardware-facing services are faked. */
#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "src/system/core/config.h"
#include "src/system/decode/apedec.h"
#include "src/system/decode/mp4.h"
#include "src/system/library/geniusbuild.h"
#include "src/system/library/geniusdb.h"
#include "src/system/pet/petalarm.h"
#include "src/system/playback/audiobook.h"
#include "src/system/audio/replaygain.h"
#include "src/system/streaming/podcast.h"
#include "src/system/device/system.h"

// Include the runtime so its real start and failure tick can be driven without
// waiting for a wall-clock minute or depending on an LVGL event loop.
#include "src/system/pet/petalarm_runtime.c"

static char alarm_path[512], old_a[512], old_b[512], db_path[512], cfg_path[512];
static const char *stream_a = "/stream/qobuz/one.flac";
static const char *stream_b = "/stream/qobuz/two.flac";
static char current_file[512];
static audio_status_t status = AUDIO_STATUS_STOPPED;
static double position;
static bool pcm_open, radio_active, radio_playing, playback_context_active;
static long quiet_ms;
static unsigned stops;
static int errors;

// The transport announces a request just as audio.c does. The test decides
// when it either opens or fails; it never opens a real PCM.
int audio_play(const char *path) {
	stops++; // the command serial also changes on a later play
	snprintf(current_file, sizeof(current_file), "%s", path);
	status = AUDIO_STATUS_PLAYING;
	position = 0;
	pcm_open = false;
	quiet_ms = -1;
	return 0;
}
int audio_play_at(const char *path, double at) {
	audio_play(path);
	position = at;
	return 0;
}
void audio_restore_stopped_file(const char *path, double at) {
	snprintf(current_file, sizeof(current_file), "%s", path ? path : "");
	status = AUDIO_STATUS_STOPPED;
	position = at;
	quiet_ms = 0;
	stops++;
}
audio_status_t audio_get_status(void) { return status; }
void audio_get_current_file(char *out, size_t size) { snprintf(out, size, "%s", current_file); }
void audio_get_progress(double *cur, double *total) {
	if (cur) *cur = position;
	if (total) *total = 300;
}
void audio_get_stream_info(int *rate, int *channels) { if (rate) *rate = 44100; if (channels) *channels = 2; }
void audio_set_speed(double value) { (void)value; }
bool audio_device_is_open(void) { return pcm_open; }
long audio_quiet_ms(void) { return quiet_ms; }
unsigned audio_command_serial(void) { return stops; }
bool audio_playback_context_active(void) { return playback_context_active; }
bool audio_external_is_active(void) { return false; }

bool clock_is_set(void) { return true; }
void power_screen_on(void) {}
void toast_error(const char *key) { (void)key; errors++; }
void player_pause_for_loading(void) {
	if (status == AUDIO_STATUS_PLAYING) status = AUDIO_STATUS_PAUSED;
	radio_playing = false;
}
void player_refresh_now_playing(void) {}
void player_key_play_pause(void) { assert(!"unexpected current-source toggle"); }

bool volume_fade_begin(void) { return false; }
bool volume_fade_active(void) { return false; }
void volume_fade_progress(int elapsed, int duration) { (void)elapsed; (void)duration; }
void volume_fade_stop(void) {}
int get_volume_percent(void) { return 40; }
const char *tr(const char *key) { return key; }

bool radio_is_active(void) { return radio_active; }
bool radio_is_playing(void) { return radio_active && radio_playing; }
bool radio_is_connecting(void) { return false; }
bool radio_current_station(radio_station_t *out) {
	if (!radio_active) return false;
	memset(out, 0, sizeof(*out));
	snprintf(out->url, sizeof(out->url), "%s", "https://radio.example/stream");
	return true;
}
bool radio_play(const radio_station_t *station) {
	assert(!strcmp(station->url, "https://radio.example/stream"));
	radio_active = radio_playing = true;
	return true;
}
void radio_clear(void) { radio_active = radio_playing = false; }
void radio_get_now(radio_now_t *out) {
	memset(out, 0, sizeof(*out));
	out->active = radio_active;
	out->playing = radio_playing;
}

bool qobuzcache_owns(const char *path) { return path && !strncmp(path, "/stream/qobuz/", 14); }
bool tidalcache_owns(const char *path) { (void)path; return false; }
bool soundcloudcache_is_soundcloud_path(const char *path) { (void)path; return false; }
bool subsonicstream_owns(const char *path) { (void)path; return false; }
bool podcastcache_is_episode(const char *path) { (void)path; return false; }
void audiobook_track_changed(const char *path) { (void)path; }
bool audiobook_is_playing(void) { return false; }
double audiobook_speed(void) { return 1.0; }
double podcast_speed(void) { return 1.0; }
void audiobook_note_position(double seconds, double total, bool force) { (void)seconds; (void)total; (void)force; }
void replaygain_load(const song_metadata_t *meta) { (void)meta; }
void metadata_read(const char *path, song_metadata_t *out) { (void)path; memset(out, 0, sizeof(*out)); }
char *read_battery_percent(void) { return "100"; }
bool read_battery_charging(void) { return false; }

// library.c includes scanner hooks; no scan or codec is needed by this test.
mp4_codec_t mp4_audio_codec(const mp4_file_t *m) { (void)m; return MP4_CODEC_NONE; }
const unsigned char *mp4_audio_config(const mp4_file_t *m, int *len) { (void)m; if (len) *len = 0; return NULL; }
int mp4_audio_sample_rate(const mp4_file_t *m) { (void)m; return 0; }
bool mp4_file_has_video(const char *path) { (void)path; return false; }
mp4_file_t *mp4_open(const char *path) { (void)path; return NULL; }
void mp4_close(mp4_file_t *m) { (void)m; }
bool apedec_probe(const char *path, int *rate, int *bits) {
	(void)path; if (rate) *rate = 0; if (bits) *bits = 0; return false;
}
geniusbuild_result_t geniusbuild_run(const char *path, const atomic_bool *cancel,
                                     geniusbuild_progress_cb progress, void *user) {
	(void)path; (void)cancel; (void)progress; (void)user; return GENIUSBUILD_CURRENT;
}
bool geniusdb_open(const char *path) { (void)path; return true; }
void geniusdb_close(void) {}
bool geniusdb_available(void) { return true; }

static void expect_saved_paths(const char *album) {
	library_index_spec_t spec;
	assert(!library_queue_load_query(&spec, NULL));
	char **paths = NULL;
	int index = -1;
	bool custom = false, ordered = true;
	int count = library_queue_load(&paths, &index, &custom, &ordered);
	assert(count == 2 && index == 1 && custom && !ordered);
	assert(!strcmp(paths[0], old_a) && !strcmp(paths[1], old_b));
	library_queue_free(paths, count);
	char found[256];
	bool has_album = library_queue_load_album(found, sizeof(found));
	assert(has_album == (album && album[0]));
	if (has_album) assert(!strcmp(found, album));
}

static void expect_remembered(const char *path, double expected) {
	char found[512];
	double at = -1;
	assert(library_playback_state_load(found, sizeof(found), &at));
	assert(!strcmp(found, path) && at == expected);
}

static void reopen_database(void) {
	library_close();
	assert(library_open(db_path));
}

static void seed_saved_paths(void) {
	const char *paths[] = {old_a, old_b};
	library_queue_save(paths, 2, 1, true, false);
	library_playback_state_save(old_b, 83.5);
}

static void open_alarm_path(const char *path) {
	start_alarm(5000);
	assert(attempt.queue && !strcmp(current_file, path));
	assert(status == AUDIO_STATUS_PLAYING && quiet_ms < 0);
}
static void open_alarm(void) { open_alarm_path(alarm_path); }

static void fail_alarm(void) {
	status = AUDIO_STATUS_STOPPED;
	quiet_ms = 0;
	attempt_tick(6000);
	assert(!attempt.queue);
}

static void test_empty_previous_queue(void) {
	assert(playlist_count() == 0 && !current_file[0]);
	seed_saved_paths();
	open_alarm();
	// A normal player poll and shutdown flush must not remember an unconfirmed
	// replacement or overwrite a previously durable local queue.
	device_state_remember_note();
	device_state_remember_flush();
	expect_saved_paths("");
	expect_remembered(old_b, 83.5);
	fail_alarm();
	assert(playlist_count() == 0 && !current_file[0]);
	device_state_remember_note(); // a later ordinary poll must not save the failed path
	expect_saved_paths("");
	expect_remembered(old_b, 83.5);
	reopen_database();
	expect_saved_paths("");
	expect_remembered(old_b, 83.5);

	// A timed-out decoder may still update its status/progress before it exits.
	// Keep the persistence hold until that context is gone, then clear it again.
	open_alarm();
	playback_context_active = true;
	attempt_tick(65000);
	assert(attempt.recovering && !current_file[0] && playlist_count() == 0);
	status = AUDIO_STATUS_PLAYING;
	position = 99;
	device_state_remember_note();
	device_state_remember_flush();
	expect_saved_paths("");
	expect_remembered(old_b, 83.5);
	playback_context_active = false;
	attempt_tick(66000);
	assert(!attempt.recovering && status == AUDIO_STATUS_STOPPED && position == 0);
	device_state_remember_note();
	reopen_database();
	expect_saved_paths("");
	expect_remembered(old_b, 83.5);
}

static void test_streaming_previous_queue(void) {
	const char *paths[] = {stream_a, stream_b};
	device_state_play_list(paths, 2, 0);
	status = AUDIO_STATUS_PAUSED;
	position = 17;
	seed_saved_paths();
	playlist_set_album("Streaming label");
	open_alarm();
	device_state_remember_note();
	device_state_queue_changed();
	expect_saved_paths("Streaming label");
	expect_remembered(old_b, 83.5);
	fail_alarm();
	assert(playlist_count() == 2 && position == 17 && !strcmp(current_file, stream_a));
	char path[512];
	assert(playlist_current_path(path, sizeof(path)) && !strcmp(path, stream_a));
	device_state_remember_note();
	expect_saved_paths("Streaming label");
	expect_remembered(old_b, 83.5);
	reopen_database();
	expect_saved_paths("Streaming label");
	expect_remembered(old_b, 83.5);
}

static void test_streaming_over_saved_query(void) {
	library_playlist_row_t row = {.seconds = 30, .present = true};
	snprintf(row.path, sizeof(row.path), "%s", old_a);
	assert(library_playlist_append("Archive", &row, library_session()));
	snprintf(row.path, sizeof(row.path), "%s", old_b);
	assert(library_playlist_append("Archive", &row, library_session()));
	library_index_t *ix = library_index_open(LIBRARY_LIST_PLAYLIST, LIBRARY_FILTER_NONE, "Archive",
	                                         LIBRARY_ORDER_DEFAULT, false);
	assert(ix && library_index_count(ix) == 2 && device_state_play_index(ix, 1));
	assert(playlist_insert_next(old_a));
	device_state_queue_changed();
	library_playback_state_save(old_b, 83.5);
	const char *paths[] = {stream_a, stream_b};
	device_state_play_list(paths, 2, 0); // transient: leaves the saved query alone
	playlist_set_album("Streaming label");
	status = AUDIO_STATUS_PAUSED;
	position = 17;

	open_alarm();
	library_index_spec_t spec;
	int at = -1;
	assert(library_queue_load_query(&spec, &at));
	assert(!strcmp(spec.value, "Archive") && at == 1);
	fail_alarm();
	reopen_database();
	assert(library_queue_load_query(&spec, &at));
	assert(!strcmp(spec.value, "Archive") && at == 1);
	char **extra = NULL;
	int *slots = NULL;
	int count = library_queue_load_extra(&extra, &slots);
	assert(count == 1 && !strcmp(extra[0], old_a) && slots[0] == 2);
	library_queue_free(extra, count);
	free(slots);
	expect_remembered(old_b, 83.5);
}

static void test_local_queue_and_success(void) {
	const char *paths[] = {old_a, old_b};
	device_state_play_list(paths, 2, 1);
	playlist_set_album("Old album");
	device_state_remember_restored(old_b, 83.5);
	library_playback_state_save(old_b, 83.5);
	status = AUDIO_STATUS_PAUSED;
	position = 83.5;
	open_alarm();
	expect_saved_paths("Old album");
	expect_remembered(old_b, 83.5);
	fail_alarm();
	assert(playlist_count() == 2 && !strcmp(current_file, old_b) && position == 83.5);
	expect_saved_paths("Old album");
	expect_remembered(old_b, 83.5);

	// A confirmed replacement is saved, including its track even when the
	// previous boot-restore seek latch is still waiting at a larger position.
	device_state_remember_restored(old_b, 83.5);
	open_alarm();
	expect_saved_paths("Old album");
	pcm_open = true;
	quiet_ms = 0;
	attempt_tick(6000);
	assert(!attempt.queue);
	library_index_spec_t spec;
	int at = -1;
	assert(library_queue_load_query(&spec, &at));
	assert(spec.kind == LIBRARY_LIST_PLAYLIST && !strcmp(spec.value, "Morning") && at == 0);
	expect_remembered(alarm_path, 0);
	char album[256];
	assert(!library_queue_load_album(album, sizeof(album)));
	reopen_database();
	assert(library_queue_load_query(&spec, &at) && !strcmp(spec.value, "Morning"));
	expect_remembered(alarm_path, 0);
}

static void test_same_track_as_restore(void) {
	const char *paths[] = {old_a, old_b};
	device_state_play_list(paths, 2, 1);
	library_playback_state_save(old_b, 83.5);
	device_state_remember_restored(old_b, 83.5);
	status = AUDIO_STATUS_PAUSED;
	position = 83.5;
	assert(petalarm_set_playlist("Same"));
	open_alarm_path(old_b); // a fresh play of the same file starts at zero
	pcm_open = true;
	quiet_ms = 0;
	attempt_tick(6000);
	library_index_spec_t spec;
	assert(library_queue_load_query(&spec, NULL) && !strcmp(spec.value, "Same"));
	expect_remembered(old_b, 0);
	assert(petalarm_set_playlist("Morning"));
}

static void test_radio_and_underlying_track(void) {
	const char *paths[] = {old_a, old_b};
	device_state_play_list(paths, 2, 1);
	playlist_set_album("Old album");
	library_playback_state_save(old_b, 83.5);
	device_state_remember_restored(old_b, 83.5);
	status = AUDIO_STATUS_STOPPED;
	position = 83.5;
	radio_active = radio_playing = true;
	open_alarm();
	expect_saved_paths("Old album");
	fail_alarm();
	assert(radio_active && radio_playing && status == AUDIO_STATUS_STOPPED);
	assert(!strcmp(current_file, old_b) && position == 83.5);
	const char *loaded = NULL;
	device_state_loaded_metadata(&loaded);
	assert(loaded && !strcmp(loaded, old_b));
	device_state_remember_note(); // live radio is not a remembered file
	expect_saved_paths("Old album");
	expect_remembered(old_b, 83.5);
	radio_clear();
}

static void test_user_edit_and_shutdown(void) {
	const char *paths[] = {old_a, old_b};
	device_state_play_list(paths, 2, 1);
	playlist_set_album("Old album");
	library_playback_state_save(old_b, 83.5);
	device_state_remember_restored(old_b, 83.5);
	status = AUDIO_STATUS_PAUSED;
	position = 83.5;
	open_alarm();
	assert(playlist_insert_next(old_a));
	device_state_queue_changed(); // deferred, but the next tick honours the edit
	expect_saved_paths("Old album");
	expect_remembered(old_b, 83.5);
	attempt_tick(6000);
	library_index_spec_t spec;
	assert(library_queue_load_query(&spec, NULL) && !strcmp(spec.value, "Morning"));
	char **extra = NULL;
	int *slots = NULL;
	int count = library_queue_load_extra(&extra, &slots);
	assert(count == 1 && !strcmp(extra[0], old_a) && slots[0] == 1);
	library_queue_free(extra, count);
	free(slots);
	expect_remembered(alarm_path, 0);

	// Both power-off and reboot flush before ending the attempt. An unconfirmed
	// request must leave the previous queue, album and track intact.
	device_state_play_list(paths, 2, 1);
	playlist_set_album("Old album");
	library_playback_state_save(old_b, 83.5);
	device_state_remember_restored(old_b, 83.5);
	status = AUDIO_STATUS_PAUSED;
	position = 83.5;
	open_alarm();
	device_state_remember_flush();
	petalarm_runtime_shutdown();
	reopen_database();
	expect_saved_paths("Old album");
	expect_remembered(old_b, 83.5);

	// Conversely, if the PCM really opened before shutdown, its queue and
	// track are committed even when the watcher has not yet had another tick.
	device_state_play_list(paths, 2, 1);
	playlist_set_album("Old album");
	library_playback_state_save(old_b, 83.5);
	device_state_remember_restored(old_b, 83.5);
	status = AUDIO_STATUS_PAUSED;
	position = 83.5;
	open_alarm();
	pcm_open = true;
	quiet_ms = 0;
	device_state_remember_flush();
	petalarm_runtime_shutdown();
	reopen_database();
	assert(library_queue_load_query(&spec, NULL) && !strcmp(spec.value, "Morning"));
	expect_remembered(alarm_path, 0);
}

int main(void) {
	char dir[] = "/tmp/sonix-alarm-persistence-XXXXXX";
	assert(mkdtemp(dir));
	snprintf(db_path, sizeof(db_path), "%s/library.db", dir);
	snprintf(cfg_path, sizeof(cfg_path), "%s/settings.ini", dir);
	snprintf(alarm_path, sizeof(alarm_path), "%s/alarm.flac", dir);
	snprintf(old_a, sizeof(old_a), "%s/old-a.flac", dir);
	snprintf(old_b, sizeof(old_b), "%s/old-b.flac", dir);
	FILE *f = fopen(alarm_path, "wb");
	assert(f && fclose(f) == 0);
	f = fopen(old_b, "wb");
	assert(f && fclose(f) == 0);
	(void)config_init(cfg_path);
	config_set_int("player", "remember_track", 1);
	assert(library_open(db_path));
	library_playlist_row_t row = {.seconds = 1, .present = true};
	snprintf(row.path, sizeof(row.path), "%s", alarm_path);
	snprintf(row.title, sizeof(row.title), "%s", "Alarm");
	assert(library_playlist_append("Morning", &row, library_session()));
	snprintf(row.path, sizeof(row.path), "%s", old_b);
	assert(library_playlist_append("Same", &row, library_session()));
	petalarm_init();
	petalarm_set_enabled(true);
	petalarm_set_fade_seconds(0);
	assert(petalarm_set_playlist("Morning"));

	test_empty_previous_queue();
	test_streaming_previous_queue();
	test_streaming_over_saved_query();
	test_local_queue_and_success();
	test_same_track_as_restore();
	test_radio_and_underlying_track();
	test_user_edit_and_shutdown();
	playlist_clear();
	library_close();
	unlink(alarm_path);
	unlink(old_b);
	unlink(db_path);
	unlink(cfg_path);
	rmdir(dir);
	puts("pet alarm persistence (real SQLite and queue): all checks passed");
	return 0;
}
