/* The real volume profiles and alarm ramp against a recording fake mixer. */
#define _DEFAULT_SOURCE
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "src/system/core/config.h"
#include "src/system/device/sysinfo.h"
#include "src/system/audio/audio.h"

struct snd_ctl { int unused; };
typedef struct snd_ctl snd_ctl_t;
static const char *mixer_name;
static long mixer_value, dac_left, dac_right;
static int dac_writes, usb_level, software_level, bluetooth_level, output_notifications;

int snd_ctl_open(snd_ctl_t **ctl, const char *name, int mode) {
	(void)name; (void)mode;
	static struct snd_ctl card;
	*ctl = &card;
	return 0;
}
int snd_ctl_close(snd_ctl_t *ctl) { (void)ctl; return 0; }
void snd_ctl_elem_id_set_interface(void *id, int iface) { (void)id; (void)iface; }
void snd_ctl_elem_id_set_name(void *id, const char *name) { (void)id; mixer_name = name; }
void snd_ctl_elem_value_set_id(void *value, void *id) { (void)value; (void)id; }
void snd_ctl_elem_value_set_integer(void *value, unsigned int channel, long level) {
	(void)value; (void)channel; mixer_value = level;
}
int snd_ctl_elem_write(snd_ctl_t *ctl, void *value) {
	(void)ctl; (void)value;
	if (!strcmp(mixer_name, "Left Playback Volume")) { dac_left = mixer_value; dac_writes++; }
	if (!strcmp(mixer_name, "Right Playback Volume")) { dac_right = mixer_value; dac_writes++; }
	return 0;
}

bool addons_enabled(const char *id) { (void)id; return true; }
bool file_matches(const char *path, const char *wanted) { (void)path; (void)wanted; return false; }
const sysinfo_model_t *sysinfo_model(void) {
	static const sysinfo_model_t v1 = {.cs43131 = true, .headset_switch_idle_one = true};
	return &v1;
}
audio_status_t audio_get_status(void) { return AUDIO_STATUS_PLAYING; }
void usbaudio_apply_volume(int percent) { usb_level = percent; output_notifications++; }
void swvolume_set_index(int percent) { software_level = percent; output_notifications++; }
void bluetooth_notify_volume(int percent) { bluetooth_level = percent; output_notifications++; }

#include "src/system/audio/alsa-controls.c"

static char test_path[] = "/tmp/sonix-alarm-volume-XXXXXX";
static void check_hw(int expected) {
	assert(get_volume_percent() == expected);
	assert(usb_level == expected && software_level == expected && bluetooth_level == expected);
	assert(dac_left == dac_right && dac_left == percent_to_raw(expected));
}
static long saved(const char *section, const char *key) { return config_get_int(section, key, -1); }

static void test_ramp_and_persistence(void) {
	volume_profile_init(80, 55);
	set_volume_percent(80);
	volume_profile_persist_now();
	assert(saved("player", "volume") == 80);
	assert(volume_fade_begin());
	check_hw(0);
	assert(volume_profile_level(false) == 80 && volume_fade_active());
	volume_profile_persist();
	volume_profile_persist_now();
	assert(saved("player", "volume") == 80);
	volume_fade_progress(15, 30);
	check_hw(40);
	assert(volume_profile_level(false) == 80);
	int writes = dac_writes;
	set_volume_percent(40); // screen wake or gain reapply, not a new profile
	assert(dac_writes == writes + 2 && volume_fade_active());
	set_high_gain(1);
	check_hw(40);
	assert(volume_fade_active() && volume_profile_level(false) == 80);
	set_high_gain(0);
	assert(volume_profile_level(false) == 80 && saved("player", "volume") == 80);
	volume_fade_progress(30, 30);
	assert(!volume_fade_active());
	check_hw(80);
	assert(saved("player", "volume") == 80);
}

static void test_user_takes_control(void) {
	assert(volume_fade_begin());
	volume_fade_progress(5, 10);
	check_hw(40);
	set_volume_percent(17);
	assert(!volume_fade_active());
	volume_fade_stop();
	volume_profile_persist();
	check_hw(17);
	assert(saved("player", "volume") == 17);

	// Pressing down while the ramp is at zero is still an explicit mute.
	assert(volume_fade_begin());
	check_hw(0);
	change_volume_percent(-1);
	assert(!volume_fade_active());
	volume_profile_persist();
	assert(saved("player", "volume") == 0 && !volume_fade_begin());
}

static void test_bluetooth_adoption_and_user_change(void) {
	set_volume_percent(80);
	volume_profile_set_level(VOLUME_OUTPUT_BLUETOOTH, 55);
	volume_profile_set_output(VOLUME_OUTPUT_BLUETOOTH);
	assert(volume_fade_begin());
	check_hw(0);
	// A headphone's initial connection level is not a request to cancel a
	// fade or persist a temporary level as the new Bluetooth profile.
	assert(!volume_adopt_bluetooth(72));
	check_hw(0);
	assert(volume_fade_active() && volume_profile_level(true) == 55);
	// A later real headphone adjustment remains an explicit user choice.
	assert(volume_adjust_bluetooth(23));
	assert(!volume_fade_active());
	volume_profile_persist();
	check_hw(23);
	assert(saved("wireless", "bt_volume") == 23);
	assert(volume_adopt_bluetooth(61));
	check_hw(61);
	volume_profile_set_output(VOLUME_OUTPUT_PHONES);
	check_hw(80);
	// Late connection and volume-change signals from the old route cannot
	// modify the wired profile or cancel an alarm fade on that output.
	assert(volume_fade_begin());
	assert(!volume_adopt_bluetooth(72));
	assert(!volume_adjust_bluetooth(23));
	assert(volume_fade_active() && volume_profile_level(false) == 80);
	check_hw(0);
	volume_fade_stop();
	check_hw(80);
}

static void test_output_and_lineout(void) {
	set_volume_percent(80);
	volume_profile_set_level(VOLUME_OUTPUT_BLUETOOTH, 55);
	assert(volume_fade_begin());
	volume_fade_progress(3, 10);
	volume_profile_set_output(VOLUME_OUTPUT_BLUETOOTH);
	assert(!volume_fade_active());
	check_hw(55);
	assert(saved("player", "volume") == 80 && saved("wireless", "bt_volume") == 55);
	volume_profile_set_output(VOLUME_OUTPUT_PHONES);
	check_hw(80);

	assert(volume_fade_begin());
	volume_fade_progress(3, 10);
	lineout_set(true);
	assert(lineout_is_active() && !volume_fade_active());
	check_hw(100);
	assert(!volume_fade_begin());
	set_volume_percent(20); // fixed line out refuses volume changes
	check_hw(100);
	// Playback/route unmute restores only the DAC. It must not publish a
	// stale line-out 100 to Bluetooth or overwrite the saved logical level.
	int notices = output_notifications;
	track_muted = true;
	alsa_track_unmute();
	suspend_muted = true;
	alsa_suspend_restore();
	assert(output_notifications == notices);
	check_hw(100);
	lineout_set(false);
	check_hw(80);
	assert(volume_profile_level(false) == 80);
}

static void test_limit_during_fade(void) {
	set_volume_percent(80);
	assert(volume_fade_begin());
	volume_limit_set_enabled(true);
	volume_limit_set(VOLUME_OUTPUT_PHONES, 30);
	volume_fade_progress(20, 30);
	check_hw(30);
	volume_fade_progress(30, 30);
	check_hw(30);
	assert(!volume_fade_active() && volume_profile_level(false) == 30);
	volume_profile_persist();
	assert(saved("player", "volume") == 30);
	volume_limit_set_enabled(false);
}

int main(void) {
	int fd = mkstemp(test_path);
	assert(fd >= 0);
	close(fd);
	unlink(test_path);
	(void)config_init(test_path);
	test_ramp_and_persistence();
	test_user_takes_control();
	test_bluetooth_adoption_and_user_change();
	test_output_and_lineout();
	test_limit_during_fade();
	unlink(test_path);
	puts("pet alarm volume and profiles: all checks passed");
	return 0;
}
