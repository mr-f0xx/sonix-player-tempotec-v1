// The USB DAC input path's PCM effects, exercised against the real DSP engine.
// DSD must remain a bitstream; PCM must receive the same graphic EQ / PEQ / MSEB
// chain as decoded audio.
#define _GNU_SOURCE 1
#include "../src/system/audio/usbdac.c"

#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "src/system/audio/eq.h"
#include "src/system/core/config.h"

#define FRAMES 2048
#define RATE 44100
#define AMP 8000

static void fill_sine_s32(int32_t *pcm, int frames, int rate, int freq, int amp) {
	for (int i = 0; i < frames; i++) {
		double sample = sin(2.0 * 3.14159265358979323846 * (double)freq * (double)i / (double)rate);
		int32_t value = (int32_t)((short)(sample * amp)) * 65536;
		pcm[2 * i] = value;
		pcm[2 * i + 1] = value;
	}
}

static void all_off(void) {
	eq_set_enabled(false);
	peq_set_enabled(false);
	mseb_set_enabled(false);
	eq_reset();
	peq_reset();
	mseb_reset();
}

static void check_pcm_effect(int32_t *original, int32_t *pcm, int frequency) {
	fill_sine_s32(original, FRAMES, RATE, frequency, AMP);
	memcpy(pcm, original, sizeof(int32_t) * FRAMES * 2);
	process_usb_dac_audio(pcm, FRAMES, RATE, false);
	assert(memcmp(pcm, original, sizeof(int32_t) * FRAMES * 2) != 0);
}

int main(void) {
	char dir[] = "/tmp/sonix-usbdac-eq-XXXXXX";
	assert(mkdtemp(dir));
	char path[512];
	snprintf(path, sizeof(path), "%s/config.ini", dir);
	config_init(path); // a missing file is the normal, all-default starting point
	eq_init();

	int32_t original[FRAMES * 2];
	int32_t pcm[FRAMES * 2];

	// Each part of the shared chain is applied to the signed 32-bit PCM the
	// UAC driver supplies, not just to the player's 16-bit decoded path.
	all_off();
	eq_set_band(5, 12); // 1 kHz, full graphic-EQ boost
	eq_set_enabled(true);
	check_pcm_effect(original, pcm, 1000);

	all_off();
	peq_band_t band;
	peq_get_band(0, &band);
	band.on = true;
	band.type = PEQ_TYPE_PEAK;
	band.freq = 1000;
	band.gain_tenths = 120;
	band.q_cent = 100;
	peq_set_band(0, &band);
	peq_set_enabled(true);
	check_pcm_effect(original, pcm, 1000);

	all_off();
	mseb_set_value(0, 80);
	mseb_set_enabled(true);
	check_pcm_effect(original, pcm, 440);

	// DSD shares the USB input node but is not PCM. Even with all three effects
	// enabled, preserve every bit for the DAC's native DSD handling.
	eq_set_band(5, 12);
	eq_set_enabled(true);
	peq_set_enabled(true);
	mseb_set_enabled(true);
	for (int i = 0; i < FRAMES * 2; i++) {
		pcm[i] = (i & 1) ? INT32_C(0x55555555) : -INT32_C(1431655766);
	}
	memcpy(original, pcm, sizeof(pcm));
	process_usb_dac_audio(pcm, FRAMES, RATE, true);
	assert(memcmp(pcm, original, sizeof(pcm)) == 0);

	// With the effects off, PCM remains an exact no-op too.
	all_off();
	fill_sine_s32(original, FRAMES, RATE, 1000, AMP);
	memcpy(pcm, original, sizeof(pcm));
	process_usb_dac_audio(pcm, FRAMES, RATE, false);
	assert(memcmp(pcm, original, sizeof(pcm)) == 0);

	printf("usbdac: GEQ, PEQ and MSEB applied to PCM; DSD left untouched\n");
	return 0;
}
