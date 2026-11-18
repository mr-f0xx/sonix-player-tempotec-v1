#include "visualizer.h"
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <string.h>
#include <time.h>

// The audio thread never waits for a reader. Only a short, downsampled window
// is captured; analysis runs on the GUI or LED worker, not in the PCM path.
#define WINDOW 256
#define SCREEN_MAX_AGE_MS 1500
#define BASS_MAX_AGE_MS 250
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static atomic_uint consumers;
static float samples[WINDOW];
static int count, stride_phase, source_rate;
static struct timespec updated;

void visualizer_enable(visualizer_consumer_t consumer, bool enabled) {
    pthread_mutex_lock(&lock);
    unsigned current = atomic_load_explicit(&consumers, memory_order_relaxed);
    unsigned next = enabled ? current | consumer : current & ~consumer;
    atomic_store_explicit(&consumers, next, memory_order_relaxed);
    if (!next) count = stride_phase = 0;
    pthread_mutex_unlock(&lock);
}

void visualizer_feed(const void *pcm, int frames, int channels, int bits, int rate) {
    if (!atomic_load_explicit(&consumers, memory_order_relaxed) || !pcm || frames <= 0 || channels <= 0 || rate <= 0 ||
        (bits != 16 && bits != 24 && bits != 32) || pthread_mutex_trylock(&lock)) return;
    // The last reader could have switched off between the fast check and lock.
    if (!atomic_load_explicit(&consumers, memory_order_relaxed)) {
        pthread_mutex_unlock(&lock);
        return;
    }
    int stride = rate / 11025;
    if (stride < 1) stride = 1;
    if (source_rate != rate) { count = stride_phase = 0; source_rate = rate; }
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

static pthread_once_t hann_once = PTHREAD_ONCE_INIT;
static float hann[WINDOW];

static void init_hann(void) {
    for (int i = 0; i < WINDOW; i++) hann[i] = 0.5f - 0.5f * cosf(6.2831853f * i / (WINDOW - 1));
}

// Copy one fresh full window. The Hann coefficients are shared by both
// readers, and the math runs with the ring unlocked so PCM can keep coming in.
static bool visualizer_window(float data[WINDOW], int *rate_out, visualizer_consumer_t consumer, int max_age_ms) {
    pthread_mutex_lock(&lock);
    if (count < WINDOW || !(atomic_load_explicit(&consumers, memory_order_relaxed) & consumer)) {
        pthread_mutex_unlock(&lock);
        return false;
    }
    struct timespec stamp = updated;
    int rate = source_rate;
    for (int i = 0; i < WINDOW; i++) data[i] = samples[(count - WINDOW + i) % WINDOW];
    pthread_mutex_unlock(&lock);

    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    int64_t age_ns = ((int64_t)now.tv_sec - stamp.tv_sec) * INT64_C(1000000000) + now.tv_nsec - stamp.tv_nsec;
    if (age_ns > (int64_t)max_age_ms * 1000000) return false;

    pthread_once(&hann_once, init_hann);
    for (int i = 0; i < WINDOW; i++) data[i] *= hann[i];
    int stride = rate / 11025;
    *rate_out = stride > 1 ? rate / stride : rate;
    return true;
}

// One Hann-windowed Goertzel filter, mapped through the log law. The gain is
// the band's perceptual weight: music carries most of its energy at the bass
// end, and a flat mapping left the treble bars sitting high on their noise
// floor while the bass bars danced between full and empty -- a display that
// read empty on the left and busy on the right however the mix sounded.
static uint8_t band_level(const float data[WINDOW], int rate, float freq, float gain) {
    if (freq >= rate * 0.5f) return 0;
    float coeff = 2.0f * cosf(6.2831853f * freq / rate), a = 0, z = 0;
    for (int i = 0; i < WINDOW; i++) {
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
    float data[WINDOW]; int rate;
    memset(levels, 0, VISUALIZER_BANDS);
    if (!visualizer_window(data, &rate, VISUALIZER_SCREENSAVER, SCREEN_MAX_AGE_MS)) return;
    // Log-spaced centres, Hann-windowed Goertzel filters.
    static const float freq[VISUALIZER_BANDS] = {85, 150, 270, 480, 850, 1500, 2700, 4400};
    static const float gain[VISUALIZER_BANDS] = {3.2f, 2.6f, 2.0f, 1.5f, 1.1f, 0.8f, 0.6f, 0.45f};
    for (int b = 0; b < VISUALIZER_BANDS; b++)
        levels[b] = band_level(data, rate, freq[b], gain[b]);
}

void visualizer_led_levels(uint8_t levels[VISUALIZER_LED_BANDS]) {
    float data[WINDOW]; int rate;
    memset(levels, 0, VISUALIZER_LED_BANDS);
    if (!visualizer_window(data, &rate, VISUALIZER_SCREENSAVER, SCREEN_MAX_AGE_MS)) return;
    // Sixteen log-spaced centres from deep bass to the top of the musical
    // range, each weighted like its neighbours above.
    static const float freq[VISUALIZER_LED_BANDS] = {45, 60, 82, 112, 153, 209, 286, 392,
                                                     536, 733, 1003, 1372, 1877, 2567, 3510, 4800};
    static const float gain[VISUALIZER_LED_BANDS] = {3.6f, 3.2f, 2.8f, 2.4f, 2.0f, 1.7f, 1.4f, 1.15f,
                                                     0.95f, 0.8f, 0.65f, 0.55f, 0.45f, 0.38f, 0.32f, 0.27f};
    for (int b = 0; b < VISUALIZER_LED_BANDS; b++)
        levels[b] = band_level(data, rate, freq[b], gain[b]);
}

uint8_t visualizer_bass_level(void) {
    float data[WINDOW]; int rate;
    if (!visualizer_window(data, &rate, VISUALIZER_STATUS_LED, BASS_MAX_AGE_MS)) return 0;
    uint8_t low = band_level(data, rate, 65, 2.0f);
    uint8_t high = band_level(data, rate, 135, 2.0f);
    return low > high ? low : high;
}
