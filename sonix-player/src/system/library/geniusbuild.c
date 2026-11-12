#include "geniusbuild.h"

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "src/system/core/sha256.h"
#include "src/system/db/sqlite3.h"
#include "src/system/library/geniusdb.h"
#include "src/system/library/library.h"

#define STAGING ".genius-build.db"
#define HEADER_BYTES 80
#define IO_BYTES 16384
// Scores 0..6: shared album (3), artist (2) and year (1), summed.
#define BUCKETS 7
// Candidates kept per score group while walking a genre. Twice the selected
// maximum, so the per-artist and per-album caps can reject within a group
// without starving it of the next candidates the reference producer would see.
#define BUCKET_ENTRIES (GENIUSBUILD_TOP * 2)
// Accepted-candidate counters for the per-artist (2) and per-album (3) caps. A
// seed accepts at most GENIUSBUILD_TOP of each, so these stay lightly loaded.
#define COUNTER_SLOTS 128
// What the reader keeps per path once the snapshot is decoded: a sorted path
// pointer (4), a 40-byte metadata record, an adjacency range (8), a membership
// bit (1/8) and its share of the path pool, which is measured separately.
#define READER_BYTES_PER_PATH 57

// One record per track. The tags themselves are not kept: their sorted ranks
// stand in for them, and the dictionary that names them is released before the
// ranking starts.
typedef struct {
	const char *path;
	int64_t year;
	uint32_t artist_id, album_id, cluster;
} track_t;

typedef struct {
	const char *text;
	uint32_t id, group;
} order_t;

// Distinct-value dictionary. Slots are open-addressed; offset is one past the
// string's place in the pool (zero marks an empty slot) and rank is set from
// the sorted order of the values, one past the rank for the same reason.
typedef struct {
	uint32_t hash, offset, rank;
} dict_slot_t;

typedef struct {
	char *pool;
	size_t pool_bytes, pool_cap;
	dict_slot_t *slots;
	uint32_t slots_cap, used;
	const char **texts;
} dict_t;

typedef struct {
	const atomic_bool *cancel;
	geniusbuild_progress_cb progress;
	void *user;
	geniusbuild_result_t result;
	// Bytes charged against GENIUSBUILD_MAX_RETAINED_BYTES, and the share of
	// GENIUSBUILD_MAX_TEXT_BYTES taken by the path and dictionary pools.
	size_t retained, peak, text_bytes;
	track_t *tracks;
	char *text;
	size_t text_used, text_cap;
	uint32_t count, rows_seen;
	// The first walk's tally, which the second must reproduce exactly.
	uint32_t survey_rows, survey_tracks;
	size_t survey_path_bytes;
	unsigned revision;
	// Rank of the empty string in the artist and album dictionaries, so an
	// untagged field can be recognised after the dictionaries are gone.
	int32_t empty_artist, empty_album;
	dict_t artists, albums, genres;
} build_t;

static bool cancelled(const build_t *b) { return b->cancel && atomic_load(b->cancel); }
static int interrupt_sql(void *user) { return cancelled(user); }

static void report(build_t *b, geniusbuild_phase_t phase, int completed, int total) {
	if (b->progress) {
		b->progress(phase, completed, total, b->user);
	}
}

// Every allocation the build keeps is charged before it is made, so a limit is
// reported as LIMIT rather than discovered as an out-of-memory failure later.
static bool reserve(build_t *b, size_t bytes) {
	if (b->retained > GENIUSBUILD_MAX_RETAINED_BYTES || bytes > GENIUSBUILD_MAX_RETAINED_BYTES - b->retained) {
		b->result = GENIUSBUILD_LIMIT;
		return false;
	}
	b->retained += bytes;
	if (b->retained > b->peak) b->peak = b->retained;
	return true;
}

static void uncharge(build_t *b, size_t bytes) { b->retained -= bytes; }

static void *allocate(build_t *b, size_t count, size_t width) {
	if (!count) count = 1;
	if (width && !reserve(b, count * width)) return NULL;
	void *p = calloc(count, width);
	if (!p) {
		uncharge(b, count * width);
		b->result = GENIUSBUILD_NOMEM;
	}
	return p;
}

static void release(build_t *b, void *p, size_t bytes) {
	free(p);
	uncharge(b, bytes);
}

static void put32(unsigned char *p, uint32_t v) {
	for (unsigned i = 0; i < 4; i++) {
		p[i] = (unsigned char)(v >> (8 * i));
	}
}

static void put64(unsigned char *p, uint64_t v) {
	put32(p, (uint32_t)v);
	put32(p + 4, (uint32_t)(v >> 32));
}

static uint32_t get32(const unsigned char *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

// In-place heapsort: bounded O(n log n) work, no libc sort scratch allocation.
static void swap_rows(unsigned char *a, unsigned char *b, size_t width) {
	while (width--) { unsigned char byte = *a; *a++ = *b; *b++ = byte; }
}

static void sift(unsigned char *rows, size_t width, size_t root, size_t n, int (*compare)(const void *, const void *)) {
	while (root < n / 2) {
		size_t child = root * 2 + 1;
		if (child + 1 < n && compare(rows + child * width, rows + (child + 1) * width) < 0) child++;
		if (compare(rows + root * width, rows + child * width) >= 0) break;
		swap_rows(rows + root * width, rows + child * width, width);
		root = child;
	}
}

static bool sort_rows(build_t *b, void *rows, size_t n, size_t width, int (*compare)(const void *, const void *)) {
	unsigned char *p = rows;
	for (size_t root = n / 2; root > 0;) {
		if (cancelled(b)) return false;
		sift(p, width, --root, n, compare);
	}
	for (size_t end = n; end > 1;) {
		if (cancelled(b)) return false;
		swap_rows(p, p + --end * width, width);
		sift(p, width, 0, end, compare);
	}
	return true;
}

static bool utf8(const unsigned char *p, size_t bytes) {
	for (size_t i = 0; i < bytes;) {
		unsigned char c = p[i++];
		if (c && c < 0x80) {
			continue;
		}
		unsigned n = c >= 0xc2 && c <= 0xdf ? 1 : c >= 0xe0 && c <= 0xef ? 2 : c >= 0xf0 && c <= 0xf4 ? 3 : 0;
		if (!n || n > bytes - i || (c == 0xe0 && p[i] < 0xa0) || (c == 0xed && p[i] >= 0xa0) ||
			(c == 0xf0 && p[i] < 0x90) || (c == 0xf4 && p[i] >= 0x90)) {
			return false;
		}
		while (n--) {
			if ((p[i++] & 0xc0) != 0x80) {
				return false;
			}
		}
	}
	return true;
}

// A field is reproducible only as the UTF-8 it claims to be, and only within
// the builder's per-field bound.
static bool field_ok(build_t *b, const char *text, size_t bytes) {
	if (bytes > GENIUSBUILD_MAX_FIELD_BYTES) {
		b->result = GENIUSBUILD_LIMIT;
		return false;
	}
	if (!utf8((const unsigned char *)text, bytes)) {
		b->result = GENIUSBUILD_INVALID;
		return false;
	}
	return true;
}

// ---------------------------------------------------------------------------
// Distinct tag values, held once each.
//
// The first walk interns every distinct artist, album and genre string and
// measures the path pool. Sorting the distinct values then gives exactly the
// dense ids the old whole-library sort produced, because they are the ranks of
// the same strings in the same UTF-8 byte order. The second walk stores one
// fixed-width record per track instead of a second copy of its tags.
// ---------------------------------------------------------------------------

static uint32_t text_hash(const char *text, size_t bytes) {
	uint32_t hash = UINT32_C(2166136261);
	for (size_t i = 0; i < bytes; i++) {
		hash = (hash ^ (unsigned char)text[i]) * UINT32_C(16777619);
	}
	return hash ? hash : 1; // zero would look like a hash that never matches
}

static bool dict_grow(build_t *b, dict_t *d, uint32_t capacity) {
	size_t bytes = (size_t)capacity * sizeof(*d->slots);
	if (!reserve(b, bytes)) return false;
	dict_slot_t *slots = calloc(capacity, sizeof(*slots));
	if (!slots) {
		uncharge(b, bytes);
		b->result = GENIUSBUILD_NOMEM;
		return false;
	}
	for (uint32_t i = 0; i < d->slots_cap; i++) {
		if (!d->slots[i].offset) continue;
		uint32_t slot = d->slots[i].hash & (capacity - 1);
		while (slots[slot].offset) slot = (slot + 1) & (capacity - 1);
		slots[slot] = d->slots[i];
	}
	release(b, d->slots, (size_t)d->slots_cap * sizeof(*d->slots));
	d->slots = slots;
	d->slots_cap = capacity;
	return true;
}

static bool dict_grow_pool(build_t *b, dict_t *d, size_t need) {
	size_t capacity = d->pool_cap ? d->pool_cap : 1024;
	while (capacity < need) {
		if (capacity > GENIUSBUILD_MAX_TEXT_BYTES / 2) {
			b->result = GENIUSBUILD_LIMIT;
			return false;
		}
		capacity *= 2;
	}
	size_t growth = capacity - d->pool_cap;
	if (b->text_bytes > GENIUSBUILD_MAX_TEXT_BYTES || growth > GENIUSBUILD_MAX_TEXT_BYTES - b->text_bytes) {
		b->result = GENIUSBUILD_LIMIT;
		return false;
	}
	if (!reserve(b, growth)) return false;
	char *pool = realloc(d->pool, capacity);
	if (!pool) {
		uncharge(b, growth);
		b->result = GENIUSBUILD_NOMEM;
		return false;
	}
	b->text_bytes += growth;
	d->pool = pool;
	d->pool_cap = capacity;
	return true;
}

static bool dict_intern(build_t *b, dict_t *d, const char *text, size_t bytes) {
	if (d->used * 2 >= d->slots_cap) {
		uint32_t capacity = d->slots_cap ? d->slots_cap * 2 : 128;
		if (capacity > GENIUSBUILD_MAX_TRACKS * 4 || !dict_grow(b, d, capacity)) {
			if (b->result == GENIUSBUILD_OK) b->result = GENIUSBUILD_LIMIT;
			return false;
		}
	}
	if (d->pool_bytes + bytes + 1 > d->pool_cap && !dict_grow_pool(b, d, d->pool_bytes + bytes + 1)) {
		return false;
	}
	uint32_t hash = text_hash(text, bytes);
	uint32_t slot = hash & (d->slots_cap - 1);
	while (d->slots[slot].offset) {
		const char *known = d->pool + d->slots[slot].offset - 1;
		if (d->slots[slot].hash == hash && strlen(known) == bytes && !memcmp(known, text, bytes)) {
			return true;
		}
		slot = (slot + 1) & (d->slots_cap - 1);
	}
	memcpy(d->pool + d->pool_bytes, text, bytes);
	d->pool[d->pool_bytes + bytes] = 0;
	d->slots[slot] = (dict_slot_t){hash, (uint32_t)d->pool_bytes + 1, 0};
	d->pool_bytes += bytes + 1;
	d->used++;
	return true;
}

typedef struct {
	const char *text;
	uint32_t slot;
} dict_entry_t;

static int entry_compare(const void *a, const void *b) { return strcmp(((const dict_entry_t *)a)->text, ((const dict_entry_t *)b)->text); }

// Assigns every distinct value the rank of its position in UTF-8 byte order,
// which is what the whole-library dense ids used to be.
static bool dict_finish(build_t *b, dict_t *d) {
	if (!d->used) return true;
	dict_entry_t *entries = allocate(b, d->used, sizeof(*entries));
	if (!entries) return false;
	uint32_t n = 0;
	for (uint32_t i = 0; i < d->slots_cap; i++) {
		if (d->slots[i].offset) {
			entries[n++] = (dict_entry_t){d->pool + d->slots[i].offset - 1, i};
		}
	}
	if (!sort_rows(b, entries, n, sizeof(*entries), entry_compare)) {
		release(b, entries, (size_t)d->used * sizeof(*entries));
		return false;
	}
	d->texts = allocate(b, d->used, sizeof(*d->texts));
	if (!d->texts) {
		release(b, entries, (size_t)d->used * sizeof(*entries));
		return false;
	}
	for (uint32_t rank = 0; rank < n; rank++) {
		d->slots[entries[rank].slot].rank = rank + 1;
		d->texts[rank] = entries[rank].text;
	}
	release(b, entries, (size_t)d->used * sizeof(*entries));
	return true;
}

// Rank of a value seen again, or -1 when the index changed under the walk.
static int32_t dict_find(const dict_t *d, const char *text, size_t bytes) {
	if (!d->slots_cap) return -1;
	uint32_t hash = text_hash(text, bytes);
	uint32_t slot = hash & (d->slots_cap - 1);
	while (d->slots[slot].offset) {
		if (d->slots[slot].hash == hash) {
			const char *known = d->pool + d->slots[slot].offset - 1;
			if (strlen(known) == bytes && !memcmp(known, text, bytes)) {
				return (int32_t)(d->slots[slot].rank - 1);
			}
		}
		slot = (slot + 1) & (d->slots_cap - 1);
	}
	return -1;
}

// Only what was actually charged is given back: a dictionary that failed
// halfway through has no ranks array to release.
static void dict_free(build_t *b, dict_t *d) {
	if (d->texts) release(b, d->texts, (size_t)d->used * sizeof(*d->texts));
	if (d->slots) release(b, d->slots, (size_t)d->slots_cap * sizeof(*d->slots));
	if (d->pool) release(b, d->pool, d->pool_cap);
	*d = (dict_t){0};
}

// First walk: count the rows, measure the paths, name the distinct tags.
static bool survey(const library_tags_t *tags, void *user) {
	build_t *b = user;
	if (cancelled(b)) {
		return false;
	}
	if (++b->rows_seen > GENIUSBUILD_MAX_TRACKS) {
		b->result = GENIUSBUILD_LIMIT;
		return false;
	}
	if (!field_ok(b, tags->artist, tags->artist_bytes) || !field_ok(b, tags->album, tags->album_bytes) ||
		!field_ok(b, tags->genre, tags->genre_bytes)) {
		return false;
	}
	if (!dict_intern(b, &b->artists, tags->artist, tags->artist_bytes) ||
		!dict_intern(b, &b->albums, tags->album, tags->album_bytes) ||
		!dict_intern(b, &b->genres, tags->genre, tags->genre_bytes)) {
		return false;
	}
	if (!tags->path_bytes) return true; // same exclusion as the reference SQL
	if (!field_ok(b, tags->path, tags->path_bytes)) return false;
	if (b->survey_path_bytes > GENIUSBUILD_MAX_TEXT_BYTES - tags->path_bytes - 1) {
		b->result = GENIUSBUILD_LIMIT;
		return false;
	}
	b->survey_path_bytes += tags->path_bytes + 1;
	b->survey_tracks++;
	if (!(b->rows_seen & 1023)) report(b, GENIUSBUILD_CHECKING, (int)b->rows_seen, (int)b->rows_seen);
	return true;
}

static int path_compare(const void *a, const void *b) {
	const char *x = ((const track_t *)a)->path, *y = ((const track_t *)b)->path;
	for (;;) {
		unsigned char cx = (unsigned char)*x++, cy = (unsigned char)*y++;
		if (cx >= 'A' && cx <= 'Z') cx += 'a' - 'A';
		if (cy >= 'A' && cy <= 'Z') cy += 'a' - 'A';
		if (cx != cy || !cx) return (int)cx - (int)cy;
	}
}

// Second walk: one fixed-width record and one path per track, with the tag
// values reduced to the ranks the first walk established.
static bool collect(const library_tags_t *tags, void *user) {
	build_t *b = user;
	if (cancelled(b)) {
		return false;
	}
	if (++b->rows_seen > GENIUSBUILD_MAX_TRACKS) {
		b->result = GENIUSBUILD_LIMIT;
		return false;
	}
	if (!tags->path_bytes) return true;
	if (b->count >= b->survey_tracks) {
		b->result = GENIUSBUILD_CHANGED; // more tracks than the first walk found
		return false;
	}
	int32_t artist = dict_find(&b->artists, tags->artist, tags->artist_bytes);
	int32_t album = dict_find(&b->albums, tags->album, tags->album_bytes);
	int32_t genre = dict_find(&b->genres, tags->genre, tags->genre_bytes);
	if (artist < 0 || album < 0 || genre < 0) {
		b->result = GENIUSBUILD_CHANGED;
		return false;
	}
	if (!field_ok(b, tags->path, tags->path_bytes)) return false;
	if (tags->path_bytes + 1 > b->text_cap - b->text_used) {
		b->result = GENIUSBUILD_LIMIT;
		return false;
	}
	track_t *t = &b->tracks[b->count++];
	memcpy(b->text + b->text_used, tags->path, tags->path_bytes);
	b->text[b->text_used + tags->path_bytes] = 0;
	t->path = b->text + b->text_used;
	b->text_used += tags->path_bytes + 1;
	t->year = tags->year;
	t->artist_id = (uint32_t)artist;
	t->album_id = (uint32_t)album;
	t->cluster = (uint32_t)genre;
	if (!(b->count & 1023)) report(b, GENIUSBUILD_CHECKING, (int)b->count, (int)b->survey_rows);
	return true;
}

// Canonical input identity: domain (including NUL), then in NOCASE path order
// four u32-length-prefixed UTF-8 strings (path/artist/album/genre), then s64
// year; NULL tags/year are normalized to empty/zero. No row ids or timestamps.
// The tag strings come back from their ranks, so the digest names exactly what
// the index held even though the tags themselves are no longer per track.
static void input_digest(build_t *b, char out[SHA256_HEX_LEN]) {
	sha256_ctx hash;
	sha256_init(&hash);
	static const char domain[] = "sonix-genius-input-v1";
	sha256_update(&hash, domain, sizeof(domain));
	for (uint32_t i = 0; i < b->count; i++) {
		if (cancelled(b)) return;
		track_t *t = &b->tracks[i];
		const char *texts[] = {t->path, b->artists.texts[t->artist_id], b->albums.texts[t->album_id], b->genres.texts[t->cluster]};
		unsigned char word[8];
		for (unsigned j = 0; j < 4; j++) {
			size_t n = strlen(texts[j]);
			put32(word, (uint32_t)n);
			sha256_update(&hash, word, 4);
			sha256_update(&hash, texts[j], n);
		}
		put64(word, (uint64_t)t->year);
		sha256_update(&hash, word, 8);
	}
	sha256_final_hex(&hash, out);
}

// Read only the identity and bounded header, never the whole old BLOB. Legacy
// exporters lacking input_sha256 are compared by their static graph digest
// after ranking; an exactly equivalent PC graph is kept, including its producer.
static bool read_generation(build_t *b, const char *path, char out[4097]) {
	sqlite3 *db = NULL;
	sqlite3_stmt *stmt = NULL;
	sqlite3_blob *blob = NULL;
	bool ok = false;
	if (sqlite3_open_v2(path, &db, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK) goto done;
	sqlite3_progress_handler(db, 1024, interrupt_sql, b);
	sqlite3_exec(db, "PRAGMA cache_size=-128", NULL, NULL, NULL);
	const char *sql = "SELECT id, CASE WHEN typeof(generation)='text' AND octet_length(generation)"
		" BETWEEN 1 AND 4096 THEN generation END,typeof(data),length(data) FROM GENIUS_SNAPSHOT";
	if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK || sqlite3_step(stmt) != SQLITE_ROW) goto done;
	int bytes = sqlite3_column_bytes(stmt, 1);
	const char *text = (const char *)sqlite3_column_text(stmt, 1);
	const char *type = (const char *)sqlite3_column_text(stmt, 2);
	int64_t length = sqlite3_column_int64(stmt, 3);
	if (sqlite3_column_type(stmt, 0) != SQLITE_INTEGER || sqlite3_column_int64(stmt, 0) != 0 || !type || strcmp(type, "blob") ||
		!text || bytes <= 0 || bytes > 4096 || memchr(text, 0, (size_t)bytes) || length < HEADER_BYTES || length > (int64_t)GENIUSBUILD_MAX_BLOB_BYTES) goto done;
	memcpy(out, text, (size_t)bytes);
	out[bytes] = 0;
	if (sqlite3_step(stmt) != SQLITE_DONE) goto done;
	sqlite3_finalize(stmt);
	stmt = NULL;
	if (sqlite3_blob_open(db, "main", "GENIUS_SNAPSHOT", "data", 0, 0, &blob) != SQLITE_OK) goto done;
	unsigned char header[HEADER_BYTES], buffer[IO_BYTES];
	if (cancelled(b) || sqlite3_blob_read(blob, header, sizeof(header), 0) != SQLITE_OK ||
		memcmp(header, "SONIXGS\0", 8) || get32(header + 8) != 1 || get32(header + 12) != HEADER_BYTES ||
		get32(header + 16) != length || get32(header + 20) != (uint32_t)bytes || get32(header + 44) != HEADER_BYTES || get32(header + 76)) goto done;
	if (sqlite3_blob_read(blob, buffer, bytes, HEADER_BYTES) != SQLITE_OK || memcmp(buffer, out, (size_t)bytes)) goto done;
	uint64_t paths = get32(header + 24), edges = get32(header + 28), groups = get32(header + 32), neighbours = get32(header + 36), pool = get32(header + 40);
	if (paths > INT32_MAX || edges > INT32_MAX || groups > INT32_MAX || neighbours > INT32_MAX || (!groups && neighbours)) goto done;
	uint64_t sizes[] = {(paths + 1) * 4, pool, paths * 36, paths * 8, edges * 8, groups * 12, neighbours * 4};
	uint64_t at = HEADER_BYTES + (uint32_t)bytes;
	sha256_ctx hash;
	sha256_init(&hash);
	sha256_update(&hash, header + 24, 20);
	for (unsigned i = 0; i < 7; i++) {
		if (get32(header + 48 + i * 4) != at || at + sizes[i] > (uint64_t)length) goto done;
		put32(buffer, (uint32_t)sizes[i]);
		sha256_update(&hash, buffer, 4);
		uint64_t end = at + sizes[i];
		while (at < end) {
			int n = end - at < IO_BYTES ? (int)(end - at) : IO_BYTES;
			if (cancelled(b) || sqlite3_blob_read(blob, buffer, n, (int)at) != SQLITE_OK) goto done;
			sha256_update(&hash, buffer, (size_t)n);
			at += (unsigned)n;
		}
	}
	char digest[SHA256_HEX_LEN], field[80];
	sha256_final_hex(&hash, digest);
	snprintf(field, sizeof(field), "\"sha256\":\"%s\"", digest);
	ok = at == (uint64_t)length && strstr(out, field);
done:
	if (blob) sqlite3_blob_close(blob);
	sqlite3_finalize(stmt);
	if (db) sqlite3_close(db);
	return ok;
}

static bool field_matches(const char *generation, const char *key, const char *value) {
	char field[128];
	snprintf(field, sizeof(field), "\"%s\":\"%s\"", key, value);
	return strstr(generation, field) != NULL;
}

// ---------------------------------------------------------------------------
// Encoding: the sections are rendered on the fly, in 16 KiB pieces, straight
// into the staging BLOB (or into the digest). Nothing proportional to the
// library is assembled in memory, so the peak is the path pool and the ranking
// arrays rather than the whole graph.
// ---------------------------------------------------------------------------

typedef struct {
	uint32_t count, top, edges, clusters, pool;
	uint32_t sizes[7];
	const track_t *tracks;
	const unsigned char *counts;
	const uint16_t *targets;
} sections_t;

typedef bool (*geniusbuild_sink_t)(void *user, size_t offset, const unsigned char *data, size_t bytes);

typedef struct {
	unsigned char buffer[IO_BYTES];
	size_t used, offset;
	geniusbuild_sink_t sink;
	void *user;
	bool ok;
} emitter_t;

static void emitter_put(emitter_t *e, const void *data, size_t bytes) {
	const unsigned char *p = data;
	while (e->ok && bytes) {
		if (e->used == sizeof(e->buffer)) {
			if (!e->sink(e->user, e->offset, e->buffer, e->used)) {
				e->ok = false;
				return;
			}
			e->offset += e->used;
			e->used = 0;
		}
		size_t n = sizeof(e->buffer) - e->used;
		if (n > bytes) n = bytes;
		memcpy(e->buffer + e->used, p, n);
		e->used += n;
		p += n;
		bytes -= n;
	}
}

static bool emitter_flush(emitter_t *e) {
	if (e->ok && e->used) {
		if (!e->sink(e->user, e->offset, e->buffer, e->used)) {
			e->ok = false;
		} else {
			e->offset += e->used;
			e->used = 0;
		}
	}
	return e->ok;
}

// Renders the seven sections named by `mask`, always in contract order. Offsets
// are relative to the first section, so a caller may emit them in one call or
// one call per section and still write the same bytes in the same places.
static bool emit_sections(const sections_t *s, unsigned mask, const atomic_bool *cancel, geniusbuild_sink_t sink, void *user) {
	emitter_t e = {.sink = sink, .user = user, .ok = true};
	unsigned char record[36];
	for (unsigned section = 0; section < 7 && e.ok; section++) {
		if (!(mask & (1u << section))) {
			e.offset += s->sizes[section];
			continue;
		}
		if (section == 0) {
			uint64_t at = 0;
			for (uint32_t i = 0; i < s->count; i++) {
				if (!(i & 1023) && cancel && atomic_load(cancel)) return false;
				put32(record, (uint32_t)at);
				emitter_put(&e, record, 4);
				at += strlen(s->tracks[i].path) + 5;
			}
			put32(record, (uint32_t)at);
			emitter_put(&e, record, 4);
		} else if (section == 1) {
			// u32 length, UTF-8 bytes, one zero; the trailing zero permits
			// borrowed C strings in the reader.
			for (uint32_t i = 0; i < s->count; i++) {
				if (!(i & 1023) && cancel && atomic_load(cancel)) return false;
				const char *path = s->tracks[i].path;
				uint32_t length = (uint32_t)strlen(path);
				put32(record, length);
				emitter_put(&e, record, 4);
				emitter_put(&e, path, length);
				emitter_put(&e, "", 1);
			}
		} else if (section == 2) {
			for (uint32_t i = 0; i < s->count; i++) {
				if (!(i & 1023) && cancel && atomic_load(cancel)) return false;
				const track_t *t = &s->tracks[i];
				memset(record, 0, sizeof(record));
				put32(record, t->cluster);
				put64(record + 4, t->artist_id);
				for (unsigned k = 12; k <= 28; k += 8) put64(record + k, (uint64_t)i + 1);
				emitter_put(&e, record, sizeof(record));
			}
		} else if (section == 3) {
			uint32_t edge = 0;
			for (uint32_t i = 0; i < s->count; i++) {
				if (!(i & 1023) && cancel && atomic_load(cancel)) return false;
				put32(record, edge);
				put32(record + 4, s->counts[i]);
				emitter_put(&e, record, 8);
				edge += s->counts[i];
			}
		} else if (section == 4) {
			for (uint32_t i = 0; i < s->count; i++) {
				if (!(i & 1023) && cancel && atomic_load(cancel)) return false;
				for (unsigned j = 0; j < s->counts[i]; j++) {
					put32(record, i);
					put32(record + 4, s->targets[(size_t)i * s->top + j]);
					emitter_put(&e, record, 8);
				}
			}
		} else if (section == 5) {
			for (uint32_t i = 0; i < s->clusters; i++) {
				if (!(i & 1023) && cancel && atomic_load(cancel)) return false;
				put32(record, i);
				put32(record + 4, i);
				put32(record + 8, 1);
				emitter_put(&e, record, 12);
			}
		} else {
			for (uint32_t i = 0; i < s->clusters; i++) {
				if (!(i & 1023) && cancel && atomic_load(cancel)) return false;
				put32(record, i);
				emitter_put(&e, record, 4);
			}
		}
	}
	return emitter_flush(&e);
}

static bool digest_sink(void *user, size_t offset, const unsigned char *data, size_t bytes) {
	(void)offset;
	sha256_update(user, data, bytes);
	return true;
}

typedef struct {
	sqlite3_blob *blob;
	const atomic_bool *cancel;
	size_t base;
} blob_sink_t;

static bool blob_sink(void *user, size_t offset, const unsigned char *data, size_t bytes) {
	blob_sink_t *w = user;
	while (bytes) {
		size_t n = bytes < IO_BYTES ? bytes : IO_BYTES;
		if ((w->cancel && atomic_load(w->cancel)) || sqlite3_blob_write(w->blob, data, (int)n, (int)(w->base + offset)) != SQLITE_OK) {
			return false;
		}
		data += n;
		offset += n;
		bytes -= n;
	}
	return true;
}

static geniusbuild_result_t publish(build_t *b, int dir, const char *parent, const struct stat *directory, unsigned revision, const char *generation, size_t total, const sections_t *sections) {
	geniusbuild_result_t result = GENIUSBUILD_IO;
	sqlite3 *db = NULL;
	sqlite3_stmt *stmt = NULL;
	sqlite3_blob *blob = NULL;
	int fd = -1;
	struct stat current, artifact;
	if (stat(parent, &current) != 0 || current.st_dev != directory->st_dev || current.st_ino != directory->st_ino || cancelled(b)) goto done;
	// These two names belong exclusively to this builder, including orphans.
	unlinkat(dir, STAGING "-journal", 0);
	unlinkat(dir, STAGING, 0);
	fd = openat(dir, STAGING, O_CREAT | O_EXCL | O_RDWR | O_NOFOLLOW, 0600);
	if (fd < 0 || fstat(fd, &artifact) != 0) goto done;
	close(fd);
	fd = -1;
	char staging[64];
	snprintf(staging, sizeof(staging), "/proc/self/fd/%d/" STAGING, dir);
	if (sqlite3_open_v2(staging, &db, SQLITE_OPEN_READWRITE, NULL) != SQLITE_OK) goto done;
	const char *opened = sqlite3_db_filename(db, "main");
	if (!opened || stat(opened, &current) != 0 || current.st_dev != artifact.st_dev || current.st_ino != artifact.st_ino) goto done;
	sqlite3_progress_handler(db, 1024, interrupt_sql, b);
	sqlite3_limit(db, SQLITE_LIMIT_LENGTH, (int)GENIUSBUILD_MAX_BLOB_BYTES + 4096);
	if (sqlite3_exec(db, "PRAGMA journal_mode=DELETE; PRAGMA synchronous=FULL; PRAGMA cache_size=-128;"
		" BEGIN; CREATE TABLE GENIUS_SNAPSHOT(id INTEGER PRIMARY KEY CHECK(id=0),generation TEXT,data BLOB)", NULL, NULL, NULL) != SQLITE_OK) goto done;
	if (sqlite3_prepare_v2(db, "INSERT INTO GENIUS_SNAPSHOT VALUES(0,?1,?2)", -1, &stmt, NULL) != SQLITE_OK ||
		sqlite3_bind_text(stmt, 1, generation, -1, SQLITE_STATIC) != SQLITE_OK ||
		sqlite3_bind_zeroblob(stmt, 2, (int)total) != SQLITE_OK || sqlite3_step(stmt) != SQLITE_DONE) goto done;
	sqlite3_finalize(stmt);
	stmt = NULL;
	if (sqlite3_blob_open(db, "main", "GENIUS_SNAPSHOT", "data", 0, 1, &blob) != SQLITE_OK) goto done;
	size_t generation_bytes = strlen(generation);
	if (generation_bytes > 512) goto done;
	unsigned char header[HEADER_BYTES];
	memset(header, 0, sizeof(header));
	memcpy(header, "SONIXGS\0", 8);
	put32(header + 8, 1);
	put32(header + 12, HEADER_BYTES);
	put32(header + 16, (uint32_t)total);
	put32(header + 20, (uint32_t)generation_bytes);
	put32(header + 24, sections->count);
	put32(header + 28, sections->edges);
	put32(header + 32, sections->clusters);
	put32(header + 36, sections->clusters);
	put32(header + 40, sections->pool);
	put32(header + 44, HEADER_BYTES);
	uint32_t position = HEADER_BYTES + (uint32_t)generation_bytes;
	for (unsigned i = 0; i < 7; i++) {
		put32(header + 48 + i * 4, position);
		position += sections->sizes[i];
	}
	if (cancelled(b) || sqlite3_blob_write(blob, header, sizeof(header), 0) != SQLITE_OK ||
		sqlite3_blob_write(blob, generation, (int)generation_bytes, HEADER_BYTES) != SQLITE_OK) goto done;
	blob_sink_t sink = {.blob = blob, .cancel = b->cancel, .base = HEADER_BYTES + generation_bytes};
	if (!emit_sections(sections, 0x7f, b->cancel, blob_sink, &sink) || cancelled(b)) goto done;
	if (sqlite3_blob_close(blob) != SQLITE_OK) { blob = NULL; goto done; }
	blob = NULL;
	if (cancelled(b) || sqlite3_exec(db, "COMMIT", NULL, NULL, NULL) != SQLITE_OK) goto done;
	if (sqlite3_prepare_v2(db, "PRAGMA quick_check", -1, &stmt, NULL) != SQLITE_OK || sqlite3_step(stmt) != SQLITE_ROW ||
		!sqlite3_column_text(stmt, 0) || strcmp((const char *)sqlite3_column_text(stmt, 0), "ok") || sqlite3_step(stmt) != SQLITE_DONE) goto done;
	sqlite3_finalize(stmt);
	stmt = NULL;
	if (sqlite3_close(db) != SQLITE_OK) goto done;
	db = NULL;
	fd = openat(dir, STAGING, O_RDWR | O_NOFOLLOW);
	if (fd < 0 || fsync(fd) != 0 || fstat(fd, &current) != 0 || current.st_dev != artifact.st_dev || current.st_ino != artifact.st_ino) goto done;
	close(fd);
	fd = -1;
	// A directory descriptor pins this mount. Never publish to a new card that
	// happens to have acquired the same mount-point name while I/O was failing.
	if (stat(parent, &current) != 0 || current.st_dev != directory->st_dev || current.st_ino != directory->st_ino) goto done;
	if (library_revision(LIBRARY_LIST_TRACKS) != revision) { result = GENIUSBUILD_CHANGED; goto done; }
	if (cancelled(b)) goto done;
	if (renameat(dir, STAGING, dir, "genius.db") != 0) goto done;
	result = GENIUSBUILD_OK;
	// Some card filesystems do not support directory fsync. The complete file
	// has already been synced and atomically renamed; report that distinction.
	if (fsync(dir) != 0) fprintf(stderr, "geniusbuild: published; directory sync failed: %s\n", strerror(errno));
	report(b, GENIUSBUILD_PUBLISHED, (int)b->count, (int)b->count);
done:
	if (fd >= 0) close(fd);
	if (blob) sqlite3_blob_close(blob);
	sqlite3_finalize(stmt);
	if (db) sqlite3_close(db);
	if (result != GENIUSBUILD_OK) {
		unlinkat(dir, STAGING "-journal", 0);
		unlinkat(dir, STAGING, 0);
		if (cancelled(b)) result = GENIUSBUILD_CANCELLED;
	}
	return result;
}

// Neighbours kept per track. The edge array is what a mounted graph costs, so a
// large library carries fewer per track rather than a bigger array. Two caps:
// the fixed edge budget, and what the reader's decoded-graph budget leaves
// after the path pool, the per-path records and the cluster tables.
static uint32_t neighbour_limit(build_t *b, size_t pool, uint32_t clusters) {
	size_t fixed = pool + (size_t)b->count * READER_BYTES_PER_PATH + (size_t)clusters * 16 + 512;
	if (!b->count) return GENIUSBUILD_TOP;
	if (fixed >= GENIUSDB_MAX_SNAPSHOT_BYTES) {
		b->result = GENIUSBUILD_LIMIT;
		return 0;
	}
	size_t allowance = GENIUSDB_MAX_SNAPSHOT_BYTES - fixed;
	size_t top = GENIUSBUILD_TOP;
	size_t by_snapshot = allowance / ((size_t)b->count * 8);
	size_t by_edges = GENIUSBUILD_MAX_EDGE_BYTES / ((size_t)b->count * 8);
	if (by_snapshot < top) top = by_snapshot;
	if (by_edges < top) top = by_edges;
	if (top < GENIUSBUILD_MIN_TOP) top = GENIUSBUILD_MIN_TOP;
	// Even the smallest graph must fit what the reader keeps.
	if ((size_t)top * 8 * b->count > allowance) {
		b->result = GENIUSBUILD_LIMIT;
		return 0;
	}
	return (uint32_t)top;
}

// Accepted-candidate counters for the per-artist and per-album caps: a tiny
// fixed open-addressed table, cleared once per seed. Direct-indexing a dense id
// array would cost a memset of the whole library for every seed. The caps are
// checked before either table is changed, exactly as the reference producer
// tests both conditions before counting a candidate.
static bool counter_allows(const uint32_t *keys, const uint32_t *hits, uint32_t key, uint32_t limit) {
	uint32_t slot = (key * UINT32_C(2654435761)) & (COUNTER_SLOTS - 1);
	while (keys[slot] != UINT32_MAX) {
		if (keys[slot] == key) return hits[slot] < limit;
		slot = (slot + 1) & (COUNTER_SLOTS - 1);
	}
	return true;
}

static void counter_add(uint32_t *keys, uint32_t *hits, uint32_t key) {
	uint32_t slot = (key * UINT32_C(2654435761)) & (COUNTER_SLOTS - 1);
	while (keys[slot] != UINT32_MAX) {
		if (keys[slot] == key) {
			hits[slot]++;
			return;
		}
		slot = (slot + 1) & (COUNTER_SLOTS - 1);
	}
	keys[slot] = key;
	hits[slot] = 1;
}

static int order_compare(const void *a, const void *b) {
	const order_t *x = a, *y = b;
	return x->group != y->group ? (x->group > y->group ? 1 : -1) : strcmp(x->text, y->text);
}

geniusbuild_result_t geniusbuild_run(const char *path, const atomic_bool *cancel, geniusbuild_progress_cb progress, void *user) {
	build_t b = {.cancel = cancel, .progress = progress, .user = user, .result = GENIUSBUILD_OK};
	order_t *order = NULL;
	uint16_t *targets = NULL, *buckets = NULL;
	uint32_t *groups = NULL, *counters = NULL;
	unsigned char *counts = NULL;
	bool published = false;
	char parent[512];
	struct stat directory;
	char old[4097] = "", input[SHA256_HEX_LEN], graph[SHA256_HEX_LEN], generation[512];
	report(&b, GENIUSBUILD_CHECKING, 0, 0);
	if (!path || !path[0]) return GENIUSBUILD_INVALID;
	if (cancelled(&b)) return GENIUSBUILD_CANCELLED;
	const char *slash = strrchr(path, '/');
	if (!slash || slash == path || strlen(path) >= sizeof(parent) || strcmp(slash + 1, "genius.db")) return GENIUSBUILD_INVALID;
	snprintf(parent, sizeof(parent), "%.*s", (int)(slash - path), path);
	// Pin the card before collecting or ranking, not only when writing starts.
	int dir = open(parent, O_RDONLY | O_DIRECTORY);
	if (dir < 0) return GENIUSBUILD_IO;
	if (fstat(dir, &directory) != 0) { close(dir); return GENIUSBUILD_IO; }
	unlinkat(dir, STAGING "-journal", 0);
	unlinkat(dir, STAGING, 0);
	// First walk: the distinct tags, the row count and the path-pool size.
	if (!library_read_tags(survey, &b, cancel, &b.revision)) {
		if (b.result == GENIUSBUILD_OK) b.result = GENIUSBUILD_INVALID;
		goto done;
	}
	b.survey_rows = b.rows_seen;
	if (!dict_finish(&b, &b.artists) || !dict_finish(&b, &b.albums) || !dict_finish(&b, &b.genres)) goto done;
	b.empty_artist = dict_find(&b.artists, "", 0);
	b.empty_album = dict_find(&b.albums, "", 0);
	if (cancelled(&b)) goto done;
	b.text_cap = b.survey_path_bytes + 1;
	if (!reserve(&b, b.text_cap)) goto done;
	b.text = calloc(b.text_cap, 1);
	if (!b.text) { b.result = GENIUSBUILD_NOMEM; goto done; }
	b.tracks = allocate(&b, b.survey_tracks, sizeof(*b.tracks));
	if (!b.tracks) goto done;
	// Second walk: one record and one path per track.
	b.rows_seen = 0;
	unsigned revision = 0;
	if (!library_read_tags(collect, &b, cancel, &revision)) {
		if (b.result == GENIUSBUILD_OK) b.result = GENIUSBUILD_INVALID;
		goto done;
	}
	if (revision != b.revision || b.rows_seen != b.survey_rows || b.count != b.survey_tracks) {
		b.result = GENIUSBUILD_CHANGED;
		goto done;
	}
	if (!sort_rows(&b, b.tracks, b.count, sizeof(*b.tracks), path_compare)) goto done;
	for (uint32_t i = 1; i < b.count; i++) {
		if (!path_compare(&b.tracks[i - 1], &b.tracks[i])) { b.result = GENIUSBUILD_INVALID; goto done; }
	}
	input_digest(&b, input);
	if (cancelled(&b)) goto done;
	// The tag strings have done their work: the digest names them and the ids
	// stand in for them from here on.
	dict_free(&b, &b.genres);
	dict_free(&b, &b.albums);
	dict_free(&b, &b.artists);
	bool have = read_generation(&b, path, old);
	if (have && field_matches(old, "algorithm", "sonix-genius-1") && field_matches(old, "input_sha256", input)) {
		b.result = library_revision(LIBRARY_LIST_TRACKS) == revision ? GENIUSBUILD_CURRENT : GENIUSBUILD_CHANGED;
		goto done;
	}
	uint32_t clusters = 0;
	for (uint32_t i = 0; i < b.count; i++) {
		if (b.tracks[i].cluster + 1 > clusters) clusters = b.tracks[i].cluster + 1;
	}
	size_t pool = 0;
	for (uint32_t i = 0; i < b.count; i++) pool += strlen(b.tracks[i].path) + 5;
	uint32_t top = neighbour_limit(&b, pool, clusters);
	if (!top) goto done;
	order = allocate(&b, b.count, sizeof(*order));
	targets = allocate(&b, (size_t)b.count * top, sizeof(*targets));
	buckets = allocate(&b, (size_t)BUCKETS * BUCKET_ENTRIES, sizeof(*buckets));
	groups = allocate(&b, (size_t)b.count + 1, sizeof(*groups));
	counts = allocate(&b, b.count, 1);
	counters = allocate(&b, (size_t)4 * COUNTER_SLOTS, sizeof(*counters));
	if (!order || !targets || !buckets || !groups || !counts || !counters) goto done;
	for (uint32_t i = 0; i < b.count; i++) {
		order[i] = (order_t){b.tracks[i].path, i, b.tracks[i].cluster};
		groups[b.tracks[i].cluster + 1]++;
	}
	for (uint32_t i = 1; i < b.count; i++) groups[i] += groups[i - 1];
	if (!sort_rows(&b, order, b.count, sizeof(*order), order_compare)) goto done;
	uint32_t *artist_keys = counters, *artist_hits = counters + COUNTER_SLOTS;
	uint32_t *album_keys = counters + 2 * COUNTER_SLOTS, *album_hits = counters + 3 * COUNTER_SLOTS;
	// An empty tag is an ordinary value, but it never scores: the reference
	// producer tests the tag's first byte before comparing ids.
	uint32_t edges = 0;
	report(&b, GENIUSBUILD_RANKING, 0, (int)b.count);
	for (uint32_t i = 0; i < b.count; i++) {
		if (cancelled(&b)) goto done;
		track_t *seed = &b.tracks[i];
		uint32_t first = groups[seed->cluster], end = groups[seed->cluster + 1];
		uint32_t size = end - first;
		// Clusters at or below the scan bound are ranked exactly; a larger one
		// is walked with a fixed stride so one enormous genre cannot turn the
		// build into hours of work. The stride keeps the whole cluster's
		// breadth, which a prefix would not.
		uint32_t step = size > GENIUSBUILD_MAX_SCAN ? (size + GENIUSBUILD_MAX_SCAN - 1) / GENIUSBUILD_MAX_SCAN : 1;
		uint32_t picked[BUCKETS] = {0};
		for (uint32_t j = first; j < end; j += step) {
			if (!(j & 31) && cancelled(&b)) goto done;
			uint32_t id = order[j].id;
			track_t *other = &b.tracks[id];
			unsigned score = (seed->album_id != (uint32_t)b.empty_album && seed->album_id == other->album_id ? 3 : 0) +
				(seed->artist_id != (uint32_t)b.empty_artist && seed->artist_id == other->artist_id ? 2 : 0) +
				(seed->year && seed->year == other->year ? 1 : 0);
			if (picked[score] < BUCKET_ENTRIES) {
				buckets[(size_t)score * BUCKET_ENTRIES + picked[score]++] = (uint16_t)id;
			}
		}
		// Constant-time caps, and the same (-score, raw UTF-8 path) order as
		// the reference producer.
		memset(artist_keys, 0xff, sizeof(uint32_t) * COUNTER_SLOTS);
		memset(album_keys, 0xff, sizeof(uint32_t) * COUNTER_SLOTS);
		memset(artist_hits, 0, sizeof(uint32_t) * COUNTER_SLOTS);
		memset(album_hits, 0, sizeof(uint32_t) * COUNTER_SLOTS);
		for (int score = 6; score >= 0 && counts[i] < top; score--) {
			for (uint32_t k = 0; k < picked[score] && counts[i] < top; k++) {
				if (!(k & 31) && cancelled(&b)) goto done;
				uint32_t id = buckets[(size_t)score * BUCKET_ENTRIES + k];
				if (id == i) continue;
				track_t *other = &b.tracks[id];
				if (!counter_allows(artist_keys, artist_hits, other->artist_id, 2) ||
					!counter_allows(album_keys, album_hits, other->album_id, 3)) {
					continue;
				}
				counter_add(artist_keys, artist_hits, other->artist_id);
				counter_add(album_keys, album_hits, other->album_id);
				targets[(size_t)i * top + counts[i]++] = (uint16_t)id;
			}
		}
		edges += counts[i];
		report(&b, GENIUSBUILD_RANKING, (int)i + 1, (int)b.count);
	}
	uint32_t sizes[7];
	size_t section_bytes = 0;
	sizes[0] = (uint32_t)(((size_t)b.count + 1) * 4);
	sizes[1] = (uint32_t)pool;
	sizes[2] = (uint32_t)((size_t)b.count * 36);
	sizes[3] = (uint32_t)((size_t)b.count * 8);
	sizes[4] = (uint32_t)((size_t)edges * 8);
	sizes[5] = (uint32_t)((size_t)clusters * 12);
	sizes[6] = (uint32_t)((size_t)clusters * 4);
	for (unsigned i = 0; i < 7; i++) section_bytes += sizes[i];
	if (section_bytes + HEADER_BYTES + sizeof(generation) > GENIUSBUILD_MAX_BLOB_BYTES) {
		b.result = GENIUSBUILD_LIMIT;
		goto done;
	}
	sections_t sections = {.count = b.count, .top = top, .edges = edges, .clusters = clusters, .pool = (uint32_t)pool,
		.tracks = b.tracks, .counts = counts, .targets = targets};
	memcpy(sections.sizes, sizes, sizeof(sizes));
	sha256_ctx hash;
	sha256_init(&hash);
	unsigned char word[20];
	uint32_t values[] = {b.count, edges, clusters, clusters, (uint32_t)pool};
	for (unsigned i = 0; i < 5; i++) put32(word + i * 4, values[i]);
	sha256_update(&hash, word, 20);
	for (unsigned i = 0; i < 7; i++) {
		put32(word, sizes[i]);
		sha256_update(&hash, word, 4);
		if (!emit_sections(&sections, 1u << i, b.cancel, digest_sink, &hash)) goto done;
	}
	sha256_final_hex(&hash, graph);
	if (have && field_matches(old, "producer", "export_genius_db.py") && field_matches(old, "algorithm", "sonix-genius-1") && field_matches(old, "sha256", graph)) {
		b.result = library_revision(LIBRARY_LIST_TRACKS) == revision ? GENIUSBUILD_CURRENT : GENIUSBUILD_CHANGED;
		goto done;
	}
	if (cancelled(&b)) goto done;
	snprintf(generation, sizeof(generation), "{\"algorithm\":\"sonix-genius-1\",\"input_sha256\":\"%s\",\"producer\":\"sonix-geniusbuild-1\",\"schema\":\"1\",\"sha256\":\"%s\",\"source\":\"library-tags-v1\",\"tracks\":%u}", input, graph, b.count);
	size_t total = HEADER_BYTES + strlen(generation) + section_bytes;
	report(&b, GENIUSBUILD_PUBLISHING, (int)b.count, (int)b.count);
	b.result = publish(&b, dir, parent, &directory, revision, generation, total, &sections);
	published = b.result == GENIUSBUILD_OK;
	if (b.result == GENIUSBUILD_OK) printf("geniusbuild: published %u tracks, %u edges, %u genres, %zu bytes; input %s\n", b.count, edges, clusters, total, input);
done:
	if (cancelled(&b) && !published) b.result = GENIUSBUILD_CANCELLED;
	free(counters);
	free(counts);
	free(groups);
	free(buckets);
	free(targets);
	free(order);
	dict_free(&b, &b.genres);
	dict_free(&b, &b.albums);
	dict_free(&b, &b.artists);
	free(b.tracks);
	free(b.text);
	close(dir);
	printf("geniusbuild: result %d, %u tracks, retained peak %zu bytes\n", b.result, b.count, b.peak);
	return b.result;
}
