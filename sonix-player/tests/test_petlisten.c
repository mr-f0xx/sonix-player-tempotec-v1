// The listening rules, with no player behind them.
//
// petlisten.c is the part that decides what a second of the player's clock is
// worth: audible music, not a muted file, not a callback that slept through the
// track, not a fourth copy of the same record. This drives its frames by hand so
// each rule can be seen on its own.

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "src/system/pet/pet.h"
#include "src/system/pet/petlisten.h"

#define HOUR 3600

static const int64_t T0 = 1761868800; // 2025-10-31 00:00:00 UTC

static petlisten_t listener;
static int64_t now;

static void fresh(void) {
	now = T0;
	petlisten_init(&listener, now);
}

static petlisten_result_t tick(bool playing, int volume, const char *file, const char *album, double position,
			       double duration, int step) {
	now += step;
	petlisten_frame_t frame = {
		.playing = playing,
		.volume_percent = volume,
		.file = file,
		.album = album,
		.genre = "Rock",
		.position = position,
		.duration = duration,
		.now = now,
	};
	petlisten_result_t result;
	petlisten_frame(&listener, &frame, &result);
	return result;
}

static void test_audible_seconds(void) {
	fresh();

	petlisten_result_t r = tick(true, 50, "/music/a.flac", "Album", 1, 240, 1);
	assert(r.listened_secs == 1);
	assert(r.first_audible);
	assert(r.track_changed);

	// Muted is not listening, however loudly the file is running.
	r = tick(true, 0, "/music/a.flac", "Album", 2, 240, 1);
	assert(r.listened_secs == 0);
	assert(!r.first_audible);
	assert(!r.track_changed);

	// Neither is a volume under the floor.
	r = tick(true, PET_VOLUME_FLOOR - 1, "/music/a.flac", "Album", 3, 240, 1);
	assert(r.listened_secs == 0);

	// At the floor it counts again, and the track is not new a second time.
	r = tick(true, PET_VOLUME_FLOOR, "/music/a.flac", "Album", 4, 240, 1);
	assert(r.listened_secs == 1);
	assert(!r.first_audible);

	// Paused is not listening either.
	r = tick(false, 80, "/music/a.flac", "Album", 4, 240, 1);
	assert(r.listened_secs == 0);
}

static void test_a_late_callback_is_capped(void) {
	fresh();
	// The timer stood still through a minute of playback. Only the last few
	// seconds can be claimed: an app that was frozen was not listening.
	petlisten_result_t r = tick(true, 80, "/music/a.flac", "Album", 60, 240, 60);
	assert(r.listened_secs == PETLISTEN_MAX_CREDIT);

	// A callback that thinks no time passed still cannot go below one.
	r = tick(true, 80, "/music/a.flac", "Album", 60, 240, 0);
	assert(r.listened_secs == 1);
}

static void test_a_full_album_is_a_deep_dive(void) {
	fresh();

	// Three tracks, each heard to the end, of the same record.
	for (int i = 1; i <= 3; i++) {
		char file[32];
		snprintf(file, sizeof(file), "/music/%d.flac", i);
		petlisten_result_t r = tick(true, 80, file, "The Record", 0, 180, 1);
		assert(r.track_changed);
		assert(!r.album_complete); // one, two... not yet
		r = tick(true, 80, file, "The Record", 180, 180, 1);
		assert(!r.album_complete);
	}

	// The fourth track starting is the moment the third has ended. That is the
	// third completion in a row, so the run is complete.
	petlisten_result_t r = tick(true, 80, "/music/4.flac", "The Record", 0, 180, 1);
	assert(r.track_changed);
	assert(r.album_complete);

	// The fifth does not award it again in the same run.
	r = tick(true, 80, "/music/5.flac", "The Record", 0, 180, 1);
	assert(!r.album_complete);
}

static void test_a_skip_starts_the_run_over(void) {
	fresh();

	// Two complete tracks...
	for (int i = 1; i <= 2; i++) {
		char file[32];
		snprintf(file, sizeof(file), "/music/%d.flac", i);
		tick(true, 80, file, "The Record", 0, 180, 1);
		tick(true, 80, file, "The Record", 180, 180, 1);
	}

	// ... then the third is skipped after ten seconds.
	petlisten_result_t r = tick(true, 80, "/music/3.flac", "The Record", 0, 180, 1);
	tick(true, 80, "/music/3.flac", "The Record", 10, 180, 1);
	r = tick(true, 80, "/music/4.flac", "The Record", 0, 180, 1);
	assert(!r.album_complete);

	// A run that starts again needs three of its own: two more complete
	// tracks after the skip are still only two.
	r = tick(true, 80, "/music/4.flac", "The Record", 180, 180, 1);
	r = tick(true, 80, "/music/5.flac", "The Record", 0, 180, 1);
	assert(!r.album_complete);
	tick(true, 80, "/music/5.flac", "The Record", 180, 180, 1);
	r = tick(true, 80, "/music/6.flac", "The Record", 0, 180, 1);
	assert(!r.album_complete);
	tick(true, 80, "/music/6.flac", "The Record", 180, 180, 1);
	r = tick(true, 80, "/music/7.flac", "The Record", 0, 180, 1);
	assert(r.album_complete);
}

static void test_short_tracks_do_not_count(void) {
	fresh();
	for (int i = 1; i <= 3; i++) {
		char file[32];
		snprintf(file, sizeof(file), "/music/%d.flac", i);
		tick(true, 80, file, "The Record", 0, 20, 1);
		tick(true, 80, file, "The Record", 20, 20, 1);
	}
	// A jingle, a sting, a notification: an album of them is not a dive.
	petlisten_result_t r = tick(true, 80, "/music/4.flac", "The Record", 0, 20, 1);
	assert(!r.album_complete);
}

static void test_a_different_album_is_a_different_run(void) {
	fresh();
	for (int i = 1; i <= 2; i++) {
		char file[32];
		snprintf(file, sizeof(file), "/music/%d.flac", i);
		tick(true, 80, file, "First", 0, 180, 1);
		tick(true, 80, file, "First", 180, 180, 1);
	}
	// Two from the first record and one from another are not three in a row.
	petlisten_result_t r = tick(true, 80, "/music/x.flac", "Second", 0, 180, 1);
	assert(!r.album_complete);
}

static void test_focus_is_thirty_uninterrupted_minutes(void) {
	fresh();

	petlisten_result_t r;
	int focus_at = -1;
	for (int i = 0; i < PET_FOCUS_SECS + 10; i++) {
		r = tick(true, 80, "/music/a.flac", "Album", 0, 0, 1);
		if (r.focus_30min) {
			focus_at = i + 1;
			break;
		}
	}
	assert(focus_at == PET_FOCUS_SECS);

	// The flag is worth a single achievement-shaped moment.
	r = tick(true, 80, "/music/a.flac", "Album", 0, 0, 1);
	assert(!r.focus_30min);

	// A pause breaks the session, so the next thirty minutes are counted from
	// the new start rather than from the old one. A second or two of a
	// stopped player is forgiven -- that is what changing track looks like --
	// so the pause here is a real one.
	for (int i = 0; i < PETLISTEN_SILENCE_GRACE; i++) {
		r = tick(false, 80, "/music/a.flac", "Album", 0, 0, 1);
		assert(r.listened_secs == 0);
	}
	for (int i = 0; i < PET_FOCUS_SECS - 1; i++) {
		r = tick(true, 80, "/music/a.flac", "Album", 0, 0, 1);
		assert(!r.focus_30min);
	}
	r = tick(true, 80, "/music/a.flac", "Album", 0, 0, 1);
	assert(r.focus_30min);
}

static void test_muting_breaks_focus(void) {
	fresh();
	for (int i = 0; i < PET_FOCUS_SECS - 10; i++) {
		tick(true, 80, "/music/a.flac", "Album", 0, 0, 1);
	}
	tick(true, 0, "/music/a.flac", "Album", 0, 0, 1); // muted

	petlisten_result_t r;
	for (int i = 0; i < PET_FOCUS_SECS - 1; i++) {
		r = tick(true, 80, "/music/a.flac", "Album", 0, 0, 1);
		assert(!r.focus_30min);
	}
	r = tick(true, 80, "/music/a.flac", "Album", 0, 0, 1);
	assert(r.focus_30min);
}

static void test_a_completed_track_does_not_break_the_session(void) {
	fresh();

	// Half an hour of an album is nine or ten three-minute tracks. Each one
	// is heard to its end, which is not a skip: the sitting goes on.
	int session = 0;
	int focus_at = -1;
	for (int track = 1; track <= 20 && focus_at < 0; track++) {
		char file[32];
		snprintf(file, sizeof(file), "/music/%d.flac", track);

		if (track > 1) {
			// The previous track ended on its own; this one starts.
			petlisten_result_t r = tick(true, 80, file, "The Record", 0, 180, 1);
			session++;
			assert(r.track_changed);
			if (r.focus_30min) {
				focus_at = session;
			}
		}
		for (int second = 1; second <= 180 && focus_at < 0; second++) {
			petlisten_result_t r = tick(true, 80, file, "The Record", second, 180, 1);
			session++;
			if (r.focus_30min) {
				focus_at = session;
			}
		}
	}
	assert(focus_at == PET_FOCUS_SECS);
}

static void test_night(void) {
	fresh();
	now = T0 + 2 * HOUR; // 02:00 UTC
	petlisten_result_t r = tick(true, 80, "/music/a.flac", "Album", 0, 0, 1);
	assert(r.night);

	now = T0 + 13 * HOUR;
	r = tick(true, 80, "/music/a.flac", "Album", 0, 0, 1);
	assert(!r.night);

	now = T0 + 23 * HOUR;
	r = tick(true, 80, "/music/a.flac", "Album", 0, 0, 1);
	assert(!r.night);
}

static void test_a_live_stream_has_no_album(void) {
	fresh();
	for (int i = 0; i < 5; i++) {
		// Radio: no file, no album. Changing what the station plays is not a
		// change of track for the rules -- it is one long listen.
		petlisten_result_t r = tick(true, 80, "", "", 0, 0, 1);
		assert(!r.album_complete);
		assert(i > 0 || r.track_changed);
	}
}

static void test_an_empty_frame_earns_nothing(void) {
	fresh();
	petlisten_result_t result;
	petlisten_frame(NULL, NULL, &result);
	assert(result.listened_secs == 0);

	petlisten_frame(&listener, NULL, &result);
	assert(result.listened_secs == 0);
}

// A skip is a track that was heard and left before its end. The pet's patience
// is measured in these, so the rule has to be exact: a track that ends on its
// own is not one, a track nobody heard is not one, and a sting is not one.
static void test_a_skip_is_reported(void) {
	fresh();
	petlisten_result_t r = tick(true, 50, "/music/a.flac", "Album", 1, 240, 1);
	assert(r.track_changed);
	assert(!r.skipped); // nothing was heard before it

	tick(true, 50, "/music/a.flac", "Album", 30, 240, 1);
	r = tick(true, 50, "/music/b.flac", "Album", 1, 240, 1);
	assert(r.track_changed);
	assert(r.skipped);

	// A track heard to its end is a completion, not a skip.
	tick(true, 50, "/music/b.flac", "Album", 239, 240, 1);
	r = tick(true, 50, "/music/c.flac", "Album", 1, 240, 1);
	assert(r.track_changed);
	assert(!r.skipped);

	// A track that was only ever seen muted was never heard, so it is not a
	// skip either: the listener was not flicking through anything.
	fresh();
	tick(true, 0, "/music/a.flac", "Album", 20, 240, 1);
	r = tick(true, 50, "/music/b.flac", "Album", 1, 240, 1);
	assert(r.track_changed);
	assert(!r.skipped);

	// And a jingle is too short to have been abandoned.
	tick(true, 50, "/music/sting.mp3", "Jingles", 2, 12, 1);
	r = tick(true, 50, "/music/e.flac", "Album", 1, 240, 1);
	assert(r.track_changed);
	assert(!r.skipped);
}

// The session's length is what a Curious pet forages on, so it is reported as
// well as used: a skip resets it, a completion does not.
static void test_the_session_is_reported(void) {
	fresh();
	tick(true, 50, "/music/a.flac", "Album", 1, 240, 1);
	petlisten_result_t r = tick(true, 50, "/music/a.flac", "Album", 2, 240, 1);
	assert(r.session_secs == 2);

	tick(true, 50, "/music/a.flac", "Album", 30, 240, 1);
	r = tick(true, 50, "/music/b.flac", "Album", 1, 240, 1);
	assert(r.skipped);
	assert(r.session_secs == 1); // a skip breaks it: the second of the new track

	r = tick(true, 50, "/music/b.flac", "Album", 2, 240, 1);
	assert(r.session_secs == 2);
	tick(true, 50, "/music/b.flac", "Album", 239, 240, 1);
	r = tick(true, 50, "/music/c.flac", "Album", 1, 240, 1);
	assert(r.session_secs > 0); // a track ending on its own does not
}

int main(void) {
	setenv("TZ", "UTC", 1);
	tzset();

	test_audible_seconds();
	test_a_late_callback_is_capped();
	test_a_full_album_is_a_deep_dive();
	test_a_skip_starts_the_run_over();
	test_short_tracks_do_not_count();
	test_a_different_album_is_a_different_run();
	test_focus_is_thirty_uninterrupted_minutes();
	test_muting_breaks_focus();
	test_a_completed_track_does_not_break_the_session();
	test_night();
	test_a_live_stream_has_no_album();
	test_a_skip_is_reported();
	test_the_session_is_reported();
	test_an_empty_frame_earns_nothing();

	printf("PASS music pet: audible seconds, album runs, focus and night\n");
	return 0;
}
