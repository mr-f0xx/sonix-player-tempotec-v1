#include "visualizer.h"
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <string.h>
#include <time.h>

// The audio thread never waits for the UI. Only a short, downsampled window is
// copied; analysis runs on the UI thread, never in the real-time write path.
#define WINDOW 256
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static atomic_bool active;
static float samples[WINDOW];
static int count, stride_phase, sample_rate;
static struct timespec updated;

void visualizer_enable(bool enabled) {
    atomic_store(&active, enabled);
    if (!enabled) {
        pthread_mutex_lock(&lock);
        count = stride_phase = 0;
        updated.tv_sec = updated.tv_nsec = 0;
        pthread_mutex_unlock(&lock);
    }
}

void visualizer_feed(const void *pcm, int frames, int channels, int bits, int rate) {
    if (!atomic_load(&active) || !pcm || frames <= 0 || channels <= 0 || rate <= 0 ||
        (bits != 16 && bits != 24 && bits != 32) || pthread_mutex_trylock(&lock)) return;
    int stride = rate / 11025;
    if (stride < 1) stride = 1;
    int sr = rate / stride;
    if (sample_rate != sr) { count = stride_phase = 0; sample_rate = sr; }
    const unsigned char *p = pcm;
    int bytes = bits / 8;
    // Jump directly between the samples we keep, rather than visiting every
    // source frame (especially important for high-rate material).
    int first = stride_phase ? stride - stride_phase : 0;
    for (int i = first; i < frames; i += stride) {
        float sum = 0;
        for (int c = 0; c < channels; c++) {
            const unsigned char *s = p + ((size_t)i * channels + c) * bytes;
            int32_t v;
            if (bits == 16) { int16_t x; memcpy(&x, s, 2); v = x; sum += v / 32768.0f; }
            else if (bits == 24) { v = (int32_t)((uint32_t)s[0] | (uint32_t)s[1]<<8 | (uint32_t)s[2]<<16); if (v & 0x800000) v |= (int32_t)0xff000000; sum += v / 8388608.0f; }
            else { memcpy(&v, s, 4); sum += v / 2147483648.0f; }
        }
        samples[count++ % WINDOW] = sum / channels;
        if (count >= WINDOW * 2) count -= WINDOW;
    }
    stride_phase = (stride_phase + frames % stride) % stride;
    clock_gettime(CLOCK_MONOTONIC, &updated);
    pthread_mutex_unlock(&lock);
}

// The freshest full window of downsampled samples, Hann-applied, or false when
// there is nothing current to analyse (silence, paused, or a stale feed).
static bool visualizer_window(float *data, int *n_out, int *rate_out) {
    int n, rate; struct timespec stamp, now;
    pthread_mutex_lock(&lock);
    n = count < WINDOW ? count : WINDOW; rate = sample_rate; stamp = updated;
    for (int i = 0; i < n; i++) data[i] = samples[(count - n + i) % WINDOW];
    pthread_mutex_unlock(&lock);
    clock_gettime(CLOCK_MONOTONIC, &now);
    if (n < WINDOW || !stamp.tv_sec || now.tv_sec - stamp.tv_sec > 1 || rate <= 0) return false;
    // Apply the window once, not once per band.
    static float window[WINDOW];
    static bool window_ready;
    if (!window_ready) {
        for (int i = 0; i < WINDOW; i++) window[i] = 0.5f - 0.5f * cosf(6.2831853f * i / (WINDOW - 1));
        window_ready = true;
    }
    for (int i = 0; i < n; i++) data[i] *= window[i];
    *n_out = n;
    *rate_out = rate;
    return true;
}

// One Hann-windowed Goertzel filter, mapped through the log law. The gain is
// the band's perceptual weight: music carries most of its energy at the bass
// end, and a flat mapping left the treble bars sitting high on their noise
// floor while the bass bars danced between full and empty -- a display that
// read empty on the left and busy on the right however the mix sounded.
static uint8_t band_level(const float *data, int n, int rate, float freq, float gain) {
    if (freq >= rate * 0.5f) return 0;
    float coeff = 2.0f * cosf(6.2831853f * freq / rate), a = 0, z = 0;
    for (int i = 0; i < n; i++) {
        float x = data[i];
        float v = x + coeff * a - z; z = a; a = v;
    }
    float magnitude = sqrtf(fmaxf(0, a*a + z*z - coeff*a*z)) / (WINDOW * 0.25f);
    float level = (log10f(magnitude * gain + 0.00001f) + 3.0f) / 3.0f;
    if (level < 0) level = 0;
    if (level > 1) level = 1;
    return (uint8_t)(level * 255);
}

void visualizer_levels(uint8_t levels[VISUALIZER_BANDS]) {
    float data[WINDOW]; int n, rate;
    memset(levels, 0, VISUALIZER_BANDS);
    if (!visualizer_window(data, &n, &rate)) return;
    // Log-spaced centres, Hann-windowed Goertzel filters.
    static const float freq[VISUALIZER_BANDS] = {85, 150, 270, 480, 850, 1500, 2700, 4400};
    static const float gain[VISUALIZER_BANDS] = {3.2f, 2.6f, 2.0f, 1.5f, 1.1f, 0.8f, 0.6f, 0.45f};
    for (int b = 0; b < VISUALIZER_BANDS; b++)
        levels[b] = band_level(data, n, rate, freq[b], gain[b]);
}

void visualizer_led_levels(uint8_t levels[VISUALIZER_LED_BANDS]) {
    float data[WINDOW]; int n, rate;
    memset(levels, 0, VISUALIZER_LED_BANDS);
    if (!visualizer_window(data, &n, &rate)) return;
    // Sixteen log-spaced centres from deep bass to the top of the musical
    // range, each weighted like its neighbours above.
    static const float freq[VISUALIZER_LED_BANDS] = {45, 60, 82, 112, 153, 209, 286, 392,
                                                     536, 733, 1003, 1372, 1877, 2567, 3510, 4800};
    static const float gain[VISUALIZER_LED_BANDS] = {3.6f, 3.2f, 2.8f, 2.4f, 2.0f, 1.7f, 1.4f, 1.15f,
                                                     0.95f, 0.8f, 0.65f, 0.55f, 0.45f, 0.38f, 0.32f, 0.27f};
    for (int b = 0; b < VISUALIZER_LED_BANDS; b++)
        levels[b] = band_level(data, n, rate, freq[b], gain[b]);
}
