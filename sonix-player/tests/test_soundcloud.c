#define _GNU_SOURCE 1

#include "src/system/streaming/soundcloud.h"
#include "src/system/streaming/soundcloudcache.h"
#include "src/system/streaming/streamkeys.h"
#include "src/system/streaming/streamturn.h"
#include "src/system/core/config.h"
#include "src/system/core/utils.h"
#include "src/system/decode/decode.h"
#include "src/system/decode/growfile.h"
#include "src/system/decode/opusdec.h"
#include "src/system/decode/wavpackdec.h"
#include "src/system/library/metadata.h"
#include "src/system/net/hls.h"
#include "src/system/net/http.h"
#include "src/system/streaming/subsonicstream.h"

#include <arpa/inet.h>
#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <ftw.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

// Stubs for external static-lib decoders/streams not linked in host unit tests
// (mirrors test_tags.c, while exercising grow_read_opus + .tags duration fallback).
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

struct opusdec {
	int dummy;
};

opusdec_t *opusdec_open(const char *p) {
	FILE *f = fopen(p, "rb");
	if (!f)
		return NULL;
	unsigned char hdr[36];
	size_t n = fread(hdr, 1, sizeof(hdr), f);
	fclose(f);
	if (n < sizeof(hdr) || memcmp(hdr, "OggS", 4) != 0 || memcmp(hdr + 28, "OpusHead", 8) != 0)
		return NULL;
	return calloc(1, sizeof(opusdec_t));
}
opusdec_t *opusdec_open_stream(void *s, int (*r)(void *, unsigned char *, int)) {
	unsigned char hdr[36];
	int got = 0;
	while (got < (int)sizeof(hdr)) {
		int n = r(s, hdr + got, (int)sizeof(hdr) - got);
		if (n <= 0)
			return NULL;
		got += n;
	}
	if (memcmp(hdr, "OggS", 4) != 0 || memcmp(hdr + 28, "OpusHead", 8) != 0)
		return NULL;
	return calloc(1, sizeof(opusdec_t));
}
void opusdec_close(opusdec_t *o) { free(o); }
int opusdec_channels(const opusdec_t *o) {
	(void)o;
	return 2;
}
int opusdec_sample_rate(const opusdec_t *o) {
	(void)o;
	return 48000;
}
uint64_t opusdec_total_frames(const opusdec_t *o) {
	(void)o;
	return 0; // growing Ogg Opus stream has unknown total frames -> decode.c falls back to .tags duration=
}
uint64_t opusdec_read_s16(opusdec_t *o, uint64_t f, short *out) {
	(void)o;
	if (f == 0)
		return 0;
	uint64_t n = f > 256 ? 256 : f;
	memset(out, 0, (size_t)n * 2 * sizeof(short));
	return n;
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

const char *tr(const char *key) {
	return key ? key : "";
}

static char current_playing_path[512];
void device_state_get_current_file(char *out, size_t size) {
	if (!out || size == 0) return;
	snprintf(out, size, "%s", current_playing_path);
}

// ---------------------------------------------------------------------------
// Portable FIPS-197 AES-128-CBC encryptor (for testing HLS AES-128 decryption)
// ---------------------------------------------------------------------------

static const uint8_t test_sbox[256] = {
	0x63,0x7c,0x77,0x7b,0xf2,0x6b,0x6f,0xc5,0x30,0x01,0x67,0x2b,0xfe,0xd7,0xab,0x76,
	0xca,0x82,0xc9,0x7d,0xfa,0x59,0x47,0xf0,0xad,0xd4,0xa2,0xaf,0x9c,0xa4,0x72,0xc0,
	0xb7,0xfd,0x93,0x26,0x36,0x3f,0xf7,0xcc,0x34,0xa5,0xe5,0xf1,0x71,0xd8,0x31,0x15,
	0x04,0xc7,0x23,0xc3,0x18,0x96,0x05,0x9a,0x07,0x12,0x80,0xe2,0xeb,0x27,0xb2,0x75,
	0x09,0x83,0x2c,0x1a,0x1b,0x6e,0x5a,0xa0,0x52,0x3b,0xd6,0xb3,0x29,0xe3,0x2f,0x84,
	0x53,0xd1,0x00,0xed,0x20,0xfc,0xb1,0x5b,0x6a,0xcb,0xbe,0x39,0x4a,0x4c,0x58,0xcf,
	0xd0,0xef,0xaa,0xfb,0x43,0x4d,0x33,0x85,0x45,0xf9,0x02,0x7f,0x50,0x3c,0x9f,0xa8,
	0x51,0xa3,0x40,0x8f,0x92,0x9d,0x38,0xf5,0xbc,0xb6,0xda,0x21,0x10,0xff,0xf3,0xd2,
	0xcd,0x0c,0x13,0xec,0x5f,0x97,0x44,0x17,0xc4,0xa7,0x7e,0x3d,0x64,0x5d,0x19,0x73,
	0x60,0x81,0x4f,0xdc,0x22,0x2a,0x90,0x88,0x46,0xee,0xb8,0x14,0xde,0x5e,0x0b,0xdb,
	0xe0,0x32,0x3a,0x0a,0x49,0x06,0x24,0x5c,0xc2,0xd3,0xac,0x62,0x91,0x95,0xe4,0x79,
	0xe7,0xc8,0x37,0x6d,0x8d,0xd5,0x4e,0xa9,0x6c,0x56,0xf4,0xea,0x65,0x7a,0xae,0x08,
	0xba,0x78,0x25,0x2e,0x1c,0xa6,0xb4,0xc6,0xe8,0xdd,0x74,0x1f,0x4b,0xbd,0x8b,0x8a,
	0x70,0x3e,0xb5,0x66,0x48,0x03,0xf6,0x0e,0x61,0x35,0x57,0xb9,0x86,0xc1,0x1d,0x9e,
	0xe1,0xf8,0x98,0x11,0x69,0xd9,0x8e,0x94,0x9b,0x1e,0x87,0xe9,0xce,0x55,0x28,0xdf,
	0x8c,0xa1,0x89,0x0d,0xbf,0xe6,0x42,0x68,0x41,0x99,0x2d,0x0f,0xb0,0x54,0xbb,0x16,
};

static uint8_t xtime2(uint8_t x) {
	return (uint8_t)((x << 1) ^ ((x & 0x80) ? 0x1b : 0x00));
}

static void aes128_key_expand_enc(const uint8_t key[16], uint8_t rk[176]) {
	static const uint8_t rcon[10] = {0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80, 0x1b, 0x36};
	memcpy(rk, key, 16);
	for (int i = 4; i < 44; i++) {
		uint8_t t[4];
		memcpy(t, rk + (i - 1) * 4, 4);
		if (i % 4 == 0) {
			uint8_t u = t[0];
			t[0] = test_sbox[t[1]] ^ rcon[(i / 4) - 1];
			t[1] = test_sbox[t[2]];
			t[2] = test_sbox[t[3]];
			t[3] = test_sbox[u];
		}
		for (int j = 0; j < 4; j++) {
			rk[i * 4 + j] = rk[(i - 4) * 4 + j] ^ t[j];
		}
	}
}

static void aes128_encrypt_block(uint8_t s[16], const uint8_t rk[176]) {
	for (int i = 0; i < 16; i++) s[i] ^= rk[i];
	for (int round = 1; round <= 10; round++) {
		uint8_t t[16];
		for (int i = 0; i < 16; i++) t[i] = test_sbox[s[i]];
		s[0] = t[0]; s[4] = t[4]; s[8] = t[8]; s[12] = t[12];
		s[1] = t[5]; s[5] = t[9]; s[9] = t[13]; s[13] = t[1];
		s[2] = t[10]; s[6] = t[14]; s[10] = t[2]; s[14] = t[6];
		s[3] = t[15]; s[7] = t[3]; s[11] = t[7]; s[15] = t[11];
		if (round < 10) {
			for (int c = 0; c < 4; c++) {
				uint8_t *col = s + c * 4;
				uint8_t a0 = col[0], a1 = col[1], a2 = col[2], a3 = col[3];
				uint8_t xr = a0 ^ a1 ^ a2 ^ a3;
				col[0] ^= xr ^ xtime2(a0 ^ a1);
				col[1] ^= xr ^ xtime2(a1 ^ a2);
				col[2] ^= xr ^ xtime2(a2 ^ a3);
				col[3] ^= xr ^ xtime2(a3 ^ a0);
			}
		}
		for (int i = 0; i < 16; i++) s[i] ^= rk[round * 16 + i];
	}
}

static uint8_t *aes128_cbc_encrypt_pkcs7(const uint8_t *plain, size_t plain_len, const uint8_t key[16],
										 const uint8_t iv_in[16], size_t *out_len) {
	size_t pad = 16 - (plain_len % 16);
	size_t total = plain_len + pad;
	uint8_t *out = malloc(total);
	assert(out != NULL);
	memcpy(out, plain, plain_len);
	memset(out + plain_len, (int)pad, pad);
	uint8_t rk[176], prev[16];
	aes128_key_expand_enc(key, rk);
	memcpy(prev, iv_in, 16);
	for (size_t off = 0; off < total; off += 16) {
		for (int i = 0; i < 16; i++) out[off + i] ^= prev[i];
		aes128_encrypt_block(out + off, rk);
		memcpy(prev, out + off, 16);
	}
	*out_len = total;
	return out;
}

// ---------------------------------------------------------------------------
// Deterministic audio & container fixtures (MP3, Ogg Opus, fMP4 AAC)
// ---------------------------------------------------------------------------

static uint8_t *make_mp3_frames(int frames, int id3_pad, size_t *out_len) {
	size_t audio_len = (size_t)frames * 417;
	size_t hdr_len = id3_pad > 0 ? (size_t)(10 + id3_pad) : 0;
	uint8_t *buf = calloc(1, hdr_len + audio_len);
	assert(buf != NULL);
	if (id3_pad > 0) {
		memcpy(buf, "ID3\x04\x00\x00", 6);
		buf[6] = (uint8_t)((id3_pad >> 21) & 0x7f);
		buf[7] = (uint8_t)((id3_pad >> 14) & 0x7f);
		buf[8] = (uint8_t)((id3_pad >> 7) & 0x7f);
		buf[9] = (uint8_t)(id3_pad & 0x7f);
	}
	for (int i = 0; i < frames; i++) {
		uint8_t *f = buf + hdr_len + (size_t)i * 417;
		f[0] = 0xff;
		f[1] = 0xfb;
		f[2] = 0x90;
		f[3] = 0x00;
	}
	*out_len = hdr_len + audio_len;
	return buf;
}

static uint32_t ogg_crc32(const uint8_t *data, size_t len) {
	uint32_t crc = 0;
	for (size_t i = 0; i < len; i++) {
		crc ^= (uint32_t)data[i] << 24;
		for (int b = 0; b < 8; b++) {
			crc = (crc << 1) ^ ((crc & 0x80000000U) ? 0x04c11db7U : 0U);
		}
	}
	return crc;
}

static size_t append_ogg_page(uint8_t *dst, const uint8_t *pkt, size_t pkt_len, uint32_t seq, uint64_t granule,
							  uint8_t flags) {
	assert(pkt_len < 255);
	memcpy(dst, "OggS", 4);
	dst[4] = 0;
	dst[5] = flags;
	for (int i = 0; i < 8; i++) dst[6 + i] = (uint8_t)((granule >> (i * 8)) & 0xff);
	uint32_t serial = 0x534f4e49U;
	memcpy(dst + 14, &serial, 4);
	memcpy(dst + 18, &seq, 4);
	memset(dst + 22, 0, 4);
	dst[26] = 1;
	dst[27] = (uint8_t)pkt_len;
	memcpy(dst + 28, pkt, pkt_len);
	size_t total = 28 + pkt_len;
	uint32_t crc = ogg_crc32(dst, total);
	for (int i = 0; i < 4; i++) dst[22 + i] = (uint8_t)((crc >> (i * 8)) & 0xff);
	return total;
}

static void make_opus_two_segments(uint8_t **seg0, size_t *len0, uint8_t **seg1, size_t *len1) {
	*seg0 = calloc(1, 2048);
	*seg1 = calloc(1, 2048);
	assert(*seg0 && *seg1);

	uint8_t head[19] = {'O', 'p', 'u', 's', 'H', 'e', 'a', 'd', 1, 2, 0x38, 0x01, 0x80, 0xbb, 0x00, 0x00, 0, 0, 0};
	uint8_t tags_full[21] = {'O', 'p', 'u', 's', 'T', 'a', 'g', 's', 5, 0, 0, 0, 's', 'o', 'n', 'i', 'x', 0, 0, 0, 0};

	size_t off0 = 0;
	off0 += append_ogg_page(*seg0 + off0, head, sizeof(head), 0, 0, 2);
	off0 += append_ogg_page(*seg0 + off0, tags_full, sizeof(tags_full), 1, 0, 0);
	const uint8_t sil[3] = {0xf8, 0xff, 0xfe};
	for (int i = 0; i < 10; i++) {
		off0 += append_ogg_page(*seg0 + off0, sil, sizeof(sil), (uint32_t)(2 + i), (uint64_t)(i + 1) * 960, 0);
	}
	*len0 = off0;

	size_t off1 = 0;
	for (int i = 10; i < 20; i++) {
		uint8_t fl = (i == 19) ? 4 : 0;
		off1 += append_ogg_page(*seg1 + off1, sil, sizeof(sil), (uint32_t)(2 + i), (uint64_t)(i + 1) * 960, fl);
	}
	*len1 = off1;
}

static void put_be32(uint8_t *p, uint32_t v) {
	p[0] = (uint8_t)(v >> 24);
	p[1] = (uint8_t)(v >> 16);
	p[2] = (uint8_t)(v >> 8);
	p[3] = (uint8_t)v;
}

static size_t make_fmp4_init(uint8_t *out) {
	// ftyp (16 bytes)
	put_be32(out + 0, 16);
	memcpy(out + 4, "ftypisom", 8);
	put_be32(out + 12, 0x200);

	// esds box (36 bytes): AudioSpecificConfig for AAC-LC (aot=2), 44100 Hz (sr_idx=4), stereo (ch=2) -> 0x1210
	static const uint8_t esds_payload[] = {
		0, 0, 0, 0,
		0x03, 22, 0x00, 0x01, 0x00,
		0x04, 14, 0x40, 0x15, 0x00, 0x00, 0x00, 0x00, 0x03, 0xe8, 0x00, 0x00, 0x03, 0xe8, 0x00, 0x00,
		0x05, 2, 0x12, 0x10,
	};
	uint8_t esds_box[8 + sizeof(esds_payload)];
	put_be32(esds_box, sizeof(esds_box));
	memcpy(esds_box + 4, "esds", 4);
	memcpy(esds_box + 8, esds_payload, sizeof(esds_payload));

	uint8_t mp4a[36 + sizeof(esds_box)];
	memset(mp4a, 0, sizeof(mp4a));
	put_be32(mp4a, sizeof(mp4a));
	memcpy(mp4a + 4, "mp4a", 4);
	mp4a[25] = 2;
	mp4a[27] = 16;
	put_be32(mp4a + 32, 44100U << 16);
	memcpy(mp4a + 36, esds_box, sizeof(esds_box));

	uint8_t stsd[16 + sizeof(mp4a)];
	memset(stsd, 0, sizeof(stsd));
	put_be32(stsd, sizeof(stsd));
	memcpy(stsd + 4, "stsd", 4);
	put_be32(stsd + 12, 1);
	memcpy(stsd + 16, mp4a, sizeof(mp4a));

	uint8_t trak[32 + sizeof(stsd)];
	put_be32(trak + 0, sizeof(trak)); memcpy(trak + 4, "trak", 4);
	put_be32(trak + 8, sizeof(trak) - 8); memcpy(trak + 12, "mdia", 4);
	put_be32(trak + 16, sizeof(trak) - 16); memcpy(trak + 20, "minf", 4);
	put_be32(trak + 24, sizeof(trak) - 24); memcpy(trak + 28, "stbl", 4);
	memcpy(trak + 32, stsd, sizeof(stsd));

	uint8_t trex[32];
	memset(trex, 0, sizeof(trex));
	put_be32(trex + 0, 32);
	memcpy(trex + 4, "trex", 4);
	put_be32(trex + 12, 1);
	put_be32(trex + 16, 1);
	put_be32(trex + 20, 1024);
	put_be32(trex + 24, 6);

	uint8_t mvex[40];
	put_be32(mvex + 0, 40);
	memcpy(mvex + 4, "mvex", 4);
	memcpy(mvex + 8, trex, sizeof(trex));

	uint32_t moov_size = (uint32_t)(8 + sizeof(trak) + sizeof(mvex));
	put_be32(out + 16, moov_size);
	memcpy(out + 20, "moov", 4);
	memcpy(out + 24, trak, sizeof(trak));
	memcpy(out + 24 + sizeof(trak), mvex, sizeof(mvex));
	return 16 + moov_size;
}

static size_t make_fmp4_media_segment(uint8_t *out, uint8_t tag_byte) {
	uint8_t moof[40];
	memset(moof, 0, sizeof(moof));
	put_be32(moof + 0, 40); memcpy(moof + 4, "moof", 4);
	put_be32(moof + 8, 32); memcpy(moof + 12, "traf", 4);
	put_be32(moof + 16, 24); memcpy(moof + 20, "trun", 4);
	put_be32(moof + 24, 0x000200);
	put_be32(moof + 28, 2);
	put_be32(moof + 32, 5);
	put_be32(moof + 36, 7);

	uint8_t mdat[20];
	put_be32(mdat + 0, 20);
	memcpy(mdat + 4, "mdat", 4);
	for (int i = 0; i < 5; i++) mdat[8 + i] = (uint8_t)(tag_byte + i);
	for (int i = 0; i < 7; i++) mdat[13 + i] = (uint8_t)(tag_byte + 0x20 + i);

	memcpy(out, moof, sizeof(moof));
	memcpy(out + sizeof(moof), mdat, sizeof(mdat));
	return sizeof(moof) + sizeof(mdat);
}

static const uint8_t TINY_JPEG[] = {
	0xff,0xd8,0xff,0xe0,0x00,0x10,0x4a,0x46,0x49,0x46,0x00,0x01,0x01,0x01,0x00,0x48,
	0x00,0x48,0x00,0x00,0xff,0xdb,0x00,0x43,0x00,0x08,0x06,0x06,0x07,0x06,0x05,0x08,
	0x07,0x07,0x07,0x09,0x09,0x08,0x0a,0x0c,0x14,0x0d,0x0c,0x0b,0x0b,0x0c,0x19,0x12,
	0x13,0x0f,0x14,0x1d,0x1a,0x1f,0x1e,0x1d,0x1a,0x1c,0x1c,0x20,0x24,0x2e,0x27,0x20,
	0x22,0x2c,0x23,0x1c,0x1c,0x28,0x37,0x29,0x2c,0x30,0x31,0x34,0x34,0x34,0x1f,0x27,
	0x39,0x3d,0x38,0x32,0x3c,0x2e,0x33,0x34,0x32,0xff,0xc0,0x00,0x0b,0x08,0x00,0x01,
	0x00,0x01,0x01,0x01,0x11,0x00,0xff,0xc4,0x00,0x14,0x00,0x01,0x00,0x00,0x00,0x00,
	0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x08,0xff,0xc4,0x00,0x14,
	0x10,0x01,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
	0x00,0x00,0xff,0xda,0x00,0x08,0x01,0x01,0x00,0x00,0x3f,0x00,0x7f,0x00,0xff,0xd9,
};

// ---------------------------------------------------------------------------
// Local mock SoundCloud API v2 + CDN HTTP server
// ---------------------------------------------------------------------------

typedef struct {
	int api_port;
	int cdn_port;
	int api_listen_fd;
	int cdn_listen_fd;
	pthread_t api_tid;
	pthread_t cdn_tid;
	volatile bool stop;

	pthread_mutex_t mu;
	char active_client_id[64];
	char rotated_client_id[64];
	bool rotate_triggered;
	int discovery_html_hits;
	int discovery_js_hits;
	int cdn_auth_leaks;
	int cdn_prog_hits;
	int like_puts;
	int like_deletes;
	int sub_tier; // 0=free, 1=go, 2=go+

	uint8_t *prog_mp3;
	size_t prog_mp3_len;
	uint8_t *hls_mp3_seg0;
	size_t hls_mp3_seg0_len;
	uint8_t *hls_mp3_seg1_enc;
	size_t hls_mp3_seg1_enc_len;
	uint8_t aes_key[16];
	uint8_t aes_iv[16];
	uint8_t *opus_seg0;
	size_t opus_seg0_len;
	uint8_t *opus_seg1;
	size_t opus_seg1_len;
	uint8_t fmp4_init[256];
	size_t fmp4_init_len;
	uint8_t fmp4_seg0[128];
	size_t fmp4_seg0_len;
	uint8_t fmp4_seg1[128];
	size_t fmp4_seg1_len;
} mock_sc_t;

static mock_sc_t G;

static void send_http(int fd, int status, const char *ctype, const void *body, size_t len) {
	const char *reason = (status == 200) ? "OK" : (status == 401) ? "Unauthorized" : (status == 403) ? "Forbidden" : "Not Found";
	char hdr[512];
	int hlen = snprintf(hdr, sizeof(hdr),
						"HTTP/1.1 %d %s\r\n"
						"Content-Type: %s\r\n"
						"Content-Length: %zu\r\n"
						"Connection: close\r\n\r\n",
						status, reason, ctype, len);
	(void)send(fd, hdr, (size_t)hlen, MSG_NOSIGNAL);
	if (body && len > 0) {
		const uint8_t *p = body;
		size_t left = len;
		while (left > 0) {
			ssize_t w = send(fd, p, left, MSG_NOSIGNAL);
			if (w <= 0) break;
			p += w;
			left -= (size_t)w;
		}
	}
}

static bool read_http_request(int fd, char *buf, size_t size) {
	size_t used = 0;
	while (used + 1 < size) {
		ssize_t n = recv(fd, buf + used, size - 1 - used, 0);
		if (n <= 0) break;
		used += (size_t)n;
		buf[used] = '\0';
		if (strstr(buf, "\r\n\r\n")) return true;
	}
	return used > 0;
}

static bool query_param(const char *path, const char *key, char *out, size_t size) {
	const char *q = strchr(path, '?');
	if (!q) return false;
	q++;
	size_t klen = strlen(key);
	while (*q) {
		const char *amp = strchr(q, '&');
		size_t len = amp ? (size_t)(amp - q) : strlen(q);
		if (len > klen + 1 && strncmp(q, key, klen) == 0 && q[klen] == '=') {
			size_t vlen = len - klen - 1;
			if (vlen >= size) vlen = size - 1;
			memcpy(out, q + klen + 1, vlen);
			out[vlen] = '\0';
			return true;
		}
		if (!amp) break;
		q = amp + 1;
	}
	return false;
}

static void format_track_json(char *out, size_t size, long long id, const char *title, int cdn_port, int mode) {
	// mode 0: progressive mp3 + snipped Go+ hq aac (tests Free/Go skipping snipped preview!)
	// mode 1: full hq aac HLS + mp3_128k progressive + opus_64k HLS (tests Go+ / Low / Standard)
	// mode 2: AES-128 encrypted HLS mp3
	// mode 3: BLOCK policy (no transcodings)
	if (mode == 3) {
		snprintf(out, size,
				 "{\"id\":%lld,\"title\":\"%s\",\"duration\":180000,\"full_duration\":180000,"
				 "\"policy\":\"BLOCK\",\"access\":\"blocked\",\"streamable\":false,"
				 "\"user\":{\"id\":9000000001,\"username\":\"Blocked Artist\"},"
				 "\"media\":{\"transcodings\":[]}}",
				 id, title);
		return;
	}
	if (mode == 2) {
		snprintf(out, size,
				 "{\"id\":%lld,\"title\":\"%s\",\"duration\":90000,\"full_duration\":90000,"
				 "\"policy\":\"ALLOW\",\"access\":\"playable\",\"streamable\":true,"
				 "\"artwork_url\":\"http://127.0.0.1:%d/art-large.jpg\","
				 "\"user\":{\"id\":9000000002,\"username\":\"Cipher DJ\"},"
				 "\"media\":{\"transcodings\":["
				 "{\"url\":\"http://127.0.0.1:%d/resolve/hls_aes\",\"quality\":\"sq\","
				 "\"duration\":90000,\"snipped\":false,"
				 "\"format\":{\"protocol\":\"hls\",\"mime_type\":\"audio/mpeg\"}}"
				 "]}}",
				 id, title, cdn_port, G.api_port);
		return;
	}
	if (mode == 1) {
		snprintf(out, size,
				 "{\"id\":%lld,\"title\":\"%s\",\"duration\":120000,\"full_duration\":120000,"
				 "\"policy\":\"ALLOW\",\"access\":\"playable\",\"streamable\":true,"
				 "\"artwork_url\":\"http://127.0.0.1:%d/art-large.jpg\","
				 "\"user\":{\"id\":9000000003,\"username\":\"Neon Producer\"},"
				 "\"media\":{\"transcodings\":["
				 "{\"url\":\"http://127.0.0.1:%d/resolve/hls_aac\",\"quality\":\"hq\","
				 "\"duration\":120000,\"snipped\":false,"
				 "\"format\":{\"protocol\":\"hls\",\"mime_type\":\"audio/mp4; codecs=\\\"mp4a.40.2\\\"\"}},"
				 "{\"url\":\"http://127.0.0.1:%d/resolve/prog_mp3\",\"quality\":\"sq\","
				 "\"duration\":120000,\"snipped\":false,"
				 "\"format\":{\"protocol\":\"progressive\",\"mime_type\":\"audio/mpeg\"}},"
				 "{\"url\":\"http://127.0.0.1:%d/resolve/hls_opus\",\"quality\":\"sq\","
				 "\"duration\":120000,\"snipped\":false,"
				 "\"format\":{\"protocol\":\"hls\",\"mime_type\":\"audio/ogg; codecs=\\\"opus\\\"\"}}"
				 "]}}",
				 id, title, cdn_port, G.api_port, G.api_port, G.api_port);
		return;
	}
	snprintf(out, size,
			 "{\"id\":%lld,\"title\":\"%s\",\"duration\":210000,\"full_duration\":210000,"
			 "\"policy\":\"ALLOW\",\"access\":\"playable\",\"streamable\":true,"
			 "\"artwork_url\":\"http://127.0.0.1:%d/art-large.jpg\","
			 "\"user\":{\"id\":9000000004,\"username\":\"Indie Uploader\"},"
			 "\"media\":{\"transcodings\":["
			 "{\"url\":\"http://127.0.0.1:%d/resolve/snip_aac\",\"quality\":\"hq\","
			 "\"duration\":30000,\"snipped\":true,"
			 "\"format\":{\"protocol\":\"hls\",\"mime_type\":\"audio/mp4; codecs=\\\"mp4a.40.2\\\"\"}},"
			 "{\"url\":\"http://127.0.0.1:%d/resolve/prog_mp3\",\"quality\":\"sq\","
			 "\"duration\":210000,\"snipped\":false,"
			 "\"format\":{\"protocol\":\"progressive\",\"mime_type\":\"audio/mpeg\"}}"
			 "]}}",
			 id, title, cdn_port, G.api_port, G.api_port);
}

static void handle_api_conn(int fd) {
	char req[4096];
	if (!read_http_request(fd, req, sizeof(req))) return;

	char method[16] = "", path[2048] = "";
	if (sscanf(req, "%15s %2047s", method, path) != 2) return;

	// Web HTML & JS bundle endpoints for client_id auto-discovery.
	if (strcmp(path, "/web") == 0 || strcmp(path, "/web/") == 0) {
		pthread_mutex_lock(&G.mu);
		G.discovery_html_hits++;
		const char *cid_js = G.rotate_triggered ? "/assets/50-rotated.js" : "/assets/49-initial.js";
		pthread_mutex_unlock(&G.mu);
		char html[512];
		int n = snprintf(html, sizeof(html),
						 "<!doctype html><html><body>"
						 "<script crossorigin src=\"http://127.0.0.1:%d/assets/0-vendor.js\"></script>"
						 "<script crossorigin src=\"http://127.0.0.1:%d%s\"></script>"
						 "</body></html>",
						 G.api_port, G.api_port, cid_js);
		send_http(fd, 200, "text/html", html, (size_t)n);
		return;
	}
	if (strncmp(path, "/assets/0-vendor.js", 19) == 0) {
		const char *js = "console.log('vendor bundle without id');";
		send_http(fd, 200, "application/javascript", js, strlen(js));
		return;
	}
	if (strncmp(path, "/assets/49-initial.js", 21) == 0 || strncmp(path, "/assets/50-rotated.js", 21) == 0) {
		pthread_mutex_lock(&G.mu);
		G.discovery_js_hits++;
		const char *cid = strstr(path, "rotated") ? G.rotated_client_id : G.active_client_id;
		char js[256];
		int n = snprintf(js, sizeof(js), "window.__sc={env:'prod',client_id:\"%s\",version:2};", cid);
		pthread_mutex_unlock(&G.mu);
		send_http(fd, 200, "application/javascript", js, (size_t)n);
		return;
	}

	assert(strcasestr(req, "Origin: https://soundcloud.com") != NULL);
	assert(strcasestr(req, "Referer: https://soundcloud.com/") != NULL);

	char cid_param[128] = "";
	(void)query_param(path, "client_id", cid_param, sizeof(cid_param));
	pthread_mutex_lock(&G.mu);
	const char *expected_cid = G.rotate_triggered ? G.rotated_client_id : G.active_client_id;
	bool cid_ok = (strcmp(cid_param, expected_cid) == 0);
	int tier = G.sub_tier;
	pthread_mutex_unlock(&G.mu);

	bool has_oauth = (strcasestr(req, "Authorization: OAuth test-oauth-token-secret") != NULL);

	if (strncmp(path, "/me", 3) == 0 && (path[3] == '\0' || path[3] == '?')) {
		if (!has_oauth) {
			send_http(fd, 401, "application/json", "{\"error\":\"no oauth\"}", 20);
			return;
		}
		if (!cid_ok) {
			send_http(fd, 401, "application/json", "{\"error\":\"bad client_id\"}", 25);
			return;
		}
		const char *sub_json = (tier == 2)
			? "[{\"product\":{\"id\":\"soundcloud-go-plus\"}}]"
			: (tier == 1) ? "[{\"product\":{\"id\":\"soundcloud-go\"}}]" : "[]";
		char body[512];
		int n = snprintf(body, sizeof(body),
						 "{\"id\":4294967305,\"username\":\"SonixListener\",\"permalink\":\"sonix-listener\","
						 "\"full_name\":\"Sonix Listener\",\"consumer_subscription\":%s,\"subscriptions\":%s}",
						 (tier == 2) ? "{\"product\":{\"id\":\"soundcloud-go-plus\"}}" : "null", sub_json);
		send_http(fd, 200, "application/json", body, (size_t)n);
		return;
	}

	if (!cid_ok) {
		send_http(fd, 401, "application/json", "{\"error\":\"expired client_id\"}", 29);
		return;
	}

	// Like / Unlike PUT & DELETE
	if (strncmp(path, "/users/4294967305/track_likes/2199023255552", 43) == 0) {
		if (!has_oauth) {
			send_http(fd, 401, "application/json", "{}", 2);
			return;
		}
		pthread_mutex_lock(&G.mu);
		if (strcmp(method, "PUT") == 0) G.like_puts++;
		else if (strcmp(method, "DELETE") == 0) G.like_deletes++;
		pthread_mutex_unlock(&G.mu);
		send_http(fd, 200, "application/json", "{}", 2);
		return;
	}

	// User track likes
	if (strncmp(path, "/users/4294967305/track_likes", 29) == 0) {
		char t1[1536], body[2048];
		format_track_json(t1, sizeof(t1), 2199023255552LL, "64-Bit Midnight Drive", G.cdn_port, 0);
		int n = snprintf(body, sizeof(body), "{\"collection\":[{\"track\":%s}],\"next_href\":null}", t1);
		send_http(fd, 200, "application/json", body, (size_t)n);
		return;
	}

	// Reposts
	if (strncmp(path, "/stream/users/4294967305", 24) == 0) {
		char t1[1536], body[2048];
		format_track_json(t1, sizeof(t1), 2199023255553LL, "Stream Repost Tune", G.cdn_port, 1);
		int n = snprintf(body, sizeof(body),
						 "{\"collection\":[{\"type\":\"track-repost\",\"track\":%s}],\"next_href\":null}", t1);
		send_http(fd, 200, "application/json", body, (size_t)n);
		return;
	}

	// Search tracks
	if (strncmp(path, "/search/tracks?", 15) == 0) {
		char t0[1536], t1[1536], t2[1536], t3[1024];
		format_track_json(t0, sizeof(t0), 2199023255552LL, "64-Bit Midnight Drive", G.cdn_port, 0);
		format_track_json(t1, sizeof(t1), 2199023255560LL, "Neon Synth Go+", G.cdn_port, 1);
		format_track_json(t2, sizeof(t2), 2199023255561LL, "Encrypted Club Mix", G.cdn_port, 2);
		format_track_json(t3, sizeof(t3), 2199023255562LL, "Region Blocked Track", G.cdn_port, 3);
		char *body = malloc(6144);
		assert(body != NULL);
		int n = snprintf(body, 6144, "{\"collection\":[%s,%s,%s,%s],\"next_href\":null}", t0, t1, t2, t3);
		send_http(fd, 200, "application/json", body, (size_t)n);
		free(body);
		return;
	}

	// Single track lookup `/tracks/<id>` (used by soundcloud_get_stream)
	if (strncmp(path, "/tracks/", 8) == 0) {
		long long tid = atoll(path + 8);
		char tj[1536];
		if (tid == 2199023255552LL) format_track_json(tj, sizeof(tj), tid, "64-Bit Midnight Drive", G.cdn_port, 0);
		else if (tid == 2199023255560LL) format_track_json(tj, sizeof(tj), tid, "Neon Synth Go+", G.cdn_port, 1);
		else if (tid == 2199023255561LL) format_track_json(tj, sizeof(tj), tid, "Encrypted Club Mix", G.cdn_port, 2);
		else format_track_json(tj, sizeof(tj), tid, "Region Blocked Track", G.cdn_port, 3);
		send_http(fd, 200, "application/json", tj, strlen(tj));
		return;
	}

	// Playlist with 55 stub tracks (exercises >50 multi-batch hydration while preserving order!)
	if (strncmp(path, "/playlists/7700000001?", 22) == 0) {
		size_t cap = 16384;
		char *body = malloc(cap);
		assert(body != NULL);
		char first_full[1536];
		format_track_json(first_full, sizeof(first_full), 8800000000LL, "Lead Track Full", G.cdn_port, 0);
		int used = snprintf(body, cap,
							"{\"id\":7700000001,\"title\":\"Hydrated Set\",\"track_count\":56,"
							"\"user\":{\"id\":9000000004,\"username\":\"Curator\"},\"tracks\":[%s",
							first_full);
		for (int i = 1; i <= 55; i++) {
			long long sid = 8800001000LL - i;
			used += snprintf(body + used, cap - (size_t)used, ",{\"id\":%lld}", sid);
		}
		used += snprintf(body + used, cap - (size_t)used, "]}");
		send_http(fd, 200, "application/json", body, (size_t)used);
		free(body);
		return;
	}

	// Batch hydration endpoint: /tracks?ids=1,2,3...
	if (strncmp(path, "/tracks?", 8) == 0) {
		char ids_raw[2048] = "";
		if (!query_param(path, "ids", ids_raw, sizeof(ids_raw))) {
			send_http(fd, 400, "application/json", "[]", 2);
			return;
		}
		char ids[2048];
		size_t j = 0;
		for (size_t i = 0; ids_raw[i] && j + 1 < sizeof(ids); i++) {
			if (ids_raw[i] == '%' && ids_raw[i + 1] == '2' && (ids_raw[i + 2] == 'C' || ids_raw[i + 2] == 'c')) {
				ids[j++] = ',';
				i += 2;
			} else {
				ids[j++] = ids_raw[i];
			}
		}
		ids[j] = '\0';

		long long parsed[64];
		int count = 0;
		char *save = NULL;
		for (char *tok = strtok_r(ids, ",", &save); tok && count < 64; tok = strtok_r(NULL, ",", &save)) {
			parsed[count++] = atoll(tok);
		}
		assert(count <= 50);

		size_t cap = 100000;
		char *body = malloc(cap);
		assert(body != NULL);
		int used = snprintf(body, cap, "[");
		for (int i = count - 1; i >= 0; i--) {
			char tj[1536], tname[64];
			snprintf(tname, sizeof(tname), "Hydrated #%lld", parsed[i]);
			format_track_json(tj, sizeof(tj), parsed[i], tname, G.cdn_port, 0);
			used += snprintf(body + used, cap - (size_t)used, "%s%s", (i < count - 1) ? "," : "", tj);
		}
		used += snprintf(body + used, cap - (size_t)used, "]");
		send_http(fd, 200, "application/json", body, (size_t)used);
		free(body);
		return;
	}

	if (strncmp(path, "/resolve/", 9) == 0) {
		char body[512];
		const char *target = "/prog.mp3";
		if (strncmp(path + 9, "hls_aes", 7) == 0) target = "/hls_aes/master.m3u8";
		else if (strncmp(path + 9, "hls_opus", 8) == 0) target = "/hls_opus/playlist.m3u8";
		else if (strncmp(path + 9, "hls_aac", 7) == 0) target = "/hls_aac/playlist.m3u8";
		else if (strncmp(path + 9, "snip_aac", 8) == 0) target = "/snip_aac/playlist.m3u8";
		int n = snprintf(body, sizeof(body), "{\"url\":\"http://127.0.0.1:%d%s\"}", G.cdn_port, target);
		send_http(fd, 200, "application/json", body, (size_t)n);
		return;
	}

	send_http(fd, 404, "application/json", "{\"error\":\"not found\"}", 21);
}

static void handle_cdn_conn(int fd) {
	char req[4096];
	if (!read_http_request(fd, req, sizeof(req))) return;

	if (strcasestr(req, "Authorization:") != NULL) {
		pthread_mutex_lock(&G.mu);
		G.cdn_auth_leaks++;
		pthread_mutex_unlock(&G.mu);
	}

	char method[16] = "", path[2048] = "";
	if (sscanf(req, "%15s %2047s", method, path) != 2) return;

	if (strncmp(path, "/art-", 5) == 0) {
		send_http(fd, 200, "image/jpeg", TINY_JPEG, sizeof(TINY_JPEG));
		return;
	}
	if (strcmp(path, "/prog.mp3") == 0) {
		pthread_mutex_lock(&G.mu);
		G.cdn_prog_hits++;
		pthread_mutex_unlock(&G.mu);
		send_http(fd, 200, "audio/mpeg", G.prog_mp3, G.prog_mp3_len);
		return;
	}
	if (strcmp(path, "/hls_aes/master.m3u8") == 0) {
		const char *m =
			"#EXTM3U\n"
			"#EXT-X-STREAM-INF:BANDWIDTH=128000,CODECS=\"mp4a.40.34\"\n"
			"media.m3u8\n";
		send_http(fd, 200, "application/vnd.apple.mpegurl", m, strlen(m));
		return;
	}
	if (strcmp(path, "/hls_aes/media.m3u8") == 0) {
		const char *m =
			"#EXTM3U\n"
			"#EXT-X-VERSION:3\n"
			"#EXT-X-MEDIA-SEQUENCE:0\n"
			"#EXT-X-EXTINF:2.0,\n"
			"seg0.mp3\n"
			"#EXT-X-KEY:METHOD=AES-128,URI=\"aes.key\",IV=0x0102030405060708090a0b0c0d0e0f10\n"
			"#EXT-X-EXTINF:2.0,\n"
			"seg1.enc\n"
			"#EXT-X-ENDLIST\n";
		send_http(fd, 200, "application/vnd.apple.mpegurl", m, strlen(m));
		return;
	}
	if (strcmp(path, "/hls_aes/aes.key") == 0) {
		send_http(fd, 200, "application/octet-stream", G.aes_key, 16);
		return;
	}
	if (strcmp(path, "/hls_aes/seg0.mp3") == 0) {
		send_http(fd, 200, "audio/mpeg", G.hls_mp3_seg0, G.hls_mp3_seg0_len);
		return;
	}
	if (strcmp(path, "/hls_aes/seg1.enc") == 0) {
		send_http(fd, 200, "application/octet-stream", G.hls_mp3_seg1_enc, G.hls_mp3_seg1_enc_len);
		return;
	}
	if (strcmp(path, "/hls_opus/playlist.m3u8") == 0) {
		const char *m =
			"#EXTM3U\n"
			"#EXT-X-VERSION:3\n"
			"#EXT-X-EXTINF:1.0,\n"
			"seg0.opus\n"
			"#EXT-X-EXTINF:1.0,\n"
			"seg1.opus\n"
			"#EXT-X-ENDLIST\n";
		send_http(fd, 200, "application/vnd.apple.mpegurl", m, strlen(m));
		return;
	}
	if (strcmp(path, "/hls_opus/seg0.opus") == 0) {
		send_http(fd, 200, "audio/ogg", G.opus_seg0, G.opus_seg0_len);
		return;
	}
	if (strcmp(path, "/hls_opus/seg1.opus") == 0) {
		send_http(fd, 200, "audio/ogg", G.opus_seg1, G.opus_seg1_len);
		return;
	}
	if (strcmp(path, "/hls_aac/playlist.m3u8") == 0) {
		const char *m =
			"#EXTM3U\n"
			"#EXT-X-VERSION:7\n"
			"#EXT-X-MAP:URI=\"init.mp4\"\n"
			"#EXT-X-EXTINF:1.0,\n"
			"seg0.m4s\n"
			"#EXT-X-EXTINF:1.0,\n"
			"seg1.m4s\n"
			"#EXT-X-ENDLIST\n";
		send_http(fd, 200, "application/vnd.apple.mpegurl", m, strlen(m));
		return;
	}
	if (strcmp(path, "/hls_aac/init.mp4") == 0) {
		send_http(fd, 200, "video/mp4", G.fmp4_init, G.fmp4_init_len);
		return;
	}
	if (strcmp(path, "/hls_aac/seg0.m4s") == 0) {
		send_http(fd, 200, "video/iso.segment", G.fmp4_seg0, G.fmp4_seg0_len);
		return;
	}
	if (strcmp(path, "/hls_aac/seg1.m4s") == 0) {
		send_http(fd, 200, "video/iso.segment", G.fmp4_seg1, G.fmp4_seg1_len);
		return;
	}
	send_http(fd, 404, "text/plain", "not found", 9);
}

static int bind_loopback(int *port_out) {
	int s = socket(AF_INET, SOCK_STREAM, 0);
	assert(s >= 0);
	int one = 1;
	setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
	struct sockaddr_in addr = {.sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
	assert(bind(s, (struct sockaddr *)&addr, sizeof(addr)) == 0);
	assert(listen(s, 32) == 0);
	socklen_t alen = sizeof(addr);
	assert(getsockname(s, (struct sockaddr *)&addr, &alen) == 0);
	*port_out = ntohs(addr.sin_port);
	return s;
}

static void *api_server_main(void *arg) {
	(void)arg;
	while (!G.stop) {
		int c = accept(G.api_listen_fd, NULL, NULL);
		if (c < 0) {
			if (errno == EINTR) continue;
			break;
		}
		handle_api_conn(c);
		close(c);
	}
	return NULL;
}

static void *cdn_server_main(void *arg) {
	(void)arg;
	while (!G.stop) {
		int c = accept(G.cdn_listen_fd, NULL, NULL);
		if (c < 0) {
			if (errno == EINTR) continue;
			break;
		}
		handle_cdn_conn(c);
		close(c);
	}
	return NULL;
}

static int unlink_cb(const char *fpath, const struct stat *sb, int typeflag, struct FTW *ftwbuf) {
	(void)sb;
	(void)typeflag;
	(void)ftwbuf;
	return remove(fpath);
}

static void wait_until_file_complete(const char *path) {
	char inc[768];
	snprintf(inc, sizeof(inc), "%s.incompleto", path);
	for (int i = 0; i < 400; i++) {
		if (!growfile_is_growing(path) && access(inc, F_OK) != 0 && access(path, R_OK) == 0) {
			return;
		}
		usleep(10000);
	}
	assert(!"timed out waiting for background cache download to finish");
}

int main(void) {
	memset(&G, 0, sizeof(G));
	pthread_mutex_init(&G.mu, NULL);
	snprintf(G.active_client_id, sizeof(G.active_client_id), "aBcDeFgHiJkLmNoPqRsTuVwXyZ012345");
	snprintf(G.rotated_client_id, sizeof(G.rotated_client_id), "RoTaTeDcLiEnTiD9876543210abcdef");
	G.sub_tier = 0;

	G.prog_mp3 = make_mp3_frames(220, 64, &G.prog_mp3_len);
	G.hls_mp3_seg0 = make_mp3_frames(80, 32, &G.hls_mp3_seg0_len);
	size_t raw_seg1_len = 0;
	uint8_t *raw_seg1 = make_mp3_frames(80, 48, &raw_seg1_len);
	for (int i = 0; i < 16; i++) {
		G.aes_key[i] = (uint8_t)(0xa0 + i);
		G.aes_iv[i] = (uint8_t)(i + 1);
	}
	G.hls_mp3_seg1_enc = aes128_cbc_encrypt_pkcs7(raw_seg1, raw_seg1_len, G.aes_key, G.aes_iv, &G.hls_mp3_seg1_enc_len);
	free(raw_seg1);
	make_opus_two_segments(&G.opus_seg0, &G.opus_seg0_len, &G.opus_seg1, &G.opus_seg1_len);
	G.fmp4_init_len = make_fmp4_init(G.fmp4_init);
	G.fmp4_seg0_len = make_fmp4_media_segment(G.fmp4_seg0, 0x10);
	G.fmp4_seg1_len = make_fmp4_media_segment(G.fmp4_seg1, 0x50);

	G.api_listen_fd = bind_loopback(&G.api_port);
	G.cdn_listen_fd = bind_loopback(&G.cdn_port);
	assert(pthread_create(&G.api_tid, NULL, api_server_main, NULL) == 0);
	assert(pthread_create(&G.cdn_tid, NULL, cdn_server_main, NULL) == 0);

	char tmpdir[] = "/tmp/test_soundcloud_XXXXXX";
	assert(mkdtemp(tmpdir) != NULL);
	char cfg_path[512], sd_root[512];
	snprintf(cfg_path, sizeof(cfg_path), "%s/device_config.ini", tmpdir);
	snprintf(sd_root, sizeof(sd_root), "%s/sdcard", tmpdir);
	assert(mkdir(sd_root, 0700) == 0);

	(void)config_init(cfg_path);
	streamkeys_init();
	soundcloud_init();
	soundcloudcache_set_root(sd_root);

	char api_base[128], web_base[128];
	snprintf(api_base, sizeof(api_base), "http://127.0.0.1:%d", G.api_port);
	snprintf(web_base, sizeof(web_base), "http://127.0.0.1:%d/web", G.api_port);
	soundcloud_set_endpoints_for_test(api_base, web_base);

	// -----------------------------------------------------------------------
	// 1. Anonymous client_id auto-discovery + 401 rotation + 64-bit track IDs
	// -----------------------------------------------------------------------
	assert(!soundcloud_has_account());
	soundcloud_track_t results[8];
	bool more = false;
	int n = soundcloud_search_tracks("midnight", 0, results, 8, &more);
	assert(n == 4);
	assert(G.discovery_html_hits == 1 && G.discovery_js_hits == 1);
	assert(strcmp(config_get("soundcloud", "cached_client_id", ""), G.active_client_id) == 0);

	soundcloud_track_t tr_free = results[0];
	soundcloud_track_t tr_goplus = results[1];
	soundcloud_track_t tr_aes = results[2];
	soundcloud_track_t tr_blocked = results[3];
	assert(tr_free.id == 2199023255552LL);
	assert(strstr(tr_free.cover, "-t500x500.jpg") != NULL);

	// Trigger client_id expiry -> client must auto-rotate on 401 and succeed transparently!
	pthread_mutex_lock(&G.mu);
	G.rotate_triggered = true;
	pthread_mutex_unlock(&G.mu);
	n = soundcloud_search_tracks("midnight", 0, results, 8, &more);
	assert(n == 4);
	assert(strcmp(config_get("soundcloud", "cached_client_id", ""), G.rotated_client_id) == 0);

	// -----------------------------------------------------------------------
	// 2. Credential storage (0600 permissions, remember=false, streamkeys) & OAuth host isolation
	// -----------------------------------------------------------------------
	soundcloud_credentials_t creds_in = {0};
	snprintf(creds_in.oauth_token, sizeof(creds_in.oauth_token), "OAuth test-oauth-token-secret");
	char err[128] = "";
	assert(soundcloud_configure(&creds_in, true, err, sizeof(err)));
	assert(soundcloud_has_account() && soundcloud_can_like());

	soundcloud_account_info_t info = {0};
	soundcloud_account_info(&info);
	assert(info.user_id == 4294967305LL);
	assert(strcmp(info.tier, "Free") == 0);
	assert(info.has_token && info.remembered);

	char cred_path[512];
	snprintf(cred_path, sizeof(cred_path), "%s/soundcloud-credentials.json", tmpdir);
	struct stat st;
	assert(stat(cred_path, &st) == 0);
	assert((st.st_mode & 0777) == 0600);

	// Verify OAuth header isolation: attached ONLY on SoundCloud API hosts, NEVER on CDN hosts!
	char hdrs[512];
	char sc_url[256], cdn_url[256];
	snprintf(sc_url, sizeof(sc_url), "http://127.0.0.1:%d/tracks", G.api_port);
	snprintf(cdn_url, sizeof(cdn_url), "http://127.0.0.1:%d/prog.mp3", G.cdn_port);
	soundcloud_headers_for_url(sc_url, hdrs, sizeof(hdrs));
	assert(strstr(hdrs, "Authorization: OAuth test-oauth-token-secret") != NULL);
	soundcloud_headers_for_url(cdn_url, hdrs, sizeof(hdrs));
	assert(strstr(hdrs, "Authorization:") == NULL);
	soundcloud_headers_for_url("https://cf-media.sndcdn.com/abc.mp3", hdrs, sizeof(hdrs));
	assert(strstr(hdrs, "Authorization:") == NULL);

	// Remember=false keeps active session in memory and removes disk credentials file.
	assert(soundcloud_configure(&creds_in, false, err, sizeof(err)));
	assert(soundcloud_has_account());
	assert(access(cred_path, F_OK) != 0);
	assert(soundcloud_configure(&creds_in, true, err, sizeof(err)));
	assert(stat(cred_path, &st) == 0 && (st.st_mode & 0777) == 0600);

	// Verify [soundcloud] section in microSD streaming-keys.ini is parsed by streamkeys.
	char sk_ini[600];
	snprintf(sk_ini, sizeof(sk_ini), "%s/streaming-keys.ini", sd_root);
	FILE *skf = fopen(sk_ini, "w");
	assert(skf != NULL);
	fprintf(skf, "[soundcloud]\noauth_token = test-oauth-token-secret\nclient_id = %s\nusername = sonix-listener\n",
			G.rotated_client_id);
	fclose(skf);
	assert(streamkeys_load_card(sd_root));
	assert(strcmp(streamkeys_soundcloud_oauth_token(), "test-oauth-token-secret") == 0);
	assert(strcmp(streamkeys_soundcloud_client_id(), G.rotated_client_id) == 0);
	assert(strcmp(streamkeys_soundcloud_username(), "sonix-listener") == 0);
	unlink(sk_ini);

	// -----------------------------------------------------------------------
	// 3. Likes, Like/Unlike toggle, Reposts, and >50-stub Playlist Hydration
	// -----------------------------------------------------------------------
	char next_cur[SOUNDCLOUD_CURSOR_MAX] = "";
	n = soundcloud_liked_tracks(NULL, results, 8, next_cur, sizeof(next_cur));
	assert(n == 1 && results[0].id == 2199023255552LL);
	assert(soundcloud_is_liked(2199023255552LL));

	assert(soundcloud_like_track(2199023255552LL, false));
	assert(!soundcloud_is_liked(2199023255552LL));
	assert(soundcloud_like_track(2199023255552LL, true));
	assert(soundcloud_is_liked(2199023255552LL));
	assert(G.like_puts == 1 && G.like_deletes == 1);

	soundcloud_repost_t reps[4];
	n = soundcloud_reposts(NULL, reps, 4, next_cur, sizeof(next_cur));
	assert(n == 1 && !reps[0].is_playlist && reps[0].track.id == 2199023255553LL);

	// Hydrate 56-track playlist (1 full + 55 stubs across 2 batches of <=50, preserving order!)
	soundcloud_track_t pl_tracks[60];
	n = soundcloud_playlist_tracks(7700000001LL, NULL, 0, pl_tracks, 60, &more);
	assert(n == 56 && !more);
	assert(pl_tracks[0].id == 8800000000LL && strcmp(pl_tracks[0].title, "Lead Track Full") == 0);
	for (int i = 1; i <= 55; i++) {
		assert(pl_tracks[i].id == 8800001000LL - i);
		assert(strncmp(pl_tracks[i].title, "Hydrated #", 10) == 0);
	}

	// -----------------------------------------------------------------------
	// 4. Tier & Quality negotiation (Free preview-skip, Blocked, Opus 64k, Go+ AAC 256k)
	// -----------------------------------------------------------------------
	soundcloud_stream_t resolved = {0};
	// Even with HIGH requested, when `hq` AAC is a 30s `snipped` preview, soundcloud_get_stream
	// MUST prefer the full-length `sq` MP3 stream over the 30s preview!
	soundcloud_set_quality(SOUNDCLOUD_QUALITY_HIGH);
	soundcloudcache_set_aac_override_for_test(1);
	assert(soundcloud_get_stream(tr_free.id, NULL, true, &resolved));
	assert(resolved.codec == SOUNDCLOUD_CODEC_MP3);
	assert(resolved.kind == SOUNDCLOUD_STREAM_PROGRESSIVE && !resolved.snipped);

	// Blocked track fails cleanly with soundcloud_track_unavailable.
	assert(!soundcloud_get_stream(tr_blocked.id, NULL, false, &resolved));
	assert(strcmp(soundcloud_last_error(), "soundcloud_track_unavailable") == 0);

	// Upgrade mock user to Go+ and verify tier detection on /me.
	pthread_mutex_lock(&G.mu);
	G.sub_tier = 2;
	pthread_mutex_unlock(&G.mu);
	assert(soundcloud_configure(&creds_in, true, err, sizeof(err)));
	soundcloud_account_info(&info);
	assert(strcmp(info.tier, "Go+") == 0);

	// -----------------------------------------------------------------------
	// 5. Progressive MP3 download + growfile + decode_open + cache hit + sidecar tags
	// -----------------------------------------------------------------------
	soundcloud_set_quality(SOUNDCLOUD_QUALITY_STANDARD);
	char cached_mp3[576] = "";
	assert(soundcloudcache_get(&tr_free, cached_mp3, sizeof(cached_mp3)));
	assert(soundcloudcache_is_soundcloud_path(cached_mp3));
	assert(soundcloudcache_track_id_of(cached_mp3) == 2199023255552LL);

	decoder_t *dec = decoder_open(cached_mp3, DECODE_FORMAT_MP3);
	assert(dec != NULL);
	int32_t pcm32[2048];
	assert(decoder_read_pcm_frames_s32(dec, 512, pcm32) > 0);
	decoder_close(dec);
	wait_until_file_complete(cached_mp3);

	song_metadata_t meta = {0};
	metadata_read(cached_mp3, &meta);
	assert(strcmp(meta.title, "64-Bit Midnight Drive") == 0);
	assert(strcmp(meta.artist, "Indie Uploader") == 0);

	int hits_before = G.cdn_prog_hits;
	char cached_mp3_again[576] = "";
	assert(soundcloudcache_get(&tr_free, cached_mp3_again, sizeof(cached_mp3_again)));
	assert(strcmp(cached_mp3, cached_mp3_again) == 0);
	assert(G.cdn_prog_hits == hits_before);

	// -----------------------------------------------------------------------
	// 6. HLS MP3 with master playlist, AES-128-CBC encryption & duplicate ID3v2 stripping
	// -----------------------------------------------------------------------
	char cached_aes_mp3[576] = "";
	assert(soundcloudcache_get(&tr_aes, cached_aes_mp3, sizeof(cached_aes_mp3)));
	wait_until_file_complete(cached_aes_mp3);
	assert(stat(cached_aes_mp3, &st) == 0);
	assert((size_t)st.st_size == (42 + 80 * 417) + (80 * 417));
	dec = decoder_open(cached_aes_mp3, DECODE_FORMAT_MP3);
	assert(dec != NULL);
	assert(decoder_read_pcm_frames_s32(dec, 512, pcm32) > 0);
	decoder_close(dec);

	// -----------------------------------------------------------------------
	// 7. HLS Opus (.opus) + placeholder (.mp3 -> .opus) resolution + .tags sidecar
	// -----------------------------------------------------------------------
	soundcloud_set_quality(SOUNDCLOUD_QUALITY_LOW);
	char placeholder[576] = "", cached_opus[576] = "";
	assert(soundcloudcache_placeholder_path(tr_goplus.id, placeholder, sizeof(placeholder)));
	assert(strstr(placeholder, ".mp3") != NULL);
	assert(soundcloudcache_get(&tr_goplus, cached_opus, sizeof(cached_opus)));
	assert(strstr(cached_opus, ".opus") != NULL);
	dec = decoder_open(cached_opus, DECODE_FORMAT_OPUS);
	assert(dec != NULL);
	assert(decoder_total_pcm_frames(dec) == 120ULL * 48000ULL);
	assert(decoder_read_pcm_frames_s32(dec, 128, pcm32) > 0);
	decoder_close(dec);
	wait_until_file_complete(cached_opus);

	char playable[576] = "";
	assert(soundcloudcache_find_playable(tr_goplus.id, playable, sizeof(playable)));
	assert(strcmp(playable, cached_opus) == 0);
	assert(stat(cached_opus, &st) == 0);
	assert((size_t)st.st_size == G.opus_seg0_len + G.opus_seg1_len);
	memset(&meta, 0, sizeof(meta));
	metadata_read(cached_opus, &meta);
	assert(strcmp(meta.title, "Neon Synth Go+") == 0);
	char opus_tags_path[640];
	snprintf(opus_tags_path, sizeof(opus_tags_path), "%s.tags", cached_opus);
	char *tags_content = read_file_content(opus_tags_path);
	assert(tags_content != NULL && strstr(tags_content, "duration=120") != NULL);
	free(tags_content);

	// -----------------------------------------------------------------------
	// 8. Go+ HLS fMP4 -> ADTS .aac unwrapping
	// -----------------------------------------------------------------------
	soundcloudcache_clear();
	soundcloud_set_quality(SOUNDCLOUD_QUALITY_HIGH);
	soundcloudcache_set_aac_override_for_test(1);
	char cached_aac[576] = "";
	assert(soundcloudcache_get(&tr_goplus, cached_aac, sizeof(cached_aac)));
	assert(strstr(cached_aac, ".aac") != NULL);
	wait_until_file_complete(cached_aac);
	assert(stat(cached_aac, &st) == 0);
	assert(st.st_size == 52);
	FILE *af = fopen(cached_aac, "rb");
	assert(af != NULL);
	uint8_t aac_bytes[52];
	assert(fread(aac_bytes, 1, sizeof(aac_bytes), af) == sizeof(aac_bytes));
	fclose(af);
	assert(aac_bytes[0] == 0xff && (aac_bytes[1] & 0xf6) == 0xf0);
	int frame0_len = ((aac_bytes[3] & 0x03) << 11) | (aac_bytes[4] << 3) | ((aac_bytes[5] >> 5) & 0x07);
	assert(frame0_len == 12);
	assert(aac_bytes[7] == 0x10 && aac_bytes[11] == 0x14);
	assert(aac_bytes[12] == 0xff && (aac_bytes[13] & 0xf6) == 0xf0);
	int frame1_len = ((aac_bytes[15] & 0x03) << 11) | (aac_bytes[16] << 3) | ((aac_bytes[17] >> 5) & 0x07);
	assert(frame1_len == 14);
	assert(aac_bytes[19] == 0x30);

	// -----------------------------------------------------------------------
	// 9. Active-track protection on clear + zero CDN OAuth leaks + Forget cleanup
	// -----------------------------------------------------------------------
	soundcloudcache_protect(cached_aac);
	int files = 0;
	long long bytes = 0;
	soundcloudcache_stats(&files, &bytes);
	assert(files >= 1 && bytes > 0);
	soundcloudcache_clear();
	assert(access(cached_aac, R_OK) == 0);
	soundcloudcache_protect(NULL);
	soundcloudcache_clear();
	assert(access(cached_aac, R_OK) != 0);

	assert(soundcloud_forget());
	assert(!soundcloud_has_account());
	assert(access(cred_path, F_OK) != 0);
	assert(G.cdn_auth_leaks == 0);

	soundcloudcache_cancel();
	G.stop = true;
	shutdown(G.api_listen_fd, SHUT_RDWR);
	shutdown(G.cdn_listen_fd, SHUT_RDWR);
	close(G.api_listen_fd);
	close(G.cdn_listen_fd);
	pthread_join(G.api_tid, NULL);
	pthread_join(G.cdn_tid, NULL);

	free(G.prog_mp3);
	free(G.hls_mp3_seg0);
	free(G.hls_mp3_seg1_enc);
	free(G.opus_seg0);
	free(G.opus_seg1);
	nftw(tmpdir, unlink_cb, 32, FTW_DEPTH | FTW_PHYS);

	printf("PASS soundcloud (API v2, 64-bit IDs, 0600 credentials, client_id rotation, OAuth isolation, "
		   "56-track hydration, Free/Go+ tiers, progressive MP3, AES-128 HLS, Opus HLS & fMP4->ADTS AAC)\n");
	return 0;
}
