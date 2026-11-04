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

static int sends;
static int last_freq;
static bool fail_send;

static bool fake_apply(int freq) {
	sends++;
	last_freq = freq;
	return !fail_send;
}

static void test_follow(void) {
	btafh_follow_t follow = {0};
	sends = 0;
	fail_send = false;

	/* No A2DP, no known frequency, and 5 GHz must not apply a restriction. */
	btafh_follow_wifi(&follow, 0, true, false, 2437, fake_apply);
	btafh_follow_wifi(&follow, 1, true, true, 0, fake_apply);
	btafh_follow_wifi(&follow, 2, true, true, 5180, fake_apply);
	assert(sends == 0);

	/* A connected link sends immediately; unchanged state refreshes after
	 * fifteen seconds so a reset controller gets the restriction back. */
	btafh_follow_wifi(&follow, 100, true, true, 2437, fake_apply);
	assert(sends == 1 && last_freq == 2437 && follow.sent_freq == 2437);
	btafh_follow_wifi(&follow, 15099, true, true, 2437, fake_apply);
	assert(sends == 1);
	btafh_follow_wifi(&follow, 15100, true, true, 2437, fake_apply);
	assert(sends == 2 && last_freq == 2437);

	/* Roaming sends the new target immediately, even inside the retry period.
	 * Failure leaves the old map recorded and retries the new one at 1 s. */
	fail_send = true;
	btafh_follow_wifi(&follow, 15110, true, true, 2412, fake_apply);
	assert(sends == 3 && last_freq == 2412 && follow.sent_freq == 2437);
	btafh_follow_wifi(&follow, 16109, true, true, 2412, fake_apply);
	assert(sends == 3);
	fail_send = false;
	btafh_follow_wifi(&follow, 16110, true, true, 2412, fake_apply);
	assert(sends == 4 && follow.sent_freq == 2412);

	/* Losing the incoming or outgoing link clears the map. In particular a
	 * failed clear must not be recorded as success while the controller lives. */
	fail_send = true;
	btafh_follow_wifi(&follow, 16120, true, false, 2412, fake_apply);
	assert(sends == 5 && last_freq == 0 && follow.sent_freq == 2412);
	btafh_follow_wifi(&follow, 17119, true, false, 2412, fake_apply);
	assert(sends == 5);
	fail_send = false;
	btafh_follow_wifi(&follow, 17120, true, false, 2412, fake_apply);
	assert(sends == 6 && last_freq == 0 && follow.sent_freq == 0);
	btafh_follow_wifi(&follow, 50000, true, false, 2412, fake_apply);
	assert(sends == 6);

	/* A 5 GHz roam also clears. Off pauses sends but remembers the old map:
	 * a transient park may not reset it. On re-enable the same restriction is
	 * reasserted even inside the 15 s refresh window. */
	btafh_follow_wifi(&follow, 50001, true, true, 2437, fake_apply);
	btafh_follow_wifi(&follow, 50002, true, true, 5180, fake_apply);
	assert(sends == 8 && last_freq == 0);
	btafh_follow_wifi(&follow, 50003, true, true, 2437, fake_apply);
	btafh_follow_wifi(&follow, 50004, false, true, 2437, fake_apply);
	assert(sends == 9 && follow.sent_freq == 2437 && follow.needs_refresh && !follow.attempted);
	btafh_follow_wifi(&follow, 50005, true, true, 2437, fake_apply);
	assert(sends == 10 && last_freq == 2437 && !follow.needs_refresh);

	/* If it comes back with no link, clear the potentially retained map; a
	 * failed clear is retried and only success forgets the restriction. */
	btafh_follow_wifi(&follow, 50006, false, true, 2437, fake_apply);
	fail_send = true;
	btafh_follow_wifi(&follow, 50007, true, false, 2437, fake_apply);
	assert(sends == 11 && last_freq == 0 && follow.sent_freq == 2437);
	btafh_follow_wifi(&follow, 51006, true, false, 2437, fake_apply);
	assert(sends == 11);
	fail_send = false;
	btafh_follow_wifi(&follow, 51007, true, false, 2437, fake_apply);
	assert(sends == 12 && last_freq == 0 && follow.sent_freq == 0 && !follow.needs_refresh);
	btafh_follow_wifi(&follow, 51008, false, false, 0, fake_apply);
	btafh_follow_wifi(&follow, 51009, true, false, 0, fake_apply);
	assert(sends == 12);

	/* Unsigned elapsed time still works over the millisecond counter wrap. */
	memset(&follow, 0, sizeof(follow));
	sends = 0;
	btafh_follow_wifi(&follow, UINT32_MAX - 500, true, true, 2437, fake_apply);
	btafh_follow_wifi(&follow, 14498, true, true, 2437, fake_apply);
	assert(sends == 1);
	btafh_follow_wifi(&follow, 14499, true, true, 2437, fake_apply);
	assert(sends == 2);
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

	test_follow();
	printf("PASS btafh (channel map, link changes, retries, clears, refresh and clock wrap)\n");
	return 0;
}
