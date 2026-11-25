// test_musictiles.c -- the Music page's tile arrangement, without LVGL.
//
// What is checked is what a config holds and what the page makes of it, which
// is the half of "Rearrange main page" that decides what the Music page has on
// it. Issue #494 took "Most played" off that page -- what it showed is the
// "Top tracks" card of Music > Stats -- so the interesting cases are the ones
// about a tile the page no longer offers: a config that names it, a config that
// does not, and a page arranged again afterwards.
//
// The rule itself is src/gui/settings/tilelayout.c, which has no display in it,
// and the real config.c is the store it reads and writes.

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "src/gui/library/music_tiles.h"
#include "src/gui/settings/tilelayout.h"
#include "src/system/core/config.h"

static int failures;

#define CHECK(cond)                                                                             \
	do {                                                                                    \
		if (!(cond)) {                                                                  \
			failures++;                                                             \
			fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);         \
		}                                                                               \
	} while (0)

// Which tiles a list holds, in order, as slugs: the thing the page and the
// config both speak.
static void slugs(const music_tile_id_t *list, int count, char *out, size_t size) {
	size_t used = 0;
	out[0] = '\0';
	for (int i = 0; i < count; i++) {
		int n = snprintf(out + used, size - used, "%s%s", used > 0 ? "," : "",
				 tilelayout_slug(list[i]));
		if (n < 0 || (size_t)n >= size - used) {
			return;
		}
		used += (size_t)n;
	}
}

static int tile_in(const music_tile_id_t *list, int count, music_tile_id_t id) {
	int n = 0;
	for (int i = 0; i < count; i++) {
		if (list[i] == id) {
			n++;
		}
	}
	return n;
}

// A fresh config file for each case, so nothing one case writes is read by the
// next.
static void fresh_config(const char *dir) {
	char path[512];
	snprintf(path, sizeof(path), "%s/device_config.ini", dir);
	unlink(path);
	config_init(path);
}

int main(void) {
	char dir[] = "/tmp/sonix-musictiles-XXXXXX";
	if (!mkdtemp(dir)) {
		perror("mkdtemp");
		return 1;
	}

	music_tile_id_t active[MUSIC_TILE_COUNT];
	music_tile_id_t hidden[MUSIC_TILE_COUNT];
	char list[512];

	// --- a config that says nothing -----------------------------------------
	// Every tile the page offers, in the page's own order, and the retired one
	// waiting among the ones not in use. A player that has never arranged its
	// Music page must not come up with "Most played" on it: that is the old
	// page, not this one.
	fresh_config(dir);
	int act_n = tilelayout_active(active, MUSIC_TILE_COUNT);
	int hid_n = tilelayout_hidden(hidden, MUSIC_TILE_COUNT);
	slugs(active, act_n, list, sizeof(list));
	CHECK(strcmp(list, "all_tracks,albums,artists,album_artists,genres,years,playlists,browse") == 0);
	CHECK(tile_in(active, act_n, MUSIC_TILE_MOST_PLAYED) == 0);
	CHECK(tile_in(hidden, hid_n, MUSIC_TILE_MOST_PLAYED) == 1);
	CHECK(act_n + hid_n == MUSIC_TILE_COUNT);

	// --- a config from a build that had it on the page -----------------------
	// The tile is still named, so it is still honoured in the sense that
	// matters: it does not vanish from the arrangement, it lands among the ones
	// not in use, and everything behind it keeps the place it was put in.
	fresh_config(dir);
	config_set("music", "main_tiles", "playlists,most_played,albums,artists");
	config_set("music", "main_tiles_hidden", "");
	config_save();
	act_n = tilelayout_active(active, MUSIC_TILE_COUNT);
	hid_n = tilelayout_hidden(hidden, MUSIC_TILE_COUNT);
	slugs(active, act_n, list, sizeof(list));
	// The four it named, minus the retired one, then the ones it did not name.
	CHECK(strcmp(list, "playlists,albums,artists,all_tracks,album_artists,genres,years,browse") == 0);
	CHECK(tile_in(hidden, hid_n, MUSIC_TILE_MOST_PLAYED) == 1);
	CHECK(act_n + hid_n == MUSIC_TILE_COUNT);

	// Arranging the page again writes it all back out: the retired tile goes
	// into the hidden column under the slug it always had, so the next build
	// that offers it finds it where the user left it.
	tilelayout_save(active, act_n, hidden, hid_n);
	CHECK(strcmp(config_get("music", "main_tiles", ""),
				 "playlists,albums,artists,all_tracks,album_artists,genres,years,browse") == 0);
	CHECK(strcmp(config_get("music", "main_tiles_hidden", ""), "most_played") == 0);

	// --- a config that hid it, as a user already could ----------------------
	// Music > Settings > Library > Rearrange main page has offered this since
	// the tile arrived, and a page arranged with it hidden is the page this
	// build draws anyway -- with the tile still written down where it was.
	fresh_config(dir);
	config_set("music", "main_tiles", "all_tracks,albums,artists,album_artists,genres,years,playlists,browse");
	config_set("music", "main_tiles_hidden", "most_played");
	config_save();
	act_n = tilelayout_active(active, MUSIC_TILE_COUNT);
	hid_n = tilelayout_hidden(hidden, MUSIC_TILE_COUNT);
	slugs(active, act_n, list, sizeof(list));
	CHECK(strcmp(list, "all_tracks,albums,artists,album_artists,genres,years,playlists,browse") == 0);
	slugs(hidden, hid_n, list, sizeof(list));
	CHECK(strcmp(list, "most_played") == 0);

	// --- a name nothing knows ----------------------------------------------
	// An unknown slug is dropped rather than read as some other tile, and the
	// page is still the whole page.
	fresh_config(dir);
	config_set("music", "main_tiles", "albums,not_a_tile,years");
	config_set("music", "main_tiles_hidden", "");
	config_save();
	act_n = tilelayout_active(active, MUSIC_TILE_COUNT);
	hid_n = tilelayout_hidden(hidden, MUSIC_TILE_COUNT);
	slugs(active, act_n, list, sizeof(list));
	CHECK(strcmp(list, "albums,years,all_tracks,artists,album_artists,genres,playlists,browse") == 0);
	CHECK(act_n + hid_n == MUSIC_TILE_COUNT);

	// --- the same slug in both columns -------------------------------------
	// The first column wins, the second does not hand it out twice, and the
	// page still holds it once.
	fresh_config(dir);
	config_set("music", "main_tiles", "years,albums");
	config_set("music", "main_tiles_hidden", "years,playlists");
	config_save();
	act_n = tilelayout_active(active, MUSIC_TILE_COUNT);
	hid_n = tilelayout_hidden(hidden, MUSIC_TILE_COUNT);
	CHECK(tile_in(active, act_n, MUSIC_TILE_YEARS) == 1);
	CHECK(tile_in(hidden, hid_n, MUSIC_TILE_YEARS) == 0);
	CHECK(tile_in(hidden, hid_n, MUSIC_TILE_PLAYLISTS) == 1);
	CHECK(act_n + hid_n == MUSIC_TILE_COUNT);

	// --- every tile hidden but the retired one -----------------------------
	// There has to be something on the page: the first one not in use comes
	// back rather than a Music page with nothing on it.
	fresh_config(dir);
	config_set("music", "main_tiles", "");
	config_set("music", "main_tiles_hidden",
			   "all_tracks,albums,artists,album_artists,genres,years,playlists,browse,most_played");
	config_save();
	act_n = tilelayout_active(active, MUSIC_TILE_COUNT);
	hid_n = tilelayout_hidden(hidden, MUSIC_TILE_COUNT);
	CHECK(act_n >= 1);
	CHECK(act_n + hid_n == MUSIC_TILE_COUNT);

	// The name a tile is saved under is the one it always had, retired or not:
	// nothing a user arranged is rewritten by a build that stopped drawing one
	// tile.
	CHECK(strcmp(tilelayout_slug(MUSIC_TILE_MOST_PLAYED), "most_played") == 0);
	CHECK(strcmp(tilelayout_slug(MUSIC_TILE_BROWSE), "browse") == 0);
	music_tile_id_t parsed = MUSIC_TILE_COUNT;
	CHECK(tilelayout_parse("most_played", strlen("most_played"), &parsed));
	CHECK(parsed == MUSIC_TILE_MOST_PLAYED);
	CHECK(!tilelayout_parse("not_a_tile", strlen("not_a_tile"), &parsed));
	CHECK(!tilelayout_available(MUSIC_TILE_MOST_PLAYED));
	CHECK(tilelayout_available(MUSIC_TILE_ALBUMS));
	CHECK(!tilelayout_available(MUSIC_TILE_COUNT));

	// The page offers every tile but the one it retired, and offers them in the
	// order a config that says nothing is read in -- the order checked at the
	// top of this file, said here without a config in the way.
	music_tile_id_t offered[MUSIC_TILE_COUNT];
	int offered_n = tilelayout_offered(offered, MUSIC_TILE_COUNT);
	CHECK(offered_n == MUSIC_TILE_COUNT - 1);
	CHECK(tile_in(offered, offered_n, MUSIC_TILE_MOST_PLAYED) == 0);
	CHECK(offered_n > 0 && offered[0] == MUSIC_TILE_ALL_TRACKS);
	CHECK(offered_n > 0 && offered[offered_n - 1] == MUSIC_TILE_BROWSE);

	if (failures == 0) {
		printf("PASS music tiles: Most played off the page, arrangements from before it still honoured\n");
	}
	return failures ? 1 : 0;
}
