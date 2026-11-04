#include "btafh.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

// Kernel UAPI, spelled out for the same reason bluetooth.c spells out its own:
// this rootfs has the library but not its headers.
#define BT_AF_BLUETOOTH 31
#define BT_PROTO_HCI 1
#define BT_HCI_CHANNEL_RAW 0
#define BT_HCI_COMMAND_PKT 0x01
// OGF 0x03 (controller and baseband), OCF 0x003F
#define BT_OP_SET_AFH_HOST_CLASS ((0x03 << 10) | 0x003F)

#define BT_CHANNELS 79
#define WIFI_HALF_WIDTH_MHZ 12

struct bt_sockaddr_hci {
	unsigned short hci_family;
	unsigned short hci_dev;
	unsigned short hci_channel;
};

int btafh_map_for_wifi(int wifi_freq_mhz, uint8_t map[10]) {
	int masked = 0;

	memset(map, 0, 10);
	for (int n = 0; n < BT_CHANNELS; n++) {
		int mhz = 2402 + n;
		bool under_wifi = wifi_freq_mhz >= 2400 && wifi_freq_mhz <= 2500 &&
						  mhz >= wifi_freq_mhz - WIFI_HALF_WIDTH_MHZ && mhz <= wifi_freq_mhz + WIFI_HALF_WIDTH_MHZ;
		if (under_wifi) {
			masked++;
		} else {
			map[n / 8] |= (uint8_t)(1u << (n % 8));
		}
	}
	// A controller needs at least twenty channels to hop over. A 20 MHz network
	// masks 25 and leaves 54; this holds only if something odd was passed in.
	if (BT_CHANNELS - masked < 20) {
		memset(map, 0, 10);
		for (int n = 0; n < BT_CHANNELS; n++) {
			map[n / 8] |= (uint8_t)(1u << (n % 8));
		}
		masked = 0;
	}
	return masked;
}

// A failed socket/bind/write is retried by the follower. Report the first
// failure (and a changed error) without printing the same line every second.
static unsigned last_hci_error;

static void report_hci_error(unsigned stage, const char *action, int error) {
	unsigned signature = (stage << 16) | ((unsigned)error & 0xffffu);
	if (__atomic_exchange_n(&last_hci_error, signature, __ATOMIC_RELAXED) != signature) {
		fprintf(stderr, "bluetooth: AFH %s failed (%s); will retry\n", action, strerror(error));
	}
}

bool btafh_apply_for_wifi(int wifi_freq_mhz) {
	uint8_t packet[4 + 10];
	uint8_t map[10];
	int masked = btafh_map_for_wifi(wifi_freq_mhz, map);

	int fd = socket(BT_AF_BLUETOOTH, SOCK_RAW, BT_PROTO_HCI);
	if (fd < 0) {
		report_hci_error(1, "socket", errno);
		return false;
	}
	(void)fcntl(fd, F_SETFD, FD_CLOEXEC);

	struct bt_sockaddr_hci addr = {BT_AF_BLUETOOTH, 0, BT_HCI_CHANNEL_RAW};
	if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
		int kept = errno;
		close(fd);
		report_hci_error(2, "bind", kept);
		return false;
	}

	packet[0] = BT_HCI_COMMAND_PKT;
	packet[1] = (uint8_t)(BT_OP_SET_AFH_HOST_CLASS & 0xff);
	packet[2] = (uint8_t)(BT_OP_SET_AFH_HOST_CLASS >> 8);
	packet[3] = 10;
	memcpy(packet + 4, map, 10);

	ssize_t wrote = write(fd, packet, sizeof(packet));
	int kept = wrote < 0 ? errno : EIO; // a short write may leave errno unchanged
	close(fd);

	if (wrote != (ssize_t)sizeof(packet)) {
		report_hci_error(3, "write", kept);
		return false;
	}
	__atomic_store_n(&last_hci_error, 0, __ATOMIC_RELAXED);
	// A successful write is not the controller's acceptance. In particular,
	// the other end may be the piconet master. btmon records the actual HCI
	// Command Complete; do not claim that the hop set has changed here.
	if (masked) {
		fprintf(stderr, "bluetooth: AFH: requested avoidance of %d channels under Wi-Fi at %d MHz; "
						"check HCI Command Complete\n", masked, wifi_freq_mhz);
	} else {
		fprintf(stderr, "bluetooth: AFH: requested all channels; check HCI Command Complete\n");
	}
	return true;
}

#define AFH_RESEND_MS 15000u
#define AFH_RETRY_MS 1000u

void btafh_follow_wifi(btafh_follow_t *state, uint32_t now_ms, bool bluetooth_enabled, bool a2dp_link,
					  int wifi_freq_mhz, bool (*apply)(int)) {
	if (!state || !apply) {
		return;
	}
	if (!bluetooth_enabled) {
		// A transient radio park may leave the transport alive, so do not
		// assume its classification was reset. Avoid writes while off, but
		// clear or reassert any restriction immediately after it comes back.
		state->needs_refresh = state->sent_freq != 0;
		state->attempted = false;
		return;
	}

	// A 5 GHz or unknown Wi-Fi channel needs the unrestricted map, just like
	// a disconnected link. Keep only frequencies which actually mask bits.
	int want = a2dp_link && wifi_freq_mhz >= 2400 && wifi_freq_mhz <= 2500 ? wifi_freq_mhz : 0;
	if (want == 0 && state->sent_freq == 0) {
		return;
	}
	if (want == state->sent_freq && !state->needs_refresh &&
		(uint32_t)(now_ms - state->last_success_ms) < AFH_RESEND_MS) {
		return;
	}
	// A changed target, including a clear, is sent immediately. Retries for
	// the same target are bounded so a failed HCI write cannot spin the UI.
	if (state->attempted && want == state->attempted_freq &&
		(uint32_t)(now_ms - state->last_try_ms) < AFH_RETRY_MS) {
		return;
	}
	state->attempted = true;
	state->attempted_freq = want;
	state->last_try_ms = now_ms;
	if (apply(want)) {
		state->sent_freq = want;
		state->last_success_ms = now_ms;
		state->needs_refresh = false;
	}
}
