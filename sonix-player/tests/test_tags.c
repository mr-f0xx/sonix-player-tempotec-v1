#include <assert.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "src/system/core/config.h"
#include "src/system/decode/apedec.h"
#include "src/system/decode/decode.h"
#include "src/system/decode/opusdec.h"
#include "src/system/decode/wavpackdec.h"
#include "src/system/library/cue.h"
#include "src/system/library/metadata.h"
#include "src/system/tagmanager/tagwrite.h"
#include "src/system/library/oggtags.h"
#include "src/system/library/vorbischap.h"
#include "src/system/streaming/subsonicstream.h"

// Stubs for network/streaming externals not needed for local tag/format tests.
bool subsonicstream_owns(const char *path) {
	(void)path;
	return false;
}

bool subsonicstream_metadata(const char *path, song_metadata_t *out) {
	(void)path;
	(void)out;
	return false;
}

streambridge_t *subsonicstream_acquire(const char *path, int *duration, int *bitrate) {
	(void)path;
	(void)duration;
	(void)bitrate;
	return NULL;
}

void subsonicstream_release(streambridge_t *sb) { (void)sb; }

// Stubs for external static-lib PCM decoders (opusfile/wavpack); tags & covers
// for Opus and WavPack no longer touch either library (oggtags.c & apedec.c).
opusdec_t *opusdec_open(const char *p) {
	(void)p;
	return NULL;
}
opusdec_t *opusdec_open_stream(void *s, int (*r)(void *, unsigned char *, int)) {
	(void)s;
	(void)r;
	return NULL;
}
void opusdec_close(opusdec_t *o) { (void)o; }
int opusdec_channels(const opusdec_t *o) {
	(void)o;
	return 0;
}
int opusdec_sample_rate(const opusdec_t *o) {
	(void)o;
	return 0;
}
uint64_t opusdec_total_frames(const opusdec_t *o) {
	(void)o;
	return 0;
}
uint64_t opusdec_read_s16(opusdec_t *o, uint64_t f, short *out) {
	(void)o;
	(void)f;
	(void)out;
	return 0;
}
bool opusdec_seek(opusdec_t *o, uint64_t f) {
	(void)o;
	(void)f;
	return false;
}

wavpackdec_t *wavpackdec_open(const char *p) {
	(void)p;
	return NULL;
}
void wavpackdec_close(wavpackdec_t *w) { (void)w; }
int wavpackdec_channels(const wavpackdec_t *w) {
	(void)w;
	return 0;
}
int wavpackdec_sample_rate(const wavpackdec_t *w) {
	(void)w;
	return 0;
}
int wavpackdec_bits(const wavpackdec_t *w) {
	(void)w;
	return 0;
}
uint64_t wavpackdec_total_frames(const wavpackdec_t *w) {
	(void)w;
	return 0;
}
uint64_t wavpackdec_read_s16(wavpackdec_t *w, uint64_t f, short *out) {
	(void)w;
	(void)f;
	(void)out;
	return 0;
}
uint64_t wavpackdec_read_s32(wavpackdec_t *w, uint64_t f, int32_t *out) {
	(void)w;
	(void)f;
	(void)out;
	return 0;
}
bool wavpackdec_seek(wavpackdec_t *w, uint64_t f) {
	(void)w;
	(void)f;
	return false;
}

// ---------------------------------------------------------------------------
// Binary builders for real container/tag fixtures
// ---------------------------------------------------------------------------

typedef struct {
	uint8_t *data;
	size_t len;
	size_t cap;
} buf_t;

static void buf_reserve(buf_t *b, size_t add) {
	if (b->len + add <= b->cap)
		return;
	size_t ncap = b->cap ? b->cap * 2 : 256;
	while (ncap < b->len + add)
		ncap *= 2;
	uint8_t *p = realloc(b->data, ncap);
	assert(p != NULL);
	b->data = p;
	b->cap = ncap;
}

static void buf_put(buf_t *b, const void *src, size_t n) {
	buf_reserve(b, n);
	memcpy(b->data + b->len, src, n);
	b->len += n;
}

static void buf_u8(buf_t *b, uint8_t v) { buf_put(b, &v, 1); }

static void buf_be16(buf_t *b, uint16_t v) {
	uint8_t p[2] = {(uint8_t)(v >> 8), (uint8_t)v};
	buf_put(b, p, 2);
}

static void buf_be24(buf_t *b, uint32_t v) {
	uint8_t p[3] = {(uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v};
	buf_put(b, p, 3);
}

static void buf_be32(buf_t *b, uint32_t v) {
	uint8_t p[4] = {(uint8_t)(v >> 24), (uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v};
	buf_put(b, p, 4);
}

static void buf_be64(buf_t *b, uint64_t v) {
	buf_be32(b, (uint32_t)(v >> 32));
	buf_be32(b, (uint32_t)v);
}

static void buf_le16(buf_t *b, uint16_t v) {
	uint8_t p[2] = {(uint8_t)v, (uint8_t)(v >> 8)};
	buf_put(b, p, 2);
}

static void buf_le32(buf_t *b, uint32_t v) {
	uint8_t p[4] = {(uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24)};
	buf_put(b, p, 4);
}

static void buf_syncsafe32(buf_t *b, uint32_t v) {
	uint8_t p[4] = {
		(uint8_t)((v >> 21) & 0x7F),
		(uint8_t)((v >> 14) & 0x7F),
		(uint8_t)((v >> 7) & 0x7F),
		(uint8_t)(v & 0x7F),
	};
	buf_put(b, p, 4);
}

static void write_fixture(const char *path, const uint8_t *data, size_t len) {
	FILE *f = fopen(path, "wb");
	assert(f != NULL);
	if (len > 0) {
		assert(fwrite(data, 1, len, f) == len);
	}
	assert(fclose(f) == 0);
}

static void append_id3v2_text_frame(buf_t *frames, uint8_t version, const char id[4], uint8_t enc, const void *payload,
									size_t payload_len, bool non_syncsafe_v24) {
	buf_put(frames, id, 4);
	uint32_t fsize = (uint32_t)(1 + payload_len);
	if (version >= 4 && !non_syncsafe_v24) {
		buf_syncsafe32(frames, fsize);
	} else {
		buf_be32(frames, fsize);
	}
	buf_be16(frames, 0);
	buf_u8(frames, enc);
	buf_put(frames, payload, payload_len);
}

// Wraps raw bytes into a valid RFC 1950 zlib stream using a stored block so
// tinfl_decompress_mem_to_mem inflates it cleanly with zero external library.
static void make_zlib_stored(buf_t *z, const uint8_t *src, uint16_t len) {
	buf_u8(z, 0x78);
	buf_u8(z, 0x01);
	buf_u8(z, 0x01); // BFINAL=1, BTYPE=00 (stored)
	buf_le16(z, len);
	buf_le16(z, (uint16_t)~len);
	buf_put(z, src, len);
	uint32_t s1 = 1, s2 = 0;
	for (size_t i = 0; i < len; i++) {
		s1 = (s1 + src[i]) % 65521u;
		s2 = (s2 + s1) % 65521u;
	}
	buf_be32(z, (s2 << 16) | s1);
}

static void wrap_id3v2_tag(buf_t *out, uint8_t version, uint8_t flags, const buf_t *frames) {
	buf_put(out, "ID3", 3);
	buf_u8(out, version);
	buf_u8(out, 0);
	buf_u8(out, flags);
	buf_syncsafe32(out, (uint32_t)frames->len);
	buf_put(out, frames->data, frames->len);
}

static void append_id3v1(buf_t *out, const char *title, const char *artist, const char *album, const char *year,
						 uint8_t track, uint8_t genre) {
	uint8_t tag[128];
	memset(tag, 0, sizeof(tag));
	memcpy(tag, "TAG", 3);
	if (title)
		strncpy((char *)tag + 3, title, 30);
	if (artist)
		strncpy((char *)tag + 33, artist, 30);
	if (album)
		strncpy((char *)tag + 63, album, 30);
	if (year)
		strncpy((char *)tag + 93, year, 4);
	tag[125] = 0;
	tag[126] = track;
	tag[127] = genre;
	buf_put(out, tag, sizeof(tag));
}

// Minimal playable 1-channel 44100 Hz 16-bit FLAC stream (verbatim 4-sample frame)
static uint16_t flac_crc16(const uint8_t *data, size_t len) {
	uint16_t crc = 0;
	for (size_t i = 0; i < len; i++) {
		crc ^= (uint16_t)data[i] << 8;
		for (int b = 0; b < 8; b++) {
			crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x8005) : (uint16_t)(crc << 1);
		}
	}
	return crc;
}

static uint8_t flac_crc8(const uint8_t *data, size_t len) {
	uint8_t crc = 0;
	for (size_t i = 0; i < len; i++) {
		crc ^= data[i];
		for (int b = 0; b < 8; b++) {
			crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ 0x07) : (uint8_t)(crc << 1);
		}
	}
	return crc;
}

static void append_flac_stream(buf_t *out, const char *const *comments, size_t n_comments) {
	buf_put(out, "fLaC", 4);

	// STREAMINFO (type 0, length 34)
	bool last = (n_comments == 0);
	buf_u8(out, (uint8_t)((last ? 0x80 : 0x00) | 0));
	buf_be24(out, 34);
	uint8_t si[34];
	memset(si, 0, sizeof(si));
	si[0] = 0;
	si[1] = 16; // min blocksize 16
	si[2] = 0;
	si[3] = 16; // max blocksize 16
	// 44100 Hz (0xAC44), 1 ch (0), 16 bps (15 -> 01111), total samples = 16
	// 20-bit sample rate: 44100 = 0x0AC44 -> si[10]=0x0A, si[11]=0xC4, top4 of si[12]=0x4
	// 3-bit ch-1: 0 -> bits 3..1 of si[12]=0
	// 5-bit bps-1: 15 (0b01111) -> bit 0 of si[12]=0, top4 of si[13]=0xF
	// 36-bit total samples: 16 -> si[17]=16
	si[10] = 0x0A;
	si[11] = 0xC4;
	si[12] = 0x40;
	si[13] = 0xF0;
	si[17] = 16;
	buf_put(out, si, sizeof(si));

	if (n_comments > 0) {
		buf_t vc = {0};
		buf_le32(&vc, 5);
		buf_put(&vc, "sonix", 5);
		buf_le32(&vc, (uint32_t)n_comments);
		for (size_t i = 0; i < n_comments; i++) {
			uint32_t clen = (uint32_t)strlen(comments[i]);
			buf_le32(&vc, clen);
			buf_put(&vc, comments[i], clen);
		}
		buf_u8(out, 0x80 | 4); // last=1, VORBIS_COMMENT
		buf_be24(out, (uint32_t)vc.len);
		buf_put(out, vc.data, vc.len);
		free(vc.data);
	}

	// Single verbatim FLAC audio frame of 16 samples (16-bit mono, 44.1 kHz)
	// Sync 0xFFF8, blocksize=0110 (8-bit blocksize-1 at end of header = 15),
	// samplerate=1001 (44.1 kHz), chan=0000 (1 ch), sample_size=100 (16-bit), res=0
	buf_t frame = {0};
	uint8_t fh[6] = {0xFF, 0xF8, 0x69, 0x08, 0x00, 15};
	buf_put(&frame, fh, sizeof(fh));
	buf_u8(&frame, flac_crc8(frame.data, frame.len));
	// Subframe header: 0 (pad) | 000001 (VERBATIM) | 0 (no wasted bits) = 0x02
	buf_u8(&frame, 0x02);
	for (int s = 0; s < 16; s++) {
		int16_t sample = (int16_t)((s + 1) * 100);
		buf_be16(&frame, (uint16_t)sample);
	}
	uint16_t crc16 = flac_crc16(frame.data, frame.len);
	buf_be16(&frame, crc16);
	buf_put(out, frame.data, frame.len);
	free(frame.data);
}

// Appends one Ogg page with arbitrary payload bytes (auto-segmented into <=255 chunks)
static void append_ogg_page(buf_t *out, uint8_t header_type, uint32_t serial, uint32_t seq, const uint8_t *payload,
							size_t len, bool packet_ends) {
	size_t full = len / 255;
	size_t rem = len % 255;
	size_t nsegs;
	bool need_zero_lace = false;
	if (len == 0) {
		nsegs = 1;
	} else if (rem == 0) {
		if (packet_ends) {
			nsegs = full + 1;
			need_zero_lace = true;
		} else {
			nsegs = full;
		}
	} else {
		nsegs = full + 1;
	}
	assert(nsegs <= 255);

	buf_put(out, "OggS", 4);
	buf_u8(out, 0); // version
	buf_u8(out, header_type);
	buf_le32(out, 0);
	buf_le32(out, 0); // granule
	buf_le32(out, serial);
	buf_le32(out, seq);
	buf_le32(out, 0); // CRC (oggtags does not check CRC)
	buf_u8(out, (uint8_t)nsegs);
	for (size_t i = 0; i < full; i++)
		buf_u8(out, 255);
	if (len == 0 || need_zero_lace) {
		buf_u8(out, 0);
	} else if (rem > 0) {
		buf_u8(out, (uint8_t)rem);
	}
	if (len > 0) {
		buf_put(out, payload, len);
	}
}

static void append_apev2_tag(buf_t *out, const char *const keys[], const char *const vals[], size_t count) {
	buf_t items = {0};
	for (size_t i = 0; i < count; i++) {
		uint32_t vlen = (uint32_t)strlen(vals[i]);
		buf_le32(&items, vlen);
		buf_le32(&items, 0); // UTF-8 text item
		buf_put(&items, keys[i], strlen(keys[i]) + 1);
		buf_put(&items, vals[i], vlen);
	}
	uint32_t tag_size = (uint32_t)(items.len + 32);
	// Header
	buf_put(out, "APETAGEX", 8);
	buf_le32(out, 2000);
	buf_le32(out, tag_size);
	buf_le32(out, (uint32_t)count);
	buf_le32(out, 0x80000000u | 0x20000000u); // header present, this is header
	buf_be64(out, 0);
	// Items
	buf_put(out, items.data, items.len);
	// Footer
	buf_put(out, "APETAGEX", 8);
	buf_le32(out, 2000);
	buf_le32(out, tag_size);
	buf_le32(out, (uint32_t)count);
	buf_le32(out, 0x80000000u); // header present, this is footer
	buf_be64(out, 0);
	free(items.data);
}

// ---------------------------------------------------------------------------
// 1. FLAC with ID3v2 header in front (both metadata & dr_flac playback/seek)
// ---------------------------------------------------------------------------
static void test_flac_with_id3_header(const char *dir) {
	char path[512];
	snprintf(path, sizeof(path), "%s/id3_prefixed.flac", dir);

	buf_t frames = {0};
	const char *id3_title = "ID3 Fallback Title";
	const char *id3_genre = "Electronic";
	append_id3v2_text_frame(&frames, 3, "TIT2", 3, id3_title, strlen(id3_title), false);
	append_id3v2_text_frame(&frames, 3, "TCON", 3, id3_genre, strlen(id3_genre), false);
	append_id3v2_text_frame(&frames, 3, "TYER", 3, "2023", 4, false);
	append_id3v2_text_frame(&frames, 3, "TDAT", 3, "1508", 4, false); // 15 August 2023

	buf_t file = {0};
	wrap_id3v2_tag(&file, 3, 0, &frames);
	const char *vc[] = {
		"TITLE=Native FLAC Title",
		"ARTIST=Boards of Canada",
		"ALBUM=Tomorrow's Harvest",
		"TRACKNUMBER=3",
	};
	append_flac_stream(&file, vc, 4);
	write_fixture(path, file.data, file.len);
	free(frames.data);
	free(file.data);

	// Format sniffing skips ID3v2 and identifies FLAC
	assert(decode_detect_format(path) == DECODE_FORMAT_FLAC);

	// Metadata prefers native Vorbis comments, and fills missing genre + V1 date from ID3v2!
	song_metadata_t meta;
	metadata_read(path, &meta);
	assert(strcmp(meta.title, "Native FLAC Title") == 0);
	assert(strcmp(meta.artist, "Boards of Canada") == 0);
	assert(strcmp(meta.album, "Tomorrow's Harvest") == 0);
	assert(strcmp(meta.genre, "Electronic") == 0);
	assert(meta.track_number == 3);
	assert(meta.year == 2023);
	assert(strcmp(meta.date, "2023-08-15") == 0);

	// Real dr_flac decoder opens, reads PCM, and seeks accurately despite leading ID3v2!
	decoder_t *dec = decoder_open(path, DECODE_FORMAT_FLAC);
	assert(dec != NULL);
	assert(decoder_sample_rate(dec) == 44100);
	assert(decoder_channels(dec) == 1);
	assert(decoder_total_pcm_frames(dec) == 16);
	short pcm[16] = {0};
	uint64_t got = decoder_read_pcm_frames_s16(dec, 16, pcm);
	assert(got == 16);
	assert(pcm[0] == 100 && pcm[15] == 1600);
	assert(decoder_seek_to_frame(dec, 4) != 0);
	memset(pcm, 0, sizeof(pcm));
	got = decoder_read_pcm_frames_s16(dec, 4, pcm);
	assert(got == 4);
	assert(pcm[0] == 500 && pcm[3] == 800);
	decoder_close(dec);
}

// ---------------------------------------------------------------------------
// 2. Broken ID3v2 tags: non-syncsafe v2.4, unsynchronisation, zlib compression,
//    chained ID3v2 tags, UTF-16 without BOM, and V1 tagdate precision
// ---------------------------------------------------------------------------
static void test_broken_id3v2_tags(const char *dir) {
	char path[512];
	snprintf(path, sizeof(path), "%s/broken_id3.mp3", dir);

	// First ID3v2.4 tag:
	// - TIT2 has a >127 byte payload written with a NON-SYNCSAFE 32-bit size (broken tagger!)
	// - TPE1 is zlib-compressed (v2.4 flag 0x0009: compressed + data length indicator)
	// - TALB uses frame-level unsynchronisation (0xFF 0x00 -> 0xFF in UTF-16LE BOM!)
	// - TDRC carries full ISO timestamp for V1 tagdate
	buf_t tag1 = {0};
	char long_title[180];
	memset(long_title, 'A', 150);
	memcpy(long_title, "Broken Syncsafe Size — ", strlen("Broken Syncsafe Size — "));
	long_title[150] = '\0';
	append_id3v2_text_frame(&tag1, 4, "TIT2", 3, long_title, strlen(long_title), true /* non-syncsafe! */);

	// Zlib-compressed TPE1 frame
	const char *artist_raw = "\x03Massive Attack";
	size_t artist_raw_len = 1 + strlen("Massive Attack");
	buf_t z = {0};
	make_zlib_stored(&z, (const uint8_t *)artist_raw, (uint16_t)artist_raw_len);
	buf_put(&tag1, "TPE1", 4);
	buf_syncsafe32(&tag1, (uint32_t)(4 + z.len));
	buf_be16(&tag1, 0x0009); // compressed (0x08) + data length indicator (0x01)
	buf_syncsafe32(&tag1, (uint32_t)artist_raw_len);
	buf_put(&tag1, z.data, z.len);
	free(z.data);

	// Unsynchronised TALB frame: UTF-16LE BOM 0xFF 0xFE written as 0xFF 0x00 0xFE + "Mezzanine"
	buf_t unsync_alb = {0};
	buf_u8(&unsync_alb, 0x01); // UTF-16 with BOM
	buf_u8(&unsync_alb, 0xFF);
	buf_u8(&unsync_alb, 0x00); // unsync stuffed zero!
	buf_u8(&unsync_alb, 0xFE);
	const char *alb = "Mezzanine";
	for (size_t i = 0; i < strlen(alb); i++) {
		buf_u8(&unsync_alb, (uint8_t)alb[i]);
		buf_u8(&unsync_alb, 0x00);
	}
	buf_put(&tag1, "TALB", 4);
	buf_syncsafe32(&tag1, (uint32_t)unsync_alb.len);
	buf_be16(&tag1, 0x0002); // frame unsynchronisation
	buf_put(&tag1, unsync_alb.data, unsync_alb.len);
	free(unsync_alb.data);

	// TDRC with full date
	const char *tdrc = "1998-04-20";
	append_id3v2_text_frame(&tag1, 4, "TDRC", 3, tdrc, strlen(tdrc), false);

	// Second chained ID3v2.3 tag right behind the first (fills missing TCON + TRCK!)
	buf_t tag2 = {0};
	append_id3v2_text_frame(&tag2, 3, "TIT2", 3, "Should Not Override Tag 1", 25, false);
	append_id3v2_text_frame(&tag2, 3, "TCON", 3, "(27)", 4, false); // Trip-Hop
	// UTF-16LE without BOM in TRCK ("03")
	uint8_t trck_nobom[] = {'0', 0, '3', 0};
	append_id3v2_text_frame(&tag2, 3, "TRCK", 1, trck_nobom, sizeof(trck_nobom), false);

	buf_t file = {0};
	wrap_id3v2_tag(&file, 4, 0, &tag1);
	buf_u8(&file, 0); // stray zero padding between chained ID3v2 tags
	buf_u8(&file, 0);
	wrap_id3v2_tag(&file, 3, 0, &tag2);
	append_id3v1(&file, NULL, NULL, NULL, "1998", 9, 0);
	write_fixture(path, file.data, file.len);
	free(tag1.data);
	free(tag2.data);
	free(file.data);

	song_metadata_t meta;
	metadata_read(path, &meta);
	assert(strcmp(meta.title, long_title) == 0);
	assert(strcmp(meta.artist, "Massive Attack") == 0);
	assert(strcmp(meta.album, "Mezzanine") == 0);
	assert(strcmp(meta.genre, "Trip-Hop") == 0);
	assert(meta.track_number == 3);
	assert(meta.year == 1998);
	// V1 tagdate kept full 1998-04-20 precision over ID3v1's bare "1998"
	assert(strcmp(meta.date, "1998-04-20") == 0);
}

// ---------------------------------------------------------------------------
// 3. Wrong extensions + APEv2-only MP3 + CUE sheet mismatched audio extension
// ---------------------------------------------------------------------------
static void test_wrong_extensions_and_cue(const char *dir) {
	// (a) FLAC file misnamed as .mp3
	char flac_as_mp3[512];
	snprintf(flac_as_mp3, sizeof(flac_as_mp3), "%s/misnamed.mp3", dir);
	buf_t flac = {0};
	const char *vc[] = {"TITLE=Secret FLAC", "ARTIST=Aphex Twin", "DATE=2001-10-22"};
	append_flac_stream(&flac, vc, 3);
	write_fixture(flac_as_mp3, flac.data, flac.len);
	free(flac.data);

	assert(decode_detect_format(flac_as_mp3) == DECODE_FORMAT_FLAC);
	song_metadata_t meta;
	metadata_read(flac_as_mp3, &meta);
	assert(strcmp(meta.title, "Secret FLAC") == 0);
	assert(strcmp(meta.artist, "Aphex Twin") == 0);
	assert(strcmp(meta.date, "2001-10-22") == 0);

	// (b) Ogg Opus file misnamed as .ogg, with a giant picture skipped unread
	//     and comments split across multiple Ogg pages!
	char opus_as_ogg[512];
	snprintf(opus_as_ogg, sizeof(opus_as_ogg), "%s/misnamed_opus.ogg", dir);
	buf_t ogg = {0};
	uint8_t opus_head[19] = {'O', 'p', 'u', 's', 'H', 'e', 'a', 'd', 1, 2, 0, 0, 0x80, 0xBB, 0, 0, 0, 0, 0};
	append_ogg_page(&ogg, 0x02, 0x1234, 0, opus_head, sizeof(opus_head), true);

	buf_t tags_pkt = {0};
	buf_put(&tags_pkt, "OpusTags", 8);
	buf_le32(&tags_pkt, 5);
	buf_put(&tags_pkt, "sonix", 5);
	buf_le32(&tags_pkt, 4); // 4 comments: TITLE, huge PICTURE, ARTIST, long LYRICS
	const char *c1 = "TITLE=Opus In Ogg Clothing";
	buf_le32(&tags_pkt, (uint32_t)strlen(c1));
	buf_put(&tags_pkt, c1, strlen(c1));

	// 120 KB fake METADATA_BLOCK_PICTURE=AAAA... (exceeds OGG_TEXT_MAX, skipped unread!)
	size_t pic_val_len = 120 * 1024;
	const char *pic_prefix = "METADATA_BLOCK_PICTURE=";
	buf_le32(&tags_pkt, (uint32_t)(strlen(pic_prefix) + pic_val_len));
	buf_put(&tags_pkt, pic_prefix, strlen(pic_prefix));
	for (size_t i = 0; i < pic_val_len; i++)
		buf_u8(&tags_pkt, 'A');

	const char *c3 = "ARTIST=Burial";
	buf_le32(&tags_pkt, (uint32_t)strlen(c3));
	buf_put(&tags_pkt, c3, strlen(c3));

	// 70 KB LYRICS comment (> OGG_TEXT_MAX 64 KB, read via ogg_long_comment!)
	const char *lyr_prefix = "LYRICS=[00:01.00]Archangel ";
	size_t pad_len = 70 * 1024;
	buf_le32(&tags_pkt, (uint32_t)(strlen(lyr_prefix) + pad_len));
	buf_put(&tags_pkt, lyr_prefix, strlen(lyr_prefix));
	for (size_t i = 0; i < pad_len; i++)
		buf_u8(&tags_pkt, 'x');

	// Split OpusTags packet across continuation Ogg pages ( per RFC 3533, every
	// non-final page of a continued packet carries 255-byte segments only ).
	const size_t page_cap = 200 * 255; // 51000 bytes (200 x 255 segments)
	uint32_t seq = 1;
	for (size_t off = 0; off < tags_pkt.len;) {
		size_t left = tags_pkt.len - off;
		bool final_page = left <= page_cap;
		size_t chunk = final_page ? left : page_cap;
		append_ogg_page(&ogg, off == 0 ? 0x00 : 0x01, 0x1234, seq++, tags_pkt.data + off, chunk, final_page);
		off += chunk;
	}
	write_fixture(opus_as_ogg, ogg.data, ogg.len);
	free(tags_pkt.data);
	free(ogg.data);

	assert(decode_detect_format(opus_as_ogg) == DECODE_FORMAT_OPUS);
	metadata_read(opus_as_ogg, &meta);
	assert(strcmp(meta.title, "Opus In Ogg Clothing") == 0);
	assert(strcmp(meta.artist, "Burial") == 0);
	char *lyrics = metadata_read_lyrics(opus_as_ogg);
	assert(lyrics != NULL);
	assert(strncmp(lyrics, "[00:01.00]Archangel ", 20) == 0);
	free(lyrics);

	// (c) MP3 tagged ONLY with APEv2 at the end (foobar2000 / Mp3tag style) + WavPack APEv2
	char ape_mp3[512];
	snprintf(ape_mp3, sizeof(ape_mp3), "%s/ape_only.mp3", dir);
	buf_t mp3 = {0};
	uint8_t dummy_mpeg[64] = {0xFF, 0xFB, 0x90, 0x64};
	buf_put(&mp3, dummy_mpeg, sizeof(dummy_mpeg));
	const char *akeys[] = {"Title", "Artist", "AlbumArtist", "TrackNumber", "DiscNumber", "date"};
	const char *avals[] = {"APEv2 Track", "Portishead", "Portishead", "7/11", "2/2", "1994-08-22"};
	append_apev2_tag(&mp3, akeys, avals, 6);
	write_fixture(ape_mp3, mp3.data, mp3.len);
	free(mp3.data);

	metadata_read(ape_mp3, &meta);
	assert(strcmp(meta.title, "APEv2 Track") == 0);
	assert(strcmp(meta.artist, "Portishead") == 0);
	assert(strcmp(meta.album_artist, "Portishead") == 0);
	assert(meta.track_number == 7);
	assert(meta.disc_number == 2);
	assert(meta.year == 1994);
	assert(strcmp(meta.date, "1994-08-22") == 0);

	// (d) CUE sheet declaring FILE "disc.wav" WAVE when the ripped image is disc.flac
	char disc_flac[512], disc_cue[512];
	snprintf(disc_flac, sizeof(disc_flac), "%s/disc.flac", dir);
	snprintf(disc_cue, sizeof(disc_cue), "%s/disc.cue", dir);
	buf_t df = {0};
	append_flac_stream(&df, NULL, 0);
	write_fixture(disc_flac, df.data, df.len);
	free(df.data);

	const char *cue_text =
		"PERFORMER \"Pink Floyd\"\n"
		"TITLE \"Wish You Were Here\"\n"
		"FILE \"disc.wav\" WAVE\n"
		"  TRACK 01 AUDIO\n"
		"    TITLE \"Shine On You Crazy Diamond\"\n"
		"    INDEX 01 00:00:00\n";
	write_fixture(disc_cue, (const uint8_t *)cue_text, strlen(cue_text));

	cue_sheet_t sheet;
	assert(cue_parse(disc_cue, &sheet));
	assert(strcmp(sheet.audio_path, disc_flac) == 0);
	char cue_track_path[540];
	cue_virtual_path(disc_cue, 1, cue_track_path, sizeof(cue_track_path));
	assert(decode_detect_format(cue_track_path) == DECODE_FORMAT_FLAC);
}

// ---------------------------------------------------------------------------
// 4. Ogg chapters (vorbischap via oggtags) + CAF & WAV date preservation
// ---------------------------------------------------------------------------
static void test_vorbischap_and_containers(const char *dir) {
	// Ogg chapters via oggtags
	char chap_ogg[512];
	snprintf(chap_ogg, sizeof(chap_ogg), "%s/chapters.opus", dir);
	buf_t ogg = {0};
	uint8_t opus_head[19] = {'O', 'p', 'u', 's', 'H', 'e', 'a', 'd', 1, 2, 0, 0, 0x80, 0xBB, 0, 0, 0, 0, 0};
	append_ogg_page(&ogg, 0x02, 0x77, 0, opus_head, sizeof(opus_head), true);
	buf_t pkt = {0};
	buf_put(&pkt, "OpusTags", 8);
	buf_le32(&pkt, 5);
	buf_put(&pkt, "sonix", 5);
	const char *ch[] = {
		"CHAPTER001=00:00:00.000",
		"CHAPTER001NAME=Prologue",
		"CHAPTER002=00:02:30.500",
		"CHAPTER002NAME=Part Two",
	};
	buf_le32(&pkt, 4);
	for (int i = 0; i < 4; i++) {
		buf_le32(&pkt, (uint32_t)strlen(ch[i]));
		buf_put(&pkt, ch[i], strlen(ch[i]));
	}
	append_ogg_page(&ogg, 0x00, 0x77, 1, pkt.data, pkt.len, true);
	write_fixture(chap_ogg, ogg.data, ogg.len);
	free(pkt.data);
	free(ogg.data);

	vorbischap_t list[8];
	int n = vorbischap_read(chap_ogg, list, 8);
	assert(n == 2);
	assert(strcmp(list[0].title, "Prologue") == 0 && fabs(list[0].start - 0.0) < 0.001);
	assert(strcmp(list[1].title, "Part Two") == 0 && fabs(list[1].start - 150.5) < 0.001);

	// CAF container with "info" chunk and "recorded date" -> V1 tagdate
	char caf_path[512];
	snprintf(caf_path, sizeof(caf_path), "%s/sample.caf", dir);
	buf_t caf = {0};
	buf_put(&caf, "caff", 4);
	buf_be16(&caf, 1); // version
	buf_be16(&caf, 0); // flags
	buf_t info = {0};
	buf_be32(&info, 3); // 3 entries
	const char *kv[][2] = {
		{"title", "Core Audio Song"},
		{"artist", "Ryuichi Sakamoto"},
		{"recorded date", "2017-03-29"},
	};
	for (int i = 0; i < 3; i++) {
		buf_put(&info, kv[i][0], strlen(kv[i][0]) + 1);
		buf_put(&info, kv[i][1], strlen(kv[i][1]) + 1);
	}
	buf_put(&caf, "info", 4);
	buf_be64(&caf, info.len);
	buf_put(&caf, info.data, info.len);
	write_fixture(caf_path, caf.data, caf.len);
	free(info.data);
	free(caf.data);

	song_metadata_t meta;
	metadata_read(caf_path, &meta);
	assert(strcmp(meta.title, "Core Audio Song") == 0);
	assert(strcmp(meta.artist, "Ryuichi Sakamoto") == 0);
	assert(meta.year == 2017);
	assert(strcmp(meta.date, "2017-03-29") == 0);
}

// Exercise the actual writer with the player's real readers and decoder.
static void test_tagwriter_roundtrip(const char *dir) {
    char path[512]; song_metadata_t meta; buf_t data={0}, frames={0};
    snprintf(path,sizeof(path),"%s/edit.mp3",dir);
    append_id3v2_text_frame(&frames,3,"TIT2",0,"Old",3,false);
    wrap_id3v2_tag(&data,3,0,&frames); buf_put(&data,"\xff\xfb\x90\x00",4);
    write_fixture(path,data.data,data.len); free(data.data); free(frames.data);
    assert(tagwrite_set(path,TAGWRITE_TITLE,"Été 🎵")==TAGWRITE_OK);
    assert(tagwrite_set(path,TAGWRITE_ARTIST,"Björk")==TAGWRITE_OK);
    metadata_read(path,&meta); assert(!strcmp(meta.title,"Été 🎵")); assert(!strcmp(meta.artist,"Björk"));
    snprintf(path,sizeof(path),"%s/edit.flac",dir); buf_t flac={0};
    const char *tags[]={"TITLE=Old","ARTIST=Old"}; append_flac_stream(&flac,tags,2);
    write_fixture(path,flac.data,flac.len); free(flac.data);
    assert(tagwrite_set(path,TAGWRITE_TITLE,"Été 🎵")==TAGWRITE_OK);
    assert(tagwrite_set(path,TAGWRITE_ARTIST,"Björk")==TAGWRITE_OK);
    metadata_read(path,&meta); assert(!strcmp(meta.title,"Été 🎵")); assert(!strcmp(meta.artist,"Björk"));
    decoder_t *decoder=decoder_open(path,DECODE_FORMAT_FLAC); assert(decoder);
    short pcm[16]; assert(decoder_read_pcm_frames_s16(decoder,16,pcm)==16); decoder_close(decoder);
}

int main(void) {
	char dir[] = "/tmp/sonix-tags-test-XXXXXX";
	assert(mkdtemp(dir) != NULL);

	test_tagwriter_roundtrip(dir);
	test_flac_with_id3_header(dir);
	test_broken_id3v2_tags(dir);
	test_wrong_extensions_and_cue(dir);
	test_vorbischap_and_containers(dir);

	char cmd[600];
	snprintf(cmd, sizeof(cmd), "rm -rf '%s'", dir);
	int rc = system(cmd);
	(void)rc;

	printf("PASS tags (FLAC+ID3, broken ID3v2.4/zlib/unsync/chained, format sniffing, CUE, Ogg streaming, V1 tagdate)\n");
	return 0;
}
