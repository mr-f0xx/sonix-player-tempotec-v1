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

// A pitched WAV can use two-point or four-point Catmull-Rom interpolation.
// Linear is the legacy 8-bit-fractional interpolator; cubic uses four source
// frames and a Q16 fraction, with the outer frames clamped at trim boundaries.
typedef enum { TAPE_INTERP_LINEAR, TAPE_INTERP_CUBIC } tape_interp_t;
#define TAPE_PITCH_UNITY 65536u // Q16 playback-rate multiplier
#define TAPE_PITCH_MIN 16384u   // -24 semitones
#define TAPE_PITCH_MAX 262144u  // +24 semitones

// Equal-tempered semitone ratio in Q16. Values outside -24..+24 are clamped.
uint32_t tape_pitch_q16(int semitones);
// Four-point interpolation helper; p1 is at frac, p2 at frac + 1. `frac` is Q16.
int32_t tape_interpolate(int32_t p0, int32_t p1, int32_t p2, int32_t p3,
	uint16_t frac, tape_interp_t mode);

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

// A pitched/trimmed voice needs one frame before and two frames after the
// current position, so its window has a one-frame lead and a two-frame tail.
typedef struct {
	uint32_t at, count;
	uint8_t data[(TAPE_WAV_CACHE_FRAMES + 3) * 8];
} tape_wav_resample_cache_t;

// `pos` is an absolute Q16 source-frame position; `end` is an exclusive frame
// marker (zero means the whole file). The output stays inside [0, end) even
// when cubic interpolation would otherwise look past the trim marker.
bool tape_wav_resample(int fd, const tape_wav_t *wav, tape_wav_resample_cache_t *cache,
	uint64_t pos, uint32_t end, tape_interp_t mode, int *left, int *right);

#endif
