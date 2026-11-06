#include "src/gui/library/browserstate.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static void path(char *out, size_t size, const char *a, const char *b) {
	assert(snprintf(out, size, "%s/%s", a, b) < (int)size);
}

int main(void) {
	char tmp[] = "/tmp/sonix-browser-XXXXXX";
	assert(mkdtemp(tmp));
	char root[512], music[512], album[512], state[512], song[512], outside[512], link[512], chosen[512];
	path(root, sizeof(root), tmp, "card"); assert(mkdir(root, 0700) == 0);
	path(music, sizeof(music), root, "Music"); assert(mkdir(music, 0700) == 0);
	path(album, sizeof(album), music, "album with spaces\n雪"); assert(mkdir(album, 0700) == 0);
	path(song, sizeof(song), album, "track.flac"); FILE *f = fopen(song, "w"); assert(f); fclose(f);
	path(state, sizeof(state), tmp, "state");
	path(outside, sizeof(outside), tmp, "other"); assert(mkdir(outside, 0700) == 0);
	path(link, sizeof(link), root, "escape"); assert(symlink(outside, link) == 0);
	int scroll = -1;
	assert(browser_state_save(state, album, 789));
	assert(browser_state_load(state, chosen, sizeof(chosen), &scroll));
	assert(strcmp(album, chosen) == 0 && scroll == 789);
	assert(browser_state_choose(root, chosen, scroll, song, chosen, sizeof(chosen), &scroll));
	assert(strcmp(chosen, album) == 0 && scroll == 789);
	assert(browser_state_choose(root, link, 200, song, chosen, sizeof(chosen), &scroll));
	assert(strcmp(chosen, album) == 0 && scroll == 0);
	assert(browser_state_choose(root, outside, 200, "/missing", chosen, sizeof(chosen), &scroll));
	assert(strcmp(chosen, music) == 0 && scroll == 0);
	assert(!browser_state_directory(root, outside, chosen, sizeof(chosen)));
	assert(!browser_state_directory(root, link, chosen, sizeof(chosen)));
	assert(!browser_state_load(state, chosen, 4, &scroll));
	char long_path[512];
	memset(long_path, 'x', 500); long_path[500] = '\0';
	assert(browser_state_save(state, long_path, 42));
	assert(browser_state_load(state, chosen, sizeof(chosen), &scroll));
	assert(strcmp(chosen, long_path) == 0 && scroll == 42);
	memset(long_path, 'x', sizeof(long_path) - 1); long_path[sizeof(long_path) - 1] = '\0';
	assert(browser_state_save(state, long_path, -1));
	assert(browser_state_load(state, chosen, sizeof(chosen), &scroll) && scroll == 0);
	f = fopen(state, "wb"); assert(f); fputs("truncated", f); fclose(f);
	assert(!browser_state_load(state, chosen, sizeof(chosen), &scroll));
	assert(unlink(song) == 0); assert(rmdir(album) == 0); assert(rmdir(music) == 0);
	assert(browser_state_choose(root, "", 0, "", chosen, sizeof(chosen), &scroll));
	assert(strcmp(chosen, root) == 0);
	unlink(link); unlink(state); rmdir(outside); rmdir(root); rmdir(tmp);
	puts("PASS browser state: persistence, validation and fallback");
	return 0;
}
