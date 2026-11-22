#include "crossfade.h"

#include <limits.h>
#include <math.h>
#include <string.h>

#ifndef M_PI_2
#define M_PI_2 1.57079632679489661923
#endif

// 257-entry quarter-wave cosine/sine table in Q15 (0..32768), computed lazily
// once and interpolated linearly per block/sub-block so the inner sample loop
// uses pure integer arithmetic.
#define CURVE_STEPS 256
static int16_t cos_q15_tab[CURVE_STEPS + 1];
static int16_t sin_q15_tab[CURVE_STEPS + 1];
static volatile int curve_ready;

static void ensure_curve_table(void) {
	if (__atomic_load_n(&curve_ready, __ATOMIC_ACQUIRE)) {
		return;
	}
	for (int i = 0; i <= CURVE_STEPS; i++) {
		double angle = ((double)i / (double)CURVE_STEPS) * M_PI_2;
		int c = (int)(cos(angle) * 32768.0 + 0.5);
		int s = (int)(sin(angle) * 32768.0 + 0.5);
		if (c > 32767) {
			c = 32767;
		}
		if (s > 32767) {
			s = 32767;
		}
		cos_q15_tab[i] = (int16_t)c;
		sin_q15_tab[i] = (int16_t)s;
	}
	__atomic_store_n(&curve_ready, 1, __ATOMIC_RELEASE);
}

void crossfade_gains_q15(double t_norm, int *out_gain_q15, int *in_gain_q15) {
	ensure_curve_table();
	if (t_norm <= 0.0) {
		if (out_gain_q15) {
			*out_gain_q15 = 32768;
		}
		if (in_gain_q15) {
			*in_gain_q15 = 0;
		}
		return;
	}
	if (t_norm >= 1.0) {
		if (out_gain_q15) {
			*out_gain_q15 = 0;
		}
		if (in_gain_q15) {
			*in_gain_q15 = 32768;
		}
		return;
	}

	double scaled = t_norm * (double)CURVE_STEPS;
	int idx = (int)scaled;
	if (idx < 0) {
		idx = 0;
	} else if (idx >= CURVE_STEPS) {
		idx = CURVE_STEPS - 1;
	}
	int frac_q10 = (int)((scaled - (double)idx) * 1024.0 + 0.5);
	if (frac_q10 < 0) {
		frac_q10 = 0;
	} else if (frac_q10 > 1024) {
		frac_q10 = 1024;
	}

	int c0 = (idx == 0) ? 32768 : (int)cos_q15_tab[idx];
	int c1 = (idx + 1 == CURVE_STEPS) ? 0 : (int)cos_q15_tab[idx + 1];
	int s0 = (idx == 0) ? 0 : (int)sin_q15_tab[idx];
	int s1 = (idx + 1 == CURVE_STEPS) ? 32768 : (int)sin_q15_tab[idx + 1];

	int g_out = c0 + (((c1 - c0) * frac_q10) >> 10);
	int g_in = s0 + (((s1 - s0) * frac_q10) >> 10);

	if (out_gain_q15) {
		*out_gain_q15 = g_out;
	}
	if (in_gain_q15) {
		*in_gain_q15 = g_in;
	}
}

// Sub-block size for stepping the equal-power curve smoothly without calling
// floating-point math on every sample (~1.4 ms at 44.1 kHz).
#define MIX_SLICE_FRAMES 64

void crossfade_mix_s16(int16_t *outgoing, const int16_t *incoming, int frames, int channels,
					   double t0_norm, double t1_norm) {
	if (!outgoing || !incoming || frames <= 0 || channels <= 0) {
		return;
	}
	ensure_curve_table();

	int pos = 0;
	while (pos < frames) {
		int slice = frames - pos;
		if (slice > MIX_SLICE_FRAMES) {
			slice = MIX_SLICE_FRAMES;
		}
		double f0 = (double)pos / (double)frames;
		double f1 = (double)(pos + slice) / (double)frames;
		int g_out0, g_in0, g_out1, g_in1;
		crossfade_gains_q15(t0_norm + (t1_norm - t0_norm) * f0, &g_out0, &g_in0);
		crossfade_gains_q15(t0_norm + (t1_norm - t0_norm) * f1, &g_out1, &g_in1);

		// Q23 fixed-point linear ramp inside the slice for click-free smoothness.
		int32_t out_q23 = g_out0 << 8;
		int32_t in_q23 = g_in0 << 8;
		int32_t out_step = ((g_out1 - g_out0) << 8) / slice;
		int32_t in_step = ((g_in1 - g_in0) << 8) / slice;

		int16_t *dst = outgoing + (size_t)pos * (size_t)channels;
		const int16_t *src = incoming + (size_t)pos * (size_t)channels;
		if (channels == 2) {
			for (int i = 0; i < slice; i++) {
				int go = out_q23 >> 8;
				int gi = in_q23 >> 8;
				int l = (((int)dst[2 * i] * go) + ((int)src[2 * i] * gi) + 16384) >> 15;
				int r = (((int)dst[2 * i + 1] * go) + ((int)src[2 * i + 1] * gi) + 16384) >> 15;
				if (l > 32767) {
					l = 32767;
				} else if (l < -32768) {
					l = -32768;
				}
				if (r > 32767) {
					r = 32767;
				} else if (r < -32768) {
					r = -32768;
				}
				dst[2 * i] = (int16_t)l;
				dst[2 * i + 1] = (int16_t)r;
				out_q23 += out_step;
				in_q23 += in_step;
			}
		} else {
			for (int i = 0; i < slice; i++) {
				int go = out_q23 >> 8;
				int gi = in_q23 >> 8;
				for (int c = 0; c < channels; c++) {
					int v = (((int)dst[i * channels + c] * go) + ((int)src[i * channels + c] * gi) + 16384) >> 15;
					if (v > 32767) {
						v = 32767;
					} else if (v < -32768) {
						v = -32768;
					}
					dst[i * channels + c] = (int16_t)v;
				}
				out_q23 += out_step;
				in_q23 += in_step;
			}
		}
		pos += slice;
	}
}

void crossfade_mix_s32(int32_t *outgoing, const int32_t *incoming, int frames, int channels,
					   double t0_norm, double t1_norm) {
	if (!outgoing || !incoming || frames <= 0 || channels <= 0) {
		return;
	}
	ensure_curve_table();

	int pos = 0;
	while (pos < frames) {
		int slice = frames - pos;
		if (slice > MIX_SLICE_FRAMES) {
			slice = MIX_SLICE_FRAMES;
		}
		double f0 = (double)pos / (double)frames;
		double f1 = (double)(pos + slice) / (double)frames;
		int g_out0, g_in0, g_out1, g_in1;
		crossfade_gains_q15(t0_norm + (t1_norm - t0_norm) * f0, &g_out0, &g_in0);
		crossfade_gains_q15(t0_norm + (t1_norm - t0_norm) * f1, &g_out1, &g_in1);

		int32_t out_q23 = g_out0 << 8;
		int32_t in_q23 = g_in0 << 8;
		int32_t out_step = ((g_out1 - g_out0) << 8) / slice;
		int32_t in_step = ((g_in1 - g_in0) << 8) / slice;

		int32_t *dst = outgoing + (size_t)pos * (size_t)channels;
		const int32_t *src = incoming + (size_t)pos * (size_t)channels;
		for (int i = 0; i < slice; i++) {
			int64_t go = out_q23 >> 8;
			int64_t gi = in_q23 >> 8;
			for (int c = 0; c < channels; c++) {
				int64_t v = ((int64_t)dst[i * channels + c] * go + (int64_t)src[i * channels + c] * gi + 16384) >> 15;
				if (v > INT32_MAX) {
					v = INT32_MAX;
				} else if (v < INT32_MIN) {
					v = INT32_MIN;
				}
				dst[i * channels + c] = (int32_t)v;
			}
			out_q23 += out_step;
			in_q23 += in_step;
		}
		pos += slice;
	}
}

void crossfade_resampler_init(crossfade_resampler_t *r, int src_rate, int src_channels,
							  int dst_rate, int dst_channels) {
	memset(r, 0, sizeof(*r));
	r->src_rate = src_rate > 0 ? src_rate : 44100;
	r->src_channels = src_channels > 0 ? src_channels : 2;
	r->dst_rate = dst_rate > 0 ? dst_rate : 44100;
	r->dst_channels = dst_channels > 0 ? dst_channels : 2;
	r->step_fp = (((uint64_t)r->src_rate) << 32) / (uint64_t)r->dst_rate;
	if (r->step_fp == 0) {
		r->step_fp = 1;
	}
}

static inline void map_frame_s16_to_s32(const int16_t *src, int src_ch, int dst_ch, int32_t out[2]) {
	if (src_ch == 1) {
		int32_t v = src[0];
		out[0] = v;
		out[1] = v;
	} else if (dst_ch == 1) {
		int32_t sum = ((int32_t)src[0] + (int32_t)src[1]) / 2;
		out[0] = sum;
		out[1] = sum;
	} else {
		out[0] = src[0];
		out[1] = src[1];
	}
}

static inline void map_frame_s32(const int32_t *src, int src_ch, int dst_ch, int32_t out[2]) {
	if (src_ch == 1) {
		out[0] = src[0];
		out[1] = src[0];
	} else if (dst_ch == 1) {
		int32_t sum = (int32_t)(((int64_t)src[0] + (int64_t)src[1]) / 2);
		out[0] = sum;
		out[1] = sum;
	} else {
		out[0] = src[0];
		out[1] = src[1];
	}
}

#define RESAMPLE_SCRATCH_FRAMES 256

int crossfade_resample_s16(crossfade_resampler_t *r, int16_t *dst, int dst_frames,
						   crossfade_pull_s16_fn pull, void *user, uint64_t *src_consumed_out) {
	if (src_consumed_out) {
		*src_consumed_out = 0;
	}
	if (!r || !dst || dst_frames <= 0 || !pull || r->src_channels > 8 || r->dst_channels > 2) {
		return 0;
	}

	// Fast path: identical sample rate and channel count.
	if (r->src_rate == r->dst_rate && r->src_channels == r->dst_channels && !r->primed) {
		int written = 0;
		while (written < dst_frames) {
			int got = pull(user, dst + (size_t)written * (size_t)r->dst_channels, dst_frames - written);
			if (got <= 0) {
				break;
			}
			written += got;
		}
		if (src_consumed_out) {
			*src_consumed_out = (uint64_t)written;
		}
		return written;
	}

	// Same sample rate, mono <-> stereo conversion only.
	if (r->src_rate == r->dst_rate && !r->primed) {
		int16_t scratch[RESAMPLE_SCRATCH_FRAMES * 8];
		int written = 0;
		uint64_t consumed = 0;
		while (written < dst_frames) {
			int want = dst_frames - written;
			if (want > RESAMPLE_SCRATCH_FRAMES) {
				want = RESAMPLE_SCRATCH_FRAMES;
			}
			int got = pull(user, scratch, want);
			if (got <= 0) {
				break;
			}
			consumed += (uint64_t)got;
			for (int i = 0; i < got; i++) {
				int32_t pair[2];
				map_frame_s16_to_s32(scratch + (size_t)i * (size_t)r->src_channels,
									 r->src_channels, r->dst_channels, pair);
				dst[written * r->dst_channels] = (int16_t)pair[0];
				if (r->dst_channels == 2) {
					dst[written * 2 + 1] = (int16_t)pair[1];
				}
				written++;
			}
		}
		if (src_consumed_out) {
			*src_consumed_out = consumed;
		}
		return written;
	}

	int16_t one[8];
	int written = 0;
	uint64_t consumed = 0;
	while (written < dst_frames) {
		if (!r->primed) {
			int got = pull(user, one, 1);
			if (got <= 0) {
				break;
			}
			consumed++;
			map_frame_s16_to_s32(one, r->src_channels, r->dst_channels, r->prev);
			r->next[0] = r->prev[0];
			r->next[1] = r->prev[1];
			r->primed = true;
			r->have_next = false;
			r->pos_fp = 0;
		}

		while ((r->pos_fp >= (1ULL << 32)) || !r->have_next) {
			if (r->pos_fp >= (1ULL << 32) && r->have_next) {
				r->prev[0] = r->next[0];
				r->prev[1] = r->next[1];
				r->have_next = false;
				r->pos_fp -= (1ULL << 32);
				continue;
			}
			int got = pull(user, one, 1);
			if (got <= 0) {
				break;
			}
			consumed++;
			map_frame_s16_to_s32(one, r->src_channels, r->dst_channels, r->next);
			r->have_next = true;
		}

		if (r->pos_fp >= (1ULL << 32)) {
			break; // source reached EOF
		}

		int32_t frac_q15 = (int32_t)(r->pos_fp >> 17); // 0..32767
		for (int c = 0; c < r->dst_channels; c++) {
			int32_t p = r->prev[c];
			int32_t n = r->have_next ? r->next[c] : p;
			int32_t v = p + (((n - p) * frac_q15) >> 15);
			if (v > 32767) {
				v = 32767;
			} else if (v < -32768) {
				v = -32768;
			}
			dst[written * r->dst_channels + c] = (int16_t)v;
		}
		written++;
		r->pos_fp += r->step_fp;
	}

	if (src_consumed_out) {
		*src_consumed_out = consumed;
	}
	return written;
}

int crossfade_resample_s32(crossfade_resampler_t *r, int32_t *dst, int dst_frames,
						   crossfade_pull_s32_fn pull, void *user, uint64_t *src_consumed_out) {
	if (src_consumed_out) {
		*src_consumed_out = 0;
	}
	if (!r || !dst || dst_frames <= 0 || !pull || r->src_channels > 8 || r->dst_channels > 2) {
		return 0;
	}

	if (r->src_rate == r->dst_rate && r->src_channels == r->dst_channels && !r->primed) {
		int written = 0;
		while (written < dst_frames) {
			int got = pull(user, dst + (size_t)written * (size_t)r->dst_channels, dst_frames - written);
			if (got <= 0) {
				break;
			}
			written += got;
		}
		if (src_consumed_out) {
			*src_consumed_out = (uint64_t)written;
		}
		return written;
	}

	if (r->src_rate == r->dst_rate && !r->primed) {
		int32_t scratch[RESAMPLE_SCRATCH_FRAMES * 8];
		int written = 0;
		uint64_t consumed = 0;
		while (written < dst_frames) {
			int want = dst_frames - written;
			if (want > RESAMPLE_SCRATCH_FRAMES) {
				want = RESAMPLE_SCRATCH_FRAMES;
			}
			int got = pull(user, scratch, want);
			if (got <= 0) {
				break;
			}
			consumed += (uint64_t)got;
			for (int i = 0; i < got; i++) {
				int32_t pair[2];
				map_frame_s32(scratch + (size_t)i * (size_t)r->src_channels,
							  r->src_channels, r->dst_channels, pair);
				dst[written * r->dst_channels] = pair[0];
				if (r->dst_channels == 2) {
					dst[written * 2 + 1] = pair[1];
				}
				written++;
			}
		}
		if (src_consumed_out) {
			*src_consumed_out = consumed;
		}
		return written;
	}

	int32_t one[8];
	int written = 0;
	uint64_t consumed = 0;
	while (written < dst_frames) {
		if (!r->primed) {
			int got = pull(user, one, 1);
			if (got <= 0) {
				break;
			}
			consumed++;
			map_frame_s32(one, r->src_channels, r->dst_channels, r->prev);
			r->next[0] = r->prev[0];
			r->next[1] = r->prev[1];
			r->primed = true;
			r->have_next = false;
			r->pos_fp = 0;
		}

		while ((r->pos_fp >= (1ULL << 32)) || !r->have_next) {
			if (r->pos_fp >= (1ULL << 32) && r->have_next) {
				r->prev[0] = r->next[0];
				r->prev[1] = r->next[1];
				r->have_next = false;
				r->pos_fp -= (1ULL << 32);
				continue;
			}
			int got = pull(user, one, 1);
			if (got <= 0) {
				break;
			}
			consumed++;
			map_frame_s32(one, r->src_channels, r->dst_channels, r->next);
			r->have_next = true;
		}

		if (r->pos_fp >= (1ULL << 32)) {
			break;
		}

		int64_t frac_q16 = (int64_t)(r->pos_fp >> 16); // 0..65535
		for (int c = 0; c < r->dst_channels; c++) {
			int64_t p = r->prev[c];
			int64_t n = r->have_next ? r->next[c] : p;
			int64_t v = p + (((n - p) * frac_q16) >> 16);
			if (v > INT32_MAX) {
				v = INT32_MAX;
			} else if (v < INT32_MIN) {
				v = INT32_MIN;
			}
			dst[written * r->dst_channels + c] = (int32_t)v;
		}
		written++;
		r->pos_fp += r->step_fp;
	}

	if (src_consumed_out) {
		*src_consumed_out = consumed;
	}
	return written;
}
