#include "src/system/audio/visualizer.h"
#include <assert.h>
#include <limits.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>

static int16_t pcm[VISUALIZER_SAMPLES * 2];
static void reset(void) { visualizer_enable(false); visualizer_enable(true); }
static void tone(double hz, double gain, int rate) {
	for (int i = 0; i < VISUALIZER_SAMPLES; i++) {
		pcm[i * 2] = (int16_t)(32767 * gain * sin(2 * M_PI * hz * i / rate));
		pcm[i * 2 + 1] = -pcm[i * 2]; // anti-phase stereo must not erase the picture
	}
}
static visualizer_frame_t capture(visualizer_mode_t mode) {
	visualizer_frame_t frame;
	visualizer_feed(pcm, VISUALIZER_SAMPLES, 2, 48000, 16);
	assert(visualizer_read(mode, &frame));
	return frame;
}
static void *producer(void *unused) {
	(void)unused;
	for (int i = 0; i < 20000; i++) visualizer_feed(pcm, VISUALIZER_SAMPLES, 2, 48000, 16);
	return NULL;
}
int main(void) {
	visualizer_frame_t frame;
	tone(1500, 0.8, 48000); // exact FFT bin
	visualizer_feed(pcm, VISUALIZER_SAMPLES, 2, 48000, 16);
	assert(!visualizer_read(VISUALIZER_SPECTRUM, &frame)); // disabled by default
	reset();
	visualizer_feed(NULL, 512, 2, 48000, 16);
	visualizer_feed(pcm, -1, 2, 48000, 16);
	visualizer_feed(pcm, 512, 0, 48000, 16);
	visualizer_feed(pcm, 512, 2, 0, 16);
	visualizer_feed(pcm, 512, 2, 48000, 24);
	assert(!visualizer_read(VISUALIZER_SPECTRUM, &frame));
	frame = capture(VISUALIZER_SPECTRUM);
	int peak = 0;
	for (int i = 1; i < VISUALIZER_BANDS; i++) if (frame.bands[i] > frame.bands[peak]) peak = i;
	assert(peak == 8); // logarithmic 1095..1574 Hz band
	assert(frame.bands[peak] > 235);
	assert(!visualizer_read(VISUALIZER_SPECTRUM, &frame)); // consume-once mailbox
	reset();
	tone(1500, 0.08, 48000);
	frame = capture(VISUALIZER_SPECTRUM);
	assert(frame.bands[8] > 145 && frame.bands[8] < 175); // 20 dB reduction
	reset();
	memset(pcm, 0, sizeof(pcm));
	frame = capture(VISUALIZER_SPECTRUM);
	for (int i = 0; i < VISUALIZER_BANDS; i++) assert(frame.bands[i] == 0);
	reset();
	tone(1500, 1, 48000);
	int16_t saved[sizeof(pcm) / sizeof(*pcm)];
	memcpy(saved, pcm, sizeof(pcm));
	// A window assembled across decoder chunks, without padding or gaps.
	visualizer_feed(pcm, 200, 2, 48000, 16);
	assert(!visualizer_read(VISUALIZER_WAVEFORM, &frame));
	visualizer_feed(pcm + 400, 312, 2, 48000, 16);
	assert(visualizer_read(VISUALIZER_WAVEFORM, &frame));
	for (int i = 0; i < VISUALIZER_POINTS; i++) assert(frame.wave[i] == pcm[2 * (i * VISUALIZER_SAMPLES / VISUALIZER_POINTS)]);
	assert(memcmp(saved, pcm, sizeof(pcm)) == 0);
	visualizer_feed(pcm, 512, 2, 48000, 16);
	assert(!visualizer_read(VISUALIZER_WAVEFORM, &frame)); // frame budget
	visualizer_enable(false);
	assert(!visualizer_read(VISUALIZER_WAVEFORM, &frame));
	reset();
	int32_t wide[512];
	for (int i = 0; i < 512; i++) wide[i] = i % 2 ? INT32_MIN : INT32_MAX;
	visualizer_feed(wide, 512, 1, 192000, 32);
	assert(visualizer_read(VISUALIZER_WAVEFORM, &frame));
	assert(frame.wave[0] == 32767 && frame.wave[1] == -32768);
	reset();
	// Format changes discard partial windows.
	visualizer_feed(pcm, 200, 2, 48000, 16);
	visualizer_feed(wide, 312, 1, 192000, 32);
	assert(!visualizer_read(VISUALIZER_WAVEFORM, &frame));
	visualizer_feed(wide, 200, 1, 192000, 32);
	assert(visualizer_read(VISUALIZER_SPECTRUM, &frame));
	reset();
	visualizer_backoff();
	for (int i = 0; i < 3 * 48000 / 512; i++) {
		visualizer_feed(pcm, 512, 2, 48000, 16);
		assert(!visualizer_read(VISUALIZER_SPECTRUM, &frame));
	}
	visualizer_feed(pcm, 512, 2, 48000, 16); // end of cooldown, partial window
	visualizer_feed(pcm, 512, 2, 48000, 16);
	assert(visualizer_read(VISUALIZER_SPECTRUM, &frame));
	// Adversarial input amplitudes/rates, including saturated DC and impulses.
	for (int rate = 8000; rate <= 768000; rate *= 2) {
		for (int i = 0; i < 512; i++) wide[i] = i % 3 ? INT32_MIN : INT32_MAX;
		reset();
		visualizer_feed(wide, 512, 1, rate, 32);
		assert(visualizer_read(VISUALIZER_SPECTRUM, &frame));
	}
	reset();
	pthread_t worker;
	assert(pthread_create(&worker, NULL, producer, NULL) == 0);
	for (int i = 0; i < 2000; i++) {
		if (i % 5 == 0) reset();
		visualizer_read(VISUALIZER_SPECTRUM, &frame);
	}
	assert(pthread_join(worker, NULL) == 0);
	visualizer_enable(false);
	puts("PASS visualizer: tones, silence, 16/32-bit, chunks, disabled, throttle, recovery, concurrency");
}
