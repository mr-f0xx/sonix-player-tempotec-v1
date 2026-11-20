#ifndef LISTENINGSTATS_H
#define LISTENINGSTATS_H

#include <stdbool.h>
#include <stdint.h>

// Listening statistics: how long the player has been playing, and which tracks,
// records and artists it has been playing -- per day, so the Music page's
// Stats view can answer for a week, a month, a year or since the card was
// first used.
//
// Three things make this cheap enough to keep on a player with ten megabytes
// free:
//
//   * what is counted is written to the card about once a minute, not once a
//     second. The seconds and the plays accumulate in RAM until the minute is
//     up, so a track played straight through costs one write per minute and a
//     player left paused costs nothing at all.
//   * a play is the same thing PLAY_COUNTS counts (library_record_play): a
//     track reaching a third of its length, ten seconds at the very least.
//     Both stores are written from the same call, so Most played and this
//     page can never disagree about how often something played.
//   * the whole history lives in the library's own database on the card, so
//     it survives reboots and rescans the way the play counts do, and nothing
//     here copies it into RAM to draw a page.
//
// The tables are LISTEN_DAY (one row per day: seconds and plays) and
// LISTEN_TRACK (one row per day and track, with its plays and the record,
// artist and genre it was filed under at the time). Everything the page shows
// -- the summary cards, the bar chart, the three top lists -- is a GROUP BY
// over those two.

#include "src/system/library/library.h"

// The ranges the Stats page offers, in the order its pills are laid out.
typedef enum {
	STATS_RANGE_WEEK = 0,
	STATS_RANGE_MONTH,
	STATS_RANGE_YEAR,
	STATS_RANGE_ALL,
	STATS_RANGE_COUNT,
} stats_range_t;

// One day of history, for the chart.
typedef struct {
	int day;      // days since 1970-01-01 (UTC)
	long seconds; // how long the player played that day
	int tracks;   // how many times a track was played that day
} stats_day_t;

// The chart's two windows: the last seven days (one bar each) and the last
// thirty (a bar each, fewer labels under them).
#define STATS_CHART_WEEK_DAYS 7
#define STATS_CHART_MONTH_DAYS 30

// One row of a top list: a track, a record or an artist. library.h's own row,
// so the page and the database speak the same struct.
typedef listening_stats_entry_t stats_entry_t;

// Everything the summary cards say about one range.
typedef listening_stats_summary_t stats_summary_t;

// The kinds of top list, matching the library's own numbering.
typedef enum {
	STATS_TOP_TRACKS = 0,
	STATS_TOP_ALBUMS,
	STATS_TOP_ARTISTS,
} stats_top_t;

// Fills `out` with `count` days ending today, oldest first. Days with nothing
// played come back as zeroes rather than missing, so the caller can draw a bar
// for every one of them. `days` is the width of the window the numbers cover:
// the chart's own week or month.
void stats_daily(int days, stats_day_t *out, int count);

// The summary cards for one range.
void stats_summary(stats_range_t range, stats_summary_t *out);

// The leaders of `kind` in `range`. Read from the top of the range's list, so
// `offset` and `count` page the whole thing (the Stats page's "See all").
// Returns how many were written, which is fewer than asked at the end of the
// list.
int stats_top(stats_top_t kind, stats_range_t range, int offset, int count, stats_entry_t *out);

// How many rows the "See all" page will hold.
int stats_top_count(stats_top_t kind, stats_range_t range);

// "0h 00m" up to a day and "12h 34m" after that, which is the shape the Stats
// page draws: minutes below an hour, never a bare number, the same everywhere
// a listening time is shown.
void stats_format_duration(long seconds, char *out, size_t size);

// ---------------------------------------------------------------------------
// Counting
//
// Driven by the now-playing screen's progress poll. Nothing here blocks: what
// it needs from the library is a handful of rows already in its page cache,
// and the writing out happens on the library's own connection when the minute
// is up.
// ---------------------------------------------------------------------------

// Where the player has got to. Called on every progress poll -- twice a second
// while something plays -- for local files only: audiobooks, podcasts and the
// streaming services' cache files are not the library's music.
//
// `position` and `duration` are in seconds, `now_ms` is the interface's own
// clock (lv_tick_get()), which is what decides when the minute is up. The
// module never reads a clock itself, so the tests drive it.
//
// Time is counted from how far the position moved since the last call and not
// from how many calls there were, so the poll's cadence does not change the
// answer. Standing still counts nothing; a jump (a seek, a new file) counts
// nothing either.
void stats_note_playback(const char *file, double position, double duration, uint32_t now_ms);

// Nothing is playing. Forgets the position being compared against, so that a
// pause, a seek made while paused, or a stretch of a stream is not counted as
// listening when the next local file starts.
void stats_note_idle(void);

// A play began: the same rule library_record_play() counts by, so Most played
// and the Stats page always agree. Banked, written with the seconds.
void stats_note_play(const char *file);

// Writes the seconds and plays gathered so far to the card. Called by the
// minute (see stats_note_playback()) and on the way into standby or off.
void stats_flush(void);

// Clears the counters at startup. Nothing is read from the card here.
void listeningstats_init(void);

// For the tests: everything counted since the last flush, discarded.
void stats_reset_pending(void);

#endif /* LISTENINGSTATS_H */
