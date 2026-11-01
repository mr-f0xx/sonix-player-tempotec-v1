// test_eq.c -- the graphic EQ, PEQ and MSEB on PCM, on the host.
//
// Radio stations do not go through audio.c's decode loop: they decode on their
// own thread and write through audio_external_write(). The chain they share
// with a track from the card is eq_process() on stereo 16-bit frames, called
// from radio_push_pcm(). This suite drives that same function, so a station
// and a file cannot drift apart: if EQ, PEQ or MSEB stop changing samples
// here, they have stopped on the radio too.

#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

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

// Every rate a station can arrive at. 44.1 and 48 kHz are the usual MP3/AAC
// output; low-bitrate HE-AAC and speech stations decode at 22.05, 24 or
// 32 kHz. radio_push_pcm() hands every one to eq_process() with the station's
// own rate, so the chain has to run at all of them.
static const int radio_rates[] = {22050, 24000, 32000, 44100, 48000};

// Switches one module on with filters low enough to survive the slowest rate
// above -- the 31 Hz graphic band, a 100 Hz parametric low shelf and MSEB's
// bass0 (its 70 Hz low shelf). Called after all_off(), so only this module is
// live. The caller turns everything off again.
static void enable_one_module(int module) {
	if (module == 0) {
		eq_set_enabled(true);
		eq_set_band(0, 12); // 31 Hz, the slider's full travel
	} else if (module == 1) {
		peq_band_t band;
		peq_get_band(0, &band);
		band.on = true;
		band.type = PEQ_TYPE_LOWSHELF;
		band.freq = 100;
		band.gain_tenths = 120;
		band.q_cent = 100;
		peq_set_band(0, &band);
		peq_set_enabled(true);
	} else {
		mseb_set_enabled(true);
		mseb_set_value(1, 80); // bass0
	}
}

// Each of the three modules has to change the stereo PCM at every rate a
// station uses, and with all three off the same frames must come back
// untouched at every rate -- an untuned station reaches the card identical.
static void test_radio_rates(void) {
	static const char *const names[] = {"graphic EQ", "PEQ", "MSEB"};

	for (int module = 0; module < 3; module++) {
		for (unsigned i = 0; i < sizeof(radio_rates) / sizeof(radio_rates[0]); i++) {
			int rate = radio_rates[i];
			all_off();
			enable_one_module(module);

			short original[FRAMES * 2];
			short buf[FRAMES * 2];
			// 200 Hz sits inside each chosen filter and under every Nyquist.
			fill_sine(original, FRAMES, rate, 200, AMP);
			memcpy(buf, original, sizeof(buf));
			eq_process(buf, FRAMES, 2, rate);
			if (memcmp(buf, original, sizeof(buf)) == 0) {
				failures++;
				fprintf(stderr, "FAIL: %s changed nothing on a %d Hz station\n", names[module], rate);
			}
		}
	}

	for (unsigned i = 0; i < sizeof(radio_rates) / sizeof(radio_rates[0]); i++) {
		int rate = radio_rates[i];
		all_off();
		short original[FRAMES * 2];
		short buf[FRAMES * 2];
		fill_sine(original, FRAMES, rate, 200, AMP);
		memcpy(buf, original, sizeof(buf));
		eq_process(buf, FRAMES, 2, rate);
		CHECK(memcmp(buf, original, sizeof(buf)) == 0);
	}
	all_off();
}

// A mono station is widened into an identical stereo pair before the chain,
// because the card is always opened in two channels -- the channels == 1
// branch of radio_push_pcm(). The chain then has to leave it bit-identical
// with every module off, and tune both channels identically when one is on:
// a mono signal must not come back with its two sides drifting apart.
static void test_mono_station_widening(void) {
	const int rate = 44100;
	short mono[FRAMES];
	short widened[FRAMES * 2];
	short buf[FRAMES * 2];

	for (int i = 0; i < FRAMES; i++) {
		double s = sin(2.0 * 3.14159265358979323846 * 300.0 * (double)i / (double)rate);
		mono[i] = (short)(s * AMP);
	}
	// Same reverse-order copy radio_push_pcm() makes.
	for (int i = FRAMES - 1; i >= 0; i--) {
		widened[2 * i] = mono[i];
		widened[2 * i + 1] = mono[i];
	}

	all_off();
	memcpy(buf, widened, sizeof(buf));
	eq_process(buf, FRAMES, 2, rate);
	CHECK(memcmp(buf, widened, sizeof(buf)) == 0);

	all_off();
	eq_set_enabled(true);
	eq_set_band(0, 12);
	memcpy(buf, widened, sizeof(buf));
	eq_process(buf, FRAMES, 2, rate);
	CHECK(memcmp(buf, widened, sizeof(buf)) != 0);
	for (int i = 0; i < FRAMES; i++) {
		CHECK(buf[2 * i] == buf[2 * i + 1]);
	}
	all_off();
}

// Two headphones, two complete curves. A switch must carry the enable state
// and preamp as well as the bands; neither profile may edit the other one.
static void set_profile_curve(int frequency, int gain, int preamp, bool enabled) {
	peq_reset();
	peq_band_t b;
	peq_get_band(0, &b);
	b.on = true;
	b.type = PEQ_TYPE_LOWSHELF;
	b.freq = frequency;
	b.gain_tenths = gain;
	b.q_cent = 71;
	peq_set_band(0, &b);
	peq_set_preamp(preamp);
	peq_set_enabled(enabled);
}

static void check_profile_curve(int frequency, int gain, int preamp, bool enabled) {
	peq_band_t b;
	peq_get_band(0, &b);
	CHECK(b.on);
	CHECK(b.type == PEQ_TYPE_LOWSHELF);
	CHECK(b.freq == frequency);
	CHECK(b.gain_tenths == gain);
	CHECK(b.q_cent == 71);
	CHECK(peq_get_preamp() == preamp);
	CHECK(peq_get_enabled() == enabled);
	for (int i = 1; i < PEQ_BANDS; i++) {
		peq_get_band(i, &b);
		CHECK(!b.on);
	}
}

static void test_peq_profiles(const char *dir) {
	all_off();
	CHECK(peq_get_profile() == 1);
	CHECK(!peq_select_profile(0));
	CHECK(!peq_select_profile(PEQ_PROFILE_COUNT + 1));
	CHECK(!peq_select_profile(1));
	set_profile_curve(105, 65, -62, true);
	CHECK(peq_next_profile() == 2);
	CHECK(!peq_get_enabled());
	CHECK(peq_get_preamp() == 0);
	for (int i = 0; i < PEQ_BANDS; i++) {
		peq_band_t b;
		peq_get_band(i, &b);
		CHECK(!b.on);
	}
	set_profile_curve(300, -45, -20, false);
	CHECK(peq_next_profile() == 1);
	check_profile_curve(105, 65, -62, true);

	// A preset load and a full reset are scoped to the selected slot.
	char preset_dir[512];
	snprintf(preset_dir, sizeof(preset_dir), "%s/PEQ", dir);
	mkdir(preset_dir, 0777);
	char path[700];
	snprintf(path, sizeof(path), "%s/curve.txt", preset_dir);
	FILE *f = fopen(path, "w");
	CHECK(f != NULL);
	if (f) {
		fputs("Preamp: -3.5 dB\nFilter 1: ON PK Fc 1000 Hz Gain 4.0 dB Q 0.70\n", f);
		fclose(f);
	}
	peq_presets_set_dir(dir);
	int ignored = -1;
	CHECK(peq_preset_load("curve", &ignored));
	CHECK(ignored == 0);
	CHECK(peq_get_enabled()); // importing does not change the enable state
	CHECK(peq_get_preamp() == -35);
	peq_band_t imported;
	peq_get_band(0, &imported);
	CHECK(imported.on && imported.freq == 1000 && imported.gain_tenths == 40);
	CHECK(peq_preset_save("exported"));
	CHECK(config_save());
	peq_reset();
	CHECK(peq_get_preamp() == 0);
	CHECK(peq_next_profile() == 2);
	check_profile_curve(300, -45, -20, false);

	// Restart on Profile 2, then once more on Profile 1. The legacy [peq]
	// section remains the active mirror, not a third, divergent profile.
	char config_path[512];
	snprintf(config_path, sizeof(config_path), "%s/device_config.ini", dir);
	config_init(config_path);
	eq_init();
	CHECK(peq_get_profile() == 2);
	check_profile_curve(300, -45, -20, false);
	CHECK(peq_next_profile() == 1);
	CHECK(peq_get_enabled());
	CHECK(peq_get_preamp() == 0);
	peq_get_band(0, &imported);
	CHECK(!imported.on);
	CHECK(peq_preset_load("exported", NULL));
	CHECK(config_save());
	config_init(config_path);
	eq_init();
	CHECK(peq_get_profile() == 1);
	CHECK(peq_get_preamp() == -35);
	peq_get_band(0, &imported);
	CHECK(imported.on && imported.freq == 1000 && imported.gain_tenths == 40);

	// The actual PCM path, including a switch from enabled to disabled.
	set_profile_curve(100, 120, -120, true);
	peq_select_profile(2);
	set_profile_curve(800, -100, 30, true);
	int profile_two_response = peq_response_tenths(1000, RATE);
	short original[FRAMES * 2], first[FRAMES * 2], second[FRAMES * 2];
	fill_sine(original, FRAMES, RATE, 1000, AMP);
	memcpy(first, original, sizeof(first));
	eq_process(first, FRAMES, 2, RATE);
	peq_select_profile(1);
	memcpy(second, original, sizeof(second));
	eq_process(second, FRAMES, 2, RATE);
	CHECK(memcmp(first, second, sizeof(first)) != 0);
	CHECK(memcmp(second, original, sizeof(second)) != 0);
	peq_set_enabled(false);
	memcpy(second, original, sizeof(second));
	eq_process(second, FRAMES, 2, RATE);
	CHECK(memcmp(second, original, sizeof(second)) == 0);
	peq_select_profile(2);
	CHECK(peq_get_enabled());
	memcpy(second, original, sizeof(second));
	eq_process(second, FRAMES, 2, RATE);
	CHECK(memcmp(second, original, sizeof(second)) != 0);
	// Filter history and the intentional preamp glide belong to the running
	// chain, so two visits need not produce identical transients. The settled
	// response must return to the same curve, not the other profile's curve.
	CHECK(peq_response_tenths(1000, RATE) == profile_two_response);
	all_off();
}

static void test_legacy_peq_upgrade(const char *dir) {
	char path[512];
	snprintf(path, sizeof(path), "%s/legacy.ini", dir);
	FILE *f = fopen(path, "w");
	CHECK(f != NULL);
	if (!f) {
		return;
	}
	fputs("[peq]\nenabled=1\npreamp=-42\nb0_on=1\nb0_type=1\nb0_freq=105\nb0_gain=65\nb0_q=71\n"
		  "[eq]\nenabled=1\nband0=3\n[mseb]\nenabled=1\nv0=20\n", f);
	fclose(f);
	config_init(path);
	eq_init();
	CHECK(peq_get_profile() == 1);
	check_profile_curve(105, 65, -42, true);
	CHECK(eq_get_enabled() && eq_get_band(0) == 3);
	CHECK(mseb_get_enabled() && mseb_get_value(0) == 20);
	peq_select_profile(2);
	CHECK(!peq_get_enabled());
	CHECK(eq_get_enabled() && eq_get_band(0) == 3);
	CHECK(mseb_get_enabled() && mseb_get_value(0) == 20);
	peq_select_profile(1);
	check_profile_curve(105, 65, -42, true);
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
	test_mono_station_widening();
	test_peq_profiles(dir);
	test_legacy_peq_upgrade(dir);

	if (failures) {
		fprintf(stderr, "%d check(s) failed\n", failures);
		return 1;
	}
	printf("eq: PCM, radio rates, mono, persistent PEQ profiles, scoped presets/reset and legacy upgrade passed\n");
	return 0;
}
