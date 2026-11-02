/* The AFH map for a Wi-Fi channel: which Bluetooth channels are masked, that the
 * rest stay usable, and that nothing outside the 2.4 GHz band masks anything.
 * Plain C; the HCI write is not exercised here (it needs a controller). */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "src/system/bluetooth/btafh.h"

static int bit(const uint8_t *map, int n) { return (map[n / 8] >> (n % 8)) & 1; }

static int good(const uint8_t *map) {
	int count = 0;
	for (int n = 0; n < 79; n++) {
		count += bit(map, n);
	}
	return count;
}

int main(void) {
	uint8_t map[10];

	/* Wi-Fi channel 6, 2437 MHz: Bluetooth 2425..2449 MHz are channels 23..47. */
	int masked = btafh_map_for_wifi(2437, map);
	assert(masked == 25);
	assert(good(map) == 54);
	assert(bit(map, 22) == 1 && bit(map, 23) == 0 && bit(map, 47) == 0 && bit(map, 48) == 1);
	assert(bit(map, 0) == 1 && bit(map, 78) == 1);
	assert(map[9] == 0x7f); /* channel 79 does not exist and stays 0 */

	/* Channel 1, 2412 MHz: the mask is cut off by the bottom of the band. */
	masked = btafh_map_for_wifi(2412, map);
	assert(masked == 23 && bit(map, 0) == 0 && bit(map, 22) == 0 && bit(map, 23) == 1);

	/* Channel 13, 2472 MHz: cut off by the top. */
	masked = btafh_map_for_wifi(2472, map);
	assert(bit(map, 78) == 0 && bit(map, 58) == 0 && bit(map, 57) == 1 && good(map) >= 20);

	/* Unknown and 5 GHz: every channel is good. */
	for (int i = 0; i < 2; i++) {
		masked = btafh_map_for_wifi(i ? 5180 : 0, map);
		assert(masked == 0 && good(map) == 79);
	}

	printf("PASS btafh (a 2.4 GHz channel masks the Bluetooth channels under it and no others)\n");
	return 0;
}
