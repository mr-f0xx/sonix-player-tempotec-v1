/* The real Bluetooth signal cache, without a D-Bus daemon or hardware. */
#include "../src/system/bluetooth/btstack.c"

#include <assert.h>

static char fake_connection;
static bool deliver_late_signal;
static const char *address = "20:04:84:3A:6C:FE";
static const char *transport_path = "/org/bluez/hci0/dev_20_04_84_3A_6C_FE/fd0";

/* dbus_disconnect joins the reader. Simulate a signal already in flight while
 * it is joining, which must not resurrect a PCM after btstack_close returns. */
void dbus_disconnect(dbus_conn_t *c) {
	assert(c == (dbus_conn_t *)(void *)&fake_connection);
	if (deliver_late_signal) {
		pthread_mutex_lock(&lock);
		pcm_add(rx_addr, &rx_count, address);
		transport_note(transport_path, "active");
		snprintf(media_status, sizeof(media_status), "playing");
		snprintf(media_track.title, sizeof(media_track.title), "late track");
		pthread_mutex_unlock(&lock);
	}
}

static void populate(void) {
	pthread_mutex_lock(&lock);
	pcm_add(pcm_addr, &pcm_count, address);
	pcm_add(rx_addr, &rx_count, address);
	transport_note(transport_path, "active");
	snprintf(media_status, sizeof(media_status), "playing");
	snprintf(media_track.title, sizeof(media_track.title), "old track");
	pthread_mutex_unlock(&lock);
}

static void test_media_ownership_and_races(void) {
	const char *player = "/org/bluez/hci0/dev_20_04_84_3A_6C_FE/player0";
	const char *other = "/org/bluez/hci0/dev_AA_BB_CC_DD_EE_FF/player0";
	pthread_mutex_lock(&lock);
	pcm_add(rx_addr, &rx_count, address);
	media_sync_owner_locked();
	pthread_mutex_unlock(&lock);
	assert(media_path_is_current(player) && !media_path_is_current(other));
	assert(!media_path_is_current("/org/bluez/dev_X"));
	btstack_track_t first = {0}, newer = {0}, stale = {0}, out;
	snprintf(first.title, sizeof(first.title), "first");
	snprintf(newer.title, sizeof(newer.title), "newer");
	snprintf(stale.title, sizeof(stale.title), "stale reply");
	media_signal_track(player, &first);
	media_signal_track(other, &stale);
	assert(btstack_media_track(&out) && strcmp(out.title, "first") == 0);
	unsigned track_at = media_track_revision, status_at = media_status_revision;
	media_signal_track(player, &newer);
	media_signal_status(player, "playing");
	apply_media_reply(address, &stale, true, track_at, "paused", true, status_at);
	assert(btstack_media_track(&out) && strcmp(out.title, "newer") == 0);
	char status[32];
	assert(btstack_media_status(status, sizeof(status)) && strcmp(status, "playing") == 0);
	// Even a repeated signal prevents an older in-flight Get from winning.
	track_at = media_track_revision;
	media_signal_track(player, &newer);
	apply_media_reply(address, &stale, true, track_at, "", false, media_status_revision);
	assert(btstack_media_track(&out) && strcmp(out.title, "newer") == 0);
	// Invalidated Track is represented by an empty value.
	btstack_track_t empty = {0};
	media_signal_track(player, &empty);
	assert(!btstack_media_track(&out));
	pthread_mutex_lock(&lock);
	pcm_remove(rx_addr, &rx_count, address);
	media_sync_owner_locked();
	pthread_mutex_unlock(&lock);
	assert(!btstack_media_status(status, sizeof(status)));
	media_signal_track(player, &stale);
	assert(!btstack_media_track(&out));
	pthread_mutex_lock(&lock);
	pcm_add(rx_addr, &rx_count, "AA:BB:CC:DD:EE:FF");
	media_sync_owner_locked();
	pthread_mutex_unlock(&lock);
	assert(media_path_is_current(other) && !media_path_is_current(player));
	media_signal_track(player, &first);
	assert(!btstack_media_track(&out));
	media_signal_track(other, &newer);
	assert(btstack_media_track(&out) && strcmp(out.title, "newer") == 0);
	pthread_mutex_lock(&lock);
	pcm_remove(rx_addr, &rx_count, "AA:BB:CC:DD:EE:FF");
	media_sync_owner_locked();
	pthread_mutex_unlock(&lock);
}

int main(void) {
	test_media_ownership_and_races();
	populate();
	assert(btstack_audio_sink(NULL, 0) && btstack_audio_source(NULL, 0));
	unsigned before;
	assert(btstack_transport_sending(address, &before) == 1);

	/* An explicit close, even with no connection, clears both directions and
	 * media/transport state and advances the transport serial. */
	btstack_close();
	assert(!btstack_audio_sink(NULL, 0) && !btstack_audio_source(NULL, 0));
	unsigned after;
	assert(btstack_transport_sending(address, &after) == -1 && after != before);
	char status[32];
	btstack_track_t track;
	assert(!btstack_media_status(status, sizeof(status)));
	assert(!btstack_media_track(&track));

	/* A racing final signal also disappears once the reader has stopped. */
	populate();
	conn = (dbus_conn_t *)(void *)&fake_connection;
	deliver_late_signal = true;
	btstack_close();
	assert(!btstack_audio_sink(NULL, 0) && !btstack_audio_source(NULL, 0));
	assert(btstack_transport_sending(address, &after) == -1);
	assert(!btstack_media_status(status, sizeof(status)));
	assert(!btstack_media_track(&track));

	printf("PASS btstack (RX metadata ownership, stale replies, close and late signals)\n");
	return 0;
}
