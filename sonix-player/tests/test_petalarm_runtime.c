/* Alarm playback integration, with the real scheduler and fake transports.
 * No GUI, mixer, card, radio or real clock is touched. */
#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "src/system/core/config.h"
#include "src/system/pet/petalarm.h"

static time_t wall_now;
static time_t fake_time(time_t *out) {
	if (out) *out = wall_now;
	return wall_now;
}
#define time fake_time
#include "src/system/pet/petalarm_runtime.c"
#undef time

// Exercise the real sleep-timer state machine with a controllable monotonic
// clock, just as the player's playback-status callback drives it.
static long long sleep_now_ms;
static int fake_clock_gettime(clockid_t id, struct timespec *out) {
	assert(id == CLOCK_MONOTONIC);
	out->tv_sec = sleep_now_ms / 1000;
	out->tv_nsec = (long)(sleep_now_ms % 1000) * 1000000L;
	return 0;
}
#define clock_gettime fake_clock_gettime
#include "src/system/playback/sleeptimer.c"
#undef clock_gettime

struct library_index { int count; };
struct playlist_checkpoint { int unused; };
static struct library_index index_fixture;
static struct playlist_checkpoint checkpoint_fixture;
static bool clock_ready, external_active, index_available, first_readable, play_index_ok;
static bool checkpoint_available, checkpoint_stale, persistence_held;
static unsigned queue_revision, stops_serial;
static int queue_index;
static playback_mode_t queue_mode;
static int checkpoints, checkpoint_restores, checkpoint_discards, preserving_plays, folder_plays, radio_plays;
static int persistence_begins, persistence_commits, persistence_cancels, queue_clears;
static double progress_seconds;
static bool pcm_open, playback_context_active;
static long quiet_ms;
static audio_status_t audio_status;
static bool radio_active, radio_playing, radio_connecting;
static char loaded_file[512];
static int wakes, toggles, pauses, index_opens, index_closes, index_plays, refreshes;
static char errors[4][64];
static int error_count;
static bool fade_active, can_fade;
static int volume, fade_target, fade_begins, fade_stops, progress_calls;

bool clock_is_set(void) { return clock_ready; }
audio_status_t audio_get_status(void) { return audio_status; }
unsigned audio_command_serial(void) { return stops_serial; }
bool audio_playback_context_active(void) { return playback_context_active; }
void audio_get_progress(double *current, double *total) {
	if (current) *current = progress_seconds;
	if (total) *total = 300;
}
bool audio_device_is_open(void) { return pcm_open; }
long audio_quiet_ms(void) { return quiet_ms; }
bool audio_external_is_active(void) { return external_active; }
bool radio_is_active(void) { return radio_active; }
bool radio_is_connecting(void) { return radio_active && radio_connecting; }
bool radio_is_playing(void) { return radio_active && (radio_playing || radio_connecting); }
bool radio_current_station(radio_station_t *out) {
	if (!radio_active) return false;
	if (out) snprintf(out->url, sizeof(out->url), "%s", "https://radio.example/stream");
	return true;
}
bool radio_play(const radio_station_t *station) {
	assert(station && !strcmp(station->url, "https://radio.example/stream"));
	radio_plays++;
	radio_active = radio_connecting = true;
	radio_playing = false;
	audio_status = AUDIO_STATUS_STOPPED;
	return true;
}
void radio_clear(void) { radio_active = radio_playing = radio_connecting = false; }
void power_screen_on(void) { wakes++; }
void toast_error(const char *text) {
	assert(error_count < 4);
	snprintf(errors[error_count++], sizeof(errors[0]), "%s", text);
}
const song_metadata_t *device_state_loaded_metadata(const char **file_out) {
	if (file_out) *file_out = loaded_file;
	return NULL;
}

library_index_t *library_index_open(library_list_t kind, library_filter_t filter, const char *name,
									library_order_t order, bool desc) {
	assert(kind == LIBRARY_LIST_PLAYLIST && filter == LIBRARY_FILTER_NONE);
	assert(order == LIBRARY_ORDER_DEFAULT && !desc && !strcmp(name, "Morning"));
	index_opens++;
	return index_available ? &index_fixture : NULL;
}
void library_index_close(library_index_t *ix) {
	if (ix) {
		assert(ix == &index_fixture);
		index_closes++;
	}
}
int library_index_count(const library_index_t *ix) { return ix ? ix->count : 0; }
int library_index_window(const library_index_t *ix, int offset, int count, library_row_cb cb, void *user) {
	assert(ix == &index_fixture && offset == 0 && count == 1 && cb);
	return cb("Track", "/card/first.flac", "Artist", user) ? 1 : 0;
}
bool library_playback_track_readable(const char *path) {
	assert(!strcmp(path, "/card/first.flac"));
	return first_readable;
}
playlist_checkpoint_t *playlist_checkpoint_take(void) {
	if (!checkpoint_available) return NULL;
	checkpoints++;
	queue_revision++;
	return &checkpoint_fixture;
}
bool playlist_checkpoint_restore(playlist_checkpoint_t *saved) {
	assert(saved == &checkpoint_fixture);
	checkpoint_restores++;
	if (checkpoint_stale) return false;
	queue_revision++;
	return true;
}
void playlist_checkpoint_discard(playlist_checkpoint_t *saved) {
	if (saved) {
		assert(saved == &checkpoint_fixture);
		checkpoint_discards++;
	}
}
unsigned playlist_revision(void) { return queue_revision; }
int playlist_current_index(void) { return queue_index; }
playback_mode_t playlist_get_mode(void) { return queue_mode; }
void playlist_clear(void) { queue_clears++; queue_revision++; }
void device_state_tentative_playback_begin(void) {
	assert(!persistence_held);
	persistence_held = true;
	persistence_begins++;
}
void device_state_tentative_playback_end(bool commit_current) {
	assert(persistence_held);
	persistence_held = false;
	if (commit_current) persistence_commits++;
	else persistence_cancels++;
}
static void resume_file(const char *file, double position) {
	assert(!strcmp(file, "/card/old.flac") && position == 17.0);
	snprintf(loaded_file, sizeof(loaded_file), "%s", file);
	progress_seconds = position;
	audio_status = AUDIO_STATUS_PLAYING;
	radio_active = radio_playing = radio_connecting = false;
}
void device_state_play_preserving_queue_at(const char *file, double position) {
	preserving_plays++;
	resume_file(file, position);
}
void device_state_play_file_at(const char *file, double position) {
	folder_plays++;
	resume_file(file, position);
}
void audio_restore_stopped_file(const char *file, double position) {
	(void)file;
	progress_seconds = position;
	audio_status = AUDIO_STATUS_STOPPED;
	stops_serial++;
}
void device_state_restore_stopped_file(const char *file, double position) {
	snprintf(loaded_file, sizeof(loaded_file), "%s", file ? file : "");
	audio_restore_stopped_file(file, position);
}
void player_pause_for_loading(void) {
	pauses++;
	if (audio_status == AUDIO_STATUS_PLAYING) {
		audio_status = AUDIO_STATUS_PAUSED;
		sleeptimer_note_playing(SLEEPTIMER_COUNT);
	}
	radio_playing = radio_connecting = false;
}
bool device_state_play_index(library_index_t *ix, int start_index) {
	assert(ix == &index_fixture && start_index == 0);
	index_plays++; // the real queue takes ownership on success or failure
	queue_revision++; // loading may already have cleared the outgoing queue
	if (!play_index_ok) return false;
	progress_seconds = 0;
	audio_status = AUDIO_STATUS_PLAYING;
	radio_active = radio_playing = radio_connecting = false;
	snprintf(loaded_file, sizeof(loaded_file), "%s", "/card/first.flac");
	return true;
}
void player_refresh_now_playing(void) {
	refreshes++;
	if (audio_status == AUDIO_STATUS_PLAYING) sleeptimer_note_playing(SLEEPTIMER_MUSIC);
}
void player_key_play_pause(void) {
	toggles++;
	if (radio_active) {
		radio_connecting = true;
		sleeptimer_note_playing(SLEEPTIMER_COUNT);
	} else {
		audio_status = AUDIO_STATUS_PLAYING;
		sleeptimer_note_playing(SLEEPTIMER_MUSIC);
	}
}

bool volume_fade_begin(void) {
	fade_begins++;
	if (!can_fade || volume == 0) return false;
	fade_target = volume;
	volume = 0;
	fade_active = true;
	return true;
}
bool volume_fade_active(void) { return fade_active; }
void volume_fade_stop(void) {
	if (!fade_active) return;
	fade_stops++;
	fade_active = false;
	volume = fade_target;
}
void volume_fade_progress(int elapsed, int duration) {
	assert(fade_active && duration > 0);
	progress_calls++;
	if (elapsed >= duration) volume_fade_stop();
	else volume = fade_target * elapsed / duration;
}

static char test_config_path[] = "/tmp/sonix-alarm-runtime-XXXXXX";
static const time_t due = 1773039600; /* Monday, 9 March 2026 07:00 UTC */

static void fresh(const char *playlist, int fade) {
	petalarm_runtime_shutdown();
	unlink(test_config_path);
	(void)config_init(test_config_path);
	sleep_now_ms = 0;
	sleeptimer_init();
	petalarm_init();
	petalarm_set_enabled(true);
	petalarm_set_fade_seconds(fade);
	assert(petalarm_set_playlist(playlist));
	clock_ready = true;
	external_active = false;
	index_available = first_readable = play_index_ok = checkpoint_available = true;
	checkpoint_stale = false;
	queue_revision = 1;
	queue_index = 0;
	queue_mode = PLAYBACK_MODE_NORMAL;
	stops_serial = 0;
	checkpoints = checkpoint_restores = checkpoint_discards = 0;
	preserving_plays = folder_plays = radio_plays = 0;
	assert(!persistence_held);
	persistence_begins = persistence_commits = persistence_cancels = queue_clears = 0;
	progress_seconds = 17;
	index_fixture.count = 2;
	pcm_open = true;
	playback_context_active = false;
	quiet_ms = 0;
	audio_status = AUDIO_STATUS_PAUSED;
	radio_active = radio_playing = radio_connecting = false;
	snprintf(loaded_file, sizeof(loaded_file), "%s", "/card/old.flac");
	wakes = toggles = pauses = index_opens = index_closes = index_plays = refreshes = 0;
	error_count = 0;
	fade_active = false;
	can_fade = true;
	volume = 80;
	fade_begins = fade_stops = progress_calls = 0;
	wall_now = due - 10;
	petalarm_runtime_init(false);
}

static void fire(uint32_t tick) {
	petalarm_runtime_tick(due - 1, tick - 1000);
	petalarm_runtime_tick(due, tick);
}

static void test_resume_and_fade(void) {
	fresh("", 30);
	fire(5000);
	assert(wakes == 1 && toggles == 1 && pauses == 0 && index_opens == 0);
	assert(fade_begins == 1 && fade_active && volume == 0);
	petalarm_runtime_tick(due + 1, 6000); // playback has started
	petalarm_runtime_tick(due + 16, 21000);
	assert(progress_calls == 1 && volume == 40);
	petalarm_runtime_tick(due + 31, 36000);
	assert(!fade_active && volume == 80 && fade_stops == 1);
	petalarm_runtime_tick(due + 32, 37000);
	assert(toggles == 1 && wakes == 1); // never repeats the same occurrence

	// A user adjustment wins and is not restored to the old target.
	fresh("", 30);
	fire(5000);
	petalarm_runtime_tick(due + 1, 6000);
	fade_active = false;
	volume = 17;
	petalarm_runtime_tick(due + 2, 7000);
	assert(volume == 17 && fade_stops == 0);

	// A stop, disabling the alarm, or shutdown restores a temporary ramp.
	fresh("", 30);
	fire(5000);
	petalarm_runtime_tick(due + 1, 6000);
	audio_status = AUDIO_STATUS_PAUSED;
	petalarm_runtime_tick(due + 2, 7000);
	assert(!fade_active && volume == 80);

	fresh("", 30);
	fire(5000);
	petalarm_set_enabled(false);
	petalarm_runtime_tick(due + 1, 6000);
	assert(!fade_active && volume == 80);

	fresh("", 30);
	fire(5000);
	petalarm_set_fade_seconds(0);
	petalarm_runtime_tick(due + 1, 6000);
	assert(!fade_active && volume == 80);

	fresh("", 30);
	fire(5000);
	petalarm_runtime_shutdown();
	assert(!fade_active && volume == 80);
}

static void test_asynchronous_playback(void) {
	fresh("Morning", 10);
	// PLAYING is the request, not proof that the decoder and PCM are ready.
	pcm_open = false;
	quiet_ms = -1;
	fire(5000);
	assert(audio_status == AUDIO_STATUS_PLAYING && fade_active && volume == 0);
	assert(persistence_held && persistence_begins == 1 && !persistence_commits);
	petalarm_runtime_tick(due + 12, 17000);
	petalarm_runtime_tick(due + 35, 40000);
	assert(fade_active && volume == 0 && progress_calls == 0);
	pcm_open = true;
	petalarm_runtime_tick(due + 36, 41000); // still opening the new track
	assert(progress_calls == 0);
	quiet_ms = 0;
	petalarm_runtime_tick(due + 37, 42000); // start the ramp now
	assert(!persistence_held && persistence_commits == 1 && !persistence_cancels);
	petalarm_runtime_tick(due + 42, 47000);
	assert(fade_active && volume == 40);
	petalarm_runtime_tick(due + 47, 52000);
	assert(!fade_active && volume == 80);

	// A readable but corrupt file fails asynchronously, after the new queue
	// has been installed. Restore the old queue and resume it at its position.
	fresh("Morning", 10);
	pcm_open = false;
	quiet_ms = -1;
	fire(5000);
	audio_status = AUDIO_STATUS_STOPPED;
	quiet_ms = 0;
	petalarm_runtime_tick(due + 1, 6000);
	assert(checkpoint_restores == 1 && preserving_plays == 1 && !folder_plays);
	assert(!persistence_held && persistence_cancels == 1 && !persistence_commits);
	assert(error_count == 1 && !strcmp(errors[0], "pet_alarm_playlist_unavailable"));
	assert(fade_active && volume == 0);
	pcm_open = true;
	petalarm_runtime_tick(due + 2, 7000);
	petalarm_runtime_tick(due + 12, 17000);
	assert(!fade_active && volume == 80);

	fresh("Morning", 10);
	pcm_open = false;
	quiet_ms = -1;
	fire(5000);
	audio_status = AUDIO_STATUS_PAUSED; // user pauses before the PCM opens
	petalarm_runtime_tick(due + 1, 6000);
	assert(!fade_active && volume == 80 && !checkpoint_restores && checkpoint_discards == 1);
}

static void test_existing_and_missing_sources(void) {
	fresh("", 30);
	audio_status = AUDIO_STATUS_PLAYING;
	fire(5000);
	assert(wakes == 1 && !toggles && !fade_begins);

	fresh("", 30);
	radio_active = radio_connecting = true;
	fire(5000);
	assert(wakes == 1 && !toggles && !fade_begins);

	fresh("", 30);
	external_active = true;
	fire(5000);
	assert(wakes == 1 && !toggles && !fade_begins);

	fresh("", 30);
	loaded_file[0] = '\0';
	fire(5000);
	assert(wakes == 1 && !toggles && !fade_begins);
	assert(error_count == 1 && !strcmp(errors[0], "pet_alarm_no_source"));

	// Stopped radio is reconnected, but the fade waits for audio. If it never
	// connects, the output cannot remain at zero indefinitely.
	fresh("", 30);
	radio_active = true;
	fire(5000);
	assert(toggles == 1 && radio_connecting && fade_active && volume == 0);
	petalarm_runtime_tick(due + 59, 64000);
	assert(fade_active && volume == 0);
	petalarm_runtime_tick(due + 60, 65000);
	assert(!fade_active && volume == 80);
}

static void test_playlist(void) {
	fresh("Morning", 10);
	audio_status = AUDIO_STATUS_PLAYING;
	fire(5000);
	assert(wakes == 1 && index_opens == 1 && index_plays == 1 && !index_closes);
	assert(pauses == 1 && refreshes == 1 && !toggles && fade_active && volume == 0);
	petalarm_runtime_tick(due + 1, 6000);
	petalarm_runtime_tick(due + 11, 16000);
	assert(!fade_active && volume == 80);

	// Missing/empty playlist or missing first track leaves the old queue alone
	// and resumes it instead. The index, if created, is released.
	for (int failure = 0; failure < 3; failure++) {
		fresh("Morning", 0);
		if (failure == 0) index_available = false;
		if (failure == 1) index_fixture.count = 0;
		if (failure == 2) first_readable = false;
		fire(5000);
		assert(index_opens == 1 && !index_plays && !pauses && toggles == 1);
		assert(index_closes == (failure == 0 ? 0 : 1));
		assert(error_count == 1 && !strcmp(errors[0], "pet_alarm_playlist_unavailable"));
	}

	// Even a queue-load failure after clearing takes ownership and restores
	// the outgoing queue, not only its visible track.
	fresh("Morning", 10);
	play_index_ok = false;
	fire(5000);
	assert(index_plays == 1 && pauses == 1 && preserving_plays == 1 && checkpoint_restores == 1);
	assert(error_count == 1 && !strcmp(errors[0], "pet_alarm_playlist_unavailable"));
	assert(fade_stops == 1 && fade_begins == 2 && fade_active);

	// If the backup itself cannot be allocated, the old queue is never touched.
	fresh("Morning", 0);
	checkpoint_available = false;
	fire(5000);
	assert(!index_plays && !pauses && index_closes == 1 && toggles == 1);
	assert(error_count == 1 && !strcmp(errors[0], "pet_alarm_playlist_unavailable"));

	// An unrelated external source owns its PCM; do not close it or mix the
	// chosen playlist on top of it.
	fresh("Morning", 10);
	external_active = true;
	fire(5000);
	assert(wakes == 1 && !index_opens && !pauses && !toggles && !fade_begins);
	assert(error_count == 1 && !strcmp(errors[0], "pet_alarm_source_busy"));
}

static void test_failed_start_and_user_choices(void) {
	// Failure handling applies even when no fade was selected.
	fresh("Morning", 0);
	pcm_open = false;
	quiet_ms = -1;
	fire(5000);
	audio_status = AUDIO_STATUS_STOPPED;
	quiet_ms = 0;
	petalarm_runtime_tick(due + 1, 6000);
	assert(checkpoint_restores == 1 && preserving_plays == 1 && !fade_begins);

	// A previous station can be reconnected, not replaced by the old track
	// that happened to be underneath radio playback.
	fresh("Morning", 0);
	radio_active = radio_playing = true;
	pcm_open = false;
	quiet_ms = -1;
	fire(5000);
	audio_status = AUDIO_STATUS_STOPPED;
	quiet_ms = 0;
	petalarm_runtime_tick(due + 1, 6000);
	assert(checkpoint_restores == 1 && radio_plays == 1 && !preserving_plays && radio_connecting);
	assert(persistence_begins == 1 && persistence_cancels == 1 && !persistence_commits);

	// Do not restore row IDs invalidated by a card rescan: play the old file
	// with a fresh folder queue instead.
	fresh("Morning", 0);
	pcm_open = false;
	quiet_ms = -1;
	fire(5000);
	checkpoint_stale = true;
	audio_status = AUDIO_STATUS_STOPPED;
	quiet_ms = 0;
	petalarm_runtime_tick(due + 1, 6000);
	assert(checkpoint_restores == 1 && !preserving_plays && folder_plays == 1);
	assert(persistence_commits == 1 && !persistence_cancels);

	// With a stale queue under radio there is no file from which to rebuild;
	// clear the failed alarm queue and leave the old durable state alone.
	fresh("Morning", 0);
	radio_active = radio_playing = true;
	pcm_open = false;
	quiet_ms = -1;
	fire(5000);
	checkpoint_stale = true;
	audio_status = AUDIO_STATUS_STOPPED;
	quiet_ms = 0;
	petalarm_runtime_tick(due + 1, 6000);
	assert(checkpoint_restores == 1 && queue_clears == 1 && radio_plays == 1);
	assert(persistence_cancels == 1 && !persistence_commits);

	// A hanging request is bounded, but a user stop, queue edit, source
	// change or disable takes precedence over automatic recovery.
	fresh("Morning", 10);
	pcm_open = false;
	quiet_ms = -1;
	fire(5000);
	petalarm_runtime_tick(due + 60, 65000);
	assert(checkpoint_restores == 1 && preserving_plays == 1 && fade_active);

	for (int choice = 0; choice < 6; choice++) {
		fresh("Morning", 10);
		pcm_open = false;
		quiet_ms = -1;
		fire(5000);
		if (choice == 0) { audio_status = AUDIO_STATUS_STOPPED; stops_serial++; }
		if (choice == 1) queue_revision++;
		if (choice == 2) queue_index++;
		if (choice == 3) snprintf(loaded_file, sizeof(loaded_file), "%s", "/card/other.flac");
		if (choice == 4) petalarm_set_enabled(false);
		if (choice == 5) external_active = true;
		petalarm_runtime_tick(due + 1, 6000);
		assert(!checkpoint_restores && !preserving_plays && checkpoint_discards == 1);
		assert(!persistence_held && persistence_commits == 1 && !persistence_cancels);
	}

	// Shutdown before the next tick retains a pending attempt's previous disk
	// state, but commits one that opened or was explicitly changed.
	for (int outcome = 0; outcome < 3; outcome++) {
		fresh("Morning", 0);
		pcm_open = false;
		quiet_ms = -1;
		fire(5000);
		if (outcome == 1) { pcm_open = true; quiet_ms = 0; }
		if (outcome == 2) { audio_status = AUDIO_STATUS_STOPPED; stops_serial++; }
		petalarm_runtime_shutdown();
		assert(!persistence_held && checkpoint_discards == 1);
		assert(persistence_commits == (outcome != 0));
		assert(persistence_cancels == (outcome == 0));
	}

	// A genuine manual volume change while waiting must not be overwritten by
	// a later fallback or re-arm the fade.
	fresh("Morning", 10);
	pcm_open = false;
	quiet_ms = -1;
	fire(5000);
	fade_active = false;
	volume = 17;
	audio_status = AUDIO_STATUS_STOPPED;
	quiet_ms = 0;
	petalarm_runtime_tick(due + 1, 6000);
	assert(preserving_plays == 1 && !fade_active && volume == 17);

	// A short track which really played and finished before the first tick is
	// not an open failure.
	fresh("Morning", 0);
	fire(5000);
	audio_status = AUDIO_STATUS_STOPPED;
	progress_seconds = 0.5;
	petalarm_runtime_tick(due + 1, 6000);
	assert(!checkpoint_restores && checkpoint_discards == 1 && !error_count);
}

static void test_delayed_audio_teardown(void) {
	// A timeout is not proof the decoder/PCM thread has gone away. Keep the
	// restored queue durable and wait without starting a second source over it.
	fresh("Morning", 10);
	pcm_open = false;
	quiet_ms = -1;
	playback_context_active = true;
	fire(5000);
	petalarm_runtime_tick(due + 60, 65000);
	assert(attempt.recovering && checkpoint_restores == 1 && !preserving_plays);
	assert(persistence_held && fade_active && volume == 0);
	assert(!strcmp(loaded_file, "/card/old.flac") && audio_status == AUDIO_STATUS_STOPPED);
	// The stale open may briefly announce a PCM or overwrite progress after
	// cancellation. It is not readiness for the new fade or the old track.
	audio_status = AUDIO_STATUS_PLAYING;
	progress_seconds = 99;
	pcm_open = true;
	quiet_ms = 0;
	petalarm_runtime_tick(due + 61, 66000);
	assert(attempt.recovering && !preserving_plays && !progress_calls && volume == 0);
	playback_context_active = false;
	pcm_open = false;
	quiet_ms = -1;
	petalarm_runtime_tick(due + 62, 67000);
	assert(!attempt.recovering && preserving_plays == 1 && progress_seconds == 17);
	assert(!persistence_held && persistence_cancels == 1 && fade_active);

	// Radio must not connect on top of that old context either.
	fresh("Morning", 10);
	radio_active = radio_playing = true;
	playback_context_active = true;
	pcm_open = false;
	quiet_ms = -1;
	fire(5000);
	petalarm_runtime_tick(due + 60, 65000);
	assert(attempt.recovering && !radio_plays && !radio_active && persistence_held);
	playback_context_active = false;
	petalarm_runtime_tick(due + 61, 66000);
	assert(!attempt.recovering && radio_plays == 1 && radio_connecting && !persistence_held);

	// With no previous source, a late progress update must not turn the failed
	// alarm into the remembered track after the context finishes.
	fresh("Morning", 10);
	loaded_file[0] = '\0';
	progress_seconds = 0;
	playback_context_active = true;
	pcm_open = false;
	quiet_ms = -1;
	fire(5000);
	petalarm_runtime_tick(due + 60, 65000);
	assert(attempt.recovering && !loaded_file[0] && !fade_active && persistence_held);
	audio_status = AUDIO_STATUS_PLAYING;
	progress_seconds = 99;
	playback_context_active = false;
	petalarm_runtime_tick(due + 61, 66000);
	assert(!attempt.recovering && !loaded_file[0] && audio_status == AUDIO_STATUS_STOPPED);
	assert(progress_seconds == 0 && !persistence_held && persistence_cancels == 1);

	// A later command (including a play/seek of the same file), queue edit,
	// source takeover or disabling the alarm must win over delayed fallback.
	for (int choice = 0; choice < 4; choice++) {
		fresh("Morning", 10);
		playback_context_active = true;
		pcm_open = false;
		quiet_ms = -1;
		fire(5000);
		petalarm_runtime_tick(due + 60, 65000);
		assert(attempt.recovering && persistence_held);
		if (choice == 0) stops_serial++; // another transport command, same file
		if (choice == 1) queue_revision++;
		if (choice == 2) external_active = true;
		if (choice == 3) petalarm_set_enabled(false);
		petalarm_runtime_tick(due + 61, 66000);
		assert(!attempt.recovering && !preserving_plays && !radio_plays);
		assert(!persistence_held && persistence_commits == 1 && !fade_active);
	}

	// Do not hold a temporary zero forever if the old decoder never returns.
	fresh("Morning", 10);
	playback_context_active = true;
	pcm_open = false;
	quiet_ms = -1;
	fire(5000);
	petalarm_runtime_tick(due + 60, 65000);
	petalarm_runtime_tick(due + 120, 125000);
	assert(attempt.recovering && !fade_active && volume == 80);
	playback_context_active = false;
	petalarm_runtime_tick(due + 121, 126000);
	assert(preserving_plays == 1 && !fade_active);

	// Shutdown while a cancelled context is still winding down retains the
	// last confirmed disk state, even if that context reports PLAYING again.
	fresh("Morning", 10);
	playback_context_active = true;
	pcm_open = false;
	quiet_ms = -1;
	fire(5000);
	petalarm_runtime_tick(due + 60, 65000);
	audio_status = AUDIO_STATUS_PLAYING;
	pcm_open = true;
	quiet_ms = 0;
	petalarm_runtime_shutdown();
	assert(!persistence_held && persistence_cancels == 1 && !persistence_commits);
}

static void test_sleep_timer_interaction(void) {
	fresh("", 0);
	sleeptimer_set_minutes(SLEEPTIMER_MUSIC, 1);
	sleeptimer_set_enabled(SLEEPTIMER_MUSIC, true);
	fire(5000); // resuming from pause starts a new sleep-timer stretch
	assert(sleeptimer_running(SLEEPTIMER_MUSIC));
	sleep_now_ms = 59000;
	assert(!sleeptimer_expired(SLEEPTIMER_MUSIC));
	sleep_now_ms = 60000;
	assert(sleeptimer_expired(SLEEPTIMER_MUSIC));
	audio_status = AUDIO_STATUS_PAUSED; // the player's timer pauses playback
	sleeptimer_note_playing(SLEEPTIMER_COUNT);
	petalarm_runtime_tick(due + 60, 65000);
	assert(toggles == 1); // the alarm does not defeat the timer or re-trigger

	fresh("", 0);
	sleeptimer_set_minutes(SLEEPTIMER_MUSIC, 1);
	sleeptimer_set_enabled(SLEEPTIMER_MUSIC, true);
	audio_status = AUDIO_STATUS_PLAYING;
	sleeptimer_note_playing(SLEEPTIMER_MUSIC);
	sleep_now_ms = 59000;
	fire(5000); // already playing: leave both transport and timer alone
	assert(!toggles && !sleeptimer_expired(SLEEPTIMER_MUSIC));
	sleep_now_ms = 60000;
	assert(sleeptimer_expired(SLEEPTIMER_MUSIC));

	fresh("Morning", 0);
	sleeptimer_set_minutes(SLEEPTIMER_MUSIC, 1);
	sleeptimer_set_enabled(SLEEPTIMER_MUSIC, true);
	audio_status = AUDIO_STATUS_PLAYING;
	sleeptimer_note_playing(SLEEPTIMER_MUSIC);
	sleep_now_ms = 59000;
	fire(5000); // switching to the chosen playlist starts a fresh stretch
	assert(pauses == 1 && index_plays == 1 && sleeptimer_running(SLEEPTIMER_MUSIC));
	sleep_now_ms = 60000;
	assert(!sleeptimer_expired(SLEEPTIMER_MUSIC));
	sleep_now_ms = 119000;
	assert(sleeptimer_expired(SLEEPTIMER_MUSIC));
}

static void test_startup_restore_order(void) {
	fresh("", 0);
	loaded_file[0] = '\0';
	wall_now = due - 10;
	petalarm_runtime_init(true);
	petalarm_runtime_tick(due + 70, 70000);
	assert(!wakes && !toggles); // the remembered queue is not ready yet
	snprintf(loaded_file, sizeof(loaded_file), "%s", "/card/restored.flac");
	petalarm_runtime_restore_complete();
	petalarm_runtime_tick(due + 71, 71000);
	assert(wakes == 1 && toggles == 1 && audio_status == AUDIO_STATUS_PLAYING);
	petalarm_runtime_tick(due + 72, 72000);
	assert(toggles == 1);
}

int main(void) {
	assert(setenv("TZ", "UTC0", 1) == 0);
	tzset();
	int fd = mkstemp(test_config_path);
	assert(fd >= 0);
	close(fd);
	test_resume_and_fade();
	test_asynchronous_playback();
	test_existing_and_missing_sources();
	test_playlist();
	test_failed_start_and_user_choices();
	test_delayed_audio_teardown();
	test_sleep_timer_interaction();
	test_startup_restore_order();
	petalarm_runtime_shutdown();
	unlink(test_config_path);
	puts("pet alarm playback and startup: all checks passed");
	return 0;
}
