// test_usbaudio_volume.c -- where the level physically happens on a USB DAC.
//
// Two complaints meet here. The first is the one this file exists for: a quiet
// hiss under the music on both of the V1's dongles, worse on the louder one,
// absent on a phone and absent on the stock firmware. That is what a dongle
// sounds like when the player leaves its own control at full gain and turns the
// *samples* down instead: the music loses 20-odd dB and the amplifier's noise
// floor does not, so the two end up that much closer together.
//
// The second is the reason the old default was chosen in the first place: a USB
// descriptor may lie about its own dB scale, or publish a control that will not
// move, and a level law built on a lie is worse than a hiss. Both are checked
// here, because the answer is neither "always hardware" nor "never" -- it is
// "the device, and the samples the moment the device stops making sense".
//
// The mixer itself is faked: snd_ctl_* is a small table with one integer
// control, its range, its dB scale and the value a write actually leaves
// behind. Everything above that -- find_volume_control()'s shape matching and
// scale checks, the split arithmetic, the read-back that gives up on a control
// which does not take -- is the real usbaudio.c, compiled in.

#define _GNU_SOURCE
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ---------------------------------------------------------------------------
// The fake mixer
// ---------------------------------------------------------------------------

// One integer control, the way a UAC feature unit presents itself.
typedef struct {
	const char *name;
	long min, max;
	unsigned int count;
	bool writable;
	bool tlv_readable;
	long db_min, db_max; // hundredths of a dB
	long value;          // what a read returns
	long write_cap;      // a write is clamped to at most this (a control that lies)
	bool ignore_writes;  // value never moves: a control that does nothing
	int writes;
} fake_ctl_t;

static fake_ctl_t mixer;

struct snd_ctl {};
typedef struct snd_ctl snd_ctl_t;
struct snd_ctl_elem_id {};
struct snd_ctl_elem_info {};
struct snd_ctl_elem_list {};
struct snd_ctl_elem_value {};

int snd_ctl_open(snd_ctl_t **ctl, const char *name, int mode) {
	(void)name;
	(void)mode;
	static struct snd_ctl only;
	*ctl = &only;
	return 0;
}
int snd_ctl_close(void *ctl) {
	(void)ctl;
	return 0;
}

static unsigned int list_count;
int snd_ctl_elem_list(void *ctl, void *list) {
	(void)ctl;
	(void)list;
	list_count = 1;
	return 0;
}
int snd_ctl_elem_list_alloc_space(void *list, unsigned int count) {
	(void)list;
	(void)count;
	return 0;
}
void snd_ctl_elem_list_free_space(void *list) { (void)list; }
unsigned int snd_ctl_elem_list_get_count(void *list) {
	(void)list;
	return list_count;
}
void snd_ctl_elem_list_get_id(void *list, unsigned int i, void *id) {
	(void)list;
	(void)i;
	(void)id;
}

static const char *elem_name;
static long pending_write;
int snd_ctl_elem_info(void *ctl, void *info) {
	(void)ctl;
	(void)info;
	return 0;
}
int snd_ctl_elem_info_get_type(void *info) {
	(void)info;
	return 1; // SND_CTL_ELEM_TYPE_INTEGER
}
int snd_ctl_elem_info_is_writable(void *info) {
	(void)info;
	return mixer.writable;
}
int snd_ctl_elem_info_is_tlv_readable(void *info) {
	(void)info;
	return mixer.tlv_readable;
}
unsigned int snd_ctl_elem_info_get_count(void *info) {
	(void)info;
	return mixer.count;
}
long snd_ctl_elem_info_get_min(void *info) {
	(void)info;
	return mixer.min;
}
long snd_ctl_elem_info_get_max(void *info) {
	(void)info;
	return mixer.max;
}
const char *snd_ctl_elem_info_get_name(void *info) {
	(void)info;
	return elem_name;
}
void snd_ctl_elem_info_set_id(void *info, const void *id) {
	(void)info;
	(void)id;
}
void snd_ctl_elem_id_set_interface(void *id, int iface) {
	(void)id;
	(void)iface;
}
static char id_name[64];
void snd_ctl_elem_id_set_name(void *id, const char *name) {
	(void)id;
	snprintf(id_name, sizeof(id_name), "%s", name);
}

// The TLV is opaque to the code under test except through these two calls, so
// the scale is handed back from the fake control's own numbers.
static unsigned int tlv[256];
int snd_ctl_elem_tlv_read(void *ctl, const void *id, unsigned int *out, unsigned int size) {
	(void)ctl;
	(void)id;
	(void)size;
	out[0] = (unsigned int)mixer.min;
	out[1] = (unsigned int)mixer.max;
	out[2] = (unsigned int)mixer.db_min;
	out[3] = (unsigned int)mixer.db_max;
	memcpy(tlv, out, 4 * sizeof(unsigned int));
	return 0;
}
int snd_tlv_get_dB_range(const unsigned int *t, long min, long max, long *db_min, long *db_max) {
	(void)t;
	(void)min;
	(void)max;
	*db_min = mixer.db_min;
	*db_max = mixer.db_max;
	return 0;
}

// The real arithmetic: a linear scale of (db_max - db_min) over the range,
// which is what a UAC control with a dB TLV describes.
static long value_to_db(long value) {
	long span = mixer.max - mixer.min;
	if (span <= 0) {
		return mixer.db_max;
	}
	return mixer.db_min + (value - mixer.min) * (mixer.db_max - mixer.db_min) / span;
}

int snd_tlv_convert_to_dB(const unsigned int *t, long min, long max, long value, long *db) {
	(void)t;
	(void)min;
	(void)max;
	*db = value_to_db(value);
	return 0;
}

// `round` +1: the nearest step at or above the target (less attenuation), -1:
// at or below. Both are "which index is closest to this dB figure".
int snd_tlv_convert_from_dB(const unsigned int *t, long min, long max, long db, long *value, int round) {
	(void)t;
	(void)min;
	(void)max;
	long span = mixer.max - mixer.min;
	if (mixer.db_max == mixer.db_min || span <= 0) {
		*value = mixer.max;
		return 0;
	}
	long step = (db - mixer.db_min) * span / (mixer.db_max - mixer.db_min);
	*value = mixer.min + step;
	if (round > 0) {
		// at or above: never attenuate more than asked for
		while (*value < mixer.max && value_to_db(*value) < db) {
			(*value)++;
		}
	} else {
		while (*value > mixer.min && value_to_db(*value) > db) {
			(*value)--;
		}
	}
	if (*value < mixer.min) {
		*value = mixer.min;
	}
	if (*value > mixer.max) {
		*value = mixer.max;
	}
	return 0;
}

int snd_ctl_elem_read(void *ctl, void *value) {
	(void)ctl;
	(void)value;
	return 0; // the fake's value is handed back by get_integer below
}
int snd_ctl_elem_write(void *ctl, void *value) {
	(void)ctl;
	(void)value;
	(void)id_name;
	mixer.writes++;
	if (!mixer.ignore_writes) {
		// A control that will not move keeps whatever it was reading at.
		mixer.value = pending_write;
		if (mixer.value > mixer.max) {
			mixer.value = mixer.max;
		}
		if (mixer.value < mixer.min) {
			mixer.value = mixer.min;
		}
	}
	return 0;
}
void snd_ctl_elem_value_set_id(void *value, const void *id) {
	(void)value;
	(void)id;
}
void snd_ctl_elem_value_set_integer(void *value, unsigned int index, long v) {
	(void)value;
	(void)index;
	pending_write = v;
}
long snd_ctl_elem_value_get_integer(void *value, unsigned int index) {
	(void)value;
	(void)index;
	return mixer.value;
}

// ---------------------------------------------------------------------------
// The rest of what usbaudio.c asks for
// ---------------------------------------------------------------------------

#define SOUND_CLASS "/nonexistent/sound"
#define USB_AUDIO_BUS "/nonexistent/usb"
#define TYPEC_PORT_TYPE "/nonexistent/typec"
#define TYPEC_PARTNER "/nonexistent/partner"
#define TCS1421_CFG "/nonexistent/tcs"
#define DWC2_OTG_ID "/nonexistent/id"

// What swvolume.c was told, captured instead of applied: the split is the
// subject, not the multiplication.
static long sw_gain_db100;
static bool sw_mute;
static bool sw_cleared;

#include "../src/system/audio/swvolume.h"
void swvolume_set_usb_gain(long db100, bool mute) {
	sw_gain_db100 = db100;
	sw_mute = mute;
}
void swvolume_clear_usb_gain(void) {
	sw_gain_db100 = 0;
	sw_mute = false;
	sw_cleared = true;
}
bool swvolume_active(void) { return false; }
void swvolume_set_index(int percent) { (void)percent; }

// The config key under test.
static long config_hardware_volume = 1;
long config_get_int(const char *section, const char *key, long fallback) {
	(void)section;
	if (strcmp(key, "hardware_volume") == 0) {
		return config_hardware_volume;
	}
	return fallback;
}
const char *config_get(const char *section, const char *key, const char *fallback) {
	(void)section;
	(void)key;
	return fallback;
}

bool usb_vbus_present(void) { return false; }
bool usbhostprobe_active(void) { return false; }
void usbhostprobe_recover(void) {}
bool bluetooth_audio_active(void) { return false; }
void usbhostmode_tick(void) {}
int get_volume_percent(void) { return 57; }
void audio_set_output_device(const char *pcm) { (void)pcm; }
long alsa_volume_gain_db100(int percent) {
	// A straight stand-in for the jack law: 0.5 dB per index below 100, which
	// is the shape the real tables have over the range this file cares about.
	return -(100 - percent) * 50;
}

#include "../src/system/audio/usbaudio.c"

// ---------------------------------------------------------------------------
// The cases
// ---------------------------------------------------------------------------

// The JCally-shaped control from the log: 0..50, -50.00..0.00 dB, 1 dB a step.
static void a_one_db_control(void) {
	memset(&mixer, 0, sizeof(mixer));
	mixer.name = "PCM Playback Volume";
	mixer.min = 0;
	mixer.max = 50;
	mixer.count = 2;
	mixer.writable = true;
	mixer.tlv_readable = true;
	mixer.db_min = -5000;
	mixer.db_max = 0;
	mixer.value = 50;
	elem_name = mixer.name;
	active_card = 3;
	find_volume_control(3);
	assert(strcmp(volume_control, "PCM Playback Volume") == 0);
	assert(volume_has_db);
}

static void test_the_device_takes_the_level_by_default(void) {
	// The hiss. At 57% the player's law wants 21.5 dB down; the dongle's own
	// control should be doing that, not the samples.
	a_one_db_control();
	config_hardware_volume = 1;
	mixer.value = 50;

	sw_gain_db100 = 0;
	usbaudio_apply_volume(57);

	// The control was asked for roughly the whole of it...
	long asked = pending_write;
	assert(asked < mixer.max);
	long ctl_db = value_to_db(asked);
	if (ctl_db > -2100) {
		fprintf(stderr, "FAIL: control asked for %ld (%ld cB), wanted about -2150\n", asked, ctl_db);
		exit(1);
	}
	// ...and the samples only the remainder, within one of the control's steps.
	long remainder = -2150 - ctl_db;
	if (sw_gain_db100 != remainder) {
		fprintf(stderr, "FAIL: samples asked for %ld, wanted %ld (control has %ld)\n", sw_gain_db100, remainder, ctl_db);
		exit(1);
	}
	if (sw_gain_db100 < -100) {
		fprintf(stderr, "FAIL: the samples still carry %ld cB of the level\n", sw_gain_db100);
		exit(1);
	}
}

static void test_a_coarse_control_still_moves_smoothly(void) {
	// One step of a 10 dB control is 10 dB. The level must still land where
	// the index says, with the samples carrying the fraction -- otherwise the
	// fix for the hiss would be a volume knob with six usable positions.
	memset(&mixer, 0, sizeof(mixer));
	mixer.name = "PCM Playback Volume";
	mixer.min = 0;
	mixer.max = 10;
	mixer.count = 2;
	mixer.writable = true;
	mixer.tlv_readable = true;
	mixer.db_min = -10000;
	mixer.db_max = 0;
	elem_name = mixer.name;
	active_card = 3;
	find_volume_control(3);
	assert(volume_has_db);

	for (int percent = 20; percent <= 100; percent += 5) {
		sw_gain_db100 = 0;
		usbaudio_apply_volume(percent);
		long ctl_db = value_to_db(pending_write);
		long total = ctl_db + sw_gain_db100;
		long want = -(100 - percent) * 50;
		// A step of this control is 1 dB, so the split can be a whole step
		// "up" from the target before the samples take the difference. Never
		// more than that, and never the wrong side (which would clip).
		if (total > want || total < want - 1000) {
			fprintf(stderr, "FAIL at %d%%: control %ld + samples %ld = %ld, wanted %ld\n", percent, ctl_db,
					sw_gain_db100, total, want);
			exit(1);
		}
	}
}

static void test_a_lying_descriptor_falls_back_to_the_samples(void) {
	// The reason the old default existed: 256 steps claiming -128..-127 dB.
	// Nothing may be believed about that scale, so the control is not used for
	// the level at all and the samples carry all of it.
	memset(&mixer, 0, sizeof(mixer));
	mixer.name = "PCM Playback Volume";
	mixer.min = 0;
	mixer.max = 255;
	mixer.count = 2;
	mixer.writable = true;
	mixer.tlv_readable = true;
	mixer.db_min = -12800;
	mixer.db_max = -12700;
	elem_name = mixer.name;
	active_card = 3;
	find_volume_control(3);
	assert(!volume_has_db); // rejected on sight

	sw_gain_db100 = 0;
	usbaudio_apply_volume(57);
	assert(sw_gain_db100 == -2150);
}

static void test_a_control_that_will_not_move_gives_up(void) {
	// A writable control that reads back at its top however it is written is
	// not a volume control. It has to be given up, and the samples take over.
	a_one_db_control();
	config_hardware_volume = 1;
	mixer.ignore_writes = true;
	mixer.value = 50; // sits at the top, always

	sw_gain_db100 = 0;
	usbaudio_apply_volume(57);
	assert(volume_control[0] == '\0'); // given up
	assert(sw_gain_db100 == -2150);    // and the samples carry the whole level
}

static void test_the_old_behaviour_is_still_askable(void) {
	// [usb] hardware_volume = 0: the control pinned at 0 dB, the samples take
	// everything. Kept because a device whose control is audible-sounding but
	// badly behaved is a reason to want it back.
	a_one_db_control();
	config_hardware_volume = 0;
	mixer.value = 50;

	sw_gain_db100 = 0;
	usbaudio_apply_volume(57);
	assert(pending_write == mixer.max);
	assert(sw_gain_db100 == -2150);
}

static void test_no_control_means_all_of_it_on_the_samples(void) {
	memset(&mixer, 0, sizeof(mixer));
	mixer.name = "Not A Volume";
	mixer.min = 0;
	mixer.max = 10;
	mixer.writable = true;
	elem_name = mixer.name;
	active_card = 3;
	find_volume_control(3);
	assert(volume_control[0] == '\0');

	sw_gain_db100 = 0;
	usbaudio_apply_volume(57);
	assert(sw_gain_db100 == -2150);
}

static void test_mute_goes_to_the_control(void) {
	a_one_db_control();
	config_hardware_volume = 1;
	mixer.value = 50;

	sw_gain_db100 = -1;
	usbaudio_apply_volume(0);
	assert(pending_write == mixer.min); // the device itself, not a stream of zeroes
	assert(sw_mute);
}

int main(void) {
	test_the_device_takes_the_level_by_default();
	test_a_coarse_control_still_moves_smoothly();
	test_a_lying_descriptor_falls_back_to_the_samples();
	test_a_control_that_will_not_move_gives_up();
	test_the_old_behaviour_is_still_askable();
	test_no_control_means_all_of_it_on_the_samples();
	test_mute_goes_to_the_control();
	puts("PASS USB audio volume (the device takes the level, the samples the remainder)");
	return 0;
}
