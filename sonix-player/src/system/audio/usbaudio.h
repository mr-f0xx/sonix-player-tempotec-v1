#ifndef USBAUDIO_H
#define USBAUDIO_H

#include <stdbool.h>
#include <stddef.h>

// Audio out of the USB-C port: a DAC or a pair of USB-C headphones, driven as
// a USB Audio Class device with this player as the host.
//
// Watch for a USB sound card and route playback to it. When the connector is
// known to be empty, allow its automatic dual-role mode. A live V1 can expose
// TCS1421 settings without a usable OTG-ID signal; VBUS alone cannot tell a
// charger/computer from an unenumerated DAC, and dual role is not proof that
// this board will supply or enumerate one. See the explicit opt-in host trial
// in usbhostprobe.h for diagnostic retries on supported firmware.

// Restore dual-role mode only when the port appears empty. Returns whether it
// was already set or the request succeeded; false also means the port is in
// use, unknown, or no supported role attribute is available.
bool usbaudio_init(void);

// The same write, made again whenever the port is found not to be dual. The
// setting does not stay put on its own -- see the .c -- so the poll re-asserts
// it rather than trusting the one at startup.
bool usbaudio_ensure_dual(void);

// Looks for a USB sound card and points playback at it, or takes playback back
// when it goes. Cheap: one readdir of /sys/class/sound, and it only acts when
// what it finds has changed. Called from the same once-a-second poll that
// watches the headphone jacks.
void usbaudio_poll(void);

// Whether something has just attached to the port and has not yet turned into a
// sound card or been given up on. Anything else about to write the port's role
// should wait while this is true: a bus-powered dongle spends its first seconds
// being powered up by the host role that a "sink" write would take away.
bool usbaudio_handover_pending(void);

// Whether the sound is going out over the port right now.
bool usbaudio_active(void);

// The card's name ("Headset", "USB Audio DAC"...), empty when there is none.
void usbaudio_card_name(char *out, size_t out_size);

// The volume, on the USB device's own control. The CS43198's attenuation is
// not in the path any more once the audio leaves as USB packets, so the
// built-in volume does nothing and this is what has to move instead. No-op
// when nothing is attached, or when the device exposes no volume control --
// some do not, and then the level belongs to whatever is on the far end.
void usbaudio_apply_volume(int percent);

// Whether the device on the port has such a control. False also when the port
// is empty. When it is false and the port is busy, nothing in the chain holds
// the level any more and the software volume takes it over -- see swvolume.c.
bool usbaudio_has_volume_control(void);

// ---------------------------------------------------------------------------
// Compatibility mode: 48 kHz, 16 bit, over the port
//
// The one thing this file cannot do for a picky DAC is make it accept a stream
// it does not understand. Plenty of desktop DACs on the end of a Type-C adapter
// present a single fixed-rate, 16-bit interface -- 48 kHz is the common one --
// and the V1's usual "open the card at the track's own rate and depth" is then
// a stream the device refuses: the PCM does not open, and the track plays to
// nothing with a card that looks healthy in the interface.
//
// So there is a fallback: a switch in Developer options, beside the external
// USB DAC setting, and audio.c turns it on by itself when a card refuses the
// track's own format and takes 48 kHz/16 bit instead (see open_pcm_device_now).
// Every stream to the port is then opened at 48 kHz, 16 bit, and the track is
// converted to it by a plug of the player's own, in front of the card. Nothing
// else changes -- the route, the level, the glyph all behave as before -- and
// the attach popup mentions the switch the first time a card appears, which is
// the moment a picky DAC is met.
//
// Off by default, and remembered per player rather than per device: a DAC that
// needs it has no way of saying so before it has been tried.
bool usbaudio_compat_mode(void);
// The plain setter: what audio.c calls when a card has just refused the track's
// own format. config_save() is left to the poll, which runs on the GUI thread,
// because the config file's writer is not one that can be shared between the
// playback thread and the interface.
void usbaudio_set_compat_mode(bool on);
// The settings row's switch, which saves on the spot: it is already on the GUI
// thread and the user is owed the write before the page is left.
void usbaudio_set_compat_mode_saved(bool on);
// Writes any change that is published but not yet saved. Called by the poll;
// exported for the settings row.
void usbaudio_flush_compat(void);

// Whether compatibility mode is what is in force for the stream right now,
// which is the setting AND a USB card actually being the output.
bool usbaudio_compat_active(void);

// The rate and depth the port is opened at when compatibility mode is on.
#define USBAUDIO_COMPAT_RATE 48000
#define USBAUDIO_COMPAT_BITS 16

#endif /* USBAUDIO_H */
