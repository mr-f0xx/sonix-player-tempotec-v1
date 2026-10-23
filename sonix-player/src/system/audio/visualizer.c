#include "visualizer.h"

#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <string.h>

// Word-sized atomics stay lock-free on the MIPS32r2 target.
_Static_assert(ATOMIC_INT_LOCK_FREE == 2, "visualizer needs lock-free word atomics");
static atomic_uint enabled, overloaded;

void visualizer_backoff(void) {
	if (atomic_load_explicit(&enabled, memory_order_relaxed))
		atomic_store_explicit(&overloaded, true, memory_order_relaxed);
}
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static int16_t samples[VISUALIZER_SAMPLES];
static int filled, sample_rate, sample_bits, sample_channels, skip;
static bool ready;

void visualizer_enable(bool on) {
	if (atomic_load_explicit(&enabled, memory_order_relaxed) == on) return;
	pthread_mutex_lock(&lock);
	filled = skip = 0;
	ready = false;
	atomic_store_explicit(&overloaded, false, memory_order_relaxed);
	atomic_store_explicit(&enabled, on, memory_order_relaxed);
	pthread_mutex_unlock(&lock);
}

void visualizer_feed(const void *pcm, int frames, int channels, int rate, int bits) {
	if (!atomic_load_explicit(&enabled, memory_order_relaxed) || !pcm || frames <= 0 ||
		channels < 1 || channels > 8 || rate < 8000 || rate > 768000 || (bits != 16 && bits != 32)) return;
	// Dropping a picture is always preferable to delaying even one audio write.
	if (pthread_mutex_trylock(&lock) != 0) return;
	if (!atomic_load_explicit(&enabled, memory_order_relaxed)) {
		pthread_mutex_unlock(&lock);
		return;
	}
	if (sample_rate != rate || sample_bits != bits || sample_channels != channels) {
		filled = skip = 0;
		ready = false;
		sample_rate = rate;
		sample_bits = bits;
		sample_channels = channels;
	}
	if (atomic_exchange_explicit(&overloaded, false, memory_order_relaxed)) {
		filled = 0;
		ready = false;
		skip = rate * 3;
	}
	if (skip >= frames) {
		skip -= frames;
		pthread_mutex_unlock(&lock);
		return;
	}
	int offset = skip;
	skip = 0;
	// One mailbox: never overwrite a window the UI has not consumed. There
	// is no queue to grow when the renderer falls behind.
	if (!ready) {
		int n = frames - offset;
		if (n > VISUALIZER_SAMPLES - filled) n = VISUALIZER_SAMPLES - filled;
		for (int i = 0; i < n; i++) {
			int at = (offset + i) * channels;
			// First channel, deliberately: summing anti-phase stereo can erase
			// the picture. This is a lightweight display, not a measurement tool.
			samples[filled++] = bits == 16 ? ((const int16_t *)pcm)[at] :
				(int16_t)(((const int32_t *)pcm)[at] / 65536);
		}
		if (filled == VISUALIZER_SAMPLES) {
			ready = true;
			skip = rate / 15; // at most 15 windows/s, independent of block size
		}
	}
	pthread_mutex_unlock(&lock);
}

// Tables initialized on the UI thread, never in the playback loop.
static int16_t window[VISUALIZER_SAMPLES], cosine[VISUALIZER_SAMPLES / 2], sine[VISUALIZER_SAMPLES / 2];
static bool tables_ready;
static void tables_init(void) {
	if (tables_ready) return;
	for (int i = 0; i < VISUALIZER_SAMPLES; i++) {
		window[i] = (int16_t)(32767 * (0.5 - 0.5 * cos(2 * M_PI * i / (VISUALIZER_SAMPLES - 1))));
		if (i < VISUALIZER_SAMPLES / 2) {
			cosine[i] = (int16_t)(32767 * cos(2 * M_PI * i / VISUALIZER_SAMPLES));
			sine[i] = (int16_t)(-32767 * sin(2 * M_PI * i / VISUALIZER_SAMPLES));
		}
	}
	tables_ready = true;
}

bool visualizer_read(visualizer_mode_t mode, visualizer_frame_t *out) {
	if (!out || mode < VISUALIZER_SPECTRUM || mode > VISUALIZER_WAVEFORM) return false;
	int16_t input[VISUALIZER_SAMPLES];
	int rate;
	pthread_mutex_lock(&lock);
	if (!ready || !atomic_load_explicit(&enabled, memory_order_relaxed)) {
		pthread_mutex_unlock(&lock);
		return false;
	}
	memcpy(input, samples, sizeof(input));
	rate = sample_rate;
	filled = 0;
	ready = false;
	pthread_mutex_unlock(&lock);
	memset(out, 0, sizeof(*out));
	for (int i = 0; i < VISUALIZER_POINTS; i++) out->wave[i] = input[i * VISUALIZER_SAMPLES / VISUALIZER_POINTS];
	if (mode == VISUALIZER_WAVEFORM) return true; // no FFT at all
	tables_init();
	int32_t re[VISUALIZER_SAMPLES], im[VISUALIZER_SAMPLES] = {0};
	for (unsigned i = 0; i < VISUALIZER_SAMPLES; i++) {
		unsigned j = 0;
		for (unsigned b = i, k = VISUALIZER_SAMPLES; k > 1; k >>= 1, b >>= 1) j = (j << 1) | (b & 1);
		re[j] = (int32_t)input[i] * window[i] / 32768;
	}
	// Radix-2, scaled at every stage: 16-bit bounds, no soft-float FFT on MIPS.
	for (int len = 2; len <= VISUALIZER_SAMPLES; len *= 2) {
		for (int start = 0; start < VISUALIZER_SAMPLES; start += len) {
			for (int j = 0; j < len / 2; j++) {
				int a = start + j, b = a + len / 2, t = j * VISUALIZER_SAMPLES / len;
				int32_t r = (re[b] * cosine[t] - im[b] * sine[t]) / 32768;
				int32_t v = (re[b] * sine[t] + im[b] * cosine[t]) / 32768;
				re[b] = (re[a] - r) / 2;
				im[b] = (im[a] - v) / 2;
				re[a] = (re[a] + r) / 2;
				im[a] = (im[a] + v) / 2;
			}
		}
	}
	// Fixed logarithmic edges, 60 Hz..20 kHz. Unresolved / above-Nyquist
	// bands stay empty rather than inventing bass or folding ultrasonic energy.
	for (int band = 0; band < VISUALIZER_BANDS; band++) {
		double low = 60 * pow(20000.0 / 60, band / (double)VISUALIZER_BANDS);
		double high = 60 * pow(20000.0 / 60, (band + 1) / (double)VISUALIZER_BANDS);
		int first = (int)ceil(low * VISUALIZER_SAMPLES / rate);
		int last = (int)ceil(high * VISUALIZER_SAMPLES / rate);
		if (first < 1) first = 1;
		if (last > VISUALIZER_SAMPLES / 2) last = VISUALIZER_SAMPLES / 2;
		double power = 0;
		for (int k = first; k < last; k++) {
			double p = (double)re[k] * re[k] + (double)im[k] * im[k];
			if (p > power) power = p;
		}
		// Hann coherent gain 0.5, normalized FFT: full-scale sine is ~8192.
		double level = power > 0 ? (10 * log10(power / (8192.0 * 8192)) + 60) / 60 : 0;
		if (level < 0) level = 0;
		if (level > 1) level = 1;
		out->bands[band] = (uint8_t)(level * 255);
	}
	return true;
}
