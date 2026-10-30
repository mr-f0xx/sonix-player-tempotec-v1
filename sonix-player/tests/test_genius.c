// Exercise the real mix walker against a deterministic, memory-only graph.
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "src/system/library/genius.h"

#define TRACKS 40
struct geniusdb_snapshot {
	int count;
	bool current;
	char paths[TRACKS][32];
	genius_meta_t meta[TRACKS];
	genius_edge_t edges[TRACKS - 1];
};

bool geniusdb_snapshot_current(const geniusdb_snapshot_t *s) { return s && s->current; }
size_t geniusdb_snapshot_bytes(const geniusdb_snapshot_t *s) { (void)s; return sizeof(*s); }
int32_t geniusdb_path_count(const geniusdb_snapshot_t *s) { return s->count; }
int32_t geniusdb_find_path(const geniusdb_snapshot_t *s, const char *path) {
	if (s && path) {
		for (int i = 0; i < s->count; i++) {
			if (strcmp(s->paths[i], path) == 0) return i;
		}
	}
	return -1;
}
const char *geniusdb_get_path(const geniusdb_snapshot_t *s, int32_t id) {
	return s && id >= 0 && id < s->count ? s->paths[id] : NULL;
}
const genius_meta_t *geniusdb_get_meta(const geniusdb_snapshot_t *s, int32_t id) {
	return s && id >= 0 && id < s->count ? &s->meta[id] : NULL;
}
genius_sim_list_t geniusdb_get_sims(const geniusdb_snapshot_t *s, int32_t id) {
	return (genius_sim_list_t){.edges = s->edges, .count = id == 0 ? s->count - 1 : 0};
}
genius_cluster_set_t geniusdb_get_neighbours(const geniusdb_snapshot_t *s, int32_t cluster) {
	(void)s;
	(void)cluster;
	static const int32_t allowed[] = {1};
	return (genius_cluster_set_t){.neighbours = allowed, .count = 1};
}

static void check_mix(geniusdb_snapshot_t *s, int target, int expected) {
	genius_mix_t mix = {0};
	assert(genius_mix_build(s, s->paths[0], target, 123, NULL, &mix) == GENIUS_OK);
	assert(mix.count == expected);
	assert(strcmp(mix.paths[0], s->paths[0]) == 0);
	for (int i = 0; i < mix.count; i++) {
		assert(geniusdb_find_path(s, mix.paths[i]) >= 0);
		assert(mix.paths[i] != geniusdb_get_path(s, geniusdb_find_path(s, mix.paths[i])));
		for (int j = 0; j < i; j++) assert(strcmp(mix.paths[i], mix.paths[j]) != 0);
	}
	genius_mix_free(&mix);
	assert(!mix.paths && !mix.count);
}

int main(void) {
	geniusdb_snapshot_t s = {.count = TRACKS, .current = true};
	for (int i = 0; i < TRACKS; i++) {
		snprintf(s.paths[i], sizeof(s.paths[i]), "/music/track%02d.flac", i);
		s.meta[i] = (genius_meta_t){.cluster = 1, .k1 = i, .k2 = i, .k3 = i, .k20 = i, .present = true};
		if (i) s.edges[i - 1] = (genius_edge_t){.path = 0, .other = i};
	}
	for (int count = 10; count <= 30; count += 5) check_mix(&s, count, count);
	check_mix(&s, 2, 2);

	genius_mix_t mix = {0};
	assert(genius_mix_build(&s, s.paths[0], 0, 123, NULL, &mix) == GENIUS_INVALID);
	assert(!mix.paths && !mix.count);
	assert(genius_mix_build(&s, s.paths[0], 31, 123, NULL, &mix) == GENIUS_INVALID);
	assert(!mix.paths && !mix.count);
	assert(genius_mix_build(&s, "/missing", 30, 123, NULL, &mix) == GENIUS_NO_DATA);
	atomic_bool cancel = true;
	assert(genius_mix_build(&s, s.paths[0], 30, 123, &cancel, &mix) == GENIUS_CANCELLED);
	assert(!mix.paths && !mix.count);
	s.current = false;
	assert(genius_mix_build(&s, s.paths[0], 30, 123, NULL, &mix) == GENIUS_CANCELLED);
	s.current = true;
	s.count = 7;
	check_mix(&s, 30, 7);
	s.count = 1;
	assert(genius_mix_build(&s, s.paths[0], 30, 123, NULL, &mix) == GENIUS_TOO_FEW);
	assert(!mix.paths && !mix.count);
	puts("Smart Mix size tests passed");
	return 0;
}
