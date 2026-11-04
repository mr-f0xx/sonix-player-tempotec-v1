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

#endif /* USBAUDIO_H */
