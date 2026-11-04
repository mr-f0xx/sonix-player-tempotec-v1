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

int main(void) {
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

	printf("PASS btstack (close clears both audio directions and late signals)\n");
	return 0;
}
