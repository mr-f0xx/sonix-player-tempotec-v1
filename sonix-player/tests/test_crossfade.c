// test_crossfade.c -- crossfade equal-power curve, s16/s32 slice mixing,
// stateful resampler + channel mapper, ReplayGain Q15 precomputation, and
// deterministic playlist_peek_crossfade() across all playback modes.

#include <assert.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "src/system/audio/crossfade.h"
#include "src/system/audio/replaygain.h"
#include "src/system/core/config.h"
#include "src/system/playback/playlist.h"

static int failures;

#define CHECK(cond)                                                         \
	do {                                                                    \
		if (!(cond)) {                                                      \
			failures++;                                                     \
			fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
		}                                                                   \
	} while (0)

static void test_equal_power_curve(void) {
	int go = 0, gi = 0;

	crossfade_gains_q15(0.0, &go, &gi);
	CHECK(go == 32768 && gi == 0);

	crossfade_gains_q15(-0.5, &go, &gi);
	CHECK(go == 32768 && gi == 0);

	crossfade_gains_q15(1.0, &go, &gi);
	CHECK(go == 0 && gi == 32768);

	crossfade_gains_q15(1.5, &go, &gi);
	CHECK(go == 0 && gi == 32768);

	crossfade_gains_q15(0.5, &go, &gi);
	// cos(pi/4) = sin(pi/4) ~= 0.70710678 -> ~23170 in Q15 (-3.01 dB each).
	CHECK(abs(go - 23170) <= 2);
	CHECK(abs(gi - 23170) <= 2);

	int prev_out = 32768, prev_in = 0;
	for (int step = 0; step <= 1000; step++) {
		double t = (double)step / 1000.0;
		crossfade_gains_q15(t, &go, &gi);
		CHECK(go <= prev_out && gi >= prev_in);
		prev_out = go;
		prev_in = gi;

		// Constant power: go^2 + gi^2 ~= 1.0 in Q30 within <0.05 dB.
		double power = ((double)go * (double)go + (double)gi * (double)gi) / (32768.0 * 32768.0);
		CHECK(fabs(power - 1.0) < 0.004);
	}
}

static void test_mix_s16_and_s32(void) {
	enum { FRAMES = 512, CH = 2 };
	int16_t out16[FRAMES * CH];
	int16_t in16[FRAMES * CH];
	for (int i = 0; i < FRAMES * CH; i++) {
		out16[i] = 20000;
		in16[i] = 10000;
	}

	// At t=0..0 outgoing is untouched.
	crossfade_mix_s16(out16, in16, FRAMES, CH, 0.0, 0.0);
	CHECK(out16[0] == 20000 && out16[FRAMES * CH - 1] == 20000);

	// At t=1..1 incoming completely replaces outgoing.
	crossfade_mix_s16(out16, in16, FRAMES, CH, 1.0, 1.0);
	CHECK(out16[0] == 10000 && out16[FRAMES * CH - 1] == 10000);

	// Saturation clamp on correlated full-scale peaks at midpoint:
	// (32767 * 23170 + 32767 * 23170) >> 15 = 46339 -> clamped cleanly to 32767, never wraps negative.
	for (int i = 0; i < FRAMES * CH; i++) {
		out16[i] = 32767;
		in16[i] = 32767;
	}
	crossfade_mix_s16(out16, in16, FRAMES, CH, 0.0, 1.0);
	for (int i = 0; i < FRAMES * CH; i++) {
		CHECK(out16[i] > 0 && out16[i] == 32767);
	}

	for (int i = 0; i < FRAMES * CH; i++) {
		out16[i] = -32768;
		in16[i] = -32768;
	}
	crossfade_mix_s16(out16, in16, FRAMES, CH, 0.5, 0.5);
	for (int i = 0; i < FRAMES * CH; i++) {
		CHECK(out16[i] == -32768);
	}

	// S32 path: smooth ramp & saturation clamp.
	int32_t out32[FRAMES * CH];
	int32_t in32[FRAMES * CH];
	for (int i = 0; i < FRAMES * CH; i++) {
		out32[i] = 1000000000;
		in32[i] = 0;
	}
	crossfade_mix_s32(out32, in32, FRAMES, CH, 0.0, 1.0);
	CHECK(out32[0] > 990000000);
	CHECK(out32[(FRAMES / 2) * CH] > 690000000 && out32[(FRAMES / 2) * CH] < 720000000);
	CHECK(out32[(FRAMES - 1) * CH] < 50000000);

	for (int i = 0; i < FRAMES * CH; i++) {
		out32[i] = INT32_MAX;
		in32[i] = INT32_MAX;
	}
	crossfade_mix_s32(out32, in32, FRAMES, CH, 0.5, 0.5);
	CHECK(out32[0] == INT32_MAX && out32[FRAMES * CH - 1] == INT32_MAX);
}

typedef struct {
	const int16_t *pcm16;
	const int32_t *pcm32;
	int total_frames;
	int channels;
	int pos;
	int max_pull_chunk;
} pull_fixture_t;

static int pull_s16_cb(void *user, int16_t *dst, int max_frames) {
	pull_fixture_t *fx = user;
	if (fx->pos >= fx->total_frames) {
		return 0;
	}
	int n = max_frames;
	if (fx->max_pull_chunk > 0 && n > fx->max_pull_chunk) {
		n = fx->max_pull_chunk;
	}
	if (fx->pos + n > fx->total_frames) {
		n = fx->total_frames - fx->pos;
	}
	memcpy(dst, fx->pcm16 + (size_t)fx->pos * (size_t)fx->channels,
		   (size_t)n * (size_t)fx->channels * sizeof(int16_t));
	fx->pos += n;
	return n;
}

static int pull_s32_cb(void *user, int32_t *dst, int max_frames) {
	pull_fixture_t *fx = user;
	if (fx->pos >= fx->total_frames) {
		return 0;
	}
	int n = max_frames;
	if (fx->max_pull_chunk > 0 && n > fx->max_pull_chunk) {
		n = fx->max_pull_chunk;
	}
	if (fx->pos + n > fx->total_frames) {
		n = fx->total_frames - fx->pos;
	}
	memcpy(dst, fx->pcm32 + (size_t)fx->pos * (size_t)fx->channels,
		   (size_t)n * (size_t)fx->channels * sizeof(int32_t));
	fx->pos += n;
	return n;
}

static void test_resampler_and_channel_map(void) {
	// 1. Same-rate mono -> stereo passthrough (exact sample duplication, exact frame accounting).
	enum { SRC_N = 1000 };
	int16_t mono_src[SRC_N];
	for (int i = 0; i < SRC_N; i++) {
		mono_src[i] = (int16_t)(i * 17 - 8000);
	}
	pull_fixture_t fx1 = {.pcm16 = mono_src, .total_frames = SRC_N, .channels = 1, .pos = 0, .max_pull_chunk = 37};
	crossfade_resampler_t rs;
	crossfade_resampler_init(&rs, 44100, 1, 44100, 2);

	int16_t stereo_dst[SRC_N * 2];
	uint64_t total_consumed = 0;
	int out_pos = 0;
	while (out_pos < SRC_N) {
		int ask = 113;
		if (out_pos + ask > SRC_N) {
			ask = SRC_N - out_pos;
		}
		uint64_t c = 0;
		int got = crossfade_resample_s16(&rs, stereo_dst + out_pos * 2, ask, pull_s16_cb, &fx1, &c);
		CHECK(got == ask);
		total_consumed += c;
		out_pos += got;
	}
	CHECK(total_consumed == SRC_N);
	for (int i = 0; i < SRC_N; i++) {
		CHECK(stereo_dst[i * 2] == mono_src[i]);
		CHECK(stereo_dst[i * 2 + 1] == mono_src[i]);
	}

	// 2. Upsampling 44.1 kHz -> 96 kHz on a 1 kHz sine wave across chunk boundaries:
	// verify phase continuity (no discontinuities at chunk seams) and frequency preservation.
	enum { SINE_SRC = 4410, SINE_DST = 9000 };
	int16_t sine44[SINE_SRC * 2];
	for (int i = 0; i < SINE_SRC; i++) {
		double phase = 2.0 * 3.14159265358979323846 * 1000.0 * (double)i / 44100.0;
		sine44[i * 2] = (int16_t)lrint(20000.0 * sin(phase));
		sine44[i * 2 + 1] = (int16_t)lrint(20000.0 * cos(phase));
	}
	pull_fixture_t fx2 = {.pcm16 = sine44, .total_frames = SINE_SRC, .channels = 2, .pos = 0, .max_pull_chunk = 64};
	crossfade_resampler_init(&rs, 44100, 2, 96000, 2);

	int16_t sine96[SINE_DST * 2];
	total_consumed = 0;
	out_pos = 0;
	while (out_pos < SINE_DST) {
		int ask = 127; // prime chunk size to exercise fractional phase wrap across calls
		if (out_pos + ask > SINE_DST) {
			ask = SINE_DST - out_pos;
		}
		uint64_t c = 0;
		int got = crossfade_resample_s16(&rs, sine96 + out_pos * 2, ask, pull_s16_cb, &fx2, &c);
		CHECK(got == ask);
		total_consumed += c;
		out_pos += got;
	}
	// 9000 frames at 96 kHz = 93.75 ms ~= 4134.375 frames at 44.1 kHz.
	CHECK(total_consumed >= 4134 && total_consumed <= 4136);
	double max_err = 0.0;
	for (int i = 0; i < SINE_DST; i++) {
		double phase = 2.0 * 3.14159265358979323846 * 1000.0 * (double)i / 96000.0;
		double exp_l = 20000.0 * sin(phase);
		double exp_r = 20000.0 * cos(phase);
		double err_l = fabs((double)sine96[i * 2] - exp_l);
		double err_r = fabs((double)sine96[i * 2 + 1] - exp_r);
		if (err_l > max_err) max_err = err_l;
		if (err_r > max_err) max_err = err_r;
	}
	// Linear interpolation of 1 kHz at 44.1 kHz has < 0.25% error (< 60 out of 20000).
	CHECK(max_err < 60.0);

	// 3. Downsampling 96 kHz stereo -> 48 kHz mono (s32).
	enum { S32_SRC = 2000, S32_DST = 1000 };
	int32_t ramp96[S32_SRC * 2];
	for (int i = 0; i < S32_SRC; i++) {
		ramp96[i * 2] = i * 1000;
		ramp96[i * 2 + 1] = i * 3000;
	}
	pull_fixture_t fx3 = {.pcm32 = ramp96, .total_frames = S32_SRC, .channels = 2, .pos = 0, .max_pull_chunk = 50};
	crossfade_resampler_init(&rs, 96000, 2, 48000, 1);
	int32_t mono48[S32_DST];
	total_consumed = 0;
	int got32 = crossfade_resample_s32(&rs, mono48, S32_DST, pull_s32_cb, &fx3, &total_consumed);
	CHECK(got32 == S32_DST);
	for (int i = 0; i < S32_DST - 1; i++) {
		// Average of (2*i*1000) and (2*i*3000) = 4000 * i.
		CHECK(abs(mono48[i] - i * 4000) <= 2);
	}
}

static void test_replaygain_q15(void) {
	song_metadata_t meta;
	memset(&meta, 0, sizeof(meta));
	meta.has_track_gain = true;
	meta.track_gain_db = -6.0206; // ~0.5x -> 16384 in Q15
	meta.has_album_gain = true;
	meta.album_gain_db = 0.0;

	replaygain_set_mode(REPLAYGAIN_OFF);
	CHECK(replaygain_compute_q15(&meta) == 32768);

	replaygain_set_mode(REPLAYGAIN_TRACK);
	int g = replaygain_compute_q15(&meta);
	CHECK(abs(g - 16384) <= 8);

	replaygain_set_mode(REPLAYGAIN_ALBUM);
	CHECK(replaygain_compute_q15(&meta) == 32768);

	// Peak-limiting protection on positive gain (+6 dB requested, peak = 0.8 -> clamped to 1/0.8 = 1.25x = 40960).
	meta.track_gain_db = 6.0;
	meta.track_peak = 0.8;
	replaygain_set_mode(REPLAYGAIN_TRACK);
	g = replaygain_compute_q15(&meta);
	CHECK(abs(g - 40960) <= 16);

	int16_t s16[4] = {10000, -10000, 30000, -30000};
	replaygain_apply_q15_s16(s16, 2, 2, 16384);
	CHECK(abs(s16[0] - 5000) <= 1 && abs(s16[1] + 5000) <= 1);

	replaygain_set_mode(REPLAYGAIN_OFF);
}

static void test_playlist_peek_crossfade_modes(void) {
	const char *tracks[] = {"/card/t1.flac", "/card/t2.flac", "/card/t3.flac", "/card/t4.flac"};
	char peek[128], adv[128];

	// Normal mode: peeks next, false at end.
	playlist_set_mode(PLAYBACK_MODE_NORMAL);
	playlist_load_paths(tracks, 4, 2);
	CHECK(playlist_peek_crossfade(peek, sizeof(peek)) && strcmp(peek, "/card/t4.flac") == 0);
	CHECK(playlist_advance_auto(adv, sizeof(adv)) && strcmp(adv, peek) == 0);
	CHECK(!playlist_peek_crossfade(peek, sizeof(peek)));

	// Repeat All: wraps cleanly from end to index 0.
	playlist_set_mode(PLAYBACK_MODE_REPEAT_ALL);
	CHECK(playlist_peek_crossfade(peek, sizeof(peek)) && strcmp(peek, "/card/t1.flac") == 0);
	CHECK(playlist_advance_auto(adv, sizeof(adv)) && strcmp(adv, peek) == 0);

	// Repeat One: peeks current track.
	playlist_set_mode(PLAYBACK_MODE_REPEAT_ONE);
	CHECK(playlist_peek_crossfade(peek, sizeof(peek)) && strcmp(peek, "/card/t1.flac") == 0);

	// Shuffle Repeat wrap-around: playlist_peek_crossfade pre-deals the next round
	// deterministically and playlist_advance_auto lands on that exact track!
	playlist_set_mode(PLAYBACK_MODE_SHUFFLE_REPEAT);
	playlist_load_paths(tracks, 4, 0);
	for (int round = 0; round < 5; round++) {
		for (int step = 0; step < 4; step++) {
			char peeked_a[128], peeked_b[128];
			CHECK(playlist_peek_crossfade(peeked_a, sizeof(peeked_a)));
			// Calling peek repeatedly (as progress polling does) must return the exact same track.
			CHECK(playlist_peek_crossfade(peeked_b, sizeof(peeked_b)));
			CHECK(strcmp(peeked_a, peeked_b) == 0);

			CHECK(playlist_advance_auto(adv, sizeof(adv)));
			CHECK(strcmp(adv, peeked_a) == 0);
		}
	}

	// Queue mutation invalidates pre-dealt round cleanly.
	CHECK(playlist_set_current(3));
	CHECK(playlist_peek_crossfade(peek, sizeof(peek)));
	CHECK(playlist_remove_at(0));
	CHECK(playlist_peek_crossfade(peek, sizeof(peek)));
	CHECK(playlist_advance_auto(adv, sizeof(adv)) && strcmp(adv, peek) == 0);
}

int main(void) {
	char dir[] = "/tmp/sonix-xfade-test-XXXXXX";
	assert(mkdtemp(dir));
	char cfg_path[192];
	snprintf(cfg_path, sizeof(cfg_path), "%s/config.ini", dir);
	(void)config_init(cfg_path);

	test_equal_power_curve();
	test_mix_s16_and_s32();
	test_resampler_and_channel_map();
	test_replaygain_q15();
	test_playlist_peek_crossfade_modes();

	unlink(cfg_path);
	rmdir(dir);

	if (failures > 0) {
		fprintf(stderr, "test_crossfade: %d failure(s)\n", failures);
		return 1;
	}
	printf("test_crossfade: equal-power curve, s16/s32 mix, resampler, ReplayGain Q15, and shuffle-repeat peek passed\n");
	return 0;
}
