#define _GNU_SOURCE
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "src/system/gearboy/gbdb.h"

static void make_dir(const char *path) {
	assert(mkdir(path, 0777) == 0);
}

static void make_file(const char *path, size_t size) {
	FILE *f = fopen(path, "wb");
	assert(f);
	for (size_t i = 0; i < size; i++) {
		fputc((int)(i & 0xff), f);
	}
	assert(fclose(f) == 0);
}

int main(void) {
	char root[] = "/tmp/sonix-gbdb-XXXXXX";
	assert(mkdtemp(root));
	char path[1024];
#define MKDIR(suffix) do { snprintf(path, sizeof(path), "%s/%s", root, suffix); make_dir(path); } while (0)
#define FILEPATH(suffix) snprintf(path, sizeof(path), "%s/%s", root, suffix)
	MKDIR("Games");
	MKDIR("Games/GB");
	MKDIR("Games/GBC");
	MKDIR("Games/GB/Cover Art");
	MKDIR("Games/GBC/Cover Art");

	FILEPATH("Games/GB/Tetris.gb"); make_file(path, 32 * 1024);
	FILEPATH("Games/GB/Pokemon_Red.gb"); make_file(path, 32 * 1024);
	FILEPATH("Games/GB/No Art.gb"); make_file(path, 32 * 1024);
	FILEPATH("Games/GBC/Color Quest.gbc"); make_file(path, 32 * 1024);
	FILEPATH("Games/GB/Cover Art/tetris.PNG"); make_file(path, 8);
	FILEPATH("Games/GB/Cover Art/Pokemon Red.jpeg"); make_file(path, 8);
	FILEPATH("Games/GBC/Cover Art/Color-Quest.jpg"); make_file(path, 8);

	gbdb_set_root(root);
	gbrom_t *roms = NULL;
	int count = gbdb_scan(&roms, NULL, NULL);
	assert(count == 4);
	int found_tetris = 0, found_pokemon = 0, found_color = 0, found_empty = 0;
	for (int i = 0; i < count; i++) {
		if (strstr(roms[i].path, "/Tetris.gb")) {
			assert(!roms[i].color);
			assert(strstr(roms[i].cover_path, "/Cover Art/tetris.PNG"));
			found_tetris++;
		} else if (strstr(roms[i].path, "/Pokemon_Red.gb")) {
			assert(strstr(roms[i].cover_path, "/Cover Art/Pokemon Red.jpeg"));
			found_pokemon++;
		} else if (strstr(roms[i].path, "/Color Quest.gbc")) {
			assert(roms[i].color);
			assert(strstr(roms[i].cover_path, "/Cover Art/Color-Quest.jpg"));
			found_color++;
		} else if (strstr(roms[i].path, "/No Art.gb")) {
			assert(!roms[i].cover_path[0]);
			found_empty++;
		}
	}
	assert(found_tetris && found_pokemon && found_color && found_empty);
	free(roms);

	// Remove the fixture, including the scan cache.
	const char *files[] = {"Games/GB/Tetris.gb", "Games/GB/Pokemon_Red.gb", "Games/GB/No Art.gb",
		"Games/GBC/Color Quest.gbc", "Games/GB/Cover Art/tetris.PNG", "Games/GB/Cover Art/Pokemon Red.jpeg",
		"Games/GBC/Cover Art/Color-Quest.jpg", ".local/gearboy-cache/index.tsv"};
	for (size_t i = 0; i < sizeof(files) / sizeof(files[0]); i++) {
		FILEPATH(files[i]);
		unlink(path);
	}
	const char *dirs[] = {"Games/GB/Cover Art", "Games/GBC/Cover Art", "Games/GB", "Games/GBC", "Games", ".local/gearboy-cache", ".local"};
	for (size_t i = 0; i < sizeof(dirs) / sizeof(dirs[0]); i++) {
		FILEPATH(dirs[i]);
		rmdir(path);
	}
	rmdir(root);
	puts("gbdb cover lookup: ok");
	return 0;
}
