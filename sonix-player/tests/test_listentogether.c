#include "src/system/net/listentogether.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

/* Stub audio_external_* and audio_set_pcm_tap_cb for unit testing */
void audio_set_pcm_tap_cb(void (*cb)(const void *, int, int, int, int)) { (void)cb; }
void audio_set_external_release_cb(bool (*cb)(void)) { (void)cb; }
bool audio_external_begin_exclusive_latency(int sr, int ch, int bits, int ms) {
	(void)sr; (void)ch; (void)bits; (void)ms;
	return true;
}
int audio_external_write(const void *frames, int count) { (void)frames; return count; }
void audio_external_end(void) {}

static void test_mtu_size(void) {
	size_t pkt_bytes = sizeof(lt_pkt_header_t) + LT_PACKET_FRAMES * 4;
	assert(pkt_bytes <= 1472);
	printf("PASS: test_mtu_size (%zu bytes <= 1472)\n", pkt_bytes);
}

static void simulate_drift(double host_ppm) {
	const int target = 4410;
	lt_sync_resampler_t r;
	lt_sync_resampler_init(&r, target);

	double buf_fill = (double)target;
	const int block = 256;
	int16_t in[256 * 2];
	int16_t out[256 * 2 + 16];

	for (int i = 0; i < 256; i++) {
		double s = sin(2.0 * M_PI * 440.0 * (double)i / 44100.0) * 16000.0;
		in[i * 2] = (int16_t)s;
		in[i * 2 + 1] = (int16_t)s;
	}

	double host_rate_factor = 1.0 + host_ppm * 1e-6;
	int min_fill = target, max_fill = target;

	/* Simulate ~113 seconds (20,000 blocks of 256 frames) */
	for (int step = 0; step < 20000; step++) {
		buf_fill += (double)block * host_rate_factor;
		lt_sync_resampler_update_pi(&r, (int)buf_fill);

		int consumed = 0;
		int out_frames = lt_sync_resampler_process(&r, in, block, &consumed, out, block + 8);
		assert(out_frames > 0);
		buf_fill -= (double)consumed;

		if ((int)buf_fill < min_fill) min_fill = (int)buf_fill;
		if ((int)buf_fill > max_fill) max_fill = (int)buf_fill;
		assert(buf_fill > 1000.0 && buf_fill < 10000.0);
	}

	printf("PASS: simulate_drift(%+.0f ppm) -> min=%d max=%d final=%.1f (target=%d)\n",
		   host_ppm, min_fill, max_fill, buf_fill, target);
}

int main(void) {
	test_mtu_size();
	simulate_drift(+100.0);
	simulate_drift(-100.0);
	return 0;
}
