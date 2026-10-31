#include "petwatch.h"

#include "petlisten.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "lvgl/lvgl.h"

#include "src/system/audio/audio.h"
#include "src/system/bluetooth/btreceiver.h"
#include "src/system/core/config.h"
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

// Timestamps from before this are from a clock that had never been set. If the
// setup happened while the player was off, the first boot afterwards sees a
// true clock beside those timestamps and has to forgive them just the same.
#define PET_CLOCK_FLOOR 1577836800LL // 2020-01-01 00:00:00 UTC

static pet_state_t state;
static lv_timer_t *tick_timer;
static petlisten_t listener;

static bool listening;
static bool focus_active;
static bool clock_was_set;
static char track_title[128];
static char current_album[256];
static char current_genre[128];
static int track_bonus;
static bool note_pending;
static int unsaved_listen_secs;
static int pending_events;
static int pending_achievement;
static int64_t night_secs;
static pet_vibe_t current_vibe = PET_VIBE_NEUTRAL;

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
static void take_track_title(bool radio_playing, bool phone_streaming) {
	track_title[0] = '\0';

	if (phone_streaming) {
		return; // the phone names its own music, not this player
	}

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

// The key an album is recognised by. The tag when there is one; otherwise the
// folder, which is what a record without tags looks like on a card. A live
// stream has neither, and so has no album runs.
static void album_key_for(const song_metadata_t *meta, const char *file, char *out, size_t out_size) {
	out[0] = '\0';
	if (meta && meta->album[0]) {
		copy_text(out, out_size, meta->album);
		return;
	}
	if (!file || !file[0]) {
		return;
	}
	const char *slash = strrchr(file, '/');
	if (!slash || slash == file) {
		return;
	}
	size_t length = (size_t)(slash - file);
	if (length >= out_size) {
		length = out_size - 1;
	}
	memcpy(out, file, length);
	out[length] = '\0';
}

static void save_now(void) {
	pet_save(&state);
	unsaved_listen_secs = 0;
}

static void award(int achievement) {
	if (pet_award_achievement(&state, achievement)) {
		pending_achievement = achievement;
		save_now();
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
	bool phone_streaming = btreceiver_is_streaming();
	bool live = radio_playing || phone_streaming;
	bool playing = playback_is_music();
	int volume = device_state_get_volume_percent();

	double position = 0;
	double duration = 0;
	audio_get_progress(&position, &duration);

	const char *file = NULL;
	const song_metadata_t *meta = device_state_loaded_metadata(&file);
	if (!file) {
		file = "";
	}

	take_track_title(radio_playing, phone_streaming);

	char album[256];
	album[0] = '\0';
	char genre[128];
	genre[0] = '\0';
	if (!live) {
		album_key_for(meta, file, album, sizeof(album));
		if (meta) {
			copy_text(genre, sizeof(genre), meta->genre);
		}
	}

	petlisten_frame_t frame = {
		.playing = playing,
		.volume_percent = volume,
		.file = live ? "" : file,
		.album = album,
		.genre = genre,
		.position = live ? 0 : position,
		.duration = live ? 0 : duration,
		.now = now,
	};
	petlisten_result_t result;
	petlisten_frame(&listener, &frame, &result);

	listening = result.listened_secs > 0;
	focus_active = listener.session_secs >= PET_FOCUS_SECS;

	// Too much skipping and the pet stops crediting. It is reported once, on
	// the skip that tips it over, and PET_EVENT_DIZZY carries it to the page.
	if (result.skipped && pet_skip(&state, now)) {
		pending_events |= PET_EVENT_DIZZY;
		save_now();
	}

	if (result.track_changed) {
		copy_text(current_album, sizeof(current_album), album);
		copy_text(current_genre, sizeof(current_genre), genre);
		current_vibe = pet_vibe_for_genre(current_genre);
		note_pending = true;
		track_bonus = 0;
	}

	if (!listening) {
		// The track was changed while paused or muted: it is not a discovery
		// and it earns nothing. It stays pending until a second is heard.
		night_secs = 0;
	}

	bool noted = false;
	if (listening && note_pending) {
		int track_events = PET_EVENT_NONE;
		track_bonus = pet_note_track(&state, current_genre, current_album, &track_events);
		note_pending = false;
		noted = true;
		if (track_events & PET_EVENT_POLYMATH) {
			pending_achievement = PET_ACH_POLYMATH;
		}
	}

	int events = pet_advance(&state, now, day, result.listened_secs, track_bonus);
	pending_events |= events & (PET_EVENT_GOAL_MET | PET_EVENT_STAGE_UP | PET_EVENT_DROP);

	// A long uninterrupted session turns something up for a Curious pet, once
	// a day.
	if (listening && pet_forage(&state, listener.session_secs) > 0) {
		pending_events |= PET_EVENT_FORAGE;
		save_now();
	}

	if (listening) {
		unsaved_listen_secs += result.listened_secs;

		if (result.night && !pet_has_achievement(&state, PET_ACH_NIGHT_OWL)) {
			night_secs += result.listened_secs;
			if (night_secs >= PET_NIGHT_OWL_SECS) {
				award(PET_ACH_NIGHT_OWL);
			}
		} else if (!result.night) {
			night_secs = 0;
		}
	}

	int cheered = 0;
	if (result.focus_30min) {
		award(PET_ACH_FOCUS);
		pet_cheer(&state, PET_FOCUS_HAPPY_BONUS);
		cheered = 1;
	}
	if (result.album_complete) {
		award(PET_ACH_DEEP_DIVER);
		pet_cheer(&state, PET_ALBUM_HAPPY_BONUS);
		cheered = 1;
	}
	if (cheered) {
		save_now();
	}

	if (unsaved_listen_secs >= PET_SAVE_LISTEN_SECS || (events & (PET_EVENT_GOAL_MET | PET_EVENT_STAGE_UP)) ||
		noted) {
		save_now();
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
	pet_advance(&state, now, day, 0, 0);

	petlisten_init(&listener, now);
	note_pending = false;
	unsaved_listen_secs = 0;

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

bool petwatch_focus(void) { return focus_active; }

pet_vibe_t petwatch_vibe(void) { return current_vibe; }

bool petwatch_pet_now(void) {
	int64_t now = (int64_t)time(NULL);
	if (!pet_pet(&state, now)) {
		return false;
	}
	save_now();
	return true;
}

pet_temperament_t petwatch_temperament(void) { return pet_temperament(&state); }

bool petwatch_has_soundscape(void) { return pet_has_soundscape(&state); }

bool petwatch_rhythm_ready(void) { return pet_rhythm_ready(&state); }

int petwatch_rhythm_finish(int score) {
	int notes = pet_rhythm_finish(&state, score);
	if (notes > 0) {
		save_now();
	}
	return notes;
}

bool petwatch_set_species(pet_species_t species) {
	if (!pet_set_species(&state, species)) {
		return false;
	}
	save_now();
	return true;
}

bool petwatch_set_name(const char *name) {
	pet_set_name(name);
	config_save(); // a name typed once is worth a write
	return true;
}

bool petwatch_buy(pet_item_t item) {
	if (!pet_buy(&state, item)) {
		return false;
	}
	save_now();
	return true;
}

bool petwatch_wear(pet_item_t item) {
	if (!pet_wear(&state, item)) {
		return false;
	}
	save_now();
	return true;
}

int petwatch_take_events(void) {
	int events = pending_events;
	pending_events = 0;
	return events;
}

int petwatch_take_achievement(void) {
	int achievement = pending_achievement;
	pending_achievement = 0;
	return achievement;
}

void petwatch_flush(void) {
	save_now();
}

void petwatch_track_title(char *out, size_t out_size) {
	if (!out || out_size == 0) {
		return;
	}
	copy_text(out, out_size, track_title);
}

int64_t petwatch_now(void) { return (int64_t)time(NULL); }
