#ifndef TAPEWAV_H
#define TAPEWAV_H

#include <stdbool.h>
#include <stdint.h>
#include <sys/types.h>

#define TAPE_PATH_SIZE 512
#define TAPE_FILE_NAME_SIZE 256
#define TAPE_KIT_DIR "Drum Kits"
#define TAPE_SHORT_CLIP_SECONDS 8
#define TAPE_WAV_CACHE_FRAMES 2048

typedef struct {
	uint32_t frames, rate;
	unsigned channels, bytes_per_sample;
	off_t offset;
} tape_wav_t;

typedef struct {
	char name[TAPE_FILE_NAME_SIZE];
	bool directory;
} tape_file_t;

// All paths are relative to an already reserved card. The trusted root may
// be a firmware alias, but neither sample files nor folders may be symlinks.
int tape_card_begin(bool writable);
void tape_card_end(void);
int tape_open_at(int root, const char *path, bool directory);
bool tape_wav_name(const char *name);

// Bounded, sorted page (folders first) from a card directory. `after` is the
// last entry on the previous page; missing directories return an empty page.
int tape_card_list(const char *dir, bool folders_only, const tape_file_t *after, tape_file_t *out, int cap);

// PCM WAV (including extensible PCM), mono/stereo, 8/16/24/32-bit, 8–96 kHz.
// Caller owns the fd. Probing does not read or allocate the PCM payload.
bool tape_wav_probe(int fd, tape_wav_t *out);
bool tape_wav_is_long(const tape_wav_t *wav);

typedef struct {
	uint32_t at, count;
	uint8_t data[(TAPE_WAV_CACHE_FRAMES + 1) * 8];
} tape_wav_cache_t;

// Interpolate to the mixer's sample position (Q16 in source frames). Each
// cache is reused on retrigger and includes the next frame at block edges.
bool tape_wav_sample(int fd, const tape_wav_t *wav, tape_wav_cache_t *cache, uint64_t pos, int *left, int *right);

#endif
