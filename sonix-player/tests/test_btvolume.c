/* Exercise btvolume's BlueALSA D-Bus volume bridge in-process: initial AVRCP
 * adoption, write coalescing/rate-limiting, delayed and step-quantized echo
 * suppression during rapid volume-up steps, and genuine remote adjustments. */
#define _GNU_SOURCE 1

#include <arpa/inet.h>
#include <assert.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* Use the real D-Bus marshaller/unmarshaller, intercepting only bus I/O. */
#define dbus_connect_system real_dbus_connect_system
#define dbus_disconnect real_dbus_disconnect
#define dbus_alive real_dbus_alive
#define dbus_set_signal_handler real_dbus_set_signal_handler
#define dbus_add_match real_dbus_add_match
#define dbus_call_send real_dbus_call_send
#include "../src/system/bluetooth/dbuslite.c"
#undef dbus_connect_system
#undef dbus_disconnect
#undef dbus_alive
#undef dbus_set_signal_handler
#undef dbus_add_match
#undef dbus_call_send

/* ---------------------------------------------------------------------------
 * Controlled monotonic clock for deterministic settle & rate-limit tests
 * --------------------------------------------------------------------------- */

static pthread_mutex_t test_clock_lock = PTHREAD_MUTEX_INITIALIZER;
static uint32_t test_mono_ms = 10000;

static uint32_t test_now_ms(void) {
	pthread_mutex_lock(&test_clock_lock);
	uint32_t v = test_mono_ms;
	pthread_mutex_unlock(&test_clock_lock);
	return v;
}

static void advance_mono_ms(uint32_t delta) {
	pthread_mutex_lock(&test_clock_lock);
	test_mono_ms += delta;
	pthread_mutex_unlock(&test_clock_lock);
}

static int test_clock_gettime(clockid_t clk_id, struct timespec *tp) {
	if (clk_id == CLOCK_MONOTONIC) {
		uint32_t ms = test_now_ms();
		tp->tv_sec = (time_t)(ms / 1000);
		tp->tv_nsec = (long)(ms % 1000) * 1000000L;
		return 0;
	}
	return clock_gettime(clk_id, tp);
}

/* ---------------------------------------------------------------------------
 * Stubbed player state (alsa-controls, gui, utils)
 * --------------------------------------------------------------------------- */

static pthread_mutex_t player_lock = PTHREAD_MUTEX_INITIALIZER;
static int player_volume = 30;
static bool player_bt_profile = true;
static int set_volume_calls = 0;
static int gui_notify_calls = 0;
static int last_gui_volume = -1;

int get_volume_percent(void) {
	pthread_mutex_lock(&player_lock);
	int v = player_volume;
	pthread_mutex_unlock(&player_lock);
	return v;
}

void set_volume_percent(int percent) {
	if (percent < 0) percent = 0;
	if (percent > 100) percent = 100;
	pthread_mutex_lock(&player_lock);
	player_volume = percent;
	set_volume_calls++;
	pthread_mutex_unlock(&player_lock);
}

bool volume_profile_is_bluetooth(void) {
	pthread_mutex_lock(&player_lock);
	bool bt = player_bt_profile;
	pthread_mutex_unlock(&player_lock);
	return bt;
}

void gui_notify_volume(int percent) {
	pthread_mutex_lock(&player_lock);
	gui_notify_calls++;
	last_gui_volume = percent;
	pthread_mutex_unlock(&player_lock);
}

void thread_be_background(const char *name) { (void)name; }

void deadline_in_ms(struct timespec *out, unsigned int ms) {
	struct timespec now;
	clock_gettime(CLOCK_REALTIME, &now);
	uint64_t nsec = (uint64_t)now.tv_nsec + (uint64_t)ms * 1000000ULL;
	out->tv_sec = now.tv_sec + (time_t)(nsec / 1000000000ULL);
	out->tv_nsec = (long)(nsec % 1000000000ULL);
}

/* ---------------------------------------------------------------------------
 * In-process BlueALSA D-Bus mock
 * --------------------------------------------------------------------------- */

static pthread_mutex_t bus_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t bus_cond = PTHREAD_COND_INITIALIZER;

static dbus_conn_t *active_conn = NULL;
static dbus_signal_cb bus_signal_cb = NULL;
static void *bus_signal_user = NULL;
static bool bus_connected = false;
static bool mock_soft_volume = true;
static int mock_stream_volume = 127;
static int write_history[128];
static int write_count = 0;
static int soft_write_count = 0;

/* Optional quantized echo injected automatically on Set("Volume"). */
static bool auto_quantize_on_set = false;
static int auto_quantize_delta = 0;

static void reset_mock(int initial_player_vol, bool bt_profile, int initial_stream_vol) {
	pthread_mutex_lock(&player_lock);
	player_volume = initial_player_vol;
	player_bt_profile = bt_profile;
	set_volume_calls = 0;
	gui_notify_calls = 0;
	last_gui_volume = -1;
	pthread_mutex_unlock(&player_lock);

	pthread_mutex_lock(&bus_lock);
	active_conn = NULL;
	bus_signal_cb = NULL;
	bus_signal_user = NULL;
	bus_connected = false;
	mock_soft_volume = true;
	mock_stream_volume = initial_stream_vol;
	write_count = 0;
	soft_write_count = 0;
	auto_quantize_on_set = false;
	auto_quantize_delta = 0;
	pthread_mutex_unlock(&bus_lock);
}

static dbus_conn_t *dbus_connect_system(const char *address, dbus_method_cb on_call, void *user) {
	(void)address;
	(void)on_call;
	(void)user;
	dbus_conn_t *c = calloc(1, sizeof(*c));
	assert(c != NULL);
	pthread_mutex_init(&c->write_lock, NULL);
	pthread_mutex_init(&c->reply_lock, NULL);
	pthread_cond_init(&c->reply_cond, NULL);
	c->next_serial = 1;
	c->alive = true;

	pthread_mutex_lock(&bus_lock);
	active_conn = c;
	bus_connected = true;
	pthread_mutex_unlock(&bus_lock);
	return c;
}

static void dbus_disconnect(dbus_conn_t *c) {
	if (!c) return;
	pthread_mutex_lock(&bus_lock);
	if (active_conn == c) {
		active_conn = NULL;
	}
	bus_connected = false;
	pthread_mutex_unlock(&bus_lock);

	pthread_mutex_destroy(&c->write_lock);
	pthread_mutex_destroy(&c->reply_lock);
	pthread_cond_destroy(&c->reply_cond);
	free(c->read_buf);
	free(c->reply_body);
	free(c);
}

static bool dbus_alive(const dbus_conn_t *c) {
	pthread_mutex_lock(&bus_lock);
	bool ok = c != NULL && bus_connected;
	pthread_mutex_unlock(&bus_lock);
	return ok;
}

static void dbus_set_signal_handler(dbus_conn_t *c, dbus_signal_cb cb, void *user) {
	(void)c;
	pthread_mutex_lock(&bus_lock);
	bus_signal_cb = cb;
	bus_signal_user = user;
	pthread_mutex_unlock(&bus_lock);
}

static bool dbus_add_match(dbus_conn_t *c, const char *rule) {
	(void)c;
	(void)rule;
	return true;
}

static void emit_volume_signal(const char *path, int left) {
	dbus_writer_t w;
	memset(&w, 0, sizeof(w));
	dbus_w_string(&w, "org.bluealsa.PCM1");
	dbus_array_t dict;
	dbus_w_array_begin(&w, "{sv}", &dict);
	dbus_w_dict_variant_begin(&w, "Volume", "q");
	uint16_t packed = (uint16_t)(((left & 0x7f) << 8) | (left & 0x7f));
	dbus_w_u16(&w, packed);
	dbus_w_array_end(&w, &dict);
	dbus_array_t inv;
	dbus_w_array_begin(&w, "s", &inv);
	dbus_w_array_end(&w, &inv);
	assert(!w.overflow);

	dbus_msg_t m;
	memset(&m, 0, sizeof(m));
	m.type = DBUS_TYPE_SIGNAL;
	set_str(m.path, sizeof(m.path), path);
	set_str(m.interface, sizeof(m.interface), "org.freedesktop.DBus.Properties");
	set_str(m.member, sizeof(m.member), "PropertiesChanged");
	set_str(m.signature, sizeof(m.signature), "sa{sv}as");
	m.body = w.buf;
	m.body_len = w.used;
	m.big_endian = host_is_big_endian();

	pthread_mutex_lock(&bus_lock);
	dbus_conn_t *c = active_conn;
	dbus_signal_cb cb = bus_signal_cb;
	void *user = bus_signal_user;
	pthread_mutex_unlock(&bus_lock);

	if (cb) {
		cb(c, &m, user);
	}
}

static bool dbus_call_send(dbus_conn_t *c, int timeout_ms, char *err_out, size_t err_size) {
	(void)timeout_ms;
	if (err_out && err_size) {
		err_out[0] = '\0';
	}

	bool ok = true;
	if (strcmp(c->out_member, "Get") == 0) {
		dbus_reader_t r = {.data = c->writer.buf, .len = c->writer.used, .big_endian = host_is_big_endian()};
		char iface[128], prop[64];
		assert(dbus_r_string(&r, iface, sizeof(iface)));
		assert(dbus_r_string(&r, prop, sizeof(prop)));

		dbus_writer_t rw;
		memset(&rw, 0, sizeof(rw));
		if (strcmp(prop, "Volume") == 0) {
			pthread_mutex_lock(&bus_lock);
			int v = mock_stream_volume;
			pthread_mutex_unlock(&bus_lock);
			dbus_w_variant_u16(&rw, (uint16_t)(((v & 0x7f) << 8) | (v & 0x7f)));
		} else {
			pthread_mutex_lock(&bus_lock);
			bool sv = mock_soft_volume;
			pthread_mutex_unlock(&bus_lock);
			dbus_w_variant_bool(&rw, sv);
		}
		assert(!rw.overflow);
		assert(grow_buffer(&c->reply_body, &c->reply_body_size, rw.used ? rw.used : 1));
		memcpy(c->reply_body, rw.buf, rw.used);
		c->reply_body_len = rw.used;
		c->reply_big_endian = host_is_big_endian();
	} else if (strcmp(c->out_member, "Set") == 0) {
		dbus_reader_t r = {.data = c->writer.buf, .len = c->writer.used, .big_endian = host_is_big_endian()};
		char iface[128], prop[64], sig[16];
		assert(dbus_r_string(&r, iface, sizeof(iface)));
		assert(dbus_r_string(&r, prop, sizeof(prop)));
		assert(dbus_r_signature(&r, sig, sizeof(sig)));

		if (strcmp(prop, "SoftVolume") == 0) {
			bool sv = false;
			assert(dbus_r_bool(&r, &sv));
			pthread_mutex_lock(&bus_lock);
			mock_soft_volume = sv;
			soft_write_count++;
			pthread_cond_broadcast(&bus_cond);
			pthread_mutex_unlock(&bus_lock);
		} else if (strcmp(prop, "Volume") == 0) {
			uint16_t packed = 0;
			assert(dbus_r_u16(&r, &packed));
			int left = (packed >> 8) & 0x7f;
			char call_path[DBUS_NAME_MAX];
			snprintf(call_path, sizeof(call_path), "%s", c->out_path);

			pthread_mutex_lock(&bus_lock);
			mock_stream_volume = left;
			if (write_count < (int)(sizeof(write_history) / sizeof(write_history[0]))) {
				write_history[write_count++] = left;
			}
			bool do_quant = auto_quantize_on_set;
			int qdelta = auto_quantize_delta;
			pthread_cond_broadcast(&bus_cond);
			pthread_mutex_unlock(&bus_lock);

			if (do_quant) {
				int echoed = left + qdelta;
				if (echoed < 0) echoed = 0;
				if (echoed > 127) echoed = 127;
				pthread_mutex_unlock(&c->write_lock);
				emit_volume_signal(call_path, echoed);
				return ok;
			}
		}
		c->reply_body_len = 0;
	}

	pthread_mutex_unlock(&c->write_lock);
	return ok;
}

#define clock_gettime test_clock_gettime
#include "../src/system/bluetooth/btvolume.c"
#undef clock_gettime

static const char *const TEST_MAC = "2C:41:A1:11:22:33";
static const char *const TEST_PATH = "/org/bluealsa/hci0/dev_2C_41_A1_11_22_33/a2dpsrc/sink";

static void wait_until_writes(int want_writes) {
	for (int i = 0; i < 250; i++) {
		pthread_mutex_lock(&bus_lock);
		int wc = write_count;
		pthread_mutex_unlock(&bus_lock);
		if (wc >= want_writes) {
			return;
		}
		usleep(2000);
	}
	pthread_mutex_lock(&bus_lock);
	int wc = write_count;
	pthread_mutex_unlock(&bus_lock);
	fprintf(stderr, "timeout waiting for write_count >= %d (got %d)\n", want_writes, wc);
	assert(wc >= want_writes);
}

static void wait_until_available(void) {
	for (int i = 0; i < 250; i++) {
		if (btvolume_available()) {
			return;
		}
		usleep(2000);
	}
	assert(btvolume_available());
}

static void wake_worker(void) {
	pthread_mutex_lock(&lock);
	pthread_cond_signal(&wake);
	pthread_mutex_unlock(&lock);
}

static void setup_adopted_ready(int initial_bt_level) {
	btvolume_init();
	btvolume_set_sync(true);
	btvolume_set_device(TEST_MAC);
	wait_until_available();

	/* Wait until BTV_ADOPT has issued its initial Get("Volume") (last_bus_bt >= 0). */
	for (int i = 0; i < 250; i++) {
		pthread_mutex_lock(&lock);
		int seen = last_bus_bt;
		pthread_mutex_unlock(&lock);
		if (seen >= 0) break;
		usleep(2000);
	}

	/* Headphones announce their initial level during BTV_ADOPT. */
	emit_volume_signal(TEST_PATH, initial_bt_level);
	for (int i = 0; i < 250; i++) {
		if (get_volume_percent() == bt_to_local(initial_bt_level)) {
			break;
		}
		usleep(2000);
	}
	assert(get_volume_percent() == bt_to_local(initial_bt_level));

	pthread_mutex_lock(&player_lock);
	set_volume_calls = 0;
	gui_notify_calls = 0;
	pthread_mutex_unlock(&player_lock);
}

/* 1. Rapid volume-up steps must coalesce over WRITE_MIN_INTERVAL_MS and
 *    delayed/stale AVRCP echoes must never drop player volume back down. */
static void test_rapid_volume_up_and_delayed_echoes(void) {
	reset_mock(20, true, 127);
	setup_adopted_ready(38); /* 30% */

	/* Step 1: 30% -> 31% (bt 39) sends immediately. */
	set_volume_percent(31);
	btvolume_notify_local(31);
	wait_until_writes(1);
	assert(write_history[0] == local_to_bt(31));

	/* User keeps turning volume up within the 150 ms rate-limit window:
	 * 32%, 33%, 34%, 35%, 36%. These must coalesce instead of sending 5
	 * back-to-back synchronous writes. */
	advance_mono_ms(40);
	set_volume_percent(32);
	btvolume_notify_local(32);
	usleep(5000);

	advance_mono_ms(40);
	set_volume_percent(33);
	btvolume_notify_local(33);
	usleep(5000);

	/* Delayed echo for step 1 (39) and a stale lower echo (38) arrive mid-ramp. */
	emit_volume_signal(TEST_PATH, 38);
	emit_volume_signal(TEST_PATH, 39);

	advance_mono_ms(40);
	set_volume_percent(34);
	btvolume_notify_local(34);
	set_volume_percent(35);
	btvolume_notify_local(35);
	set_volume_percent(36);
	btvolume_notify_local(36);
	usleep(10000);

	/* Still inside 150 ms cooldown (120 ms elapsed): only write 0 has gone out. */
	pthread_mutex_lock(&bus_lock);
	int wc_mid = write_count;
	pthread_mutex_unlock(&bus_lock);
	assert(wc_mid == 1);

	/* Advance past the 150 ms cooldown so the coalesced final step (36% -> 46) flushes. */
	advance_mono_ms(40);
	wake_worker();
	wait_until_writes(2);
	assert(write_history[1] == local_to_bt(36));

	/* Now a delayed echo from an intermediate step (e.g. 42) arrives after the
	 * final write (46) went out: must NOT drop player volume back down! */
	emit_volume_signal(TEST_PATH, 42);
	emit_volume_signal(TEST_PATH, 44);
	usleep(20000);

	assert(get_volume_percent() == 36);
	pthread_mutex_lock(&player_lock);
	assert(gui_notify_calls == 0);
	pthread_mutex_unlock(&player_lock);

	btvolume_stop();
}

/* 2. Speaker with coarse volume steps (e.g. Bose SoundLink Micro) rounds our
 *    requested level down and echoes the quantized value back immediately. */
static void test_quantized_speaker_echo_does_not_drop_volume(void) {
	reset_mock(20, true, 127);
	setup_adopted_ready(51); /* 40% */

	pthread_mutex_lock(&bus_lock);
	auto_quantize_on_set = true;
	auto_quantize_delta = -2; /* speaker rounds requested step down by 2 */
	pthread_mutex_unlock(&bus_lock);

	/* Turn up 40% -> 42% -> 44%. */
	set_volume_percent(42);
	btvolume_notify_local(42);
	wait_until_writes(1);
	assert(write_history[0] == local_to_bt(42));
	usleep(15000);
	assert(get_volume_percent() == 42);

	advance_mono_ms(160);
	set_volume_percent(44);
	btvolume_notify_local(44);
	wait_until_writes(2);
	assert(write_history[1] == local_to_bt(44));
	usleep(15000);
	assert(get_volume_percent() == 44);

	pthread_mutex_lock(&player_lock);
	assert(gui_notify_calls == 0);
	pthread_mutex_unlock(&player_lock);

	btvolume_stop();
}

/* 3. After LOCAL_SETTLE_MS expires, a real button press on the headphones IS
 *    adopted onto the player and raises the UI overlay. */
static void test_genuine_remote_change_after_settle(void) {
	reset_mock(20, true, 127);
	setup_adopted_ready(38); /* 30% */

	set_volume_percent(40);
	btvolume_notify_local(40);
	wait_until_writes(1);

	/* Advance past the 2000 ms settle window. */
	advance_mono_ms(2100);
	emit_volume_signal(TEST_PATH, 76); /* ~60% from speaker's own '+' button */

	for (int i = 0; i < 250; i++) {
		if (get_volume_percent() == bt_to_local(76)) {
			break;
		}
		usleep(2000);
	}
	assert(get_volume_percent() == bt_to_local(76));
	pthread_mutex_lock(&player_lock);
	assert(gui_notify_calls == 1);
	assert(last_gui_volume == bt_to_local(76));
	pthread_mutex_unlock(&player_lock);

	btvolume_stop();
}

/* 4. Simultaneous remote signal and pending local update must not discard local. */
static void test_local_wins_over_concurrent_remote(void) {
	reset_mock(20, true, 127);
	setup_adopted_ready(51); /* 40% */

	/* Advance past settle so handle_properties_changed does enqueue pending_remote. */
	advance_mono_ms(2100);
	emit_volume_signal(TEST_PATH, 30); /* remote drop */
	set_volume_percent(50);
	btvolume_notify_local(50);

	wait_until_writes(1);
	assert(write_history[0] == local_to_bt(50));
	usleep(15000);
	assert(get_volume_percent() == 50);

	btvolume_stop();
}

/* 5. BTV_ADOPT: route transition without btvolume_notify_local adopts the
 *    headphones' level; an explicit hand-turn while on jack beats adoption. */
static void test_adopt_vs_hand_turn(void) {
	/* Case A: profile switches to BT cleanly without calling btvolume_notify_local. */
	reset_mock(55, false, 127);
	btvolume_init();
	btvolume_set_sync(true);
	btvolume_set_device(TEST_MAC);
	wait_until_available();
	usleep(10000);

	/* Profile switches to BT (like alsa-controls.c with suppress_bt_notify). */
	pthread_mutex_lock(&player_lock);
	player_volume = 65; /* saved bt_volume profile */
	player_bt_profile = true;
	pthread_mutex_unlock(&player_lock);
	wake_worker();

	/* Wait for initial Get("Volume") in BTV_ADOPT, then deliver headphone level 25 (~20%). */
	for (int i = 0; i < 250; i++) {
		pthread_mutex_lock(&lock);
		int seen = last_bus_bt;
		pthread_mutex_unlock(&lock);
		if (seen >= 0) break;
		usleep(2000);
	}
	emit_volume_signal(TEST_PATH, 25);

	for (int i = 0; i < 250; i++) {
		if (get_volume_percent() == bt_to_local(25)) {
			break;
		}
		usleep(2000);
	}
	assert(get_volume_percent() == bt_to_local(25));
	btvolume_stop();

	/* Case B: user deliberately moves volume while still on jack -> wins over adoption. */
	reset_mock(55, false, 127);
	btvolume_init();
	btvolume_set_sync(true);
	btvolume_set_device(TEST_MAC);
	wait_until_available();

	set_volume_percent(70);
	btvolume_notify_local(70);
	usleep(10000);

	pthread_mutex_lock(&player_lock);
	player_bt_profile = true;
	pthread_mutex_unlock(&player_lock);
	wake_worker();

	wait_until_writes(1);
	assert(write_history[0] == local_to_bt(70));
	assert(get_volume_percent() == 70);
	btvolume_stop();
}

/* 6. Separate (SoftVolume = true) mode writes immediately without 150 ms
 *    AVRCP cooldown and ignores remote Volume signals. */
static void test_separate_mode_immediate_and_ignores_remote(void) {
	reset_mock(35, true, 127);
	btvolume_init();
	btvolume_set_sync(false);
	btvolume_set_device(TEST_MAC);
	wait_until_available();

	/* Initial player volume 35% is written immediately upon entering BTV_READY. */
	wait_until_writes(1);
	assert(write_history[0] == local_to_bt(35));
	assert(mock_soft_volume == true);

	/* Immediate second local step within <10 ms is not rate-limited in SoftVolume mode. */
	set_volume_percent(45);
	btvolume_notify_local(45);
	wait_until_writes(2);
	assert(write_history[1] == local_to_bt(45));

	/* Remote signal in separate mode must not move player volume. */
	advance_mono_ms(3000);
	emit_volume_signal(TEST_PATH, 10);
	usleep(15000);
	assert(get_volume_percent() == 45);

	btvolume_stop();
}

int main(void) {
	test_rapid_volume_up_and_delayed_echoes();
	test_quantized_speaker_echo_does_not_drop_volume();
	test_genuine_remote_change_after_settle();
	test_local_wins_over_concurrent_remote();
	test_adopt_vs_hand_turn();
	test_separate_mode_immediate_and_ignores_remote();
	printf("PASS btvolume (AVRCP echo suppression, step quantization, coalescing, adoption and separate mode)\n");
	return 0;
}
