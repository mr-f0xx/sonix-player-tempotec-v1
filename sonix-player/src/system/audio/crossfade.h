#ifndef CROSSFADE_H
#define CROSSFADE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Equal-power crossfade curves and rate/channel conversion helpers for
// overlapping automatic track transitions.
//
// Gains are returned in Q15 (0..32768), with outgoing^2 + incoming^2 ~= 1.0
// (cos/sin quarter-wave curve) so uncorrelated tracks keep constant perceived
// loudness across the overlap instead of dipping by 3 dB in the middle.

void crossfade_gains_q15(double t_norm, int *out_gain_q15, int *in_gain_q15);

// Mixes `frames` interleaved frames of `incoming` into `outgoing` in place,
// stepping smoothly from `t0_norm` to `t1_norm` across the block and clamping
// to full scale.
void crossfade_mix_s16(int16_t *outgoing, const int16_t *incoming, int frames, int channels,
					   double t0_norm, double t1_norm);

void crossfade_mix_s32(int32_t *outgoing, const int32_t *incoming, int frames, int channels,
					   double t0_norm, double t1_norm);

// Stateful linear resampler + mono/stereo channel mapper for an incoming track
// whose sample rate or channel count differs from the currently open ALSA PCM
// during the crossfade overlap.
typedef struct {
	int src_rate;
	int src_channels;
	int dst_rate;
	int dst_channels;
	uint64_t step_fp; // Q32 source frames per destination frame
	uint64_t pos_fp;  // Q32 fractional offset relative to `prev` (0..2^32-1 once primed)
	bool primed;
	bool have_next;
	int32_t prev[2];
	int32_t next[2];
} crossfade_resampler_t;

typedef int (*crossfade_pull_s16_fn)(void *user, int16_t *dst, int max_frames);
typedef int (*crossfade_pull_s32_fn)(void *user, int32_t *dst, int max_frames);

void crossfade_resampler_init(crossfade_resampler_t *r, int src_rate, int src_channels,
							  int dst_rate, int dst_channels);

// Produces up to `dst_frames` converted frames into `dst` (interleaved with
// `r->dst_channels`). Writes the number of source frames consumed from `pull`
// into `*src_consumed_out` (may be NULL). Returns the number of destination
// frames written (< dst_frames only at source EOF).
int crossfade_resample_s16(crossfade_resampler_t *r, int16_t *dst, int dst_frames,
						   crossfade_pull_s16_fn pull, void *user, uint64_t *src_consumed_out);

int crossfade_resample_s32(crossfade_resampler_t *r, int32_t *dst, int dst_frames,
						   crossfade_pull_s32_fn pull, void *user, uint64_t *src_consumed_out);

#endif // CROSSFADE_H
