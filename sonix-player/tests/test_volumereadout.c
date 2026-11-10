// test_volumereadout.c -- the volume read as a voltage, on the host.
//
// Settings > Screen > "Volume as voltage" prints what the socket puts out
// ("502mV") where the status bar and the volume bar print the index ("88").
// Nothing here can say whether the V1 really puts 2 V on its 3.5 mm jack at
// the top of the high gain table -- that takes a meter -- but everything
// between that published figure and the text on the glass is arithmetic, and
// arithmetic can be checked:
//
//   - the half-dB table against the logarithm it stands in for;
//   - the three-figure text against the value it was made from, across the
//     whole of the DAC's range, and at the edges where rounding gains a digit;
//   - the reading at every index of BOTH gain tables, which must rise with
//     every step and never print the same thing twice running;
//   - what is and is not in the figure: the gain switch and the socket are,
//     the DSD compensation is only while a DSD track runs, and Bluetooth and
//     USB-C get the plain number back;
//   - that the figure is the one the DAC was given: the real routine that
//     writes the two channel controls runs against a mixer which only
//     remembers what it was told, and the reading has to be that attenuation
//     and no other, at every index, gain and DSD setting;
//   - that a label measured against volumereadout_sample() has been measured
//     against every reading it will ever be given.
//
// The gain tables, the DSD offsets and the output profile are the real ones:
// alsa-controls.c is compiled in, against the declarations-only ALSA header the
// USB tests use. Its state is set by writing its statics, which including the
// file makes visible: the setters do a good deal besides (Bluetooth, the USB
// DAC, the software volume, a log line a step) that has no part in this. The
// mixer is the seven calls alsa_set_control() makes, kept below; every other
// function of the file that would have reached one is dropped by the linker.
// The config is faked as two integers that can be read back.

#include <limits.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ---------------------------------------------------------------------------
// The config: [screen] volume_voltage and volume_voltage_jack, and a count of
// who asked and who saved.
// ---------------------------------------------------------------------------

#define UNSET LONG_MIN
static long stored_enabled = UNSET;
static long stored_jack = UNSET;
static int config_reads;
static int config_saves;
static int config_strays; // anything asked of the config that is not those two

static long *stored_slot(const char *section, const char *key) {
	if (strcmp(section, "screen") == 0 && strcmp(key, "volume_voltage") == 0) {
		return &stored_enabled;
	}
	if (strcmp(section, "screen") == 0 && strcmp(key, "volume_voltage_jack") == 0) {
		return &stored_jack;
	}
	config_strays++;
	return NULL;
}

long config_get_int(const char *section, const char *key, long fallback) {
	config_reads++;
	long *slot = stored_slot(section, key);
	return slot && *slot != UNSET ? *slot : fallback;
}

void config_set_int(const char *section, const char *key, long value) {
	long *slot = stored_slot(section, key);
	if (slot) {
		*slot = value;
	}
}

bool config_save(void) {
	config_saves++;
	return true;
}

// ---------------------------------------------------------------------------
// The mixer: what alsa_set_control() needs of ALSA, and no card behind it. It
// keeps the last value each of the DAC's two channel controls was written, and
// counts the writes -- to those two, and to anything else.
// ---------------------------------------------------------------------------

struct snd_ctl {
	int unused;
};
typedef struct snd_ctl snd_ctl_t;

static const char *mixer_name; // the control being addressed
static long mixer_value;	   // and what it is about to be given
static long dac_left = -1;
static long dac_right = -1;
static int dac_left_writes;
static int dac_right_writes;
static int mixer_strays;

int snd_ctl_open(snd_ctl_t **ctl, const char *name, int mode) {
	(void)name;
	(void)mode;
	static struct snd_ctl card;
	*ctl = &card;
	return 0;
}

int snd_ctl_close(snd_ctl_t *ctl) {
	(void)ctl;
	return 0;
}

void snd_ctl_elem_id_set_interface(void *id, int interface) {
	(void)id;
	(void)interface;
}

void snd_ctl_elem_id_set_name(void *id, const char *name) {
	(void)id;
	mixer_name = name;
}

void snd_ctl_elem_value_set_id(void *value, void *id) {
	(void)value;
	(void)id;
}

void snd_ctl_elem_value_set_integer(void *value, unsigned int channel, long level) {
	(void)value;
	(void)channel;
	mixer_value = level;
}

int snd_ctl_elem_write(snd_ctl_t *ctl, void *value) {
	(void)ctl;
	(void)value;
	if (mixer_name && strcmp(mixer_name, "Left Playback Volume") == 0) {
		dac_left = mixer_value;
		dac_left_writes++;
	} else if (mixer_name && strcmp(mixer_name, "Right Playback Volume") == 0) {
		dac_right = mixer_value;
		dac_right_writes++;
	} else {
		mixer_strays++;
	}
	return 0;
}

#include "../src/system/audio/alsa-controls.c"

#include "src/gui/shell/volumereadout.h"

static int failures;

#define CHECK(cond)                                                         \
	do {                                                                    \
		if (!(cond)) {                                                        \
			failures++;                                                        \
			fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);     \
		}                                                                     \
	} while (0)

#define CHECK_TEXT(got, want)                                                                 \
	do {                                                                                      \
		if (strcmp((got), (want)) != 0) {                                                       \
			failures++;                                                                          \
			fprintf(stderr, "FAIL %s:%d: got \"%s\", want \"%s\"\n", __FILE__, __LINE__, (got), (want)); \
		}                                                                                       \
	} while (0)

#define MICRO "\xC2\xB5" // U+00B5, as volumereadout.c writes it

// ---------------------------------------------------------------------------
// The player's state, set the way the test header says
// ---------------------------------------------------------------------------

static void player_state(int gain_high, volume_output_t output, int dop, int dsd_index) {
	high_gain = gain_high;
	profile_out = output;
	current_dop = dop;
	dsd_gain_index = dsd_index;
}

static const char *reading(int percent) {
	static char text[VOLUMEREADOUT_TEXT_MAX];
	memset(text, '#', sizeof(text));
	volumereadout_text(percent, text, sizeof(text));
	return text;
}

// The text taken apart again: the figure, how many digits and decimals it was
// written with, and the size of its unit in volts. False for anything that is
// not a figure followed by V, mV or the micro sign and V.
static bool parse_reading(const char *text, double *figure, int *digits, int *decimals, double *unit) {
	const char *p = text;
	*digits = 0;
	*decimals = -1;
	while ((*p >= '0' && *p <= '9') || *p == '.') {
		if (*p == '.') {
			if (*decimals >= 0) {
				return false; // two points
			}
			*decimals = 0;
		} else {
			(*digits)++;
			if (*decimals >= 0) {
				(*decimals)++;
			}
		}
		p++;
	}
	if (*decimals < 0) {
		*decimals = 0;
	}
	if (*digits == 0) {
		return false;
	}
	*figure = strtod(text, NULL);
	if (strcmp(p, "V") == 0) {
		*unit = 1.0;
	} else if (strcmp(p, "mV") == 0) {
		*unit = 1e-3;
	} else if (strcmp(p, MICRO "V") == 0) {
		*unit = 1e-6;
	} else {
		return false;
	}
	return true;
}

// Characters as the label draws them: the micro sign is two bytes and one
// glyph.
static int glyph_count(const char *text) {
	int n = 0;
	for (const unsigned char *p = (const unsigned char *)text; *p; p++) {
		if ((*p & 0xC0) != 0x80) {
			n++;
		}
	}
	return n;
}

// ---------------------------------------------------------------------------
// First, before anything has asked: what a player that has never seen the
// setting comes up with, and what a hand-edited config is taken for.
// ---------------------------------------------------------------------------

static void test_off_until_asked_for(void) {
	stored_enabled = UNSET;
	stored_jack = 7; // not a socket
	player_state(1, VOLUME_OUTPUT_PHONES, 0, 0);

	CHECK(!volumereadout_enabled());
	CHECK(volumereadout_jack() == VOLUMEREADOUT_JACK_PHONES);
	CHECK(volumereadout_form() == VOLUMEREADOUT_FORM_INDEX);

	// Off, it is the number the bar has always printed, clamped the way the
	// bar clamps it.
	for (int percent = 0; percent <= 100; percent++) {
		char want[8];
		snprintf(want, sizeof(want), "%d", percent);
		CHECK_TEXT(reading(percent), want);
	}
	CHECK_TEXT(reading(-5), "0");
	CHECK_TEXT(reading(250), "100");

	// And the label is measured against exactly those: 0 to 100, no more.
	char text[VOLUMEREADOUT_TEXT_MAX];
	int count = 0;
	while (volumereadout_sample(count, text, sizeof(text))) {
		char want[16];
		snprintf(want, sizeof(want), "%d", count);
		CHECK_TEXT(text, want);
		count++;
	}
	CHECK(count == 101);
	CHECK(!volumereadout_sample(-1, text, sizeof(text)));

	// The two settings were read once each and are not read again: the reading
	// is worked out on every step of a held key.
	CHECK(config_reads == 2);
	CHECK(config_saves == 0);
}

static void test_settings_are_saved(void) {
	int saves = config_saves;
	volumereadout_set_enabled(true);
	CHECK(volumereadout_enabled());
	CHECK(stored_enabled == 1);
	CHECK(config_saves == saves + 1);

	volumereadout_set_jack(VOLUMEREADOUT_JACK_BALANCED);
	CHECK(volumereadout_jack() == VOLUMEREADOUT_JACK_BALANCED);
	CHECK(stored_jack == 1);
	CHECK(config_saves == saves + 2);

	// Not a socket: the 3.5 mm one, never an index past the table of full
	// scales.
	volumereadout_set_jack((volumereadout_jack_t)9);
	CHECK(volumereadout_jack() == VOLUMEREADOUT_JACK_PHONES);
	CHECK(stored_jack == 0);

	volumereadout_set_enabled(false);
	CHECK(!volumereadout_enabled());
	CHECK(stored_enabled == 0);

	CHECK(config_reads == 2); // still the two from the first look
	CHECK(config_strays == 0);
}

// ---------------------------------------------------------------------------
// The arithmetic
// ---------------------------------------------------------------------------

static void test_table_is_the_logarithm(void) {
	// Every step the DAC's register can hold, against 10^(dB/20) worked out
	// the long way. The table is nine figures, so on four volts it may be two
	// nanovolts out -- half a part in a thousand million.
	static const uint32_t FULLSCALE[] = {VOLUMEREADOUT_FULLSCALE_PHONES_MV, VOLUMEREADOUT_FULLSCALE_BALANCED_MV};
	for (unsigned f = 0; f < 2; f++) {
		for (int step = 0; step < 255; step++) {
			uint64_t got = volumereadout_nanovolts(-(long)step * 50, FULLSCALE[f]);
			double want = FULLSCALE[f] * 1e6 * pow(10.0, -step / 40.0);
			if (fabs((double)got - want) > 2.5) {
				failures++;
				fprintf(stderr, "FAIL %s:%d: %u mV at -%.1f dB: got %llu nV, want %.1f\n", __FILE__, __LINE__,
						FULLSCALE[f], step * 0.5, (unsigned long long)got, want);
			}
		}
	}

	// The decades are exact, which is the point of counting them.
	CHECK(volumereadout_nanovolts(0, 2000) == UINT64_C(2000000000));
	CHECK(volumereadout_nanovolts(-2000, 2000) == UINT64_C(200000000));
	CHECK(volumereadout_nanovolts(-4000, 4000) == UINT64_C(40000000));
	CHECK(volumereadout_nanovolts(-12000, 2000) == UINT64_C(2000));

	// Six dB is very nearly a half and is not one: 10^-0.3. A decade further
	// down the same figure has a digit to lose, and loses it by rounding:
	// 100 237 446.7 nV.
	CHECK(volumereadout_nanovolts(-600, 2000) == UINT64_C(1002374468));
	CHECK(volumereadout_nanovolts(-2600, 2000) == UINT64_C(100237447));

	// To the DAC's half-dB step, to the nearest.
	CHECK(volumereadout_nanovolts(-24, 2000) == volumereadout_nanovolts(0, 2000));
	CHECK(volumereadout_nanovolts(-25, 2000) == volumereadout_nanovolts(-50, 2000));
	CHECK(volumereadout_nanovolts(-74, 2000) == volumereadout_nanovolts(-50, 2000));
	CHECK(volumereadout_nanovolts(-75, 2000) == volumereadout_nanovolts(-100, 2000));

	// Nothing above full scale, whatever is asked: a gain over 0 dB is not a
	// step back up the table from its top.
	for (long gain = 1; gain <= 1300; gain++) {
		CHECK(volumereadout_nanovolts(gain, 2000) == UINT64_C(2000000000));
	}
	CHECK(volumereadout_nanovolts(LONG_MAX, 4000) == UINT64_C(4000000000));

	// And nothing at all at the mute, or anywhere on the way down to it --
	// with the largest full scale there is, so that a divisor which wrapped
	// (or came to nought, as ten to the hundredth does in sixty-four bits)
	// would show.
	CHECK(volumereadout_nanovolts(ALSA_VOLUME_MUTE_DB100, 2000) == 0);
	CHECK(volumereadout_nanovolts(ALSA_VOLUME_MUTE_DB100, UINT32_MAX) == 0);
	CHECK(volumereadout_nanovolts(LONG_MIN, UINT32_MAX) == 0);
	CHECK(volumereadout_nanovolts(-30000, UINT32_MAX) == 4); // 4.29e6 V, 300 dB down
	CHECK(volumereadout_nanovolts(-31850, UINT32_MAX) == 1); // the last of it
	for (long gain = -31900; gain >= -400000; gain -= 50) {
		CHECK(volumereadout_nanovolts(gain, UINT32_MAX) == 0);
	}
	CHECK(volumereadout_nanovolts(-1, 0) == 0);
}

static const char *formatted(uint64_t nanovolts) {
	static char text[VOLUMEREADOUT_TEXT_MAX];
	memset(text, '#', sizeof(text));
	volumereadout_format(nanovolts, text, sizeof(text));
	return text;
}

static void test_three_figures_and_their_edges(void) {
	CHECK_TEXT(formatted(0), "0V");
	CHECK_TEXT(formatted(UINT64_C(4000000000)), "4.00V");
	CHECK_TEXT(formatted(UINT64_C(2000000000)), "2.00V");
	CHECK_TEXT(formatted(UINT64_C(1264911064)), "1.26V");
	CHECK_TEXT(formatted(UINT64_C(632455532)), "632mV");
	CHECK_TEXT(formatted(UINT64_C(84339000)), "84.3mV");
	CHECK_TEXT(formatted(UINT64_C(8433900)), "8.43mV");
	CHECK_TEXT(formatted(UINT64_C(843390)), "843" MICRO "V");
	CHECK_TEXT(formatted(UINT64_C(39905)), "39.9" MICRO "V");
	CHECK_TEXT(formatted(UINT64_C(1500)), "1.50" MICRO "V");

	// Where rounding carries: one nanovolt either side of each edge, in each
	// unit. The figure never comes out with four digits or as 1000 of a unit.
	CHECK_TEXT(formatted(UINT64_C(9994999999)), "9.99V");
	CHECK_TEXT(formatted(UINT64_C(9995000000)), "10.0V");
	CHECK_TEXT(formatted(UINT64_C(99949999999)), "99.9V");
	CHECK_TEXT(formatted(UINT64_C(99950000000)), "100V");

	CHECK_TEXT(formatted(UINT64_C(999499999)), "999mV");
	CHECK_TEXT(formatted(UINT64_C(999500000)), "1.00V");
	CHECK_TEXT(formatted(UINT64_C(9994999)), "9.99mV");
	CHECK_TEXT(formatted(UINT64_C(9995000)), "10.0mV");
	CHECK_TEXT(formatted(UINT64_C(99949999)), "99.9mV");
	CHECK_TEXT(formatted(UINT64_C(99950000)), "100mV");

	CHECK_TEXT(formatted(UINT64_C(999499)), "999" MICRO "V");
	CHECK_TEXT(formatted(UINT64_C(999500)), "1.00mV");
	CHECK_TEXT(formatted(UINT64_C(9994)), "9.99" MICRO "V");
	CHECK_TEXT(formatted(UINT64_C(9995)), "10.0" MICRO "V");
	CHECK_TEXT(formatted(UINT64_C(99949)), "99.9" MICRO "V");
	CHECK_TEXT(formatted(UINT64_C(99950)), "100" MICRO "V");

	// Under a microvolt there is no smaller unit to move to. Nothing the gain
	// tables reach is down here; the DAC's register is.
	CHECK_TEXT(formatted(1000), "1.00" MICRO "V");
	CHECK_TEXT(formatted(999), "1.00" MICRO "V");
	CHECK_TEXT(formatted(893), "0.89" MICRO "V");
	CHECK_TEXT(formatted(1), "0.00" MICRO "V");

	// Out of any socket's reach: held, not wrapped.
	CHECK_TEXT(formatted(UINT64_C(999000000000)), "999V");
	CHECK_TEXT(formatted(UINT64_C(999000000001)), "999V");
	CHECK_TEXT(formatted(UINT64_C(5000000000000)), "999V");
	CHECK_TEXT(formatted(UINT64_MAX), "999V");

	// A buffer too short gets what fits, terminated; none gets left alone.
	char tiny[4] = "###";
	volumereadout_format(UINT64_C(84339000), tiny, sizeof(tiny));
	CHECK_TEXT(tiny, "84.");
	char one[1] = {'#'};
	volumereadout_format(UINT64_C(84339000), one, sizeof(one));
	CHECK(one[0] == '\0');
	volumereadout_format(UINT64_C(84339000), NULL, 8);
	volumereadout_format(UINT64_C(84339000), tiny, 0);
	CHECK_TEXT(tiny, "84.");
}

static void test_text_is_the_value_to_three_figures(void) {
	// Across everything the register holds, on both sockets: the text read
	// back is the level it was made from, to within half of its last digit.
	static const uint32_t FULLSCALE[] = {VOLUMEREADOUT_FULLSCALE_PHONES_MV, VOLUMEREADOUT_FULLSCALE_BALANCED_MV};
	for (unsigned f = 0; f < 2; f++) {
		for (int step = 0; step < 255; step++) {
			double volts = FULLSCALE[f] * 1e-3 * pow(10.0, -step / 40.0);
			const char *text = formatted(volumereadout_nanovolts(-(long)step * 50, FULLSCALE[f]));

			double figure, unit;
			int digits, decimals;
			bool parsed = parse_reading(text, &figure, &digits, &decimals, &unit);
			double last_digit = unit * pow(10.0, -decimals);
			// Half the last digit, and the two nanovolts the table may be out.
			bool close = parsed && fabs(figure * unit - volts) <= last_digit * 0.5 + 2.5e-9;
			bool shaped = parsed && digits == 3 && glyph_count(text) <= 6 && strlen(text) < VOLUMEREADOUT_TEXT_MAX;
			// A whole figure wherever there is a smaller unit to take the
			// rest: 1.00 to 999, never 0.63 V or 1000 mV.
			bool ranged = parsed && figure < 1000.0 && (figure >= 1.0 || unit == 1e-6);
			if (!close || !shaped || !ranged) {
				failures++;
				fprintf(stderr, "FAIL %s:%d: %u mV at -%.1f dB is %.9g V, printed \"%s\"\n", __FILE__, __LINE__,
						FULLSCALE[f], step * 0.5, volts, text);
			}
		}
	}
}

// ---------------------------------------------------------------------------
// The reading, index by index
// ---------------------------------------------------------------------------

static void test_spot_readings(void) {
	volumereadout_set_enabled(true);

	// 3.5 mm, low gain: the stock default. The top of the table is 6 dB under
	// the DAC's full scale, so 100 is one volt and not two.
	volumereadout_set_jack(VOLUMEREADOUT_JACK_PHONES);
	player_state(0, VOLUME_OUTPUT_PHONES, 0, 0);
	CHECK(volumereadout_form() == VOLUMEREADOUT_FORM_VOLTS_PHONES);
	CHECK_TEXT(reading(0), "0V");
	CHECK_TEXT(reading(1), "39.9" MICRO "V");
	CHECK_TEXT(reading(11), "893" MICRO "V");
	CHECK_TEXT(reading(12), "1.00mV");
	CHECK_TEXT(reading(44), "39.9mV");
	CHECK_TEXT(reading(60), "100mV");
	CHECK_TEXT(reading(88), "502mV");
	CHECK_TEXT(reading(100), "1.00V");

	// High gain: the same table 6 dB up, and the published 2 V at its top.
	player_state(1, VOLUME_OUTPUT_PHONES, 0, 0);
	CHECK_TEXT(reading(0), "0V");
	CHECK_TEXT(reading(1), "79.6" MICRO "V");
	CHECK_TEXT(reading(45), "84.3mV");
	CHECK_TEXT(reading(88), "1.00V");
	CHECK_TEXT(reading(92), "1.26V");
	CHECK_TEXT(reading(100), "2.00V");

	// 4.4 mm: twice the 3.5 mm figure at every index, and 4 V at the top.
	volumereadout_set_jack(VOLUMEREADOUT_JACK_BALANCED);
	CHECK(volumereadout_form() == VOLUMEREADOUT_FORM_VOLTS_BALANCED);
	CHECK_TEXT(reading(1), "159" MICRO "V");
	CHECK_TEXT(reading(88), "2.00V");
	CHECK_TEXT(reading(100), "4.00V");
	player_state(0, VOLUME_OUTPUT_PHONES, 0, 0);
	CHECK_TEXT(reading(1), "79.8" MICRO "V");
	CHECK_TEXT(reading(100), "2.00V");

	// Out of range is the nearest end, as for the number.
	CHECK_TEXT(reading(-3), "0V");
	CHECK_TEXT(reading(101), "2.00V");

	// The profile says 4.4 mm on a board whose jack detect can tell: still a
	// socket, still the one that was picked. (On the V1 it never says so.)
	volumereadout_set_jack(VOLUMEREADOUT_JACK_PHONES);
	player_state(1, VOLUME_OUTPUT_BALANCED, 0, 0);
	CHECK(volumereadout_form() == VOLUMEREADOUT_FORM_VOLTS_PHONES);
	CHECK_TEXT(reading(100), "2.00V");
}

static void test_every_step_moves_the_reading(void) {
	volumereadout_set_enabled(true);
	for (int jack = 0; jack < VOLUMEREADOUT_JACK_COUNT; jack++) {
		volumereadout_set_jack((volumereadout_jack_t)jack);
		uint32_t fullscale = jack == VOLUMEREADOUT_JACK_BALANCED ? 4000 : 2000;
		for (int gain = 0; gain <= 1; gain++) {
			player_state(gain, VOLUME_OUTPUT_PHONES, 0, 0);

			char previous[VOLUMEREADOUT_TEXT_MAX] = "";
			uint64_t previous_nv = 0;
			for (int percent = 0; percent <= 100; percent++) {
				char text[VOLUMEREADOUT_TEXT_MAX];
				snprintf(text, sizeof(text), "%s", reading(percent));
				uint64_t nv = volumereadout_nanovolts(alsa_volume_output_db100(percent), fullscale);

				// Up with every step, and never the same text twice running:
				// a key press that changes the level changes the figure.
				bool moved = percent == 0 || (nv > previous_nv && strcmp(text, previous) != 0);
				// Only the mute is nothing.
				bool live = (percent == 0) == (strcmp(text, "0V") == 0) && (percent == 0) == (nv == 0);
				// Three whole figures: nothing the tables reach is under a
				// microvolt, so no reading starts with a nought.
				bool whole = percent == 0 || (text[0] >= '1' && text[0] <= '9');
				if (!moved || !live || !whole || glyph_count(text) > 6) {
					failures++;
					fprintf(stderr, "FAIL %s:%d: jack %d gain %d index %d reads \"%s\" after \"%s\"\n", __FILE__,
							__LINE__, jack, gain, percent, text, previous);
				}
				snprintf(previous, sizeof(previous), "%s", text);
				previous_nv = nv;
			}
		}
	}
}

static void test_gain_and_socket_are_in_the_figure(void) {
	volumereadout_set_enabled(true);
	volumereadout_set_jack(VOLUMEREADOUT_JACK_PHONES);

	// The gain switch is 6.0 dB at every index, and the socket a factor of two
	// (to the nanovolt each of the two is rounded to).
	for (int percent = 1; percent <= 100; percent++) {
		player_state(0, VOLUME_OUTPUT_PHONES, 0, 0);
		long low = alsa_volume_output_db100(percent);
		player_state(1, VOLUME_OUTPUT_PHONES, 0, 0);
		long high = alsa_volume_output_db100(percent);
		CHECK(high - low == 600);

		uint64_t balanced = volumereadout_nanovolts(high, 4000);
		uint64_t doubled = 2 * volumereadout_nanovolts(high, 2000);
		CHECK((balanced > doubled ? balanced - doubled : doubled - balanced) <= 1);
	}
}

static void test_dsd_compensation_only_while_dsd_plays(void) {
	volumereadout_set_enabled(true);
	volumereadout_set_jack(VOLUMEREADOUT_JACK_PHONES);

	// Low gain, index 88: 502 mV, twelve dB under the top of the DAC.
	player_state(0, VOLUME_OUTPUT_PHONES, 0, 0);
	CHECK_TEXT(reading(88), "502mV");

	// The compensation chosen but a PCM track playing: nothing is added.
	player_state(0, VOLUME_OUTPUT_PHONES, 0, 6);
	CHECK_TEXT(reading(88), "502mV");
	// Before the first track, when the DoP switch has never been written.
	player_state(0, VOLUME_OUTPUT_PHONES, -1, 6);
	CHECK_TEXT(reading(88), "502mV");

	// A DSD track: each step of the setting is one dB on the output.
	player_state(0, VOLUME_OUTPUT_PHONES, 1, 0);
	CHECK_TEXT(reading(88), "502mV");
	player_state(0, VOLUME_OUTPUT_PHONES, 1, 1);
	CHECK_TEXT(reading(88), "564mV");
	player_state(0, VOLUME_OUTPUT_PHONES, 1, 3);
	CHECK_TEXT(reading(88), "710mV");
	player_state(0, VOLUME_OUTPUT_PHONES, 1, 6);
	CHECK_TEXT(reading(88), "1.00V");

	// No headroom past the top of the DAC: high gain at 100 is already there,
	// and at 95 only two and a half of the six dB are left to give.
	player_state(1, VOLUME_OUTPUT_PHONES, 1, 6);
	CHECK_TEXT(reading(100), "2.00V");
	CHECK_TEXT(reading(95), "2.00V");
	CHECK_TEXT(reading(88), "2.00V");
	CHECK_TEXT(reading(87), "1.89V");

	// And a mute stays one.
	CHECK_TEXT(reading(0), "0V");
}

// The figure and the hardware. alsa_volume_output_db100() is what the reading
// is made from and write_dac_attenuation() is what sets the level; they are two
// functions, and a reading is only worth printing while they say the same
// thing. So the real write runs, and the reading has to be the attenuation the
// two channel controls were left at -- 255 the mute, anything else half a dB a
// step down from the socket's full scale.
static void test_reading_is_what_the_dac_was_given(void) {
	volumereadout_set_enabled(true);
	for (int jack = 0; jack < VOLUMEREADOUT_JACK_COUNT; jack++) {
		volumereadout_set_jack((volumereadout_jack_t)jack);
		uint32_t fullscale = jack == VOLUMEREADOUT_JACK_BALANCED ? VOLUMEREADOUT_FULLSCALE_BALANCED_MV
																  : VOLUMEREADOUT_FULLSCALE_PHONES_MV;
		for (int gain = 0; gain <= 1; gain++) {
			// Before the first track, a PCM one, and a DSD one.
			for (int dop = -1; dop <= 1; dop++) {
				for (int dsd = 0; dsd <= DSD_GAIN_MAX_DB; dsd++) {
					player_state(gain, VOLUME_OUTPUT_PHONES, dop, dsd);
					for (int percent = 0; percent <= 100; percent++) {
						current_percent = percent;
						dac_left = dac_right = -1;
						int left_before = dac_left_writes;
						int right_before = dac_right_writes;
						write_dac_attenuation();

						bool written = dac_left_writes == left_before + 1 && dac_right_writes == right_before + 1 &&
									   dac_left == dac_right && dac_left >= 0 && dac_left <= 255;
						char want[VOLUMEREADOUT_TEXT_MAX] = "0V";
						if (written && dac_left < 255) {
							volumereadout_format(volumereadout_nanovolts(-dac_left * 50, fullscale), want, sizeof(want));
						}
						const char *text = reading(percent);
						// Only index 0 is written as the mute, so nothing else may read as one.
						bool mute_is_zero = (dac_left == 255) == (percent == 0);
						if (!written || !mute_is_zero || strcmp(text, want) != 0) {
							failures++;
							fprintf(stderr,
									"FAIL %s:%d: jack %d gain %d dop %d dsd %d index %d: DAC given %ld/%ld, reads \"%s\", want \"%s\"\n",
									__FILE__, __LINE__, jack, gain, dop, dsd, percent, dac_left, dac_right, text, want);
						}
					}
				}
			}
		}
	}

	// The one place they part, on purpose. Around a track change, a suspend and
	// a power-off the DAC is held at the mute for a moment with the index left
	// where it was; the reading goes on saying what that index puts out, where
	// following the register would flash "0V" between every two tracks.
	volumereadout_set_jack(VOLUMEREADOUT_JACK_PHONES);
	player_state(0, VOLUME_OUTPUT_PHONES, 0, 0);
	current_percent = 88;
	track_muted = true;
	write_dac_attenuation();
	CHECK(dac_left == 255 && dac_right == 255);
	CHECK_TEXT(reading(88), "502mV");
	track_muted = false;
	write_dac_attenuation();
	CHECK(dac_left == 24 && dac_right == 24); // 12 dB down: 502 mV of the 2 V
	CHECK_TEXT(reading(88), "502mV");

	// Nothing but the two channel controls was touched.
	CHECK(mixer_strays == 0);
}

static void test_other_outputs_keep_the_number(void) {
	volumereadout_set_enabled(true);
	volumereadout_set_jack(VOLUMEREADOUT_JACK_BALANCED);

	// Bluetooth headphones and a USB-C DAC set the level themselves, at a full
	// scale nobody here knows.
	player_state(1, VOLUME_OUTPUT_BLUETOOTH, 0, 0);
	CHECK(volumereadout_form() == VOLUMEREADOUT_FORM_INDEX);
	CHECK_TEXT(reading(57), "57");
	CHECK_TEXT(reading(0), "0");
	CHECK_TEXT(reading(100), "100");

	player_state(1, VOLUME_OUTPUT_USB, 1, 6);
	CHECK(volumereadout_form() == VOLUMEREADOUT_FORM_INDEX);
	CHECK_TEXT(reading(57), "57");

	// Back on a socket, back to volts, without anything being set again.
	player_state(1, VOLUME_OUTPUT_PHONES, 0, 0);
	CHECK(volumereadout_form() == VOLUMEREADOUT_FORM_VOLTS_BALANCED);
	CHECK_TEXT(reading(100), "4.00V");

	// Switched off, the number everywhere.
	volumereadout_set_enabled(false);
	CHECK(volumereadout_form() == VOLUMEREADOUT_FORM_INDEX);
	CHECK_TEXT(reading(100), "100");
}

// ---------------------------------------------------------------------------
// What a label is measured against
// ---------------------------------------------------------------------------

#define SAMPLES_MAX 300
static char samples[SAMPLES_MAX][VOLUMEREADOUT_TEXT_MAX];
static int sample_count;

static void collect_samples(void) {
	sample_count = 0;
	while (sample_count < SAMPLES_MAX &&
		   volumereadout_sample(sample_count, samples[sample_count], sizeof(samples[0]))) {
		sample_count++;
	}
}

static bool among_samples(const char *text) {
	for (int i = 0; i < sample_count; i++) {
		if (strcmp(samples[i], text) == 0) {
			return true;
		}
	}
	return false;
}

static void test_samples_cover_every_reading(void) {
	volumereadout_set_enabled(true);
	for (int jack = 0; jack < VOLUMEREADOUT_JACK_COUNT; jack++) {
		volumereadout_set_jack((volumereadout_jack_t)jack);
		player_state(0, VOLUME_OUTPUT_PHONES, 0, 0);
		collect_samples();

		// The register's 255 levels and the mute, whatever the player is doing
		// at the moment the label is measured.
		CHECK(sample_count == 256);
		CHECK(among_samples("0V"));
		CHECK(among_samples(jack == VOLUMEREADOUT_JACK_BALANCED ? "4.00V" : "2.00V"));
		for (int i = 0; i < sample_count; i++) {
			CHECK(glyph_count(samples[i]) <= 6);
		}

		// So the width holds over both gain tables and every DSD setting,
		// with and without a DSD track: none of them prints anything that was
		// not measured.
		for (int gain = 0; gain <= 1; gain++) {
			for (int dop = 0; dop <= 1; dop++) {
				for (int dsd = 0; dsd <= DSD_GAIN_MAX_DB; dsd++) {
					player_state(gain, VOLUME_OUTPUT_PHONES, dop, dsd);
					for (int percent = 0; percent <= 100; percent++) {
						const char *text = reading(percent);
						if (!among_samples(text)) {
							failures++;
							fprintf(stderr, "FAIL %s:%d: jack %d gain %d dop %d dsd %d index %d: \"%s\" was never measured\n",
									__FILE__, __LINE__, jack, gain, dop, dsd, percent, text);
						}
					}
				}
			}
		}
	}

	// The samples are of the form in force: on Bluetooth they are the numbers
	// again, so the label gives the width back.
	player_state(1, VOLUME_OUTPUT_BLUETOOTH, 0, 0);
	collect_samples();
	CHECK(sample_count == 101);
	CHECK(among_samples("100") && !among_samples("0V"));
}

int main(void) {
	// The first look at the config has to be the first test.
	test_off_until_asked_for();
	test_settings_are_saved();

	test_table_is_the_logarithm();
	test_three_figures_and_their_edges();
	test_text_is_the_value_to_three_figures();

	test_spot_readings();
	test_every_step_moves_the_reading();
	test_gain_and_socket_are_in_the_figure();
	test_dsd_compensation_only_while_dsd_plays();
	test_reading_is_what_the_dac_was_given();
	test_other_outputs_keep_the_number();
	test_samples_cover_every_reading();

	CHECK(config_strays == 0);

	if (failures) {
		fprintf(stderr, "test_volumereadout: %d failure(s)\n", failures);
		return 1;
	}
	printf("test_volumereadout: all checks passed\n");
	return 0;
}
