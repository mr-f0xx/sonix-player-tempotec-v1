#ifndef TIDALSYNC_H
#define TIDALSYNC_H

#include <stdbool.h>
#include <stddef.h>

#include "src/system/streaming/tidal.h"

// What the player writes back to the Tidal account, off the UI thread.
//
// The twin of qobuzsync.h, same reasons and same contract. The star and "add to
// playlist" are one tap each, and for a Tidal track they have to land on the
// account rather than in the local database: the stored path would be a cache
// file that is gone twenty tracks later, and no other device of the user's
// would see the favourite.
//
// An HTTP request takes seconds and the UI thread cannot stall, so this holds
// one worker with a single-slot queue, and every job ends in a callback. That
// callback runs on the worker, not on the UI thread -- the receiver must bounce
// it itself with gui_post(), as the rest of the program does.
//
// The star also cannot wait for the network to know how to draw itself, so a
// mirror of the favourite track ids is kept in memory, filled once and updated
// on every tap.
//
// The only real difference from Qobuz is that Tidal playlists are named by UUID
// rather than by number, so they pass through here as strings.

// --- the favourites mirror -------------------------------------------------

bool tidalsync_favorites_known(void);
bool tidalsync_is_favorite(long track_id);
void tidalsync_forget(void);
void tidalsync_favorites_refresh(void (*done)(void));

// The same answer for an album or an artist, needed for the same reason: a
// result in a search or an artist's album list has to offer Remove when it is
// already in the favourites, and Add when it is not. Tracks have had a mirror
// for a while; albums and artists were taken on trust from the list they were
// opened in, which is exactly what made a search result offer Add twice.
bool tidalsync_album_is_favorite(long album_id);
bool tidalsync_artist_is_favorite(long artist_id);
void tidalsync_albums_refresh(void (*done)(void));
void tidalsync_artists_refresh(void (*done)(void));

// --- the jobs --------------------------------------------------------------

typedef void (*tidalsync_done_cb)(bool ok, const char *error, void *user);

// Adds or removes the track from the account's favourites. The mirror is
// updated immediately, before the network, so the star responds to the tap, and
// put back if the request fails.
void tidalsync_favorite_toggle(long track_id, bool want, tidalsync_done_cb done, void *user);

void tidalsync_playlist_add(const char *playlist_id, long track_id, tidalsync_done_cb done, void *user);
void tidalsync_playlist_create_with(const char *name, long track_id, tidalsync_done_cb done, void *user);

// Adds or removes an album from the favourites. The mirror is updated before
// the request, as it is for tracks, and put back if the request fails.
void tidalsync_album_favorite(const char *album_id, bool want, tidalsync_done_cb done, void *user);

void tidalsync_playlist_remove(const char *playlist_id, tidalsync_done_cb done, void *user);

// Takes a playlist out of the favourites: one somebody else made, which the
// account keeps but does not own. Not the function above, which deletes one of
// the account's own; this one leaves the playlist where it is.
void tidalsync_playlist_unfavorite(const char *playlist_id, tidalsync_done_cb done, void *user);

// Takes one entry out of a playlist, by the position it has in the playlist
// (the row's index on the page).
void tidalsync_playlist_track_remove(const char *playlist_id, int index, tidalsync_done_cb done, void *user);

// Follows or unfollows an artist, mirrored like an album's favourite.
void tidalsync_artist_favorite(long artist_id, bool want, tidalsync_done_cb done, void *user);

// --- the account's playlists -----------------------------------------------

#define TIDALSYNC_PLAYLISTS_MAX 60

void tidalsync_playlists_refresh(void (*done)(void));
int tidalsync_playlists(tidal_playlist_t *out, int max);
bool tidalsync_playlists_known(void);

#endif /* TIDALSYNC_H */
