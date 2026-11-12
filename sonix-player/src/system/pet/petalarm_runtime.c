#include "petalarm_runtime.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "petalarm.h"
#include "src/gui/nowplaying/player.h"
#include "src/gui/shell/toast.h"
#include "src/system/audio/alsa-controls.h"
#include "src/system/audio/audio.h"
#include "src/system/device/clock.h"
#include "src/system/device/power.h"
#include "src/system/library/library.h"
#include "src/system/playback/device_state.h"
#include "src/system/playback/playlist.h"
#include "src/system/streaming/radio.h"

#define FADE_START_TIMEOUT_MS 60000U

static bool ready;
static bool waiting_for_restore;
static bool fade_waiting;
static bool fade_started;
static uint32_t fade_requested_ms;
static uint32_t fade_started_ms;
static int fade_duration;

// Keep the old queue only until the alarm track has really opened. A readable
// file can still fail in the decoder or while opening PCM; PLAYING alone merely
// means that a request was queued. The checkpoint moves the old queue's memory,
// so even a library-sized shuffle can be put back without copying its paths.
static struct {
	playlist_checkpoint_t *queue;
	bool recovering, queue_restored;
	char previous_file[512];
	double previous_position;
	bool previous_radio;
	radio_station_t station;
	char expected_file[512];
	unsigned revision, command_serial;
	int index;
	playback_mode_t mode;
	uint32_t requested_ms;
} attempt;

// A local play request reports PLAYING before the decoder or PCM has opened.
// Starting the ramp at that point can finish it in silence during a slow open.
// audio_quiet_ms() is negative until the new request/open has finished; checking
// the PCM as well distinguishes a failed open from a stream ready to play.
static bool playback_started(void) {
	if (radio_is_active()) return radio_is_playing() && !radio_is_connecting();
	return audio_get_status() == AUDIO_STATUS_PLAYING && audio_device_is_open() && audio_quiet_ms() >= 0;
}

static bool playback_continues(void) {
	if (radio_is_active()) return radio_is_playing();
	return audio_get_status() == AUDIO_STATUS_PLAYING;
}

static void fade_stop(void) {
	volume_fade_stop();
	fade_waiting = false;
	fade_started = false;
}

static void fade_begin(uint32_t now_ms) {
	fade_stop();
	fade_duration = petalarm_fade_seconds();
	if (fade_duration && volume_fade_begin()) {
		fade_waiting = true;
		fade_requested_ms = now_ms;
	}
}

static void fade_tick(uint32_t now_ms) {
	if (!fade_waiting && !fade_started) return;
	if (!petalarm_enabled() || petalarm_fade_seconds() != fade_duration || !volume_fade_active()) {
		fade_stop();
		return;
	}
	if (fade_waiting) {
		// The failed decoder may still briefly report an open PCM while it is
		// unwinding. Do not start the fallback ramp until that context is gone.
		if (attempt.recovering) {
			if ((uint32_t)(now_ms - fade_requested_ms) >= FADE_START_TIMEOUT_MS) fade_stop();
			return;
		}
		if (playback_started()) {
			fade_waiting = false;
			fade_started = true;
			fade_started_ms = now_ms;
		} else if ((!radio_is_active() && audio_get_status() == AUDIO_STATUS_PAUSED) ||
				   (radio_is_active() && !radio_is_playing()) ||
				   (uint32_t)(now_ms - fade_requested_ms) >= FADE_START_TIMEOUT_MS) {
			// A pause, failed stream or missing file must not leave output at 0.
			fade_stop();
		}
		return;
	}
	if (!playback_continues()) {
		// A pause, stop or output takeover during the ramp leaves the ordinary
		// volume ready for the next play, not a half-faded setting.
		fade_stop();
		return;
	}
	volume_fade_progress((int)((uint32_t)(now_ms - fade_started_ms) / 1000U), fade_duration);
	if (!volume_fade_active()) {
		fade_started = false;
	}
}

static bool first_track(const char *title, const char *path, const char *artist, void *user) {
	(void)title;
	(void)artist;
	char *out = user;
	if (snprintf(out, 512, "%s", path ? path : "") >= 512) out[0] = '\0';
	return true;
}

static bool playlist_ready(library_index_t *ix) {
	if (!ix || library_index_count(ix) <= 0) return false;
	// The index may still contain a removed file. Check only the first entry:
	// the queue starts there even in shuffle mode, and there is no need to read
	// a whole playlist (or replace the current queue) to discover it is gone.
	char path[512] = {0};
	return library_index_window(ix, 0, 1, first_track, path) == 1 &&
		   library_playback_track_readable(path);
}

static void attempt_forget(bool commit_current) {
	if (attempt.queue || attempt.recovering) {
		playlist_checkpoint_discard(attempt.queue);
		device_state_tentative_playback_end(commit_current);
	}
	memset(&attempt, 0, sizeof(attempt));
}

static void attempt_recovery_tick(void);

static void attempt_fallback(uint32_t now_ms) {
	// Cancel before lifting a temporary mute: a slow decoder might otherwise
	// start at the full target while the old queue is being restored.
	bool continue_fade = volume_fade_active();
	device_state_restore_stopped_file(attempt.previous_file, attempt.previous_position);
	radio_clear();
	fade_stop();
	attempt.queue_restored = playlist_checkpoint_restore(attempt.queue);
	attempt.queue = NULL;
	attempt.recovering = true;
	// A stale library handle cannot safely be reinstalled. Do not leave the
	// failed alarm queue visible while waiting to rebuild a folder or a radio.
	if (!attempt.queue_restored) playlist_clear();
	fprintf(stderr, "pet alarm: playlist did not start; trying the previous source\n");
	toast_error("pet_alarm_playlist_unavailable");

	// Even after the pending request is cancelled, the playback thread may be
	// inside a slow decoder or PCM open. Do not open another source until it
	// has left that context. The wait is tick-driven, not a UI-thread block.
	snprintf(attempt.expected_file, sizeof(attempt.expected_file), "%s", attempt.previous_file);
	attempt.revision = playlist_revision();
	attempt.index = playlist_current_index();
	attempt.mode = playlist_get_mode();
	attempt.command_serial = audio_command_serial();
	if (continue_fade && (attempt.previous_radio || attempt.previous_file[0])) fade_begin(now_ms);
	if (!attempt.previous_radio && !attempt.previous_file[0]) toast_error("pet_alarm_no_source");
	player_refresh_now_playing();
	attempt_recovery_tick();
}

// Do not undo a user's later choice, a queue edit, a sleep-timer pause, or
// an external source taking the output while the decoder was opening.
static bool attempt_intervened(audio_status_t status) {
	const char *file = NULL;
	device_state_loaded_metadata(&file);
	return !petalarm_enabled() || radio_is_active() || audio_external_is_active() ||
		   playlist_revision() != attempt.revision || playlist_current_index() != attempt.index ||
		   playlist_get_mode() != attempt.mode || !file || strcmp(file, attempt.expected_file) ||
		   audio_command_serial() != attempt.command_serial || status == AUDIO_STATUS_PAUSED;
}

static void attempt_recovery_tick(void) {
	if (attempt_intervened(audio_get_status())) {
		// A play, stop, seek, queue edit or another source wins even while the
		// old decoder is still unwinding. Never start fallback over that choice.
		fade_stop();
		attempt_forget(true);
		return;
	}
	if (audio_playback_context_active()) return;

	// A late return from the old decoder may have updated progress or status
	// after the first reset. Restore them once more, now that it is really idle.
	audio_restore_stopped_file(attempt.previous_file, attempt.previous_position);
	if (attempt.previous_radio) {
		if (!radio_play(&attempt.station)) {
			fade_stop();
			toast_error("pet_alarm_no_source");
		}
	} else if (attempt.previous_file[0]) {
		if (attempt.queue_restored) {
			device_state_play_preserving_queue_at(attempt.previous_file, attempt.previous_position);
		} else {
			// A rescan/card swap invalidated the saved library handle. The old
			// track is still worth trying, but the stale queue is not safe.
			device_state_play_file_at(attempt.previous_file, attempt.previous_position);
		}
	}
	player_refresh_now_playing();
	// The old durable queue and remembered track were never overwritten,
	// including when the restored RAM queue was empty or transient. Only a
	// freshly rebuilt folder after a stale checkpoint needs committing.
	device_state_tentative_playback_end(!attempt.queue_restored && !attempt.previous_radio && attempt.previous_file[0]);
	memset(&attempt, 0, sizeof(attempt));
}

static void attempt_tick(uint32_t now_ms) {
	if (attempt.recovering) {
		attempt_recovery_tick();
		return;
	}
	if (!attempt.queue) return;
	audio_status_t status = audio_get_status();
	if (attempt_intervened(status)) {
		attempt_forget(true);
		return;
	}
	if (playback_started()) {
		attempt_forget(true);
		return;
	}
	if (status == AUDIO_STATUS_STOPPED && audio_quiet_ms() >= 0) {
		double played = 0;
		audio_get_progress(&played, NULL);
		if (played > 0) {
			// A very short track can finish before the first watcher tick.
			attempt_forget(true);
		} else {
			attempt_fallback(now_ms);
		}
	} else if ((uint32_t)(now_ms - attempt.requested_ms) >= FADE_START_TIMEOUT_MS) {
		attempt_fallback(now_ms);
	}
}

static void start_alarm(uint32_t now_ms) {
	attempt_forget(false);
	const char *name = petalarm_playlist();

	// Waking the ordinary screen also brings back radios parked for standby.
	// It does not promise that an internet connection or BT headset has
	// reconnected by the time playback needs it.
	power_screen_on();

	// A radio can be stopped by the player's transport. Other external PCM
	// owners (receiver/DAC/game) have their own lifecycle and must not be
	// closed underneath their thread or mixed with a local track.
	if (name[0] && audio_external_is_active() && !radio_is_playing()) {
		fprintf(stderr, "pet alarm: another external audio source owns the output\n");
		toast_error("pet_alarm_source_busy");
		return;
	}

	library_index_t *ix = NULL;
	if (name[0]) {
		// The same compact, library-backed queue the playlist page uses: no
		// whole-playlist array of paths or extra copy of the track metadata.
		ix = library_index_open(LIBRARY_LIST_PLAYLIST, LIBRARY_FILTER_NONE, name, LIBRARY_ORDER_DEFAULT, false);
		if (!playlist_ready(ix)) {
			library_index_close(ix);
			ix = NULL;
			fprintf(stderr, "pet alarm: playlist '%s' unavailable or its first track is missing; trying the current source\n", name);
			toast_error("pet_alarm_playlist_unavailable");
		}
	}

	if (ix) {
		const char *file = NULL;
		device_state_loaded_metadata(&file);
		snprintf(attempt.previous_file, sizeof(attempt.previous_file), "%s", file ? file : "");
		audio_get_progress(&attempt.previous_position, NULL);
		attempt.previous_radio = radio_is_active() && radio_current_station(&attempt.station);
		attempt.queue = playlist_checkpoint_take();
		if (!attempt.queue) {
			// Do not risk destroying the current queue on an allocation failure.
			library_index_close(ix);
			memset(&attempt, 0, sizeof(attempt));
			fprintf(stderr, "pet alarm: could not retain the current queue\n");
			toast_error("pet_alarm_playlist_unavailable");
		} else {
			// Neither the queue, its album label nor the remembered track should
			// reach disk until this replacement has actually opened. The prior
			// durable queue may be unrelated to a transient/empty RAM queue.
			device_state_tentative_playback_begin();
			player_pause_for_loading();
			fade_begin(now_ms);
			// The new queue takes ownership even on failure.
			if (!device_state_play_index(ix, 0)) {
				attempt_fallback(now_ms);
				return;
			}
			player_refresh_now_playing();
			device_state_loaded_metadata(&file);
			snprintf(attempt.expected_file, sizeof(attempt.expected_file), "%s", file ? file : "");
			attempt.revision = playlist_revision();
			attempt.index = playlist_current_index();
			attempt.mode = playlist_get_mode();
			attempt.command_serial = audio_command_serial();
			attempt.requested_ms = now_ms;
			return;
		}
	}

	// Resume is deliberately not a blind toggle: if music (or a live external
	// source) is already playing, toggling it would silence the alarm instead.
	if (audio_get_status() == AUDIO_STATUS_PLAYING || radio_is_playing() || audio_external_is_active()) {
		return;
	}
	const char *file = NULL;
	device_state_loaded_metadata(&file);
	if (!radio_is_active() && (!file || !file[0])) {
		fprintf(stderr, "pet alarm: no current source to resume\n");
		toast_error("pet_alarm_no_source");
		return;
	}
	fade_begin(now_ms);
	player_key_play_pause();
}

void petalarm_runtime_init(bool restore_pending) {
	attempt_forget(false);
	petalarm_init();
	waiting_for_restore = restore_pending;
	ready = true;
	fade_waiting = fade_started = false;
	// Cache the deadline before the delayed restore: if the minute arrives
	// during that work, the first eligible tick still sees it as due.
	(void)petalarm_next(time(NULL), clock_is_set());
}

void petalarm_runtime_restore_complete(void) { waiting_for_restore = false; }

void petalarm_runtime_tick(time_t now, uint32_t monotonic_ms) {
	if (!ready || waiting_for_restore) return;
	attempt_tick(monotonic_ms);
	fade_tick(monotonic_ms);
	if (petalarm_take_due(now, clock_is_set())) {
		start_alarm(monotonic_ms);
	}
}

void petalarm_runtime_shutdown(void) {
	if (ready) fade_stop();
	if (attempt.queue || attempt.recovering) {
		audio_status_t status = audio_get_status();
		bool commit = attempt_intervened(status) || (!attempt.recovering && playback_started());
		if (!commit && !attempt.recovering && status == AUDIO_STATUS_STOPPED && audio_quiet_ms() >= 0) {
			double played = 0;
			audio_get_progress(&played, NULL);
			commit = played > 0;
		}
		// A shutdown during a genuinely pending or failed open preserves the
		// last confirmed state. A user change or successful short track is
		// still honoured even if it happened before the next watcher tick.
		attempt_forget(commit);
	}
}
