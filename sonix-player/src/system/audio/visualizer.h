#ifndef SONIX_VISUALIZER_H
#define SONIX_VISUALIZER_H
#include <stdbool.h>
#include <stdint.h>
// The Neon visualizer's bars: eight wide log-spaced bands, the shape the
// screensaver has always drawn.
#define VISUALIZER_BANDS 8
// The LED visualizer's bars: the same spectrum read at twice the resolution,
// sixteen narrow bars like a bench spectrum analyzer's scale.
#define VISUALIZER_LED_BANDS 16
typedef enum {
	VISUALIZER_SCREENSAVER = 1,
	VISUALIZER_STATUS_LED_BASS = 2,
	VISUALIZER_STATUS_LED_ENERGY = 4,
} visualizer_consumer_t;

// Capture PCM while any consumer is using it. One consumer turning off
// (for example, when the screen goes dark) must not stop the other.
void visualizer_enable(visualizer_consumer_t consumer, bool enabled);
void visualizer_feed(const void *pcm, int frames, int channels, int bits, int rate);
void visualizer_levels(uint8_t levels[VISUALIZER_BANDS]);
void visualizer_led_levels(uint8_t levels[VISUALIZER_LED_BANDS]);
// Bass energy from fresh PCM, or zero when the feed has stopped. The LED
// worker turns rising edges into pulses; audio playback is never made to wait.
uint8_t visualizer_bass_level(void);
// Overall PCM level, measured across channels before the mono mix. Like bass,
// this is zero until a full window arrives and after the feed goes stale.
uint8_t visualizer_energy_level(void);
#endif
