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
	unsigned char next[4];
	size_t peek = fread(next, 1, sizeof(next), in);
	if (ferror(in) || (peek >= 3 && !memcmp(next, "ID3", 3)) || (peek == 4 && (!memcmp(next, "fLaC", 4) || !memcmp(next, "RIFF", 4) || !memcmp(next, "FORM", 4) || !memcmp(next, "OggS", 4))))
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
	unsigned char magic[4];
	int got = readn(in, magic, 4);
	rewind(in);
	buf header = {0};
	int ok = got && (!memcmp(magic, "fLaC", 4) ? flac(in, &header, edit) : !memcmp(magic, "ID3", 3) ? mp3(in, &header, edit) : 0);
	if (!ok) {
		free(header.p);
		fclose(in);
		return TAGWRITE_UNSUPPORTED;
	}
	size_t plen = strlen(path);
	char *tmp = malloc(plen + 20);
	if (!tmp) {
		free(header.p);
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
		free(header.p);
		fclose(in);
		return TAGWRITE_IO;
	}
	ok = fchmod(fd, before.st_mode & 07777) == 0 && fwrite(header.p, 1, header.n, out) == header.n;
	free(header.p);
	unsigned char chunk[32768];
	size_t n;
	while (ok && (n = fread(chunk, 1, sizeof(chunk), in)) > 0)
		ok = fwrite(chunk, 1, n, out) == n;
	if (ferror(in))
		ok = 0;
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
	return ok ? TAGWRITE_OK : TAGWRITE_IO;
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
