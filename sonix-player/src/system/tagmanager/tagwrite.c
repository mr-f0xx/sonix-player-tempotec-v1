#define _POSIX_C_SOURCE 200809L
#include "tagwrite.h"
#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

// Bound untrusted metadata allocations on a memory-constrained player.
#define LIMIT (16u * 1024u * 1024u)
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
	if (!v || !*v || strlen(v) > 127 || strchr(v, '\n') || strchr(v, '\r'))
		return 0;
	if (field == TAGWRITE_GENRE)
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
// ID3v2.3 text encoding 0 is Latin-1; restrict input to ASCII rather than
// writing invalid UTF-8 into a Latin-1 frame. v2.4 accepts UTF-8.
static int mp3(FILE *in, buf *out, tagwrite_field_t field, const char *value) {
	unsigned char h[10];
	if (!readn(in, h, 10) || memcmp(h, "ID3", 3) || (h[3] != 3 && h[3] != 4) || h[4] || h[5] || !syncok(h + 6))
		return 0;
	if (h[3] == 3)
		for (const unsigned char *p = (const unsigned char *)value; *p; p++)
			if (*p >= 128)
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
	const char *id = field == TAGWRITE_TRACK ? "TRCK" : "TCON";
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
		if (!memcmp(f, id, 4)) {
			if (f[8] || f[9]) {
				ok = 0;
				break;
			}
		} else if (!add(&frames, f, 10 + n)) {
			ok = 0;
			break;
		}
		pos += 10 + n;
	}
	free(tag);
	// Do not leave a competing consecutive ID3 tag behind.
	unsigned char next[3];
	size_t peek = fread(next, 1, 3, in);
	if (ferror(in) || (peek == 3 && !memcmp(next, "ID3", 3)))
		ok = 0;
	if (fseeko(in, -(off_t)peek, SEEK_CUR))
		ok = 0;
	size_t n = strlen(value) + 1;
	unsigned char fh[10] = {0};
	memcpy(fh, id, 4);
	if (h[3] == 4)
		syncput(fh + 4, n);
	else
		putbe(fh + 4, n);
	unsigned char enc = h[3] == 4 ? 3 : 0;
	if (ok)
		ok = add(&frames, fh, 10) && add(&frames, &enc, 1) && add(&frames, value, n - 1);
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
	const char *key = field == TAGWRITE_TRACK ? "TRACKNUMBER" : "GENRE";
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
static int flac(FILE *in, buf *out, tagwrite_field_t field, const char *v) {
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
		if (type == 4) {
			buf b = {0};
			if (seen++)
				ok = 0;
			else
				ok = comments(p, n, &b, field, v);
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
			ok = comments(base, 8, &b, field, v);
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
	return seen;
}
tagwrite_result_t tagwrite_set(const char *path, tagwrite_field_t field, const char *value) {
	if (!path || !valid_value(field, value))
		return TAGWRITE_INVALID;
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
	int ok = got && (!memcmp(magic, "fLaC", 4) ? flac(in, &header, field, value) : !memcmp(magic, "ID3", 3) ? mp3(in, &header, field, value) : 0);
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
