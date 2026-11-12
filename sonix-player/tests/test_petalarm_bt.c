/* Real Bluetooth volume worker + real alarm volume, with a controllable D-Bus
 * and mixer. Signals are delivered on this test thread like a D-Bus reader. */
#define _DEFAULT_SOURCE
#include <assert.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

// Keep dbuslite's actual wire encoding/decoding, replacing only the connection.
#define dbus_connect_system real_dbus_connect_system
#define dbus_set_signal_handler real_dbus_set_signal_handler
#define dbus_add_match real_dbus_add_match
#define dbus_disconnect real_dbus_disconnect
#define dbus_alive real_dbus_alive
#define dbus_call_begin real_dbus_call_begin
#define dbus_call_send real_dbus_call_send
#define dbus_reply_reader real_dbus_reply_reader
#include "src/system/bluetooth/dbuslite.c"
#undef dbus_connect_system
#undef dbus_set_signal_handler
#undef dbus_add_match
#undef dbus_disconnect
#undef dbus_alive
#undef dbus_call_begin
#undef dbus_call_send
#undef dbus_reply_reader

dbus_conn_t *dbus_connect_system(const char *, dbus_method_cb, void *);
void dbus_set_signal_handler(dbus_conn_t *, dbus_signal_cb, void *);
bool dbus_add_match(dbus_conn_t *, const char *);
void dbus_disconnect(dbus_conn_t *);
bool dbus_alive(const dbus_conn_t *);
dbus_writer_t *dbus_call_begin(dbus_conn_t *, const char *, const char *, const char *, const char *, const char *);
bool dbus_call_send(dbus_conn_t *, int, char *, size_t);
void dbus_reply_reader(dbus_conn_t *, dbus_reader_t *);
#include "src/system/bluetooth/btvolume.c"

#include "src/system/audio/audio.h"
#include "src/system/core/config.h"
#include "src/system/device/sysinfo.h"

struct snd_ctl { int unused; };
typedef struct snd_ctl snd_ctl_t;
int snd_ctl_open(snd_ctl_t **out, const char *name, int mode) {
	(void)name; (void)mode;
	static struct snd_ctl ctl;
	*out = &ctl;
	return 0;
}
int snd_ctl_close(snd_ctl_t *ctl) { (void)ctl; return 0; }
void snd_ctl_elem_id_set_interface(void *id, int iface) { (void)id; (void)iface; }
void snd_ctl_elem_id_set_name(void *id, const char *name) { (void)id; (void)name; }
void snd_ctl_elem_value_set_id(void *value, void *id) { (void)value; (void)id; }
void snd_ctl_elem_value_set_integer(void *value, unsigned int channel, long level) {
	(void)value; (void)channel; (void)level;
}
int snd_ctl_elem_write(snd_ctl_t *ctl, void *value) { (void)ctl; (void)value; return 0; }

bool addons_enabled(const char *id) { (void)id; return true; }
bool file_matches(const char *path, const char *wanted) { (void)path; (void)wanted; return false; }
const sysinfo_model_t *sysinfo_model(void) {
	static const sysinfo_model_t v1 = {.cs43131 = true, .headset_switch_idle_one = true};
	return &v1;
}
audio_status_t audio_get_status(void) { return AUDIO_STATUS_PLAYING; }
void usbaudio_apply_volume(int percent) { (void)percent; }
void swvolume_set_index(int percent) { (void)percent; }
// The first test deliberately holds the Bluetooth worker's ordinary local
// notification back, exercising adoption that arrives before that job.
static bool forward_local;
void bluetooth_notify_volume(int percent) { if (forward_local) btvolume_notify_local(percent); }
void thread_be_background(const char *name) { (void)name; }
void deadline_in_ms(struct timespec *out, unsigned int ms) {
	clock_gettime(CLOCK_REALTIME, out);
	out->tv_sec += ms / 1000;
	out->tv_nsec += (long)(ms % 1000) * 1000000L;
	if (out->tv_nsec >= 1000000000L) { out->tv_sec++; out->tv_nsec -= 1000000000L; }
}

static pthread_mutex_t fake_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t fake_wake = PTHREAD_COND_INITIALIZER;
static int get_count, writes_count, written[16], notices, mode_entered;
static bool hold_mode;
static dbus_conn_t fake_bus;
static dbus_writer_t fake_reply;

void gui_notify_volume(int percent) {
	(void)percent;
	pthread_mutex_lock(&fake_lock);
	notices++;
	pthread_cond_broadcast(&fake_wake);
	pthread_mutex_unlock(&fake_lock);
}
dbus_conn_t *dbus_connect_system(const char *addr, dbus_method_cb cb, void *user) {
	(void)addr; (void)cb; (void)user;
	return &fake_bus;
}
void dbus_set_signal_handler(dbus_conn_t *conn, dbus_signal_cb cb, void *user) {
	(void)conn; (void)cb; (void)user;
}
bool dbus_add_match(dbus_conn_t *conn, const char *rule) { (void)conn; (void)rule; return true; }
void dbus_disconnect(dbus_conn_t *conn) { (void)conn; }
bool dbus_alive(const dbus_conn_t *conn) { return conn == &fake_bus; }
dbus_writer_t *dbus_call_begin(dbus_conn_t *conn, const char *dest, const char *path,
                               const char *iface, const char *member, const char *sig) {
	assert(conn == &fake_bus);
	(void)dest; (void)path; (void)iface; (void)sig;
	memset(&conn->writer, 0, sizeof(conn->writer));
	snprintf(conn->out_member, sizeof(conn->out_member), "%s", member);
	return &conn->writer;
}
bool dbus_call_send(dbus_conn_t *conn, int timeout, char *err, size_t err_size) {
	assert(conn == &fake_bus && timeout == CALL_TIMEOUT_MS && !conn->writer.overflow);
	if (err && err_size) err[0] = '\0';
	dbus_msg_t msg = {.body = conn->writer.buf, .body_len = conn->writer.used};
	dbus_reader_t r;
	dbus_reader_init(&r, &msg);
	char iface[80], property[80], sig[8];
	assert(dbus_r_string(&r, iface, sizeof(iface)) && !strcmp(iface, PCM_INTERFACE));
	assert(dbus_r_string(&r, property, sizeof(property)));
	if (!strcmp(conn->out_member, "Get")) {
		assert(!strcmp(property, "Volume"));
		memset(&fake_reply, 0, sizeof(fake_reply));
		dbus_w_variant_u16(&fake_reply, 0x7f7f);
		pthread_mutex_lock(&fake_lock);
		get_count++;
		pthread_cond_broadcast(&fake_wake);
		pthread_mutex_unlock(&fake_lock);
	} else {
		assert(!strcmp(conn->out_member, "Set") && dbus_r_signature(&r, sig, sizeof(sig)));
		if (!strcmp(property, "Volume")) {
			uint16_t both;
			assert(!strcmp(sig, "q") && dbus_r_u16(&r, &both));
			pthread_mutex_lock(&fake_lock);
			assert(writes_count < (int)(sizeof(written) / sizeof(written[0])));
			written[writes_count++] = (both >> 8) & 0x7f;
			pthread_cond_broadcast(&fake_wake);
			pthread_mutex_unlock(&fake_lock);
		} else {
			bool value;
			assert(!strcmp(property, "SoftVolume") && !strcmp(sig, "b") && dbus_r_bool(&r, &value));
			pthread_mutex_lock(&fake_lock);
			if (hold_mode) {
				mode_entered++;
				pthread_cond_broadcast(&fake_wake);
				while (hold_mode) pthread_cond_wait(&fake_wake, &fake_lock);
			}
			pthread_mutex_unlock(&fake_lock);
		}
	}
	return true;
}
void dbus_reply_reader(dbus_conn_t *conn, dbus_reader_t *out) {
	assert(conn == &fake_bus);
	dbus_msg_t msg = {.body = fake_reply.buf, .body_len = fake_reply.used};
	dbus_reader_init(out, &msg);
}

static void wait_for_count(int *value, int n) {
	struct timespec deadline;
	deadline_in_ms(&deadline, 5000);
	pthread_mutex_lock(&fake_lock);
	while (*value < n) {
		int rc = pthread_cond_timedwait(&fake_wake, &fake_lock, &deadline);
		assert(rc == 0);
	}
	pthread_mutex_unlock(&fake_lock);
}
static void emit_volume(int level) {
	dbus_writer_t w = {0};
	dbus_w_string(&w, PCM_INTERFACE);
	dbus_array_t array;
	dbus_w_array_begin(&w, "{sv}", &array);
	dbus_w_dict_variant_begin(&w, "Volume", "q");
	dbus_w_u16(&w, (uint16_t)((level << 8) | level));
	dbus_w_array_end(&w, &array);
	assert(!w.overflow);
	dbus_msg_t msg = {.body = w.buf, .body_len = w.used};
	snprintf(msg.path, sizeof(msg.path), "%s", want_path);
	handle_properties_changed(&msg);
}

int main(void) {
	volume_profile_init(80, 80);
	set_volume_percent(80);
	volume_profile_set_output(VOLUME_OUTPUT_BLUETOOTH);
	const char *mac = "20:04:84:3A:6C:FE";
	btvolume_set_device(mac);
	btvolume_init();
	wait_for_count(&get_count, 1); // worker is adopting the initial transport

	assert(volume_fade_begin());
	emit_volume(120); // initial adoption arrives before the delayed local job
	wait_for_count(&writes_count, 1);
	assert(written[0] == 0 && volume_fade_active() && get_volume_percent() == 0);
	assert(volume_profile_level(true) == 80);

	volume_fade_progress(5, 10);
	assert(get_volume_percent() == 40 && volume_fade_active());
	btvolume_notify_local(40);
	wait_for_count(&writes_count, 2);
	assert(written[1] == local_to_bt(40));

	// The old zero arrives after the newer 40% write; it must not cancel the
	// fade or save zero. A fallback connection's echo is treated the same.
	emit_volume(0);
	btvolume_note_fallback_write(mac, 15);
	emit_volume(local_to_bt(15));
	pthread_mutex_lock(&lock);
	assert(pending_remote < 0 && last_bt == local_to_bt(40));
	pthread_mutex_unlock(&lock);
	assert(volume_fade_active() && get_volume_percent() == 40 && volume_profile_level(true) == 80);

	// A slow fallback write can physically land after the bridge's newer 40%.
	// Its completion must cause the latest level to be sent again, even though
	// the bridge already believes it wrote that value.
	btvolume_finish_fallback_write("12:34:56:78:9A:BC"); // wrong device: no resync
	pthread_mutex_lock(&lock);
	assert(pending_local < 0 && last_bt == local_to_bt(40));
	pthread_mutex_unlock(&lock);
	btvolume_finish_fallback_write(mac);
	wait_for_count(&writes_count, 3);
	assert(written[2] == local_to_bt(40));
	emit_volume(local_to_bt(40)); // first of the two 40% writes
	volume_fade_progress(6, 10);
	btvolume_notify_local(48);
	wait_for_count(&writes_count, 4);
	assert(written[3] == local_to_bt(48));
	emit_volume(local_to_bt(40)); // second delayed 40% echo, after the new step
	pthread_mutex_lock(&lock);
	assert(pending_remote < 0 && last_bt == local_to_bt(48));
	pthread_mutex_unlock(&lock);
	assert(volume_fade_active() && get_volume_percent() == 48);
	emit_volume(local_to_bt(48));

	// A genuinely different remote level remains a user adjustment.
	emit_volume(95);
	wait_for_count(&notices, 1);
	assert(!volume_fade_active() && get_volume_percent() == bt_to_local(95));
	assert(volume_profile_level(true) == bt_to_local(95));
	btvolume_stop();

	// The zero can be queued before the worker has opened the bus. It must
	// survive that connection attempt rather than being lost and leaving the
	// headphones at their old level during the adoption settle window.
	btvolume_set_device(NULL);
	pthread_mutex_lock(&lock);
	have_path[0] = adopted_path[0] = '\0';
	latest_local = pending_local = pending_remote = last_bt = -1;
	path_live = bus_broken = bus_reset = false;
	echo_clear_locked();
	pthread_mutex_unlock(&lock);
	pthread_mutex_lock(&fake_lock);
	get_count = writes_count = notices = mode_entered = 0;
	hold_mode = true;
	pthread_mutex_unlock(&fake_lock);
	set_volume_percent(80);
	btvolume_set_device(mac);
	assert(volume_fade_begin());
	btvolume_notify_local(0); // present before open_bus() on the first iteration
	btvolume_init();
	wait_for_count(&mode_entered, 1);
	// A bus connection is not yet a usable bridge while setup is in flight.
	assert(!btvolume_available());
	pthread_mutex_lock(&fake_lock);
	hold_mode = false;
	pthread_cond_broadcast(&fake_wake);
	pthread_mutex_unlock(&fake_lock);
	wait_for_count(&writes_count, 1);
	pthread_mutex_lock(&fake_lock);
	assert(written[0] == 0 && get_count == 0);
	pthread_mutex_unlock(&fake_lock);
	assert(volume_fade_active() && volume_profile_level(true) == 80);
	btvolume_stop();
	volume_fade_stop();
	puts("pet alarm Bluetooth worker and echoes: all checks passed");
	return 0;
}
