/* Real BlueZ discovery calls and D-Bus body, with replies supplied in-process. */
#define dbus_call_send real_dbus_call_send
#include "../src/system/bluetooth/dbuslite.c"
#undef dbus_call_send
bool dbus_call_send(dbus_conn_t *c, int timeout_ms, char *err, size_t size);
#include "../src/system/bluetooth/btstack.c"
#include <assert.h>

static dbus_conn_t fake_bus;
static char members[4][DBUS_NAME_MAX];
static int calls;
static const char *filter_error;
static const char *start_error;

bool dbus_call_send(dbus_conn_t *c, int timeout_ms, char *err, size_t size) {
	assert(c == &fake_bus && timeout_ms == CALL_MS);
	assert(calls < 4);
	memcpy(members[calls++], c->out_member, DBUS_NAME_MAX);
	const char *failure = NULL;
	if (!strcmp(c->out_member, "SetDiscoveryFilter")) {
		assert(!strcmp(c->out_signature, "a{sv}"));
		assert(!strcmp(c->out_destination, BLUEZ_SERVICE));
		assert(!strcmp(c->out_path, BLUEZ_ADAPTER_PATH));
		assert(!strcmp(c->out_interface, BLUEZ_ADAPTER_IFACE));
		assert(!c->writer.overflow);
		dbus_msg_t m = {.body = c->writer.buf, .body_len = c->writer.used};
		dbus_reader_t r;
		dbus_reader_init(&r, &m);
		dbus_array_iter_t entries;
		assert(dbus_r_array_begin(&r, "{sv}", &entries));
		assert(dbus_r_array_more(&r, &entries));
		char key[32], sig[8], value[32];
		assert(dbus_r_string(&r, key, sizeof(key)) && !strcmp(key, "Transport"));
		assert(dbus_r_signature(&r, sig, sizeof(sig)) && !strcmp(sig, "s"));
		assert(dbus_r_string(&r, value, sizeof(value)) && !strcmp(value, "bredr"));
		assert(!dbus_r_array_more(&r, &entries) && !r.bad);
		failure = filter_error;
	} else if (!strcmp(c->out_member, "StartDiscovery")) {
		assert(!c->out_signature[0]);
		failure = start_error;
	} else {
		assert(!strcmp(c->out_member, "StopDiscovery") && !c->out_signature[0]);
	}
	if (err && size) snprintf(err, size, "%s", failure ? failure : "");
	pthread_mutex_unlock(&c->write_lock);
	return !failure;
}

static void reset(void) {
	calls = 0;
	filter_error = start_error = NULL;
}

int main(void) {
	pthread_mutex_init(&fake_bus.write_lock, NULL);
	conn = &fake_bus;
	assert(btstack_discovery(true));
	assert(calls == 2 && !strcmp(members[0], "SetDiscoveryFilter") && !strcmp(members[1], "StartDiscovery"));
	reset();
	filter_error = "org.freedesktop.DBus.Error.UnknownMethod";
	assert(btstack_discovery(true));
	assert(calls == 2 && !strcmp(members[1], "StartDiscovery"));
	reset();
	filter_error = "org.bluez.Error.NotSupported";
	assert(btstack_discovery(true));
	assert(calls == 2);
	reset();
	filter_error = "org.bluez.Error.NotReady";
	assert(!btstack_discovery(true) && calls == 1);
	reset();
	filter_error = "org.bluez.Error.InvalidArguments";
	assert(!btstack_discovery(true) && calls == 1);
	reset();
	start_error = "org.bluez.Error.Failed";
	assert(!btstack_discovery(true) && calls == 2);
	reset();
	start_error = "org.bluez.Error.InProgress";
	assert(btstack_discovery(true) && calls == 2);
	reset();
	assert(btstack_discovery(false) && calls == 1 && !strcmp(members[0], "StopDiscovery"));
	conn = NULL;
	assert(!btstack_discovery(true) && calls == 1);
	pthread_mutex_destroy(&fake_bus.write_lock);
	puts("PASS Bluetooth discovery (BR/EDR filter marshalling, fallback and real start failures)");
	return 0;
}
