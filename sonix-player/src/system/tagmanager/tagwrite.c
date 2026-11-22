#define _POSIX_C_SOURCE 200809L
#include "tagwrite.h"
#include <ctype.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

// Bound untrusted metadata allocations on a memory-constrained player.
#define LIMIT (16u * 1024u * 1024u)
#define COVER_LIMIT (2u * 1024u * 1024u)
typedef struct {
	tagwrite_field_t field;
	const char *value;
	const uint8_t *image;
	size_t image_size;
	int width, height;
} edit_t;
typedef struct {
	unsigned char *p;
	size_t n, cap;
} buf;
static int add(buf *b, const void *p, size_t n) {
	if (n > LIMIT || b->n > LIMIT - n)
		return 0;
	if (b->n + n > b->cap) {
		size_t cap = b->n + n;
		unsigned char *q = realloc(b->p, cap);
		if (!q)
			return 0;
		b->p = q;
		b->cap = cap;
	}
	memcpy(b->p + b->n, p, n);
	b->n += n;
	return 1;
}
static uint32_t be(const unsigned char *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static uint32_t le(const unsigned char *p) { return (uint32_t)p[3] << 24 | (uint32_t)p[2] << 16 | (uint32_t)p[1] << 8 | p[0]; }
static void putle(unsigned char *p, uint32_t n) {
	p[0] = n;
	p[1] = n >> 8;
	p[2] = n >> 16;
	p[3] = n >> 24;
}
static void putbe(unsigned char *p, uint32_t n) {
	p[0] = n >> 24;
	p[1] = n >> 16;
	p[2] = n >> 8;
	p[3] = n;
}
static int syncok(const unsigned char *p) { return !((p[0] | p[1] | p[2] | p[3]) & 128); }
static uint32_t syncget(const unsigned char *p) { return (uint32_t)p[0] << 21 | (uint32_t)p[1] << 14 | (uint32_t)p[2] << 7 | p[3]; }
static void syncput(unsigned char *p, uint32_t n) {
	p[0] = (n >> 21) & 127;
	p[1] = (n >> 14) & 127;
	p[2] = (n >> 7) & 127;
	p[3] = n & 127;
}
static int readn(FILE *f, void *p, size_t n) { return fread(p, 1, n, f) == n; }
// Ogg CRC: polynomial 0x04c11db7, initial 0, no reflection. The table is
// built once; the CRC field itself is zeroed before computing.
static uint32_t ogg_crc_table[256];
static int ogg_crc_table_ready;
static uint32_t ogg_crc(const unsigned char *p, size_t n) {
	if (!ogg_crc_table_ready) {
		for (uint32_t i = 0; i < 256; i++) {
			uint32_t r = i << 24;
			for (int j = 0; j < 8; j++)
				r = (r & 0x80000000u) ? ((r << 1) ^ 0x04c11db7u) : (r << 1);
			ogg_crc_table[i] = r;
		}
		ogg_crc_table_ready = 1;
	}
	uint32_t crc = 0;
	for (size_t i = 0; i < n; i++)
		crc = (crc << 8) ^ ogg_crc_table[((crc >> 24) & 0xff) ^ p[i]];
	return crc;
}
static int b64_encode(const unsigned char *in, size_t n, buf *out) {
	static const char alpha[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
	for (size_t i = 0; i < n; i += 3) {
		size_t left = n - i;
		uint32_t v = (uint32_t)in[i] << 16;
		if (left > 1)
			v |= (uint32_t)in[i + 1] << 8;
		if (left > 2)
			v |= in[i + 2];
		char q[4];
		q[0] = alpha[(v >> 18) & 63];
		q[1] = alpha[(v >> 12) & 63];
		q[2] = left > 1 ? alpha[(v >> 6) & 63] : '=';
		q[3] = left > 2 ? alpha[v & 63] : '=';
		if (!add(out, q, 4))
			return 0;
	}
	return 1;
}
static int valid_value(tagwrite_field_t field, const char *v) {
	if (!v || !*v || strlen(v) > ((field == TAGWRITE_TITLE || field == TAGWRITE_ARTIST) ? 255u : 127u) || strchr(v, '\n') || strchr(v, '\r'))
		return 0;
	if (field == TAGWRITE_GENRE || field == TAGWRITE_TITLE || field == TAGWRITE_ARTIST)
		return 1;
	if (field != TAGWRITE_TRACK)
		return 0;
	const char *p = v;
	unsigned long a = 0, b = 0;
	while (isdigit((unsigned char)*p)) {
		a = a * 10 + (*p++ - '0');
		if (a > 9999)
			return 0;
	}
	if (!a)
		return 0;
	if (*p == '/') {
		p++;
		if (!isdigit((unsigned char)*p))
			return 0;
		while (isdigit((unsigned char)*p)) {
			b = b * 10 + (*p++ - '0');
			if (b > 9999)
				return 0;
		}
		if (!b || b < a)
			return 0;
	}
	return !*p;
}
// UTF-8 to ID3v2.3 UTF-16LE with BOM. Reject invalid or overlong sequences.
static int utf16(const char *s, buf *out) {
	const unsigned char *p = (const unsigned char *)s;
	unsigned char bom[2] = {0xff, 0xfe};
	if (!add(out, bom, 2))
		return 0;
	while (*p) {
		uint32_t c;
		int n;
		if (*p < 0x80) {
			c = *p;
			n = 1;
		} else if (*p >= 0xc2 && *p <= 0xdf) {
			c = *p & 31;
			n = 2;
		} else if (*p >= 0xe0 && *p <= 0xef) {
			c = *p & 15;
			n = 3;
		} else if (*p >= 0xf0 && *p <= 0xf4) {
			c = *p & 7;
			n = 4;
		} else
			return 0;
		for (int i = 1; i < n; i++) {
			if (!p[i] || (p[i] & 0xc0) != 0x80)
				return 0;
			c = (c << 6) | (p[i] & 63);
		}
		if ((n == 3 && c < 0x800) || (n == 4 && c < 0x10000) || c > 0x10ffff || (c >= 0xd800 && c <= 0xdfff))
			return 0;
		unsigned char b[4];
		size_t len = 2;
		if (c > 0xffff) {
			c -= 0x10000;
			uint32_t a = 0xd800 + (c >> 10), d = 0xdc00 + (c & 1023);
			b[0] = a;
			b[1] = a >> 8;
			b[2] = d;
			b[3] = d >> 8;
			len = 4;
		} else {
			b[0] = c;
			b[1] = c >> 8;
		}
		if (!add(out, b, len))
			return 0;
		p += n;
	}
	return 1;
}
static const char *id3_id(tagwrite_field_t field) {
	switch (field) {
	case TAGWRITE_TRACK:
		return "TRCK";
	case TAGWRITE_GENRE:
		return "TCON";
	case TAGWRITE_TITLE:
		return "TIT2";
	case TAGWRITE_ARTIST:
		return "TPE1";
	}
	return NULL;
}
static const char *vorbis_key(tagwrite_field_t field) {
	switch (field) {
	case TAGWRITE_TRACK:
		return "TRACKNUMBER";
	case TAGWRITE_GENRE:
		return "GENRE";
	case TAGWRITE_TITLE:
		return "TITLE";
	case TAGWRITE_ARTIST:
		return "ARTIST";
	}
	return NULL;
}
// Return picture type, or -1 for malformed APIC. Other pictures stay intact.
static int apic_type(const unsigned char *p, size_t n) {
	if (n < 4 || p[0] > 3)
		return -1;
	size_t i = 1;
	while (i < n && p[i])
		i++;
	if (i + 1 >= n)
		return -1;
	return p[i + 1];
}
static int mp3(FILE *in, buf *out, const edit_t *edit) {
	unsigned char h[10];
	if (!readn(in, h, 10) || memcmp(h, "ID3", 3) || (h[3] != 3 && h[3] != 4) || h[4] || h[5] || !syncok(h + 6))
		return 0;

	uint32_t size = syncget(h + 6);
	if (size > LIMIT)
		return 0;
	unsigned char *tag = malloc(size ? size : 1);
	if (!tag)
		return 0;
	if (!readn(in, tag, size)) {
		free(tag);
		return 0;
	}
	buf frames = {0};
	size_t pos = 0;
	const char *id = edit->image ? "APIC" : id3_id(edit->field);
	int ok = 1, offsets = 0;
	while (pos < size) {
		if (tag[pos] == 0) {
			for (size_t i = pos; i < size; i++)
				if (tag[i])
					ok = 0;
			break;
		}
		if (size - pos < 10) {
			ok = 0;
			break;
		}
		unsigned char *f = tag + pos;
		for (int i = 0; i < 4; i++)
			if (!((f[i] >= 'A' && f[i] <= 'Z') || (f[i] >= '0' && f[i] <= '9')))
				ok = 0;
		if (h[3] == 4 && !syncok(f + 4))
			ok = 0;
		uint32_t n = h[3] == 4 ? syncget(f + 4) : be(f + 4);
		if (!ok || n > size - pos - 10) {
			ok = 0;
			break;
		}
		if (!memcmp(f, "CHAP", 4) || !memcmp(f, "CTOC", 4) || !memcmp(f, "SEEK", 4) || !memcmp(f, "ASPI", 4))
			offsets = 1;
		// Do not discard an encrypted, compressed, grouped, or otherwise
		// flagged target frame. Unknown frames are retained byte-for-byte.
		int replace = !memcmp(f, id, 4);
		if (replace && (f[8] || f[9])) {
			ok = 0;
			break;
		}
		if (replace && edit->image) {
			int type = apic_type(f + 10, n);
			if (type < 0) {
				ok = 0;
				break;
			}
			replace = (type == 3);
		}
		if (!replace && !add(&frames, f, 10 + n)) {
			ok = 0;
			break;
		}
		pos += 10 + n;
	}
	free(tag);
	// Do not edit a competing ID3 tag or a different container behind this
	// ID3 prefix (FLAC can carry both ID3 and native metadata). Editing the
	// prefix alone would leave the actual tags and artwork unchanged.
	unsigned char next[8];
	size_t peek = fread(next, 1, sizeof(next), in);
	if (ferror(in) || (peek >= 3 && !memcmp(next, "ID3", 3)) || (peek >= 4 && (!memcmp(next, "fLaC", 4) || !memcmp(next, "RIFF", 4) || !memcmp(next, "FORM", 4) || !memcmp(next, "OggS", 4))) ||
	    (peek == 8 && !memcmp(next + 4, "ftyp", 4)))
		ok = 0;
	if (fseeko(in, -(off_t)peek, SEEK_CUR))
		ok = 0;
	buf payload = {0};
	if (edit->image) {
		const unsigned char pic_header[] = {0, 'i', 'm', 'a', 'g', 'e', '/', 'j', 'p', 'e', 'g', 0, 3, 0};
		ok = ok && add(&payload, pic_header, sizeof(pic_header)) && add(&payload, edit->image, edit->image_size);
	} else {
		bool ascii = true;
		for (const unsigned char *p = (const unsigned char *)edit->value; *p; p++)
			if (*p >= 128)
				ascii = false;
		unsigned char enc = h[3] == 4 ? 3 : (ascii ? 0 : 1);
		ok = ok && add(&payload, &enc, 1);
		if (ok && enc == 1)
			ok = utf16(edit->value, &payload);
		else if (ok)
			ok = add(&payload, edit->value, strlen(edit->value));
	}
	unsigned char fh[10] = {0};
	memcpy(fh, id, 4);
	if (h[3] == 4)
		syncput(fh + 4, payload.n);
	else
		putbe(fh + 4, payload.n);
	if (ok)
		ok = add(&frames, fh, 10) && add(&frames, payload.p, payload.n);
	free(payload.p);
	// Keep the audio at the same offset whenever the original padding permits.
	// If it cannot, reject known frames that can carry absolute file offsets.
	if (ok && frames.n > size && offsets)
		ok = 0;
	if (ok && frames.n < size) {
		size_t pad = size - frames.n;
		unsigned char zeros[256] = {0};
		while (ok && pad) {
			size_t step = pad > sizeof(zeros) ? sizeof(zeros) : pad;
			ok = add(&frames, zeros, step);
			pad -= step;
		}
	}
	if (ok && frames.n < (1u << 28)) {
		syncput(h + 6, frames.n);
		ok = add(out, h, 10) && add(out, frames.p, frames.n);
	} else
		ok = 0;
	free(frames.p);
	return ok;
}
static int comments(const unsigned char *p, size_t n, buf *out, tagwrite_field_t field, const char *v) {
	if (n < 8)
		return 0;
	uint32_t vendor = le(p);
	if (vendor > n - 8)
		return 0;
	size_t pos = 4 + vendor;
	uint32_t count = le(p + pos);
	pos += 4;
	if (count > (n - pos) / 4)
		return 0;
	if (!add(out, p, 4 + vendor))
		return 0;
	size_t countpos = out->n;
	unsigned char zero[4] = {0};
	if (!add(out, zero, 4))
		return 0;
	const char *key = vorbis_key(field);
	size_t kl = strlen(key);
	uint32_t kept = 0;
	for (uint32_t i = 0; i < count; i++) {
		if (n - pos < 4)
			return 0;
		uint32_t len = le(p + pos);
		pos += 4;
		if (len > n - pos)
			return 0;
		if (!(len > kl && p[pos + kl] == '=' && !strncasecmp((const char *)p + pos, key, kl))) {
			unsigned char sz[4];
			putle(sz, len);
			if (!add(out, sz, 4) || !add(out, p + pos, len))
				return 0;
			kept++;
		}
		pos += len;
	}
	if (pos != n)
		return 0;
	unsigned char sz[4];
	putle(sz, kl + 1 + strlen(v));
	if (!add(out, sz, 4) || !add(out, key, kl) || !add(out, "=", 1) || !add(out, v, strlen(v)))
		return 0;
	putle(out->p + countpos, kept + 1);
	return 1;
}
static int flac(FILE *in, buf *out, const edit_t *edit) {
	unsigned char magic[4];
	if (!readn(in, magic, 4) || memcmp(magic, "fLaC", 4) || !add(out, magic, 4))
		return 0;
	int last = 0, seen = 0, index = 0;
	while (!last) {
		unsigned char h[4];
		if (!readn(in, h, 4))
			return 0;
		last = h[0] & 128;
		int type = h[0] & 127;
		uint32_t n = (uint32_t)h[1] << 16 | (uint32_t)h[2] << 8 | h[3];
		if (type == 127 || (index == 0 && (type != 0 || n != 34)) || (index > 0 && type == 0) || n > LIMIT)
			return 0;
		unsigned char *p = malloc(n ? n : 1);
		if (!p)
			return 0;
		if (!readn(in, p, n)) {
			free(p);
			return 0;
		}
		int ok = 1;
		if (edit->image) {
			// Retain non-front pictures; a malformed PICTURE block is unsafe.
			if (type == 6 && n < 4)
				ok = 0;
			int drop = type == 6 && n >= 4 && be(p) == 3;
			if (last)
				h[0] &= 0x7f;
			if (ok && !drop)
				ok = add(out, h, 4) && add(out, p, n);
			if (ok && last) {
				buf pic = {0};
				unsigned char num[4];
				putbe(num, 3);
				ok = add(&pic, num, 4);
				putbe(num, 10);
				ok = ok && add(&pic, num, 4) && add(&pic, "image/jpeg", 10);
				putbe(num, 0);
				ok = ok && add(&pic, num, 4);
				putbe(num, edit->width);
				ok = ok && add(&pic, num, 4);
				putbe(num, edit->height);
				ok = ok && add(&pic, num, 4);
				putbe(num, 24);
				ok = ok && add(&pic, num, 4);
				putbe(num, 0);
				ok = ok && add(&pic, num, 4);
				putbe(num, edit->image_size);
				ok = ok && add(&pic, num, 4) && add(&pic, edit->image, edit->image_size);
				unsigned char ph[4] = {0x86, pic.n >> 16, pic.n >> 8, pic.n};
				ok = ok && pic.n <= 0xffffff && add(out, ph, 4) && add(out, pic.p, pic.n);
				free(pic.p);
			}
		} else if (type == 4) {
			buf b = {0};
			if (seen++)
				ok = 0;
			else
				ok = comments(p, n, &b, edit->field, edit->value);
			if (ok && b.n <= 0xffffff) {
				h[1] = b.n >> 16;
				h[2] = b.n >> 8;
				h[3] = b.n;
				ok = add(out, h, 4) && add(out, b.p, b.n);
			} else
				ok = 0;
			free(b.p);
		} else if (last && !seen) {
			// Preserve STREAMINFO as the first block; append the new last block.
			unsigned char base[8] = {0};
			buf b = {0};
			ok = comments(base, 8, &b, edit->field, edit->value);
			unsigned char ch[4] = {0x84, (unsigned char)(b.n >> 16), (unsigned char)(b.n >> 8), (unsigned char)b.n};
			h[0] &= 0x7f;
			if (ok)
				ok = add(out, h, 4) && add(out, p, n) && add(out, ch, 4) && add(out, b.p, b.n);
			free(b.p);
			seen = 1;
		} else
			ok = add(out, h, 4) && add(out, p, n);
		free(p);
		if (!ok)
			return 0;
		index++;
	}
	return edit->image ? 1 : seen;
}
// ---------------------------------------------------------------------------
// MP4 (M4A/M4B, AAC or ALAC in an MP4 container)
// ---------------------------------------------------------------------------
//
// The tags live in moov/udta/meta/ilst. Rebuilding that box changes the size
// of moov itself, which moves everything after it. When moov sits after the
// audio (mdat), nothing else moves and the edit is a plain rewrite. When moov
// sits before mdat, the audio must stay put: a shrink is padded with a free
// box, a growth eats a following free box, and anything else shifts the audio
// and patches every chunk offset (stco/co64) by the same amount. A file whose
// layout cannot be kept consistent is refused rather than rewritten.

typedef struct {
	uint32_t type;
	uint64_t off, total, header;
} mp4_top_t;

static uint32_t mp4_type(const unsigned char *p) { return be(p); }

// Parses a sequence of boxes in a memory buffer. Returns 0 on malformed input.
typedef struct {
	uint32_t type;
	size_t off, header, total;
} mp4_box_t;
static int mp4_parse(const unsigned char *p, size_t n, mp4_box_t *boxes, int max, int *count) {
	size_t pos = 0;
	int c = 0;
	while (pos < n) {
		if (n - pos < 8 || c >= max)
			return 0;
		uint32_t size = be(p + pos);
		uint32_t type = be(p + pos + 4);
		size_t header = 8;
		uint64_t total;
		if (size == 1) {
			if (n - pos < 16)
				return 0;
			total = ((uint64_t)be(p + pos + 8) << 32) | be(p + pos + 12);
			header = 16;
		} else if (size == 0) {
			total = n - pos;
		} else {
			total = size;
		}
		if (total < header || total > n - pos)
			return 0;
		boxes[c].type = type;
		boxes[c].off = pos;
		boxes[c].header = header;
		boxes[c].total = (size_t)total;
		c++;
		pos += (size_t)total;
	}
	*count = c;
	return pos == n;
}
static int mp4_write_header(buf *out, uint32_t type, size_t payload) {
	if (payload + 8 > 0xffffffffu || payload + 8 > LIMIT)
		return 0;
	unsigned char h[8];
	putbe(h, (uint32_t)(payload + 8));
	putbe(h + 4, type);
	return add(out, h, 8);
}
// Builds one ilst item: size, type, one data box (type flag, locale, payload).
static int mp4_build_item(buf *out, uint32_t type, uint32_t flag, const unsigned char *payload, size_t plen) {
	if (plen + 24 > 0xffffffffu || plen + 24 > LIMIT)
		return 0;
	unsigned char h[8];
	putbe(h, (uint32_t)(plen + 24));
	putbe(h + 4, type);
	if (!add(out, h, 8))
		return 0;
	unsigned char dh[8];
	putbe(dh, (uint32_t)(plen + 16));
	putbe(dh + 4, mp4_type((const unsigned char *)"data"));
	if (!add(out, dh, 8))
		return 0;
	unsigned char fl[8];
	putbe(fl, flag);
	putbe(fl + 4, 0);
	return add(out, fl, 8) && add(out, payload, plen);
}
static uint32_t mp4_target_type(const edit_t *edit) {
	if (edit->image)
		return mp4_type((const unsigned char *)"covr");
	switch (edit->field) {
	case TAGWRITE_TRACK:
		return mp4_type((const unsigned char *)"trkn");
	case TAGWRITE_GENRE:
		return mp4_type((const unsigned char *)"\xa9" "gen");
	case TAGWRITE_TITLE:
		return mp4_type((const unsigned char *)"\xa9" "nam");
	case TAGWRITE_ARTIST:
		return mp4_type((const unsigned char *)"\xa9" "ART");
	}
	return 0;
}
// Rebuilds an ilst box (header included) with one item replaced or added.
static int mp4_rebuild_ilst(const unsigned char *p, size_t n, const edit_t *edit, buf *out) {
	if (n < 8)
		return 0;
	size_t header = 8;
	uint32_t size = be(p);
	if (size == 1) {
		if (n < 16)
			return 0;
		uint64_t t = ((uint64_t)be(p + 8) << 32) | be(p + 12);
		if (t != n)
			return 0;
		header = 16;
	} else if (size != n) {
		return 0;
	}
	mp4_box_t boxes[256];
	int count = 0;
	if (!mp4_parse(p + header, n - header, boxes, 256, &count))
		return 0;
	uint32_t target = mp4_target_type(edit);
	uint32_t gnre = mp4_type((const unsigned char *)"gnre");
	// New item payload.
	buf payload = {0};
	int ok = 1;
	if (edit->image) {
		ok = add(&payload, edit->image, edit->image_size);
	} else if (edit->field == TAGWRITE_TRACK) {
		unsigned long a = 0, b = 0;
		const char *s = edit->value;
		while (*s >= '0' && *s <= '9')
			a = a * 10 + (*s++ - '0');
		if (*s == '/') {
			s++;
			while (*s >= '0' && *s <= '9')
				b = b * 10 + (*s++ - '0');
		}
		unsigned char tr[8] = {0, 0, (unsigned char)(a >> 8), (unsigned char)a, (unsigned char)(b >> 8), (unsigned char)b, 0, 0};
		ok = add(&payload, tr, 8);
	} else {
		ok = add(&payload, edit->value, strlen(edit->value));
	}
	if (!ok) {
		free(payload.p);
		return 0;
	}
	buf body = {0};
	int found = 0;
	for (int i = 0; i < count; i++) {
		uint32_t t = boxes[i].type;
		const unsigned char *item = p + header + boxes[i].off;
		if (edit->field == TAGWRITE_GENRE && !edit->image && t == gnre)
			continue; // superseded by the text genre below
		if (t == target) {
			if (!mp4_build_item(&body, t, edit->image ? 13 : (edit->field == TAGWRITE_TRACK ? 0 : 1), payload.p, payload.n)) {
				ok = 0;
				break;
			}
			found = 1;
		} else if (!add(&body, item, boxes[i].total)) {
			ok = 0;
			break;
		}
	}
	if (ok && !found)
		ok = mp4_build_item(&body, target, edit->image ? 13 : (edit->field == TAGWRITE_TRACK ? 0 : 1), payload.p, payload.n);
	free(payload.p);
	if (!ok) {
		free(body.p);
		return 0;
	}
	ok = mp4_write_header(out, mp4_type((const unsigned char *)"ilst"), body.n) && add(out, body.p, body.n);
	free(body.p);
	return ok;
}
// Rebuilds a plain container (moov/udta) or meta (with its 4-byte prefix).
static int mp4_rebuild_container(const unsigned char *p, size_t n, const edit_t *edit, buf *out, int is_meta) {
	if (n < 8)
		return 0;
	size_t header = 8;
	uint32_t size = be(p);
	if (size == 1) {
		if (n < 16)
			return 0;
		uint64_t t = ((uint64_t)be(p + 8) << 32) | be(p + 12);
		if (t != n)
			return 0;
		header = 16;
	} else if (size != n) {
		return 0;
	}
	uint32_t type = be(p + 4);
	size_t prefix = 0;
	if (is_meta) {
		// A FullBox carries version/flags first, unless the first child
		// already starts where they would be (QuickTime style).
		if (n - header >= 8) {
			uint32_t maybe = be(p + header + 4);
			if (maybe != mp4_type((const unsigned char *)"hdlr") && maybe != mp4_type((const unsigned char *)"ilst") &&
				maybe != mp4_type((const unsigned char *)"keys"))
				prefix = 4;
		} else {
			prefix = 4;
		}
		if (n - header < prefix)
			return 0;
	}
	mp4_box_t boxes[256];
	int count = 0;
	if (!mp4_parse(p + header + prefix, n - header - prefix, boxes, 256, &count))
		return 0;
	buf body = {0};
	int ok = 1, found = 0;
	if (prefix && !add(&body, p + header, prefix))
		ok = 0;
	for (int i = 0; ok && i < count; i++) {
		const unsigned char *child = p + header + prefix + boxes[i].off;
		uint32_t ct = boxes[i].type;
		if (ct == mp4_type((const unsigned char *)"ilst")) {
			if (found || !mp4_rebuild_ilst(child, boxes[i].total, edit, &body)) {
				ok = 0;
				break;
			}
			found = 1;
		} else if (ct == mp4_type((const unsigned char *)"udta") || ct == mp4_type((const unsigned char *)"meta")) {
			if (found || !mp4_rebuild_container(child, boxes[i].total, edit, &body, ct == mp4_type((const unsigned char *)"meta"))) {
				ok = 0;
				break;
			}
			found = 1;
		} else if (!add(&body, child, boxes[i].total)) {
			ok = 0;
			break;
		}
	}
	if (!ok || !found) {
		free(body.p);
		return 0;
	}
	ok = mp4_write_header(out, type, body.n) && add(out, body.p, body.n);
	free(body.p);
	return ok;
}
// Patches every stco/co64 entry in a rebuilt moov by delta.
static int mp4_fixup_walk(const unsigned char *p, size_t n, int64_t delta) {
	mp4_box_t boxes[256];
	int count = 0;
	if (!mp4_parse(p, n, boxes, 256, &count))
		return 0;
	for (int i = 0; i < count; i++) {
		const unsigned char *b = p + boxes[i].off;
		uint32_t t = boxes[i].type;
		const unsigned char *payload = b + boxes[i].header;
		size_t plen = boxes[i].total - boxes[i].header;
		if (t == mp4_type((const unsigned char *)"stco")) {
			if (plen < 8)
				return 0;
			uint32_t cnt = be(payload + 4);
			if (plen < 8 + (size_t)cnt * 4)
				return 0;
			for (uint32_t j = 0; j < cnt; j++) {
				unsigned char *e = (unsigned char *)payload + 8 + (size_t)j * 4;
				int64_t v = (int64_t)be(e) + delta;
				if (v < 0 || v > 0xffffffffLL)
					return 0;
				putbe(e, (uint32_t)v);
			}
		} else if (t == mp4_type((const unsigned char *)"co64")) {
			if (plen < 8)
				return 0;
			uint32_t cnt = be(payload + 4);
			if (plen < 8 + (size_t)cnt * 8)
				return 0;
			for (uint32_t j = 0; j < cnt; j++) {
				unsigned char *e = (unsigned char *)payload + 8 + (size_t)j * 8;
				uint64_t v = ((uint64_t)be(e) << 32) | be(e + 4);
				if (delta < 0 && v < (uint64_t)(-delta))
					return 0;
				v += (uint64_t)delta;
				putbe(e, (uint32_t)(v >> 32));
				putbe(e + 4, (uint32_t)v);
			}
		} else if (t == mp4_type((const unsigned char *)"moov") || t == mp4_type((const unsigned char *)"trak") ||
			   t == mp4_type((const unsigned char *)"mdia") || t == mp4_type((const unsigned char *)"minf") ||
			   t == mp4_type((const unsigned char *)"stbl")) {
			if (!mp4_fixup_walk(payload, plen, delta))
				return 0;
		}
	}
	return 1;
}
static int mp4_copy_range(FILE *in, FILE *out, uint64_t off, uint64_t len) {
	if (fseeko(in, (off_t)off, SEEK_SET))
		return 0;
	unsigned char chunk[32768];
	while (len) {
		size_t want = len > sizeof(chunk) ? sizeof(chunk) : (size_t)len;
		if (fread(chunk, 1, want, in) != want || fwrite(chunk, 1, want, out) != want)
			return 0;
		len -= want;
	}
	return 1;
}
// Full MP4 writer: copies in to out with the rebuilt moov. Returns 1 ok,
// 0 unsupported, -1 io error.
static int mp4_write(FILE *in, FILE *out, const edit_t *edit) {
	struct stat st;
	if (fstat(fileno(in), &st) || st.st_size < 16)
		return 0;
	uint64_t fsize = (uint64_t)st.st_size;
	// Scan top-level boxes.
	mp4_top_t moov = {0}, freebox = {0};
	int have_moov = 0, have_free = 0, mdat_before = 0, mdat_after = 0;
	uint64_t pos = 0;
	while (pos < fsize) {
		unsigned char h[16];
		if (fseeko(in, (off_t)pos, SEEK_SET) || fread(h, 1, 8, in) != 8)
			return 0;
		uint32_t size = be(h);
		uint32_t type = be(h + 4);
		uint64_t header = 8, total;
		if (size == 1) {
			if (fread(h + 8, 1, 8, in) != 8)
				return 0;
			total = ((uint64_t)be(h + 8) << 32) | be(h + 12);
			header = 16;
		} else if (size == 0) {
			total = fsize - pos;
		} else {
			total = size;
		}
		if (total < header || pos + total > fsize)
			return 0;
		if (type == mp4_type((const unsigned char *)"moov")) {
			if (have_moov)
				return 0;
			have_moov = 1;
			moov.type = type;
			moov.off = pos;
			moov.total = total;
			moov.header = header;
		} else if (type == mp4_type((const unsigned char *)"mdat")) {
			if (have_moov)
				mdat_after = 1;
			else
				mdat_before = 1;
		} else if ((type == mp4_type((const unsigned char *)"free") || type == mp4_type((const unsigned char *)"skip")) && have_moov && !mdat_after && !have_free) {
			have_free = 1;
			freebox.type = type;
			freebox.off = pos;
			freebox.total = total;
			freebox.header = header;
		}
		pos += total;
	}
	if (!have_moov || moov.total > LIMIT)
		return 0;
	if (mdat_before && mdat_after)
		return 0; // audio on both sides: offsets cannot be kept consistent
	// Read moov and rebuild it.
	unsigned char *old = malloc((size_t)moov.total);
	if (!old)
		return 0;
	if (fseeko(in, (off_t)moov.off, SEEK_SET) || fread(old, 1, (size_t)moov.total, in) != (size_t)moov.total) {
		free(old);
		return 0;
	}
	buf rebuilt = {0};
	int ok = mp4_rebuild_container(old, (size_t)moov.total, edit, &rebuilt, 0);
	free(old);
	if (!ok) {
		free(rebuilt.p);
		return 0;
	}
	int64_t delta = (int64_t)rebuilt.n - (int64_t)moov.total;
	int need_fixup = 0, absorb = 0, insert_free = 0;
	if (delta != 0 && mdat_after) {
		if (delta < 0 && -delta >= 8)
			insert_free = 1;
		else if (delta > 0 && have_free && (freebox.total - (uint64_t)delta == 0 || freebox.total - (uint64_t)delta >= 8))
			absorb = 1;
		else
			need_fixup = 1;
	}
	if (need_fixup && !mp4_fixup_walk(rebuilt.p, rebuilt.n, delta)) {
		free(rebuilt.p);
		return 0;
	}
	// Copy: prefix, new moov, then the rest.
	int w = mp4_copy_range(in, out, 0, moov.off);
	if (w && fwrite(rebuilt.p, 1, rebuilt.n, out) != rebuilt.n)
		w = 0;
	free(rebuilt.p);
	uint64_t rest = moov.off + moov.total;
	if (w && insert_free) {
		unsigned char fh[8];
		putbe(fh, (uint32_t)(-delta));
		putbe(fh + 4, mp4_type((const unsigned char *)"free"));
		if (fwrite(fh, 1, 8, out) != 8)
			w = 0;
		uint64_t zeros = (uint64_t)(-delta) - 8;
		unsigned char z[256] = {0};
		while (w && zeros) {
			size_t step = zeros > sizeof(z) ? sizeof(z) : (size_t)zeros;
			if (fwrite(z, 1, step, out) != step)
				w = 0;
			zeros -= step;
		}
	}
	if (w && absorb) {
		uint64_t new_free = freebox.total - (uint64_t)delta;
		if (new_free >= 8) {
			unsigned char fh[8];
			putbe(fh, (uint32_t)new_free);
			putbe(fh + 4, freebox.type);
			if (fwrite(fh, 1, 8, out) != 8)
				w = 0;
			if (w && !mp4_copy_range(in, out, freebox.off + freebox.header, new_free - 8))
				w = 0;
			rest = freebox.off + freebox.total;
		} else {
			rest = freebox.off + freebox.total; // free box consumed entirely
		}
	}
	if (w && rest < fsize && !mp4_copy_range(in, out, rest, fsize - rest))
		w = 0;
	return w ? 1 : -1;
}

// ---------------------------------------------------------------------------
// Ogg (Opus and Vorbis): the OpusTags / Vorbis comment packet
// ---------------------------------------------------------------------------
//
// The comment packet is repaginated into fresh Ogg pages and the following
// pages of the same stream are renumbered (and their CRCs redone) so the
// sequence stays contiguous. Pages of other streams and any chained stream
// after the first stream's end marker are copied untouched. A comment packet
// that does not end on a page boundary, or a multiplexed header, is refused.

typedef struct {
	unsigned char *data;
	size_t len;
	uint32_t serial, seq;
	uint8_t type;
	uint64_t granule;
	int nseg;
	uint8_t segs[255];
	size_t data_off;
} ogg_page_t;
static void ogg_page_free(ogg_page_t *p) {
	free(p->data);
	p->data = NULL;
	p->len = 0;
}
// 1 ok, 0 clean EOF, -1 malformed.
static int ogg_read_page(FILE *in, ogg_page_t *pg) {
	ogg_page_free(pg);
	unsigned char h[27];
	size_t got = fread(h, 1, sizeof(h), in);
	if (got == 0)
		return ferror(in) ? -1 : 0;
	if (got != sizeof(h) || memcmp(h, "OggS", 4) || h[4] != 0)
		return -1;
	int nseg = h[26];
	unsigned char segs[255];
	if (nseg && fread(segs, 1, (size_t)nseg, in) != (size_t)nseg)
		return -1;
	size_t data_len = 0;
	for (int i = 0; i < nseg; i++)
		data_len += segs[i];
	if (data_len > LIMIT)
		return -1;
	unsigned char *data = malloc(27 + (size_t)nseg + data_len);
	if (!data)
		return -1;
	memcpy(data, h, 27);
	memcpy(data + 27, segs, (size_t)nseg);
	if (data_len && fread(data + 27 + nseg, 1, data_len, in) != data_len) {
		free(data);
		return -1;
	}
	// Validate CRC (field zeroed).
	uint32_t want = (uint32_t)h[22] | ((uint32_t)h[23] << 8) | ((uint32_t)h[24] << 16) | ((uint32_t)h[25] << 24);
	data[22] = data[23] = data[24] = data[25] = 0;
	if (ogg_crc(data, 27 + (size_t)nseg + data_len) != want) {
		free(data);
		return -1;
	}
	pg->data = data;
	pg->len = 27 + (size_t)nseg + data_len;
	pg->serial = (uint32_t)h[14] | ((uint32_t)h[15] << 8) | ((uint32_t)h[16] << 16) | ((uint32_t)h[17] << 24);
	pg->seq = (uint32_t)h[18] | ((uint32_t)h[19] << 8) | ((uint32_t)h[20] << 16) | ((uint32_t)h[21] << 24);
	pg->type = h[5];
	pg->granule = 0;
	for (int i = 0; i < 8; i++)
		pg->granule |= (uint64_t)h[6 + i] << (8 * i);
	pg->nseg = nseg;
	memcpy(pg->segs, segs, (size_t)nseg);
	pg->data_off = 27 + (size_t)nseg;
	return 1;
}
static int ogg_write_page(FILE *out, ogg_page_t *pg) {
	pg->data[22] = pg->data[23] = pg->data[24] = pg->data[25] = 0;
	uint32_t crc = ogg_crc(pg->data, pg->len);
	pg->data[22] = (unsigned char)crc;
	pg->data[23] = (unsigned char)(crc >> 8);
	pg->data[24] = (unsigned char)(crc >> 16);
	pg->data[25] = (unsigned char)(crc >> 24);
	return fwrite(pg->data, 1, pg->len, out) == pg->len;
}
// Builds one Ogg page holding a slice of a packet. Returns 0 on error.
static int ogg_build_page(buf *out, uint32_t serial, uint32_t seq, uint8_t type, const unsigned char *data, size_t n, int first_seg) {
	// Segments for this page: 255s, then the remainder (<255, possibly 0).
	size_t segs_needed = n / 255 + 1;
	size_t segs_here = segs_needed > 255 ? 255 : segs_needed;
	size_t data_here = segs_here == segs_needed ? n : 255 * segs_here;
	size_t start = out->n;
	unsigned char h[27] = {0};
	memcpy(h, "OggS", 4);
	h[5] = type;
	h[14] = (unsigned char)serial;
	h[15] = (unsigned char)(serial >> 8);
	h[16] = (unsigned char)(serial >> 16);
	h[17] = (unsigned char)(serial >> 24);
	h[18] = (unsigned char)seq;
	h[19] = (unsigned char)(seq >> 8);
	h[20] = (unsigned char)(seq >> 16);
	h[21] = (unsigned char)(seq >> 24);
	h[26] = (unsigned char)segs_here;
	if (!add(out, h, 27))
		return 0;
	for (size_t i = 0; i < segs_here; i++) {
		unsigned char s = (i + 1 == segs_here) ? (unsigned char)(data_here - 255 * (segs_here - 1)) : 255;
		if (!add(out, &s, 1))
			return 0;
	}
	(void)first_seg;
	if (!add(out, data, data_here))
		return 0;
	// CRC over the page with its field zeroed (already zero), patched in.
	uint32_t crc = ogg_crc(out->p + start, out->n - start);
	out->p[start + 22] = (unsigned char)crc;
	out->p[start + 23] = (unsigned char)(crc >> 8);
	out->p[start + 24] = (unsigned char)(crc >> 16);
	out->p[start + 25] = (unsigned char)(crc >> 24);
	return 1;
}
// Replaces the front-cover picture in a Vorbis comment block (vendor+count).
static int ogg_cover_comments(const unsigned char *p, size_t n, const edit_t *edit, buf *out) {
	if (n < 8)
		return 0;
	uint32_t vendor = le(p);
	if (vendor > n - 8)
		return 0;
	size_t pos = 4 + vendor;
	uint32_t count = le(p + pos);
	pos += 4;
	if (count > (n - pos) / 4)
		return 0;
	// FLAC PICTURE block for the new front cover.
	buf pic = {0};
	unsigned char num[4];
	putbe(num, 3);
	int ok = add(&pic, num, 4);
	putbe(num, 10);
	ok = ok && add(&pic, num, 4) && add(&pic, "image/jpeg", 10);
	putbe(num, 0);
	ok = ok && add(&pic, num, 4);
	putbe(num, (uint32_t)edit->width);
	ok = ok && add(&pic, num, 4);
	putbe(num, (uint32_t)edit->height);
	ok = ok && add(&pic, num, 4);
	putbe(num, 24);
	ok = ok && add(&pic, num, 4);
	putbe(num, 0);
	ok = ok && add(&pic, num, 4);
	putbe(num, (uint32_t)edit->image_size);
	ok = ok && add(&pic, num, 4) && add(&pic, edit->image, edit->image_size);
	if (!ok) {
		free(pic.p);
		return 0;
	}
	buf b64 = {0};
	ok = b64_encode(pic.p, pic.n, &b64);
	free(pic.p);
	if (!ok) {
		free(b64.p);
		return 0;
	}
	const char *key = "METADATA_BLOCK_PICTURE";
	size_t kl = strlen(key);
	if (!add(out, p, 4 + vendor)) {
		free(b64.p);
		return 0;
	}
	size_t countpos = out->n;
	unsigned char zero[4] = {0};
	if (!add(out, zero, 4)) {
		free(b64.p);
		return 0;
	}
	uint32_t kept = 0;
	for (uint32_t i = 0; i < count; i++) {
		if (n - pos < 4) {
			free(b64.p);
			return 0;
		}
		uint32_t len = le(p + pos);
		pos += 4;
		if (len > n - pos) {
			free(b64.p);
			return 0;
		}
		int picture = (len > 22 && p[pos + 22] == '=' && !strncasecmp((const char *)p + pos, "METADATA_BLOCK_PICTURE", 22)) ||
			      (len > 8 && p[pos + 8] == '=' && !strncasecmp((const char *)p + pos, "COVERART", 8));
		if (!picture) {
			unsigned char sz[4];
			putle(sz, len);
			if (!add(out, sz, 4) || !add(out, p + pos, len)) {
				free(b64.p);
				return 0;
			}
			kept++;
		}
		pos += len;
	}
	if (pos != n) {
		free(b64.p);
		return 0;
	}
	unsigned char sz[4];
	putle(sz, (uint32_t)(kl + 1 + b64.n));
	ok = add(out, sz, 4) && add(out, key, kl) && add(out, "=", 1) && add(out, b64.p, b64.n);
	free(b64.p);
	if (!ok)
		return 0;
	putle(out->p + countpos, kept + 1);
	return 1;
}
// Full Ogg writer. Returns 1 ok, 0 unsupported, -1 io error.
static int ogg_write(FILE *in, FILE *out, const edit_t *edit) {
	// Refuse an ID3 prefix: editing the Ogg stream alone would leave the
	// competing tag (and its artwork) in charge.
	unsigned char head[4];
	if (fread(head, 1, 4, in) != 4 || memcmp(head, "OggS", 4))
		return 0;
	if (fseeko(in, 0, SEEK_SET))
		return 0;
	ogg_page_t pg = {0};
	int r = ogg_read_page(in, &pg);
	if (r != 1 || !(pg.type & 0x02)) {
		ogg_page_free(&pg);
		return 0;
	}
	uint32_t serial = pg.serial;
	// First packet on the BOS page must be the identification header alone.
	const unsigned char *d = pg.data + pg.data_off;
	size_t dlen = pg.len - pg.data_off;
	int opus = 0, vorbis = 0;
	if (dlen >= 8 && !memcmp(d, "OpusHead", 8))
		opus = 1;
	else if (dlen >= 7 && d[0] == 1 && !memcmp(d + 1, "vorbis", 6))
		vorbis = 1;
	if (!opus && !vorbis) {
		ogg_page_free(&pg);
		return 0;
	}
	// The identification packet must end on this page with nothing after it.
	{
		size_t off = 0;
		int packets = 0, ended = 0;
		for (int i = 0; i < pg.nseg; i++) {
			off += pg.segs[i];
			if (pg.segs[i] < 255) {
				packets++;
				if (i != pg.nseg - 1)
					ended = 1; // another packet starts on this page
			}
		}
		if (packets != 1 || ended || off != dlen) {
			ogg_page_free(&pg);
			return 0;
		}
	}
	buf id_packet = {0};
	if (!add(&id_packet, d, dlen)) {
		ogg_page_free(&pg);
		return 0;
	}
	ogg_page_free(&pg);
	// Read pages until the comment packet is complete.
	buf tags_packet = {0};
	size_t tags_pages = 0;
	int tags_done = 0, first = 1;
	while (!tags_done) {
		r = ogg_read_page(in, &pg);
		if (r != 1 || pg.serial != serial) {
			ogg_page_free(&pg);
			free(id_packet.p);
			free(tags_packet.p);
			return 0;
		}
		tags_pages++;
		const unsigned char *pd = pg.data + pg.data_off;
		size_t plen = pg.len - pg.data_off;
		if (first && pg.type & 0x01) { // continued from the BOS page: unexpected
			ogg_page_free(&pg);
			free(id_packet.p);
			free(tags_packet.p);
			return 0;
		}
		first = 0;
		if (!add(&tags_packet, pd, plen) || tags_packet.n > LIMIT) {
			ogg_page_free(&pg);
			free(id_packet.p);
			free(tags_packet.p);
			return 0;
		}
		// Packet ends when a segment <255 is the last segment of the page.
		if (pg.nseg > 0 && pg.segs[pg.nseg - 1] < 255) {
			// Must be the only packet ending here: no earlier segment <255
			// (that would mean a previous packet ended on this page).
			for (int i = 0; i + 1 < pg.nseg; i++) {
				if (pg.segs[i] < 255) {
					ogg_page_free(&pg);
					free(id_packet.p);
					free(tags_packet.p);
					return 0;
				}
			}
			tags_done = 1;
		} else if (pg.nseg == 0) {
			ogg_page_free(&pg);
			free(id_packet.p);
			free(tags_packet.p);
			return 0;
		}
		ogg_page_free(&pg);
		if (tags_pages > 1000) { // a comment packet this large is not a comment
			free(id_packet.p);
			free(tags_packet.p);
			return 0;
		}
	}
	// Validate the comment packet magic and rebuild it.
	size_t magic_len = opus ? 8 : 7;
	const char *magic = opus ? "OpusTags" : "\x03vorbis";
	if (tags_packet.n < magic_len || memcmp(tags_packet.p, magic, magic_len)) {
		free(id_packet.p);
		free(tags_packet.p);
		return 0;
	}
	buf new_block = {0};
	int ok;
	if (edit->image)
		ok = ogg_cover_comments(tags_packet.p + magic_len, tags_packet.n - magic_len, edit, &new_block);
	else
		ok = comments(tags_packet.p + magic_len, tags_packet.n - magic_len, &new_block, edit->field, edit->value);
	free(tags_packet.p);
	if (!ok) {
		free(id_packet.p);
		free(new_block.p);
		return 0;
	}
	buf new_tags = {0};
	ok = add(&new_tags, magic, magic_len) && add(&new_tags, new_block.p, new_block.n);
	free(new_block.p);
	if (!ok) {
		free(id_packet.p);
		free(new_tags.p);
		return 0;
	}
	// Repaginate: identification page, then comment pages.
	buf header = {0};
	ok = ogg_build_page(&header, serial, 0, 0x02, id_packet.p, id_packet.n, 1);
	free(id_packet.p);
	if (!ok) {
		free(new_tags.p);
		return 0;
	}
	size_t new_tags_pages = 0;
	{
		size_t off = 0;
		uint32_t seq = 1;
		while (off < new_tags.n || new_tags_pages == 0) {
			size_t left = new_tags.n - off;
			size_t segs_needed = left / 255 + 1;
			size_t segs_here = segs_needed > 255 ? 255 : segs_needed;
			size_t data_here = segs_here == segs_needed ? left : 255 * segs_here;
			uint8_t type = (new_tags_pages == 0) ? 0 : 0x01;
			if (!ogg_build_page(&header, serial, seq++, type, new_tags.p + off, data_here, 0)) {
				ok = 0;
				break;
			}
			off += data_here;
			new_tags_pages++;
			if (off >= new_tags.n)
				break;
		}
	}
	free(new_tags.p);
	if (!ok) {
		free(header.p);
		return 0;
	}
	size_t old_header_pages = 1 + tags_pages;
	int64_t delta = (int64_t)(1 + new_tags_pages) - (int64_t)old_header_pages;
	if (fwrite(header.p, 1, header.n, out) != header.n) {
		free(header.p);
		return -1;
	}
	free(header.p);
	// Copy the rest, renumbering this stream until its end marker.
	uint32_t expected = (uint32_t)old_header_pages;
	int after_eos = 0;
	for (;;) {
		r = ogg_read_page(in, &pg);
		if (r == 0)
			break;
		if (r < 0) {
			ogg_page_free(&pg);
			return 0;
		}
		if (!after_eos && pg.serial == serial) {
			if (pg.seq != expected) {
				ogg_page_free(&pg);
				return 0;
			}
			expected++;
			int64_t ns = (int64_t)pg.seq + delta;
			if (ns < 0 || ns > 0xffffffffLL) {
				ogg_page_free(&pg);
				return 0;
			}
			pg.data[18] = (unsigned char)ns;
			pg.data[19] = (unsigned char)(ns >> 8);
			pg.data[20] = (unsigned char)(ns >> 16);
			pg.data[21] = (unsigned char)(ns >> 24);
			if (!ogg_write_page(out, &pg)) {
				ogg_page_free(&pg);
				return -1;
			}
			if (pg.type & 0x04)
				after_eos = 1;
		} else {
			if (!ogg_write_page(out, &pg)) {
				ogg_page_free(&pg);
				return -1;
			}
		}
		ogg_page_free(&pg);
	}
	return 1;
}

// FLAC/MP3 writers: build the new header, then copy the audio verbatim.
// 1 ok, 0 unsupported, -1 io error.
static int flac_write(FILE *in, FILE *out, const edit_t *edit) {
	buf header = {0};
	if (!flac(in, &header, edit)) {
		free(header.p);
		return 0;
	}
	int ok = fwrite(header.p, 1, header.n, out) == header.n;
	free(header.p);
	unsigned char chunk[32768];
	size_t n;
	while (ok && (n = fread(chunk, 1, sizeof(chunk), in)) > 0)
		ok = fwrite(chunk, 1, n, out) == n;
	if (ferror(in))
		ok = 0;
	return ok ? 1 : -1;
}
static int mp3_write(FILE *in, FILE *out, const edit_t *edit) {
	buf header = {0};
	if (!mp3(in, &header, edit)) {
		free(header.p);
		return 0;
	}
	int ok = fwrite(header.p, 1, header.n, out) == header.n;
	free(header.p);
	unsigned char chunk[32768];
	size_t n;
	while (ok && (n = fread(chunk, 1, sizeof(chunk), in)) > 0)
		ok = fwrite(chunk, 1, n, out) == n;
	if (ferror(in))
		ok = 0;
	return ok ? 1 : -1;
}
static tagwrite_result_t write_edit(const char *path, const edit_t *edit) {
	struct stat before, current;
	if (lstat(path, &before) || !S_ISREG(before.st_mode) || before.st_nlink != 1 || !(before.st_mode & 0222) || access(path, W_OK) || before.st_uid != geteuid())
		return TAGWRITE_IO;
	FILE *in = fopen(path, "rb");
	if (!in)
		return TAGWRITE_IO;
	struct stat opened;
	if (fstat(fileno(in), &opened) || opened.st_dev != before.st_dev || opened.st_ino != before.st_ino) {
		fclose(in);
		return TAGWRITE_IO;
	}
	unsigned char magic[8];
	size_t got = fread(magic, 1, sizeof(magic), in);
	if (got < 4) {
		fclose(in);
		return TAGWRITE_UNSUPPORTED;
	}
	// Format is decided by content, not extension: a misnamed file still
	// goes to the writer that understands it, and an unknown one is refused.
	int (*writer)(FILE *, FILE *, const edit_t *) = NULL;
	if (!memcmp(magic, "fLaC", 4))
		writer = flac_write;
	else if (!memcmp(magic, "ID3", 3))
		writer = mp3_write; // MP3 and ID3-tagged AAC (ADTS) share this writer
	else if (!memcmp(magic, "OggS", 4))
		writer = ogg_write;
	else if (got >= 8 && !memcmp(magic + 4, "ftyp", 4))
		writer = mp4_write;
	if (!writer) {
		fclose(in);
		return TAGWRITE_UNSUPPORTED;
	}
	if (fseeko(in, 0, SEEK_SET)) {
		fclose(in);
		return TAGWRITE_IO;
	}
	size_t plen = strlen(path);
	char *tmp = malloc(plen + 20);
	if (!tmp) {
		fclose(in);
		return TAGWRITE_IO;
	}
	sprintf(tmp, "%s.sonix-XXXXXX", path);
	int fd = mkstemp(tmp);
	FILE *out = fd < 0 ? NULL : fdopen(fd, "wb");
	if (!out) {
		if (fd >= 0) {
			close(fd);
			unlink(tmp);
		}
		free(tmp);
		fclose(in);
		return TAGWRITE_IO;
	}
	int r = fchmod(fd, before.st_mode & 07777) == 0 ? writer(in, out, edit) : -1;
	int ok = r == 1;
	if (fflush(out) || fsync(fd))
		ok = 0;
	if (fclose(out))
		ok = 0;
	if (fstat(fileno(in), &current) || current.st_size != opened.st_size || (current.st_mtim.tv_sec != opened.st_mtim.tv_sec || current.st_mtim.tv_nsec != opened.st_mtim.tv_nsec || current.st_ctim.tv_sec != opened.st_ctim.tv_sec || current.st_ctim.tv_nsec != opened.st_ctim.tv_nsec) || lstat(path, &current) || current.st_dev != opened.st_dev || current.st_ino != opened.st_ino || current.st_size != opened.st_size || (current.st_mtim.tv_sec != opened.st_mtim.tv_sec || current.st_mtim.tv_nsec != opened.st_mtim.tv_nsec || current.st_ctim.tv_sec != opened.st_ctim.tv_sec || current.st_ctim.tv_nsec != opened.st_ctim.tv_nsec))
		ok = 0;
	fclose(in);
	if (ok && rename(tmp, path))
		ok = 0;
	if (!ok)
		unlink(tmp);
	free(tmp);
	if (ok)
		return TAGWRITE_OK;
	return r == 0 ? TAGWRITE_UNSUPPORTED : TAGWRITE_IO;
}

tagwrite_result_t tagwrite_set(const char *path, tagwrite_field_t field, const char *value) {
	if (!path || !valid_value(field, value))
		return TAGWRITE_INVALID;
	// Validate UTF-8 even for FLAC and ID3v2.4 before writing it.
	buf check = {0};
	int valid = utf16(value, &check);
	free(check.p);
	if (!valid)
		return TAGWRITE_INVALID;
	edit_t edit = {.field = field, .value = value};
	return write_edit(path, &edit);
}
tagwrite_result_t tagwrite_cover(const char *path, const uint8_t *jpeg, size_t size, int width, int height) {
	if (!path || !jpeg || size < 4 || size > COVER_LIMIT || jpeg[0] != 0xff || jpeg[1] != 0xd8 || jpeg[size - 2] != 0xff || jpeg[size - 1] != 0xd9 || width <= 0 || height <= 0 || width > 4096 || height > 4096)
		return TAGWRITE_INVALID;
	edit_t edit = {.image = jpeg, .image_size = size, .width = width, .height = height};
	return write_edit(path, &edit);
}
