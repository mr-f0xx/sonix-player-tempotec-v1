#include "tilelayout.h"

#include <stdio.h>
#include <string.h>

#include "src/system/core/config.h"

// The Music page's tile arrangement: what a saved list of names means.
//
// "Most played" is no longer one of the tiles. What it showed is the "Top
// tracks" card of Music > Stats, with the play count beside each row and a way
// into the record and the artist from it too (issue #494), and the page has one
// tile the fewer for it. The list behind the tile still exists -- the artist
// pages' "Show artist" pills name it, and so does the player's own history --
// so the tile is still a tile here, and only the page stopped offering it.
//
// What that means for a saved arrangement is the whole of this file:
//
//   * its slug is the one the setting has always held, so a config written
//     before the change comes back as the page it is now, with that tile among
//     the ones not in use rather than silently dropped;
//   * arranging the page again writes it back out under that slug, so the next
//     build that offers it finds it where the user left it;
//   * a config that has never been written does not get it on the page, which
//     is the whole point of retiring it.

// The names, in the order the enum is in. A retired tile keeps its name: it is
// what a config written then holds, and what one written now still holds it
// under.
static const char *const SLUGS[MUSIC_TILE_COUNT] = {
	[MUSIC_TILE_ALL_TRACKS] = "all_tracks",	 [MUSIC_TILE_ALBUMS] = "albums",
	[MUSIC_TILE_ARTISTS] = "artists",		 [MUSIC_TILE_ALBUM_ARTISTS] = "album_artists",
	[MUSIC_TILE_GENRES] = "genres",			 [MUSIC_TILE_YEARS] = "years",
	[MUSIC_TILE_PLAYLISTS] = "playlists",	 [MUSIC_TILE_MOST_PLAYED] = "most_played",
	[MUSIC_TILE_BROWSE] = "browse",
};

// What the page offers, in the order an arrangement that says nothing is read
// in: the library's own index first, then the browser.
static const music_tile_id_t OFFERED[] = {
	MUSIC_TILE_ALL_TRACKS, MUSIC_TILE_ALBUMS,	  MUSIC_TILE_ARTISTS, MUSIC_TILE_ALBUM_ARTISTS, MUSIC_TILE_GENRES,
	MUSIC_TILE_YEARS,	   MUSIC_TILE_PLAYLISTS, MUSIC_TILE_BROWSE,
};
#define OFFERED_COUNT ((int)(sizeof(OFFERED) / sizeof(OFFERED[0])))

int tilelayout_offered(music_tile_id_t *out, int max) {
	if (out && max > 0) {
		int n = OFFERED_COUNT < max ? OFFERED_COUNT : max;
		memcpy(out, OFFERED, (size_t)n * sizeof(music_tile_id_t));
	}
	return OFFERED_COUNT;
}

bool tilelayout_available(music_tile_id_t id) {
	if (id < 0 || id >= MUSIC_TILE_COUNT) {
		return false;
	}
	for (int i = 0; i < OFFERED_COUNT; i++) {
		if (OFFERED[i] == id) {
			return true;
		}
	}
	return false;
}

const char *tilelayout_slug(music_tile_id_t id) {
	if (id < 0 || id >= MUSIC_TILE_COUNT) {
		return "";
	}
	return SLUGS[id];
}

bool tilelayout_parse(const char *slug, size_t len, music_tile_id_t *out) {
	if (!slug || len == 0) {
		return false;
	}
	for (int i = 0; i < MUSIC_TILE_COUNT; i++) {
		if (strlen(SLUGS[i]) == len && strncmp(SLUGS[i], slug, len) == 0) {
			if (out) {
				*out = (music_tile_id_t)i;
			}
			return true;
		}
	}
	return false;
}

// One comma-separated list of names. `seen` keeps the two columns from handing
// the same tile out twice, whatever order they are read in, and is set for a
// name that is dropped as well: a tile already in the first column must not
// come back through the second one.
static int parse_list(const char *csv, music_tile_id_t *out, int max, bool *seen) {
	if (!csv || !csv[0] || !out || max <= 0) {
		return 0;
	}
	int count = 0;
	const char *p = csv;
	while (*p && count < max) {
		while (*p == ' ' || *p == ',') {
			p++;
		}
		if (!*p) {
			break;
		}
		const char *end = p;
		while (*end && *end != ',' && *end != ' ') {
			end++;
		}
		music_tile_id_t id;
		if (tilelayout_parse(p, (size_t)(end - p), &id) && !seen[id]) {
			seen[id] = true;
			out[count++] = id;
		}
		p = end;
	}
	return count;
}

void tilelayout_load(music_tile_id_t *active, int *active_count, music_tile_id_t *hidden, int *hidden_count) {
	bool seen[MUSIC_TILE_COUNT] = {false};
	char active_csv[256] = "";
	char hidden_csv[256] = "";
	const char *saved_active = config_get("music", "main_tiles", "");
	const char *saved_hidden = config_get("music", "main_tiles_hidden", "");
	if (saved_active) {
		snprintf(active_csv, sizeof(active_csv), "%s", saved_active);
	}
	if (saved_hidden) {
		snprintf(hidden_csv, sizeof(hidden_csv), "%s", saved_hidden);
	}

	music_tile_id_t read_active[MUSIC_TILE_COUNT];
	music_tile_id_t read_hidden[MUSIC_TILE_COUNT];
	int read_active_n = parse_list(active_csv, read_active, MUSIC_TILE_COUNT, seen);
	int read_hidden_n = parse_list(hidden_csv, read_hidden, MUSIC_TILE_COUNT, seen);

	int act_n = 0, hid_n = 0;
	for (int i = 0; i < read_active_n; i++) {
		music_tile_id_t id = read_active[i];
		if (tilelayout_available(id)) {
			active[act_n++] = id;
		} else {
			// Named, not offered: honoured in the sense that matters, which is
			// that the user's arrangement is not thrown away.
			hidden[hid_n++] = id;
		}
	}
	for (int i = 0; i < read_hidden_n; i++) {
		hidden[hid_n++] = read_hidden[i];
	}

	// Whatever is left is a tile the config has no opinion on. Every offered
	// one goes on the page in its own order; a retired one waits, because a
	// config that has never been written must not come up with it on the page.
	for (int i = 0; i < OFFERED_COUNT; i++) {
		music_tile_id_t id = OFFERED[i];
		if (!seen[id]) {
			seen[id] = true;
			active[act_n++] = id;
		}
	}
	for (int i = 0; i < MUSIC_TILE_COUNT; i++) {
		if (!seen[i]) {
			hidden[hid_n++] = (music_tile_id_t)i;
		}
	}
	if (act_n == 0 && hid_n > 0) {
		// At least one tile has to be on the page. Nothing in use anywhere is
		// a config from before tiles could be hidden, so the first one comes
		// back rather than a page with no tiles on it at all.
		active[act_n++] = hidden[0];
		memmove(&hidden[0], &hidden[1], (size_t)(hid_n - 1) * sizeof(music_tile_id_t));
		hid_n--;
	}
	*active_count = act_n;
	*hidden_count = hid_n;
}

int tilelayout_active(music_tile_id_t *out, int max) {
	music_tile_id_t active[MUSIC_TILE_COUNT];
	music_tile_id_t hidden[MUSIC_TILE_COUNT];
	int act_n = 0, hid_n = 0;
	tilelayout_load(active, &act_n, hidden, &hid_n);
	if (out && max > 0) {
		int n = act_n < max ? act_n : max;
		memcpy(out, active, (size_t)n * sizeof(music_tile_id_t));
		return n;
	}
	return act_n;
}

int tilelayout_hidden(music_tile_id_t *out, int max) {
	music_tile_id_t active[MUSIC_TILE_COUNT];
	music_tile_id_t hidden[MUSIC_TILE_COUNT];
	int act_n = 0, hid_n = 0;
	tilelayout_load(active, &act_n, hidden, &hid_n);
	if (out && max > 0) {
		int n = hid_n < max ? hid_n : max;
		memcpy(out, hidden, (size_t)n * sizeof(music_tile_id_t));
		return n;
	}
	return hid_n;
}

static void write_list(const music_tile_id_t *list, int count, char *buf, size_t size) {
	size_t used = 0;
	if (size > 0) {
		buf[0] = '\0';
	}
	for (int i = 0; i < count; i++) {
		music_tile_id_t id = list[i];
		if (id < 0 || id >= MUSIC_TILE_COUNT) {
			continue;
		}
		int n = snprintf(buf + used, size - used, "%s%s", used > 0 ? "," : "", SLUGS[id]);
		if (n < 0 || (size_t)n >= size - used) {
			break;
		}
		used += (size_t)n;
	}
}

void tilelayout_save(const music_tile_id_t *active, int active_count, const music_tile_id_t *hidden, int hidden_count) {
	bool seen[MUSIC_TILE_COUNT] = {false};
	music_tile_id_t clean_active[MUSIC_TILE_COUNT];
	music_tile_id_t clean_hidden[MUSIC_TILE_COUNT];
	int act_n = 0, hid_n = 0;

	for (int i = 0; active && i < active_count && act_n < MUSIC_TILE_COUNT; i++) {
		music_tile_id_t id = active[i];
		if (id >= 0 && id < MUSIC_TILE_COUNT && !seen[id]) {
			seen[id] = true;
			clean_active[act_n++] = id;
		}
	}
	for (int i = 0; hidden && i < hidden_count && hid_n < MUSIC_TILE_COUNT; i++) {
		music_tile_id_t id = hidden[i];
		if (id >= 0 && id < MUSIC_TILE_COUNT && !seen[id]) {
			seen[id] = true;
			clean_hidden[hid_n++] = id;
		}
	}
	// Nothing said about a tile is not the same as "on the page": a retired one
	// waits among the ones not in use, exactly as tilelayout_load() reads a
	// config that says nothing.
	for (int i = 0; i < OFFERED_COUNT; i++) {
		music_tile_id_t id = OFFERED[i];
		if (!seen[id]) {
			seen[id] = true;
			clean_active[act_n++] = id;
		}
	}
	for (int i = 0; i < MUSIC_TILE_COUNT; i++) {
		if (!seen[i]) {
			seen[i] = true;
			clean_hidden[hid_n++] = (music_tile_id_t)i;
		}
	}
	if (act_n == 0 && hid_n > 0) {
		clean_active[act_n++] = clean_hidden[0];
		memmove(&clean_hidden[0], &clean_hidden[1], (size_t)(hid_n - 1) * sizeof(music_tile_id_t));
		hid_n--;
	}

	char act_csv[256];
	char hid_csv[256];
	write_list(clean_active, act_n, act_csv, sizeof(act_csv));
	write_list(clean_hidden, hid_n, hid_csv, sizeof(hid_csv));
	config_set("music", "main_tiles", act_csv);
	config_set("music", "main_tiles_hidden", hid_csv);
	config_save();
}
