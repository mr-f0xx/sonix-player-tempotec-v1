#ifndef PODCASTBOOKMARK_H
#define PODCASTBOOKMARK_H

#include <stdbool.h>
#include <stddef.h>

// Where each episode in the podcast folder was left.
//
// The player already remembers one track and one position across a power
// cycle (device_state.c's "remember track"), which is the right shape for
// music and the wrong one for podcasts: coming back to a show means coming
// back to the second of that episode, not to whatever was playing last. So
// the catcher keeps a row per episode file, in its own small database on the
// card, and every episode in the Podcast folder is bookmarked as it plays --
// downloaded by the catcher, downloaded by hand from the Podcasts page, or
// copied there by hand.
//
// Part of the podcast catcher module: with that module off nothing is written
// and nothing is restored, so an install that never asked for this keeps the
// one-track behaviour it has always had.

// The card the database lives on. NULL or empty clears it.
void podcastbookmark_set_root(const char *sd_root);

// Records that `path` was left at `seconds`. `total` is the episode's length,
// or 0 when unknown; a position within a few seconds of the end clears the
// bookmark instead, because there is nothing left to come back to. A position
// under two seconds is ignored: that is a file that has only just opened, not
// a place anybody stopped at.
void podcastbookmark_note(const char *path, double seconds, double total);

// The position to start `path` at, or false when it has none.
bool podcastbookmark_get(const char *path, double *seconds);

// Forgets the position. Called when an episode is started from the beginning
// on purpose, and when it is heard to the end.
void podcastbookmark_clear(const char *path);

#endif /* PODCASTBOOKMARK_H */
