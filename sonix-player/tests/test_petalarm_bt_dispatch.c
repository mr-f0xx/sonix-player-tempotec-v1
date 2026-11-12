/* The production Bluetooth job dispatch, with the bus and bridge faked. */
#define _GNU_SOURCE 1
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "src/system/bluetooth/bluetooth.c"

static int bridge_calls, bridge_level, fallback_notes, fallback_level, fallback_finishes, mode_calls, volume_calls;
static bool bridge_ready, post_new_during_mode, bridge_online_during_mode, disconnect_during_mode;
static char fallback_mac[BT_MAC_MAX];

void btvolume_notify_local(int percent) { bridge_calls++; bridge_level = percent; }
bool btvolume_available(void) { return bridge_ready; }
void btvolume_note_fallback_write(const char *mac, int percent) {
	fallback_notes++;
	fallback_level = percent;
	snprintf(fallback_mac, sizeof(fallback_mac), "%s", mac);
}
void btvolume_finish_fallback_write(const char *mac) {
	assert(!strcmp(mac, "20:04:84:3A:6C:FE") && volume_calls == 1);
	fallback_finishes++;
}
bool btstack_pcm_softvolume_set(const char *mac, bool receiving, bool soft) {
	assert(!strcmp(mac, "20:04:84:3A:6C:FE") && !receiving && !soft);
	mode_calls++;
	if (post_new_during_mode) bluetooth_notify_volume(22);
	if (bridge_online_during_mode) bridge_ready = true;
	if (disconnect_during_mode) g_a2dp_connected = false;
	return true;
}
bool btstack_pcm_volume_set(const char *mac, bool receiving, int packed) {
	assert(!strcmp(mac, "20:04:84:3A:6C:FE") && !receiving);
	assert(fallback_notes == 1 && fallback_level == 40); // registered before send
	assert(packed == ((51 << 8) | 51));
	volume_calls++;
	return true;
}

int main(void) {
	g_enabled = false;
	g_a2dp_connected = true;
	g_pending_volume = -1;
	queue_head = queue_count = 0;
	bluetooth_notify_volume(5);
	assert(!bridge_calls && !queue_count && g_pending_volume == -1);

	g_enabled = true;
	snprintf(g_a2dp_mac, sizeof(g_a2dp_mac), "%s", "20:04:84:3A:6C:FE");
	bluetooth_notify_volume(0);
	assert(bridge_calls == 1 && bridge_level == 0 && queue_count == 1 && g_pending_volume == 0);
	assert(queue[queue_head].type == JOB_VOLUME);
	bluetooth_notify_volume(40);
	assert(bridge_calls == 2 && bridge_level == 40 && queue_count == 1 && g_pending_volume == 40);

	g_volume_sync = true;
	// JOB_VOLUME has taken the pending level off the queue.
	g_pending_volume = -1;
	queue_head = queue_count = 0;
	bridge_ready = true;
	do_set_volume(40);
	assert(!mode_calls && !volume_calls && !fallback_notes && bridge_calls == 2);
	bridge_ready = false;
	do_set_volume(40);
	assert(mode_calls == 1 && volume_calls == 1 && fallback_notes == 1 && fallback_finishes == 1);
	assert(!strcmp(fallback_mac, g_a2dp_mac) && bridge_calls == 2);

	// A later choice, bridge becoming ready, or disconnect during the slow
	// mode call all prevent the old level from being sent after that event.
	post_new_during_mode = true;
	do_set_volume(40);
	assert(mode_calls == 2 && volume_calls == 1 && g_pending_volume == 22 && bridge_level == 22);
	post_new_during_mode = false;
	g_pending_volume = -1;
	queue_head = queue_count = 0;
	bridge_online_during_mode = true;
	do_set_volume(40);
	assert(mode_calls == 3 && volume_calls == 1);
	bridge_online_during_mode = bridge_ready = false;
	disconnect_during_mode = true;
	do_set_volume(40);
	assert(mode_calls == 4 && volume_calls == 1 && fallback_finishes == 1);
	puts("pet alarm Bluetooth dispatch: all checks passed");
	return 0;
}
