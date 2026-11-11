#include "tidalsync.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "src/system/core/utils.h"
#include "src/system/core/lang.h"
#include "src/system/streaming/listcache.h"

// How many favourites fit in the local mirror. Beyond this the oldest show an
// empty star until touched: the price of not asking Tidal whether each track is
// a favourite on every track change, which would be one extra request per song.
#define FAVORITES_MAX 500

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

static long favorites[FAVORITES_MAX];
static int favorites_count;
static bool favorites_loaded;

// Albums and artists, for the same question the track mirror answers: is this
// already in the favourites? The menu on a search result or on an artist's
// albums used to answer it from the list it was opened in, which is why an
// album that had been favourited from search offered "Add" again at the next
// visit. The ids are all that is kept -- a name is not needed to compare.
#define FAVORITE_ALBUMS_MAX 300
#define FAVORITE_ARTISTS_MAX 300
#define FAVORITES_PAGE 50

static long favorite_albums[FAVORITE_ALBUMS_MAX];
static int favorite_albums_count;
static bool favorite_albums_loaded;

static long favorite_artists[FAVORITE_ARTISTS_MAX];
static int favorite_artists_count;
static bool favorite_artists_loaded;

static tidal_playlist_t playlists[TIDALSYNC_PLAYLISTS_MAX];
static int playlists_count;
static bool playlists_loaded;

// ---------------------------------------------------------------------------
// the mirror
// ---------------------------------------------------------------------------

bool tidalsync_favorites_known(void) {
	pthread_mutex_lock(&lock);
	bool known = favorites_loaded;
	pthread_mutex_unlock(&lock);
	return known;
}

bool tidalsync_is_favorite(long track_id) {
	if (track_id <= 0) {
		return false;
	}
	pthread_mutex_lock(&lock);
	bool found = false;
	for (int i = 0; i < favorites_count && !found; i++) {
		found = favorites[i] == track_id;
	}
	pthread_mutex_unlock(&lock);
	return found;
}

// Call with the lock held.
static void mirror_set_locked(long track_id, bool on) {
	for (int i = 0; i < favorites_count; i++) {
		if (favorites[i] != track_id) {
			continue;
		}
		if (!on) {
			favorites[i] = favorites[--favorites_count];
		}
		return;
	}
	if (on && favorites_count < FAVORITES_MAX) {
		favorites[favorites_count++] = track_id;
	}
}

static void mirror_set(long track_id, bool on) {
	pthread_mutex_lock(&lock);
	mirror_set_locked(track_id, on);
	pthread_mutex_unlock(&lock);
}

// Call with the lock held. A list, not a set: the number of favourites is
// counted in hundreds at most, and a linear scan costs nothing next to the
// request the answer replaces.
static void id_list_set_locked(long *ids, int *count, int max, long id, bool on) {
	if (id <= 0) {
		return;
	}
	for (int i = 0; i < *count; i++) {
		if (ids[i] != id) {
			continue;
		}
		if (!on) {
			ids[i] = ids[--(*count)];
		}
		return;
	}
	if (on && *count < max) {
		ids[(*count)++] = id;
	}
}

bool tidalsync_album_is_favorite(long album_id) {
	if (album_id <= 0) {
		return false;
	}
	pthread_mutex_lock(&lock);
	bool found = false;
	for (int i = 0; i < favorite_albums_count && !found; i++) {
		found = favorite_albums[i] == album_id;
	}
	pthread_mutex_unlock(&lock);
	return found;
}

bool tidalsync_artist_is_favorite(long artist_id) {
	if (artist_id <= 0) {
		return false;
	}
	pthread_mutex_lock(&lock);
	bool found = false;
	for (int i = 0; i < favorite_artists_count && !found; i++) {
		found = favorite_artists[i] == artist_id;
	}
	pthread_mutex_unlock(&lock);
	return found;
}

void tidalsync_forget(void) {
	pthread_mutex_lock(&lock);
	favorites_count = 0;
	favorites_loaded = false;
	favorite_albums_count = 0;
	favorite_albums_loaded = false;
	favorite_artists_count = 0;
	favorite_artists_loaded = false;
	playlists_count = 0;
	playlists_loaded = false;
	pthread_mutex_unlock(&lock);
}

// ---------------------------------------------------------------------------
// the worker
//
// One thread, one queue slot. Submitting while a job is already waiting
// replaces it: these are user taps, and the last one wins. A job already
// running is always finished -- aborting a request that writes to the account
// halfway through would leave its state unknown.
// ---------------------------------------------------------------------------

typedef enum {
	JOB_NONE = 0,
	JOB_FAVORITES_LOAD,
	JOB_FAVORITE_TOGGLE,
	JOB_PLAYLISTS_LOAD,
	JOB_PLAYLIST_ADD,
	JOB_PLAYLIST_CREATE,
	JOB_ALBUM_FAVORITE,
	JOB_PLAYLIST_REMOVE,
	JOB_PLAYLIST_TRACK_REMOVE,
	JOB_ARTIST_FAVORITE,
	JOB_ALBUMS_FAVORITES_LOAD,
	JOB_ARTISTS_FAVORITES_LOAD,
	JOB_PLAYLIST_UNFAVORITE,
} job_kind_t;

typedef struct {
	job_kind_t kind;
	long track_id;
	int index; // which entry of a playlist, for the removal
	// Tidal playlists are addressed by UUID, so the job carries its own copy
	// rather than a number: the caller can free its string as soon as submit()
	// returns, without waiting for the worker.
	char playlist_id[TIDAL_ID_MAX];
	bool want;
	char name[128];
	char album_id[TIDAL_ID_MAX];
	tidalsync_done_cb done;
	void *user;
	void (*changed)(void);
} job_t;

static pthread_mutex_t job_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t job_ready = PTHREAD_COND_INITIALIZER;
static job_t pending;
static bool have_pending;
static bool worker_started;

static void do_favorites_load(const job_t *job) {
	long ids[FAVORITES_MAX];
	int count = tidal_favorite_track_ids(ids, FAVORITES_MAX);
	if (count < 0) {
		printf("tidalsync: favorites not read: %s\n", tidal_last_error());
		return;
	}

	pthread_mutex_lock(&lock);
	memcpy(favorites, ids, sizeof(long) * (size_t)count);
	favorites_count = count;
	favorites_loaded = true;
	pthread_mutex_unlock(&lock);

	printf("tidalsync: %d favorite tracks\n", count);
	if (job->changed) {
		job->changed();
	}
}

static void do_favorite_toggle(const job_t *job) {
	bool ok = job->want ? tidal_favorite_add_track(job->track_id) : tidal_favorite_remove_track(job->track_id);
	if (!ok) {
		// The mirror was flipped before the request so the star could answer the
		// tap immediately; if the request fails it has to go back, or the player
		// and the account disagree.
		mirror_set(job->track_id, !job->want);
	}
	if (job->done) {
		job->done(ok, ok ? NULL : tidal_last_error(), job->user);
	}
}

static void do_playlists_load(const job_t *job) {
	tidal_playlist_t got[TIDALSYNC_PLAYLISTS_MAX];
	int count = tidal_user_playlists(0, got, TIDALSYNC_PLAYLISTS_MAX);
	if (count < 0) {
		printf("tidalsync: playlists not read: %s\n", tidal_last_error());
		return;
	}

	pthread_mutex_lock(&lock);
	memcpy(playlists, got, sizeof(tidal_playlist_t) * (size_t)count);
	playlists_count = count;
	playlists_loaded = true;
	pthread_mutex_unlock(&lock);

	if (job->changed) {
		job->changed();
	}
}

// A playlist's rows and the account's list of playlists are both on the card
// from a visit to the Tidal page; a write here makes them wrong.
static void forget_playlist_caches(const char *playlist_id) {
	char key[64];
	listcache_tidal_playlist_key(playlist_id, key, sizeof(key));
	listcache_drop(key);
	listcache_drop(LISTCACHE_TIDAL_PLAYLISTS);
}

static void do_playlist_add(const job_t *job) {
	bool ok = tidal_playlist_add_track(job->playlist_id, job->track_id);
	if (ok) {
		forget_playlist_caches(job->playlist_id);
	}
	if (job->done) {
		job->done(ok, ok ? NULL : tidal_last_error(), job->user);
	}
}

static void do_playlist_create(const job_t *job) {
	// The new playlist's UUID comes back here: without it there is nowhere to
	// put the track, since the id cannot be guessed.
	char id[TIDAL_ID_MAX] = {0};
	bool ok = tidal_playlist_create(job->name, id, sizeof(id));
	if (ok && job->track_id > 0) {
		ok = tidal_playlist_add_track(id, job->track_id);
	}
	if (ok) {
		// The cached list is one playlist out of date; force a reload so the
		// picker shows the new one next time it opens -- and the copy on the
		// card, which the Tidal page reads.
		pthread_mutex_lock(&lock);
		playlists_loaded = false;
		pthread_mutex_unlock(&lock);
		forget_playlist_caches(id);
	}
	if (job->done) {
		job->done(ok, ok ? NULL : tidal_last_error(), job->user);
	}
}

// The favourite albums, page by page. The answer is a list of full records
// because that is what the endpoint returns; only the ids are kept.
static void do_albums_favorites_load(const job_t *job) {
	static tidal_album_t page[FAVORITES_PAGE];
	long ids[FAVORITE_ALBUMS_MAX];
	int count = 0;
	int offset = 0;
	while (count < FAVORITE_ALBUMS_MAX) {
		int got = tidal_favorite_albums(offset, page, FAVORITES_PAGE);
		if (got < 0) {
			printf("tidalsync: favorite albums not read: %s\n", tidal_last_error());
			return;
		}
		for (int i = 0; i < got && count < FAVORITE_ALBUMS_MAX; i++) {
			if (page[i].id > 0) {
				ids[count++] = page[i].id;
			}
		}
		offset += got;
		if (got < FAVORITES_PAGE) {
			break; // the last page
		}
	}

	pthread_mutex_lock(&lock);
	memcpy(favorite_albums, ids, sizeof(long) * (size_t)count);
	favorite_albums_count = count;
	favorite_albums_loaded = true;
	pthread_mutex_unlock(&lock);

	if (job->changed) {
		job->changed();
	}
}

static void do_artists_favorites_load(const job_t *job) {
	static tidal_artist_t page[FAVORITES_PAGE];
	long ids[FAVORITE_ARTISTS_MAX];
	int count = 0;
	int offset = 0;
	while (count < FAVORITE_ARTISTS_MAX) {
		int got = tidal_favorite_artists(offset, page, FAVORITES_PAGE);
		if (got < 0) {
			printf("tidalsync: favorite artists not read: %s\n", tidal_last_error());
			return;
		}
		for (int i = 0; i < got && count < FAVORITE_ARTISTS_MAX; i++) {
			if (page[i].id > 0) {
				ids[count++] = page[i].id;
			}
		}
		offset += got;
		if (got < FAVORITES_PAGE) {
			break;
		}
	}

	pthread_mutex_lock(&lock);
	memcpy(favorite_artists, ids, sizeof(long) * (size_t)count);
	favorite_artists_count = count;
	favorite_artists_loaded = true;
	pthread_mutex_unlock(&lock);

	if (job->changed) {
		job->changed();
	}
}

static void do_album_favorite(const job_t *job) {
	bool ok = job->want ? tidal_favorite_add_album(job->album_id) : tidal_favorite_remove_album(job->album_id);
	if (!ok) {
		// The mirror was changed before the request so the menu could answer the
		// tap; put it back, or the player and the account disagree.
		pthread_mutex_lock(&lock);
		id_list_set_locked(favorite_albums, &favorite_albums_count, FAVORITE_ALBUMS_MAX, job->track_id,
						   !job->want);
		pthread_mutex_unlock(&lock);
	}
	if (job->done) {
		job->done(ok, ok ? NULL : tidal_last_error(), job->user);
	}
}

static void do_playlist_track_remove(const job_t *job) {
	bool ok = tidal_playlist_remove_track(job->playlist_id, job->index);
	if (ok) {
		// The cached copy of the account's playlists keeps the old track count,
		// which the picker shows.
		pthread_mutex_lock(&lock);
		playlists_loaded = false;
		pthread_mutex_unlock(&lock);
		forget_playlist_caches(job->playlist_id);
	}
	if (job->done) {
		job->done(ok, ok ? NULL : tidal_last_error(), job->user);
	}
}

static void do_artist_favorite(const job_t *job) {
	bool ok = job->want ? tidal_favorite_add_artist(job->track_id) : tidal_favorite_remove_artist(job->track_id);
	if (!ok) {
		pthread_mutex_lock(&lock);
		id_list_set_locked(favorite_artists, &favorite_artists_count, FAVORITE_ARTISTS_MAX, job->track_id,
						   !job->want);
		pthread_mutex_unlock(&lock);
	}
	if (job->done) {
		job->done(ok, ok ? NULL : tidal_last_error(), job->user);
	}
}

static void do_playlist_remove(const job_t *job) {
	bool ok = tidal_playlist_delete(job->playlist_id);
	if (ok) {
		pthread_mutex_lock(&lock);
		playlists_loaded = false; // the cached list still holds the deleted one
		pthread_mutex_unlock(&lock);
		forget_playlist_caches(job->playlist_id);
	}
	if (job->done) {
		job->done(ok, ok ? NULL : tidal_last_error(), job->user);
	}
}

// A favourited playlist is somebody else's, so this touches neither the
// account's own playlists nor the copy of them the picker keeps: only the
// favourites list on the card is made wrong. The playlist's rows go with it --
// they are still true, but nothing is left on the page to open them from.
static void do_playlist_unfavorite(const job_t *job) {
	bool ok = tidal_favorite_remove_playlist(job->playlist_id);
	if (ok) {
		char key[64];
		listcache_tidal_playlist_key(job->playlist_id, key, sizeof(key));
		listcache_drop(key);
		listcache_drop(LISTCACHE_TIDAL_FAV_PLAYLISTS);
	}
	if (job->done) {
		job->done(ok, ok ? NULL : tidal_last_error(), job->user);
	}
}

static void *worker_main(void *arg) {
	(void)arg;
	thread_be_background("tidal sync");

	for (;;) {
		pthread_mutex_lock(&job_lock);
		while (!have_pending) {
			pthread_cond_wait(&job_ready, &job_lock);
		}
		job_t job = pending;
		have_pending = false;
		pthread_mutex_unlock(&job_lock);

		switch (job.kind) {
		case JOB_FAVORITES_LOAD:
			do_favorites_load(&job);
			break;
		case JOB_FAVORITE_TOGGLE:
			do_favorite_toggle(&job);
			break;
		case JOB_PLAYLISTS_LOAD:
			do_playlists_load(&job);
			break;
		case JOB_PLAYLIST_ADD:
			do_playlist_add(&job);
			break;
		case JOB_PLAYLIST_CREATE:
			do_playlist_create(&job);
			break;
		case JOB_ALBUM_FAVORITE:
			do_album_favorite(&job);
			break;
		case JOB_PLAYLIST_REMOVE:
			do_playlist_remove(&job);
			break;
		case JOB_PLAYLIST_TRACK_REMOVE:
			do_playlist_track_remove(&job);
			break;
		case JOB_ARTIST_FAVORITE:
			do_artist_favorite(&job);
			break;
		case JOB_ALBUMS_FAVORITES_LOAD:
			do_albums_favorites_load(&job);
			break;
		case JOB_ARTISTS_FAVORITES_LOAD:
			do_artists_favorites_load(&job);
			break;
		case JOB_PLAYLIST_UNFAVORITE:
			do_playlist_unfavorite(&job);
			break;
		default:
			break;
		}
	}
	return NULL;
}

static void submit(const job_t *job) {
	pthread_mutex_lock(&job_lock);
	if (!worker_started) {
		pthread_t thread;
		if (pthread_create(&thread, NULL, worker_main, NULL) == 0) {
			pthread_detach(thread);
			worker_started = true;
		} else {
			pthread_mutex_unlock(&job_lock);
			if (job->done) {
				job->done(false, tr("cannot_start_the_request"), job->user);
			}
			return;
		}
	}
	pending = *job;
	have_pending = true;
	pthread_cond_signal(&job_ready);
	pthread_mutex_unlock(&job_lock);
}

// ---------------------------------------------------------------------------
// public face
// ---------------------------------------------------------------------------

void tidalsync_favorites_refresh(void (*done)(void)) {
	if (!tidal_logged_in() || tidalsync_favorites_known()) {
		return;
	}
	job_t job = {.kind = JOB_FAVORITES_LOAD, .changed = done};
	submit(&job);
}

void tidalsync_favorite_toggle(long track_id, bool want, tidalsync_done_cb done, void *user) {
	if (track_id <= 0) {
		if (done) {
			done(false, tr("tidal_track_without_id"), user);
		}
		return;
	}

	// Mirror first, network after: the star fills under the finger instead of a
	// second and a half later.
	mirror_set(track_id, want);

	job_t job = {.kind = JOB_FAVORITE_TOGGLE, .track_id = track_id, .want = want, .done = done, .user = user};
	submit(&job);
}

void tidalsync_playlist_add(const char *playlist_id, long track_id, tidalsync_done_cb done, void *user) {
	if (!playlist_id || !playlist_id[0]) {
		if (done) {
			done(false, tr("tidal_playlist_without_id"), user);
		}
		return;
	}
	job_t job = {.kind = JOB_PLAYLIST_ADD, .track_id = track_id, .done = done, .user = user};
	snprintf(job.playlist_id, sizeof(job.playlist_id), "%s", playlist_id);
	submit(&job);
}

void tidalsync_playlist_create_with(const char *name, long track_id, tidalsync_done_cb done, void *user) {
	job_t job = {.kind = JOB_PLAYLIST_CREATE, .track_id = track_id, .done = done, .user = user};
	snprintf(job.name, sizeof(job.name), "%s", name ? name : "");
	submit(&job);
}

void tidalsync_album_favorite(const char *album_id, bool want, tidalsync_done_cb done, void *user) {
	if (!album_id || !album_id[0]) {
		if (done) {
			done(false, tr("tidal_album_without_id"), user);
		}
		return;
	}
	// Tidal addresses an album by its UUID and its numeric id is the same
	// number in the mirror, taken from the digits before the first dash.
	long numeric = strtol(album_id, NULL, 10);
	pthread_mutex_lock(&lock);
	id_list_set_locked(favorite_albums, &favorite_albums_count, FAVORITE_ALBUMS_MAX, numeric, want);
	pthread_mutex_unlock(&lock);

	job_t job = {.kind = JOB_ALBUM_FAVORITE, .want = want, .done = done, .user = user, .track_id = numeric};
	snprintf(job.album_id, sizeof(job.album_id), "%s", album_id);
	submit(&job);
}

void tidalsync_playlist_remove(const char *playlist_id, tidalsync_done_cb done, void *user) {
	if (!playlist_id || !playlist_id[0]) {
		if (done) {
			done(false, tr("tidal_playlist_without_id"), user);
		}
		return;
	}
	job_t job = {.kind = JOB_PLAYLIST_REMOVE, .done = done, .user = user};
	snprintf(job.playlist_id, sizeof(job.playlist_id), "%s", playlist_id);
	submit(&job);
}

void tidalsync_playlist_unfavorite(const char *playlist_id, tidalsync_done_cb done, void *user) {
	if (!playlist_id || !playlist_id[0]) {
		if (done) {
			done(false, tr("tidal_playlist_without_id"), user);
		}
		return;
	}
	job_t job = {.kind = JOB_PLAYLIST_UNFAVORITE, .done = done, .user = user};
	snprintf(job.playlist_id, sizeof(job.playlist_id), "%s", playlist_id);
	submit(&job);
}

void tidalsync_playlist_track_remove(const char *playlist_id, int index, tidalsync_done_cb done, void *user) {
	if (!playlist_id || !*playlist_id || index < 0) {
		if (done) {
			done(false, tr("tidal_playlist_without_id"), user);
		}
		return;
	}
	job_t job = {.kind = JOB_PLAYLIST_TRACK_REMOVE, .index = index, .done = done, .user = user};
	snprintf(job.playlist_id, sizeof(job.playlist_id), "%s", playlist_id);
	submit(&job);
}

void tidalsync_artist_favorite(long artist_id, bool want, tidalsync_done_cb done, void *user) {
	if (artist_id <= 0) {
		if (done) {
			done(false, tr("tidal_artist_without_id"), user);
		}
		return;
	}
	// Mirror first, as for a track: the menu has to be right the next time it
	// is opened, even before the request comes back.
	pthread_mutex_lock(&lock);
	id_list_set_locked(favorite_artists, &favorite_artists_count, FAVORITE_ARTISTS_MAX, artist_id, want);
	pthread_mutex_unlock(&lock);

	job_t job = {.kind = JOB_ARTIST_FAVORITE, .track_id = artist_id, .want = want, .done = done, .user = user};
	submit(&job);
}

// Reads the two mirrors the first time a page that asks about them is opened.
// Cheap when they are already known: no job is submitted at all.
void tidalsync_albums_refresh(void (*done)(void)) {
	if (!tidal_logged_in()) {
		return;
	}
	pthread_mutex_lock(&lock);
	bool known = favorite_albums_loaded;
	pthread_mutex_unlock(&lock);
	if (known) {
		return;
	}
	job_t job = {.kind = JOB_ALBUMS_FAVORITES_LOAD, .changed = done};
	submit(&job);
}

void tidalsync_artists_refresh(void (*done)(void)) {
	if (!tidal_logged_in()) {
		return;
	}
	pthread_mutex_lock(&lock);
	bool known = favorite_artists_loaded;
	pthread_mutex_unlock(&lock);
	if (known) {
		return;
	}
	job_t job = {.kind = JOB_ARTISTS_FAVORITES_LOAD, .changed = done};
	submit(&job);
}

void tidalsync_playlists_refresh(void (*done)(void)) {
	if (!tidal_logged_in()) {
		return;
	}
	job_t job = {.kind = JOB_PLAYLISTS_LOAD, .changed = done};
	submit(&job);
}

bool tidalsync_playlists_known(void) {
	pthread_mutex_lock(&lock);
	bool known = playlists_loaded;
	pthread_mutex_unlock(&lock);
	return known;
}

int tidalsync_playlists(tidal_playlist_t *out, int max) {
	if (!out || max <= 0) {
		return 0;
	}
	pthread_mutex_lock(&lock);
	int count = playlists_count < max ? playlists_count : max;
	memcpy(out, playlists, sizeof(tidal_playlist_t) * (size_t)count);
	pthread_mutex_unlock(&lock);
	return count;
}
