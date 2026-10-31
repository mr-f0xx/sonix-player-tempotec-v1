#include "petwatch.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "lvgl/lvgl.h"

#include "src/system/audio/audio.h"
#include "src/system/bluetooth/btreceiver.h"
#include "src/system/playback/device_state.h"
#include "src/system/device/clock.h"
#include "src/system/streaming/radio.h"

// One tick a second. The player's main loop is already awake four times a
// second for the status bar and the input devices, so this adds no wakeup of
// its own; it is one more callback in a loop that is running anyway.
#define PET_TICK_MS 1000

// Save after this much music, rather than after this many ticks: a paused
// player writes nothing. Two minutes is the most the pet can lose to a battery
// pulled out without a clean shutdown, and it is 720 small writes a day at the
// outside -- the settings file the player rewrites whenever anything changes
// anyway.
#define PET_SAVE_LISTEN_SECS 120

// A timer that arrives this late was not a second of listening: the interface
// thread was busy decoding, opening a file, or the device was suspended. The
// tick credits at most this much to the session and lets pet_advance() treat
// the rest as idle time.
#define PET_TICK_MAX_CREDIT 5

// Timestamps from before this are from a clock that had never been set. If the
// setup happened while the player was off, the first boot afterwards sees a
// true clock beside those timestamps and has to forgive them just the same.
#define PET_CLOCK_FLOOR 1577836800LL // 2020-01-01 00:00:00 UTC

static pet_state_t state;
static lv_timer_t *tick_timer;

static int64_t tick_now;
static bool listening;
static bool clock_was_set;
static char track_title[128];
static int unsaved_listen_secs;
static int pending_events;

static bool playback_is_music(void) {
	// Local files and streams: audio.c is the source of truth for those.
	if (audio_get_status() == AUDIO_STATUS_PLAYING) {
		return true;
	}
	// Internet radio decodes on its own thread and asks audio.c for the
	// output, so audio_get_status() cannot answer for it.
	if (radio_is_playing()) {
		return true;
	}
	// Bluetooth receiver mode: the music is coming from a phone, but it is
	// still music going into this player's ears.
	return btreceiver_is_streaming();
}

static void copy_text(char *out, size_t out_size, const char *text) {
	if (!out || out_size == 0) {
		return;
	}
	snprintf(out, out_size, "%.*s", (int)(out_size - 1), text ? text : "");
}

// The name the page's status line puts after "feeding on". A local track's
// title, a station's name, or nothing when the source is a phone (which does
// not tell this player what it is sending). Cheap enough for once a second:
// device_state_loaded_metadata() is documented as no copy and no sysfs.
static void take_track_title(bool radio_playing) {
	track_title[0] = '\0';

	if (radio_playing) {
		radio_now_t now;
		radio_get_now(&now);
		if (now.title[0]) {
			copy_text(track_title, sizeof(track_title), now.title);
		} else if (now.station[0]) {
			copy_text(track_title, sizeof(track_title), now.station);
		}
		return;
	}

	const char *file = NULL;
	const song_metadata_t *meta = device_state_loaded_metadata(&file);
	if (meta && meta->title[0]) {
		copy_text(track_title, sizeof(track_title), meta->title);
		return;
	}
	if (file && file[0]) {
		// No tags: the file's own name, which is what the player shows
		// everywhere else for a track without them.
		const char *slash = strrchr(file, '/');
		copy_text(track_title, sizeof(track_title), slash ? slash + 1 : file);
	}
}

// Move every timestamp the pet holds by the same amount: its age and its
// listening survive, and only the calendar under them changes.
static void shift_timestamps(int64_t now) {
	int64_t shift = now - state.last_seen;
	if (shift <= 0) {
		return;
	}
	state.adopted += shift;
	state.last_seen += shift;
	if (state.last_music > 0) {
		state.last_music += shift;
	} else {
		// A pet adopted at the epoch has never heard anything: move its last
		// song to the moment the clock became true, so it wakes up on the
		// first real second instead of looking six hours abandoned.
		state.last_music = now;
	}
	if (state.last_pet > 0) {
		state.last_pet += shift;
	}
	printf("petwatch: the clock is set; the pet's timestamps moved by %lld s\n", (long long)shift);
}

// A device whose clock has never been set starts at the epoch and has the real
// time put in a moment later by the setup screen. The pet adopted at the epoch
// would otherwise see fifty-six years pass in one second, come back sad and
// asleep, and be blamed on nothing the user did. When the clock becomes
// trustworthy for the first time, its timestamps are shifted rather than aged.
static void adopt_clock_if_it_was_set(int64_t now) {
	if (clock_was_set || !clock_is_set() || now <= 0) {
		return;
	}
	clock_was_set = true;

	if (state.adopted <= 0) {
		return;
	}
	shift_timestamps(now);
}

static void tick_cb(lv_timer_t *timer) {
	(void)timer;

	int64_t now = (int64_t)time(NULL);
	long day = pet_day_now(now);
	adopt_clock_if_it_was_set(now);

	bool radio_playing = radio_is_playing();
	bool playing = playback_is_music();
	listening = playing;
	take_track_title(radio_playing);

	int listened = 0;
	if (playing) {
		// The first tick has no predecessor, and a tick that arrives late
		// cannot be certain the music was playing for all of the gap.
		int64_t gap = (tick_now > 0 && now > tick_now) ? now - tick_now : 1;
		if (gap < 1) {
			gap = 1;
		}
		if (gap > PET_TICK_MAX_CREDIT) {
			gap = PET_TICK_MAX_CREDIT;
		}
		listened = (int)gap;
	}

	int events = pet_advance(&state, now, day, listened);
	pending_events |= events;

	tick_now = now;

	if (listened > 0) {
		unsaved_listen_secs += listened;
		if (unsaved_listen_secs >= PET_SAVE_LISTEN_SECS || (events & (PET_EVENT_GOAL_MET | PET_EVENT_STAGE_UP))) {
			pet_save(&state);
			unsaved_listen_secs = 0;
		}
	}
}

void petwatch_init(void) {
	int64_t now = (int64_t)time(NULL);
	long day = pet_day_now(now);

	clock_was_set = clock_is_set();
	pet_load(&state, now, day);

	// The clock was set while the player was off: it is true now, but the pet
	// was adopted before that and its timestamps are still from the epoch. A
	// state that knows where the clock stands needs no second chance.
	if (clock_was_set && now >= PET_CLOCK_FLOOR && state.adopted > 0 && state.last_seen > 0 &&
		state.last_seen < PET_CLOCK_FLOOR) {
		shift_timestamps(now);
	}

	// The offline time. A pet that was left with the player off for two days
	// comes back two days hungrier, which is exactly what the stored
	// last_seen is for; the first tick would do it too, but doing it here
	// means the page opens on the truth rather than one second behind it.
	pet_advance(&state, now, day, 0);

	tick_now = now;
	listening = playback_is_music();

	tick_timer = lv_timer_create(tick_cb, PET_TICK_MS, NULL);
	if (tick_timer) {
		// Deliberately NOT registered with power_pause_in_standby(): this is
		// the timer that has to keep counting when the panel is dark and the
		// music is still playing.
		lv_timer_set_repeat_count(tick_timer, -1);
	}

	pet_save(&state); // establishes the section on first run
}

const pet_state_t *petwatch_state(void) { return &state; }

bool petwatch_listening(void) { return listening; }

bool petwatch_pet_now(void) {
	int64_t now = (int64_t)time(NULL);
	if (!pet_pet(&state, now)) {
		return false;
	}
	pet_save(&state);
	unsaved_listen_secs = 0;
	return true;
}

int petwatch_take_events(void) {
	int events = pending_events;
	pending_events = 0;
	return events;
}

void petwatch_flush(void) {
	pet_save(&state);
	unsaved_listen_secs = 0;
}

void petwatch_track_title(char *out, size_t out_size) {
	if (!out || out_size == 0) {
		return;
	}
	copy_text(out, out_size, track_title);
}

int64_t petwatch_now(void) { return (int64_t)time(NULL); }
