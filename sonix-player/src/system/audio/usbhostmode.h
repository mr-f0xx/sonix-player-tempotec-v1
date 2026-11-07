#ifndef USBHOSTMODE_H
#define USBHOSTMODE_H

#include <stdbool.h>

#include "src/system/audio/usbhostprobe.h"

// External USB audio, as a setting rather than as a diagnostic.
//
// On the TempoTec V1 a USB-C sound card does nothing on the port as the player
// leaves it. The TCS1421 sits in NormalDRP -- the dual role the stock player
// uses for a phone -- and a dongle presents nothing on the CC lines that reads
// as a request for the host role, so the controller never supplies the far end
// and nothing enumerates. Two unrelated dongles, a JCally and an ESR, were each
// invisible that way and each worked immediately once developer options forced
// Source on the TCS1421 and host on the x1600 PHY.
//
// This module is that write, kept on: [usb] host_dac in device_config.ini.
// Built on usbhostprobe.c, which does the sysfs work, the journal, the crash
// recovery and the serialisation against ADB, DAC mode and storage export.
//
// What it is not is automatic. A forced Source is a promise to supply 5 V on
// the port before anything has been identified, so it is made because the user
// asked for it, and given back when another owner needs the controller. While
// it is held the player's storage gadget stays off the port: a card reader
// cannot share it with a DAC, and the stock player does not offer one either.

// The setting, as the interface sees it. These only touch the config.
bool usbhostmode_wanted(void);
void usbhostmode_set_wanted(bool on); // writes the config; usbhostmode_tick() acts on it

// True while the port is held as host -- as opposed to the timed trial in
// developer options, which owns the port for twenty seconds at a time.
bool usbhostmode_active(void);

// True while the setting is on but the controller was found busy (ADB, DAC
// mode, an export), so the request is being repeated.
bool usbhostmode_retrying(void);

// The last thing that happened, as a translation key, for the row's own text:
// "" when there is nothing to say. The page translates it.
const char *usbhostmode_status_key(void);

// Brings the port in line with the setting. Cheap and idempotent; called from
// the USB poll in usbaudio.c, which is already asking the port what is on it.
void usbhostmode_tick(void);

// The same events the explicit trial reports, because usbaudio.c owns the poll
// and decides what the user is told. Called back by that poll.
void usbhostmode_event(usbhost_probe_event_t event);

// For the owners of the controller that cannot share it: ADB, DAC mode, a
// storage export, suspend, shutdown, a firmware update, a factory reset. The
// port is given back; `clear_setting` also switches the setting off, which is
// what a factory reset and a firmware update do. Returns whether the role
// really went back -- false means it is still held and being retried, and the
// caller must not hand the port to anything else.
bool usbhostmode_release(bool clear_setting);

// Everything a bug report needs about this setting, in the log's own voice.
void usbhostmode_log_state(const char *why);

#endif /* USBHOSTMODE_H */
