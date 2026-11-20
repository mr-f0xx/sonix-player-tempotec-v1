#include "listeningstats.h"

#include <stdio.h>
#include <string.h>

// Written to the card once a minute while something plays. The seconds gather
// in RAM between writes, so a track played straight through costs one UPDATE
// of one row per minute -- and a player left paused costs nothing at all. A
// crash or a battery pull loses at most this many seconds.
#define STATS_FLUSH_SECONDS 60

// Time is counted from how far the position actually moved, not from how
// often the poll ran: the player polls twice a second while something plays
// and once every two seconds when it does not, and a counter that added one
// per call would count a listening minute twice over on a playing track and
// half the time on a paused one. A track standing still (a pause the poll has
// not reported yet) adds nothing: there was no movement to add. A jump -- a
// seek, or the moment a new file is loaded -- is not listening either, which
// is what the upper bound rules out; below it sit only the step widths of the
// polls themselves.
#define STATS_SECONDS_EPSILON 0.05
#define STATS_SECONDS_JUMP 5.0

// One track with plays banked since the last flush. Plays are rare -- one
// entry per track played inside the minute -- so a small table is plenty.
typedef struct {
	char path[512];
	int plays;
} pending_play_t;

#define PENDING_PLAYS 12

static struct {
	long seconds; // whole seconds of playback seen since the last flush
	double carried; // the fraction of a second not yet worth one, kept across calls
	int plays;
	pending_play_t play[PENDING_PLAYS];

	// Where the player had got to, for the "how far did it move" reading.
	// Kept across flushes and track changes: a track resumed at the position
	// it was left at has not moved, and counting that as listening would
	// count a second for every resume.
	double last_position;
	bool have_last;
	char last_file[512];

	// The minute, on the interface's own clock (lv_tick_get(), handed in with
	// every call): the module never reads the wall clock itself, so the tests
	// drive it and the player never has to explain what 'now' means here.
	uint32_t mark_ms;
	bool have_mark;
} pending;

void stats_reset_pending(void) { memset(&pending, 0, sizeof(pending)); }

// ---------------------------------------------------------------------------
// Counting
// ---------------------------------------------------------------------------

void stats_note_playback(const char *file, double position, double duration, uint32_t now_ms) {
	(void)duration; // a second of the track is a second however long the track is

	bool same = pending.have_last && file && strcmp(file, pending.last_file) == 0;

	// Forwards, and by less than a jump. A position that went backwards is a
	// seek or the track starting again, a jump is a seek forwards, and
	// neither is time spent listening -- but the fraction gathered up to
	// either of them was listening and stays where it is, so a scrub in the
	// middle of a minute does not throw the minute away.
	double moved = same ? position - pending.last_position : 0;
	if (moved > STATS_SECONDS_EPSILON && moved < STATS_SECONDS_JUMP) {
		pending.carried += moved;
		while (pending.carried >= 1.0 && pending.seconds < 1000000000L) {
			pending.seconds++;
			pending.carried -= 1.0;
		}
	}
	// The fraction left over carries into the next minute, and into the next
	// track: half a second on the end of one and half a second at the start
	// of the other is still a second of listening.

	pending.have_last = file && file[0];
	if (pending.have_last) {
		snprintf(pending.last_file, sizeof(pending.last_file), "%.511s", file);
	} else {
		pending.last_file[0] = '\0';
	}
	pending.last_position = position;

	// The minute that writes it all down. Measured from the first second
	// counted, so a player that never plays never opens the database.
	if (pending.seconds > 0 || pending.plays > 0) {
		if (!pending.have_mark) {
			pending.mark_ms = now_ms;
			pending.have_mark = true;
		} else if ((uint32_t)(now_ms - pending.mark_ms) >= STATS_FLUSH_SECONDS * 1000u) {
			stats_flush();
		}
	} else {
		pending.have_mark = false;
	}
}

void stats_note_idle(void) {
	pending.have_last = false;
	pending.last_file[0] = '\0';
	pending.last_position = 0;
}

void stats_note_play(const char *file) {
	if (!file || !file[0]) {
		return;
	}
	// The same track played twice in a minute is two plays, banked together.
	for (int i = 0; i < pending.plays; i++) {
		if (strcmp(pending.play[i].path, file) == 0) {
			pending.play[i].plays++;
			return;
		}
	}
	if (pending.plays >= PENDING_PLAYS) {
		return; // a minute that played thirteen different tracks counted each once already
	}
	snprintf(pending.play[pending.plays].path, sizeof(pending.play[pending.plays].path), "%.511s", file);
	pending.play[pending.plays].plays = 1;
	pending.plays++;
}

// ---------------------------------------------------------------------------
// Writing it down
// ---------------------------------------------------------------------------

void stats_flush(void) {
	if (pending.seconds <= 0 && pending.plays == 0) {
		pending.have_mark = false;
		return;
	}

	// Taken before the write, and put back if the card refused it: a play is
	// not counted twice because the card was busy, and not lost because the
	// row was already there.
	long seconds = pending.seconds;
	library_stats_play_t plays[PENDING_PLAYS];
	int play_count = pending.plays;
	memcpy(plays, pending.play, sizeof(plays));
	pending.seconds = 0;
	memset(pending.play, 0, sizeof(pending.play));
	pending.plays = 0;
	pending.have_mark = false;

	int day = library_stats_today();
	if (!library_stats_add(day, seconds, plays, play_count)) {
		fprintf(stderr, "stats: a minute of listening could not be written\n");
		pending.seconds += seconds;
		for (int i = 0; i < play_count; i++) {
			if (!plays[i].path[0] || plays[i].plays <= 0) {
				continue;
			}
			stats_note_play(plays[i].path);
			// stats_note_play() banked one of the plays just put back; the
			// rest go in with it.
			if (pending.plays > 0 && strcmp(pending.play[pending.plays - 1].path, plays[i].path) == 0) {
				pending.play[pending.plays - 1].plays += plays[i].plays - 1;
			}
		}
	}
}

void listeningstats_init(void) { stats_reset_pending(); }

// ---------------------------------------------------------------------------
// Reading it back
// ---------------------------------------------------------------------------

void stats_daily(int days, stats_day_t *out, int count) {
	if (!out || count <= 0) {
		return;
	}
	if (days <= 0) {
		days = count;
	}
	int today = library_stats_today();
	int first = today - (days - 1);
	int filled = days < count ? days : count;

	// Read into a window of the database's own rows and then merged in by day,
	// field by field: the two structs carry the same two numbers and are not
	// the same layout (this one has the day number in front of them), so
	// handing the array over as the other type would write each value at the
	// wrong offset.
	static stats_day_row_t rows[STATS_CHART_MONTH_DAYS];
	if (filled > STATS_CHART_MONTH_DAYS) {
		filled = STATS_CHART_MONTH_DAYS;
	}
	for (int i = 0; i < filled; i++) {
		rows[i].seconds = 0;
		rows[i].tracks = 0;
	}
	library_stats_daily(first, filled, rows);

	for (int i = 0; i < count; i++) {
		out[i].day = first + i;
		out[i].seconds = i < filled ? rows[i].seconds : 0;
		out[i].tracks = i < filled ? rows[i].tracks : 0;
	}
}

void stats_summary(stats_range_t range, stats_summary_t *out) {
	if (!out) {
		return;
	}
	memset(out, 0, sizeof(*out));
	library_stats_summary((int)range, out);
}

int stats_top(stats_top_t kind, stats_range_t range, int offset, int count, stats_entry_t *out) {
	if (!out || count <= 0) {
		return 0;
	}
	return library_stats_top((int)kind, (int)range, offset, count, out);
}

int stats_top_count(stats_top_t kind, stats_range_t range) { return library_stats_top_count((int)kind, (int)range); }

// "0h 03m" / "12h 34m" / "0h 00m". Listening time keeps counting in hours past
// a day: the summary card is half the panel wide, and a figure in days ('3d
// 4h') would be the one number on the page nobody can compare at a glance.
void stats_format_duration(long seconds, char *out, size_t size) {
	if (!out || size == 0) {
		return;
	}
	if (seconds <= 0) {
		snprintf(out, size, "0h 00m");
		return;
	}
	long minutes = (seconds + 30) / 60;
	if (minutes < 1) {
		snprintf(out, size, "0h 01m");
	} else if (minutes < 60) {
		snprintf(out, size, "0h %02ldm", minutes);
	} else {
		snprintf(out, size, "%ldh %02ldm", minutes / 60, minutes % 60);
	}
}
