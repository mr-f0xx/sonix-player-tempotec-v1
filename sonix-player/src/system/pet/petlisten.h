#ifndef SYSTEM_PET_PETLISTEN_H
#define SYSTEM_PET_PETLISTEN_H

#include <stdbool.h>
#include <stdint.h>

#include "pet.h"

// What one second of the player's state is worth to the pet.
//
// This turns the raw facts -- is something playing, how loud, which track, how
// far into it -- into the handful of things the pet cares about, and it is
// separate from petwatch.c so those rules can be tested without audio hardware:
//
//   * a paused player, or one at or below the volume floor, hears nothing;
//   * a timer callback that arrives late cannot claim more than a few seconds,
//     because nobody observed the music during the rest of the gap;
//   * a track that ends on its own, after being heard, completes; one that is
//     skipped does not;
//   * a pause, a mute or a skip ends the uninterrupted session, though a pause
//     of a second or two is forgiven because that is what changing track looks
//     like;
//   * three completed tracks of the same album in a row make an album run;
//   * thirty minutes of uninterrupted audible listening is focus;
//   * midnight to five in the morning is night.
//
// Everything here is arithmetic on the caller's frame. No LVGL, no audio, no
// clock of its own: the caller passes `now`, which makes the night test the
// caller's time zone and the suite's to choose.

#define PETLISTEN_MAX_CREDIT 5

// The share of a track that has to be heard for it to count as completed, in
// percent. The end of a track is often cut by the next one starting a little
// early, and a seek to the last few seconds should not be a "skip".
#define PETLISTEN_COMPLETE_PERCENT 90

// A track shorter than this is not part of an album run: a sting, a jingle, a
// notification the player happened to play. Three of those are not an album.
#define PETLISTEN_MIN_ALBUM_TRACK_SECS 30

// How long a stopped player may stay stopped without ending the session. It is
// the gap between a track ending and the next starting, which the engine may
// spend with the status at STOPPED; a listener pausing for longer has stopped
// listening. A mute is not forgiven, because the file is still running.
#define PETLISTEN_SILENCE_GRACE 5

#define PETLISTEN_KEY 512

typedef struct {
	bool playing;		  // the player is producing music (not paused/stopped)
	int volume_percent;	  // 0..100
	const char *file;	  // current file; "" for a live stream
	const char *album;	  // album (or folder) key; "" when unknown or live
	const char *genre;	  // the file's genre; "" when unknown
	double position;	  // seconds into the track, 0 when unknown/live
	double duration;	  // track length, 0 when unknown/live
	int64_t now;		  // epoch seconds as the caller reads them
} petlisten_frame_t;

typedef struct {
	int listened_secs;    // audible seconds this tick
	bool track_changed;   // a different track (or the first one) was seen
	bool first_audible;   // this track was just heard for the first time
	bool album_complete;  // the third track of an album run just finished
	bool focus_30min;     // thirty uninterrupted minutes just passed
	bool night;		  // this frame fell between midnight and 05:00
} petlisten_result_t;

typedef struct {
	int64_t last_now;
	char track[PETLISTEN_KEY];
	char album[PETLISTEN_KEY];
	char album_run[PETLISTEN_KEY];
	double last_position;
	double last_duration;
	int track_secs_heard;
	int session_secs;
	int album_run_count;
	int silent_secs;
	bool track_heard;
	bool have_track;
	bool focus_reported;
} petlisten_t;

// Starts from "nothing seen yet". `now` seeds the late-callback arithmetic; 0
// is fine for a clock that cannot be trusted.
void petlisten_init(petlisten_t *listen, int64_t now);

// Feeds one observation. `out` is always filled.
void petlisten_frame(petlisten_t *listen, const petlisten_frame_t *frame, petlisten_result_t *out);

#endif /* SYSTEM_PET_PETLISTEN_H */
