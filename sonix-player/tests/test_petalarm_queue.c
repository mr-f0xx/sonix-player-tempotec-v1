/* Checkpoints use the real queue implementation with small fake library handles. */
#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "src/system/library/library.h"
#include "src/system/playback/playlist.h"

struct library_index {
	int id, count;
	bool stale;
	const char *path[4];
};
static int closed[4];
static char saved_album[256];

static library_index_t *make_index(int id, const char *a, const char *b) {
	library_index_t *ix = calloc(1, sizeof(*ix));
	assert(ix);
	ix->id = id;
	ix->count = 2;
	ix->path[0] = a;
	ix->path[1] = b;
	return ix;
}
void library_index_close(library_index_t *ix) {
	if (ix) { closed[ix->id]++; free(ix); }
}
int library_index_count(const library_index_t *ix) { return ix ? ix->count : 0; }
bool library_index_stale(const library_index_t *ix) { return ix && ix->stale; }
int library_index_window(const library_index_t *ix, int offset, int count, library_row_cb cb, void *user) {
	int n = 0;
	while (ix && offset + n < ix->count && n < count) {
		if (!cb("Track", ix->path[offset + n], "Artist", user)) break;
		n++;
	}
	return n;
}
void library_index_describe(const library_index_t *ix, library_index_spec_t *out) {
	(void)ix;
	memset(out, 0, sizeof(*out));
}
library_index_t *library_index_open(library_list_t kind, library_filter_t filter, const char *value,
                                   library_order_t order, bool desc) {
	(void)kind; (void)filter; (void)value; (void)order; (void)desc;
	return NULL;
}
int library_index_find_path(const library_index_t *ix, const char *path) {
	(void)ix; (void)path;
	return -1;
}
void library_queue_save_album(const char *album) { snprintf(saved_album, sizeof(saved_album), "%s", album); }

static void expect_path(int slot, const char *expected) {
	char path[512];
	assert(playlist_path_at(slot, path, sizeof(path)));
	assert(!strcmp(path, expected));
}

static void test_path_queue(void) {
	const char *original[] = {"/a.flac", "/b.flac", "/c.flac", "/d.flac"};
	const char *replacement[] = {"/alarm.flac"};
	playlist_set_mode(PLAYBACK_MODE_SHUFFLE);
	playlist_load_paths(original, 4, 2);
	assert(playlist_insert_next("/added-one.flac"));
	assert(playlist_insert_next("/added-two.flac"));
	playlist_set_album("Previous album");
	char order[6][512];
	for (int i = 0; i < 6; i++) assert(playlist_path_at(i, order[i], sizeof(order[i])));
	int position = playlist_current_index();
	unsigned revision = playlist_revision();

	playlist_checkpoint_t *checkpoint = playlist_checkpoint_take();
	assert(checkpoint && playlist_count() == 0 && playlist_revision() != revision);
	playlist_load_paths(replacement, 1, 0);
	assert(!strcmp(saved_album, ""));
	assert(playlist_checkpoint_restore(checkpoint));
	assert(playlist_count() == 6 && playlist_current_index() == position);
	assert(!strcmp(playlist_album(), "Previous album") && !strcmp(saved_album, "Previous album"));
	for (int i = 0; i < 6; i++) expect_path(i, order[i]);
	// The add-to-queue chain is part of the saved state too.
	assert(playlist_insert_next("/added-three.flac"));
	expect_path(position + 1, "/added-one.flac");
	expect_path(position + 2, "/added-two.flac");
	expect_path(position + 3, "/added-three.flac");
	playlist_clear();
}

static void test_library_queue(void) {
	playlist_set_mode(PLAYBACK_MODE_NORMAL);
	library_index_t *old = make_index(0, "/one.flac", "/two.flac");
	assert(playlist_load_index(old, 1));
	assert(playlist_insert_next("/extra.flac"));
	playlist_checkpoint_t *checkpoint = playlist_checkpoint_take();
	assert(checkpoint && closed[0] == 0);
	assert(playlist_load_index(make_index(1, "/alarm-a.flac", "/alarm-b.flac"), 0));
	assert(playlist_checkpoint_restore(checkpoint));
	assert(closed[0] == 0 && closed[1] == 1);
	assert(playlist_is_library_backed() && playlist_count() == 3 && playlist_current_index() == 1);
	expect_path(0, "/one.flac");
	expect_path(1, "/two.flac");
	expect_path(2, "/extra.flac");

	checkpoint = playlist_checkpoint_take();
	assert(playlist_load_index(make_index(1, "/alarm-a.flac", "/alarm-b.flac"), 0));
	playlist_checkpoint_discard(checkpoint);
	assert(closed[0] == 1 && closed[1] == 1);
	expect_path(0, "/alarm-a.flac");
	playlist_clear();
	assert(closed[1] == 2);
}

static void test_stale_and_empty(void) {
	library_index_t *old = make_index(2, "/old-a.flac", "/old-b.flac");
	assert(playlist_load_index(old, 0));
	playlist_checkpoint_t *checkpoint = playlist_checkpoint_take();
	assert(playlist_load_index(make_index(3, "/new-a.flac", "/new-b.flac"), 0));
	old->stale = true;
	assert(!playlist_checkpoint_restore(checkpoint));
	assert(closed[2] == 1 && closed[3] == 0);
	expect_path(0, "/new-a.flac");
	playlist_clear();
	assert(closed[3] == 1);

	checkpoint = playlist_checkpoint_take();
	assert(checkpoint && playlist_checkpoint_restore(checkpoint) && playlist_count() == 0);
	playlist_checkpoint_discard(NULL);
	assert(!playlist_checkpoint_restore(NULL));
}

int main(void) {
	test_path_queue();
	test_library_queue();
	test_stale_and_empty();
	puts("pet alarm queue checkpoints: all checks passed");
	return 0;
}
