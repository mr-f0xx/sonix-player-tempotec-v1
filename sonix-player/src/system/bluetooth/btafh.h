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
// Set AFH Host Channel Classification asks the controller to mark channels
// under Wi-Fi as bad. The piconet's master controls the hop set, so if the
// other end is master, and depending on both controllers' AFH support, this
// request may not change the channels in use. A successful socket write is not
// proof that the controller accepted or applied the classification.

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

// The power tick follows the current A2DP link and associated Wi-Fi frequency.
// Zero-initialize one instance. A successfully sent restriction is refreshed
// periodically, and a failed send (including a clear) is retried. While the
// adapter is off, sends pause. A previously sent restriction is remembered so
// it can be cleared or reasserted after re-enabling even if the controller kept
// its old state during a transient radio park.
typedef struct {
	int sent_freq;
	int attempted_freq;
	uint32_t last_try_ms;
	uint32_t last_success_ms;
	bool attempted;
	bool needs_refresh;
} btafh_follow_t;

// `wifi_freq_mhz` is zero when Wi-Fi is not associated or its frequency is
// unknown. `apply` is normally btafh_apply_for_wifi; accepting it as an
// argument also permits testing the timing without Bluetooth hardware.
void btafh_follow_wifi(btafh_follow_t *state, uint32_t now_ms, bool bluetooth_enabled, bool a2dp_link,
					  int wifi_freq_mhz, bool (*apply)(int));

#endif /* BTAFH_H */
