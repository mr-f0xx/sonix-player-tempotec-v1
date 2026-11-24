// test_usbaudio_compat.c -- the 48 kHz/16 bit compatibility setting.
//
// The problem this is for is a DAC that presents one fixed format and refuses
// everything else: the open fails, and by the rules in audio.c a USB open that
// fails does not fall back to the jacks -- it waits for the card. A DAC like
// that is a card that never stops waiting, so the setting exists to pin the
// format at something such a device takes, and audio.c converts the track to it
// through a plug of the player's own. See usbaudio.h and open_usb_compat().
//
// What is checked here is everything about the setting that can be checked
// without a card: where it is stored, that the default is off, that the
// getter reads the config rather than a copy, and that "in force for the
// stream" is the setting AND a card actually being there -- a setting that
// claims to be converting while the jacks are playing would have the interface
// lie about the format.
//
// Compiled against the real usbaudio.c, with the same stubs the volume test
// uses for everything below the setting.

#define _GNU_SOURCE
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ---------------------------------------------------------------------------
// The config, as a two-cell store: one cell for the compatibility key, one for
// everything else. Enough to prove which section/key the setting reads and
// writes, which is the whole subject.
// ---------------------------------------------------------------------------

#define SOUND_CLASS "/nonexistent/sound"
#define USB_AUDIO_BUS "/nonexistent/usb"
#define TYPEC_PORT_TYPE "/nonexistent/typec"
#define TYPEC_PARTNER "/nonexistent/partner"
#define TCS1421_CFG "/nonexistent/tcs"
#define DWC2_OTG_ID "/nonexistent/id"

static char compat_section[32];
static char compat_key[64];
static bool compat_value = false; // what config_get_bool answers
static int compat_writes;
static int config_saves;

const char *config_get(const char *section, const char *key, const char *fallback) {
	// The rest of usbaudio.c asks the config nothing at all; only the setting
	// does. Answer the defaults for anything that is not it.
	if (strcmp(section, compat_section) == 0 && strcmp(key, compat_key) == 0) {
		return compat_value ? "1" : "0";
	}
	return fallback;
}

bool config_get_bool(const char *section, const char *key, bool fallback) {
	if (strcmp(section, compat_section) == 0 && strcmp(key, compat_key) == 0) {
		return compat_value;
	}
	return fallback;
}

long config_get_int(const char *section, const char *key, long fallback) {
	(void)section;
	(void)key;
	return fallback;
}

void config_set_bool(const char *section, const char *key, bool value) {
	// The section and key the setter must use are the getter's: a setting
	// written under another name is one that reads back as its default.
	if (strcmp(section, compat_section) != 0 || strcmp(key, compat_key) != 0) {
		fprintf(stderr, "FAIL: the setting wrote %s/%s\n", section, key);
		exit(1);
	}
	compat_value = value;
	compat_writes++;
}

void config_set(const char *section, const char *key, const char *value) {
	(void)section;
	(void)key;
	(void)value;
}
void config_set_int(const char *section, const char *key, long value) {
	(void)section;
	(void)key;
	(void)value;
}
bool config_save(void) {
	config_saves++;
	return true;
}

// ---------------------------------------------------------------------------
// The rest of what usbaudio.c links against
// ---------------------------------------------------------------------------

void swvolume_set_usb_gain(long db100, bool mute) {
	(void)db100;
	(void)mute;
}
void swvolume_clear_usb_gain(void) {}
bool swvolume_active(void) { return false; }
void swvolume_set_index(int percent) { (void)percent; }

bool usb_vbus_present(void) { return false; }
bool usbhostprobe_active(void) { return false; }
void usbhostprobe_recover(void) {}
bool bluetooth_audio_active(void) { return false; }
void usbhostmode_tick(void) {}
int get_volume_percent(void) { return 50; }
void audio_set_output_device(const char *pcm) { (void)pcm; }
long alsa_volume_gain_db100(int percent) { return -(100 - percent) * 60; }

// The notices, captured rather than shown: what the poll says is part of what
// it does. Both are called through gui_notify_popup() at the call sites.
static char last_popup[256];
static int popups;
void gui_notify_popup(const char *text) {
	snprintf(last_popup, sizeof(last_popup), "%s", text);
	popups++;
}
void gui_notify_popup_long(const char *text) { gui_notify_popup(text); }

// tr() returns the tag, as a language that has not been translated would.
const char *tr(const char *text) { return text ? text : ""; }

// The mixer, as a card that is not there: every open fails, so the level code
// returns before it reads anything. Stubbed only because usbaudio_poll() --
// which this test calls for its deferred save -- reaches find_volume_control()
// through the same object file; the volume law has its own test.
int snd_ctl_open() { return -1; }
int snd_ctl_close() { return 0; }
int snd_ctl_elem_list() { return -1; }
int snd_ctl_elem_list_alloc_space() { return -1; }
void snd_ctl_elem_list_free_space() {}
unsigned int snd_ctl_elem_list_get_count() { return 0; }
void snd_ctl_elem_list_get_id() {}
int snd_ctl_elem_info() { return -1; }
int snd_ctl_elem_info_get_type() { return 0; }
int snd_ctl_elem_info_is_writable() { return 0; }
int snd_ctl_elem_info_is_tlv_readable() { return 0; }
unsigned int snd_ctl_elem_info_get_count() { return 1; }
long snd_ctl_elem_info_get_min() { return 0; }
long snd_ctl_elem_info_get_max() { return 0; }
const char *snd_ctl_elem_info_get_name() { return ""; }
void snd_ctl_elem_info_set_id() {}
void snd_ctl_elem_id_set_interface() {}
void snd_ctl_elem_id_set_name() {}
int snd_ctl_elem_tlv_read() { return -1; }
int snd_ctl_elem_read() { return -1; }
int snd_ctl_elem_write() { return -1; }
void snd_ctl_elem_value_set_id() {}
void snd_ctl_elem_value_set_integer() {}
long snd_ctl_elem_value_get_integer() { return 0; }
int snd_tlv_get_dB_range() { return -1; }
int snd_tlv_convert_from_dB() { return -1; }
int snd_tlv_convert_to_dB() { return -1; }

#include "../src/system/audio/usbaudio.c"

// ---------------------------------------------------------------------------
// The cases
// ---------------------------------------------------------------------------

static void test_the_setting_is_off_until_it_is_asked_for(void) {
	compat_value = false;
	assert(usbaudio_compat_mode() == false);

	// And the defaults name the key the hardware_volume lives beside: one
	// section for the port's own settings.
	assert(strcmp(COMPAT_SECTION, "usb") == 0);
	assert(strcmp(COMPAT_KEY, "compat_48k") == 0);
}

static void test_the_setting_reads_through(void) {
	// Switched on somewhere else -- audio.c's fallback does this on the
	// playback thread -- the getter must see it on the next ask, not at the
	// next boot. That is why it is not cached across a run.
	compat_value = true;
	assert(usbaudio_compat_mode() == true);
	compat_value = false;
	assert(usbaudio_compat_mode() == false);
}

static void test_the_playback_thread_writes_nothing(void) {
	// audio.c's fallback, on the playback thread. The config's entries and
	// its file belong to the GUI thread, so this call must touch neither --
	// but the change has to be visible to the very next ask, which is the
	// track audio.c is about to open.
	compat_value = false;
	compat_writes = 0;
	config_saves = 0;
	atomic_store(&compat_pending, -1);
	atomic_store(&compat_save_pending, false);

	usbaudio_set_compat_mode(true);
	assert(usbaudio_compat_mode() == true);
	assert(compat_writes == 0);
	assert(config_saves == 0);
	// ...and the config still says what it said.
	assert(compat_value == false);

	// The poll is what makes it durable, on the GUI thread.
	usbaudio_poll();
	assert(compat_value == true);
	assert(compat_writes == 1);
	assert(config_saves >= 1);
	// Written once, not at every poll afterwards.
	compat_writes = 0;
	usbaudio_poll();
	assert(compat_writes == 0);

	// Asking for what is already set is not a write.
	usbaudio_set_compat_mode(true);
	assert(atomic_load(&compat_pending) == -1);
}

static void test_the_row_saves_on_the_spot(void) {
	compat_value = false;
	compat_writes = 0;
	config_saves = 0;
	atomic_store(&compat_pending, -1);
	atomic_store(&compat_save_pending, false);

	usbaudio_set_compat_mode_saved(true);
	assert(usbaudio_compat_mode() == true);
	assert(compat_value == true);
	assert(compat_writes == 1);
	assert(config_saves == 1);
	// Nothing left for the poll to write.
	assert(atomic_load(&compat_pending) == -1);
	assert(atomic_load(&compat_save_pending) == false);

	// And off again, the same way.
	config_saves = 0;
	usbaudio_set_compat_mode_saved(false);
	assert(usbaudio_compat_mode() == false);
	assert(compat_value == false);
	assert(config_saves == 1);
}

static void test_a_pending_change_reads_through_before_it_is_saved(void) {
	// The order matters: published first, durable second. Between the two --
	// which is the whole life of a track's open on the playback thread -- the
	// getter must answer with the new value or the fallback that switched it
	// on would be ignored by the very open it was meant to fix.
	compat_value = false;
	atomic_store(&compat_pending, 1);
	assert(usbaudio_compat_mode() == true);
	atomic_store(&compat_pending, 0);
	assert(usbaudio_compat_mode() == false);
	atomic_store(&compat_pending, -1);
	assert(usbaudio_compat_mode() == false);
}

static void test_in_force_only_while_a_card_is_the_output(void) {
	compat_value = true;
	active_card = -1;
	assert(usbaudio_compat_active() == false); // the jacks are playing

	active_card = 2;
	assert(usbaudio_compat_active() == true); // a USB card is

	// And not while the setting is off, whatever is attached.
	compat_value = false;
	assert(usbaudio_compat_active() == false);
	active_card = -1;
}

static void test_the_attach_notice_names_the_card(void) {
	// The dual-role attach had no notice at all: the status bar's glyph was
	// the whole of it. It is the moment a picky DAC is met, so the hint about
	// compatibility belongs on the same line -- and only while it is off.
	active_card = 4;
	snprintf(active_name, sizeof(active_name), "USB Audio DAC");

	compat_value = false;
	popups = 0;
	announce_card(false);
	assert(popups == 1);
	assert(strstr(last_popup, "usbaudio_connected") != NULL);
	assert(strstr(last_popup, "USB Audio DAC") != NULL);
	assert(strstr(last_popup, "usbaudio_compat_hint") != NULL);

	// Already on: no advice, it is doing the thing.
	popups = 0;
	announce_card(true);
	assert(popups == 1);
	assert(strstr(last_popup, "usbaudio_compat_hint") == NULL);

	// A card with no id still reads as a sentence.
	active_name[0] = '\0';
	announce_card(true);
	assert(strcmp(last_popup, tr("usbaudio_connected_plain")) == 0);
}

int main(void) {
	snprintf(compat_section, sizeof(compat_section), "%s", "usb");
	snprintf(compat_key, sizeof(compat_key), "%s", "compat_48k");

	test_the_setting_is_off_until_it_is_asked_for();
	test_the_setting_reads_through();
	test_the_playback_thread_writes_nothing();
	test_the_row_saves_on_the_spot();
	test_a_pending_change_reads_through_before_it_is_saved();
	test_in_force_only_while_a_card_is_the_output();
	test_the_attach_notice_names_the_card();

	printf("PASS USB audio compatibility mode (the setting, its save, and when it is in force)\n");
	return 0;
}
