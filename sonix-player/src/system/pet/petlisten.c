#include "petlisten.h"

#include <string.h>
#include <time.h>

// The listening rules. See petlisten.h for what they are and why they are not
// inside the watcher.

static void copy_key(char *out, const char *text) {
	if (!out) {
		return;
	}
	if (!text) {
		text = "";
	}
	size_t length = strlen(text);
	if (length >= PETLISTEN_KEY) {
		length = PETLISTEN_KEY - 1;
	}
	memcpy(out, text, length);
	out[length] = '\0';
}

// The key a track is recognised by: its file, or a sentinel that cannot be a
// path for a live stream. A station that changes what it is playing is still
// the same session of listening, so nothing about an album run applies to it.
static const char *track_key(const petlisten_frame_t *frame) {
	if (frame->file && frame->file[0]) {
		return frame->file;
	}
	return "\x01live";
}

static bool at_night(int64_t now) {
	if (now <= 0) {
		return false;
	}
	time_t t = (time_t)now;
	struct tm local;
	if (!localtime_r(&t, &local)) {
		return false;
	}
	return local.tm_hour >= 0 && local.tm_hour < 5;
}

void petlisten_init(petlisten_t *listen, int64_t now) {
	if (!listen) {
		return;
	}
	memset(listen, 0, sizeof(*listen));
	listen->last_now = now;
}

// Whether the track that is ending was heard to its end. `listen->album` and
// `listen->track_heard` still describe that track when this is called.
static bool track_completed(const petlisten_t *listen) {
	if (!listen->track_heard || listen->last_duration < PETLISTEN_MIN_ALBUM_TRACK_SECS) {
		return false;
	}
	double needed = listen->last_duration * PETLISTEN_COMPLETE_PERCENT / 100.0;
	return listen->last_position >= needed;
}

// Settles the album run when the track that has just ended was a completion or
// a skip. Returns true when this completion is the one that finished a run.
static bool settle_album_run(petlisten_t *listen, bool completed) {
	if (!completed || !listen->album[0]) {
		listen->album_run_count = 0;
		listen->album_run[0] = '\0';
		return false;
	}

	if (strcmp(listen->album, listen->album_run) == 0) {
		listen->album_run_count++;
	} else {
		copy_key(listen->album_run, listen->album);
		listen->album_run_count = 1;
	}

	if (listen->album_run_count >= PET_DEEP_DIVER_TRACKS) {
		// One milestone per run: the fourth track does not award it again.
		listen->album_run_count = 0;
		listen->album_run[0] = '\0';
		return true;
	}
	return false;
}

void petlisten_frame(petlisten_t *listen, const petlisten_frame_t *frame, petlisten_result_t *out) {
	if (!out) {
		return;
	}
	memset(out, 0, sizeof(*out));
	if (!listen || !frame) {
		return;
	}

	// A callback that arrives late was not observed listening. A callback that
	// arrives early (the clock moved, or the timer is early) is worth the one
	// second it is pretending to be.
	int64_t gap = 1;
	if (frame->now > 0 && listen->last_now > 0 && frame->now > listen->last_now) {
		gap = frame->now - listen->last_now;
	}
	if (gap > PETLISTEN_MAX_CREDIT) {
		gap = PETLISTEN_MAX_CREDIT;
	}
	if (gap < 1) {
		gap = 1;
	}

	bool audible = frame->playing && pet_audible_volume(frame->volume_percent);
	out->listened_secs = audible ? (int)gap : 0;
	out->night = at_night(frame->now);

	// ---- the track ------------------------------------------------------
	const char *key = track_key(frame);
	bool changed = !listen->have_track || strcmp(key, listen->track) != 0;
	bool completed = false;

	if (changed) {
		out->track_changed = true;
		completed = track_completed(listen);
		out->album_complete = settle_album_run(listen, completed);

		copy_key(listen->track, key);
		copy_key(listen->album, frame->album);
		listen->track_secs_heard = 0;
		listen->track_heard = false;
		listen->have_track = true;

		// A skip breaks the uninterrupted session; a track that ended by
		// itself does not, because the listener is still there.
		if (!completed) {
			listen->session_secs = 0;
			listen->focus_reported = false;
		}
	}

	// ---- what was heard -------------------------------------------------
	if (out->listened_secs > 0) {
		if (!listen->track_heard) {
			listen->track_heard = true;
			out->first_audible = true;
		}
		listen->track_secs_heard += out->listened_secs;
		listen->session_secs += out->listened_secs;
		listen->silent_secs = 0;
		if (!listen->focus_reported && listen->session_secs >= PET_FOCUS_SECS) {
			listen->focus_reported = true;
			out->focus_30min = true;
		}
	} else if (frame->playing) {
		// Muted, or under the floor, while the file runs: the user cut the
		// sound, so the sitting is over now rather than a few seconds later.
		listen->session_secs = 0;
		listen->focus_reported = false;
		listen->silent_secs = 0;
	} else {
		// Paused or stopped. A second or two of this is what the gap between
		// two tracks looks like; a longer one is a pause.
		listen->silent_secs += (int)gap;
		if (listen->silent_secs >= PETLISTEN_SILENCE_GRACE) {
			listen->session_secs = 0;
			listen->focus_reported = false;
		}
	}

	// The position is remembered even while muted or paused: it is how the end
	// of the track is recognised when the next one starts. `track_heard` is
	// what keeps a muted track from counting as completed.
	if (frame->duration > 0) {
		listen->last_duration = frame->duration;
	}
	listen->last_position = frame->position > 0 ? frame->position : 0;
	listen->last_now = frame->now > 0 ? frame->now : listen->last_now;
}
