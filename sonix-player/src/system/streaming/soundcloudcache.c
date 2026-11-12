#include "soundcloudcache.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

#include "src/system/core/config.h"
#include "src/system/decode/aacdec.h"
#include "src/system/decode/growfile.h"
#include "src/system/net/hls.h"
#include "src/system/net/http.h"
#include "src/system/streaming/streamturn.h"

#define CACHE_SUBDIR ".local/soundcloud-cache"
#define CACHE_TIMEOUT_SECS 20
#define MANIFEST_LIMIT (1024 * 1024)
#define SEGMENT_LIMIT (16 * 1024 * 1024)
#define COVER_LIMIT (2 * 1024 * 1024)

#define PROG_PREBUFFER_MIN (96L * 1024)
#define PROG_PREBUFFER_MAX (256L * 1024)
#define HLS_PREBUFFER_BYTES (96L * 1024)
#define HLS_MAX_SEGMENTS 2048

static char cache_dir[512];
static long long protected_id;
static int cache_max_mb = SOUNDCLOUDCACHE_DEFAULT_MAX_MB;
static long long cache_max_bytes = SOUNDCLOUDCACHE_MAX_BYTES;
static int aac_override = -1;

static pthread_mutex_t cache_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t dl_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t dl_cond = PTHREAD_COND_INITIALIZER;

static bool dl_busy;
static bool dl_cancel_flag;
static long long dl_track_id;
static int dl_setup_fd = -1;
static http_stream_t *dl_active_stream;

static __thread char cache_error[256];

const char *soundcloudcache_last_error(void) { return cache_error; }

static void set_cache_error(const char *msg) {
	snprintf(cache_error, sizeof(cache_error), "%s", msg ? msg : "");
}

void soundcloudcache_set_aac_override_for_test(int mode) { aac_override = mode; }

static bool aac_supported(void) {
	if (aac_override >= 0) {
		return aac_override != 0;
	}
	return aacdec_available();
}

// ---------------------------------------------------------------------------
// Portable AES-128-CBC (RFC 8216 #EXT-X-KEY:METHOD=AES-128)
// ---------------------------------------------------------------------------

static const uint8_t aes_sbox[256] = {
	0x63, 0x7c, 0x77, 0x7b, 0xf2, 0x6b, 0x6f, 0xc5, 0x30, 0x01, 0x67, 0x2b, 0xfe, 0xd7, 0xab, 0x76,
	0xca, 0x82, 0xc9, 0x7d, 0xfa, 0x59, 0x47, 0xf0, 0xad, 0xd4, 0xa2, 0xaf, 0x9c, 0xa4, 0x72, 0xc0,
	0xb7, 0xfd, 0x93, 0x26, 0x36, 0x3f, 0xf7, 0xcc, 0x34, 0xa5, 0xe5, 0xf1, 0x71, 0xd8, 0x31, 0x15,
	0x04, 0xc7, 0x23, 0xc3, 0x18, 0x96, 0x05, 0x9a, 0x07, 0x12, 0x80, 0xe2, 0xeb, 0x27, 0xb2, 0x75,
	0x09, 0x83, 0x2c, 0x1a, 0x1b, 0x6e, 0x5a, 0xa0, 0x52, 0x3b, 0xd6, 0xb3, 0x29, 0xe3, 0x2f, 0x84,
	0x53, 0xd1, 0x00, 0xed, 0x20, 0xfc, 0xb1, 0x5b, 0x6a, 0xcb, 0xbe, 0x39, 0x4a, 0x4c, 0x58, 0xcf,
	0xd0, 0xef, 0xaa, 0xfb, 0x43, 0x4d, 0x33, 0x85, 0x45, 0xf9, 0x02, 0x7f, 0x50, 0x3c, 0x9f, 0xa8,
	0x51, 0xa3, 0x40, 0x8f, 0x92, 0x9d, 0x38, 0xf5, 0xbc, 0xb6, 0xda, 0x21, 0x10, 0xff, 0xf3, 0xd2,
	0xcd, 0x0c, 0x13, 0xec, 0x5f, 0x97, 0x44, 0x17, 0xc4, 0xa7, 0x7e, 0x3d, 0x64, 0x5d, 0x19, 0x73,
	0x60, 0x81, 0x4f, 0xdc, 0x22, 0x2a, 0x90, 0x88, 0x46, 0xee, 0xb8, 0x14, 0xde, 0x5e, 0x0b, 0xdb,
	0xe0, 0x32, 0x3a, 0x0a, 0x49, 0x06, 0x24, 0x5c, 0xc2, 0xd3, 0xac, 0x62, 0x91, 0x95, 0xe4, 0x79,
	0xe7, 0xc8, 0x37, 0x6d, 0x8d, 0xd5, 0x4e, 0xa9, 0x6c, 0x56, 0xf4, 0xea, 0x65, 0x7a, 0xae, 0x08,
	0xba, 0x78, 0x25, 0x2e, 0x1c, 0xa6, 0xb4, 0xc6, 0xe8, 0xdd, 0x74, 0x1f, 0x4b, 0xbd, 0x8b, 0x8a,
	0x70, 0x3e, 0xb5, 0x66, 0x48, 0x03, 0xf6, 0x0e, 0x61, 0x35, 0x57, 0xb9, 0x86, 0xc1, 0x1d, 0x9e,
	0xe1, 0xf8, 0x98, 0x11, 0x69, 0xd9, 0x8e, 0x94, 0x9b, 0x1e, 0x87, 0xe9, 0xce, 0x55, 0x28, 0xdf,
	0x8c, 0xa1, 0x89, 0x0d, 0xbf, 0xe6, 0x42, 0x68, 0x41, 0x99, 0x2d, 0x0f, 0xb0, 0x54, 0xbb, 0x16,
};

static const uint8_t aes_inv_sbox[256] = {
	0x52, 0x09, 0x6a, 0xd5, 0x30, 0x36, 0xa5, 0x38, 0xbf, 0x40, 0xa3, 0x9e, 0x81, 0xf3, 0xd7, 0xfb,
	0x7c, 0xe3, 0x39, 0x82, 0x9b, 0x2f, 0xff, 0x87, 0x34, 0x8e, 0x43, 0x44, 0xc4, 0xde, 0xe9, 0xcb,
	0x54, 0x7b, 0x94, 0x32, 0xa6, 0xc2, 0x23, 0x3d, 0xee, 0x4c, 0x95, 0x0b, 0x42, 0xfa, 0xc3, 0x4e,
	0x08, 0x2e, 0xa1, 0x66, 0x28, 0xd9, 0x24, 0xb2, 0x76, 0x5b, 0xa2, 0x49, 0x6d, 0x8b, 0xd1, 0x25,
	0x72, 0xf8, 0xf6, 0x64, 0x86, 0x68, 0x98, 0x16, 0xd4, 0xa4, 0x5c, 0xcc, 0x5d, 0x65, 0xb6, 0x92,
	0x6c, 0x70, 0x48, 0x50, 0xfd, 0xed, 0xb9, 0xda, 0x5e, 0x15, 0x46, 0x57, 0xa7, 0x8d, 0x9d, 0x84,
	0x90, 0xd8, 0xab, 0x00, 0x8c, 0xbc, 0xd3, 0x0a, 0xf7, 0xe4, 0x58, 0x05, 0xb8, 0xb3, 0x45, 0x06,
	0xd0, 0x2c, 0x1e, 0x8f, 0xca, 0x3f, 0x0f, 0x02, 0xc1, 0xaf, 0xbd, 0x03, 0x01, 0x13, 0x8a, 0x6b,
	0x3a, 0x91, 0x11, 0x41, 0x4f, 0x67, 0xdc, 0xea, 0x97, 0xf2, 0xcf, 0xce, 0xf0, 0xb4, 0xe6, 0x73,
	0x96, 0xac, 0x74, 0x22, 0xe7, 0xad, 0x35, 0x85, 0xe2, 0xf9, 0x37, 0xe8, 0x1c, 0x75, 0xdf, 0x6e,
	0x47, 0xf1, 0x1a, 0x71, 0x1d, 0x29, 0xc5, 0x89, 0x6f, 0xb7, 0x62, 0x0e, 0xaa, 0x18, 0xbe, 0x1b,
	0xfc, 0x56, 0x3e, 0x4b, 0xc6, 0xd2, 0x79, 0x20, 0x9a, 0xdb, 0xc0, 0xfe, 0x78, 0xcd, 0x5a, 0xf4,
	0x1f, 0xdd, 0xa8, 0x33, 0x88, 0x07, 0xc7, 0x31, 0xb1, 0x12, 0x10, 0x59, 0x27, 0x80, 0xec, 0x5f,
	0x60, 0x51, 0x7f, 0xa9, 0x19, 0xb5, 0x4a, 0x0d, 0x2d, 0xe5, 0x7a, 0x9f, 0x93, 0xc9, 0x9c, 0xef,
	0xa0, 0xe0, 0x3b, 0x4d, 0xae, 0x2a, 0xf5, 0xb0, 0xc8, 0xeb, 0xbb, 0x3c, 0x83, 0x53, 0x99, 0x61,
	0x17, 0x2b, 0x04, 0x7e, 0xba, 0x77, 0xd6, 0x26, 0xe1, 0x69, 0x14, 0x63, 0x55, 0x21, 0x0c, 0x7d,
};

static uint8_t gf_mul2(uint8_t x) { return (uint8_t)((x << 1) ^ ((x & 0x80) ? 0x1b : 0)); }

static uint8_t gf_mul(uint8_t a, uint8_t b) {
	uint8_t p = 0;
	for (int i = 0; i < 8; i++) {
		if (b & 1) p ^= a;
		a = gf_mul2(a);
		b >>= 1;
	}
	return p;
}

static void aes128_expand_key(const uint8_t key[16], uint8_t rk[176]) {
	static const uint8_t rcon[10] = {0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80, 0x1b, 0x36};
	memcpy(rk, key, 16);
	for (int i = 4; i < 44; i++) {
		uint8_t t[4];
		memcpy(t, &rk[(i - 1) * 4], 4);
		if (i % 4 == 0) {
			uint8_t u = t[0];
			t[0] = aes_sbox[t[1]] ^ rcon[(i / 4) - 1];
			t[1] = aes_sbox[t[2]];
			t[2] = aes_sbox[t[3]];
			t[3] = aes_sbox[u];
		}
		for (int j = 0; j < 4; j++) {
			rk[i * 4 + j] = rk[(i - 4) * 4 + j] ^ t[j];
		}
	}
}

static void aes128_decrypt_block(uint8_t s[16], const uint8_t rk[176]) {
	for (int i = 0; i < 16; i++) s[i] ^= rk[160 + i];
	for (int round = 9; round >= 1; round--) {
		uint8_t t;
		// InvShiftRows
		t = s[13]; s[13] = s[9]; s[9] = s[5]; s[5] = s[1]; s[1] = t;
		t = s[2]; s[2] = s[10]; s[10] = t;
		t = s[6]; s[6] = s[14]; s[14] = t;
		t = s[3]; s[3] = s[7]; s[7] = s[11]; s[11] = s[15]; s[15] = t;
		// InvSubBytes + AddRoundKey
		for (int i = 0; i < 16; i++) s[i] = aes_inv_sbox[s[i]] ^ rk[round * 16 + i];
		// InvMixColumns
		for (int c = 0; c < 4; c++) {
			uint8_t *col = &s[c * 4];
			uint8_t a0 = col[0], a1 = col[1], a2 = col[2], a3 = col[3];
			col[0] = gf_mul(a0, 0x0e) ^ gf_mul(a1, 0x0b) ^ gf_mul(a2, 0x0d) ^ gf_mul(a3, 0x09);
			col[1] = gf_mul(a0, 0x09) ^ gf_mul(a1, 0x0e) ^ gf_mul(a2, 0x0b) ^ gf_mul(a3, 0x0d);
			col[2] = gf_mul(a0, 0x0d) ^ gf_mul(a1, 0x09) ^ gf_mul(a2, 0x0e) ^ gf_mul(a3, 0x0b);
			col[3] = gf_mul(a0, 0x0b) ^ gf_mul(a1, 0x0d) ^ gf_mul(a2, 0x09) ^ gf_mul(a3, 0x0e);
		}
	}
	uint8_t t;
	t = s[13]; s[13] = s[9]; s[9] = s[5]; s[5] = s[1]; s[1] = t;
	t = s[2]; s[2] = s[10]; s[10] = t;
	t = s[6]; s[6] = s[14]; s[14] = t;
	t = s[3]; s[3] = s[7]; s[7] = s[11]; s[11] = s[15]; s[15] = t;
	for (int i = 0; i < 16; i++) s[i] = aes_inv_sbox[s[i]] ^ rk[i];
}

static bool aes128_cbc_decrypt_pkcs7(uint8_t *buf, size_t *len_io, const uint8_t key[16], const uint8_t iv_in[16]) {
	size_t len = *len_io;
	if (len == 0 || (len % 16) != 0) {
		return false;
	}
	uint8_t rk[176];
	aes128_expand_key(key, rk);
	uint8_t prev[16];
	memcpy(prev, iv_in, 16);
	for (size_t off = 0; off < len; off += 16) {
		uint8_t next_prev[16];
		memcpy(next_prev, buf + off, 16);
		aes128_decrypt_block(buf + off, rk);
		for (int j = 0; j < 16; j++) {
			buf[off + (size_t)j] ^= prev[j];
		}
		memcpy(prev, next_prev, 16);
	}
	uint8_t pad = buf[len - 1];
	if (pad == 0 || pad > 16 || pad > len) {
		return false;
	}
	for (size_t i = 0; i < pad; i++) {
		if (buf[len - 1 - i] != pad) {
			return false;
		}
	}
	*len_io = len - pad;
	return true;
}

// ---------------------------------------------------------------------------
// Fragmented MP4 (init + moof/mdat) -> ADTS .aac unwrapping for Go+ HQ
// ---------------------------------------------------------------------------

typedef struct {
	bool ready;
	uint8_t profile;   // ADTS profile = AudioObjectType - 1 (1 = AAC-LC)
	uint8_t freq_idx;  // MPEG-4 Sampling Frequency Index (0..12)
	uint8_t chan_cfg;  // Channel Configuration (1..7)
} fmp4_aac_cfg_t;

static uint32_t be32(const uint8_t *p) {
	return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static uint16_t be16(const uint8_t *p) {
	return (uint16_t)(((uint16_t)p[0] << 8) | (uint16_t)p[1]);
}

static uint8_t sample_rate_to_freq_idx(uint32_t sr) {
	static const uint32_t rates[13] = {
		96000, 88200, 64000, 48000, 44100, 32000, 24000, 22050, 16000, 12000, 11025, 8000, 7350,
	};
	for (uint8_t i = 0; i < 13; i++) {
		if (rates[i] == sr) return i;
	}
	return 4; // 44100 fallback
}

static void parse_esds_body(const uint8_t *p, size_t len, fmp4_aac_cfg_t *cfg) {
	if (len < 6) return;
	size_t i = 4; // version + flags
	while (i + 2 <= len) {
		uint8_t tag = p[i++];
		uint32_t dlen = 0;
		for (int k = 0; k < 4 && i < len; k++) {
			uint8_t b = p[i++];
			dlen = (dlen << 7) | (b & 0x7f);
			if (!(b & 0x80)) break;
		}
		if (tag == 0x03) { // ES_Descriptor
			if (i + 3 > len) return;
			uint8_t flags = p[i + 2];
			i += 3;
			if ((flags & 0x80) && i + 2 <= len) i += 2;
			if ((flags & 0x40) && i < len) i += 1 + (size_t)p[i];
			if ((flags & 0x20) && i + 2 <= len) i += 2;
		} else if (tag == 0x04) { // DecoderConfigDescriptor
			if (i + 13 > len) return;
			i += 13;
		} else if (tag == 0x05) { // DecoderSpecificInfo (AudioSpecificConfig)
			if (dlen < 2 || i + 2 > len) return;
			uint8_t b0 = p[i];
			uint8_t b1 = p[i + 1];
			uint8_t aot = (b0 >> 3) & 0x1f;
			uint8_t fidx = (uint8_t)(((b0 & 0x07) << 1) | ((b1 >> 7) & 0x01));
			uint8_t ch = (b1 >> 3) & 0x0f;
			if (fidx < 13 && ch >= 1 && ch <= 7) {
				cfg->profile = (aot >= 1 && aot <= 4) ? (uint8_t)(aot - 1) : 1;
				cfg->freq_idx = fidx;
				cfg->chan_cfg = ch;
				cfg->ready = true;
			}
			return;
		} else {
			if (i + dlen > len) return;
			i += dlen;
		}
	}
}

static void scan_mp4_boxes_for_aac(const uint8_t *buf, size_t len, fmp4_aac_cfg_t *cfg) {
	size_t off = 0;
	while (off + 8 <= len && !cfg->ready) {
		uint64_t bsize = be32(buf + off);
		const uint8_t *type = buf + off + 4;
		size_t hdr = 8;
		if (bsize == 1) {
			if (off + 16 > len) break;
			bsize = ((uint64_t)be32(buf + off + 8) << 32) | be32(buf + off + 12);
			hdr = 16;
		} else if (bsize == 0) {
			bsize = len - off;
		}
		if (bsize < hdr || off + bsize > len) break;
		const uint8_t *body = buf + off + hdr;
		size_t blen = (size_t)(bsize - hdr);

		if (memcmp(type, "moov", 4) == 0 || memcmp(type, "trak", 4) == 0 || memcmp(type, "mdia", 4) == 0 ||
			memcmp(type, "minf", 4) == 0 || memcmp(type, "stbl", 4) == 0) {
			scan_mp4_boxes_for_aac(body, blen, cfg);
		} else if (memcmp(type, "stsd", 4) == 0 && blen > 8) {
			scan_mp4_boxes_for_aac(body + 8, blen - 8, cfg);
		} else if (memcmp(type, "mp4a", 4) == 0 && blen >= 28) {
			uint16_t ch = be16(body + 16);
			uint32_t sr = be32(body + 24) >> 16;
			if (blen > 28) {
				scan_mp4_boxes_for_aac(body + 28, blen - 28, cfg);
			}
			if (!cfg->ready && sr > 0 && ch >= 1 && ch <= 7) {
				cfg->profile = 1; // AAC-LC
				cfg->freq_idx = sample_rate_to_freq_idx(sr);
				cfg->chan_cfg = (uint8_t)ch;
				cfg->ready = true;
			}
		} else if (memcmp(type, "esds", 4) == 0) {
			parse_esds_body(body, blen, cfg);
		}
		off += (size_t)bsize;
	}
}

static bool write_all_fd(int fd, const void *buf, size_t len) {
	const uint8_t *p = buf;
	while (len > 0) {
		ssize_t w = write(fd, p, len);
		if (w < 0) {
			if (errno == EINTR) continue;
			return false;
		}
		p += (size_t)w;
		len -= (size_t)w;
	}
	return true;
}

static bool write_adts_frame(int fd, const fmp4_aac_cfg_t *cfg, const uint8_t *aac, uint32_t aac_len) {
	uint32_t flen = aac_len + 7;
	if (aac_len == 0 || flen >= 8192) {
		return false;
	}
	uint8_t hdr[7];
	hdr[0] = 0xff;
	hdr[1] = 0xf1;
	hdr[2] = (uint8_t)(((cfg->profile & 0x03) << 6) | ((cfg->freq_idx & 0x0f) << 2) | ((cfg->chan_cfg >> 2) & 0x01));
	hdr[3] = (uint8_t)(((cfg->chan_cfg & 0x03) << 6) | ((flen >> 11) & 0x03));
	hdr[4] = (uint8_t)((flen >> 3) & 0xff);
	hdr[5] = (uint8_t)(((flen & 0x07) << 5) | 0x1f);
	hdr[6] = 0xfc;
	return write_all_fd(fd, hdr, sizeof(hdr)) && write_all_fd(fd, aac, aac_len);
}

static size_t parse_moof_sample_sizes(const uint8_t *moof_body, size_t moof_len, uint32_t *sizes, size_t max_samples) {
	size_t count = 0;
	size_t off = 0;
	while (off + 8 <= moof_len) {
		uint32_t bsize = be32(moof_body + off);
		if (bsize < 8 || off + bsize > moof_len) break;
		if (memcmp(moof_body + off + 4, "traf", 4) == 0) {
			const uint8_t *traf = moof_body + off + 8;
			size_t tlen = bsize - 8;
			uint32_t default_size = 0;
			size_t toff = 0;
			while (toff + 8 <= tlen) {
				uint32_t csize = be32(traf + toff);
				if (csize < 8 || toff + csize > tlen) break;
				const uint8_t *ctype = traf + toff + 4;
				const uint8_t *cbody = traf + toff + 8;
				size_t clen = csize - 8;
				if (memcmp(ctype, "tfhd", 4) == 0 && clen >= 8) {
					uint32_t flags = be32(cbody) & 0x00ffffffu;
					size_t p = 8; // version/flags + track_id
					if (flags & 0x000001u) p += 8;
					if (flags & 0x000002u) p += 4;
					if (flags & 0x000008u) p += 4;
					if ((flags & 0x000010u) && p + 4 <= clen) default_size = be32(cbody + p);
				} else if (memcmp(ctype, "trun", 4) == 0 && clen >= 8) {
					uint32_t flags = be32(cbody) & 0x00ffffffu;
					uint32_t scount = be32(cbody + 4);
					size_t p = 8;
					if (flags & 0x000001u) p += 4;
					if (flags & 0x000004u) p += 4;
					for (uint32_t s = 0; s < scount && count < max_samples; s++) {
						if (flags & 0x000100u) p += 4;
						uint32_t sz = default_size;
						if (flags & 0x000200u) {
							if (p + 4 > clen) break;
							sz = be32(cbody + p);
							p += 4;
						}
						if (flags & 0x000400u) p += 4;
						if (flags & 0x000800u) p += 4;
						if (p > clen) break;
						if (sz > 0) sizes[count++] = sz;
					}
				}
				toff += csize;
			}
		}
		off += bsize;
	}
	return count;
}

static size_t strip_id3v2_prefix(const uint8_t *buf, size_t len) {
	size_t off = 0;
	while (len - off >= 10 && buf[off] == 'I' && buf[off + 1] == 'D' && buf[off + 2] == '3') {
		uint32_t tag_len = ((uint32_t)(buf[off + 6] & 0x7f) << 21) | ((uint32_t)(buf[off + 7] & 0x7f) << 14) |
						   ((uint32_t)(buf[off + 8] & 0x7f) << 7) | (uint32_t)(buf[off + 9] & 0x7f);
		size_t total = 10 + (size_t)tag_len + ((buf[off + 5] & 0x10) ? 10 : 0);
		if (off + total > len) break;
		off += total;
	}
	return off;
}

static bool unwrap_fmp4_segment_to_adts(int fd, fmp4_aac_cfg_t *cfg, const uint8_t *seg, size_t seg_len,
										long *written_io) {
	size_t id3_skip = strip_id3v2_prefix(seg, seg_len);
	seg += id3_skip;
	seg_len -= id3_skip;

	// Raw ADTS segment (0xFFFx syncword): pass through directly.
	if (seg_len >= 7 && seg[0] == 0xff && (seg[1] & 0xf0) == 0xf0) {
		if (!write_all_fd(fd, seg, seg_len)) return false;
		*written_io += (long)seg_len;
		return true;
	}

	// SoundCloud's HLS AAC streams sometimes deliver `ftyp` + `moov` as regular
	// segment #0 rather than `#EXT-X-MAP`; scan `moov` dynamically when needed.
	if (!cfg->ready) {
		scan_mp4_boxes_for_aac(seg, seg_len, cfg);
	}
	if (!cfg->ready) {
		return false;
	}

	uint32_t *sizes = malloc(4096 * sizeof(*sizes));
	if (!sizes) {
		return false;
	}
	size_t n_sizes = 0;
	size_t off = 0;
	bool wrote_any = false;
	bool saw_moov = false;

	while (off + 8 <= seg_len) {
		uint64_t bsize = be32(seg + off);
		const uint8_t *btype = seg + off + 4;
		size_t hdr = 8;
		if (bsize == 1) {
			if (off + 16 > seg_len) break;
			bsize = ((uint64_t)be32(seg + off + 8) << 32) | be32(seg + off + 12);
			hdr = 16;
		} else if (bsize == 0) {
			bsize = seg_len - off;
		}
		if (bsize < hdr || off + bsize > seg_len) break;

		const uint8_t *body = seg + off + hdr;
		size_t blen = (size_t)(bsize - hdr);
		if (memcmp(btype, "moov", 4) == 0) {
			saw_moov = true;
			if (!cfg->ready) {
				scan_mp4_boxes_for_aac(body, blen, cfg);
			}
		} else if (memcmp(btype, "moof", 4) == 0) {
			n_sizes = parse_moof_sample_sizes(body, blen, sizes, 4096);
		} else if (memcmp(btype, "mdat", 4) == 0 && n_sizes > 0) {
			size_t mpos = 0;
			for (size_t i = 0; i < n_sizes; i++) {
				if (mpos + sizes[i] > blen) break;
				if (!write_adts_frame(fd, cfg, body + mpos, sizes[i])) {
					free(sizes);
					return false;
				}
				*written_io += (long)(sizes[i] + 7);
				mpos += sizes[i];
				wrote_any = true;
			}
			n_sizes = 0;
		}
		off += (size_t)bsize;
	}

	free(sizes);
	return wrote_any || saw_moov;
}

// ---------------------------------------------------------------------------
// Cache paths, sidecars & bounded LRU eviction
// ---------------------------------------------------------------------------

static bool ensure_dir_tree(const char *dir) {
	char tmp[512];
	if (snprintf(tmp, sizeof(tmp), "%s", dir) >= (int)sizeof(tmp)) {
		return false;
	}
	for (char *p = tmp + 1; *p; p++) {
		if (*p == '/') {
			*p = '\0';
			(void)mkdir(tmp, 0755);
			*p = '/';
		}
	}
	return mkdir(tmp, 0755) == 0 || errno == EEXIST;
}

static const char *codec_ext(soundcloud_codec_t codec) {
	switch (codec) {
	case SOUNDCLOUD_CODEC_OPUS:
		return ".opus";
	case SOUNDCLOUD_CODEC_AAC:
		return ".aac";
	default:
		return ".mp3";
	}
}

static void build_audio_path(long long id, soundcloud_codec_t codec, char *out, size_t size) {
	snprintf(out, size, "%s/%lld%s", cache_dir, id, codec_ext(codec));
}

static void build_incomplete_path(const char *audio_path, char *out, size_t size) {
	snprintf(out, size, "%s.incompleto", audio_path);
}

static void build_tags_path(const char *audio_path, char *out, size_t size) {
	snprintf(out, size, "%s.tags", audio_path);
}

static void build_jpg_path(long long id, char *out, size_t size) {
	snprintf(out, size, "%s/%lld.jpg", cache_dir, id);
}

static bool is_audio_ext(const char *name) {
	const char *dot = strrchr(name, '.');
	return dot && (strcmp(dot, ".mp3") == 0 || strcmp(dot, ".opus") == 0 || strcmp(dot, ".aac") == 0);
}

static bool find_completed_track(long long id, char *out_path, size_t out_size) {
	static const soundcloud_codec_t order[3] = {
		SOUNDCLOUD_CODEC_AAC, SOUNDCLOUD_CODEC_MP3, SOUNDCLOUD_CODEC_OPUS,
	};
	for (int i = 0; i < 3; i++) {
		if (order[i] == SOUNDCLOUD_CODEC_AAC && !aac_supported()) {
			continue;
		}
		char cand[576], inc[600];
		build_audio_path(id, order[i], cand, sizeof(cand));
		build_incomplete_path(cand, inc, sizeof(inc));
		struct stat st, ist;
		if (stat(cand, &st) == 0 && st.st_size > 0 && stat(inc, &ist) != 0 && !growfile_is_growing(cand)) {
			snprintf(out_path, out_size, "%s", cand);
			return true;
		}
	}
	return false;
}

static void sanitize_tag_value(const char *in, char *out, size_t size) {
	size_t j = 0;
	for (size_t i = 0; in && in[i] && j + 1 < size; i++) {
		char c = in[i];
		out[j++] = (c == '\r' || c == '\n') ? ' ' : c;
	}
	out[j] = '\0';
}

static void write_sidecars(const char *audio_path, const soundcloud_track_t *track, int resolved_duration) {
	char tags_path[600], tmp_tags[608];
	build_tags_path(audio_path, tags_path, sizeof(tags_path));
	snprintf(tmp_tags, sizeof(tmp_tags), "%s.tmp", tags_path);

	FILE *f = fopen(tmp_tags, "w");
	if (f) {
		char title[SOUNDCLOUD_TITLE_MAX], artist[SOUNDCLOUD_NAME_MAX], album[SOUNDCLOUD_TITLE_MAX];
		sanitize_tag_value(track->title, title, sizeof(title));
		sanitize_tag_value(track->artist, artist, sizeof(artist));
		sanitize_tag_value(track->album[0] ? track->album : "SoundCloud", album, sizeof(album));
		int dur = resolved_duration > 0 ? resolved_duration : track->duration;
		fprintf(f, "title=%s\nartist=%s\nalbum=%s\n", title, artist, album);
		if (dur > 0) {
			fprintf(f, "duration=%d\n", dur);
		}
		fclose(f);
		(void)rename(tmp_tags, tags_path);
	}

	if (!track->cover[0]) {
		return;
	}
	char jpg_path[576], tmp_jpg[584];
	build_jpg_path(track->id, jpg_path, sizeof(jpg_path));
	struct stat jst;
	if (stat(jpg_path, &jst) == 0 && jst.st_size > 256) {
		return;
	}
	snprintf(tmp_jpg, sizeof(tmp_jpg), "%s.tmp", jpg_path);

	char *img = NULL;
	size_t img_len = 0;
	int status = 0;
	http_req_t req = {
		.user_agent = SOUNDCLOUD_USER_AGENT,
	};
	if (http_request(track->cover, &req, &img, &img_len, COVER_LIMIT, 10, &status) && status >= 200 && status < 300 &&
		img && img_len > 128) {
		int fd = open(tmp_jpg, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
		if (fd >= 0) {
			bool ok = write_all_fd(fd, img, img_len);
			close(fd);
			if (ok) {
				(void)rename(tmp_jpg, jpg_path);
			} else {
				(void)unlink(tmp_jpg);
			}
		}
	}
	free(img);
}

typedef struct {
	char name[256];
	long long id;
	long long bytes;
	time_t mtime;
} cache_entry_t;

static int cmp_entry_mtime(const void *a, const void *b) {
	const cache_entry_t *ea = a, *eb = b;
	if (ea->mtime < eb->mtime) return -1;
	if (ea->mtime > eb->mtime) return 1;
	return 0;
}

static long long bundle_extra_bytes(const char *audio_name, long long id) {
	long long extra = 0;
	struct stat st;
	char p[800];
	snprintf(p, sizeof(p), "%s/%s.tags", cache_dir, audio_name);
	if (stat(p, &st) == 0 && S_ISREG(st.st_mode)) extra += (long long)st.st_size;
	snprintf(p, sizeof(p), "%s/%s.incompleto", cache_dir, audio_name);
	if (stat(p, &st) == 0 && S_ISREG(st.st_mode)) extra += (long long)st.st_size;
	if (id > 0) {
		build_jpg_path(id, p, sizeof(p));
		if (stat(p, &st) == 0 && S_ISREG(st.st_mode)) extra += (long long)st.st_size;
	}
	return extra;
}

static void unlink_track_bundle(const char *audio_name, long long id) {
	char p[800];
	snprintf(p, sizeof(p), "%s/%s", cache_dir, audio_name);
	(void)unlink(p);
	snprintf(p, sizeof(p), "%s/%s.tags", cache_dir, audio_name);
	(void)unlink(p);
	snprintf(p, sizeof(p), "%s/%s.incompleto", cache_dir, audio_name);
	(void)unlink(p);
	if (id > 0) {
		build_jpg_path(id, p, sizeof(p));
		(void)unlink(p);
	}
}

static long long free_disk_bytes(void) {
	struct statvfs vfs;
	if (statvfs(cache_dir, &vfs) != 0) {
		return LLONG_MAX;
	}
	return (long long)vfs.f_bavail * (long long)vfs.f_frsize;
}

static int normalize_max_mb(int max_mb) {
	if (max_mb == 0) {
		return 0; // Unlimited (free-space floor only)
	}
	if (max_mb < 0) {
		return SOUNDCLOUDCACHE_DEFAULT_MAX_MB;
	}
	if (max_mb < SOUNDCLOUDCACHE_MIN_MAX_MB) {
		return SOUNDCLOUDCACHE_MIN_MAX_MB;
	}
	if (max_mb > SOUNDCLOUDCACHE_MAX_MAX_MB) {
		return SOUNDCLOUDCACHE_MAX_MAX_MB;
	}
	return max_mb;
}

static void evict_if_needed(long long active_id) {
	pthread_mutex_lock(&cache_lock);
	if (!cache_dir[0]) {
		pthread_mutex_unlock(&cache_lock);
		return;
	}
	DIR *d = opendir(cache_dir);
	if (!d) {
		pthread_mutex_unlock(&cache_lock);
		return;
	}

	cache_entry_t *entries = NULL;
	size_t count = 0, cap = 0;
	long long total_bytes = 0;
	long long cap_bytes = (cache_max_bytes <= 0) ? LLONG_MAX : cache_max_bytes;

	struct dirent *de;
	while ((de = readdir(d)) != NULL) {
		if (de->d_name[0] == '.') continue;
		char full[800];
		snprintf(full, sizeof(full), "%s/%s", cache_dir, de->d_name);
		struct stat st;
		if (stat(full, &st) != 0 || !S_ISREG(st.st_mode)) continue;
		total_bytes += (long long)st.st_size;

		if (!is_audio_ext(de->d_name)) continue;
		char *end = NULL;
		long long id = strtoll(de->d_name, &end, 10);
		if (id <= 0 || !end || *end != '.') continue;
		if (id == protected_id || id == active_id || growfile_is_growing(full)) continue;

		if (count == cap) {
			size_t ncap = cap ? cap * 2 : 32;
			cache_entry_t * tmp = realloc(entries, ncap * sizeof(*entries));
			if (!tmp) break;
			entries = tmp;
			cap = ncap;
		}
		snprintf(entries[count].name, sizeof(entries[count].name), "%s", de->d_name);
		entries[count].id = id;
		entries[count].bytes = (long long)st.st_size + bundle_extra_bytes(de->d_name, id);
		entries[count].mtime = st.st_mtime;
		count++;
	}
	closedir(d);

	if (count > 1) {
		qsort(entries, count, sizeof(*entries), cmp_entry_mtime);
	}

	for (size_t i = 0; i < count; i++) {
		long long avail = free_disk_bytes();
		if (total_bytes <= cap_bytes && avail >= SOUNDCLOUDCACHE_MIN_FREE) {
			break;
		}
		unlink_track_bundle(entries[i].name, entries[i].id);
		total_bytes -= entries[i].bytes;
	}

	free(entries);
	pthread_mutex_unlock(&cache_lock);
}

// ---------------------------------------------------------------------------
// Cancellation & streamturn coordination
// ---------------------------------------------------------------------------

static bool setup_socket_cb(void *user, int fd) {
	(void)user;
	pthread_mutex_lock(&dl_lock);
	bool cancelled = dl_cancel_flag;
	dl_setup_fd = cancelled ? -1 : fd;
	pthread_mutex_unlock(&dl_lock);
	return !cancelled;
}

static bool is_cancelled(void) {
	pthread_mutex_lock(&dl_lock);
	bool c = dl_cancel_flag;
	pthread_mutex_unlock(&dl_lock);
	return c;
}

static void abandon_active_download(void) {
	pthread_mutex_lock(&dl_lock);
	if (dl_busy) {
		dl_cancel_flag = true;
		if (dl_setup_fd >= 0) {
			(void)shutdown(dl_setup_fd, SHUT_RDWR);
		}
		if (dl_active_stream) {
			http_stream_wake(dl_active_stream);
		}
		while (dl_busy) {
			pthread_cond_wait(&dl_cond, &dl_lock);
		}
	}
	pthread_mutex_unlock(&dl_lock);
}

void soundcloudcache_cancel(void) { abandon_active_download(); }

bool soundcloudcache_downloading(void) {
	pthread_mutex_lock(&dl_lock);
	bool b = dl_busy;
	pthread_mutex_unlock(&dl_lock);
	return b;
}

long long soundcloudcache_current_id(void) {
	pthread_mutex_lock(&dl_lock);
	long long id = dl_busy ? dl_track_id : 0;
	pthread_mutex_unlock(&dl_lock);
	return id;
}

void soundcloudcache_set_root(const char *sd_root) {
	abandon_active_download();
	int cfg_mb = normalize_max_mb(config_get_int("soundcloud", "cache_max_mb", SOUNDCLOUDCACHE_DEFAULT_MAX_MB));
	pthread_mutex_lock(&cache_lock);
	cache_max_mb = cfg_mb;
	cache_max_bytes = (cfg_mb == 0) ? 0 : (long long)cfg_mb * 1024LL * 1024LL;
	if (sd_root && sd_root[0]) {
		snprintf(cache_dir, sizeof(cache_dir), "%s/%s", sd_root, CACHE_SUBDIR);
		if (!ensure_dir_tree(cache_dir)) {
			cache_dir[0] = '\0';
		}
	} else {
		cache_dir[0] = '\0';
	}
	pthread_mutex_unlock(&cache_lock);
	if (cache_dir[0]) {
		streamturn_register_abandon(abandon_active_download);
	}
}

int soundcloudcache_get_max_mb(void) {
	pthread_mutex_lock(&cache_lock);
	int mb = cache_max_mb;
	pthread_mutex_unlock(&cache_lock);
	return mb;
}

void soundcloudcache_set_max_mb(int max_mb) {
	int norm = normalize_max_mb(max_mb);
	pthread_mutex_lock(&cache_lock);
	cache_max_mb = norm;
	cache_max_bytes = (norm == 0) ? 0 : (long long)norm * 1024LL * 1024LL;
	pthread_mutex_unlock(&cache_lock);
	config_set_int("soundcloud", "cache_max_mb", norm);
	config_save();
	evict_if_needed(soundcloudcache_current_id());
}

long long soundcloudcache_get_max_bytes(void) {
	pthread_mutex_lock(&cache_lock);
	long long b = cache_max_bytes;
	pthread_mutex_unlock(&cache_lock);
	return b;
}

void soundcloudcache_set_max_bytes(long long max_bytes) {
	pthread_mutex_lock(&cache_lock);
	if (max_bytes <= 0) {
		cache_max_mb = 0;
		cache_max_bytes = 0;
	} else {
		cache_max_bytes = max_bytes;
		long long mb = (max_bytes + 1024LL * 1024LL - 1LL) / (1024LL * 1024LL);
		if (mb > SOUNDCLOUDCACHE_MAX_MAX_MB) mb = SOUNDCLOUDCACHE_MAX_MAX_MB;
		cache_max_mb = (int)mb;
	}
	pthread_mutex_unlock(&cache_lock);
	evict_if_needed(soundcloudcache_current_id());
}

bool soundcloudcache_available(void) {
	pthread_mutex_lock(&cache_lock);
	bool ok = cache_dir[0] != '\0';
	pthread_mutex_unlock(&cache_lock);
	return ok;
}

const char *soundcloudcache_dir(void) {
	return cache_dir[0] ? cache_dir : NULL;
}

bool soundcloudcache_is_soundcloud_path(const char *path) {
	return path && strstr(path, "/" CACHE_SUBDIR "/") != NULL;
}

long long soundcloudcache_track_id_of(const char *path) {
	if (!soundcloudcache_is_soundcloud_path(path)) {
		return 0;
	}
	const char *slash = strrchr(path, '/');
	if (!slash) {
		return 0;
	}
	char *end = NULL;
	long long id = strtoll(slash + 1, &end, 10);
	return (id > 0 && end && *end == '.') ? id : 0;
}

bool soundcloudcache_placeholder_path(long long id, char *out_path, size_t out_size) {
	if (id <= 0 || !out_path || out_size == 0) {
		return false;
	}
	pthread_mutex_lock(&cache_lock);
	if (!cache_dir[0]) {
		pthread_mutex_unlock(&cache_lock);
		return false;
	}
	int n = snprintf(out_path, out_size, "%s/%lld.mp3", cache_dir, id);
	pthread_mutex_unlock(&cache_lock);
	return n > 0 && (size_t)n < out_size;
}

bool soundcloudcache_find_playable(long long id, char *out_path, size_t out_size) {
	if (id <= 0) {
		return false;
	}
	pthread_mutex_lock(&cache_lock);
	if (!cache_dir[0]) {
		pthread_mutex_unlock(&cache_lock);
		return false;
	}
	char hit[576];
	if (find_completed_track(id, hit, sizeof(hit))) {
		pthread_mutex_unlock(&cache_lock);
		if (out_path && out_size) {
			snprintf(out_path, out_size, "%s", hit);
		}
		return true;
	}
	static const soundcloud_codec_t codecs[3] = {
		SOUNDCLOUD_CODEC_MP3, SOUNDCLOUD_CODEC_OPUS, SOUNDCLOUD_CODEC_AAC,
	};
	for (int i = 0; i < 3; i++) {
		char cand[576];
		build_audio_path(id, codecs[i], cand, sizeof(cand));
		if (growfile_is_growing(cand)) {
			pthread_mutex_unlock(&cache_lock);
			if (out_path && out_size) {
				snprintf(out_path, out_size, "%s", cand);
			}
			return true;
		}
	}
	pthread_mutex_unlock(&cache_lock);
	return false;
}

void soundcloudcache_protect(const char *path) {
	pthread_mutex_lock(&cache_lock);
	protected_id = soundcloudcache_track_id_of(path);
	pthread_mutex_unlock(&cache_lock);
}

void soundcloudcache_clear(void) {
	abandon_active_download();
	pthread_mutex_lock(&cache_lock);
	if (!cache_dir[0]) {
		pthread_mutex_unlock(&cache_lock);
		return;
	}
	DIR *d = opendir(cache_dir);
	if (d) {
		struct dirent *de;
		while ((de = readdir(d)) != NULL) {
			if (de->d_name[0] == '.') continue;
			char full[800];
			snprintf(full, sizeof(full), "%s/%s", cache_dir, de->d_name);
			long long id = soundcloudcache_track_id_of(full);
			if (protected_id > 0 && id == protected_id) continue;
			(void)unlink(full);
		}
		closedir(d);
	}
	pthread_mutex_unlock(&cache_lock);
}

void soundcloudcache_clear_on_exit(void) {
	soundcloudcache_protect(NULL);
	soundcloudcache_clear();
}

void soundcloudcache_stats(int *files, long long *bytes) {
	if (files) *files = 0;
	if (bytes) *bytes = 0;
	pthread_mutex_lock(&cache_lock);
	if (!cache_dir[0]) {
		pthread_mutex_unlock(&cache_lock);
		return;
	}
	DIR *d = opendir(cache_dir);
	if (d) {
		struct dirent *de;
		while ((de = readdir(d)) != NULL) {
			if (de->d_name[0] == '.') continue;
			char full[800];
			snprintf(full, sizeof(full), "%s/%s", cache_dir, de->d_name);
			struct stat st;
			if (stat(full, &st) == 0 && S_ISREG(st.st_mode)) {
				if (bytes) *bytes += (long long)st.st_size;
				if (files && is_audio_ext(de->d_name)) (*files)++;
			}
		}
		closedir(d);
	}
	pthread_mutex_unlock(&cache_lock);
}

// ---------------------------------------------------------------------------
// Progressive MP3 background download
// ---------------------------------------------------------------------------

typedef struct {
	http_stream_t st;
	int fd;
	long written;
	char audio_path[576];
	char inc_path[600];
	soundcloud_track_t track;
	int resolved_duration;
} prog_job_t;

static void finish_dl_slot(void) {
	pthread_mutex_lock(&dl_lock);
	dl_busy = false;
	dl_track_id = 0;
	dl_setup_fd = -1;
	dl_active_stream = NULL;
	pthread_cond_broadcast(&dl_cond);
	pthread_mutex_unlock(&dl_lock);
	streamturn_release();
}

static void *prog_worker_main(void *arg) {
	prog_job_t *job = arg;
	uint8_t buf[16384];
	bool ok = true;

	for (;;) {
		if (is_cancelled()) {
			ok = false;
			break;
		}
		int n = http_stream_read(&job->st, buf, sizeof(buf));
		if (n == 0) {
			break;
		}
		if (n < 0 || !write_all_fd(job->fd, buf, (size_t)n)) {
			ok = false;
			break;
		}
		job->written += n;
		growfile_progress(job->audio_path, job->written);
	}

	pthread_mutex_lock(&dl_lock);
	dl_active_stream = NULL;
	pthread_mutex_unlock(&dl_lock);

	http_stream_close(&job->st);
	close(job->fd);
	growfile_finish(job->audio_path, ok);

	if (ok) {
		(void)unlink(job->inc_path);
		write_sidecars(job->audio_path, &job->track, job->resolved_duration);
	} else {
		(void)unlink(job->audio_path);
		(void)unlink(job->inc_path);
	}

	free(job);
	finish_dl_slot();
	return NULL;
}

static bool start_progressive_download(const soundcloud_track_t *track, const soundcloud_stream_t *stream,
									   bool wait_complete, char *out_path, size_t out_size) {
	prog_job_t *job = calloc(1, sizeof(*job));
	if (!job) {
		set_cache_error("soundcloud_stream_failed");
		finish_dl_slot();
		return false;
	}
	job->fd = -1;
	job->track = *track;
	job->resolved_duration = stream->duration_secs;
	build_audio_path(track->id, stream->codec, job->audio_path, sizeof(job->audio_path));
	build_incomplete_path(job->audio_path, job->inc_path, sizeof(job->inc_path));

	char headers[512];
	soundcloud_headers_for_url(stream->url, headers, sizeof(headers));
	int status = 0;
	http_stream_options_t opts = {
		.socket_cb = setup_socket_cb,
		.no_icy = true,
		.no_https_downgrade = strncasecmp(stream->url, "https://", 8) == 0,
		.user_agent = SOUNDCLOUD_USER_AGENT,
		.extra_headers = headers,
		.status_out = &status,
	};

	bool opened = http_stream_open_ex(&job->st, stream->url, CACHE_TIMEOUT_SECS, &opts);
	pthread_mutex_lock(&dl_lock);
	dl_setup_fd = -1;
	if (opened) {
		dl_active_stream = &job->st;
	}
	pthread_mutex_unlock(&dl_lock);

	if (!opened) {
		free(job);
		finish_dl_slot();
		set_cache_error(status == 403 ? "soundcloud_stream_refused" : "soundcloud_stream_failed");
		return false;
	}

	int inc_fd = open(job->inc_path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
	if (inc_fd >= 0) close(inc_fd);

	job->fd = open(job->audio_path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
	if (job->fd < 0) {
		pthread_mutex_lock(&dl_lock);
		dl_active_stream = NULL;
		pthread_mutex_unlock(&dl_lock);
		http_stream_close(&job->st);
		(void)unlink(job->inc_path);
		free(job);
		finish_dl_slot();
		set_cache_error("soundcloud_cache_write_failed");
		return false;
	}

	long total = job->st.content_length > 0 ? job->st.content_length
											: (long)(stream->duration_secs > 0 ? stream->duration_secs : 180) * 16000L;
	if (total < 65536L) total = 65536L;
	growfile_announce(job->audio_path, total);

	long prebuffer = total / 6;
	if (prebuffer < PROG_PREBUFFER_MIN) prebuffer = PROG_PREBUFFER_MIN;
	if (prebuffer > PROG_PREBUFFER_MAX) prebuffer = PROG_PREBUFFER_MAX;
	if (wait_complete) prebuffer = LONG_MAX;

	long have = 0;
	uint8_t buf[16384];
	bool eof = false;
	bool ok = true;

	while (have < prebuffer) {
		if (is_cancelled()) {
			ok = false;
			break;
		}
		int n = http_stream_read(&job->st, buf, sizeof(buf));
		if (n == 0) {
			eof = true;
			break;
		}
		if (n < 0 || !write_all_fd(job->fd, buf, (size_t)n)) {
			ok = false;
			break;
		}
		have += n;
		job->written = have;
		growfile_progress(job->audio_path, have);
	}

	if (!ok || (eof && have == 0)) {
		pthread_mutex_lock(&dl_lock);
		dl_active_stream = NULL;
		pthread_mutex_unlock(&dl_lock);
		http_stream_close(&job->st);
		close(job->fd);
		growfile_finish(job->audio_path, false);
		(void)unlink(job->audio_path);
		(void)unlink(job->inc_path);
		free(job);
		finish_dl_slot();
		set_cache_error("soundcloud_stream_failed");
		return false;
	}

	if (eof) {
		pthread_mutex_lock(&dl_lock);
		dl_active_stream = NULL;
		pthread_mutex_unlock(&dl_lock);
		http_stream_close(&job->st);
		close(job->fd);
		growfile_finish(job->audio_path, true);
		(void)unlink(job->inc_path);
		write_sidecars(job->audio_path, &job->track, job->resolved_duration);
		snprintf(out_path, out_size, "%s", job->audio_path);
		free(job);
		finish_dl_slot();
		return true;
	}

	write_sidecars(job->audio_path, &job->track, job->resolved_duration);
	snprintf(out_path, out_size, "%s", job->audio_path);

	pthread_t tid;
	pthread_attr_t attr;
	pthread_attr_init(&attr);
	pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
	if (pthread_create(&tid, &attr, prog_worker_main, job) != 0) {
		pthread_attr_destroy(&attr);
		pthread_mutex_lock(&dl_lock);
		dl_active_stream = NULL;
		pthread_mutex_unlock(&dl_lock);
		http_stream_close(&job->st);
		close(job->fd);
		growfile_finish(job->audio_path, false);
		(void)unlink(job->audio_path);
		(void)unlink(job->inc_path);
		free(job);
		finish_dl_slot();
		set_cache_error("soundcloud_stream_failed");
		return false;
	}
	pthread_attr_destroy(&attr);
	return true;
}

// ---------------------------------------------------------------------------
// HLS VOD manifest parsing, AES-128, MPEG-TS / ID3 / fMP4 unwrapping
// ---------------------------------------------------------------------------

typedef struct {
	char *url;
	bool encrypted;
	bool has_iv;
	uint8_t key[16];
	uint8_t iv[16];
} hls_seg_t;

typedef struct {
	uint64_t media_seq;
	char map_url[SOUNDCLOUD_URL_MAX];
	hls_seg_t *segs;
	int seg_count;
} hls_plan_t;

static void free_hls_plan(hls_plan_t *p) {
	if (!p) return;
	for (int i = 0; i < p->seg_count; i++) {
		free(p->segs[i].url);
	}
	free(p->segs);
	memset(p, 0, sizeof(*p));
}

static bool fetch_bytes(const char *url, size_t max_bytes, uint8_t **out_buf, size_t *out_len, int *status_out) {
	*out_buf = NULL;
	*out_len = 0;
	if (status_out) *status_out = 0;

	char headers[512];
	soundcloud_headers_for_url(url, headers, sizeof(headers));
	int status = 0;
	http_stream_options_t opts = {
		.socket_cb = setup_socket_cb,
		.no_icy = true,
		.no_https_downgrade = strncasecmp(url, "https://", 8) == 0,
		.user_agent = SOUNDCLOUD_USER_AGENT,
		.extra_headers = headers,
		.status_out = &status,
	};

	http_stream_t st;
	bool opened = http_stream_open_ex(&st, url, CACHE_TIMEOUT_SECS, &opts);
	pthread_mutex_lock(&dl_lock);
	dl_setup_fd = -1;
	if (opened) dl_active_stream = &st;
	pthread_mutex_unlock(&dl_lock);
	if (status_out) *status_out = status;
	if (!opened) {
		return false;
	}

	size_t cap = st.content_length > 0 && (size_t)st.content_length <= max_bytes ? (size_t)st.content_length + 1
																				  : 65536;
	uint8_t *buf = malloc(cap);
	if (!buf) {
		pthread_mutex_lock(&dl_lock);
		dl_active_stream = NULL;
		pthread_mutex_unlock(&dl_lock);
		http_stream_close(&st);
		return false;
	}

	size_t have = 0;
	bool ok = true;
	for (;;) {
		if (is_cancelled()) {
			ok = false;
			break;
		}
		if (have + 16384 + 1 > cap) {
			size_t ncap = cap * 2;
			if (ncap > max_bytes + 1) ncap = max_bytes + 1;
			if (have + 1 >= ncap) {
				ok = false;
				break;
			}
			uint8_t *nb = realloc(buf, ncap);
			if (!nb) {
				ok = false;
				break;
			}
			buf = nb;
			cap = ncap;
		}
		int n = http_stream_read(&st, buf + have, (int)(cap - have - 1));
		if (n == 0) break;
		if (n < 0) {
			ok = false;
			break;
		}
		have += (size_t)n;
	}

	pthread_mutex_lock(&dl_lock);
	dl_active_stream = NULL;
	pthread_mutex_unlock(&dl_lock);
	http_stream_close(&st);

	if (!ok) {
		free(buf);
		return false;
	}
	buf[have] = '\0';
	*out_buf = buf;
	*out_len = have;
	return true;
}

static bool parse_quoted_attr(const char *line, const char *attr, char *out, size_t size) {
	const char *p = strstr(line, attr);
	if (!p) return false;
	p += strlen(attr);
	if (*p == '"') {
		p++;
		size_t n = 0;
		while (*p && *p != '"' && n + 1 < size) out[n++] = *p++;
		out[n] = '\0';
		return n > 0;
	}
	size_t n = 0;
	while (*p && *p != ',' && !isspace((unsigned char)*p) && n + 1 < size) out[n++] = *p++;
	out[n] = '\0';
	return n > 0;
}

static int hex_val(char c) {
	if (c >= '0' && c <= '9') return c - '0';
	if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
	if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
	return -1;
}

static bool parse_hex_iv(const char *hex, uint8_t iv[16]) {
	if (strncasecmp(hex, "0x", 2) == 0) hex += 2;
	size_t len = strlen(hex);
	if (len == 0 || len > 32) return false;
	memset(iv, 0, 16);
	char padded[33];
	memset(padded, '0', 32);
	padded[32] = '\0';
	memcpy(padded + (32 - len), hex, len);
	for (int i = 0; i < 16; i++) {
		int hi = hex_val(padded[i * 2]);
		int lo = hex_val(padded[i * 2 + 1]);
		if (hi < 0 || lo < 0) return false;
		iv[i] = (uint8_t)((hi << 4) | lo);
	}
	return true;
}

static bool resolve_hls_uri(const char *base_url, const char *rel, char *out, size_t size) {
	if (!base_url || !rel || !rel[0] || !out || size == 0) {
		return false;
	}
	if (strncasecmp(rel, "http://", 7) == 0 || strncasecmp(rel, "https://", 8) == 0) {
		int n = snprintf(out, size, "%s", rel);
		return n > 0 && (size_t)n < size;
	}
	const char *scheme_end = strstr(base_url, "://");
	if (!scheme_end) {
		return false;
	}
	const char *host_start = scheme_end + 3;
	const char *path_start = strchr(host_start, '/');
	if (rel[0] == '/') {
		size_t origin_len = path_start ? (size_t)(path_start - base_url) : strlen(base_url);
		int n = snprintf(out, size, "%.*s%s", (int)origin_len, base_url, rel);
		return n > 0 && (size_t)n < size;
	}
	const char *q = strchr(base_url, '?');
	const char *end = q ? q : base_url + strlen(base_url);
	const char *last_slash = end;
	while (last_slash > host_start && *last_slash != '/') {
		last_slash--;
	}
	size_t dir_len = (*last_slash == '/') ? (size_t)(last_slash - base_url + 1) : strlen(base_url);
	int n = snprintf(out, size, "%.*s%s", (int)dir_len, base_url, rel);
	return n > 0 && (size_t)n < size;
}

static bool load_hls_plan(const char *manifest_url, hls_plan_t *plan) {
	memset(plan, 0, sizeof(*plan));
	char active_url[SOUNDCLOUD_URL_MAX];
	snprintf(active_url, sizeof(active_url), "%s", manifest_url);

	uint8_t *mbytes = NULL;
	size_t mlen = 0;
	if (!fetch_bytes(active_url, MANIFEST_LIMIT, &mbytes, &mlen, NULL) || !mbytes) {
		set_cache_error("soundcloud_stream_failed");
		return false;
	}
	const char *final_url = http_last_final_url();
	if (final_url && final_url[0]) {
		snprintf(active_url, sizeof(active_url), "%s", final_url);
	}

	// Master playlist redirect (#EXT-X-STREAM-INF).
	char variant[SOUNDCLOUD_URL_MAX] = "";
	if (strstr((char *)mbytes, "#EXT-X-STREAM-INF:") &&
		hls_pick_variant((char *)mbytes, active_url, 512, variant, sizeof(variant)) && variant[0]) {
		free(mbytes);
		mbytes = NULL;
		snprintf(active_url, sizeof(active_url), "%s", variant);
		if (!fetch_bytes(active_url, MANIFEST_LIMIT, &mbytes, &mlen, NULL) || !mbytes) {
			set_cache_error("soundcloud_stream_failed");
			return false;
		}
		final_url = http_last_final_url();
		if (final_url && final_url[0]) {
			snprintf(active_url, sizeof(active_url), "%s", final_url);
		}
	}

	int cap = 64;
	plan->segs = calloc((size_t)cap, sizeof(*plan->segs));
	if (!plan->segs) {
		free(mbytes);
		return false;
	}

	bool cur_enc = false;
	bool cur_has_iv = false;
	uint8_t cur_key[16] = {0};
	uint8_t cur_iv[16] = {0};
	char last_key_url[SOUNDCLOUD_URL_MAX] = "";

	char *save = NULL;
	for (char *line = strtok_r((char *)mbytes, "\r\n", &save); line; line = strtok_r(NULL, "\r\n", &save)) {
		while (*line == ' ' || *line == '\t') line++;
		if (!*line) continue;

		if (strncmp(line, "#EXT-X-MEDIA-SEQUENCE:", 22) == 0) {
			plan->media_seq = strtoull(line + 22, NULL, 10);
			continue;
		}
		if (strncmp(line, "#EXT-X-MAP:", 11) == 0) {
			char rel[SOUNDCLOUD_URL_MAX] = "";
			if (parse_quoted_attr(line + 11, "URI=", rel, sizeof(rel))) {
				resolve_hls_uri(active_url, rel, plan->map_url, sizeof(plan->map_url));
			}
			continue;
		}
		if (strncmp(line, "#EXT-X-KEY:", 11) == 0) {
			char method[64] = "";
			(void)parse_quoted_attr(line + 11, "METHOD=", method, sizeof(method));
			if (strcmp(method, "NONE") == 0 || !method[0]) {
				cur_enc = false;
				cur_has_iv = false;
			} else if (strcmp(method, "AES-128") == 0) {
				char kuri_rel[SOUNDCLOUD_URL_MAX] = "", kuri[SOUNDCLOUD_URL_MAX] = "";
				if (!parse_quoted_attr(line + 11, "URI=", kuri_rel, sizeof(kuri_rel)) ||
					!resolve_hls_uri(active_url, kuri_rel, kuri, sizeof(kuri))) {
					free(mbytes);
					free_hls_plan(plan);
					set_cache_error("soundcloud_stream_failed");
					return false;
				}
				if (strcmp(kuri, last_key_url) != 0) {
					uint8_t *kbuf = NULL;
					size_t klen = 0;
					if (!fetch_bytes(kuri, 64, &kbuf, &klen, NULL) || klen != 16) {
						free(kbuf);
						free(mbytes);
						free_hls_plan(plan);
						set_cache_error("soundcloud_stream_failed");
						return false;
					}
					memcpy(cur_key, kbuf, 16);
					free(kbuf);
					snprintf(last_key_url, sizeof(last_key_url), "%s", kuri);
				}
				cur_enc = true;
				char iv_hex[80] = "";
				cur_has_iv = parse_quoted_attr(line + 11, "IV=", iv_hex, sizeof(iv_hex)) &&
							 parse_hex_iv(iv_hex, cur_iv);
			} else {
				// SAMPLE-AES / Widevine CENC.
				free(mbytes);
				free_hls_plan(plan);
				set_cache_error("soundcloud_drm_unsupported");
				return false;
			}
			continue;
		}
		if (line[0] == '#') {
			continue;
		}

		if (plan->seg_count >= HLS_MAX_SEGMENTS) {
			break;
		}
		if (plan->seg_count == cap) {
			int ncap = cap * 2;
			hls_seg_t *nu = realloc(plan->segs, (size_t)ncap * sizeof(*nu));
			if (!nu) break;
			plan->segs = nu;
			cap = ncap;
		}
		char resolved[SOUNDCLOUD_URL_MAX];
		if (resolve_hls_uri(active_url, line, resolved, sizeof(resolved))) {
			hls_seg_t *s = &plan->segs[plan->seg_count];
			memset(s, 0, sizeof(*s));
			s->url = strdup(resolved);
			if (s->url) {
				s->encrypted = cur_enc;
				s->has_iv = cur_has_iv;
				if (cur_enc) {
					memcpy(s->key, cur_key, 16);
					if (cur_has_iv) {
						memcpy(s->iv, cur_iv, 16);
					}
				}
				plan->seg_count++;
			}
		}
	}

	free(mbytes);
	if (plan->seg_count == 0) {
		free_hls_plan(plan);
		set_cache_error("soundcloud_stream_failed");
		return false;
	}
	return true;
}

typedef struct {
	hls_plan_t plan;
	int next_seg;
	int fd;
	long written;
	bool growing_announced;
	int ts_pid;
	int ts_stream_type;
	fmp4_aac_cfg_t aac_cfg;
	char audio_path[576];
	char inc_path[600];
	soundcloud_track_t track;
	soundcloud_stream_t stream;
} hls_job_t;

static bool process_one_hls_segment(hls_job_t *job, int idx, bool *refreshed_once) {
	uint8_t *seg = NULL;
	size_t seg_len = 0;
	int status = 0;

	if (!fetch_bytes(job->plan.segs[idx].url, SEGMENT_LIMIT, &seg, &seg_len, &status)) {
		// Signed CDN URL expired mid-track: re-resolve the manifest once and retry.
		if (status == 403 && !*refreshed_once && !is_cancelled()) {
			*refreshed_once = true;
			soundcloud_stream_t fresh;
			hls_plan_t new_plan;
			if (soundcloud_get_track_stream_ex(&job->track, aac_supported(), false, &fresh) &&
				fresh.kind == SOUNDCLOUD_STREAM_HLS && load_hls_plan(fresh.url, &new_plan) &&
				idx < new_plan.seg_count) {
				free_hls_plan(&job->plan);
				job->plan = new_plan;
				job->stream = fresh;
				if (!fetch_bytes(job->plan.segs[idx].url, SEGMENT_LIMIT, &seg, &seg_len, &status)) {
					return false;
				}
			} else {
				return false;
			}
		} else {
			return false;
		}
	}

	const hls_seg_t *sinfo = &job->plan.segs[idx];
	if (sinfo->encrypted) {
		uint8_t iv[16];
		if (sinfo->has_iv) {
			memcpy(iv, sinfo->iv, 16);
		} else {
			memset(iv, 0, 16);
			uint64_t seq = job->plan.media_seq + (uint64_t)idx;
			for (int b = 0; b < 8; b++) {
				iv[15 - b] = (uint8_t)((seq >> (b * 8)) & 0xff);
			}
		}
		if (!aes128_cbc_decrypt_pkcs7(seg, &seg_len, sinfo->key, iv)) {
			free(seg);
			return false;
		}
	}

	// Unwrap MPEG-TS container if present (sync byte 0x47 every 188 bytes).
	if (seg_len >= 188 && seg[0] == 0x47 && (seg_len < 376 || seg[188] == 0x47)) {
		uint8_t *ts_out = malloc(seg_len);
		if (!ts_out) {
			free(seg);
			return false;
		}
		int extracted = hls_ts_extract(seg, (int)seg_len, ts_out, (int)seg_len, &job->ts_pid, &job->ts_stream_type);
		free(seg);
		if (extracted < 0) {
			free(ts_out);
			return false;
		}
		seg = ts_out;
		seg_len = (size_t)extracted;
	}

	bool ok = true;
	if (job->stream.codec == SOUNDCLOUD_CODEC_AAC) {
		ok = unwrap_fmp4_segment_to_adts(job->fd, &job->aac_cfg, seg, seg_len, &job->written);
	} else {
		size_t skip = (job->stream.codec == SOUNDCLOUD_CODEC_MP3 && idx > 0) ? strip_id3v2_prefix(seg, seg_len) : 0;
		if (seg_len > skip) {
			ok = write_all_fd(job->fd, seg + skip, seg_len - skip);
			if (ok) {
				job->written += (long)(seg_len - skip);
			}
		}
	}

	free(seg);
	if (ok && job->growing_announced) {
		growfile_progress(job->audio_path, job->written);
	}
	return ok;
}

static void *hls_worker_main(void *arg) {
	hls_job_t *job = arg;
	bool refreshed = false;
	bool ok = true;

	for (int i = job->next_seg; i < job->plan.seg_count; i++) {
		if (is_cancelled() || !process_one_hls_segment(job, i, &refreshed)) {
			ok = false;
			break;
		}
	}

	close(job->fd);
	if (job->growing_announced) {
		growfile_finish(job->audio_path, ok && job->written > 0);
	}
	if (ok && job->written > 0) {
		(void)unlink(job->inc_path);
		write_sidecars(job->audio_path, &job->track, job->stream.duration_secs);
	} else {
		(void)unlink(job->audio_path);
		(void)unlink(job->inc_path);
	}

	free_hls_plan(&job->plan);
	free(job);
	finish_dl_slot();
	return NULL;
}

static bool start_hls_download(const soundcloud_track_t *track, const soundcloud_stream_t *stream, bool wait_complete,
							   char *out_path, size_t out_size) {
	hls_job_t *job = calloc(1, sizeof(*job));
	if (!job) {
		finish_dl_slot();
		set_cache_error("soundcloud_stream_failed");
		return false;
	}
	job->fd = -1;
	job->ts_pid = -1;
	job->ts_stream_type = 0;
	job->track = *track;
	job->stream = *stream;
	build_audio_path(track->id, stream->codec, job->audio_path, sizeof(job->audio_path));
	build_incomplete_path(job->audio_path, job->inc_path, sizeof(job->inc_path));

	if (!load_hls_plan(stream->url, &job->plan)) {
		free(job);
		finish_dl_slot();
		return false;
	}

	int inc_fd = open(job->inc_path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
	if (inc_fd >= 0) close(inc_fd);

	job->fd = open(job->audio_path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
	if (job->fd < 0) {
		(void)unlink(job->inc_path);
		free_hls_plan(&job->plan);
		free(job);
		finish_dl_slot();
		set_cache_error("soundcloud_cache_write_failed");
		return false;
	}

	// #EXT-X-MAP init segment: parses esds/mp4a for AAC, or writes Ogg OpusHead/OpusTags header.
	if (job->plan.map_url[0]) {
		uint8_t *init_buf = NULL;
		size_t init_len = 0;
		if (!fetch_bytes(job->plan.map_url, SEGMENT_LIMIT, &init_buf, &init_len, NULL) || !init_buf) {
			free(init_buf);
			close(job->fd);
			(void)unlink(job->audio_path);
			(void)unlink(job->inc_path);
			free_hls_plan(&job->plan);
			free(job);
			finish_dl_slot();
			set_cache_error("soundcloud_stream_failed");
			return false;
		}
		if (stream->codec == SOUNDCLOUD_CODEC_AAC) {
			scan_mp4_boxes_for_aac(init_buf, init_len, &job->aac_cfg);
		} else if (init_len > 0) {
			if (!write_all_fd(job->fd, init_buf, init_len)) {
				free(init_buf);
				close(job->fd);
				(void)unlink(job->audio_path);
				(void)unlink(job->inc_path);
				free_hls_plan(&job->plan);
				free(job);
				finish_dl_slot();
				set_cache_error("soundcloud_cache_write_failed");
				return false;
			}
			job->written += (long)init_len;
		}
		free(init_buf);
	}

	// MP3 and Ogg Opus decode while growing; ADTS .aac finishes downloading
	// first because DECODE_FORMAT_AAC_ADTS builds its seek table on open.
	bool can_grow = (stream->codec != SOUNDCLOUD_CODEC_AAC) && !wait_complete;
	if (can_grow) {
		int secs = stream->duration_secs > 0 ? stream->duration_secs : (track->duration > 0 ? track->duration : 180);
		long rate = stream->codec == SOUNDCLOUD_CODEC_OPUS ? 9000L : 16000L;
		long est = (long)secs * rate;
		if (est < 65536L) est = 65536L;
		growfile_announce(job->audio_path, est);
		job->growing_announced = true;
		if (job->written > 0) {
			growfile_progress(job->audio_path, job->written);
		}
	}

	bool refreshed = false;
	bool ok = true;
	int seg_idx = 0;
	long target_bytes = can_grow ? HLS_PREBUFFER_BYTES : LONG_MAX;

	while (seg_idx < job->plan.seg_count && job->written < target_bytes) {
		if (is_cancelled() || !process_one_hls_segment(job, seg_idx, &refreshed)) {
			ok = false;
			break;
		}
		seg_idx++;
	}
	job->next_seg = seg_idx;

	if (!ok || job->written == 0) {
		close(job->fd);
		if (job->growing_announced) {
			growfile_finish(job->audio_path, false);
		}
		(void)unlink(job->audio_path);
		(void)unlink(job->inc_path);
		free_hls_plan(&job->plan);
		free(job);
		finish_dl_slot();
		if (!cache_error[0]) {
			set_cache_error("soundcloud_stream_failed");
		}
		return false;
	}

	if (job->next_seg >= job->plan.seg_count) {
		close(job->fd);
		if (job->growing_announced) {
			growfile_finish(job->audio_path, true);
		}
		(void)unlink(job->inc_path);
		write_sidecars(job->audio_path, &job->track, job->stream.duration_secs);
		snprintf(out_path, out_size, "%s", job->audio_path);
		free_hls_plan(&job->plan);
		free(job);
		finish_dl_slot();
		return true;
	}

	write_sidecars(job->audio_path, &job->track, job->stream.duration_secs);
	snprintf(out_path, out_size, "%s", job->audio_path);

	pthread_t tid;
	pthread_attr_t attr;
	pthread_attr_init(&attr);
	pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
	if (pthread_create(&tid, &attr, hls_worker_main, job) != 0) {
		pthread_attr_destroy(&attr);
		close(job->fd);
		if (job->growing_announced) {
			growfile_finish(job->audio_path, false);
		}
		(void)unlink(job->audio_path);
		(void)unlink(job->inc_path);
		free_hls_plan(&job->plan);
		free(job);
		finish_dl_slot();
		set_cache_error("soundcloud_stream_failed");
		return false;
	}
	pthread_attr_destroy(&attr);
	return true;
}

// ---------------------------------------------------------------------------
// Public entry points: soundcloudcache_get & soundcloudcache_prefetch
// ---------------------------------------------------------------------------

static bool fetch_track_internal(const soundcloud_track_t *track, bool is_prefetch, char *out_path, size_t out_size) {
	cache_error[0] = '\0';
	if (!track || track->id <= 0) {
		set_cache_error("soundcloud_invalid_id");
		return false;
	}
	if (!soundcloudcache_available()) {
		set_cache_error("soundcloud_no_card");
		return false;
	}

	char hit[576];
	if (find_completed_track(track->id, hit, sizeof(hit))) {
		write_sidecars(hit, track, track->duration);
		if (out_path && out_size) {
			snprintf(out_path, out_size, "%s", hit);
		}
		return true;
	}

	if (is_prefetch) {
		pthread_mutex_lock(&dl_lock);
		if (dl_busy) {
			pthread_mutex_unlock(&dl_lock);
			return false;
		}
		pthread_mutex_unlock(&dl_lock);
	}

	streamturn_acquire();

	pthread_mutex_lock(&dl_lock);
	if (dl_busy) {
		pthread_mutex_unlock(&dl_lock);
		streamturn_release();
		return false;
	}
	dl_busy = true;
	dl_cancel_flag = false;
	dl_track_id = track->id;
	dl_setup_fd = -1;
	dl_active_stream = NULL;
	pthread_mutex_unlock(&dl_lock);

	// Re-check after acquiring the slot in case prefetch just finished this track!
	if (find_completed_track(track->id, hit, sizeof(hit))) {
		finish_dl_slot();
		if (out_path && out_size) {
			snprintf(out_path, out_size, "%s", hit);
		}
		return true;
	}

	evict_if_needed(track->id);
	if (free_disk_bytes() < 16LL * 1024 * 1024) {
		finish_dl_slot();
		set_cache_error("soundcloud_card_full");
		return false;
	}

	soundcloud_stream_t stream;
	if (!soundcloud_get_track_stream(track, aac_supported(), &stream)) {
		finish_dl_slot();
		const char *err = soundcloud_last_error();
		set_cache_error(err && err[0] ? err : "soundcloud_stream_failed");
		return false;
	}

	char produced[576] = "";
	bool ok;
	if (stream.kind == SOUNDCLOUD_STREAM_PROGRESSIVE) {
		ok = start_progressive_download(track, &stream, is_prefetch, produced, sizeof(produced));
		// If the CDN rejected or 404'd the progressive MP3 asset itself, retry
		// once using an HLS transcoding (`opus_0_0` / `aac_160k` / `hls_mp3`).
		if (!ok) {
			streamturn_acquire();
			pthread_mutex_lock(&dl_lock);
			if (!dl_busy) {
				dl_busy = true;
				dl_cancel_flag = false;
				dl_track_id = track->id;
				dl_setup_fd = -1;
				dl_active_stream = NULL;
				pthread_mutex_unlock(&dl_lock);
				soundcloud_stream_t hls_stream;
				if (soundcloud_get_track_stream_ex(track, aac_supported(), false, &hls_stream)) {
					ok = start_hls_download(track, &hls_stream, is_prefetch, produced, sizeof(produced));
				} else {
					finish_dl_slot();
				}
			} else {
				pthread_mutex_unlock(&dl_lock);
				streamturn_release();
			}
		}
	} else {
		ok = start_hls_download(track, &stream, is_prefetch, produced, sizeof(produced));
	}

	if (ok && out_path && out_size) {
		snprintf(out_path, out_size, "%s", produced);
	}
	return ok;
}

bool soundcloudcache_get(const soundcloud_track_t *track, char *out_path, size_t out_size) {
	return fetch_track_internal(track, false, out_path, out_size);
}

bool soundcloudcache_prefetch(const soundcloud_track_t *track) {
	char dummy[576];
	return fetch_track_internal(track, true, dummy, sizeof(dummy));
}
