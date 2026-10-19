// Include the worker implementation to inspect queued payloads without a radio.
#include "src/system/bluetooth/bluetooth.c"
#include <assert.h>

int main(void) {
	const char *mac = "AA:BB:CC:DD:EE:FF";
	assert(!bluetooth_rename(NULL, "Headphones"));
	assert(!bluetooth_rename(mac, NULL));
	assert(!bluetooth_rename(mac, " \t\r\n"));
	char oversized[BT_NAME_MAX + 1];
	memset(oversized, 'x', sizeof(oversized) - 1);
	oversized[sizeof(oversized) - 1] = '\0';
	assert(!bluetooth_rename(mac, oversized));
	assert(queue_count == 0);
	assert(bluetooth_rename(mac, "  Café headphones \t"));
	assert(queue_count == 1);
	assert(queue[0].type == JOB_RENAME);
	assert(strcmp(queue[0].mac, mac) == 0);
	assert(strcmp(queue[0].text, "Café headphones") == 0);
	assert(g_op == BT_OP_BUSY);
	puts("Bluetooth rename validation and queue tests passed");
	return 0;
}
