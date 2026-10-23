#ifndef SONIX_VISUALIZER_H
#define SONIX_VISUALIZER_H

#include <stdbool.h>
#include <stdint.h>

#define VISUALIZER_SAMPLES 512
#define VISUALIZER_BANDS 16
#define VISUALIZER_POINTS 96

typedef enum {
	VISUALIZER_OFF,
	VISUALIZER_SPECTRUM,
	VISUALIZER_WAVEFORM
} visualizer_mode_t;

typedef struct {
	uint8_t bands[VISUALIZER_BANDS]; // -60..0 dBFS mapped to 0..255
	int16_t wave[VISUALIZER_POINTS];
} visualizer_frame_t;

// UI thread: enable only while the player is visible and the screen is on.
// Disabling discards all samples; the default is disabled.
void visualizer_enable(bool enabled);
// Audio thread: bounded copy only, no allocation, FFT, I/O or waiting for locks.
// PCM only. Never pass native DSD / DoP. Does not modify the source buffer.
void visualizer_feed(const void *pcm, int frames, int channels, int rate, int bits);
// Audio thread: suspend pictures for three seconds after output recovery.
void visualizer_backoff(void);
// UI thread: consumes the newest window, performing a fixed-size integer FFT
// only for spectrum mode. False means no NEW window (caller handles expiry).
bool visualizer_read(visualizer_mode_t mode, visualizer_frame_t *out);
#endif
