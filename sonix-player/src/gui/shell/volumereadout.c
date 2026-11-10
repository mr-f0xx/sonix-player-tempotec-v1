#include "volumereadout.h"

#include <stdio.h>

#include "src/system/audio/alsa-controls.h"
#include "src/system/core/config.h"

// ---------------------------------------------------------------------------
// The arithmetic
// ---------------------------------------------------------------------------

// 10 ^ (-n / 40) for n = 0..39, times 10^9: the forty half-dB steps inside one
// 20 dB decade, as voltage ratios. Twenty dB is a division by ten exactly, so
// these forty and a count of decades reach any attenuation, with no logarithm
// to take and no rounding that could differ between two machines.
#define HALF_DB_PER_DECADE 40
static const uint32_t HALF_DB_RATIO[HALF_DB_PER_DECADE] = {
	1000000000u, 944060876u, 891250938u, 841395142u, 794328235u, //
	749894209u,	 707945784u, 668343918u, 630957344u, 595662144u, //
	562341325u,	 530884444u, 501187234u, 473151259u, 446683592u, //
	421696503u,	 398107171u, 375837404u, 354813389u, 334965439u, //
	316227766u,	 298538262u, 281838293u, 266072506u, 251188643u, //
	237137371u,	 223872114u, 211348904u, 199526231u, 188364909u, //
	177827941u,	 167880402u, 158489319u, 149623566u, 141253754u, //
	133352143u,	 125892541u, 118850223u, 112201845u, 105925373u, //
};

// Sixteen decades down, 320 dB, there is not half a nanovolt left of any full
// scale the argument can hold, and one decade further the divisor below stops
// fitting its type. The mute sentinel lands here as well: to this arithmetic
// it is a gain like any other, only very far down.
#define DECADES_TO_NOTHING 16

uint64_t volumereadout_nanovolts(long gain_db100, uint32_t fullscale_mv) {
	if (gain_db100 >= 0) {
		return (uint64_t)fullscale_mv * 1000000u;
	}

	if (gain_db100 <= -(long)DECADES_TO_NOTHING * 2000) {
		return 0;
	}

	// Attenuation in half-dB steps, to the nearest, and how many whole decades
	// of it there are.
	long steps = (-gain_db100 + 25) / 50;
	long decades = steps / HALF_DB_PER_DECADE;

	// millivolts * ratio is in units of 10^-9 mV, a thousandth of a nanovolt.
	uint64_t divisor = 1000;
	for (long i = 0; i < decades; i++) {
		divisor *= 10;
	}
	uint64_t scaled = (uint64_t)fullscale_mv * HALF_DB_RATIO[steps % HALF_DB_PER_DECADE];
	return (scaled + divisor / 2) / divisor;
}

// Nothing a socket puts out comes near this; it only keeps the products below
// inside sixty-four bits whatever is passed in.
#define NANOVOLTS_MAX UINT64_C(999000000000)

void volumereadout_format(uint64_t nanovolts, char *out, size_t size) {
	if (!out || size == 0) {
		return;
	}
	if (nanovolts == 0) {
		snprintf(out, size, "0V");
		return;
	}
	if (nanovolts > NANOVOLTS_MAX) {
		nanovolts = NANOVOLTS_MAX;
	}

	static const struct {
		uint64_t nanovolts;
		const char *name;
	} UNITS[] = {
		{UINT64_C(1000000000), "V"},
		{UINT64_C(1000000), "mV"},
		{UINT64_C(1000), "\xC2\xB5V"}, // U+00B5 MICRO SIGN
	};
	enum { UNIT_COUNT = sizeof(UNITS) / sizeof(UNITS[0]) };

	// The largest unit the reading comes to 1.00 of once it is rounded, which
	// is from 0.9995 up; microvolts take whatever is under all three.
	int unit = 0;
	while (unit < UNIT_COUNT - 1 && nanovolts * 2000 < UNITS[unit].nanovolts * 1999) {
		unit++;
	}
	uint64_t scale = UNITS[unit].nanovolts;

	// Decimals that leave three figures. The two limits are where rounding
	// carries into a new digit -- 9.995 prints 10.0 and 99.95 prints 100 -- so
	// the figure can never come out as 10.00 or 100.0.
	int decimals = 0;
	unsigned shift = 1; // ten to the power of `decimals`
	if (nanovolts * 1000 < scale * 9995) {
		decimals = 2;
		shift = 100;
	} else if (nanovolts * 1000 < scale * 99950) {
		decimals = 1;
		shift = 10;
	}

	unsigned figure = (unsigned)((nanovolts * shift + scale / 2) / scale);
	if (decimals == 0) {
		snprintf(out, size, "%u%s", figure, UNITS[unit].name);
	} else {
		snprintf(out, size, "%u.%0*u%s", figure / shift, decimals, figure % shift, UNITS[unit].name);
	}
}

// ---------------------------------------------------------------------------
// The two settings
// ---------------------------------------------------------------------------

// Read once, the first time either is asked for: the reading is worked out on
// every step of a held volume key, which is no place for two config lookups.
// -1 is "not read yet".
static int setting_enabled = -1;
static int setting_jack = -1;

bool volumereadout_enabled(void) {
	if (setting_enabled < 0) {
		setting_enabled = config_get_int("screen", "volume_voltage", 0) != 0;
	}
	return setting_enabled != 0;
}

void volumereadout_set_enabled(bool on) {
	setting_enabled = on ? 1 : 0;
	config_set_int("screen", "volume_voltage", setting_enabled);
	config_save();
}

volumereadout_jack_t volumereadout_jack(void) {
	if (setting_jack < 0) {
		// Anything but the one other value is the 3.5 mm jack, a hand-edited
		// config included: it is the lower of the two figures, and a reading
		// that understates the output is the safer one to be wrong with.
		long saved = config_get_int("screen", "volume_voltage_jack", VOLUMEREADOUT_JACK_PHONES);
		setting_jack = saved == VOLUMEREADOUT_JACK_BALANCED ? VOLUMEREADOUT_JACK_BALANCED : VOLUMEREADOUT_JACK_PHONES;
	}
	return (volumereadout_jack_t)setting_jack;
}

void volumereadout_set_jack(volumereadout_jack_t jack) {
	setting_jack = jack == VOLUMEREADOUT_JACK_BALANCED ? VOLUMEREADOUT_JACK_BALANCED : VOLUMEREADOUT_JACK_PHONES;
	config_set_int("screen", "volume_voltage_jack", setting_jack);
	config_save();
}

// ---------------------------------------------------------------------------
// The reading
// ---------------------------------------------------------------------------

volumereadout_form_t volumereadout_form(void) {
	if (!volumereadout_enabled()) {
		return VOLUMEREADOUT_FORM_INDEX;
	}

	// Only while the sound leaves by a socket. The profile is the answer that
	// already follows where it is really going (the PCM in use, not a setting),
	// and its two wired values are both "a socket" here: on this board the jack
	// detect behind the difference between them never reports the 4.4 mm one,
	// which is why the socket is a setting in the first place.
	volume_output_t output = volume_profile_output();
	if (output != VOLUME_OUTPUT_PHONES && output != VOLUME_OUTPUT_BALANCED) {
		return VOLUMEREADOUT_FORM_INDEX;
	}

	return volumereadout_jack() == VOLUMEREADOUT_JACK_BALANCED ? VOLUMEREADOUT_FORM_VOLTS_BALANCED : VOLUMEREADOUT_FORM_VOLTS_PHONES;
}

static uint32_t fullscale_mv(volumereadout_form_t form) { return form == VOLUMEREADOUT_FORM_VOLTS_BALANCED ? VOLUMEREADOUT_FULLSCALE_BALANCED_MV : VOLUMEREADOUT_FULLSCALE_PHONES_MV; }

void volumereadout_text(int percent, char *out, size_t size) {
	if (!out || size == 0) {
		return;
	}
	if (percent < 0) {
		percent = 0;
	}
	if (percent > 100) {
		percent = 100;
	}

	volumereadout_form_t form = volumereadout_form();
	if (form == VOLUMEREADOUT_FORM_INDEX) {
		snprintf(out, size, "%d", percent);
		return;
	}
	volumereadout_format(volumereadout_nanovolts(alsa_volume_output_db100(percent), fullscale_mv(form)), out, size);
}

// The DAC's attenuation register: 0 at the top of the scale, half a dB a step,
// and 255 the mute. Every index of both gain tables is one of these, and so is
// every one of them with the DSD compensation taken off it.
#define DAC_ATTENUATION_MUTE 255

bool volumereadout_sample(int index, char *out, size_t size) {
	if (index < 0 || !out || size == 0) {
		return false;
	}

	volumereadout_form_t form = volumereadout_form();
	if (form == VOLUMEREADOUT_FORM_INDEX) {
		if (index > 100) {
			return false;
		}
		snprintf(out, size, "%d", index);
		return true;
	}

	if (index > DAC_ATTENUATION_MUTE) {
		return false;
	}
	uint64_t nanovolts = 0; // the mute, last
	if (index < DAC_ATTENUATION_MUTE) {
		nanovolts = volumereadout_nanovolts(-(long)index * 50, fullscale_mv(form));
	}
	volumereadout_format(nanovolts, out, size);
	return true;
}
