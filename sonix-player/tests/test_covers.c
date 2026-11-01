#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "src/gui/nowplaying/cover.h"
#include "src/gui/nowplaying/coverloader.h"
#include "src/system/db/sqlite3.h"
#include "src/system/decode/apedec.h"
#include "src/system/decode/decode.h"
#include "src/system/device/power.h"
#include "src/system/image/art_shrink.h"
#include "src/system/library/albumart.h"
#include "src/system/tagmanager/tagwrite.h"
#include "src/system/streaming/podcastcache.h"
#include "src/system/streaming/subsonicstream.h"

bool podcastcache_ensure_cover(const char *path) {
	(void)path;
	return false;
}

// Allow albumart.c's own SIGBUS card-removal handler to run under AddressSanitizer.
const char *__asan_default_options(void);
const char *__asan_default_options(void) { return "handle_sigbus=0"; }

bool power_screen_is_on(void) { return true; }

bool subsonicstream_owns(const char *path) {
	(void)path;
	return false;
}
streambridge_t *subsonicstream_acquire(const char *path, int *duration, int *bitrate) {
	(void)path;
	(void)duration;
	(void)bitrate;
	return NULL;
}
void subsonicstream_release(streambridge_t *sb) { (void)sb; }

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

typedef struct {
	uint8_t *data;
	size_t len;
	size_t cap;
} buf_t;

static void buf_put(buf_t *b, const void *src, size_t n) {
	if (b->len + n > b->cap) {
		size_t ncap = b->cap ? b->cap * 2 : 512;
		while (ncap < b->len + n)
			ncap *= 2;
		uint8_t *p = realloc(b->data, ncap);
		assert(p != NULL);
		b->data = p;
		b->cap = ncap;
	}
	memcpy(b->data + b->len, src, n);
	b->len += n;
}

static void buf_u8(buf_t *b, uint8_t v) { buf_put(b, &v, 1); }
static void buf_be24(buf_t *b, uint32_t v) {
	uint8_t p[3] = {(uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v};
	buf_put(b, p, 3);
}
static void buf_be32(buf_t *b, uint32_t v) {
	uint8_t p[4] = {(uint8_t)(v >> 24), (uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v};
	buf_put(b, p, 4);
}
static void buf_le32(buf_t *b, uint32_t v) {
	uint8_t p[4] = {(uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24)};
	buf_put(b, p, 4);
}
static void buf_syncsafe32(buf_t *b, uint32_t v) {
	uint8_t p[4] = {(uint8_t)((v >> 21) & 0x7F), (uint8_t)((v >> 14) & 0x7F), (uint8_t)((v >> 7) & 0x7F),
					(uint8_t)(v & 0x7F)};
	buf_put(b, p, 4);
}

static void write_file_bytes(const char *path, const uint8_t *data, size_t len) {
	FILE *f = fopen(path, "wb");
	assert(f != NULL);
	if (len > 0)
		assert(fwrite(data, 1, len, f) == len);
	assert(fclose(f) == 0);
}

static uint8_t *make_test_jpeg(int w, int h, uint8_t r, uint8_t g, uint8_t b, uint32_t seed, size_t *out_len) {
	uint8_t *rgb = malloc((size_t)w * h * 3);
	assert(rgb != NULL);
	for (int y = 0; y < h; y++) {
		for (int x = 0; x < w; x++) {
			size_t idx = ((size_t)y * w + x) * 3;
			uint8_t noise = (uint8_t)(((uint32_t)(x * 31 + y * 17) ^ seed) & 0x3F);
			rgb[idx + 0] = (uint8_t)(r ^ noise);
			rgb[idx + 1] = (uint8_t)(g ^ (noise >> 1));
			rgb[idx + 2] = (uint8_t)(b ^ (noise << 1));
		}
	}
	uint8_t *jpg = art_encode_jpeg(rgb, w, h, 92, out_len);
	free(rgb);
	assert(jpg != NULL && *out_len > 16);
	return jpg;
}

static void base64_encode_into(buf_t *out, const uint8_t *src, size_t len) {
	static const char tbl[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
	for (size_t i = 0; i < len; i += 3) {
		uint32_t v = (uint32_t)src[i] << 16;
		if (i + 1 < len)
			v |= (uint32_t)src[i + 1] << 8;
		if (i + 2 < len)
			v |= src[i + 2];
		char q[4] = {
			tbl[(v >> 18) & 0x3F],
			tbl[(v >> 12) & 0x3F],
			(i + 1 < len) ? tbl[(v >> 6) & 0x3F] : '=',
			(i + 2 < len) ? tbl[v & 0x3F] : '=',
		};
		buf_put(out, q, 4);
	}
}

static void append_ogg_page(buf_t *out, uint8_t header_type, uint32_t serial, uint32_t seq, const uint8_t *payload,
							size_t len, bool packet_ends) {
	size_t full = len / 255;
	size_t rem = len % 255;
	size_t nsegs = (rem == 0) ? (packet_ends ? full + 1 : full) : (full + 1);
	assert(nsegs <= 255);
	buf_put(out, "OggS", 4);
	buf_u8(out, 0);
	buf_u8(out, header_type);
	buf_le32(out, 0);
	buf_le32(out, 0);
	buf_le32(out, serial);
	buf_le32(out, seq);
	buf_le32(out, 0);
	buf_u8(out, (uint8_t)nsegs);
	for (size_t i = 0; i < full; i++)
		buf_u8(out, 255);
	if (rem == 0 && packet_ends)
		buf_u8(out, 0);
	else if (rem > 0)
		buf_u8(out, (uint8_t)rem);
	buf_put(out, payload, len);
}

// ---------------------------------------------------------------------------
// 1. Oversized (>8 MB ALBUMART_MAX_BYTES) cover mapped from SD card + FLAC+ID3
// ---------------------------------------------------------------------------
static void test_oversized_mapped_covers(const char *dir) {
	size_t jpg_len = 0;
	uint8_t *jpg = make_test_jpeg(64, 64, 220, 60, 40, 1, &jpg_len);

	// Pad a valid baseline JPEG past ALBUMART_MAX_BYTES (9.5 MB > 8 MB!) by
	// inserting合法 JPEG COM (0xFF 0xFE) comment segments right after SOI (0xFFD8)
	// so TJpgDec / stb_image walks over the 9.5 MB markers straight to SOF/SOS!
	buf_t huge_jpg = {0};
	buf_put(&huge_jpg, jpg, 2); // FF D8
	uint8_t com_block[60000];
	memset(com_block, 'X', sizeof(com_block));
	for (int i = 0; i < 165; i++) { // 165 * 60004 ~= 9.9 MB > 8 MB!
		buf_u8(&huge_jpg, 0xFF);
		buf_u8(&huge_jpg, 0xFE);
		uint16_t clen = (uint16_t)(sizeof(com_block) + 2);
		buf_u8(&huge_jpg, (uint8_t)(clen >> 8));
		buf_u8(&huge_jpg, (uint8_t)clen);
		buf_put(&huge_jpg, com_block, sizeof(com_block));
	}
	buf_put(&huge_jpg, jpg + 2, jpg_len - 2);
	free(jpg);
	assert(huge_jpg.len > ALBUMART_MAX_BYTES + 1024 * 1024);

	// Embed this 9.9 MB JPEG inside a FLAC PICTURE block AFTER an ID3v2 header!
	char flac_path[512];
	snprintf(flac_path, sizeof(flac_path), "%s/huge_cover.flac", dir);
	buf_t flac = {0};
	// Leading ID3v2 header
	buf_put(&flac, "ID3\x03\x00\x00", 6);
	buf_syncsafe32(&flac, 10);
	buf_put(&flac, "\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00", 10);
	// fLaC + STREAMINFO + PICTURE
	buf_put(&flac, "fLaC", 4);
	buf_u8(&flac, 0);
	buf_be24(&flac, 34);
	uint8_t si[34] = {0};
	buf_put(&flac, si, 34);

	buf_t pic = {0};
	buf_be32(&pic, 3); // Front cover
	buf_be32(&pic, 10);
	buf_put(&pic, "image/jpeg", 10);
	buf_be32(&pic, 0); // desc len
	buf_be32(&pic, 64);
	buf_be32(&pic, 64);
	buf_be32(&pic, 24);
	buf_be32(&pic, 0);
	buf_be32(&pic, (uint32_t)huge_jpg.len);
	buf_put(&pic, huge_jpg.data, huge_jpg.len);

	buf_u8(&flac, 0x80 | 6); // last=1, PICTURE (6)
	buf_be24(&flac, (uint32_t)pic.len);
	buf_put(&flac, pic.data, pic.len);
	write_file_bytes(flac_path, flac.data, flac.len);
	free(pic.data);
	free(flac.data);
	free(huge_jpg.data);

	// Verify albumart maps it via mmap (>8 MB) and cover_load_for_file decodes it!
	albumart_t art;
	assert(albumart_load_for_file(flac_path, &art));
	assert(art.size > ALBUMART_MAX_BYTES);
	assert(art.map != NULL);
	assert(!albumart_faulted(&art));
	albumart_free(&art);

	cover_image_t img;
	assert(cover_load_for_file(flac_path, 48, 48, COVER_FIT_COVER, &img));
	assert(img.pixels != NULL && img.dsc.header.w == 48 && img.dsc.header.h == 48);
	cover_free(&img);
}

// ---------------------------------------------------------------------------
// 2. Real SIGBUS recovery when an SD-card file vanishes/truncates mid-read
// ---------------------------------------------------------------------------
static void test_sigbus_card_removal_guard(const char *dir) {
	size_t jpg_len = 0;
	uint8_t *jpg = make_test_jpeg(64, 64, 30, 180, 90, 2, &jpg_len);

	// Pad to >64 KB so truncating to 0 guarantees unmapped pages fault with SIGBUS
	char folder[512], cover_path[540];
	snprintf(folder, sizeof(folder), "%s/sigbus_album", dir);
	assert(mkdir(folder, 0755) == 0);
	snprintf(cover_path, sizeof(cover_path), "%s/cover.jpg", folder);

	buf_t padded = {0};
	buf_put(&padded, jpg, jpg_len);
	uint8_t zeros[65536] = {0};
	buf_put(&padded, zeros, sizeof(zeros));
	write_file_bytes(cover_path, padded.data, padded.len);
	free(padded.data);
	free(jpg);

	albumart_t art;
	assert(albumart_load_for_dir(folder, &art));
	assert(art.map != NULL);
	assert(!albumart_faulted(&art));

	// Simulate SD card disappearing underneath the mmap: truncate backing file to 0!
	assert(truncate(cover_path, 0) == 0);

	// Touching art.data raises SIGBUS -> handler swaps mapping for anonymous zero
	// pages and sets faulted=true without killing the process!
	volatile uint8_t b0 = art.data[0];
	volatile uint8_t b1 = art.data[art.size - 1];
	(void)b0;
	(void)b1;
	assert(albumart_faulted(&art));
	albumart_free(&art);
}

// ---------------------------------------------------------------------------
// 3. Streaming Ogg METADATA_BLOCK_PICTURE + APEv2 cover_at (.wv)
// ---------------------------------------------------------------------------
static void test_ogg_and_wv_embedded_covers(const char *dir) {
	size_t jpg_len = 0;
	uint8_t *jpg = make_test_jpeg(48, 48, 40, 90, 220, 3, &jpg_len);

	// (a) Ogg Opus with base64 METADATA_BLOCK_PICTURE split across pages
	buf_t pic_bin = {0};
	buf_be32(&pic_bin, 3);
	buf_be32(&pic_bin, 10);
	buf_put(&pic_bin, "image/jpeg", 10);
	buf_be32(&pic_bin, 0);
	buf_be32(&pic_bin, 48);
	buf_be32(&pic_bin, 48);
	buf_be32(&pic_bin, 24);
	buf_be32(&pic_bin, 0);
	buf_be32(&pic_bin, (uint32_t)jpg_len);
	buf_put(&pic_bin, jpg, jpg_len);

	buf_t b64 = {0};
	const char *prefix = "METADATA_BLOCK_PICTURE=";
	buf_put(&b64, prefix, strlen(prefix));
	base64_encode_into(&b64, pic_bin.data, pic_bin.len);
	free(pic_bin.data);

	buf_t opus_tags = {0};
	buf_put(&opus_tags, "OpusTags", 8);
	buf_le32(&opus_tags, 5);
	buf_put(&opus_tags, "sonix", 5);
	buf_le32(&opus_tags, 1);
	buf_le32(&opus_tags, (uint32_t)b64.len);
	buf_put(&opus_tags, b64.data, b64.len);
	free(b64.data);

	buf_t ogg = {0};
	uint8_t opus_head[19] = {'O', 'p', 'u', 's', 'H', 'e', 'a', 'd', 1, 2, 0, 0, 0x80, 0xBB, 0, 0, 0, 0, 0};
	append_ogg_page(&ogg, 0x02, 0x99, 0, opus_head, sizeof(opus_head), true);
	assert(opus_tags.len > 255);
	size_t first_page = (opus_tags.len / 2 / 255) * 255;
	if (first_page == 0)
		first_page = 255;
	append_ogg_page(&ogg, 0x00, 0x99, 1, opus_tags.data, first_page, false);
	append_ogg_page(&ogg, 0x01, 0x99, 2, opus_tags.data + first_page, opus_tags.len - first_page, true);
	free(opus_tags.data);

	char opus_path[512];
	snprintf(opus_path, sizeof(opus_path), "%s/art.opus", dir);
	write_file_bytes(opus_path, ogg.data, ogg.len);
	free(ogg.data);

	cover_image_t img;
	assert(cover_load_for_file(opus_path, 32, 32, COVER_FIT_COVER, &img));
	assert(img.pixels != NULL && img.dsc.header.w == 32);
	cover_free(&img);

	// (b) WavPack (.wv) with APEv2 binary "Cover Art (Front)" = "cover.jpg\0" + JPEG
	char wv_path[512];
	snprintf(wv_path, sizeof(wv_path), "%s/art.wv", dir);
	buf_t wv = {0};
	buf_put(&wv, "wvpk", 4);
	uint8_t pad[32] = {0};
	buf_put(&wv, pad, sizeof(pad));

	const char *key = "Cover Art (Front)";
	const char *fname = "cover.jpg";
	uint32_t val_len = (uint32_t)(strlen(fname) + 1 + jpg_len);
	uint32_t tag_size = (uint32_t)(8 + strlen(key) + 1 + val_len + 32);

	buf_put(&wv, "APETAGEX", 8);
	buf_le32(&wv, 2000);
	buf_le32(&wv, tag_size);
	buf_le32(&wv, 1);
	buf_le32(&wv, 0x80000000u | 0x20000000u);
	buf_le32(&wv, 0);
	buf_le32(&wv, 0);

	buf_le32(&wv, val_len);
	buf_le32(&wv, 2); // binary item (bits 2..1 == 1 -> value 2)
	buf_put(&wv, key, strlen(key) + 1);
	buf_put(&wv, fname, strlen(fname) + 1);
	buf_put(&wv, jpg, jpg_len);

	buf_put(&wv, "APETAGEX", 8);
	buf_le32(&wv, 2000);
	buf_le32(&wv, tag_size);
	buf_le32(&wv, 1);
	buf_le32(&wv, 0x80000000u);
	buf_le32(&wv, 0);
	buf_le32(&wv, 0);
	write_file_bytes(wv_path, wv.data, wv.len);
	free(wv.data);
	free(jpg);

	int64_t off = 0;
	uint32_t sz = 0;
	assert(apedec_cover_at(wv_path, &off, &sz));
	assert(sz == (uint32_t)jpg_len);
	assert(cover_load_for_file(wv_path, 32, 32, COVER_FIT_COVER, &img));
	assert(img.pixels != NULL && img.dsc.header.w == 32);
	cover_free(&img);
}

// ---------------------------------------------------------------------------
// 4. 1000-cover JPEG cache + LRU eviction + prefetch + V1 backdrop + id_worker
// ---------------------------------------------------------------------------
static void test_1000_cover_cache_and_loader(const char *dir) {
	cover_set_cache_dir(dir);

	// Make a small FLAC with embedded JPEG to exercise cover_prefetch_player +
	// cached cover_load_player_images + V1 backdrop_from_bottom
	size_t jpg_len = 0;
	uint8_t *jpg = make_test_jpeg(48, 48, 200, 120, 40, 99, &jpg_len);
	char track_path[512];
	snprintf(track_path, sizeof(track_path), "%s/cached_track.flac", dir);

	buf_t flac = {0};
	buf_put(&flac, "fLaC", 4);
	buf_u8(&flac, 0);
	buf_be24(&flac, 34);
	uint8_t si[34] = {0};
	buf_put(&flac, si, 34);
	buf_t pic = {0};
	buf_be32(&pic, 3);
	buf_be32(&pic, 10);
	buf_put(&pic, "image/jpeg", 10);
	buf_be32(&pic, 0);
	buf_be32(&pic, 48);
	buf_be32(&pic, 48);
	buf_be32(&pic, 24);
	buf_be32(&pic, 0);
	buf_be32(&pic, (uint32_t)jpg_len);
	buf_put(&pic, jpg, jpg_len);
	buf_u8(&flac, 0x80 | 6);
	buf_be24(&flac, (uint32_t)pic.len);
	buf_put(&flac, pic.data, pic.len);
	write_file_bytes(track_path, flac.data, flac.len);
	free(pic.data);
	free(flac.data);
	free(jpg);

	// Prefetch warms player_covers + player_cover_ids as JPEG
	cover_prefetch_player(track_path, 40, 40);

	// Enable V1 Immersive bottom-band backdrop mode and load from cache!
	cover_set_backdrop_upright(true);
	cover_set_backdrop_from_bottom(true);
	cover_set_backdrop_band_brightness(68);
	cover_image_t cov, bd;
	assert(cover_load_player_images(track_path, 40, 40, &cov, 40, 16, &bd));
	assert(cov.pixels != NULL && cov.dsc.header.w == 40 && cov.dsc.header.h == 40);
	assert(bd.pixels != NULL && bd.dsc.header.w == 40 && bd.dsc.header.h == 16);
	cover_free(&cov);
	cover_free(&bd);
	cover_set_backdrop_from_bottom(false);
	cover_set_backdrop_upright(false);

	// Verify DB holds a JPEG blob (starts with 0xFF 0xD8, much smaller than 40*40*2=3200 bytes)
	char db_path[560];
	snprintf(db_path, sizeof(db_path), "%s/.local/thumbnails", dir);
	sqlite3 *db = NULL;
	assert(sqlite3_open(db_path, &db) == SQLITE_OK);
	sqlite3_stmt *stmt = NULL;
	assert(sqlite3_prepare_v2(db, "SELECT pixels FROM player_covers LIMIT 1", -1, &stmt, NULL) == SQLITE_OK);
	assert(sqlite3_step(stmt) == SQLITE_ROW);
	const uint8_t *blob = sqlite3_column_blob(stmt, 0);
	int blob_bytes = sqlite3_column_bytes(stmt, 0);
	assert(blob_bytes > 2 && blob_bytes < 40 * 40 * 2);
	assert(blob[0] == 0xFF && blob[1] == 0xD8);
	sqlite3_finalize(stmt);
	sqlite3_close(db);

	// Store 1005 distinct cover sizes/tracks -> verify table caps at 1000 rows!
	// Calling cover_prefetch_player at 1005 distinct (w, h) pairs for the same track
	// inserts 1005 keys into player_covers and prunes down to <= 1000!
	for (int i = 0; i < 1005; i++) {
		int w = 16 + (i % 32);
		int h = 16 + (i / 32);
		cover_prefetch_player(track_path, w, h);
	}
	assert(sqlite3_open(db_path, &db) == SQLITE_OK);
	assert(sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM player_covers", -1, &stmt, NULL) == SQLITE_OK);
	assert(sqlite3_step(stmt) == SQLITE_ROW);
	int row_count = sqlite3_column_int(stmt, 0);
	sqlite3_finalize(stmt);
	sqlite3_close(db);
	assert(row_count == 1000);

	// Verify coverloader's dedicated id_worker answers coverloader_request_player_id
	coverloader_start();
	coverloader_request_player_id(track_path);
	uint64_t async_id = 0;
	bool finished = false;
	for (int tries = 0; tries < 200 && !finished; tries++) {
		coverloader_take_player_id(&async_id, &finished);
		if (!finished) {
			struct timespec ts = {0, 5 * 1000 * 1000};
			nanosleep(&ts, NULL);
		}
	}
	assert(finished);
	assert(async_id != 0 && async_id == cover_source_id(track_path));
}

// The written front cover is found by the real embedded-art reader and
// decoded by the same GUI cover path used on the player.
static void test_tagwriter_covers(const char *dir) {
    char path[512]; size_t len=0; uint8_t *jpg=make_test_jpeg(64,64,180,40,120,1,&len); assert(jpg);
    for(int mp3=0;mp3<2;mp3++) {
        snprintf(path,sizeof(path),"%s/written.%s",dir,mp3?"mp3":"flac");
        buf_t f={0};
        if(mp3) { buf_put(&f,"ID3\x03\x00\x00",6); buf_syncsafe32(&f,0); buf_put(&f,"\xff\xfb\x90\x00",4); }
        else { buf_put(&f,"fLaC",4); buf_u8(&f,0x80); buf_be24(&f,34); uint8_t info[34]={0}; buf_put(&f,info,34); buf_put(&f,"audio",5); }
        write_file_bytes(path,f.data,f.len); free(f.data);
        assert(tagwrite_cover(path,jpg,len,64,64)==TAGWRITE_OK);
        albumart_t art; assert(albumart_load_candidate(path,0,&art));
        assert(art.size==len && !memcmp(art.data,jpg,len)); albumart_free(&art);
        cover_image_t img; assert(cover_load_for_file(path,48,48,COVER_FIT_CONTAIN,&img));
        assert(img.pixels); cover_free(&img);
    }
    free(jpg);
}

int main(void) {
	char dir[] = "/tmp/sonix-covers-test-XXXXXX";
	assert(mkdtemp(dir) != NULL);

	test_tagwriter_covers(dir);
	test_oversized_mapped_covers(dir);
	test_sigbus_card_removal_guard(dir);
	test_ogg_and_wv_embedded_covers(dir);
	test_1000_cover_cache_and_loader(dir);

	char cmd[600];
	snprintf(cmd, sizeof(cmd), "rm -rf '%s'", dir);
	int rc = system(cmd);
	(void)rc;

	printf("PASS covers (>8MB mmap cover, SIGBUS card-pull guard, Ogg/APEv2 covers, 1000-JPEG cache & id_worker)\n");
	return 0;
}
