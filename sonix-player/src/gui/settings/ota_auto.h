#ifndef OTA_AUTO_H
#define OTA_AUTO_H

#include <stdbool.h>

// The on-device update notice: "a new version of Sonix Player is available".
//
// Runs the release check (ota.h) on Wi-Fi connect and again every few hours
// while the radio is connected, and -- when a newer release shows up --
// blinks the LED (led.h), puts an accent dot in the top status bar (topbar.h,
// tappable), and brings up a modal card with the option to download the update
// right away (fwupdate_start_download()) or to later. "Later" persists the
// tag, keeps the standing dot (a reminder that says tap me), and brings up a
// transient toast; the dot's tap re-brings the same card. The switch lives in
// Settings -> System (system.auto_update_notices in device_config.ini); when it
// is off the notice is disabled and the dot hidden.
//
// The dismissal is remembered per release tag, so a deferred tag does not
// re-notify on later boots; a newer tag always re-notifies. Likewise, once the
// user has made the update, that tag is treated as handled on disk: no dot,
// no LED blink and no popup come back for it, this boot or any later one
// (ota_auto_release_taken).
//
// Interface thread only, except ota_auto_link_online(), which the Wi-Fi
// worker calls on every successful link poll (the same moment the clock asks
// for network time).

// Called once from main() before gui_init(), so the Settings toggle reads its
// real state while the interface is built. Idempotent.
void ota_auto_init(void);

// Called from the Wi-Fi worker on a successful link poll. Debounced: at most
// one check per few minutes, however often the poll fires.
void ota_auto_link_online(void);

// The user has made the update for the release currently in ota_release()
// (the download succeeded and install began, or the update is starting from
// the SD card). Hide the standing dot and bring the notice up for this tag
// no more, this boot or any later one: the tag is remembered on disk, so a
// restart does not re-notify for a release that is already made. Any remembered
// deferral for this release is dropped as well.
void ota_auto_release_taken(void);

// Whether the notice switch is on.
bool ota_auto_enabled(void);
void ota_auto_set_enabled(bool on);

#endif /* OTA_AUTO_H */
