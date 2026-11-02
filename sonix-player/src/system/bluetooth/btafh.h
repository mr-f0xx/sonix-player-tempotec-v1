#ifndef BTAFH_H
#define BTAFH_H

#include <stdbool.h>
#include <stdint.h>

// Keeping Bluetooth's frequency hopping off the channel the Wi-Fi link is on.
//
// The radio chip is a combo (a Broadcom BCM4343A1 on the V1) with one antenna
// for both radios, and a Bluetooth stream received while Wi-Fi is associated
// loses packets that a stream with Wi-Fi idle does not ("bluealsa: Missing RTP
// packets" in the Bluetooth log, a handful a minute with the link joined and
// none with it off). Bluetooth hops over the same 2.4 GHz band the access point
// sits in, and every hop that lands on top of it is a slot the two radios have
// to fight over. The controller learns to avoid the band on its own, slowly and
// only from the errors it has already made; the host can simply tell it: HCI
// Set AFH Host Channel Classification marks the Bluetooth channels under the
// Wi-Fi channel as bad, and the piconet's master leaves them out of its hop
// set. It is the master that does it, so on a link the other end is master of
// the controller answers "command disallowed" and nothing changes.

// The 10-byte AFH map for an access point on `wifi_freq_mhz`: bit n is
// Bluetooth channel n (2402 + n MHz), 1 = may be used, 0 = avoid. A frequency
// outside the 2.4 GHz band (0 = unknown, or a 5 GHz network) gives a map with
// every channel good. A 20 MHz Wi-Fi channel is about 22 MHz wide; one more
// MHz is masked each side. Returns how many channels are masked.
int btafh_map_for_wifi(int wifi_freq_mhz, uint8_t map[10]);

// Sends the map for that frequency to hci0 as a Set AFH Host Channel
// Classification command, on a raw HCI socket. True when the command was
// written to the controller; the controller's own verdict is in the Bluetooth
// log (btmon's Command Complete for the command), not here.
bool btafh_apply_for_wifi(int wifi_freq_mhz);

#endif /* BTAFH_H */
