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

bool btafh_apply_for_wifi(int wifi_freq_mhz) {
	uint8_t packet[4 + 10];
	uint8_t map[10];
	int masked = btafh_map_for_wifi(wifi_freq_mhz, map);

	int fd = socket(BT_AF_BLUETOOTH, SOCK_RAW, BT_PROTO_HCI);
	if (fd < 0) {
		return false; // no Bluetooth in this kernel
	}
	(void)fcntl(fd, F_SETFD, FD_CLOEXEC);

	struct bt_sockaddr_hci addr = {BT_AF_BLUETOOTH, 0, BT_HCI_CHANNEL_RAW};
	if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
		close(fd);
		return false; // hci0 is not up
	}

	packet[0] = BT_HCI_COMMAND_PKT;
	packet[1] = (uint8_t)(BT_OP_SET_AFH_HOST_CLASS & 0xff);
	packet[2] = (uint8_t)(BT_OP_SET_AFH_HOST_CLASS >> 8);
	packet[3] = 10;
	memcpy(packet + 4, map, 10);

	ssize_t wrote = write(fd, packet, sizeof(packet));
	int kept = errno;
	close(fd);

	if (wrote != (ssize_t)sizeof(packet)) {
		fprintf(stderr, "bluetooth: AFH classification not sent (%s)\n", strerror(kept));
		return false;
	}
	if (masked) {
		fprintf(stderr, "bluetooth: AFH: %d channels under Wi-Fi at %d MHz left out of the hop set\n", masked,
				wifi_freq_mhz);
	} else {
		fprintf(stderr, "bluetooth: AFH: every channel allowed again\n");
	}
	return true;
}
