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
void visualizer_enable(bool enabled);
void visualizer_feed(const void *pcm, int frames, int channels, int bits, int rate);
void visualizer_levels(uint8_t levels[VISUALIZER_BANDS]);
void visualizer_led_levels(uint8_t levels[VISUALIZER_LED_BANDS]);
#endif
