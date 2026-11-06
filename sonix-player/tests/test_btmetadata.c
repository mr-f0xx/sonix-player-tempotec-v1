/* Real D-Bus marshalling and receiver signal handling, without a system bus. */
#define _GNU_SOURCE 1
#include "../src/system/bluetooth/btstack.c"
#include "../src/system/bluetooth/dbuslite.c"
#include <assert.h>

static const char *sender = "/org/bluez/hci0/dev_20_04_84_3A_6C_FE/player0";
static const char *other = "/org/bluez/hci0/dev_AA_BB_CC_DD_EE_FF/player0";

static void send_track(const char *path, const char *title, bool invalidate) {
	dbus_writer_t w = {0};
	dbus_w_string(&w, BLUEZ_MEDIA_PLAYER_IFACE);
	dbus_array_t props, track, invalidated;
	dbus_w_array_begin(&w, "{sv}", &props);
	if (title) {
		dbus_w_dict_variant_begin(&w, "Track", "a{sv}");
		dbus_w_array_begin(&w, "{sv}", &track);
		dbus_w_dict_string(&w, "Title", title);
		dbus_w_dict_string(&w, "Artist", "Artist");
		dbus_w_array_end(&w, &track);
	}
	dbus_w_array_end(&w, &props);
	dbus_w_array_begin(&w, "s", &invalidated);
	if (invalidate) dbus_w_string(&w, "Track");
	dbus_w_array_end(&w, &invalidated);
	assert(!w.overflow);
	dbus_msg_t m = {0};
	snprintf(m.path, sizeof(m.path), "%s", path);
	snprintf(m.interface, sizeof(m.interface), "%s", PROPS_IFACE);
	snprintf(m.member, sizeof(m.member), "PropertiesChanged");
	m.body = w.buf; m.body_len = w.used;
	on_signal(NULL, &m, NULL);
}

static void remove_player(const char *path, const char *iface) {
	dbus_writer_t w = {0};
	dbus_w_path(&w, path);
	dbus_array_t removed;
	dbus_w_array_begin(&w, "s", &removed);
	dbus_w_string(&w, iface);
	dbus_w_array_end(&w, &removed);
	dbus_msg_t m = {0};
	snprintf(m.interface, sizeof(m.interface), "%s", OBJMAN_IFACE);
	snprintf(m.member, sizeof(m.member), "InterfacesRemoved");
	m.body = w.buf; m.body_len = w.used;
	on_signal(NULL, &m, NULL);
}

int main(void) {
	pthread_mutex_lock(&lock);
	pcm_add(rx_addr, &rx_count, "20:04:84:3A:6C:FE");
	media_sync_owner_locked();
	pthread_mutex_unlock(&lock);
	btstack_track_t t;
	send_track(sender, "First song", false);
	assert(btstack_media_track(&t) && strcmp(t.title, "First song") == 0);
	send_track(other, "Other device", false);
	assert(btstack_media_track(&t) && strcmp(t.title, "First song") == 0);
	send_track(sender, "Second song", false);
	assert(btstack_media_track(&t) && strcmp(t.title, "Second song") == 0);
	remove_player(sender, "org.bluez.MediaFolder1");
	assert(btstack_media_track(&t));
	remove_player(other, BLUEZ_MEDIA_PLAYER_IFACE);
	assert(btstack_media_track(&t));
	remove_player(sender, BLUEZ_MEDIA_PLAYER_IFACE);
	assert(!btstack_media_track(&t));
	send_track(sender, "Third song", false);
	assert(btstack_media_track(&t));
	send_track(sender, NULL, true);
	assert(!btstack_media_track(&t));
	puts("PASS Bluetooth RX metadata signals and invalidation");
	return 0;
}
