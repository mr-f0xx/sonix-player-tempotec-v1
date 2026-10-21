// test_eq.c -- the graphic EQ, PEQ and MSEB on PCM, on the host.
//
// Radio stations do not go through audio.c's decode loop: they decode on their
// own thread and write through audio_external_write(). The chain they share
// with a track from the card is eq_process() on stereo 16-bit frames, called
// from radio_push_pcm(). This suite drives that same function, so a station
// and a file cannot drift apart: if EQ, PEQ or MSEB stop changing samples
// here, they have stopped on the radio too.
//
// USB DAC mode does not go through that loop either: the computer's PCM comes
// off /dev/uac_sa in 32-bit words and usbdac.c puts it through
// eq_process_s32() on its way to the DAC. That entry point is driven here too,
// so the curve set on the player cannot sound different over the cable than it
// does from the card.

#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "src/system/audio/eq.h"
#include "src/system/core/config.h"

static int failures;

#define CHECK(cond)                                                                             \
	do {                                                                                        \
		if (!(cond)) {                                                                          \
			failures++;                                                                     \
			fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                 \
		}                                                                                       \
	} while (0)

#define FRAMES 1024
#define RATE 44100
#define AMP 8000

static void fill_sine(short *pcm, int frames, int rate, int freq, int amp) {
	for (int i = 0; i < frames; i++) {
		double s = sin(2.0 * 3.14159265358979323846 * (double)freq * (double)i / (double)rate);
		short v = (short)(s * amp);
		pcm[2 * i] = v;
		pcm[2 * i + 1] = v;
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

static void test_off_is_a_no_op(void) {
	all_off();
	short original[FRAMES * 2];
	short buf[FRAMES * 2];
	fill_sine(original, FRAMES, RATE, 440, AMP);
	memcpy(buf, original, sizeof(buf));
	eq_process(buf, FRAMES, 2, RATE);
	CHECK(memcmp(buf, original, sizeof(buf)) == 0);
}

static void test_graphic_eq_changes_samples(void) {
	all_off();
	eq_set_enabled(true);
	eq_set_band(5, 12); // 1 kHz, the most a slider travels

	short original[FRAMES * 2];
	short buf[FRAMES * 2];
	fill_sine(original, FRAMES, RATE, 1000, AMP);
	memcpy(buf, original, sizeof(buf));
	eq_process(buf, FRAMES, 2, RATE);
	CHECK(memcmp(buf, original, sizeof(buf)) != 0);

	all_off();
	memcpy(buf, original, sizeof(buf));
	eq_process(buf, FRAMES, 2, RATE);
	CHECK(memcmp(buf, original, sizeof(buf)) == 0);
}

static void test_peq_changes_samples(void) {
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

	short original[FRAMES * 2];
	short buf[FRAMES * 2];
	fill_sine(original, FRAMES, RATE, 1000, AMP);
	memcpy(buf, original, sizeof(buf));
	eq_process(buf, FRAMES, 2, RATE);
	CHECK(memcmp(buf, original, sizeof(buf)) != 0);

	all_off();
	memcpy(buf, original, sizeof(buf));
	eq_process(buf, FRAMES, 2, RATE);
	CHECK(memcmp(buf, original, sizeof(buf)) == 0);
}

static void test_mseb_changes_samples(void) {
	all_off();
	mseb_set_enabled(true);
	mseb_set_value(0, 80); // temperature, well off flat

	short original[FRAMES * 2];
	short buf[FRAMES * 2];
	fill_sine(original, FRAMES, RATE, 440, AMP);
	memcpy(buf, original, sizeof(buf));
	eq_process(buf, FRAMES, 2, RATE);
	CHECK(memcmp(buf, original, sizeof(buf)) != 0);

	all_off();
	memcpy(buf, original, sizeof(buf));
	eq_process(buf, FRAMES, 2, RATE);
	CHECK(memcmp(buf, original, sizeof(buf)) == 0);
}

// Radio stations arrive at 44.1 or 48 kHz, stereo, 16-bit -- the arguments
// radio_push_pcm() hands eq_process(). Both rates have to run.
static void test_radio_rates(void) {
	all_off();
	eq_set_enabled(true);
	eq_set_band(4, 8);

	static const int rates[] = {44100, 48000};
	for (unsigned i = 0; i < sizeof(rates) / sizeof(rates[0]); i++) {
		short original[FRAMES * 2];
		short buf[FRAMES * 2];
		fill_sine(original, FRAMES, rates[i], 500, AMP);
		memcpy(buf, original, sizeof(buf));
		eq_process(buf, FRAMES, 2, rates[i]);
		CHECK(memcmp(buf, original, sizeof(buf)) != 0);
	}
	all_off();
}

// ---------------------------------------------------------------------------
// The 32-bit entry point USB DAC mode uses
//
// A computer streaming through the UAC node hands over 32-bit words with the
// sample left-justified. The card's hi-res path feeds the chain the same shape,
// and the two entry points share one set of filters -- so a tone has to come
// out of a word at the level it comes out of a 16-bit frame.
// ---------------------------------------------------------------------------

static void fill_sine_s32(int32_t *pcm, int frames, int rate, int freq, int amp) {
	for (int i = 0; i < frames; i++) {
		double s = sin(2.0 * 3.14159265358979323846 * (double)freq * (double)i / (double)rate);
		int32_t v = (int32_t)(s * (double)amp);
		pcm[2 * i] = v * 65536;		// the sample in the top 16 bits of the word
		pcm[2 * i + 1] = v * 65536;
	}
}

static double rms16(const short *pcm, int from, int frames) {
	double sum = 0;
	for (int i = from; i < from + frames; i++) {
		double l = (double)pcm[2 * i], r = (double)pcm[2 * i + 1];
		sum += l * l + r * r;
	}
	return sqrt(sum / (double)(frames * 2));
}

static double rms32(const int32_t *pcm, int from, int frames) {
	double sum = 0;
	for (int i = from; i < from + frames; i++) {
		double l = (double)pcm[2 * i] / 65536.0, r = (double)pcm[2 * i + 1] / 65536.0;
		sum += l * l + r * r;
	}
	return sqrt(sum / (double)(frames * 2));
}

static void test_dac_off_is_a_no_op(void) {
	all_off();
	static int32_t original[FRAMES * 2];
	static int32_t buf[FRAMES * 2];
	fill_sine_s32(original, FRAMES, RATE, 440, AMP);
	memcpy(buf, original, sizeof(buf));
	eq_process_s32(buf, FRAMES, 2, RATE);
	CHECK(memcmp(buf, original, sizeof(buf)) == 0);
}

// Every rate the USB DAC answers to, with a PEQ band boosted and MSEB well off
// flat. Long enough that the pre-gain ramp and every filter's start-up
// transient are behind the window the two are compared over -- a 20 Hz shelf
// rings for tens of milliseconds, and the host's 384 kHz is a lot of frames to
// the second.
#define DAC_FRAMES 65536
#define DAC_TAIL (DAC_FRAMES / 4)

static void test_dac_path_shapes_like_the_card(void) {
	all_off();
	peq_band_t band;
	peq_get_band(0, &band);
	band.on = true;
	band.type = PEQ_TYPE_PEAK;
	band.freq = 1000;
	band.gain_tenths = 60;
	band.q_cent = 100;
	peq_set_band(0, &band);
	peq_set_enabled(true);
	mseb_set_enabled(true);
	mseb_set_value(0, 40); // temperature, well off flat

	static short original[DAC_FRAMES * 2];
	static short pcm16[DAC_FRAMES * 2];
	static int32_t original32[DAC_FRAMES * 2];
	static int32_t pcm32[DAC_FRAMES * 2];

	static const int rates[] = {44100, 48000, 96000, 192000, 384000};
	for (unsigned r = 0; r < sizeof(rates) / sizeof(rates[0]); r++) {
		fill_sine(original, DAC_FRAMES, rates[r], 1000, AMP);
		memcpy(pcm16, original, sizeof(pcm16));
		fill_sine_s32(original32, DAC_FRAMES, rates[r], 1000, AMP);
		memcpy(pcm32, original32, sizeof(pcm32));

		eq_process(pcm16, DAC_FRAMES, 2, rates[r]);
		eq_process_s32(pcm32, DAC_FRAMES, 2, rates[r]);

		// The chain did something at all, through either door...
		CHECK(memcmp(pcm16, original, sizeof(pcm16)) != 0);
		CHECK(memcmp(pcm32, original32, sizeof(pcm32)) != 0);

		// ...and it did the same thing to a word as to a frame: the tone comes
		// out at the same level through both entry points, to well inside a
		// decibel. A wrongly scaled word would be tens of decibels apart.
		double card = rms16(pcm16, DAC_FRAMES - DAC_TAIL, DAC_TAIL);
		double dac = rms32(pcm32, DAC_FRAMES - DAC_TAIL, DAC_TAIL);
		CHECK(card > 0.0 && dac > 0.0);
		CHECK(fabs(card - dac) <= card * 0.06); // 0.5 dB
	}
	all_off();
}

int main(void) {
	char dir[] = "/tmp/sonix-eq-test-XXXXXX";
	if (!mkdtemp(dir)) {
		perror("mkdtemp");
		return 1;
	}
	char path[256];
	snprintf(path, sizeof(path), "%s/device_config.ini", dir);
	// A missing file is not an error: the path is still remembered and the
	// defaults are used until the first save.
	config_init(path);
	eq_init();

	test_off_is_a_no_op();
	test_graphic_eq_changes_samples();
	test_peq_changes_samples();
	test_mseb_changes_samples();
	test_radio_rates();
	test_dac_off_is_a_no_op();
	test_dac_path_shapes_like_the_card();

	if (failures) {
		fprintf(stderr, "%d check(s) failed\n", failures);
		return 1;
	}
	printf("eq: graphic EQ, PEQ and MSEB run on stereo PCM, radio and USB DAC mode\n");
	return 0;
}
