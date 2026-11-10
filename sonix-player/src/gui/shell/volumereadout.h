#ifndef VOLUMEREADOUT_H
#define VOLUMEREADOUT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// The volume as what comes out of the socket: "502mV" where the status bar and
// the volume bar would otherwise print "88".
//
// The index is a position on the gain table, and what it means in level moves
// with the gain switch and the socket: 88 is 1 V on the 3.5 mm jack in high
// gain, half a volt in low gain, and twice either on the 4.4 mm one. A voltage
// means the same level whichever of them is in force, which is what makes it
// worth reading -- and it can be set against the sensitivity figure printed on
// a pair of headphones.
//
// It is the NOMINAL figure, worked out and not measured:
//
//   V = Vfs * 10 ^ (dB / 20)
//
// dB is what the DAC has been given for the index (alsa_volume_output_db100():
// the gain table in force, plus the DSD compensation while a DSD track runs),
// and Vfs is what TempoTec publishes as the most each socket puts out -- 2 Vrms
// on the 3.5 mm jack, 4 Vrms on the 4.4 mm one -- taken to be the top of the
// high gain table. So it is the level of a 0 dBFS sine into no load. Music sits
// below it by however far the record does, and so does anything done to the
// samples on the way (ReplayGain, the equaliser's pre-amp, channel balance).
// A load takes the top of the balanced scale down as well: the power published
// for 32 ohms there, 215 mW, is 2.6 V across them and not 4.
//
// Which socket, the player cannot tell. The V1 publishes one switch node for
// both and it reads the same with either plugged or neither (PATCHES.md, the
// two board matters under "TempoTec V1 240x320 interface profile"), so the
// socket is asked for: 3.5 mm unless told otherwise.
//
// Off by default. On, it is used only while the sound leaves by a socket:
// Bluetooth headphones and a USB-C DAC apply the level themselves, at a full
// scale this player has no way to know, and they keep the plain 0-100.

// The socket the reading is for.
typedef enum {
	VOLUMEREADOUT_JACK_PHONES = 0, // 3.5 mm single-ended
	VOLUMEREADOUT_JACK_BALANCED,   // 4.4 mm balanced
	VOLUMEREADOUT_JACK_COUNT,
} volumereadout_jack_t;

// Full scale at each, in millivolts RMS: the vendor's published maximum.
#define VOLUMEREADOUT_FULLSCALE_PHONES_MV 2000u
#define VOLUMEREADOUT_FULLSCALE_BALANCED_MV 4000u

// Room for any reading and its terminator: six characters at most, one of
// which (the micro sign) is two bytes.
#define VOLUMEREADOUT_TEXT_MAX 16

// The two settings, [screen] volume_voltage and [screen] volume_voltage_jack.
// The setters save; what is on screen is the caller's to repaint.
bool volumereadout_enabled(void);
void volumereadout_set_enabled(bool on);
volumereadout_jack_t volumereadout_jack(void);
void volumereadout_set_jack(volumereadout_jack_t jack);

// What the reading looks like at this moment, which is what its width depends
// on: the plain number, or volts at one socket or the other. A label sized for
// one is re-measured when this changes.
typedef enum {
	VOLUMEREADOUT_FORM_INDEX = 0,
	VOLUMEREADOUT_FORM_VOLTS_PHONES,
	VOLUMEREADOUT_FORM_VOLTS_BALANCED,
} volumereadout_form_t;
volumereadout_form_t volumereadout_form(void);

// The reading for a volume index (0..100, clamped), in the form in force.
void volumereadout_text(int percent, char *out, size_t size);

// Every reading the form in force can come to, one per `index` from 0 until it
// returns false: what a label is measured against so that it holds one width
// while the level moves. In volts that is each step of the DAC's attenuation
// and not only the hundred the gain table lands on, so the width also holds
// across the gain switch and the DSD compensation.
bool volumereadout_sample(int index, char *out, size_t size);

// ---------------------------------------------------------------------------
// The arithmetic, on its own so it can be checked without a player around it.
// Integers throughout: the same answer on the device as on the bench, and no
// libm call for the device's older glibc to disagree about.
// ---------------------------------------------------------------------------

// The RMS level in nanovolts for a gain in hundredths of a dB against a full
// scale in millivolts. The gain is taken to the DAC's half-dB step. Full scale
// for anything above 0 dB, since there is no headroom past the top of the
// table, and nothing at all from 320 dB down -- which is where
// ALSA_VOLUME_MUTE_DB100 falls, so the mute needs no case of its own.
uint64_t volumereadout_nanovolts(long gain_db100, uint32_t fullscale_mv);

// Three significant figures in the unit that keeps them: "1.26V", "632mV",
// "84.3mV", "8.43mV", "843µV", "39.9µV", and "0V" for none. Three because the
// top half of the scale moves half a dB a step, under six per cent, and two
// figures would print the same reading twice running. No space before the
// unit: the V1's status bar is 240 px across with a clock in the middle of it.
void volumereadout_format(uint64_t nanovolts, char *out, size_t size);

#endif // VOLUMEREADOUT_H
