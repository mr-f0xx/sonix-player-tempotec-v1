#ifndef MUSIC_TILES_H
#define MUSIC_TILES_H

// The Music page's tiles, as a list of names.
//
// Its own header, with nothing but the enum in it, because two things a long
// way apart both need to name a tile: the page that draws them (music.c) and
// the rule that decides which ones, in what order and where
// (settings/tilelayout.c). Neither of those is LVGL, so neither should have to
// pull the page in to say "albums".

typedef enum {
	MUSIC_TILE_ALL_TRACKS = 0,
	MUSIC_TILE_ALBUMS,
	MUSIC_TILE_ARTISTS,
	MUSIC_TILE_ALBUM_ARTISTS,
	MUSIC_TILE_GENRES,
	MUSIC_TILE_YEARS,
	MUSIC_TILE_PLAYLISTS,
	// Still named, no longer offered on the page: what it showed is the "Top
	// tracks" card of Music > Stats, with the play counts beside each row
	// (issue #494). The tile is kept here because the list behind it is still
	// reachable -- see tilelayout.c on what an arrangement that names it
	// means.
	MUSIC_TILE_MOST_PLAYED,
	MUSIC_TILE_BROWSE,
	MUSIC_TILE_COUNT,
} music_tile_id_t;

#endif /* MUSIC_TILES_H */
