#ifndef GENIUSBUILD_H
#define GENIUSBUILD_H

#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>

// One builder, on the library scan worker. These bounds also keep its output
// inside the reader's decoded-graph budget (including its membership bits).
//
// The builder is a bounded streaming design, not a whole-library buffer. It
// walks the index twice: the first walk counts rows and interns the distinct
// artist/album/genre strings with their sorted ranks, the second keeps only a
// path pool and fixed-width records. Tag strings are therefore held once per
// distinct value rather than once per track, and the graph is encoded straight
// into the staging database in 16 KiB pieces instead of being assembled in RAM.
// A 25,000-track library uses about 4 MiB of builder memory, and the same
// numbers hold up to the row cap.
//
// The track bound counts malformed rows with an empty/missing path, so scanning
// past arbitrarily many excluded rows cannot evade the input budget.
#define GENIUSBUILD_MAX_TRACKS 65535
// Path pool plus the growing distinct-tag pools, both while they coexist.
#define GENIUSBUILD_MAX_TEXT_BYTES ((size_t)6 * 1024 * 1024)
#define GENIUSBUILD_MAX_FIELD_BYTES 4096
// Builder allocations: path pool, per-track records, the path-ordered view,
// candidate targets and the small rule counters.
#define GENIUSBUILD_MAX_RETAINED_BYTES ((size_t)12 * 1024 * 1024)
#define GENIUSBUILD_MAX_BLOB_BYTES ((size_t)12 * 1024 * 1024)
// The whole neighbour array, which is what a mix actually walks. Large
// libraries carry fewer neighbours per track so this total still holds: the
// per-track cap shrinks from GENIUSBUILD_TOP towards GENIUSBUILD_MIN_TOP as the
// library grows (8 bytes per edge in the snapshot).
#define GENIUSBUILD_MAX_EDGE_BYTES ((size_t)2 * 1024 * 1024)
#define GENIUSBUILD_TOP 32
#define GENIUSBUILD_MIN_TOP 4
// Rows of one genre cluster scored per seed track. Clusters at or below this
// size are ranked exactly; a larger one is sampled with a fixed stride so a
// library that files thousands of tracks under one genre still finishes.
#define GENIUSBUILD_MAX_SCAN 4096

typedef enum {
	GENIUSBUILD_OK, // a complete generation was published
	GENIUSBUILD_CURRENT,
	GENIUSBUILD_CANCELLED,
	GENIUSBUILD_LIMIT,
	GENIUSBUILD_NOMEM,
	GENIUSBUILD_INVALID,
	GENIUSBUILD_IO,
	GENIUSBUILD_CHANGED,
} geniusbuild_result_t;

typedef enum {
	GENIUSBUILD_CHECKING,
	GENIUSBUILD_RANKING,
	GENIUSBUILD_PUBLISHING,
	GENIUSBUILD_PUBLISHED,
} geniusbuild_phase_t;
typedef void (*geniusbuild_progress_cb)(geniusbuild_phase_t phase, int completed, int total, void *user);

// Reads tags only through library_read_tags(). Owns .genius-build.db and its
// journal beside path, never the old genius.db. Cancellation, changed inputs,
// limits and I/O errors discard staging. Library close must stop and await this
// call before unmount/export; a physical removal produces cancellation or I/O
// failure. An orphan staging file from a power cut is discarded on the next run.
// No file descriptor is retained on return. Call only from the scan worker.
geniusbuild_result_t geniusbuild_run(const char *path, const atomic_bool *cancel, geniusbuild_progress_cb progress, void *user);

#endif // GENIUSBUILD_H
