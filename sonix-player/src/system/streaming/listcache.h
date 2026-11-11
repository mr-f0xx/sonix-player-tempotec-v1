#ifndef LISTCACHE_H
#define LISTCACHE_H

#include <stdbool.h>
#include <stddef.h>

// A list of rows, kept on the card between visits.
//
// Tidal and Podcasts rebuild every list from the network when a tab is opened:
// the titles, the artist names, the ids, the cover addresses, the whole page of
// them, once per visit and once per track added. That is a request and a spinner
// for an answer that has not changed since the last time the same page was
// looked at, and on a slow card and a slow connection it is the difference
// between a tab and a wait.
//
// What is cached here is the page's own record array -- tidal_track_t,
// podcast_feed_t and their like. Those are plain structs of fixed-size fields
// with no pointers in them, so the cache is a header and the array written as it
// lies. Nothing is parsed on the way back: the file is read into the same array
// the network would have filled.
//
// Which lists are worth keeping, and for how long, is the caller's decision:
// favourites, a playlist, a podcast's episodes change when the user changes
// something, and every write on those pages drops the file it touched
// (listcache_drop), so a file that survives is the list. A search is not kept:
// every query is a different answer.
//
// The directory is <card>/.local/listcache. Losing it costs nothing but a
// request; a corrupt or half-written file is rejected by the header and read as
// a miss.

// Sets the card root. NULL or empty turns the cache off, as before it is set.
void listcache_set_root(const char *sd_root);
bool listcache_ready(void);

// Writes `count` records of `item_size` bytes each. Replaced atomically: a power
// cut in the middle leaves the previous file, never a torn one.
bool listcache_save(const char *key, const void *items, size_t item_size, int count);

// Reads at most `max` records of `item_size` bytes into `items`. Returns how
// many were read, or -1 when there is no usable file (missing, another record
// size, another version, corrupt).
int listcache_load(const char *key, void *items, size_t item_size, int max);

// How long ago the file was written, in seconds; -1 when there is no file.
int listcache_age(const char *key);

// Forgets one file. Called by every write that makes the list it names wrong:
// adding a favourite, taking a track out of a playlist, creating one.
void listcache_drop(const char *key);

// ---------------------------------------------------------------------------
// The Tidal keys the page and the sync module both name
//
// A list is written by the page that draws it and invalidated by whoever changes
// it, which for a playlist is the sync module. One place for the spelling, or a
// rename would leave the stale file behind on the card until it aged out.
// ---------------------------------------------------------------------------

#define LISTCACHE_TIDAL_FAV_TRACKS "tidal_fav_tracks"
#define LISTCACHE_TIDAL_FAV_ALBUMS "tidal_fav_albums"
#define LISTCACHE_TIDAL_FAV_ARTISTS "tidal_fav_artists"
#define LISTCACHE_TIDAL_FAV_PLAYLISTS "tidal_fav_playlists"
#define LISTCACHE_TIDAL_PLAYLISTS "tidal_playlists"

// The tracks of one playlist, by its UUID.
void listcache_tidal_playlist_key(const char *playlist_id, char *out, size_t size);

#endif /* LISTCACHE_H */
