#include "stockkeys.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

#include "src/system/core/config.h"
#include "src/system/core/sha256.h"
#include "src/system/core/utils.h"
#include "src/system/device/sysinfo.h"
#include "src/system/device/system.h"
#include "src/system/image/miniz/miniz_tinfl.h"
#include "src/system/net/http.h"
#include "src/system/net/wifi.h"
#include "src/system/streaming/streamkeys.h"

#define DEFAULT_OTA_REPO "mr-f0xx/sonix-player-tempotec-v1"

// Known size of the bundled V1 1.3 split archive (2 * 18,191,074 bytes), used
// as an initial progress bar estimate before Content-Length headers arrive.
#define BUNDLED_PART_SIZE_EST 18191074L

#define DOWNLOAD_TIMEOUT_SECS 30
#define SPACE_NEEDED_BYTES (48ULL * 1024ULL * 1024ULL)
#define MAX_UPT_UNPACK_BYTES (128ULL * 1024ULL * 1024ULL)
#define STOCKKEYS_MAX_ELF (16u * 1024u * 1024u)

#define TIDAL_MAX_SECRET_DISTANCE 16384u
#define MIN_PRINTABLE_RUN 4u

#ifdef HOST_BUILD
#define INTERNAL_DATA_DIR "usr/data"
#else
#define INTERNAL_DATA_DIR "/usr/data"
#endif

static void wipe_mem(void *p, size_t n) {
	volatile unsigned char *v = p;
	while (n--) {
		*v++ = 0;
	}
}

static uint16_t rd16le(const uint8_t *p) { return (uint16_t)(p[0] | ((uint16_t)p[1] << 8)); }

static uint32_t rd32le(const uint8_t *p) {
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t rd64le(const uint8_t *p) {
	return (uint64_t)rd32le(p) | ((uint64_t)rd32le(p + 4) << 32);
}

// ===========================================================================
// 1. Safe LZO1X decompressor (SquashFS compression == 3)
// ===========================================================================

bool stockkeys_lzo1x_decompress(const uint8_t *src, size_t src_len, uint8_t *dst, size_t dst_cap, size_t *out_len) {
	if (out_len) {
		*out_len = 0;
	}
	if (!src || !dst) {
		return false;
	}
	if (src_len == 0) {
		return true;
	}

	const uint8_t *ip = src;
	const uint8_t *const ip_end = src + src_len;
	uint8_t *op = dst;
	uint8_t *const op_end = dst + dst_cap;

#define LZO_NEED_IN(n)                                                                                                 \
	do {                                                                                                               \
		if ((size_t)(ip_end - ip) < (size_t)(n)) {                                                                     \
			return false;                                                                                              \
		}                                                                                                              \
	} while (0)

#define LZO_COPY_LITS(cnt)                                                                                             \
	do {                                                                                                               \
		size_t _c = (size_t)(cnt);                                                                                     \
		LZO_NEED_IN(_c);                                                                                               \
		if ((size_t)(op_end - op) < _c) {                                                                              \
			return false;                                                                                              \
		}                                                                                                              \
		memcpy(op, ip, _c);                                                                                            \
		ip += _c;                                                                                                      \
		op += _c;                                                                                                      \
	} while (0)

#define LZO_COPY_MATCH(back_dist, mlen)                                                                                \
	do {                                                                                                               \
		size_t _d = (size_t)(back_dist);                                                                               \
		size_t _m = (size_t)(mlen);                                                                                    \
		if (_d == 0 || (size_t)(op - dst) < _d || (size_t)(op_end - op) < _m) {                                        \
			return false;                                                                                              \
		}                                                                                                              \
		const uint8_t *_mp = op - _d;                                                                                  \
		if (_d >= _m) {                                                                                                \
			memcpy(op, _mp, _m);                                                                                       \
			op += _m;                                                                                                  \
		} else {                                                                                                       \
			for (size_t _i = 0; _i < _m; _i++) {                                                                       \
				*op++ = *_mp++;                                                                                        \
			}                                                                                                          \
		}                                                                                                              \
	} while (0)

	bool enter_match = false;
	LZO_NEED_IN(1);
	size_t t = *ip++;

	if (t > 17) {
		t -= 17;
		LZO_COPY_LITS(t);
		LZO_NEED_IN(1);
		t = *ip++;
		if (t < 4 || t >= 16) {
			enter_match = true;
		} else {
			size_t t_next = t & 3u;
			LZO_NEED_IN(1);
			size_t back = 0x0801u + (t >> 2) + ((size_t)(*ip++) << 2);
			LZO_COPY_MATCH(back, 3);
			if (t_next) {
				LZO_COPY_LITS(t_next);
				enter_match = true;
			}
			LZO_NEED_IN(1);
			t = *ip++;
		}
	}

	for (;;) {
		if (!enter_match && t < 16) {
			if (t == 0) {
				for (;;) {
					LZO_NEED_IN(1);
					uint8_t b = *ip++;
					if (b != 0) {
						t += 15u + b;
						break;
					}
					if (t > dst_cap) {
						return false;
					}
					t += 255u;
				}
			}
			LZO_COPY_LITS(t + 3u);
			LZO_NEED_IN(1);
			t = *ip++;
			if (t < 16) {
				size_t t_next = t & 3u;
				LZO_NEED_IN(1);
				size_t back = 0x0801u + (t >> 2) + ((size_t)(*ip++) << 2);
				LZO_COPY_MATCH(back, 3);
				if (t_next == 0) {
					LZO_NEED_IN(1);
					t = *ip++;
					continue;
				}
				LZO_COPY_LITS(t_next);
				LZO_NEED_IN(1);
				t = *ip++;
			}
		}
		enter_match = false;

		for (;;) {
			size_t back = 0;
			size_t m_len = 0;
			size_t t_next = 0;

			if (t >= 64) {
				t_next = t & 3u;
				LZO_NEED_IN(1);
				back = 1u + ((t >> 2) & 7u) + ((size_t)(*ip++) << 3);
				m_len = (t >> 5) + 1u;
			} else if (t >= 32) {
				m_len = t & 31u;
				if (m_len == 0) {
					for (;;) {
						LZO_NEED_IN(1);
						uint8_t b = *ip++;
						if (b != 0) {
							m_len += 31u + b;
							break;
						}
						if (m_len > dst_cap) {
							return false;
						}
						m_len += 255u;
					}
				}
				m_len += 2u;
				LZO_NEED_IN(2);
				uint32_t v = (uint32_t)ip[0] | ((uint32_t)ip[1] << 8);
				ip += 2;
				back = 1u + (v >> 2);
				t_next = v & 3u;
			} else if (t >= 16) {
				size_t m_off = (t & 8u) << 11;
				m_len = t & 7u;
				if (m_len == 0) {
					for (;;) {
						LZO_NEED_IN(1);
						uint8_t b = *ip++;
						if (b != 0) {
							m_len += 7u + b;
							break;
						}
						if (m_len > dst_cap) {
							return false;
						}
						m_len += 255u;
					}
				}
				m_len += 2u;
				LZO_NEED_IN(2);
				uint32_t v = (uint32_t)ip[0] | ((uint32_t)ip[1] << 8);
				ip += 2;
				m_off += v >> 2;
				if (m_off == 0) {
					if (out_len) {
						*out_len = (size_t)(op - dst);
					}
					return true;
				}
				back = m_off + 0x4000u;
				t_next = v & 3u;
			} else {
				t_next = t & 3u;
				LZO_NEED_IN(1);
				back = 1u + (t >> 2) + ((size_t)(*ip++) << 2);
				m_len = 2u;
			}

			LZO_COPY_MATCH(back, m_len);
			if (t_next == 0) {
				LZO_NEED_IN(1);
				t = *ip++;
				break;
			}
			LZO_COPY_LITS(t_next);
			LZO_NEED_IN(1);
			t = *ip++;
		}
	}

#undef LZO_NEED_IN
#undef LZO_COPY_LITS
#undef LZO_COPY_MATCH
}

// ===========================================================================
// 2. Credential extractor (exact C match of tools/extract_streamkeys.py)
// ===========================================================================

typedef struct {
	uint32_t off;
	uint32_t len;
} str_span_t;

static bool ascii_ieq_substr(const uint8_t *hay, size_t hlen, const char *needle) {
	size_t nlen = strlen(needle);
	if (nlen == 0 || hlen < nlen) {
		return false;
	}
	for (size_t i = 0; i + nlen <= hlen; i++) {
		size_t j = 0;
		while (j < nlen && tolower(hay[i + j]) == tolower((unsigned char)needle[j])) {
			j++;
		}
		if (j == nlen) {
			return true;
		}
	}
	return false;
}

static bool is_qobuz_strong_anchor(const uint8_t *s, size_t n) {
	return ascii_ieq_substr(s, n, "qobuz.com/api.json") || ascii_ieq_substr(s, n, "device_manufacturer_id") ||
		   ascii_ieq_substr(s, n, "trackgetfileurl") || ascii_ieq_substr(s, n, "request_sig") ||
		   ascii_ieq_substr(s, n, "x-app-id");
}

static bool is_qobuz_anchor(const uint8_t *s, size_t n) {
	return is_qobuz_strong_anchor(s, n) || ascii_ieq_substr(s, n, "qobuz");
}

static bool is_tidal_strong_anchor(const uint8_t *s, size_t n) {
	return ascii_ieq_substr(s, n, "auth.tidal.com/v1/oauth2/device_authorization") ||
		   ascii_ieq_substr(s, n, "auth.tidal.com/v1/oauth2/token") ||
		   ascii_ieq_substr(s, n, "urn:ietf:params:oauth:grant-type:device_code") ||
		   ascii_ieq_substr(s, n, "playbackinfopostpaywall");
}

static bool is_tidal_anchor(const uint8_t *s, size_t n) {
	return is_tidal_strong_anchor(s, n) || ascii_ieq_substr(s, n, "auth.tidal.com/v1/oauth2") ||
		   ascii_ieq_substr(s, n, "x-tidal-token");
}

static bool is_qobuz_app_id(const uint8_t *s, size_t n) {
	if (n < 5 || n > 12) {
		return false;
	}
	for (size_t i = 0; i < n; i++) {
		if (s[i] < '0' || s[i] > '9') {
			return false;
		}
	}
	return true;
}

// 1 = 32 lowercase hex, 2 = 32 alphanumeric fallback, 0 = neither.
static int qobuz_secret_kind(const uint8_t *s, size_t n) {
	if (n != 32) {
		return 0;
	}
	bool all_hex = true;
	for (size_t i = 0; i < 32; i++) {
		uint8_t c = s[i];
		bool digit = c >= '0' && c <= '9';
		bool lowhex = c >= 'a' && c <= 'f';
		bool alpha = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
		if (!(digit || alpha)) {
			return 0;
		}
		if (!(digit || lowhex)) {
			all_hex = false;
		}
	}
	return all_hex ? 1 : 2;
}

static bool is_tidal_b64_secret(const uint8_t *s, size_t n) {
	if (n != 44 || s[43] != '=') {
		return false;
	}
	bool has_upper = false, has_lower = false, has_digit_or_sym = false;
	for (size_t i = 0; i < 43; i++) {
		uint8_t c = s[i];
		if (c >= 'A' && c <= 'Z') {
			has_upper = true;
		} else if (c >= 'a' && c <= 'z') {
			has_lower = true;
		} else if ((c >= '0' && c <= '9') || c == '+' || c == '/') {
			has_digit_or_sym = true;
		} else {
			return false;
		}
	}
	return (has_upper && has_lower) || (has_lower && has_digit_or_sym) || (has_upper && has_digit_or_sym);
}

static bool is_tidal_rodata_id(const uint8_t *s, size_t n) {
	if (n != 16) {
		return false;
	}
	static const char *const NOISE[] = {
		"device_authorize", "application/json", "www-authenticate", "content-encoding", "transfer-encodin",
	};
	for (size_t i = 0; i < sizeof(NOISE) / sizeof(NOISE[0]); i++) {
		if (strncasecmp((const char *)s, NOISE[i], 16) == 0) {
			return false;
		}
	}
	bool has_letter = false, has_digit = false;
	for (size_t i = 0; i < 16; i++) {
		uint8_t c = s[i];
		if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')) {
			has_letter = true;
		} else if (c >= '0' && c <= '9') {
			has_digit = true;
		} else if (c != '_' && c != '-') {
			return false;
		}
	}
	return has_letter && has_digit;
}

static uint32_t nearest_anchor(uint32_t off, const uint32_t *anchors, size_t count) {
	uint32_t best = UINT32_MAX;
	for (size_t i = 0; i < count; i++) {
		uint32_t d = off >= anchors[i] ? (off - anchors[i]) : (anchors[i] - off);
		if (d < best) {
			best = d;
		}
	}
	return best;
}

typedef struct {
	char value[64];
	uint32_t score;
} scored_hit_t;

typedef struct {
	scored_hit_t *items;
	size_t count;
	size_t cap;
} hit_list_t;

static bool record_hit(hit_list_t *list, const uint8_t *s, size_t len, uint32_t score) {
	if (len == 0 || len >= sizeof(list->items[0].value)) {
		return false;
	}
	for (size_t i = 0; i < list->count; i++) {
		if (strlen(list->items[i].value) == len && memcmp(list->items[i].value, s, len) == 0) {
			if (score < list->items[i].score) {
				list->items[i].score = score;
			}
			return true;
		}
	}
	if (list->count == list->cap) {
		size_t ncap = list->cap ? list->cap * 2u : 32u;
		scored_hit_t *nitems = realloc(list->items, ncap * sizeof(*nitems));
		if (!nitems) {
			return false;
		}
		list->items = nitems;
		list->cap = ncap;
	}
	memcpy(list->items[list->count].value, s, len);
	list->items[list->count].value[len] = '\0';
	list->items[list->count].score = score;
	list->count++;
	return true;
}

static bool choose_unique_best(const hit_list_t *list, char *out, size_t out_size) {
	if (list->count == 0) {
		return false;
	}
	uint32_t best = list->items[0].score;
	size_t best_idx = 0;
	for (size_t i = 1; i < list->count; i++) {
		if (list->items[i].score < best) {
			best = list->items[i].score;
			best_idx = i;
		}
	}
	size_t winners = 0;
	for (size_t i = 0; i < list->count; i++) {
		if (list->items[i].score == best) {
			winners++;
		}
	}
	if (winners != 1) {
		return false;
	}
	snprintf(out, out_size, "%s", list->items[best_idx].value);
	return true;
}

typedef struct {
	uint32_t vaddr;
	uint32_t foff;
	uint32_t filesz;
} elf_seg_t;

static size_t parse_elf32_le_segments(const uint8_t *buf, size_t len, elf_seg_t *segs, size_t max_segs) {
	if (len < 52 || memcmp(buf, "\x7f" "ELF", 4) != 0 || buf[4] != 1 || buf[5] != 1) {
		return 0;
	}
	uint32_t phoff = rd32le(buf + 28);
	uint16_t phentsize = rd16le(buf + 42);
	uint16_t phnum = rd16le(buf + 44);
	if (phentsize < 32) {
		return 0;
	}
	size_t n = 0;
	for (uint16_t i = 0; i < phnum && n < max_segs; i++) {
		size_t off = (size_t)phoff + (size_t)i * phentsize;
		if (off + 32 > len) {
			break;
		}
		uint32_t p_type = rd32le(buf + off);
		uint32_t p_off = rd32le(buf + off + 4);
		uint32_t p_vaddr = rd32le(buf + off + 8);
		uint32_t p_filesz = rd32le(buf + off + 16);
		if (p_type == 1 && p_filesz > 0 && (uint64_t)p_off + p_filesz <= len) {
			segs[n].vaddr = p_vaddr;
			segs[n].foff = p_off;
			segs[n].filesz = p_filesz;
			n++;
		}
	}
	return n;
}

static bool va_is_client_id_label(const uint8_t *buf, size_t len, const elf_seg_t *segs, size_t nsegs, uint32_t va) {
	for (size_t i = 0; i < nsegs; i++) {
		if (va >= segs[i].vaddr && va < segs[i].vaddr + segs[i].filesz) {
			size_t start = (size_t)segs[i].foff + (size_t)(va - segs[i].vaddr);
			if (start + 10 <= len && memcmp(buf + start, "client_id\0", 10) == 0) {
				return true;
			}
			return false;
		}
	}
	return false;
}

typedef struct {
	int kind; // 0 = unset, 1 = val, 2 = sym
	uint32_t val;
	int sym_reg;
	int32_t sym_off;
} mips_reg_t;

typedef struct {
	bool used;
	int64_t addr;
	uint8_t byte_val;
	size_t pos;
} sb_cell_t;

typedef struct {
	sb_cell_t cells[64];
} sb_space_t;

static void space_put(sb_space_t *sp, int64_t addr, uint8_t bval, size_t pos) {
	for (size_t i = 0; i < 64; i++) {
		if (sp->cells[i].used && pos > sp->cells[i].pos && pos - sp->cells[i].pos > 256) {
			memset(sp, 0, sizeof(*sp));
			break;
		}
	}
	size_t free_idx = 64;
	size_t oldest_idx = 0;
	for (size_t i = 0; i < 64; i++) {
		if (sp->cells[i].used && sp->cells[i].addr == addr) {
			sp->cells[i].byte_val = bval;
			sp->cells[i].pos = pos;
			return;
		}
		if (!sp->cells[i].used && free_idx == 64) {
			free_idx = i;
		}
		if (sp->cells[i].used && sp->cells[i].pos < sp->cells[oldest_idx].pos) {
			oldest_idx = i;
		}
	}
	size_t idx = free_idx < 64 ? free_idx : oldest_idx;
	sp->cells[idx].used = true;
	sp->cells[idx].addr = addr;
	sp->cells[idx].byte_val = bval;
	sp->cells[idx].pos = pos;
}

static const sb_cell_t *space_get(const sb_space_t *sp, int64_t addr) {
	for (size_t i = 0; i < 64; i++) {
		if (sp->cells[i].used && sp->cells[i].addr == addr) {
			return &sp->cells[i];
		}
	}
	return NULL;
}

static void space_del_range(sb_space_t *sp, int64_t start_a, int64_t end_a) {
	for (size_t i = 0; i < 64; i++) {
		if (sp->cells[i].used && sp->cells[i].addr >= start_a && sp->cells[i].addr <= end_a) {
			sp->cells[i].used = false;
		}
	}
}

static bool scan_mips_tidal_client_id(const uint8_t *buf, size_t len, char *out, size_t out_size, bool *found_any) {
	*found_any = false;
	elf_seg_t segs[16];
	size_t nsegs = parse_elf32_le_segments(buf, len, segs, 16);

	mips_reg_t regs[32];
	memset(regs, 0, sizeof(regs));
	regs[0].kind = 1;
	regs[0].val = 0;

	// 32 register-relative spaces + 1 absolute space (index 32).
	sb_space_t *spaces = calloc(33, sizeof(*spaces));
	if (!spaces) {
		return false;
	}

	hit_list_t tagged = {0};
	hit_list_t untagged = {0};
	size_t limit = len & ~(size_t)3u;

	for (size_t pos = 0; pos < limit; pos += 4) {
		uint32_t w = rd32le(buf + pos);
		uint32_t op = (w >> 26) & 0x3Fu;
		uint32_t rs = (w >> 21) & 0x1Fu;
		uint32_t rt = (w >> 16) & 0x1Fu;
		uint32_t imm = w & 0xFFFFu;
		int32_t simm = (imm & 0x8000u) ? (int32_t)imm - 0x10000 : (int32_t)imm;

		if (op == 0x0Fu) { // lui rt, imm
			if (rt != 0) {
				regs[rt].kind = 1;
				regs[rt].val = imm << 16;
			}
		} else if (op == 0x09u) { // addiu rt, rs, simm
			if (rt != 0) {
				if (rs == 0) {
					regs[rt].kind = 1;
					regs[rt].val = (uint32_t)simm;
				} else if (regs[rs].kind == 1) {
					regs[rt].kind = 1;
					regs[rt].val = regs[rs].val + (uint32_t)simm;
				} else if (regs[rs].kind == 2) {
					regs[rt].kind = 2;
					regs[rt].sym_reg = regs[rs].sym_reg;
					regs[rt].sym_off = regs[rs].sym_off + simm;
				} else {
					regs[rt].kind = 2;
					regs[rt].sym_reg = (int)rs;
					regs[rt].sym_off = simm;
				}
			}
		} else if (op == 0x0Du) { // ori rt, rs, imm
			if (rt != 0) {
				if (rs == 0) {
					regs[rt].kind = 1;
					regs[rt].val = imm;
				} else if (regs[rs].kind == 1) {
					regs[rt].kind = 1;
					regs[rt].val = regs[rs].val | imm;
				} else {
					regs[rt].kind = 0;
				}
			}
		} else if (op == 0x28u) { // sb rt, simm(rs)
			bool val_ok = (rt == 0) || (regs[rt].kind == 1 && regs[rt].val <= 0x7Fu);
			if (val_ok) {
				uint8_t bval = (rt == 0) ? 0u : (uint8_t)regs[rt].val;
				size_t sp_idx = rs;
				int64_t addr = simm;
				if (regs[rs].kind == 1) {
					sp_idx = 32;
					addr = (int64_t)(uint32_t)(regs[rs].val + (uint32_t)simm);
				} else if (regs[rs].kind == 2) {
					sp_idx = (size_t)regs[rs].sym_reg;
					addr = (int64_t)regs[rs].sym_off + simm;
				}
				sb_space_t *sp = &spaces[sp_idx];
				space_put(sp, addr, bval, pos);
				if (bval == 0) {
					int64_t end_a = addr;
					int64_t start_a = end_a;
					for (;;) {
						const sb_cell_t *prev = space_get(sp, start_a - 1);
						if (!prev || prev->byte_val < 0x21u || prev->byte_val > 0x7Eu) {
							break;
						}
						start_a--;
						if (end_a - start_a > 32) {
							break;
						}
					}
					int64_t tlen = end_a - start_a;
					if (tlen >= 12 && tlen <= 24) {
						uint8_t tok[25];
						bool tok_ok = true;
						for (int64_t i = 0; i < tlen; i++) {
							const sb_cell_t *c = space_get(sp, start_a + i);
							uint8_t ch = c ? c->byte_val : 0;
							if (!((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') ||
								  (ch >= '0' && ch <= '9') || ch == '_' || ch == '-')) {
								tok_ok = false;
								break;
							}
							tok[i] = ch;
						}
						if (tok_ok) {
							bool has_label = false;
							if (nsegs > 0) {
								bool has_tv[32] = {false};
								uint32_t tv[32] = {0};
								for (int r = 0; r < 32; r++) {
									if (regs[r].kind == 1) {
										has_tv[r] = true;
										tv[r] = regs[r].val;
									}
								}
								size_t look_end = pos + 96 < limit ? pos + 96 : limit;
								for (size_t la = pos + 4; la < look_end; la += 4) {
									uint32_t lw = rd32le(buf + la);
									uint32_t lop = (lw >> 26) & 0x3Fu;
									uint32_t lrs = (lw >> 21) & 0x1Fu;
									uint32_t lrt = (lw >> 16) & 0x1Fu;
									uint32_t limm = lw & 0xFFFFu;
									int32_t lsimm = (limm & 0x8000u) ? (int32_t)limm - 0x10000 : (int32_t)limm;
									if (lop == 0x0Fu && lrt != 0) {
										has_tv[lrt] = true;
										tv[lrt] = limm << 16;
									} else if (lop == 0x09u && lrs != 0 && has_tv[lrs]) {
										uint32_t va = tv[lrs] + (uint32_t)lsimm;
										if (va_is_client_id_label(buf, len, segs, nsegs, va)) {
											has_label = true;
										}
										if (lrt != 0) {
											has_tv[lrt] = true;
											tv[lrt] = va;
										}
									}
								}
							}
							if (has_label) {
								(void)record_hit(&tagged, tok, (size_t)tlen, 0);
							}
							(void)record_hit(&untagged, tok, (size_t)tlen, 0);
						}
					}
					space_del_range(sp, start_a, end_a);
				}
			}
		} else if (op == 0u && ((w & 0x3Fu) == 0x21u || (w & 0x3Fu) == 0x25u) && rt == 0u) {
			uint32_t rd = (w >> 11) & 0x1Fu;
			if (rd != 0) {
				regs[rd] = regs[rs];
			}
		} else {
			if (op == 0x08u || op == 0x0Au || op == 0x0Bu || op == 0x0Cu || op == 0x0Eu || op == 0x20u ||
				op == 0x21u || op == 0x23u || op == 0x24u || op == 0x25u) {
				regs[rt].kind = 0;
			} else if (op == 0u && w != 0u) {
				uint32_t rd = (w >> 11) & 0x1Fu;
				if (rd != 0) {
					regs[rd].kind = 0;
				}
			}
		}
	}

	free(spaces);

	bool ok = false;
	if (tagged.count > 0) {
		*found_any = true;
		ok = choose_unique_best(&tagged, out, out_size);
	} else if (untagged.count > 0) {
		*found_any = true;
		ok = choose_unique_best(&untagged, out, out_size);
	}
	if (tagged.items) {
		wipe_mem(tagged.items, tagged.cap * sizeof(*tagged.items));
		free(tagged.items);
	}
	if (untagged.items) {
		wipe_mem(untagged.items, untagged.cap * sizeof(*untagged.items));
		free(untagged.items);
	}
	return ok;
}

bool stockkeys_extract_from_elf(const uint8_t *buf, size_t len, stockkeys_pairs_t *out) {
	if (!out) {
		return false;
	}
	memset(out, 0, sizeof(*out));
	if (!buf || len < MIN_PRINTABLE_RUN || len > UINT32_MAX) {
		return false;
	}

	// Pass 1: collect printable ASCII runs and anchor positions.
	size_t spans_cap = 1024;
	str_span_t *spans = malloc(spans_cap * sizeof(*spans));
	uint32_t *q_anchors = NULL;
	size_t q_anc_cnt = 0, q_anc_cap = 0, q_strong_cnt = 0;
	uint32_t *t_anchors = NULL;
	size_t t_anc_cnt = 0, t_anc_cap = 0, t_strong_cnt = 0;
	if (!spans) {
		return false;
	}

	size_t spans_cnt = 0;
	size_t start = SIZE_MAX;
	for (size_t i = 0; i <= len; i++) {
		bool pr = (i < len) && (buf[i] >= 0x20u && buf[i] <= 0x7Eu);
		if (pr) {
			if (start == SIZE_MAX) {
				start = i;
			}
		} else if (start != SIZE_MAX) {
			size_t slen = i - start;
			if (slen >= MIN_PRINTABLE_RUN) {
				if (spans_cnt == spans_cap) {
					size_t ncap = spans_cap * 2u;
					str_span_t *ns = realloc(spans, ncap * sizeof(*ns));
					if (!ns) {
						free(spans);
						free(q_anchors);
						free(t_anchors);
						return false;
					}
					spans = ns;
					spans_cap = ncap;
				}
				spans[spans_cnt].off = (uint32_t)start;
				spans[spans_cnt].len = (uint32_t)slen;
				spans_cnt++;

				const uint8_t *s = buf + start;
				if (is_qobuz_anchor(s, slen)) {
					if (q_anc_cnt == q_anc_cap) {
						size_t ncap = q_anc_cap ? q_anc_cap * 2u : 32u;
						uint32_t *na = realloc(q_anchors, ncap * sizeof(*na));
						if (!na) {
							free(spans);
							free(q_anchors);
							free(t_anchors);
							return false;
						}
						q_anchors = na;
						q_anc_cap = ncap;
					}
					q_anchors[q_anc_cnt++] = (uint32_t)start;
					if (is_qobuz_strong_anchor(s, slen)) {
						q_strong_cnt++;
					}
				}
				if (is_tidal_anchor(s, slen)) {
					if (t_anc_cnt == t_anc_cap) {
						size_t ncap = t_anc_cap ? t_anc_cap * 2u : 32u;
						uint32_t *na = realloc(t_anchors, ncap * sizeof(*na));
						if (!na) {
							free(spans);
							free(q_anchors);
							free(t_anchors);
							return false;
						}
						t_anchors = na;
						t_anc_cap = ncap;
					}
					t_anchors[t_anc_cnt++] = (uint32_t)start;
					if (is_tidal_strong_anchor(s, slen)) {
						t_strong_cnt++;
					}
				}
			}
			start = SIZE_MAX;
		}
	}

	// Qobuz extraction
	if (q_anc_cnt > 0 && q_strong_cnt > 0) {
		hit_list_t id_hits = {0};
		hit_list_t sec_hits = {0};
		for (size_t i = 0; i < spans_cnt; i++) {
			const uint8_t *s = buf + spans[i].off;
			size_t slen = spans[i].len;
			if (is_qobuz_app_id(s, slen)) {
				(void)record_hit(&id_hits, s, slen, nearest_anchor(spans[i].off, q_anchors, q_anc_cnt));
			}
			int sk = qobuz_secret_kind(s, slen);
			if (sk > 0) {
				uint32_t score = nearest_anchor(spans[i].off, q_anchors, q_anc_cnt);
				if (sk == 2 && score < UINT32_MAX) {
					score += 1u;
				}
				(void)record_hit(&sec_hits, s, slen, score);
			}
		}
		if (choose_unique_best(&id_hits, out->qobuz_app_id, sizeof(out->qobuz_app_id)) &&
			choose_unique_best(&sec_hits, out->qobuz_app_secret, sizeof(out->qobuz_app_secret)) &&
			strcmp(out->qobuz_app_id, out->qobuz_app_secret) != 0) {
			out->has_qobuz = true;
		} else {
			wipe_mem(out->qobuz_app_id, sizeof(out->qobuz_app_id));
			wipe_mem(out->qobuz_app_secret, sizeof(out->qobuz_app_secret));
		}
		if (id_hits.items) {
			wipe_mem(id_hits.items, id_hits.cap * sizeof(*id_hits.items));
			free(id_hits.items);
		}
		if (sec_hits.items) {
			wipe_mem(sec_hits.items, sec_hits.cap * sizeof(*sec_hits.items));
			free(sec_hits.items);
		}
	}

	// Tidal extraction
	if (t_anc_cnt > 0 && t_strong_cnt >= 2) {
		hit_list_t tsec_hits = {0};
		for (size_t i = 0; i < spans_cnt; i++) {
			const uint8_t *s = buf + spans[i].off;
			size_t slen = spans[i].len;
			if (is_tidal_b64_secret(s, slen)) {
				uint32_t dist = nearest_anchor(spans[i].off, t_anchors, t_anc_cnt);
				if (dist <= TIDAL_MAX_SECRET_DISTANCE) {
					(void)record_hit(&tsec_hits, s, slen, dist);
				}
			}
		}
		if (choose_unique_best(&tsec_hits, out->tidal_client_secret, sizeof(out->tidal_client_secret))) {
			bool had_mips_tokens = false;
			if (scan_mips_tidal_client_id(buf, len, out->tidal_client_id, sizeof(out->tidal_client_id),
										  &had_mips_tokens)) {
				out->has_tidal = true;
			} else if (!had_mips_tokens) {
				// Fallback to 16-char .rodata client_id only when no MIPS byte-assembled tokens existed.
				hit_list_t tid_hits = {0};
				for (size_t i = 0; i < spans_cnt; i++) {
					const uint8_t *s = buf + spans[i].off;
					size_t slen = spans[i].len;
					if (is_tidal_rodata_id(s, slen)) {
						uint32_t dist = nearest_anchor(spans[i].off, t_anchors, t_anc_cnt);
						if (dist <= TIDAL_MAX_SECRET_DISTANCE) {
							(void)record_hit(&tid_hits, s, slen, dist);
						}
					}
				}
				if (choose_unique_best(&tid_hits, out->tidal_client_id, sizeof(out->tidal_client_id))) {
					out->has_tidal = true;
				}
				if (tid_hits.items) {
					wipe_mem(tid_hits.items, tid_hits.cap * sizeof(*tid_hits.items));
					free(tid_hits.items);
				}
			}
			if (!out->has_tidal) {
				wipe_mem(out->tidal_client_id, sizeof(out->tidal_client_id));
				wipe_mem(out->tidal_client_secret, sizeof(out->tidal_client_secret));
			}
		}
		if (tsec_hits.items) {
			wipe_mem(tsec_hits.items, tsec_hits.cap * sizeof(*tsec_hits.items));
			free(tsec_hits.items);
		}
	}

	free(spans);
	free(q_anchors);
	free(t_anchors);
	return out->has_qobuz || out->has_tidal;
}

// ===========================================================================
// 3. Minimal SquashFS 4.0 reader (LZO1X & zlib/gzip)
// ===========================================================================

typedef bool (*vread_fn)(void *ctx, uint64_t off, void *dst, size_t len);

typedef struct {
	vread_fn read_at;
	void *ctx;
	uint32_t block_size;
	uint16_t compression;
	uint64_t root_inode;
	uint64_t inode_tbl;
	uint64_t dir_tbl;
	uint64_t frag_tbl;
	// Single-block metadata cache (8 KiB) avoids re-decompressing while walking a directory.
	uint64_t cached_meta_pos;
	uint64_t cached_meta_next;
	size_t cached_meta_len;
	uint8_t cached_meta[8192];
} sqfs_reader_t;

static bool sqfs_decompress(uint16_t comp, const uint8_t *src, size_t src_len, uint8_t *dst, size_t dst_cap,
							size_t *out_len) {
	if (comp == 3) { // LZO
		return stockkeys_lzo1x_decompress(src, src_len, dst, dst_cap, out_len);
	}
	if (comp == 1) { // gzip / zlib
		size_t n = tinfl_decompress_mem_to_mem(
			dst, dst_cap, src, src_len, TINFL_FLAG_PARSE_ZLIB_HEADER | TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF);
		if (n == TINFL_DECOMPRESS_MEM_TO_MEM_FAILED) {
			return false;
		}
		if (out_len) {
			*out_len = n;
		}
		return true;
	}
	return false;
}

static bool sqfs_read_meta(sqfs_reader_t *r, uint64_t base, uint64_t block_off, uint32_t inner_off, uint8_t *out,
						   size_t want) {
	uint64_t cur = base + block_off;
	uint32_t off = inner_off;
	size_t got = 0;
	uint8_t cbuf[8192];

	while (got < want) {
		if (r->cached_meta_pos != cur) {
			uint8_t hdr_b[2];
			if (!r->read_at(r->ctx, cur, hdr_b, 2)) {
				return false;
			}
			uint16_t hdr = rd16le(hdr_b);
			size_t clen = hdr & 0x7FFFu;
			if (clen == 0 || clen > sizeof(cbuf)) {
				return false;
			}
			if (!r->read_at(r->ctx, cur + 2, cbuf, clen)) {
				return false;
			}
			size_t ulen = 0;
			if (hdr & 0x8000u) {
				memcpy(r->cached_meta, cbuf, clen);
				ulen = clen;
			} else if (!sqfs_decompress(r->compression, cbuf, clen, r->cached_meta, sizeof(r->cached_meta), &ulen)) {
				return false;
			}
			r->cached_meta_pos = cur;
			r->cached_meta_len = ulen;
			r->cached_meta_next = cur + 2u + clen;
		}
		if (off >= r->cached_meta_len) {
			off -= (uint32_t)r->cached_meta_len;
			cur = r->cached_meta_next;
			continue;
		}
		size_t avail = r->cached_meta_len - off;
		size_t take = (want - got) < avail ? (want - got) : avail;
		memcpy(out + got, r->cached_meta + off, take);
		got += take;
		off = 0;
		cur = r->cached_meta_next;
	}
	return true;
}

static bool sqfs_lookup_dir(sqfs_reader_t *r, uint64_t dir_ref, const char *name, uint64_t *child_ref) {
	uint64_t boff = (dir_ref >> 16) & 0xFFFFFFFFULL;
	uint32_t ioff = (uint32_t)(dir_ref & 0xFFFFu);
	uint8_t ihdr[40];
	if (!sqfs_read_meta(r, r->inode_tbl, boff, ioff, ihdr, 16)) {
		return false;
	}
	uint16_t itype = rd16le(ihdr);
	uint32_t dblk = 0, fsize = 0;
	uint16_t doff = 0;
	if (itype == 1) {
		if (!sqfs_read_meta(r, r->inode_tbl, boff, ioff, ihdr, 32)) {
			return false;
		}
		dblk = rd32le(ihdr + 16);
		fsize = rd16le(ihdr + 24);
		doff = rd16le(ihdr + 26);
	} else if (itype == 8) {
		if (!sqfs_read_meta(r, r->inode_tbl, boff, ioff, ihdr, 40)) {
			return false;
		}
		fsize = rd32le(ihdr + 20);
		dblk = rd32le(ihdr + 24);
		doff = rd16le(ihdr + 34);
	} else {
		return false;
	}

	if (fsize <= 3 || fsize > 1024u * 1024u) {
		return false;
	}
	size_t total = (size_t)(fsize - 3u);
	uint8_t *dbuf = malloc(total);
	if (!dbuf) {
		return false;
	}
	if (!sqfs_read_meta(r, r->dir_tbl, dblk, doff, dbuf, total)) {
		free(dbuf);
		return false;
	}

	size_t nlen = strlen(name);
	size_t pos = 0;
	bool found = false;
	while (pos + 12 <= total) {
		uint32_t count = rd32le(dbuf + pos) + 1u;
		uint32_t start_block = rd32le(dbuf + pos + 4);
		pos += 12;
		for (uint32_t i = 0; i < count; i++) {
			if (pos + 8 > total) {
				free(dbuf);
				return false;
			}
			uint16_t eoff = rd16le(dbuf + pos);
			uint16_t esize = rd16le(dbuf + pos + 6);
			pos += 8;
			size_t elen = (size_t)esize + 1u;
			if (pos + elen > total) {
				free(dbuf);
				return false;
			}
			if (elen == nlen && memcmp(dbuf + pos, name, nlen) == 0) {
				*child_ref = ((uint64_t)start_block << 16) | eoff;
				found = true;
				break;
			}
			pos += elen;
		}
		if (found) {
			break;
		}
	}
	free(dbuf);
	return found;
}

static uint8_t *sqfs_extract_hiby_player(vread_fn read_at, void *ctx, size_t *size_out) {
	*size_out = 0;
	uint8_t sb[96];
	if (!read_at(ctx, 0, sb, sizeof(sb)) || memcmp(sb, "hsqs", 4) != 0) {
		return NULL;
	}

	sqfs_reader_t r;
	memset(&r, 0, sizeof(r));
	r.read_at = read_at;
	r.ctx = ctx;
	r.cached_meta_pos = UINT64_MAX;
	r.block_size = rd32le(sb + 12);
	r.compression = rd16le(sb + 20);
	uint16_t s_major = rd16le(sb + 28);
	uint16_t s_minor = rd16le(sb + 30);
	r.root_inode = rd64le(sb + 32);
	r.inode_tbl = rd64le(sb + 64);
	r.dir_tbl = rd64le(sb + 72);
	r.frag_tbl = rd64le(sb + 80);

	if (s_major != 4 || s_minor != 0 || r.block_size < 4096 || r.block_size > (1u << 20)) {
		return NULL;
	}
	if (r.compression != 1 && r.compression != 3) {
		return NULL;
	}

	static const char *const PATH_PARTS[] = {"usr", "bin", "hiby_player"};
	uint64_t ref = r.root_inode;
	for (size_t i = 0; i < sizeof(PATH_PARTS) / sizeof(PATH_PARTS[0]); i++) {
		if (!sqfs_lookup_dir(&r, ref, PATH_PARTS[i], &ref)) {
			return NULL;
		}
	}

	uint64_t boff = (ref >> 16) & 0xFFFFFFFFULL;
	uint32_t ioff = (uint32_t)(ref & 0xFFFFu);
	uint8_t ihdr[56];
	if (!sqfs_read_meta(&r, r.inode_tbl, boff, ioff, ihdr, 16)) {
		return NULL;
	}
	uint16_t itype = rd16le(ihdr);
	uint64_t blocks_start = 0, file_size = 0;
	uint32_t frag_idx = 0xFFFFFFFFu, block_off = 0, hdr_sz = 0;
	if (itype == 2) {
		if (!sqfs_read_meta(&r, r.inode_tbl, boff, ioff, ihdr, 32)) {
			return NULL;
		}
		blocks_start = rd32le(ihdr + 16);
		frag_idx = rd32le(ihdr + 20);
		block_off = rd32le(ihdr + 24);
		file_size = rd32le(ihdr + 28);
		hdr_sz = 32;
	} else if (itype == 9) {
		if (!sqfs_read_meta(&r, r.inode_tbl, boff, ioff, ihdr, 56)) {
			return NULL;
		}
		blocks_start = rd64le(ihdr + 16);
		file_size = rd64le(ihdr + 24);
		frag_idx = rd32le(ihdr + 40);
		block_off = rd32le(ihdr + 44);
		hdr_sz = 56;
	} else {
		return NULL;
	}

	if (file_size == 0 || file_size > STOCKKEYS_MAX_ELF) {
		return NULL;
	}

	size_t nfull = (frag_idx != 0xFFFFFFFFu)
					   ? (size_t)(file_size / r.block_size)
					   : (size_t)((file_size + r.block_size - 1u) / r.block_size);

	uint8_t *bsizes_raw = NULL;
	if (nfull > 0) {
		bsizes_raw = malloc(nfull * 4u);
		if (!bsizes_raw || !sqfs_read_meta(&r, r.inode_tbl, boff, ioff + hdr_sz, bsizes_raw, nfull * 4u)) {
			free(bsizes_raw);
			return NULL;
		}
	}

	uint8_t *out = malloc((size_t)file_size);
	uint8_t *cbuf = malloc(r.block_size);
	uint8_t *ubuf = malloc(r.block_size);
	if (!out || !cbuf || !ubuf) {
		free(bsizes_raw);
		free(out);
		free(cbuf);
		free(ubuf);
		return NULL;
	}

	size_t written = 0;
	uint64_t cur_d = blocks_start;
	bool ok = true;

	for (size_t i = 0; i < nfull; i++) {
		uint32_t bsz = rd32le(bsizes_raw + i * 4u);
		size_t remain = (size_t)file_size - written;
		size_t want = remain < r.block_size ? remain : r.block_size;
		if (bsz == 0) {
			memset(out + written, 0, want);
			written += want;
		} else {
			bool uncomp = (bsz & (1u << 24)) != 0;
			uint32_t clen = bsz & 0xFFFFFFu;
			if (clen == 0 || clen > r.block_size || !read_at(ctx, cur_d, cbuf, clen)) {
				ok = false;
				break;
			}
			cur_d += clen;
			if (uncomp) {
				if (clen < want) {
					ok = false;
					break;
				}
				memcpy(out + written, cbuf, want);
			} else {
				size_t ulen = 0;
				if (!sqfs_decompress(r.compression, cbuf, clen, ubuf, r.block_size, &ulen) || ulen < want) {
					ok = false;
					break;
				}
				memcpy(out + written, ubuf, want);
			}
			written += want;
		}
	}

	if (ok && frag_idx != 0xFFFFFFFFu && written < (size_t)file_size) {
		size_t rem = (size_t)file_size - written;
		uint64_t frag_ptr_off = r.frag_tbl + (uint64_t)(frag_idx / 512u) * 8u;
		uint8_t fptr_b[8];
		uint8_t fentry[16];
		if (!read_at(ctx, frag_ptr_off, fptr_b, 8) ||
			!sqfs_read_meta(&r, 0, rd64le(fptr_b), (frag_idx % 512u) * 16u, fentry, 16)) {
			ok = false;
		} else {
			uint64_t fstart = rd64le(fentry);
			uint32_t fsize = rd32le(fentry + 8);
			bool uncomp = (fsize & (1u << 24)) != 0;
			uint32_t clen = fsize & 0xFFFFFFu;
			if (clen == 0 || clen > r.block_size || !read_at(ctx, fstart, cbuf, clen)) {
				ok = false;
			} else if (uncomp) {
				if ((uint64_t)block_off + rem > clen) {
					ok = false;
				} else {
					memcpy(out + written, cbuf + block_off, rem);
					written += rem;
				}
			} else {
				size_t ulen = 0;
				if (!sqfs_decompress(r.compression, cbuf, clen, ubuf, r.block_size, &ulen) ||
					(uint64_t)block_off + rem > ulen) {
					ok = false;
				} else {
					memcpy(out + written, ubuf + block_off, rem);
					written += rem;
				}
			}
		}
	}

	free(bsizes_raw);
	wipe_mem(cbuf, r.block_size);
	wipe_mem(ubuf, r.block_size);
	free(cbuf);
	free(ubuf);

	if (!ok || written != (size_t)file_size) {
		wipe_mem(out, (size_t)file_size);
		free(out);
		return NULL;
	}
	*size_out = written;
	return out;
}

// ===========================================================================
// 4. ISO 9660 .upt chunked SquashFS reader
// ===========================================================================

static bool file_vread(void *ctx, uint64_t off, void *dst, size_t len) {
	FILE *f = ctx;
	return fseeko(f, (off_t)off, SEEK_SET) == 0 && fread(dst, 1, len, f) == len;
}

typedef struct {
	int index;
	uint64_t iso_off;
	uint32_t len;
} upt_chunk_t;

typedef struct {
	FILE *f;
	upt_chunk_t *chunks;
	size_t count;
	uint64_t *prefix; // prefix[i] = virtual offset where chunks[i] begins
	uint64_t total_size;
} upt_sqfs_view_t;

static int cmp_upt_chunk(const void *a, const void *b) {
	const upt_chunk_t *ca = a, *cb = b;
	return (ca->index > cb->index) - (ca->index < cb->index);
}

static bool upt_vread(void *ctx, uint64_t off, void *dst, size_t len) {
	upt_sqfs_view_t *v = ctx;
	if (off + len > v->total_size) {
		return false;
	}
	uint8_t *out = dst;
	size_t rem = len;
	uint64_t cur = off;

	// Binary search the starting chunk.
	size_t lo = 0, hi = v->count;
	while (lo + 1 < hi) {
		size_t mid = lo + (hi - lo) / 2u;
		if (v->prefix[mid] <= cur) {
			lo = mid;
		} else {
			hi = mid;
		}
	}

	for (size_t idx = lo; rem > 0 && idx < v->count; idx++) {
		uint64_t cstart = v->prefix[idx];
		uint32_t clen = v->chunks[idx].len;
		if (cur >= cstart + clen) {
			continue;
		}
		uint64_t inner = cur - cstart;
		size_t avail = (size_t)(clen - inner);
		size_t take = rem < avail ? rem : avail;
		if (!file_vread(v->f, v->chunks[idx].iso_off + inner, out, take)) {
			return false;
		}
		out += take;
		cur += take;
		rem -= take;
	}
	return rem == 0;
}

static void decode_iso_name(const uint8_t *raw, size_t nlen, bool joliet, char *out, size_t out_size) {
	size_t o = 0;
	if (joliet) {
		for (size_t i = 0; i + 1 < nlen && o + 1 < out_size; i += 2) {
			uint16_t ch = (uint16_t)(((uint16_t)raw[i] << 8) | raw[i + 1]);
			if (ch == 0 || ch == ';') {
				break;
			}
			out[o++] = (ch < 0x80u) ? (char)ch : '?';
		}
	} else {
		for (size_t i = 0; i < nlen && o + 1 < out_size; i++) {
			if (raw[i] == 0 || raw[i] == ';') {
				break;
			}
			out[o++] = (char)raw[i];
		}
	}
	out[o] = '\0';
}

static uint8_t *upt_extract_hiby_player(FILE *f, size_t *size_out) {
	*size_out = 0;
	uint8_t vd[2048];
	uint8_t chosen_vd[2048];
	bool have_vd = false;
	bool joliet = false;

	for (int sec = 16; sec < 32; sec++) {
		if (!file_vread(f, (uint64_t)sec * 2048u, vd, sizeof(vd)) || memcmp(vd + 1, "CD001", 5) != 0) {
			break;
		}
		if (vd[0] == 255) {
			break;
		}
		if (vd[0] == 2) {
			memcpy(chosen_vd, vd, sizeof(vd));
			have_vd = true;
			joliet = true;
			break;
		}
		if (vd[0] == 1 && !have_vd) {
			memcpy(chosen_vd, vd, sizeof(vd));
			have_vd = true;
		}
	}
	if (!have_vd) {
		return NULL;
	}

	uint32_t root_lba = rd32le(chosen_vd + 158);
	uint32_t root_len = rd32le(chosen_vd + 166);
	if (root_len == 0 || root_len > 256u * 1024u) {
		return NULL;
	}

	uint8_t *rbuf = malloc(root_len);
	if (!rbuf || !file_vread(f, (uint64_t)root_lba * 2048u, rbuf, root_len)) {
		free(rbuf);
		return NULL;
	}

	uint32_t ota_lba = 0, ota_len = 0;
	for (size_t pos = 0; pos < root_len;) {
		uint8_t rlen = rbuf[pos];
		if (rlen == 0) {
			pos = ((pos / 2048u) + 1u) * 2048u;
			continue;
		}
		if (rlen < 34 || pos + rlen > root_len) {
			break;
		}
		uint8_t flags = rbuf[pos + 25];
		uint8_t nlen = rbuf[pos + 32];
		if (pos + 33u + nlen <= pos + rlen && (flags & 2u) && !(nlen == 1 && rbuf[pos + 33] <= 1)) {
			char name[128];
			decode_iso_name(rbuf + pos + 33, nlen, joliet, name, sizeof(name));
			if (strcasecmp(name, "ota_v0") == 0) {
				ota_lba = rd32le(rbuf + pos + 2);
				ota_len = rd32le(rbuf + pos + 10);
				break;
			}
		}
		pos += rlen;
	}
	free(rbuf);

	if (ota_len == 0 || ota_len > 1024u * 1024u) {
		return NULL;
	}
	uint8_t *obuf = malloc(ota_len);
	if (!obuf || !file_vread(f, (uint64_t)ota_lba * 2048u, obuf, ota_len)) {
		free(obuf);
		return NULL;
	}

	size_t cap = 128, count = 0;
	upt_chunk_t *chunks = malloc(cap * sizeof(*chunks));
	int64_t expected_size = -1;
	if (!chunks) {
		free(obuf);
		return NULL;
	}

	for (size_t pos = 0; pos < ota_len;) {
		uint8_t rlen = obuf[pos];
		if (rlen == 0) {
			pos = ((pos / 2048u) + 1u) * 2048u;
			continue;
		}
		if (rlen < 34 || pos + rlen > ota_len) {
			break;
		}
		uint8_t flags = obuf[pos + 25];
		uint8_t nlen = obuf[pos + 32];
		if (pos + 33u + nlen <= pos + rlen && !(flags & 2u) && !(nlen == 1 && obuf[pos + 33] <= 1)) {
			char name[160];
			decode_iso_name(obuf + pos + 33, nlen, joliet, name, sizeof(name));
			uint32_t elba = rd32le(obuf + pos + 2);
			uint32_t elen = rd32le(obuf + pos + 10);

			if (strcasecmp(name, "ota_update.in") == 0 && elen > 0 && elen < 16384u) {
				char *txt = malloc((size_t)elen + 1u);
				if (txt && file_vread(f, (uint64_t)elba * 2048u, txt, elen)) {
					txt[elen] = '\0';
					bool in_rootfs = false;
					char *save = NULL;
					for (char *ln = strtok_r(txt, "\r\n", &save); ln; ln = strtok_r(NULL, "\r\n", &save)) {
						if (strncmp(ln, "img_name=", 9) == 0) {
							in_rootfs = strcmp(ln + 9, "rootfs.squashfs") == 0;
						} else if (in_rootfs && strncmp(ln, "img_size=", 9) == 0) {
							expected_size = strtoll(ln + 9, NULL, 10);
						}
					}
				}
				free(txt);
			} else if (strncasecmp(name, "rootfs.squashfs.", 16) == 0) {
				const char *nump = name + 16;
				if (isdigit((unsigned char)nump[0])) {
					char *endp = NULL;
					long idx = strtol(nump, &endp, 10);
					if (idx >= 0 && idx < 4096 && (*endp == '\0' || *endp == '.')) {
						if (count == cap) {
							size_t ncap = cap * 2u;
							upt_chunk_t *nc = realloc(chunks, ncap * sizeof(*nc));
							if (!nc) {
								free(chunks);
								free(obuf);
								return NULL;
							}
							chunks = nc;
							cap = ncap;
						}
						chunks[count].index = (int)idx;
						chunks[count].iso_off = (uint64_t)elba * 2048u;
						chunks[count].len = elen;
						count++;
					}
				}
			}
		}
		pos += rlen;
	}
	free(obuf);

	if (count == 0) {
		free(chunks);
		return NULL;
	}
	qsort(chunks, count, sizeof(*chunks), cmp_upt_chunk);

	uint64_t *prefix = malloc(count * sizeof(*prefix));
	if (!prefix) {
		free(chunks);
		return NULL;
	}
	uint64_t total = 0;
	for (size_t i = 0; i < count; i++) {
		if (chunks[i].index != (int)i) {
			free(prefix);
			free(chunks);
			return NULL;
		}
		prefix[i] = total;
		total += chunks[i].len;
	}
	if (expected_size > 0 && total != (uint64_t)expected_size) {
		free(prefix);
		free(chunks);
		return NULL;
	}

	upt_sqfs_view_t view = {
		.f = f,
		.chunks = chunks,
		.count = count,
		.prefix = prefix,
		.total_size = total,
	};
	uint8_t *elf = sqfs_extract_hiby_player(upt_vread, &view, size_out);
	free(prefix);
	free(chunks);
	return elf;
}

// ===========================================================================
// 5. Streaming ZIP / split-ZIP / HTTP reader -> temporary .upt -> extract
// ===========================================================================

typedef int (*byte_feed_fn)(void *ctx, uint8_t *buf, size_t max_len);

static bool stream_unpack_to_file(byte_feed_fn feed, void *feed_ctx, const char *dest_path) {
	uint8_t *in_buf = malloc(64u * 1024u);
	uint8_t *dict = malloc(TINFL_LZ_DICT_SIZE);
	tinfl_decompressor *decomp = malloc(sizeof(*decomp));
	if (!in_buf || !dict || !decomp) {
		free(in_buf);
		free(dict);
		free(decomp);
		return false;
	}

	FILE *out = fopen(dest_path, "wb");
	if (!out) {
		free(in_buf);
		free(dict);
		free(decomp);
		return false;
	}

	size_t in_pos = 0, in_avail = 0;
	bool eof = false;

	// Fill at least `want` bytes into in_buf[in_pos .. in_pos + in_avail).
	while (in_avail < 30 && !eof) {
		int n = feed(feed_ctx, in_buf + in_avail, 64u * 1024u - in_avail);
		if (n < 0) {
			fclose(out);
			unlink(dest_path);
			free(in_buf);
			free(dict);
			free(decomp);
			return false;
		}
		if (n == 0) {
			eof = true;
		} else {
			in_avail += (size_t)n;
		}
	}

	bool ok = true;
	uint64_t total_written = 0;

	if (in_avail >= 4 && memcmp(in_buf, "PK\x03\x04", 4) == 0) {
		if (in_avail < 30) {
			ok = false;
		} else {
			uint16_t flags = rd16le(in_buf + 6);
			uint16_t method = rd16le(in_buf + 8);
			uint32_t comp_size = rd32le(in_buf + 18);
			uint32_t uncomp_size = rd32le(in_buf + 22);
			uint16_t nlen = rd16le(in_buf + 26);
			uint16_t elen = rd16le(in_buf + 28);
			size_t skip = 30u + (size_t)nlen + (size_t)elen;

			while (skip > 0 && ok) {
				if (in_avail == 0) {
					int n = feed(feed_ctx, in_buf, 64u * 1024u);
					if (n <= 0) {
						ok = false;
						break;
					}
					in_pos = 0;
					in_avail = (size_t)n;
				}
				size_t take = skip < in_avail ? skip : in_avail;
				in_pos += take;
				in_avail -= take;
				skip -= take;
			}

			if (ok && method == 0) {
				// Stored ZIP entry.
				bool bounded = (flags & 0x08u) == 0 && uncomp_size > 0;
				uint64_t remain = bounded ? uncomp_size : comp_size;
				if (!bounded && remain == 0) {
					ok = false;
				}
				while (ok && remain > 0) {
					if (in_avail == 0) {
						int n = feed(feed_ctx, in_buf, 64u * 1024u);
						if (n <= 0) {
							ok = false;
							break;
						}
						in_pos = 0;
						in_avail = (size_t)n;
					}
					size_t take = remain < in_avail ? (size_t)remain : in_avail;
					if (total_written + take > MAX_UPT_UNPACK_BYTES || fwrite(in_buf + in_pos, 1, take, out) != take) {
						ok = false;
						break;
					}
					in_pos += take;
					in_avail -= take;
					remain -= take;
					total_written += take;
				}
			} else if (ok && method == 8) {
				// Raw deflate stream (both standard and streaming data-descriptor ZIPs).
				tinfl_init(decomp);
				size_t dict_ofs = 0;
				for (;;) {
					if (in_avail == 0 && !eof) {
						in_pos = 0;
						int n = feed(feed_ctx, in_buf, 64u * 1024u);
						if (n < 0) {
							ok = false;
							break;
						}
						if (n == 0) {
							eof = true;
						} else {
							in_avail = (size_t)n;
						}
					}

					size_t in_bytes = in_avail;
					size_t out_bytes = TINFL_LZ_DICT_SIZE - dict_ofs;
					uint32_t tflags = (!eof || in_avail > 0) ? TINFL_FLAG_HAS_MORE_INPUT : 0;
					if (eof && in_avail == 0) {
						tflags = 0;
					}
					tinfl_status st = tinfl_decompress(decomp, in_buf + in_pos, &in_bytes, dict, dict + dict_ofs,
													   &out_bytes, tflags);
					in_pos += in_bytes;
					in_avail -= in_bytes;

					if (out_bytes > 0) {
						if (total_written + out_bytes > MAX_UPT_UNPACK_BYTES ||
							fwrite(dict + dict_ofs, 1, out_bytes, out) != out_bytes) {
							ok = false;
							break;
						}
						total_written += out_bytes;
						dict_ofs = (dict_ofs + out_bytes) & (TINFL_LZ_DICT_SIZE - 1u);
					}

					if (st == TINFL_STATUS_DONE) {
						break;
					}
					if (st < 0) {
						if (st == TINFL_STATUS_FAILED_CANNOT_MAKE_PROGRESS && eof && in_avail == 0) {
							ok = false;
							break;
						}
						// When the last chunk in memory held the final deflate bits and eof was
						// not yet observed, retry once with HAS_MORE_INPUT cleared after EOF.
						ok = false;
						break;
					}
					if (st == TINFL_STATUS_NEEDS_MORE_INPUT && eof && in_avail == 0) {
						// One final call with HAS_MORE_INPUT cleared lets tinfl flush end-of-stream.
						in_bytes = 0;
						out_bytes = TINFL_LZ_DICT_SIZE - dict_ofs;
						st = tinfl_decompress(decomp, in_buf, &in_bytes, dict, dict + dict_ofs, &out_bytes, 0);
						if (out_bytes > 0) {
							if (total_written + out_bytes > MAX_UPT_UNPACK_BYTES ||
								fwrite(dict + dict_ofs, 1, out_bytes, out) != out_bytes) {
								ok = false;
								break;
							}
							total_written += out_bytes;
						}
						ok = (st == TINFL_STATUS_DONE);
						break;
					}
				}
			} else {
				ok = false;
			}
		}
	} else {
		// Raw .upt / SquashFS / ELF stream.
		while (ok) {
			if (in_avail > 0) {
				if (total_written + in_avail > MAX_UPT_UNPACK_BYTES ||
					fwrite(in_buf + in_pos, 1, in_avail, out) != in_avail) {
					ok = false;
					break;
				}
				total_written += in_avail;
				in_pos = 0;
				in_avail = 0;
			}
			if (eof) {
				break;
			}
			int n = feed(feed_ctx, in_buf, 64u * 1024u);
			if (n < 0) {
				ok = false;
				break;
			}
			if (n == 0) {
				eof = true;
			} else {
				in_avail = (size_t)n;
			}
		}
	}

	if (fflush(out) != 0) {
		ok = false;
	}
	if (fclose(out) != 0) {
		ok = false;
	}
	if (!ok || total_written == 0) {
		unlink(dest_path);
		ok = false;
	}

	wipe_mem(in_buf, 64u * 1024u);
	wipe_mem(dict, TINFL_LZ_DICT_SIZE);
	free(in_buf);
	free(dict);
	free(decomp);
	return ok;
}

typedef struct {
	char **paths;
	size_t count;
	size_t idx;
	FILE *cur;
} local_files_feed_t;

static int local_files_feed(void *ctx, uint8_t *buf, size_t max_len) {
	local_files_feed_t *lf = ctx;
	while (lf->cur || lf->idx < lf->count) {
		if (!lf->cur) {
			lf->cur = fopen(lf->paths[lf->idx++], "rb");
			if (!lf->cur) {
				return -1;
			}
		}
		size_t n = fread(buf, 1, max_len, lf->cur);
		if (n > 0) {
			return (int)n;
		}
		bool err = ferror(lf->cur) != 0;
		fclose(lf->cur);
		lf->cur = NULL;
		if (err) {
			return -1;
		}
	}
	return 0;
}

static bool extract_from_open_nonzip(FILE *f, stockkeys_pairs_t *out) {
	uint8_t head[16];
	if (!file_vread(f, 0, head, sizeof(head))) {
		return false;
	}

	uint8_t *elf = NULL;
	size_t elf_size = 0;

	if (memcmp(head, "hsqs", 4) == 0) {
		elf = sqfs_extract_hiby_player(file_vread, f, &elf_size);
	} else {
		uint8_t iso_magic[6];
		if (file_vread(f, 16u * 2048u, iso_magic, sizeof(iso_magic)) && memcmp(iso_magic + 1, "CD001", 5) == 0) {
			elf = upt_extract_hiby_player(f, &elf_size);
		} else {
			if (fseeko(f, 0, SEEK_END) == 0) {
				off_t sz = ftello(f);
				if (sz > 0 && (uint64_t)sz <= STOCKKEYS_MAX_ELF) {
					elf = malloc((size_t)sz);
					if (elf && file_vread(f, 0, elf, (size_t)sz)) {
						elf_size = (size_t)sz;
					} else {
						free(elf);
						elf = NULL;
					}
				}
			}
		}
	}

	if (!elf) {
		return false;
	}
	bool ok = stockkeys_extract_from_elf(elf, elf_size, out);
	wipe_mem(elf, elf_size);
	free(elf);
	return ok;
}

// Collects sibling split pieces (NAME.part1 + NAME.part2 ...) when `path` ends
// in .part1 or .001. Returns 0 when `path` is not a split piece, < 0 on a gap.
static int collect_split_files(const char *path, char ***out_paths, size_t *out_count) {
	*out_paths = NULL;
	*out_count = 0;
	const char *dot = strrchr(path, '.');
	if (!dot) {
		return 0;
	}
	bool part_style = strcasecmp(dot, ".part1") == 0;
	bool num_style = strcmp(dot, ".001") == 0;
	if (!part_style && !num_style) {
		return 0;
	}

	size_t stem_len = (size_t)(dot - path);
	if (stem_len == 0 || stem_len >= 480) {
		return 0;
	}
	char stem[480];
	memcpy(stem, path, stem_len);
	stem[stem_len] = '\0';

	char **list = NULL;
	size_t cnt = 0, cap = 0;
	for (int idx = 1; idx <= 64; idx++) {
		char cand[520];
		if (part_style) {
			snprintf(cand, sizeof(cand), "%s.part%d", stem, idx);
		} else {
			snprintf(cand, sizeof(cand), "%s.%03d", stem, idx);
		}
		struct stat st;
		if (stat(cand, &st) != 0 || !S_ISREG(st.st_mode)) {
			// Check whether a higher-numbered piece exists (missing piece gap).
			char next_cand[520];
			if (part_style) {
				snprintf(next_cand, sizeof(next_cand), "%s.part%d", stem, idx + 1);
			} else {
				snprintf(next_cand, sizeof(next_cand), "%s.%03d", stem, idx + 1);
			}
			if (stat(next_cand, &st) == 0 && S_ISREG(st.st_mode)) {
				for (size_t i = 0; i < cnt; i++) {
					free(list[i]);
				}
				free(list);
				return -1;
			}
			break;
		}
		if (cnt == cap) {
			size_t ncap = cap ? cap * 2u : 4u;
			char **nl = realloc(list, ncap * sizeof(*nl));
			if (!nl) {
				for (size_t i = 0; i < cnt; i++) {
					free(list[i]);
				}
				free(list);
				return -1;
			}
			list = nl;
			cap = ncap;
		}
		list[cnt] = strdup(cand);
		if (!list[cnt]) {
			for (size_t i = 0; i < cnt; i++) {
				free(list[i]);
			}
			free(list);
			return -1;
		}
		cnt++;
	}

	*out_paths = list;
	*out_count = cnt;
	return cnt > 0 ? 1 : 0;
}

static void pick_temp_upt_path(const char *path, const char *temp_dir, char *out, size_t out_size) {
	if (temp_dir && temp_dir[0]) {
		snprintf(out, out_size, "%s/.sonix-stock-keys.upt.part", temp_dir);
		return;
	}
	const char *slash = strrchr(path, '/');
	if (slash && slash != path) {
		size_t dlen = (size_t)(slash - path);
		if (dlen + 32 < out_size) {
			memcpy(out, path, dlen);
			out[dlen] = '\0';
			snprintf(out + dlen, out_size - dlen, "/.sonix-stock-keys.upt.part");
			return;
		}
	}
	snprintf(out, out_size, ".sonix-stock-keys.upt.part");
}

bool stockkeys_extract_from_file(const char *path, const char *temp_dir, stockkeys_pairs_t *out) {
	if (!path || !path[0] || !out) {
		return false;
	}
	memset(out, 0, sizeof(*out));

	char **split_paths = NULL;
	size_t split_count = 0;
	int sp = collect_split_files(path, &split_paths, &split_count);
	if (sp < 0) {
		return false;
	}

	if (sp > 0) {
		char tmp_upt[540];
		pick_temp_upt_path(path, temp_dir, tmp_upt, sizeof(tmp_upt));
		local_files_feed_t lf = {.paths = split_paths, .count = split_count, .idx = 0, .cur = NULL};
		bool unpacked = stream_unpack_to_file(local_files_feed, &lf, tmp_upt);
		if (lf.cur) {
			fclose(lf.cur);
		}
		for (size_t i = 0; i < split_count; i++) {
			free(split_paths[i]);
		}
		free(split_paths);
		if (!unpacked) {
			return false;
		}
		FILE *uf = fopen(tmp_upt, "rb");
		bool ok = uf && extract_from_open_nonzip(uf, out);
		if (uf) {
			fclose(uf);
		}
		unlink(tmp_upt);
		return ok;
	}

	FILE *f = fopen(path, "rb");
	if (!f) {
		return false;
	}
	uint8_t magic[4] = {0};
	size_t nread = fread(magic, 1, 4, f);
	if (nread == 4 && memcmp(magic, "PK\x03\x04", 4) == 0) {
		fclose(f);
		char *one = (char *)path;
		char tmp_upt[540];
		pick_temp_upt_path(path, temp_dir, tmp_upt, sizeof(tmp_upt));
		local_files_feed_t lf = {.paths = &one, .count = 1, .idx = 0, .cur = NULL};
		bool unpacked = stream_unpack_to_file(local_files_feed, &lf, tmp_upt);
		if (lf.cur) {
			fclose(lf.cur);
		}
		if (!unpacked) {
			return false;
		}
		FILE *uf = fopen(tmp_upt, "rb");
		bool ok = uf && extract_from_open_nonzip(uf, out);
		if (uf) {
			fclose(uf);
		}
		unlink(tmp_upt);
		return ok;
	}

	bool ok = extract_from_open_nonzip(f, out);
	fclose(f);
	return ok;
}

// ===========================================================================
// 6. Card scanning + background Wi-Fi / offline setup worker
// ===========================================================================

static bool has_free_space(const char *dir_path, uint64_t min_bytes) {
	if (!dir_path || !dir_path[0]) {
		return false;
	}
	struct stat st;
	if (stat(dir_path, &st) != 0 || !S_ISDIR(st.st_mode)) {
		return false;
	}
	struct statvfs vfs;
	if (statvfs(dir_path, &vfs) != 0) {
		return false;
	}
	uint64_t unit = vfs.f_frsize ? (uint64_t)vfs.f_frsize : (uint64_t)vfs.f_bsize;
	return (uint64_t)vfs.f_bavail * unit >= min_bytes;
}

static bool choose_scratch_dir(const char *card_root, char *out, size_t out_size) {
	if (card_root && card_root[0] && storage_sd_writable() && has_free_space(card_root, SPACE_NEEDED_BYTES)) {
		snprintf(out, out_size, "%s", card_root);
		return true;
	}
	const char *env_tmp = getenv("SONIX_STOCKKEYS_TMP");
	if (env_tmp && env_tmp[0] && has_free_space(env_tmp, SPACE_NEEDED_BYTES)) {
		snprintf(out, out_size, "%s", env_tmp);
		return true;
	}
	if (has_free_space(INTERNAL_DATA_DIR, SPACE_NEEDED_BYTES)) {
		snprintf(out, out_size, "%s", INTERNAL_DATA_DIR);
		return true;
	}
	return false;
}

static bool find_case_file(const char *dir_path, const char *wanted, char *out, size_t out_size) {
	DIR *dir = opendir(dir_path);
	if (!dir) {
		return false;
	}
	bool found = false;
	struct dirent *de;
	while ((de = readdir(dir)) != NULL) {
		if (strcasecmp(de->d_name, wanted) != 0) {
			continue;
		}
		if (snprintf(out, out_size, "%s/%s", dir_path, de->d_name) < (int)out_size) {
			struct stat st;
			if (stat(out, &st) == 0 && S_ISREG(st.st_mode)) {
				found = true;
				break;
			}
		}
	}
	closedir(dir);
	return found;
}

static bool scan_card_for_stock(const char *card_root, bool include_generic_root_upt, const char *scratch_dir,
								stockkeys_pairs_t *out) {
	if (!card_root || !card_root[0]) {
		return false;
	}

	static const char *const EXPLICIT_ROOT_NAMES[] = {
		"hiby_player", "rootfs.squashfs", "v1_stock.upt", "v1_original.upt", "v1_stock.zip", "v1.zip.part1",
	};
	static const char *const EXTRA_ROOT_NAMES[] = {
		"v1.zip", "v1.upt",
	};
	static const char *const SUBDIRS[] = {"stock", "firmware"};
	static const char *const SUBDIR_NAMES[] = {
		"hiby_player", "rootfs.squashfs", "v1_stock.upt", "v1_original.upt", "v1.upt", "v1.zip.part1", "v1.zip",
	};

	char cand[560];
	for (size_t i = 0; i < sizeof(EXPLICIT_ROOT_NAMES) / sizeof(EXPLICIT_ROOT_NAMES[0]); i++) {
		if (find_case_file(card_root, EXPLICIT_ROOT_NAMES[i], cand, sizeof(cand))) {
			if (!out || stockkeys_extract_from_file(cand, scratch_dir, out)) {
				if (out) {
					printf("stockkeys: extracted credentials from %s\n", cand);
				}
				return true;
			}
		}
	}

	// Subfolders stock/ or firmware/ (case-insensitive directory lookup).
	for (size_t s = 0; s < sizeof(SUBDIRS) / sizeof(SUBDIRS[0]); s++) {
		DIR *root_d = opendir(card_root);
		if (!root_d) {
			break;
		}
		char sub_path[520] = "";
		struct dirent *de;
		while ((de = readdir(root_d)) != NULL) {
			if (strcasecmp(de->d_name, SUBDIRS[s]) == 0) {
				if (snprintf(sub_path, sizeof(sub_path), "%s/%s", card_root, de->d_name) < (int)sizeof(sub_path)) {
					struct stat st;
					if (stat(sub_path, &st) == 0 && S_ISDIR(st.st_mode)) {
						break;
					}
				}
				sub_path[0] = '\0';
			}
		}
		closedir(root_d);
		if (!sub_path[0]) {
			continue;
		}
		for (size_t i = 0; i < sizeof(SUBDIR_NAMES) / sizeof(SUBDIR_NAMES[0]); i++) {
			if (find_case_file(sub_path, SUBDIR_NAMES[i], cand, sizeof(cand))) {
				if (!out || stockkeys_extract_from_file(cand, scratch_dir, out)) {
					if (out) {
						printf("stockkeys: extracted credentials from %s\n", cand);
					}
					return true;
				}
			}
		}
	}

	if (include_generic_root_upt) {
		for (size_t i = 0; i < sizeof(EXTRA_ROOT_NAMES) / sizeof(EXTRA_ROOT_NAMES[0]); i++) {
			if (find_case_file(card_root, EXTRA_ROOT_NAMES[i], cand, sizeof(cand))) {
				if (!out || stockkeys_extract_from_file(cand, scratch_dir, out)) {
					if (out) {
						printf("stockkeys: extracted credentials from %s\n", cand);
					}
					return true;
				}
			}
		}
	}
	return false;
}

bool stockkeys_has_sd_candidate(const char *card_root) { return scan_card_for_stock(card_root, true, NULL, NULL); }

bool stockkeys_try_boot_card(const char *card_root) {
	if (!card_root || !card_root[0]) {
		return false;
	}
	if (streamkeys_qobuz_app_id() && streamkeys_qobuz_app_secret() && streamkeys_tidal_client_id() &&
		streamkeys_tidal_client_secret()) {
		return false;
	}

	char scratch[512] = "";
	(void)choose_scratch_dir(card_root, scratch, sizeof(scratch));

	stockkeys_pairs_t pairs;
	if (!scan_card_for_stock(card_root, false, scratch[0] ? scratch : NULL, &pairs)) {
		return false;
	}

	const char *writable_card = storage_sd_writable() ? card_root : NULL;
	bool saved = streamkeys_write_extracted(pairs.has_qobuz ? pairs.qobuz_app_id : NULL,
											pairs.has_qobuz ? pairs.qobuz_app_secret : NULL,
											pairs.has_tidal ? pairs.tidal_client_id : NULL,
											pairs.has_tidal ? pairs.tidal_client_secret : NULL, writable_card);
	wipe_mem(&pairs, sizeof(pairs));
	if (!saved) {
		return false;
	}
	return streamkeys_reload(card_root);
}

// ---------------------------------------------------------------------------
// Worker state & HTTP multi-part feeder
// ---------------------------------------------------------------------------

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static stockkeys_state_t state = STOCKKEYS_IDLE;
static stockkeys_phase_t cur_phase = STOCKKEYS_PHASE_SCANNING;
static stockkeys_result_t cur_result = STOCKKEYS_OK;
static long progress_done, progress_total;
static bool cancel_requested;
static http_stream_t *active_stream;
static int active_connect_fd = -1;
static char last_detail[160];

static void set_phase(stockkeys_phase_t p) {
	pthread_mutex_lock(&lock);
	cur_phase = p;
	pthread_mutex_unlock(&lock);
}

static bool is_cancelled(void) {
	pthread_mutex_lock(&lock);
	bool c = cancel_requested;
	pthread_mutex_unlock(&lock);
	return c;
}

static void finish_worker(stockkeys_result_t r, const char *detail) {
	pthread_mutex_lock(&lock);
	if (cancel_requested && r != STOCKKEYS_OK) {
		r = STOCKKEYS_CANCELLED;
	}
	cur_result = r;
	if (detail != last_detail) {
		snprintf(last_detail, sizeof(last_detail), "%s", detail ? detail : "");
	}
	active_stream = NULL;
	active_connect_fd = -1;
	state = STOCKKEYS_FINISHED;
	pthread_mutex_unlock(&lock);
}

static bool on_http_socket(void *user, int fd) {
	(void)user;
	pthread_mutex_lock(&lock);
	active_connect_fd = fd;
	bool ok = !cancel_requested;
	pthread_mutex_unlock(&lock);
	return ok;
}

typedef struct {
	const char *urls[2];
	size_t url_count;
	size_t idx;
	http_stream_t stream;
	bool stream_open;
	sha256_ctx part_sha;
} http_parts_feed_t;

static int http_parts_feed(void *ctx, uint8_t *buf, size_t max_len) {
	http_parts_feed_t *hf = ctx;
	while (hf->stream_open || hf->idx < hf->url_count) {
		if (is_cancelled()) {
			return -1;
		}
		if (!hf->stream_open) {
			const char *url = hf->urls[hf->idx++];
			memset(&hf->stream, 0, sizeof(hf->stream));
			hf->stream.fd = -1;
			http_stream_options_t opts = {
				.socket_cb = on_http_socket,
				.socket_user = NULL,
				.no_icy = true,
				.no_https_downgrade = strncmp(url, "https://", 8) == 0,
			};
			printf("stockkeys: downloading %s\n", url);
			if (!http_stream_open_ex(&hf->stream, url, DOWNLOAD_TIMEOUT_SECS, &opts)) {
				const char *why = http_last_error();
				pthread_mutex_lock(&lock);
				active_connect_fd = -1;
				if (why && why[0]) {
					snprintf(last_detail, sizeof(last_detail), "%s", why);
				}
				pthread_mutex_unlock(&lock);
				return -1;
			}
			pthread_mutex_lock(&lock);
			active_connect_fd = -1;
			active_stream = &hf->stream;
			if (hf->stream.content_length > 0) {
				if (hf->url_count == 1) {
					progress_total = hf->stream.content_length;
				} else if (hf->idx == 1) {
					progress_total = hf->stream.content_length * (long)hf->url_count;
				} else {
					progress_total = progress_done + hf->stream.content_length;
				}
			}
			pthread_mutex_unlock(&lock);
			sha256_init(&hf->part_sha);
			hf->stream_open = true;
		}

		int want = max_len > 65536u ? 65536 : (int)max_len;
		int n = http_stream_read(&hf->stream, buf, want);
		if (n > 0) {
			sha256_update(&hf->part_sha, buf, (size_t)n);
			pthread_mutex_lock(&lock);
			progress_done += n;
			bool stop = cancel_requested;
			pthread_mutex_unlock(&lock);
			return stop ? -1 : n;
		}

		bool err = (n < 0) || is_cancelled();
		pthread_mutex_lock(&lock);
		active_stream = NULL;
		pthread_mutex_unlock(&lock);
		http_stream_close(&hf->stream);
		hf->stream_open = false;

		char hex[SHA256_HEX_LEN];
		sha256_final_hex(&hf->part_sha, hex);
		if (!err) {
			printf("stockkeys: part %zu complete (sha256 %s)\n", hf->idx, hex);
		}
		if (err) {
			return -1;
		}
	}
	return 0;
}

static bool wifi_connected_now(void) {
#ifdef HOST_BUILD
	if (!getenv("SONIX_STOCKKEYS_STRICT_WIFI")) {
		return true;
	}
#endif
	wifi_status_t st;
	wifi_get_status(&st);
	return st.state == WIFI_STATE_CONNECTED && st.ip[0] != '\0';
}

static void *worker_main(void *unused) {
	(void)unused;
	thread_be_low_priority("stockkeys");

	set_phase(STOCKKEYS_PHASE_SCANNING);
	const char *card_root = storage_sd_root();
	char scratch[512] = "";
	bool have_scratch = choose_scratch_dir(card_root, scratch, sizeof(scratch));

	stockkeys_pairs_t pairs;
	memset(&pairs, 0, sizeof(pairs));

	bool extracted = scan_card_for_stock(card_root, true, have_scratch ? scratch : NULL, &pairs);
	if (is_cancelled()) {
		wipe_mem(&pairs, sizeof(pairs));
		finish_worker(STOCKKEYS_CANCELLED, NULL);
		return NULL;
	}

	if (!extracted) {
		if (!wifi_connected_now()) {
			finish_worker(STOCKKEYS_NO_SOURCE_OFFLINE, NULL);
			return NULL;
		}
		if (!have_scratch) {
			finish_worker(STOCKKEYS_NO_SPACE, NULL);
			return NULL;
		}

		set_phase(STOCKKEYS_PHASE_DOWNLOADING);
		char url0[384], url1[384];
		http_parts_feed_t hf;
		memset(&hf, 0, sizeof(hf));

		const char *custom_url = config_get("streaming", "stock_firmware_url", NULL);
		if (custom_url && custom_url[0]) {
			snprintf(url0, sizeof(url0), "%s", custom_url);
			hf.urls[0] = url0;
			hf.url_count = 1;
			size_t ulen = strlen(url0);
			if (ulen > 6 && strcasecmp(url0 + ulen - 6, ".part1") == 0) {
				snprintf(url1, sizeof(url1), "%.*s.part2", (int)(ulen - 6), url0);
				hf.urls[1] = url1;
				hf.url_count = 2;
			}
		} else {
			const char *repo = sysinfo_ota_repo();
			if (!repo || !repo[0]) {
				repo = DEFAULT_OTA_REPO;
			}
			snprintf(url0, sizeof(url0),
					 "https://raw.githubusercontent.com/%s/main/sonix-packer/firmware-input/v1.zip.part1", repo);
			snprintf(url1, sizeof(url1),
					 "https://raw.githubusercontent.com/%s/main/sonix-packer/firmware-input/v1.zip.part2", repo);
			hf.urls[0] = url0;
			hf.urls[1] = url1;
			hf.url_count = 2;
			pthread_mutex_lock(&lock);
			progress_total = BUNDLED_PART_SIZE_EST * 2L;
			pthread_mutex_unlock(&lock);
		}

		char tmp_upt[560];
		pick_temp_upt_path("", scratch, tmp_upt, sizeof(tmp_upt));
		bool dl_ok = stream_unpack_to_file(http_parts_feed, &hf, tmp_upt);
		if (hf.stream_open) {
			pthread_mutex_lock(&lock);
			active_stream = NULL;
			pthread_mutex_unlock(&lock);
			http_stream_close(&hf.stream);
			hf.stream_open = false;
		}

		if (!dl_ok) {
			unlink(tmp_upt);
			finish_worker(is_cancelled() ? STOCKKEYS_CANCELLED : STOCKKEYS_DOWNLOAD_FAILED, last_detail);
			return NULL;
		}

		set_phase(STOCKKEYS_PHASE_EXTRACTING);
		FILE *uf = fopen(tmp_upt, "rb");
		extracted = uf && extract_from_open_nonzip(uf, &pairs);
		if (uf) {
			fclose(uf);
		}
		unlink(tmp_upt);

		if (!extracted) {
			wipe_mem(&pairs, sizeof(pairs));
			finish_worker(STOCKKEYS_EXTRACT_FAILED, NULL);
			return NULL;
		}
	}

	if (is_cancelled()) {
		wipe_mem(&pairs, sizeof(pairs));
		finish_worker(STOCKKEYS_CANCELLED, NULL);
		return NULL;
	}

	set_phase(STOCKKEYS_PHASE_EXTRACTING);
	const char *writable_card = (card_root && card_root[0] && storage_sd_writable()) ? card_root : NULL;
	bool saved = streamkeys_write_extracted(pairs.has_qobuz ? pairs.qobuz_app_id : NULL,
											pairs.has_qobuz ? pairs.qobuz_app_secret : NULL,
											pairs.has_tidal ? pairs.tidal_client_id : NULL,
											pairs.has_tidal ? pairs.tidal_client_secret : NULL, writable_card);
	wipe_mem(&pairs, sizeof(pairs));
	if (!saved) {
		finish_worker(STOCKKEYS_SAVE_FAILED, NULL);
		return NULL;
	}

	finish_worker(STOCKKEYS_OK, NULL);
	return NULL;
}

bool stockkeys_start(void) {
	pthread_mutex_lock(&lock);
	if (state == STOCKKEYS_RUNNING) {
		pthread_mutex_unlock(&lock);
		return false;
	}
	state = STOCKKEYS_RUNNING;
	cur_phase = STOCKKEYS_PHASE_SCANNING;
	cur_result = STOCKKEYS_OK;
	progress_done = 0;
	progress_total = 0;
	cancel_requested = false;
	active_stream = NULL;
	active_connect_fd = -1;
	last_detail[0] = '\0';
	pthread_mutex_unlock(&lock);

	pthread_t thread;
	if (pthread_create(&thread, NULL, worker_main, NULL) != 0) {
		finish_worker(STOCKKEYS_EXTRACT_FAILED, NULL);
		return true;
	}
	pthread_detach(thread);
	return true;
}

void stockkeys_cancel(void) {
	pthread_mutex_lock(&lock);
	if (state == STOCKKEYS_RUNNING) {
		cancel_requested = true;
		if (active_stream) {
			http_stream_wake(active_stream);
		}
		if (active_connect_fd >= 0) {
			(void)shutdown(active_connect_fd, SHUT_RDWR);
		}
	}
	pthread_mutex_unlock(&lock);
}

stockkeys_state_t stockkeys_state(void) {
	pthread_mutex_lock(&lock);
	stockkeys_state_t s = state;
	pthread_mutex_unlock(&lock);
	return s;
}

stockkeys_phase_t stockkeys_phase(void) {
	pthread_mutex_lock(&lock);
	stockkeys_phase_t p = cur_phase;
	pthread_mutex_unlock(&lock);
	return p;
}

stockkeys_result_t stockkeys_result(void) {
	pthread_mutex_lock(&lock);
	stockkeys_result_t r = cur_result;
	pthread_mutex_unlock(&lock);
	return r;
}

void stockkeys_progress(long *done, long *total) {
	pthread_mutex_lock(&lock);
	if (done) {
		*done = progress_done;
	}
	if (total) {
		*total = progress_total;
	}
	pthread_mutex_unlock(&lock);
}

const char *stockkeys_error_detail(void) {
	pthread_mutex_lock(&lock);
	const char *d = last_detail[0] ? last_detail : NULL;
	pthread_mutex_unlock(&lock);
	return d;
}

void stockkeys_acknowledge(void) {
	pthread_mutex_lock(&lock);
	if (state == STOCKKEYS_FINISHED) {
		state = STOCKKEYS_IDLE;
	}
	pthread_mutex_unlock(&lock);
}

bool stockkeys_busy(void) { return stockkeys_state() == STOCKKEYS_RUNNING; }
