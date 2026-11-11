/* The receive PCM's volume mode over D-Bus, with the replies supplied
 * in-process, and the counter that says a new receive PCM has appeared.
 *
 * What a phone's volume does on this side is decided by one flag on the
 * a2dpsnk/source PCM (bluealsa's SoftVolume), and a codec change builds that PCM
 * again from the daemon's defaults. So the worker has to be able to read the
 * flag, write it, and know that the object in front of it is a new one. */
#define dbus_call_send real_dbus_call_send
#include "../src/system/bluetooth/dbuslite.c"
#undef dbus_call_send
bool dbus_call_send(dbus_conn_t *c, int timeout_ms, char *err, size_t size);
#include "../src/system/bluetooth/btstack.c"
#include <assert.h>

static dbus_conn_t fake_bus;
static const char *address = "20:04:84:3A:6C:FE";
static const char *rx_path = "/org/bluealsa/hci0/dev_20_04_84_3A_6C_FE/a2dpsnk/source";
static const char *tx_path = "/org/bluealsa/hci0/dev_20_04_84_3A_6C_FE/a2dpsrc/sink";

static bool pcm_exists = true; // false: the object is gone while a transport is rebuilt
static bool pcm_soft;		   // the flag the fake PCM holds
static int gets, sets;
static bool bad_reply_type;	   // answers with a string where a boolean is expected
static char last_path[DBUS_NAME_MAX];

bool dbus_call_send(dbus_conn_t *c, int timeout_ms, char *err, size_t size) {
	assert(c == &fake_bus && timeout_ms == CALL_MS);
	assert(!c->writer.overflow);
	memcpy(last_path, c->out_path, DBUS_NAME_MAX);
	assert(!strcmp(c->out_destination, BALSA_SERVICE));
	assert(!strcmp(c->out_interface, PROPS_IFACE));

	dbus_msg_t m = {.body = c->writer.buf, .body_len = c->writer.used};
	dbus_reader_t r;
	dbus_reader_init(&r, &m);
	char iface[64], property[64];
	assert(dbus_r_string(&r, iface, sizeof(iface)) && !strcmp(iface, BALSA_PCM_IFACE));
	assert(dbus_r_string(&r, property, sizeof(property)) && !strcmp(property, "SoftVolume"));

	bool ok = pcm_exists;
	if (!strcmp(c->out_member, "Get")) {
		gets++;
		assert(!strcmp(c->out_signature, "ss"));
		if (ok) {
			dbus_writer_t w = {0};
			if (bad_reply_type) {
				dbus_w_variant_string(&w, "not a boolean");
			} else {
				dbus_w_variant_bool(&w, pcm_soft);
			}
			free(c->reply_body);
			c->reply_body = malloc(w.used);
			assert(c->reply_body);
			memcpy(c->reply_body, w.buf, w.used);
			c->reply_body_size = c->reply_body_len = w.used;
			c->reply_big_endian = false;
		}
	} else {
		assert(!strcmp(c->out_member, "Set") && !strcmp(c->out_signature, "ssv"));
		sets++;
		char sig[8];
		bool value = false;
		assert(dbus_r_signature(&r, sig, sizeof(sig)) && !strcmp(sig, "b") && dbus_r_bool(&r, &value) && !r.bad);
		if (ok) pcm_soft = value;
	}
	if (err && size) snprintf(err, size, "%s", ok ? "" : "org.freedesktop.DBus.Error.UnknownMethod");
	pthread_mutex_unlock(&c->write_lock);
	return ok;
}

/* InterfacesAdded / InterfacesRemoved for one object, with no interfaces
 * listed: the path is all btstack reads to tell the PCMs apart. */
static void announce(const char *member, const char *path) {
	dbus_writer_t w = {0};
	dbus_w_path(&w, path);
	dbus_array_t list;
	if (!strcmp(member, "InterfacesAdded")) {
		dbus_w_array_begin(&w, "{sa{sv}}", &list);
	} else {
		dbus_w_array_begin(&w, "s", &list);
	}
	dbus_w_array_end(&w, &list);
	assert(!w.overflow);
	dbus_msg_t m = {0};
	snprintf(m.interface, sizeof(m.interface), "%s", OBJMAN_IFACE);
	snprintf(m.member, sizeof(m.member), "%s", member);
	m.body = w.buf;
	m.body_len = w.used;
	on_signal(NULL, &m, NULL);
}

static void test_softvolume_get_and_set(void) {
	bool soft = true;

	/* Off, and read as off -- on the receive PCM, not the headphones'. */
	pcm_soft = false;
	assert(btstack_pcm_softvolume_get(address, true, &soft) && !soft);
	assert(!strcmp(last_path, rx_path));
	assert(btstack_pcm_softvolume_get(address, false, &soft) && !soft);
	assert(!strcmp(last_path, tx_path));

	/* Written on, and read back as on. */
	assert(btstack_pcm_softvolume_set(address, true, true));
	assert(!strcmp(last_path, rx_path) && pcm_soft);
	assert(btstack_pcm_softvolume_get(address, true, &soft) && soft);

	/* The object being gone, as it is for a moment while a transport is
	 * rebuilt: a miss, and the caller's value left alone. */
	pcm_exists = false;
	soft = true;
	assert(!btstack_pcm_softvolume_get(address, true, &soft) && soft);
	pcm_soft = false;
	soft = false;
	assert(!btstack_pcm_softvolume_set(address, true, true) && !pcm_soft);
	pcm_exists = true;

	/* A reply that is not a boolean is not an answer. */
	bad_reply_type = true;
	soft = true;
	assert(!btstack_pcm_softvolume_get(address, true, &soft) && soft);
	bad_reply_type = false;

	/* No bus, no address, nowhere to put the answer. */
	assert(!btstack_pcm_softvolume_get(address, true, NULL));
	assert(!btstack_pcm_softvolume_get("not an address", true, &soft));
	dbus_conn_t *held = conn;
	conn = NULL;
	assert(!btstack_pcm_softvolume_get(address, true, &soft));
	conn = held;
	assert(gets >= 5 && sets >= 2);
}

/* A codec change removes the receive PCM and builds it again; both the first
 * one of a link and each rebuilt one count, the headphones' PCM does not. */
static void test_receive_pcm_generation(void) {
	unsigned g = btstack_rx_pcm_generation();

	announce("InterfacesAdded", tx_path);
	assert(btstack_rx_pcm_generation() == g);

	announce("InterfacesAdded", rx_path);
	assert(btstack_rx_pcm_generation() == g + 1);
	assert(btstack_audio_source(NULL, 0));

	announce("InterfacesRemoved", rx_path);
	assert(btstack_rx_pcm_generation() == g + 1);
	assert(!btstack_audio_source(NULL, 0));

	announce("InterfacesAdded", rx_path);
	assert(btstack_rx_pcm_generation() == g + 2);

	/* Something else on the bus is not a PCM at all. */
	announce("InterfacesAdded", "/org/bluealsa/hci0/dev_20_04_84_3A_6C_FE");
	assert(btstack_rx_pcm_generation() == g + 2);
}

int main(void) {
	pthread_mutex_init(&fake_bus.write_lock, NULL);
	pthread_mutex_init(&fake_bus.reply_lock, NULL);
	conn = &fake_bus;
	test_softvolume_get_and_set();
	test_receive_pcm_generation();
	conn = NULL;
	free(fake_bus.reply_body);
	puts("PASS Bluetooth RX volume mode (SoftVolume read and write on the receive PCM, new-PCM counter)");
	return 0;
}
