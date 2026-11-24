#include "tapewav.h"

#include "src/system/device/system.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#if defined(HOST_BUILD) && !defined(__EMSCRIPTEN__)
#include <sys/statvfs.h>
#endif
#include <unistd.h>

int tape_card_begin(bool writable) {
#ifdef HOST_BUILD
	// The emulator owns an ordinary virtual microSD folder and never exports
	// it as a block device. Keep sampler writes in that folder too.
	const char *root = storage_sd_root();
	int fd = root && *root ? open(root, O_RDONLY | O_DIRECTORY | O_CLOEXEC) : -1;
#ifndef __EMSCRIPTEN__
	if (fd >= 0 && writable) {
		struct statvfs fs;
		if (fstatvfs(fd, &fs) != 0 || (fs.f_flag & ST_RDONLY)) { close(fd); return -1; }
	}
#else
	(void)writable; // MEMFS is writable; the individual open/write still checks
#endif
	return fd;
#else
	return storage_card_file_job_begin(writable);
#endif
}

void tape_card_end(void) {
#ifndef HOST_BUILD
	storage_card_file_job_end();
#endif
}

bool tape_wav_name(const char *name) {
	size_t n = strlen(name);
	return n > 4 && strcasecmp(name + n - 4, ".wav") == 0;
}

int tape_open_at(int root, const char *path, bool directory) {
	if (root < 0 || !path || strnlen(path, TAPE_PATH_SIZE) >= TAPE_PATH_SIZE || (!*path && !directory)) {
		errno = EINVAL;
		return -1;
	}
	if (!*path) return openat(root, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	int fd = -1;
	while (*path) {
		char component[TAPE_FILE_NAME_SIZE];
		const char *slash = strchr(path, '/');
		size_t len = slash ? (size_t)(slash - path) : strlen(path);
		if (!len || len >= sizeof(component) || (slash && !slash[1]) ||
			(len == 1 && path[0] == '.') || (len == 2 && path[0] == '.' && path[1] == '.')) {
			if (fd >= 0) close(fd);
			errno = EINVAL;
			return -1;
		}
		memcpy(component, path, len);
		component[len] = '\0';
		int flags = O_RDONLY | O_NOFOLLOW | O_CLOEXEC | ((slash || directory) ? O_DIRECTORY : O_NONBLOCK);
		int next = openat(fd >= 0 ? fd : root, component, flags);
		if (fd >= 0) close(fd);
		if (next < 0) return -1;
		fd = next;
		if (!slash) break;
		path = slash + 1;
	}
	if (!directory) {
		struct stat st;
		if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
			close(fd);
			errno = EINVAL;
			return -1;
		}
	}
	return fd;
}

static int entry_cmp(const tape_file_t *a, const tape_file_t *b) {
	if (a->directory != b->directory) return a->directory ? -1 : 1;
	int diff = strcasecmp(a->name, b->name);
	return diff ? diff : strcmp(a->name, b->name);
}

int tape_card_list(const char *path, bool folders_only, const tape_file_t *after, tape_file_t *out, int cap) {
	if (!out || cap <= 0) return 0;
	int card = tape_card_begin(false);
	if (card < 0) return -1;
	int fd = tape_open_at(card, path, true);
	int total = fd < 0 && errno != ENOENT ? -1 : 0;
	DIR *dir = fd >= 0 ? fdopendir(fd) : NULL;
	if (dir) {
		for (;;) {
			errno = 0;
			struct dirent *d = readdir(dir);
			if (!d) { if (errno) total = -1; break; }
			if (d->d_name[0] == '.') continue;
			// Most files in a music library aren't WAVs. Skip known non-dirs
			// without another metadata read, but verify even when d_type is known.
			bool wav = !folders_only && tape_wav_name(d->d_name);
			if (!wav && d->d_type != DT_DIR && d->d_type != DT_UNKNOWN) continue;
			struct stat st;
			if (fstatat(fd, d->d_name, &st, AT_SYMLINK_NOFOLLOW) != 0) continue;
			bool folder = S_ISDIR(st.st_mode);
			if (!folder && !(wav && S_ISREG(st.st_mode))) continue;
			tape_file_t row = {.directory = folder};
			size_t len = strlen(d->d_name);
			if (len >= sizeof(row.name)) continue;
			memcpy(row.name, d->d_name, len + 1);
			if (after && entry_cmp(&row, after) <= 0) continue;
			int i = 0;
			while (i < total && entry_cmp(&out[i], &row) < 0) i++;
			if (i == cap) continue;
			if (total < cap) total++;
			memmove(out + i + 1, out + i, (size_t)(total - i - 1) * sizeof(*out));
			out[i] = row;
		}
		closedir(dir);
	} else if (fd >= 0) {
		close(fd);
		total = -1;
	}
	close(card);
	tape_card_end();
	return total;
}

static bool read_at(int fd, void *buffer, size_t size, off_t offset) {
	uint8_t *p = buffer;
	while (size > 0) {
		ssize_t n = pread(fd, p, size, offset);
		if (n < 0 && errno == EINTR) continue;
		if (n <= 0) return false;
		p += n;
		offset += n;
		size -= (size_t)n;
	}
	return true;
}

static unsigned le16(const uint8_t *p) { return (unsigned)p[0] | (unsigned)p[1] << 8; }
static uint32_t le32(const uint8_t *p) { return (uint32_t)le16(p) | (uint32_t)le16(p + 2) << 16; }

bool tape_wav_probe(int fd, tape_wav_t *out) {
	struct stat st;
	uint8_t head[12], chunk[8], fmt[40];
	if (!out || fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size < 44 ||
		!read_at(fd, head, sizeof(head), 0) || memcmp(head, "RIFF", 4) || memcmp(head + 8, "WAVE", 4)) return false;
	uint64_t end = 8u + (uint64_t)le32(head + 4);
	if (end > (uint64_t)st.st_size || end < sizeof(head)) return false;
	tape_wav_t wav = {0};
	unsigned align = 0;
	uint32_t bytes = 0;
	for (uint64_t at = sizeof(head), n = 0; at + sizeof(chunk) <= end && n < 256; n++) {
		if (!read_at(fd, chunk, sizeof(chunk), (off_t)at)) return false;
		uint32_t len = le32(chunk + 4);
		uint64_t payload = at + sizeof(chunk);
		at = payload + len + (len & 1u); // word aligned, including unknown chunks
		if (at > end) return false;
		if (!align && !memcmp(chunk, "fmt ", 4)) {
			size_t read = len > sizeof(fmt) ? sizeof(fmt) : len;
			if (read < 16 || !read_at(fd, fmt, read, (off_t)payload)) return false;
			unsigned format = le16(fmt), bits = le16(fmt + 14);
			static const uint8_t pcm_guid[16] = {1, 0, 0, 0, 0, 0, 0x10, 0, 0x80, 0, 0, 0xaa, 0, 0x38, 0x9b, 0x71};
			if (format != 1 && (format != 0xfffe || read < 40 || le16(fmt + 16) < 22 ||
				!le16(fmt + 18) || le16(fmt + 18) > bits || memcmp(fmt + 24, pcm_guid, sizeof(pcm_guid)))) return false;
			wav.rate = le32(fmt + 4);
			wav.channels = le16(fmt + 2);
			wav.bytes_per_sample = bits / 8;
			align = le16(fmt + 12);
			if ((wav.channels != 1 && wav.channels != 2) || wav.rate < 8000 || wav.rate > 96000 ||
				(bits != 8 && bits != 16 && bits != 24 && bits != 32) || align != wav.channels * wav.bytes_per_sample) return false;
		} else if (!wav.offset && !memcmp(chunk, "data", 4)) {
			wav.offset = (off_t)payload;
			bytes = len;
		}
		if (align && wav.offset) {
			if (bytes < align || bytes % align) return false;
			wav.frames = bytes / align;
			*out = wav;
			return true;
		}
	}
	return false;
}

bool tape_wav_is_long(const tape_wav_t *wav) { return wav->frames > wav->rate * TAPE_SHORT_CLIP_SECONDS; }

static int sample_s16(const uint8_t *p, unsigned bytes) {
	if (bytes == 1) return ((int)*p - 128) * 256; // 8-bit PCM is unsigned
	return (int)(le16(p + bytes - 2) ^ 32768u) - 32768;
}

static int interpolation(const uint8_t *a, const uint8_t *b, unsigned bytes, unsigned frac) {
	int first = sample_s16(a, bytes), second = sample_s16(b, bytes);
	return first + (second - first) * (int)frac / 256;
}

bool tape_wav_sample(int fd, const tape_wav_t *wav, tape_wav_cache_t *cache, uint64_t pos, int *left, int *right) {
	uint64_t frame = pos >> 16;
	if (frame >= wav->frames) return false;
	unsigned stride = wav->channels * wav->bytes_per_sample;
	uint32_t next = frame + 1 < wav->frames ? (uint32_t)frame + 1 : (uint32_t)frame;
	if (frame < cache->at || next - cache->at >= cache->count) {
		uint32_t at = (uint32_t)frame / TAPE_WAV_CACHE_FRAMES * TAPE_WAV_CACHE_FRAMES;
		uint32_t count = wav->frames - at;
		if (count > TAPE_WAV_CACHE_FRAMES + 1) count = TAPE_WAV_CACHE_FRAMES + 1;
		if (!read_at(fd, cache->data, (size_t)count * stride, wav->offset + (off_t)at * stride)) {
			cache->count = 0;
			return false;
		}
		cache->at = at;
		cache->count = count;
	}
	const uint8_t *a = cache->data + (frame - cache->at) * stride;
	const uint8_t *b = cache->data + (next - cache->at) * stride;
	unsigned frac = (unsigned)(pos >> 8) & 255, bytes = wav->bytes_per_sample;
	*left = interpolation(a, b, bytes, frac);
	*right = wav->channels == 2 ? interpolation(a + bytes, b + bytes, bytes, frac) : *left;
	return true;
}

// 2^(n/12) in Q16, for n = -24..+24. A table keeps the audio worker and the
// kit/sequence loaders free of floating point and gives exact unison, octave
// and double-octave ratios.
static const uint32_t pitch_q16[49] = {
	16384u, 17358u, 18390u, 19484u, 20643u, 21870u, 23170u, 24548u,
	26008u, 27554u, 29193u, 30929u, 32768u, 34716u, 36781u, 38968u,
	41285u, 43740u, 46341u, 49097u, 52016u, 55109u, 58386u, 61858u,
	65536u, 69433u, 73562u, 77936u, 82570u, 87480u, 92682u, 98193u,
	104032u, 110218u, 116772u, 123715u, 131072u, 138866u, 147123u, 155872u,
	165140u, 174960u, 185364u, 196386u, 208064u, 220436u, 233544u, 247431u,
	262144u,
};

uint32_t tape_pitch_q16(int semitones) {
	if (semitones < -24) semitones = -24;
	if (semitones > 24) semitones = 24;
	return pitch_q16[semitones + 24];
}

// Catmull-Rom coefficients: 1/2 * (c0 + c1*t + c2*t² + c3*t³). All products
// are staged in Q16 so the largest intermediate stays well inside int64_t.
int32_t tape_interpolate(int32_t p0, int32_t p1, int32_t p2, int32_t p3,
	uint16_t frac, tape_interp_t mode) {
	if (mode == TAPE_INTERP_LINEAR) {
		// The legacy sampler quantizes the Q16 position to 8 fractional bits.
		return p1 + (p2 - p1) * (int32_t)(frac >> 8) / 256;
	}
	int64_t t = frac;
	int64_t t2 = t * t >> 16;
	int64_t t3 = t2 * t >> 16;
	int64_t c0 = 2 * (int64_t)p1;
	int64_t c1 = -(int64_t)p0 + p2;
	int64_t c2 = 2 * (int64_t)p0 - 5 * (int64_t)p1 + 4 * (int64_t)p2 - p3;
	int64_t c3 = -(int64_t)p0 + 3 * (int64_t)p1 - 3 * (int64_t)p2 + p3;
	int64_t sum = c0 * 65536 + c1 * t + c2 * t2 + c3 * t3;
	int64_t value = sum / (2 * 65536); // sum is Q16 of twice the sample
	if (value < -32768) value = -32768; // cubic overshoot at sharp transients
	if (value > 32767) value = 32767;
	return (int32_t)value;
}

static void decoded_frame(const uint8_t *p, unsigned bytes, unsigned channels, int *left, int *right) {
	*left = sample_s16(p, bytes);
	*right = channels == 2 ? sample_s16(p + bytes, bytes) : *left;
}

bool tape_wav_resample(int fd, const tape_wav_t *wav, tape_wav_resample_cache_t *cache,
	uint64_t pos, uint32_t end, tape_interp_t mode, int *left, int *right) {
	if (!wav || !cache || !left || !right || !wav->frames) return false;
	if (!end || end > wav->frames) end = wav->frames;
	uint64_t frame64 = pos >> 16;
	if (frame64 >= end) return false;
	uint32_t frame = (uint32_t)frame64, last = end - 1;
	uint32_t next = frame < UINT32_MAX ? frame + 1 : frame;
	uint32_t after = frame < UINT32_MAX - 1 ? frame + 2 : frame;
	uint32_t frames[4] = {frame ? frame - 1 : 0, frame, next, after};
	if (frames[2] > last) frames[2] = last;
	if (frames[3] > last) frames[3] = last;
	unsigned stride = wav->channels * wav->bytes_per_sample;
	if (frames[0] < cache->at || frames[3] >= cache->at + cache->count) {
		uint32_t block = frame / TAPE_WAV_CACHE_FRAMES * TAPE_WAV_CACHE_FRAMES;
		uint32_t at = block ? block - 1 : 0;
		uint32_t count = end - at;
		if (count > TAPE_WAV_CACHE_FRAMES + 3) count = TAPE_WAV_CACHE_FRAMES + 3;
		if (!read_at(fd, cache->data, (size_t)count * stride,
			wav->offset + (off_t)at * stride)) {
			cache->count = 0;
			return false;
		}
		cache->at = at;
		cache->count = count;
	}
	int l[4], r[4];
	for (unsigned i = 0; i < 4; i++) {
		uint32_t at = frames[i];
		if (at < cache->at || at >= cache->at + cache->count) {
			cache->count = 0;
			return false;
		}
		decoded_frame(cache->data + (at - cache->at) * stride,
			wav->bytes_per_sample, wav->channels, &l[i], &r[i]);
	}
	uint16_t frac = (uint16_t)(pos & 0xffffu);
	*left = tape_interpolate(l[0], l[1], l[2], l[3], frac, mode);
	*right = wav->channels == 2 ? tape_interpolate(r[0], r[1], r[2], r[3], frac, mode) : *left;
	return true;
}
