#include "tapestore.h"
#include "tapewav.h"

#include "src/system/device/system.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#ifndef __EMSCRIPTEN__
#include <sys/syscall.h>
#endif
#include <time.h>
#include <unistd.h>

#define TIMESTAMP_LEN 19 // YYYYMMDD-HHMMSS-NNN
#define WAV_HEADER_SIZE 44

static const struct {
	const char *dir, *prefix, *suffix;
	off_t min_size;
} formats[] = {
	[TAPE_STORE_SEQUENCE] = {"Sequences", "Sequence-", ".seq", 12},
	[TAPE_STORE_TAKE] = {"Recordings", "Take-", ".wav", WAV_HEADER_SIZE + 4},
};

static bool valid_name(tape_store_kind_t kind, const char *name) {
	if (!name) return false;
	size_t prefix = strlen(formats[kind].prefix);
	if (strlen(name) != prefix + TIMESTAMP_LEN + strlen(formats[kind].suffix) ||
		memcmp(name, formats[kind].prefix, prefix) != 0) return false;
	const char *stamp = name + prefix;
	return strspn(stamp, "0123456789") == 8 && stamp[8] == '-' &&
		strspn(stamp + 9, "0123456789") == 6 && stamp[15] == '-' &&
		strspn(stamp + 16, "0123456789") == 3 && strcmp(stamp + TIMESTAMP_LEN, formats[kind].suffix) == 0;
}

static int open_dir(int card, tape_store_kind_t kind, bool create) {
	const char *dir = formats[kind].dir;
	if (create && mkdirat(card, dir, 0755) != 0 && errno != EEXIST) return -1;
	return tape_open_at(card, dir, true);
}

static bool name_free(int dir, const char *name) {
	struct stat st;
	return fstatat(dir, name, &st, AT_SYMLINK_NOFOLLOW) != 0 && errno == ENOENT;
}

bool tape_store_begin(int card, tape_store_kind_t kind, tape_savefile_t *out) {
	*out = (tape_savefile_t){.dir = -1, .fd = -1};
	char stamp[16];
	time_t now = time(NULL);
	struct tm tm;
	if (!localtime_r(&now, &tm) || strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", &tm) != sizeof(stamp) - 1) return false;
	int dir = open_dir(card, kind, true);
	if (dir < 0) return false;
	for (unsigned serial = 1; serial <= 999; serial++) {
		int n = snprintf(out->name, sizeof(out->name), "%s%s-%03u%s", formats[kind].prefix, stamp, serial, formats[kind].suffix);
		if (n < 0 || (size_t)n >= sizeof(out->name)) break;
		if (!name_free(dir, out->name)) continue;
		snprintf(out->part, sizeof(out->part), ".%s.part", out->name);
		int fd = openat(dir, out->part, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
		if (fd >= 0) {
			out->fd = fd;
			out->dir = dir;
			return true;
		}
		if (errno != EEXIST) break;
	}
	close(dir);
	return false;
}

static bool publish_part(const tape_savefile_t *file) {
#if defined(SYS_renameat2) && !defined(__EMSCRIPTEN__)
	if (syscall(SYS_renameat2, file->dir, file->part, file->dir, file->name, 1u /* RENAME_NOREPLACE */) == 0) return true;
	if (errno != ENOSYS && errno != EINVAL && errno != EOPNOTSUPP) return false;
#endif
#ifndef __EMSCRIPTEN__
	if (linkat(file->dir, file->part, file->dir, file->name, 0) == 0) {
		unlinkat(file->dir, file->part, 0);
		return true;
	}
	if (errno != EPERM && errno != ENOSYS && errno != EOPNOTSUPP) return false;
#endif
	// Older FAT/exFAT drivers may have neither operation. Deck writers
	// reserve each name with O_EXCL; also recheck for an existing final file.
	return name_free(file->dir, file->name) && renameat(file->dir, file->part, file->dir, file->name) == 0;
}

bool tape_store_end(tape_savefile_t *file, bool publish) {
	bool ok = publish && fsync(file->fd) == 0;
	if (close(file->fd) != 0) ok = false;
	ok = ok && publish_part(file);
	if (ok) (void)fsync(file->dir); // some card filesystems do not sync directories
	else unlinkat(file->dir, file->part, 0);
	close(file->dir);
	file->dir = file->fd = -1;
	return ok;
}

bool tape_store_write(int fd, const void *data, size_t size) {
	const uint8_t *p = data;
	while (size) {
		ssize_t n = write(fd, p, size);
		if (n < 0 && errno == EINTR) continue;
		if (n <= 0) return false;
		p += n;
		size -= (size_t)n;
	}
	return true;
}

int tape_store_open(int card, tape_store_kind_t kind, const char *name) {
	if (!valid_name(kind, name)) return -1;
	char path[16 + TAPE_SAVE_NAME_SIZE];
	snprintf(path, sizeof(path), "%s/%s", formats[kind].dir, name);
	int fd = tape_open_at(card, path, false);
	if (fd >= 0) {
		struct stat st;
		if (fstat(fd, &st) != 0 || st.st_size < formats[kind].min_size) { close(fd); return -1; }
	}
	return fd;
}

int tape_store_list(tape_store_kind_t kind, const char *after, char names[][TAPE_SAVE_NAME_SIZE], int cap) {
	if (!names || cap <= 0) return 0;
	int card = tape_card_begin(false);
	if (card < 0) return -1;
	int fd = open_dir(card, kind, false);
	int count = fd < 0 && errno != ENOENT ? -1 : 0;
	DIR *dir = fd >= 0 ? fdopendir(fd) : NULL;
	if (dir) {
		for (;;) {
			errno = 0;
			struct dirent *d = readdir(dir);
			if (!d) { if (errno) count = -1; break; }
			if (!valid_name(kind, d->d_name) || (after && strcmp(d->d_name, after) >= 0)) continue;
			int at = 0;
			while (at < count && strcmp(names[at], d->d_name) > 0) at++;
			if (at == cap) continue;
			struct stat st;
			if (fstatat(fd, d->d_name, &st, AT_SYMLINK_NOFOLLOW) != 0 || !S_ISREG(st.st_mode) ||
				st.st_size < formats[kind].min_size) continue;
			if (count < cap) count++;
			memmove(names + at + 1, names + at, (size_t)(count - at - 1) * sizeof(*names));
			strcpy(names[at], d->d_name); // exact bounded format checked above
		}
		closedir(dir);
	} else if (fd >= 0) { close(fd); count = -1; }
	close(card);
	tape_card_end();
	return count;
}

bool tape_store_take_path(const char *name, char *out, size_t size) {
	if (!out || !size) return false;
	int card = tape_card_begin(false);
	if (card < 0) return false;
	int fd = tape_store_open(card, TAPE_STORE_TAKE, name);
	const char *root = storage_sd_root();
	bool ok = false;
	tape_wav_t wav;
	if (fd >= 0 && root && *root && tape_wav_probe(fd, &wav)) {
		int n = snprintf(out, size, "%s/%s/%s", root, formats[TAPE_STORE_TAKE].dir, name);
		ok = n >= 0 && (size_t)n < size;
	}
	if (fd >= 0) close(fd);
	close(card);
	tape_card_end();
	return ok;
}

static void le32(uint8_t *out, uint32_t value) {
	for (int i = 0; i < 4; i++) out[i] = (uint8_t)(value >> (8 * i));
}

bool tape_store_wav_header(int fd, uint32_t rate, uint32_t frames) {
	if (frames > (UINT32_MAX - 36u) / 4u) return false;
	uint8_t header[WAV_HEADER_SIZE] = {
		'R','I','F','F',0,0,0,0,'W','A','V','E','f','m','t',' ',
		16,0,0,0,1,0,2,0,0,0,0,0,0,0,0,0,4,0,16,0,'d','a','t','a',0,0,0,0,
	};
	le32(header + 4, 36 + frames * 4);
	le32(header + 24, rate);
	le32(header + 28, rate * 4);
	le32(header + 40, frames * 4);
	return lseek(fd, 0, SEEK_SET) == 0 && tape_store_write(fd, header, sizeof(header));
}
