/* Exercise the real Bluetooth link decision and startup wait policy without a
 * controller or daemons. Unused hardware paths are discarded by linker GC. */
#define _GNU_SOURCE 1
#include "../src/system/bluetooth/bluetooth.c"
#include "src/system/bluetooth/btafh.h"

#include <assert.h>
#include <poll.h>
#include <limits.h>

static bool source_pcm;

void config_set_int(const char *section, const char *key, long value) {
	(void)section;
	(void)key;
	(void)value;
}

// Unset by default, so every case sees the built-in default; the car-mode
// cases set these two and put them back.
static const char *fake_car_mode;
static const char *fake_car_codec;

const char *config_get(const char *section, const char *key, const char *fallback) {
	if (strcmp(section, "bluetooth") == 0 && strcmp(key, "car_mode") == 0 && fake_car_mode) {
		return fake_car_mode;
	}
	if (strcmp(section, "bluetooth") == 0 && strcmp(key, "car_codec") == 0 && fake_car_codec) {
		return fake_car_codec;
	}
	return fallback;
}

void config_set(const char *section, const char *key, const char *value) {
	(void)section;
	(void)key;
	(void)value;
}

bool config_save(void) { return true; }

// Reachable now that car mode can restart bluealsa: the restart is a pointer
// whose default is the real one, so what the real one calls has to link. No
// case runs it -- main() points the restart at a counter before anything else,
// because the real one stops every process called bluealsa on the machine.
long config_get_int(const char *section, const char *key, long fallback) {
	(void)section;
	(void)key;
	return fallback;
}
bool btstack_wait_service(const char *name, int timeout_ms) {
	(void)name;
	(void)timeout_ms;
	return false;
}
bool btstack_bluealsa_version(char *out, size_t size) {
	(void)out;
	(void)size;
	return false;
}
bool btlog_enabled(void) { return false; }

// What apply_output_routing() asked the player to play on. The routing is the
// point of the case below: a radio being parked drives the same call a
// headphone connection does, and what it hands over is what the next PCM open
// will use.
static char routed_to[128] = "default";

void audio_set_output_device(const char *pcm) {
	snprintf(routed_to, sizeof(routed_to), "%s", (pcm && pcm[0]) ? pcm : "default");
}

void audio_get_output_device(char *out, size_t size) {
	if (out && size) {
		snprintf(out, size, "%s", routed_to);
	}
}

bool btstack_audio_source(char *address_out, size_t size) {
	if (address_out && size) {
		snprintf(address_out, size, "%s", source_pcm ? "20:04:84:3A:6C:FE" : "");
	}
	return source_pcm;
}

static int sent_count;
static int sent_freq;
static bool fake_apply(int freq) {
	sent_count++;
	sent_freq = freq;
	return true;
}

static void test_a2dp_directions(void) {
	btafh_follow_t follow = {0};
	g_enabled = false;
	g_a2dp_connected = true;
	source_pcm = true; /* stale cache during teardown */
	assert(!bluetooth_audio_active());
	assert(!bluetooth_a2dp_link_active());

	g_enabled = true;
	g_a2dp_connected = false;
	source_pcm = false;
	assert(!bluetooth_audio_active());
	assert(!bluetooth_a2dp_link_active());

	/* This is the log's phone-to-player stream: it must not change the local
	 * output route, but must drive the same coexistence policy as headphones. */
	source_pcm = true;
	assert(!bluetooth_audio_active());
	assert(bluetooth_a2dp_link_active());
	btafh_follow_wifi(&follow, 1, bluetooth_get_enabled(), bluetooth_a2dp_link_active(), 2437, fake_apply);
	assert(sent_count == 1 && sent_freq == 2437);
	source_pcm = false;
	btafh_follow_wifi(&follow, 2, bluetooth_get_enabled(), bluetooth_a2dp_link_active(), 2437, fake_apply);
	assert(sent_count == 2 && sent_freq == 0);

	/* Headphones retain the original output-routing meaning. */
	g_a2dp_connected = true;
	assert(bluetooth_audio_active());
	assert(bluetooth_a2dp_link_active());
	source_pcm = true;
	assert(bluetooth_audio_active() && bluetooth_a2dp_link_active());
	g_enabled = false;
	assert(!bluetooth_audio_active() && !bluetooth_a2dp_link_active());
}

static int simulated_ms;
static int stock_until_ms;
static int transport_until_ms;
static int hci_ready_at_ms;
static int cancel_at_ms;
static int ready_calls;

static bool fake_running(const char *name) {
	if (strcmp(name, "bt_init") == 0) {
		return simulated_ms < stock_until_ms;
	}
	assert(strcmp(name, "brcm_patchram_plus") == 0);
	return simulated_ms < transport_until_ms;
}

static bool fake_ready(void) {
	ready_calls++;
	return simulated_ms >= hci_ready_at_ms;
}

static bool fake_wanted(void) { return simulated_ms < cancel_at_ms; }
static void fake_sleep(int ms) { simulated_ms += ms; }

static const bt_startup_checks_t fake_checks = {fake_running, fake_ready, fake_wanted, fake_sleep};

static void reset_fake(void) {
	simulated_ms = 0;
	stock_until_ms = 0;
	transport_until_ms = 0;
	hci_ready_at_ms = INT_MAX;
	cancel_at_ms = INT_MAX;
	ready_calls = 0;
}

static void test_startup_waits(void) {
	reset_fake();
	assert(wait_out_stock_bringup(&fake_checks, 1000));
	assert(simulated_ms == 0);

	/* hci0 appearing early does not mean bt_init finished: the stock script
	 * can still start bluetoothd or power the adapter off afterwards. */
	reset_fake();
	stock_until_ms = 800;
	hci_ready_at_ms = 0;
	assert(wait_out_stock_bringup(&fake_checks, 1000));
	assert(simulated_ms == 800 && ready_calls == 0);

	reset_fake();
	stock_until_ms = INT_MAX;
	hci_ready_at_ms = 0;
	assert(!wait_out_stock_bringup(&fake_checks, 400));
	assert(simulated_ms == 400 && ready_calls == 0);

	/* A switch change cancels the wait instead of doing stale work. This also
	 * models cancelling a boot cleanup when the user switches Bluetooth on. */
	reset_fake();
	stock_until_ms = INT_MAX;
	cancel_at_ms = 400;
	assert(!wait_out_stock_bringup(&fake_checks, 1000));
	assert(simulated_ms == 400);

	/* An existing patchram may become ready; it must not be duplicated. */
	reset_fake();
	transport_until_ms = 1000;
	hci_ready_at_ms = 300;
	assert(wait_existing_transport(&fake_checks, 1000) == BT_TRANSPORT_READY);
	assert(simulated_ms == 300);

	/* Once the old process has exited, and only then, a new one may start. */
	reset_fake();
	transport_until_ms = 300;
	assert(wait_existing_transport(&fake_checks, 1000) == BT_TRANSPORT_START);
	assert(simulated_ms == 300);

	reset_fake();
	transport_until_ms = INT_MAX;
	assert(wait_existing_transport(&fake_checks, 500) == BT_TRANSPORT_BUSY);
	assert(simulated_ms == 500);

	reset_fake();
	transport_until_ms = INT_MAX;
	cancel_at_ms = 200;
	assert(wait_existing_transport(&fake_checks, 1000) == BT_TRANSPORT_CANCELLED);
	assert(simulated_ms == 200);
}

static void test_process_matching(void) {
	assert(comm_matches("brcm_patchram_p", "brcm_patchram_plus"));
	assert(!comm_matches("brcm_patchram_x", "brcm_patchram_plus"));
	assert(comm_matches("bt_init", "bt_init"));

	const char sh[] = "/bin/sh\0/usr/bin/bt_init\0";
	const char shell_options[] = "/bin/sh\0-e\0/usr/bin/bt_init\0";
	const char busybox[] = "/bin/busybox\0sh\0/usr/bin/bt_init\0";
	const char other[] = "/bin/sh\0/usr/bin/bt_init_old\0";
	const char shell_command[] = "/bin/sh\0-c\0/usr/bin/bt_init\0";
	const char mention[] = "/bin/sh\0-c\0echo /usr/bin/bt_init\0";
	const char positional[] = "/bin/sh\0-c\0true\0/usr/bin/bt_init\0";
	assert(shell_invokes_bt_init(sh, sizeof(sh)));
	assert(shell_invokes_bt_init(shell_options, sizeof(shell_options)));
	assert(shell_invokes_bt_init(busybox, sizeof(busybox)));
	assert(shell_invokes_bt_init(shell_command, sizeof(shell_command)));
	assert(!shell_invokes_bt_init(other, sizeof(other)));
	assert(!shell_invokes_bt_init(mention, sizeof(mention)));
	assert(!shell_invokes_bt_init(positional, sizeof(positional)));
	assert(!shell_invokes_bt_init(sh, sizeof(sh) - 3)); /* incomplete argument */

	/* Exercise /proc as well: an explicitly interpreted script has comm=sh. */
	char dir[] = "/tmp/sonix-bt-init-XXXXXX";
	assert(mkdtemp(dir));
	char path[128];
	snprintf(path, sizeof(path), "%s/bt_init", dir);
	int ready[2];
	assert(pipe(ready) == 0);
	FILE *f = fopen(path, "w");
	assert(f);
	assert(fprintf(f, "printf x >&%d\nwhile :; do sleep 1; done\n", ready[1]) > 0);
	assert(fclose(f) == 0);
	pid_t child = fork();
	assert(child >= 0);
	if (child == 0) {
		close(ready[0]);
		execl("/bin/sh", "sh", path, (char *)NULL);
		_exit(127);
	}
	close(ready[1]);
	struct pollfd poller = {.fd = ready[0], .events = POLLIN};
	assert(poll(&poller, 1, 3000) > 0);
	char byte;
	assert(read(ready[0], &byte, 1) == 1 && byte == 'x');
	close(ready[0]);
	char pid_text[32];
	snprintf(pid_text, sizeof(pid_text), "%ld", (long)child);
	assert(shell_running_bt_init(pid_text, "sh"));
	assert(process_running("bt_init"));
	assert(kill(child, SIGTERM) == 0);
	assert(waitpid(child, NULL, 0) == child);
	assert(unlink(path) == 0 && rmdir(dir) == 0);
}

static void test_alsa_template(void) {
	char dir[] = "/tmp/sonix-bt-alsa-XXXXXX";
	assert(mkdtemp(dir));
	char path[128];
	snprintf(path, sizeof(path), "%s/alsa.conf", dir);
	assert(ensure_alsa_conf_template(path));
	char text[4096];
	assert(slurp(path, text, sizeof(text)));
	assert(strstr(text, "pcm.bt_alsa_sink {") && strstr(text, "device XX:XX:XX:XX:XX:XX"));
	assert(strstr(text, "ldac_eqmid \"LDAC_ABR\"") && strstr(text, "uat_eqmid \"UAT_600\""));

	/* A legacy or user-edited template is never replaced. */
	FILE *f = fopen(path, "w");
	assert(f && fputs("custom settings\n", f) >= 0 && fclose(f) == 0);
	assert(ensure_alsa_conf_template(path));
	assert(slurp(path, text, sizeof(text)) && strcmp(text, "custom settings\n") == 0);
	assert(unlink(path) == 0);
	assert(rmdir(dir) == 0);
	assert(!ensure_alsa_conf_template(path)); /* missing parent is a failure */
}

static void make_rfkill(const char *root, const char *entry, const char *name) {
	char path[256];
	snprintf(path, sizeof(path), "%s/%s", root, entry);
	assert(mkdir(path, 0755) == 0);
	snprintf(path, sizeof(path), "%s/%s/name", root, entry);
	FILE *f = fopen(path, "w");
	assert(f && fputs(name, f) >= 0 && fclose(f) == 0);
}

static void drop_rfkill(const char *root, const char *entry) {
	char path[256];
	snprintf(path, sizeof(path), "%s/%s/name", root, entry);
	assert(unlink(path) == 0);
	snprintf(path, sizeof(path), "%s/%s", root, entry);
	assert(rmdir(path) == 0);
}

static bool switch_is(const char *root, const char *entry) {
	char path[320], expected[320];
	rfkill_state_path(root, path, sizeof(path));
	snprintf(expected, sizeof(expected), "%s/%s/state", root, entry);
	return strcmp(path, expected) == 0;
}

static void test_rfkill_lookup(void) {
	char dir[] = "/tmp/sonix-bt-rfkill-XXXXXX";
	assert(mkdtemp(dir));

	/* No switch at all, or none named for Bluetooth: rfkill0, where every
	 * firmware before 1.3 had the radio and where its bt_init falls back. */
	assert(switch_is(dir, "rfkill0"));
	make_rfkill(dir, "rfkill0", "phy0\n");
	assert(switch_is(dir, "rfkill0"));

	/* The radio's own switch is found by name, wherever it was numbered;
	 * hci0's switch is of type bluetooth too, but is not the radio. */
	make_rfkill(dir, "rfkill1", "hci0\n");
	make_rfkill(dir, "rfkill2", "bluetooth\n");
	assert(switch_is(dir, "rfkill2"));

	/* The lowest number wins; the match is case-insensitive, by numbers and
	 * not by name order (rfkill10 sorts before rfkill2). */
	make_rfkill(dir, "rfkill10", "BT_Bluetooth\n");
	assert(switch_is(dir, "rfkill2"));
	drop_rfkill(dir, "rfkill2");
	assert(switch_is(dir, "rfkill10"));

	/* Names that only look like a switch are not one. */
	make_rfkill(dir, "rfkillx", "bluetooth\n");
	make_rfkill(dir, "rfkill3x", "bluetooth\n");
	assert(switch_is(dir, "rfkill10"));

	drop_rfkill(dir, "rfkill0");
	drop_rfkill(dir, "rfkill1");
	drop_rfkill(dir, "rfkill10");
	drop_rfkill(dir, "rfkillx");
	drop_rfkill(dir, "rfkill3x");
	assert(rmdir(dir) == 0);
}

static void test_boot_cleanup_queue(void) {
	worker_running = true;
	g_enabled = false;
	queue_head = queue_count = 0;

	/* A switch-on queued after cleanup must be processed on the same worker;
	 * cleanup sees the current switch and skips rather than stopping a new
	 * stack. If cleanup was already taken, power-on still follows it. */
	bluetooth_power_down_stack();
	set_enabled(true, false);
	job_t job;
	assert(take_job(&job) && job.type == JOB_BOOT_CLEANUP);
	assert(!cleanup_checks.keep_waiting());
	assert(take_job(&job) && job.type == JOB_POWER && job.arg_int == 1);
	assert(!take_job(&job));

	g_enabled = false;
	bluetooth_power_down_stack();
	assert(take_job(&job) && job.type == JOB_BOOT_CLEANUP);
	assert(cleanup_checks.keep_waiting());
	set_enabled(true, false);
	assert(!cleanup_checks.keep_waiting());
	assert(take_job(&job) && job.type == JOB_POWER && job.arg_int == 1);
	assert(!take_job(&job));
	worker_running = false;
}

static btstack_device_t fake_devs[4];
static int fake_dev_count;
static int trust_calls;
static char last_trusted_mac[BT_MAC_MAX];
static int connect_calls;
static bool fake_visible;
static int discovery_starts, discovery_stops, visibility_writes;
static bool discovery_result = true;
static bool cancel_on_start;
static char connected_order[4][BT_MAC_MAX];

int btstack_devices(btstack_device_t *out, int max_out) {
	int n = fake_dev_count < max_out ? fake_dev_count : max_out;
	for (int i = 0; i < n; i++) {
		out[i] = fake_devs[i];
	}
	return n;
}

bool btstack_device_connected(const char *address, bool *out) {
	for (int i = 0; i < fake_dev_count; i++) {
		if (strcasecmp(fake_devs[i].address, address) == 0) {
			if (out) {
				*out = fake_devs[i].connected;
			}
			return true;
		}
	}
	return false;
}

bool btstack_trust(const char *address, bool trusted) {
	trust_calls++;
	snprintf(last_trusted_mac, sizeof(last_trusted_mac), "%s", address);
	for (int i = 0; i < fake_dev_count; i++) {
		if (strcasecmp(fake_devs[i].address, address) == 0) {
			fake_devs[i].trusted = trusted;
		}
	}
	return true;
}

bool btstack_pair_why(const char *address, int timeout_ms, char *err_out, size_t err_size) {
	(void)timeout_ms;
	if (err_out && err_size) {
		err_out[0] = '\0';
	}
	for (int i = 0; i < fake_dev_count; i++) {
		if (strcasecmp(fake_devs[i].address, address) == 0) {
			fake_devs[i].paired = true;
			return true;
		}
	}
	return false;
}

bool btstack_cancel_pairing(const char *address) {
	(void)address;
	return true;
}

bool btstack_discovery(bool on) {
	if (on) {
		discovery_starts++;
		assert(fake_visible); // the pairing window precedes inquiry
		if (cancel_on_start) {
			pthread_mutex_lock(&lock);
			g_scanning = false;
			pthread_mutex_unlock(&lock);
		}
		return discovery_result;
	}
	discovery_stops++;
	return true;
}

bool btstack_set_discoverable(bool on) {
	fake_visible = on;
	visibility_writes++;
	return true;
}

bool btstack_connect(const char *address, int timeout_ms, char *err_out, size_t err_size) {
	(void)timeout_ms;
	if (err_out && err_size) {
		err_out[0] = '\0';
	}
	if (connect_calls < 4) {
		snprintf(connected_order[connect_calls], BT_MAC_MAX, "%s", address);
	}
	connect_calls++;
	for (int i = 0; i < fake_dev_count; i++) {
		if (strcasecmp(fake_devs[i].address, address) == 0) {
			fake_devs[i].connected = true;
			return true;
		}
	}
	return false;
}

// The A2DP profile asked for by name, which is what a car needs: see
// invite_sinks(). `a2dp_profile_brings_audio` is the case where the profile
// request is what puts the PCM on the bus.
static char fake_sink[BT_ADDR_MAX];
static int a2dp_profile_requests;
static char a2dp_profile_requested_for[BT_ADDR_MAX];
static bool a2dp_profile_brings_audio;
static bool a2dp_profile_refused; // the car's "security block"
static bool a2dp_profile_busy;	  // bluez holding the stream: "Device or resource busy"

bool btstack_connect_a2dp_why(const char *address, int timeout_ms, char *err_out, size_t err_size) {
	(void)timeout_ms;
	if (err_out && err_size) {
		err_out[0] = '\0';
	}
	a2dp_profile_requests++;
	snprintf(a2dp_profile_requested_for, sizeof(a2dp_profile_requested_for), "%.*s",
			 (int)(sizeof(a2dp_profile_requested_for) - 1), address);
	if (a2dp_profile_busy) {
		// The words of the device's bluetoothd, as the player's log has them.
		if (err_out && err_size) {
			snprintf(err_out, err_size, "org.bluez.Error.Failed: Device or resource busy");
		}
		return false;
	}
	if (a2dp_profile_refused) {
		if (err_out && err_size) {
			snprintf(err_out, err_size, "org.bluez.Error.Failed: Connection refused - security block");
		}
		return false;
	}
	if (a2dp_profile_brings_audio) {
		snprintf(fake_sink, sizeof(fake_sink), "%.*s", (int)(sizeof(fake_sink) - 1), address);
	}
	return true;
}

bool btstack_connect_a2dp(const char *address, int timeout_ms) {
	return btstack_connect_a2dp_why(address, timeout_ms, NULL, 0);
}

bool btstack_connect_a2dp_source(const char *address, int timeout_ms) {
	return btstack_connect_a2dp_source_why(address, timeout_ms, NULL, 0);
}

bool btstack_connect_a2dp_source_why(const char *address, int timeout_ms, char *err_out, size_t err_size) {
	(void)timeout_ms;
	if (err_out && err_size) {
		err_out[0] = '\0';
	}
	if (connect_calls < 4) {
		snprintf(connected_order[connect_calls], BT_MAC_MAX, "%s", address);
	}
	connect_calls++;
	for (int i = 0; i < fake_dev_count; i++) {
		if (strcasecmp(fake_devs[i].address, address) == 0) {
			fake_devs[i].connected = true;
			source_pcm = true;
			return true;
		}
	}
	return false;
}

bool btstack_disconnect(const char *address) {
	for (int i = 0; i < fake_dev_count; i++) {
		if (strcasecmp(fake_devs[i].address, address) == 0) {
			fake_devs[i].connected = false;
			return true;
		}
	}
	return false;
}

bool btstack_audio_sink(char *address_out, size_t size) {
	if (address_out && size) {
		snprintf(address_out, size, "%s", fake_sink);
	}
	return fake_sink[0] != '\0';
}

// A sink that came up on SBC and also lists AAC, which is when the player asks
// for the better one by itself; off by default, where there is nothing to pick.
static bool fake_offers_aac;
static int select_codec_calls;

int btstack_codecs(const char *address, char out[][BT_CODEC_NAME_MAX], int max, char *selected, size_t selected_size) {
	(void)address;
	if (selected && selected_size) {
		selected[0] = '\0';
	}
	if (!fake_offers_aac || max < 2) {
		return 0;
	}
	snprintf(out[0], BT_CODEC_NAME_MAX, "SBC");
	snprintf(out[1], BT_CODEC_NAME_MAX, "AAC");
	if (selected && selected_size) {
		snprintf(selected, selected_size, "SBC");
	}
	return 2;
}

bool btstack_select_codec(const char *address, const char *codec) {
	(void)address;
	(void)codec;
	select_codec_calls++;
	return false;
}

bool btstack_audio_info(const char *address, char *out, size_t size) {
	(void)address;
	if (out && size) out[0] = '\0';
	return false;
}

bool btstack_stream_info(const char *address, bool receiving, btstack_stream_t *out) {
	(void)address;
	(void)receiving;
	if (out) {
		memset(out, 0, sizeof(*out));
	}
	return false;
}

bool btreceiver_is_active(void) { return false; }
bool sonixlink_bt_is_peer(const char *mac) {
	(void)mac;
	return false;
}

static void test_rx_discoverability_and_phone_pairing(void) {
	/* Idle TX stays hidden unless explicitly requested (e.g. SonixLink). A
	 * scan opens a temporary pairing window; RX is always discoverable. */
	g_receiver_profile = false;
	g_discoverable = false;
	assert(!bluetooth_receiver_profile());
	assert(!effective_discoverable());

	g_discoverable = true;
	assert(effective_discoverable());
	g_discoverable = false;
	g_scanning = true;
	assert(effective_discoverable());
	g_scanning = false;
	assert(!effective_discoverable());

	g_receiver_profile = true;
	g_discoverable = false;
	assert(bluetooth_receiver_profile());
	assert(effective_discoverable());

	/* Incoming phone pairings arrive with paired=true, trusted=false:
	 * refresh_devices() must mark them trusted so subsequent phone-initiated
	 * A2DP connections are accepted by BlueZ without authorization refusal. */
	memset(fake_devs, 0, sizeof(fake_devs));
	fake_dev_count = 2;
	snprintf(fake_devs[0].address, sizeof(fake_devs[0].address), "11:22:33:44:55:66");
	snprintf(fake_devs[0].name, sizeof(fake_devs[0].name), "Older Tablet");
	fake_devs[0].paired = true;
	fake_devs[0].trusted = true;
	fake_devs[0].audio_source = true;

	snprintf(fake_devs[1].address, sizeof(fake_devs[1].address), "AA:BB:CC:DD:EE:FF");
	snprintf(fake_devs[1].name, sizeof(fake_devs[1].name), "Galaxy Z Fold");
	fake_devs[1].paired = true;
	fake_devs[1].trusted = false;
	fake_devs[1].audio_source = true;

	trust_calls = 0;
	source_pcm = false;
	g_enabled = true;
	refresh_devices();
	assert(trust_calls == 1);
	assert(strcmp(last_trusted_mac, "AA:BB:CC:DD:EE:FF") == 0);
	assert(fake_devs[1].trusted);

	/* Tapping an audio-source phone from the TX page trusts and remembers it,
	 * defers A2DP Connect until RX mode's AudioSink profile is up, and then
	 * invite_sources() connects that preferred phone first. */
	g_receiver_profile = false;
	connect_calls = 0;
	g_preferred_source[0] = '\0';
	bool deferred = false;
	assert(pair_and_connect("AA:BB:CC:DD:EE:FF", &deferred));
	assert(deferred);
	assert(connect_calls == 0);
	assert(strcmp(g_preferred_source, "AA:BB:CC:DD:EE:FF") == 0);

	g_receiver_profile = true;
	invite_sources();
	assert(connect_calls == 1);
	assert(strcmp(connected_order[0], "AA:BB:CC:DD:EE:FF") == 0);
	assert(g_preferred_source[0] == '\0');

	source_pcm = false;
	g_enabled = false;
	g_receiver_profile = false;
}

// A sink that is linked with no A2DP profile: the car case. It is asked for
// the profile by name -- once, after a grace period, and only while nothing is
// playing -- because bluez does not open the audio profile of a connection the
// other end started.
static void test_linked_sink_is_invited(void) {
	memset(sink_invites, 0, sizeof(sink_invites));
	sink_invite_due = 0;
	fake_sink[0] = '\0';
	a2dp_profile_requests = 0;
	a2dp_profile_brings_audio = true;
	source_pcm = false;
	g_enabled = true;
	g_receiver_profile = false;
	pthread_mutex_lock(&lock);
	queue_head = 0;
	queue_count = 0;
	g_paired_count = 0;
	pthread_mutex_unlock(&lock);

	memset(fake_devs, 0, sizeof(fake_devs));
	fake_dev_count = 2;
	// The car: paired, connected, an A2DP sink, and nothing carrying audio.
	snprintf(fake_devs[0].address, sizeof(fake_devs[0].address), "3C:2E:F9:11:22:33");
	snprintf(fake_devs[0].name, sizeof(fake_devs[0].name), "Model Y");
	fake_devs[0].paired = true;
	fake_devs[0].connected = true;
	fake_devs[0].audio_sink = true;
	// A phone that is connected so it can stream here is the other direction's
	// business and must not be asked for a sink profile.
	snprintf(fake_devs[1].address, sizeof(fake_devs[1].address), "20:04:84:3A:6C:FE");
	snprintf(fake_devs[1].name, sizeof(fake_devs[1].name), "Galaxy Z Fold");
	fake_devs[1].paired = true;
	fake_devs[1].connected = true;
	fake_devs[1].audio_source = true;

	// The cached list the interface draws, which is what says whether anything
	// is linked at all before the fuller one is asked for.
	refresh_devices();

	// First sighting: only noted, and the worker is told when to look again.
	invite_sinks();
	assert(a2dp_profile_requests == 0);
	assert(sink_invite_due != 0);

	// Once the grace has run out the profile is requested, by name.
	sink_invites[0].linked_since = now_ms() - (SINK_INVITE_GRACE_MS + 1000);
	invite_sinks();
	assert(a2dp_profile_requests == 1);
	assert(strcmp(a2dp_profile_requested_for, "3C:2E:F9:11:22:33") == 0);
	assert(sink_invite_due == 0);

	// The PCM came up: the route follows it (refresh_audio_state), and nothing
	// more is asked for.
	refresh_devices();
	assert(g_a2dp_connected);
	assert(!strcmp(routed_to, "bluealsa:DEV=3C:2E:F9:11:22:33,PROFILE=a2dp"));

	// Asked once per link, even if the PCM is not there after all.
	fake_sink[0] = '\0';
	invite_sinks();
	assert(a2dp_profile_requests == 1);

	// Once the device goes, the next link is asked again.
	fake_devs[0].connected = false;
	refresh_devices();
	invite_sinks();
	assert(sink_invites[0].mac[0] == '\0');
	fake_devs[0].connected = true;
	refresh_devices();
	invite_sinks();
	assert(a2dp_profile_requests == 1); // seen for the first time again: grace
	fake_sink[0] = '\0';

	// Receiver mode carries no A2DP Source profile at all: nothing to ask for.
	sink_invites[0].linked_since = now_ms() - (SINK_INVITE_GRACE_MS + 1000);
	g_receiver_profile = true;
	invite_sinks();
	assert(a2dp_profile_requests == 1);
	g_receiver_profile = false;

	// And a user action in flight owns the link on its own.
	pthread_mutex_lock(&lock);
	queue_head = 0;
	queue_count = 1;
	queue[0].type = JOB_CONNECT;
	snprintf(queue[0].mac, sizeof(queue[0].mac), "3C:2E:F9:11:22:33");
	pthread_mutex_unlock(&lock);
	invite_sinks();
	assert(a2dp_profile_requests == 1);
	pthread_mutex_lock(&lock);
	queue_head = 0;
	queue_count = 0;
	pthread_mutex_unlock(&lock);

	// A refusal is not the end of the link. The Tesla answer to the first ask is
	// "security block" -- the link is not authenticated yet -- and then
	// "resource busy" while bluez holds the attempt; asked once, the car stayed
	// silent until it was reconnected by hand five minutes later.
	a2dp_profile_requests = 0;
	a2dp_profile_refused = true;
	fake_devs[0].connected = true;
	refresh_devices();
	invite_sinks();
	assert(a2dp_profile_requests == 1);
	// Asked again soon, not immediately: the answer needs a moment to change.
	assert(sink_invite_due != 0);
	invite_sinks();
	assert(a2dp_profile_requests == 1);
	for (int attempt = 1; attempt < SINK_INVITE_TRIES; attempt++) {
		sink_invites[0].retry_at = now_ms() - 1;
		invite_sinks();
		assert(a2dp_profile_requests == attempt + 1);
	}
	// Bounded: a car that keeps refusing is left alone rather than polled
	// forever, and bluez's own reconnect has the link.
	assert(sink_invites[0].asked);
	sink_invites[0].retry_at = now_ms() - 1;
	invite_sinks();
	assert(a2dp_profile_requests == SINK_INVITE_TRIES);

	// The next link -- a disconnect and a reconnect -- is asked again.
	a2dp_profile_refused = false;
	fake_devs[0].connected = false;
	refresh_devices();
	invite_sinks();
	assert(sink_invites[0].tries == 0);
	fake_devs[0].connected = true;
	refresh_devices();
	invite_sinks();
	sink_invites[0].linked_since = now_ms() - (SINK_INVITE_GRACE_MS + 1000);
	invite_sinks();
	assert(a2dp_profile_requests == SINK_INVITE_TRIES + 1);
	pthread_mutex_lock(&lock);
	queue_head = 0;
	queue_count = 0;
	pthread_mutex_unlock(&lock);

	a2dp_profile_brings_audio = false;
	g_enabled = false;
	fake_dev_count = 0;
}

// What the car-mode cases count instead of a restarted daemon.
static int car_restarts;
static char car_restart_for[BT_MAC_MAX]; // g_car_mac when the daemon was restarted
static char car_restart_why[64];
static bool car_restart_resumed;

static void fake_car_restart(const char *why, bool resume) {
	car_restarts++;
	snprintf(car_restart_for, sizeof(car_restart_for), "%s", g_car_mac);
	snprintf(car_restart_why, sizeof(car_restart_why), "%s", why);
	car_restart_resumed = resume;
}

// Two devices nothing is linked to yet: the car of issue 265 with its class,
// and a pair of earbuds.
#define TESLA "4C:FC:AA:12:D5:CC"
#define BUDS "54:10:4F:B0:AD:4D"

static void car_fixture(void) {
	memset(sink_invites, 0, sizeof(sink_invites));
	sink_invite_due = 0;
	fake_sink[0] = '\0';
	a2dp_profile_requests = 0;
	a2dp_profile_brings_audio = false;
	a2dp_profile_refused = false;
	a2dp_profile_busy = false;
	source_pcm = false;
	g_enabled = true;
	g_receiver_profile = false;
	pthread_mutex_lock(&lock);
	queue_head = 0;
	queue_count = 0;
	g_paired_count = 0;
	pthread_mutex_unlock(&lock);

	g_car_mac[0] = '\0';
	g_car_gone_since = 0;
	g_car_stage_floor = CAR_STAGE_SBC;
	car_restart = fake_car_restart;
	car_restarts = 0;
	car_restart_for[0] = '\0';
	fake_car_mode = NULL;
	fake_car_codec = NULL;

	memset(fake_devs, 0, sizeof(fake_devs));
	fake_dev_count = 2;
	snprintf(fake_devs[0].address, sizeof(fake_devs[0].address), TESLA);
	snprintf(fake_devs[0].name, sizeof(fake_devs[0].name), "Tesla Model Y");
	fake_devs[0].paired = true;
	fake_devs[0].audio_sink = true;
	fake_devs[0].cod = 0x240408; // Audio/Video, hands-free: what both cars announce
	snprintf(fake_devs[1].address, sizeof(fake_devs[1].address), BUDS);
	snprintf(fake_devs[1].name, sizeof(fake_devs[1].name), "Buds2 Pro");
	fake_devs[1].paired = true;
	fake_devs[1].audio_sink = true;
	fake_devs[1].cod = 0x240404; // Audio/Video, wearable headset
	refresh_devices();
}

static void car_fixture_done(void) {
	g_car_mac[0] = '\0';
	g_car_gone_since = 0;
	fake_car_mode = NULL;
	fake_car_codec = NULL;
	a2dp_profile_busy = false;
	a2dp_profile_refused = false;
	fake_sink[0] = '\0';
	memset(sink_invites, 0, sizeof(sink_invites));
	sink_invite_due = 0;
	g_enabled = false;
	fake_dev_count = 0;
	refresh_devices();
}

// Issue 265, the second car: bluez answered every one of the five invitations
// "Device or resource busy" because it was itself waiting, thirty seconds, for
// the car to configure the channel the car had opened. Asking four seconds
// apart could not change that; it is left alone until bluez is done.
static void test_busy_invitation_is_left_to_bluez(void) {
	assert(invite_was_busy("org.bluez.Error.Failed: Device or resource busy"));
	assert(invite_was_busy("org.bluez.Error.InProgress: Operation already in progress"));
	assert(!invite_was_busy("org.bluez.Error.Failed: Connection refused - security block"));
	assert(!invite_was_busy("org.bluez.Error.Failed: Resource temporarily unavailable"));
	assert(!invite_was_busy(""));
	assert(!invite_was_busy(NULL));

	car_fixture();
	fake_car_mode = "off"; // the invitation alone
	fake_devs[0].connected = true;
	refresh_devices();
	a2dp_profile_busy = true;

	invite_sinks(); // first sighting
	sink_invites[0].linked_since = now_ms() - (SINK_INVITE_GRACE_MS + 1000);
	invite_sinks();
	assert(a2dp_profile_requests == 1);
	// Not in four seconds: after bluez has had its thirty.
	uint32_t wait = sink_invites[0].retry_at - now_ms();
	assert(wait > 30000 && wait <= SINK_INVITE_BUSY_MS);
	assert(sink_invite_due == sink_invites[0].retry_at);
	// The ordinary retry moment passes and nothing is asked.
	sink_invites[0].retry_at -= SINK_INVITE_BUSY_MS - SINK_INVITE_RETRY_MS - 1000;
	invite_sinks();
	assert(a2dp_profile_requests == 1);

	// The stream bluez was making arrives: nothing more is asked at all.
	snprintf(fake_sink, sizeof(fake_sink), TESLA);
	invite_sinks();
	assert(a2dp_profile_requests == 1);
	assert(sink_invites[0].mac[0] == '\0');
	fake_sink[0] = '\0';

	// Nothing came of it: asked again once the wait is over, and still a
	// bounded number of times.
	invite_sinks();
	sink_invites[0].linked_since = now_ms() - (SINK_INVITE_GRACE_MS + 1000);
	a2dp_profile_requests = 0;
	for (int attempt = 0; attempt < SINK_INVITE_TRIES + 2; attempt++) {
		sink_invites[0].retry_at = now_ms() - 1;
		invite_sinks();
	}
	assert(a2dp_profile_requests == SINK_INVITE_TRIES);

	// A refusal that is not "busy" keeps its short retry.
	fake_devs[0].connected = false;
	refresh_devices();
	invite_sinks();
	fake_devs[0].connected = true;
	refresh_devices();
	a2dp_profile_busy = false;
	a2dp_profile_refused = true;
	invite_sinks();
	sink_invites[0].linked_since = now_ms() - (SINK_INVITE_GRACE_MS + 1000);
	invite_sinks();
	wait = sink_invites[0].retry_at - now_ms();
	assert(wait > 0 && wait <= SINK_INVITE_RETRY_MS);

	car_fixture_done();
}

// The same morning, 08:25:48: the earbuds were the last device, a car made its
// link during the wait for them to come back, and the list the wait consulted
// was the one read before it began. The page for the earbuds went out anyway
// and the car's link was gone five seconds later.
static void test_reconnect_grace_sees_a_new_link(void) {
	car_fixture();
	connect_calls = 0;

	// Nobody linked: the grace runs out and the reconnect goes on.
	assert(reconnect_grace(BUDS, 500));

	// The car links a moment into the grace. The cached list does not know.
	fake_devs[0].connected = true;
	bool cached = false;
	pthread_mutex_lock(&lock);
	for (int i = 0; i < g_paired_count; i++) {
		cached = cached || g_paired[i].connected;
	}
	pthread_mutex_unlock(&lock);
	assert(!cached);
	// Within a second it is read again, and the reconnect is off.
	assert(!reconnect_grace(BUDS, 1500));
	assert(connect_calls == 0);

	// The earbuds themselves linking is not a reason to stop.
	fake_devs[0].connected = false;
	fake_devs[1].connected = true;
	refresh_devices();
	assert(reconnect_grace(BUDS, 1250));

	// And coming back with audio ends the wait at once.
	snprintf(fake_sink, sizeof(fake_sink), BUDS);
	assert(!reconnect_grace(BUDS, 8000));

	car_fixture_done();
}

// Car mode: which devices, when the daemon is restarted, and when not.
static void test_car_mode(void) {
	// The class. 0x240408 is both cars of the report.
	assert(cod_is_car(0x240408)); // Audio/Video, hands-free
	assert(cod_is_car(0x200420)); // Audio/Video, car audio
	assert(cod_is_car(0x260408));
	assert(!cod_is_car(0x240404)); // wearable headset
	assert(!cod_is_car(0x240418)); // headphones
	assert(!cod_is_car(0x240414)); // loudspeaker
	assert(!cod_is_car(0x5a020c)); // a phone
	assert(!cod_is_car(0x5a0208)); // the same minor bits under another major class
	assert(!cod_is_car(0));

	// A daemon in car mode is told from its command line, so one left over
	// from another run is not taken for the ordinary one, or the reverse.
	bluealsa_profiles_t found = {0};
	const char plain[] = "bluealsa\0-p\0a2dp-source\0--a2dp-volume\0--keep-alive=0\0-c\0aptX\0-c\0LDAC";
	bluealsa_cmdline_scan(plain, sizeof(plain), &found);
	assert(found.has_source && !found.has_sink && !found.car);
	const char car[] = "bluealsa\0-p\0a2dp-source\0--a2dp-volume\0--keep-alive=0\0--a2dp-force-audio-cd\0-c\0-AAC";
	memset(&found, 0, sizeof(found));
	bluealsa_cmdline_scan(car, sizeof(car), &found);
	assert(found.has_source && found.car);
	const char car_long[] = "bluealsa\0-p\0a2dp-source\0--codec=-AAC";
	memset(&found, 0, sizeof(found));
	bluealsa_cmdline_scan(car_long, sizeof(car_long), &found);
	assert(found.car);

	// The stages: SBC alone by default, AAC kept on request, and never while
	// this device is the receiver or the mode is off.
	car_fixture();
	assert(car_stage() == CAR_STAGE_NONE); // auto, and no car
	snprintf(g_car_mac, sizeof(g_car_mac), TESLA);
	assert(car_stage() == CAR_STAGE_SBC);
	fake_car_codec = "aac";
	assert(car_stage() == CAR_STAGE_CD);
	fake_car_codec = NULL;
	g_car_stage_floor = CAR_STAGE_CD; // the daemon refused "-AAC"
	assert(car_stage() == CAR_STAGE_CD);
	g_car_stage_floor = CAR_STAGE_NONE; // and everything else
	assert(car_stage() == CAR_STAGE_NONE);
	g_car_stage_floor = CAR_STAGE_SBC;
	g_receiver_profile = true;
	assert(car_stage() == CAR_STAGE_NONE);
	g_receiver_profile = false;
	fake_car_mode = "off";
	assert(car_stage() == CAR_STAGE_NONE);
	g_car_mac[0] = '\0';
	fake_car_mode = "on"; // every device, car or not
	assert(car_stage() == CAR_STAGE_SBC);
	fake_car_mode = NULL;
	const char *options[3];
	int count = car_options(CAR_STAGE_SBC, options);
	assert(count >= 1 && strstr(options[count - 1], "-AAC") != NULL);
	assert(car_options(CAR_STAGE_NONE, options) == 0);

	// The car links by itself, as both did. Nothing happens at the first
	// sighting: the car opens its channel a quarter of a second after the
	// link and bluealsa has to be there to be found.
	fake_devs[0].connected = true;
	refresh_devices();
	a2dp_profile_busy = true; // bluez is waiting for the car, as in the log
	invite_sinks();
	assert(car_restarts == 0 && g_car_mac[0] == '\0');

	// At the end of the grace the daemon is restarted for it, once, with
	// nothing to resume, and before anything is asked of bluez.
	sink_invites[0].linked_since = now_ms() - (SINK_INVITE_GRACE_MS + 1000);
	invite_sinks();
	assert(car_restarts == 1);
	assert(strcmp(car_restart_for, TESLA) == 0);
	assert(strcmp(car_restart_why, "car mode on") == 0);
	assert(!car_restart_resumed);
	assert(a2dp_profile_requests == 0);
	assert(sink_invite_due != 0);

	// The invitation follows once the new endpoints are there; "busy" then
	// leaves the stream to bluez, and the daemon is not touched again.
	sink_invites[0].retry_at = now_ms() - 1;
	invite_sinks();
	assert(a2dp_profile_requests == 1);
	assert(car_restarts == 1);

	// The stream comes up. Car mode stays for as long as the car does.
	snprintf(fake_sink, sizeof(fake_sink), TESLA);
	invite_sinks();
	assert(car_restarts == 1 && strcmp(g_car_mac, TESLA) == 0);

	// The link drops and comes back, which the Tesla does: no restart.
	fake_sink[0] = '\0';
	fake_devs[0].connected = false;
	refresh_devices();
	invite_sinks();
	assert(car_restarts == 1 && g_car_gone_since != 0);
	fake_devs[0].connected = true;
	refresh_devices();
	invite_sinks();
	assert(car_restarts == 1 && g_car_gone_since == 0);
	sink_invites[0].linked_since = now_ms() - (SINK_INVITE_GRACE_MS + 1000);
	invite_sinks();
	assert(car_restarts == 1); // already in car mode: only the invitation

	// The earbuds link while the car is still there: the mode stays.
	fake_devs[1].connected = true;
	refresh_devices();
	invite_sinks();
	sink_invites[1].linked_since = now_ms() - (SINK_INVITE_GRACE_MS + 1000);
	invite_sinks();
	assert(car_restarts == 1 && strcmp(g_car_mac, TESLA) == 0);
	fake_devs[1].connected = false;

	// The car leaves for good. Not at once ...
	fake_devs[0].connected = false;
	refresh_devices();
	invite_sinks();
	assert(car_restarts == 1 && strcmp(g_car_mac, TESLA) == 0);
	invite_sinks(); // nor at the next look, a moment later
	assert(car_restarts == 1 && strcmp(g_car_mac, TESLA) == 0);
	// ... but after the linger the ordinary daemon is back, and whatever was
	// playing through the old one is asked for its stream again.
	g_car_gone_since = now_ms() - (CAR_MODE_LINGER_MS + 1000);
	invite_sinks();
	assert(car_restarts == 2);
	assert(car_restart_for[0] == '\0' && g_car_mac[0] == '\0');
	assert(car_restart_resumed);
	invite_sinks();
	assert(car_restarts == 2);

	// Earbuds that link while the mode lingers do not wait for it: they get
	// the full list before they are asked.
	car_fixture();
	snprintf(g_car_mac, sizeof(g_car_mac), TESLA);
	fake_devs[1].connected = true;
	refresh_devices();
	invite_sinks();
	assert(car_restarts == 0);
	sink_invites[0].linked_since = now_ms() - (SINK_INVITE_GRACE_MS + 1000);
	invite_sinks();
	assert(car_restarts == 1 && g_car_mac[0] == '\0');
	assert(a2dp_profile_requests == 0);
	sink_invites[0].retry_at = now_ms() - 1;
	invite_sinks();
	assert(a2dp_profile_requests == 1);
	assert(strcmp(a2dp_profile_requested_for, BUDS) == 0);

	// Earbuds alone never cause a restart.
	car_fixture();
	fake_devs[1].connected = true;
	refresh_devices();
	invite_sinks();
	sink_invites[0].linked_since = now_ms() - (SINK_INVITE_GRACE_MS + 1000);
	invite_sinks();
	assert(car_restarts == 0 && a2dp_profile_requests == 1);

	// Off is off, and "on" has nothing to switch: the daemon is in car mode
	// from the start, for everything.
	for (int pass = 0; pass < 2; pass++) {
		car_fixture();
		fake_car_mode = pass == 0 ? "off" : "on";
		fake_devs[0].connected = true;
		refresh_devices();
		invite_sinks();
		sink_invites[0].linked_since = now_ms() - (SINK_INVITE_GRACE_MS + 1000);
		invite_sinks();
		assert(car_restarts == 0 && g_car_mac[0] == '\0');
		assert(a2dp_profile_requests == 1);
	}

	// A daemon that takes none of the options: the car is invited like
	// anything else, without a restart that would change nothing.
	car_fixture();
	g_car_stage_floor = CAR_STAGE_NONE;
	fake_devs[0].connected = true;
	refresh_devices();
	invite_sinks();
	sink_invites[0].linked_since = now_ms() - (SINK_INVITE_GRACE_MS + 1000);
	invite_sinks();
	assert(car_restarts == 0 && a2dp_profile_requests == 1);

	// No codec is asked for by the player in car mode: a new configuration on
	// a stream that exists is what cost the report's car its link, twice.
	car_fixture();
	fake_offers_aac = true;
	select_codec_calls = 0;
	codec_auto_done[0] = '\0';
	do_auto_codec(BUDS);
	assert(select_codec_calls == 1); // headphones: AAC is asked for over SBC
	codec_auto_done[0] = '\0';
	snprintf(g_car_mac, sizeof(g_car_mac), TESLA);
	fake_car_codec = "aac"; // even with AAC left on offer
	do_auto_codec(TESLA);
	assert(select_codec_calls == 1);
	fake_offers_aac = false;
	codec_auto_done[0] = '\0';

	// A connection made from here: the mode is right before the page goes
	// out, in both directions, and a second connect to the same kind of
	// device restarts nothing.
	car_fixture();
	car_mode_before_connect(TESLA);
	assert(car_restarts == 1 && strcmp(car_restart_for, TESLA) == 0 && !car_restart_resumed);
	car_mode_before_connect(TESLA);
	assert(car_restarts == 1);
	car_mode_before_connect(BUDS);
	assert(car_restarts == 2 && car_restart_for[0] == '\0' && g_car_mac[0] == '\0');
	car_mode_before_connect(BUDS);
	assert(car_restarts == 2);
	car_mode_before_connect("00:11:22:33:44:55"); // not a device bluez knows
	assert(car_restarts == 2);

	car_fixture_done();
}

// The sender is linked, says it is playing case after case, and delivers
// nothing: the receiver asks it for a stream. Posting that is all this end can
// do from the capture thread, and asking twice within one report of silence
// must not put two invitations in the queue.
static void test_receiver_stream_invite(void) {
	pthread_mutex_lock(&lock);
	queue_head = 0;
	queue_count = 0;
	pthread_mutex_unlock(&lock);

	source_pcm = true; // the sender's PCM object is still published
	bluetooth_receiver_ask_to_stream();
	bluetooth_receiver_ask_to_stream();

	pthread_mutex_lock(&lock);
	int queued = queue_count;
	job_type_t type = queued ? queue[queue_head].type : JOB_NONE;
	char asked[BT_MAC_MAX] = "";
	if (queued) {
		snprintf(asked, sizeof(asked), "%s", queue[queue_head].text);
	}
	queue_count = 0;
	pthread_mutex_unlock(&lock);

	assert(queued == 1);
	assert(type == JOB_RX_INVITE);
	assert(strcmp(asked, "20:04:84:3A:6C:FE") == 0);

	// No published PCM means no sender to ask: nothing is queued.
	source_pcm = false;
	bluetooth_receiver_ask_to_stream();
	pthread_mutex_lock(&lock);
	assert(queue_count == 0);
	pthread_mutex_unlock(&lock);
}

static void test_op_result_is_reported_once(void) {
	/* A page asks "what came of the last action?" until it gets an answer.
	 * Every terminal answer has to be handed out exactly once: an answer left
	 * set is indistinguishable from the action still in flight, and the page
	 * acts on it again -- which is what happened to NEEDS_RECEIVER. It stayed
	 * armed after the receiver was opened for it, so the Bluetooth page's first
	 * poll on the way back out opened receiver mode a second time, and RX mode
	 * could not be left at all (issue #245). */
	bt_op_t answers[] = {BT_OP_OK, BT_OP_PAIRED, BT_OP_NEEDS_RECEIVER, BT_OP_FAILED};
	for (size_t i = 0; i < sizeof(answers) / sizeof(answers[0]); i++) {
		set_op(answers[i], "Some Device");
		char name[BT_NAME_MAX] = "";
		assert(bluetooth_take_op_result(name, sizeof(name)) == answers[i]);
		assert(strcmp(name, "Some Device") == 0);
		/* Spent: the next look finds nothing to act on. */
		assert(bluetooth_take_op_result(NULL, 0) == BT_OP_IDLE);
	}

	/* BUSY is not an answer but a marker for work still running, so it stays
	 * until the job posts its real result. */
	set_op(BT_OP_BUSY, NULL);
	assert(bluetooth_take_op_result(NULL, 0) == BT_OP_BUSY);
	assert(bluetooth_take_op_result(NULL, 0) == BT_OP_BUSY);
	set_op(BT_OP_OK, "Some Device");
	assert(bluetooth_take_op_result(NULL, 0) == BT_OP_OK);
	assert(bluetooth_take_op_result(NULL, 0) == BT_OP_IDLE);

	/* A pairing that defers to the receiver, acted on once, does not come
	 * back: the second look must not re-open RX mode. */
	set_op(BT_OP_NEEDS_RECEIVER, "Galaxy Z Fold");
	assert(bluetooth_take_op_result(NULL, 0) == BT_OP_NEEDS_RECEIVER);
	assert(bluetooth_take_op_result(NULL, 0) != BT_OP_NEEDS_RECEIVER);
}

static void test_scan_pairing_window(void) {
	assert(SCAN_SECONDS == 30);
	g_enabled = true;
	g_receiver_profile = false;
	g_discoverable = false;
	g_scanning = true;
	unsigned generation = ++g_scan_generation;
	queue_head = queue_count = 0;
	fake_visible = false;
	discovery_starts = discovery_stops = visibility_writes = 0;
	discovery_result = true;
	cancel_on_start = true;
	do_scan(generation);
	assert(discovery_starts == 1 && discovery_stops == 1);
	assert(!g_scanning && !fake_visible && visibility_writes == 2);

	// SonixLink owns visibility independently; a scan must not revoke it.
	g_discoverable = true;
	g_scanning = true;
	generation = ++g_scan_generation;
	discovery_starts = discovery_stops = visibility_writes = 0;
	do_scan(generation);
	assert(!g_scanning && fake_visible && discovery_stops == 1);

	// A queued pairing interrupts inquiry rather than waiting thirty seconds.
	g_scanning = true;
	generation = ++g_scan_generation;
	queue[0].type = JOB_PAIR;
	queue_count = 1;
	discovery_starts = discovery_stops = 0;
	do_scan(generation);
	assert(discovery_starts == 0 && discovery_stops == 1 && !g_scanning);
	queue_count = 0;

	// A stop from an older page cannot cancel the current request.
	g_scanning = true;
	generation = ++g_scan_generation;
	discovery_stops = visibility_writes = 0;
	do_scan_stop(generation - 1);
	assert(g_scanning && discovery_stops == 0 && visibility_writes == 0);
	do_scan_stop(generation);
	assert(!g_scanning && fake_visible && discovery_stops == 1);

	// All three start attempts failing must still close the pairing window.
	g_discoverable = false;
	g_scanning = true;
	generation = ++g_scan_generation;
	discovery_result = false;
	cancel_on_start = false;
	discovery_starts = discovery_stops = 0;
	do_scan(generation);
	assert(discovery_starts == SCAN_START_TRIES && discovery_stops == 1);
	assert(!g_scanning && !fake_visible);

	// A full queue must not leave the UI searching with no worker job to finish.
	worker_running = true;
	g_enabled = true;
	g_busy = false;
	queue_count = JOB_QUEUE_LEN;
	for (int i = 0; i < JOB_QUEUE_LEN; i++) queue[i].type = JOB_MEDIA;
	bluetooth_scan_start();
	assert(!g_scanning);
	queue_count = 0;
	worker_running = false;

	// RX visibility survives both a scan and a TX page clearing its own flag.
	g_receiver_profile = true;
	g_scanning = true;
	generation = ++g_scan_generation;
	discovery_result = true;
	cancel_on_start = true;
	do_scan(generation);
	assert(fake_visible && !g_scanning);
	g_discoverable = false;
	assert(effective_discoverable());
	g_receiver_profile = false;
	g_enabled = false;
	fake_visible = false;
}

static void test_rx_request_queue(void) {
	pthread_mutex_lock(&lock);
	queue_head = queue_count = 0;
	g_rx_requested = g_rx_answered = 0;
	pthread_mutex_unlock(&lock);
	unsigned first = bluetooth_refresh_receiver();
	unsigned second = bluetooth_refresh_receiver();
	assert(first && second != first && queue_count == 1);
	job_t job;
	pthread_mutex_lock(&lock);
	assert(take_job(&job) && job.type == JOB_RX_INFO);
	pthread_mutex_unlock(&lock);
	// An in-flight older response cannot satisfy the newer request.
	g_rx_answered = first;
	assert(bluetooth_receiver_answered() != second);
	unsigned third = bluetooth_refresh_receiver();
	assert(third != second && queue_count == 1);
	pthread_mutex_lock(&lock);
	queue_head = 0; queue_count = JOB_QUEUE_LEN;
	for (int i = 0; i < JOB_QUEUE_LEN; i++) queue[i].type = JOB_MEDIA;
	pthread_mutex_unlock(&lock);
	assert(bluetooth_refresh_receiver() == 0);
	pthread_mutex_lock(&lock);
	queue_count = 0;
	pthread_mutex_unlock(&lock);
}

/* A route on the USB-C port is not apply_output_routing()'s to give away.
 *
 * This is the V1 report's "stops after about ninety seconds": the screen blanks
 * thirty seconds after the last touch, Bluetooth is parked a minute after that,
 * and the teardown calls this with nothing to play over the air. Handing the
 * player NULL puts the next PCM open back on the CS43198 -- the jack -- while
 * the dongle is still on the cable and usbaudio.c still has it routed. The
 * sound comes back out of the player instead of the headphones.
 */
static void test_usb_route_survives_a_radio_teardown(void) {
	pthread_mutex_lock(&lock);
	g_enabled = true;
	g_a2dp_connected = false; /* no sink: the teardown's own state */
	g_a2dp_mac[0] = '\0';
	pthread_mutex_unlock(&lock);
	source_pcm = false;

	/* A card on the port stays exactly where it is. */
	snprintf(routed_to, sizeof(routed_to), "plughw:3,0");
	apply_output_routing();
	assert(strcmp(routed_to, "plughw:3,0") == 0);

	/* hw: names the same device without the plug. */
	snprintf(routed_to, sizeof(routed_to), "hw:3,0");
	apply_output_routing();
	assert(strcmp(routed_to, "hw:3,0") == 0);

	/* The jack is this call's to take, and the teardown still takes it: with
	 * no sink the route is "default", which is the CS43198 and the right answer
	 * when the sound was coming out of it anyway. */
	snprintf(routed_to, sizeof(routed_to), "default");
	apply_output_routing();
	assert(strcmp(routed_to, "default") == 0);

	/* And a bluealsa route is cleared, because the daemon is going away under
	 * it: the sound must not stay pointed at a PCM that is about to be
	 * killed. This is the case the reset exists for. */
	snprintf(routed_to, sizeof(routed_to), "bluealsa:DEV=20:04:84:3A:6C:FE,PROFILE=a2dp");
	apply_output_routing();
	assert(strcmp(routed_to, "default") == 0);

	/* A sink being connected still wins over the USB card: that is a
	 * deliberate choice by the listener, made after the cable went in. */
	pthread_mutex_lock(&lock);
	g_enabled = true;
	g_a2dp_connected = true;
	snprintf(g_a2dp_mac, sizeof(g_a2dp_mac), "20:04:84:3A:6C:FE");
	pthread_mutex_unlock(&lock);
	snprintf(routed_to, sizeof(routed_to), "plughw:3,0");
	apply_output_routing();
	assert(strcmp(routed_to, "bluealsa:DEV=20:04:84:3A:6C:FE,PROFILE=a2dp") == 0);

	pthread_mutex_lock(&lock);
	g_a2dp_connected = false;
	g_a2dp_mac[0] = '\0';
	pthread_mutex_unlock(&lock);
	snprintf(routed_to, sizeof(routed_to), "default");
}

/* Spawned Bluetooth daemons outlive card unmounts during USB mass-storage
 * export. Verify that neither an on-card stdout/stderr log nor an open track
 * descriptor nor an on-card working directory is inherited by the daemon. */
static void test_spawn_daemon_releases_card_descriptors(void) {
	char root[] = "/tmp/sonix-bt-daemon-XXXXXX";
	assert(mkdtemp(root));

	char card_dir[256], log_file[512], track_file[512];
	char report_file[512], script_file[512], orig_cwd[PATH_MAX];
	assert(getcwd(orig_cwd, sizeof(orig_cwd)) != NULL);
	snprintf(card_dir, sizeof(card_dir), "%s/mnt_sd_0", root);
	snprintf(log_file, sizeof(log_file), "%s/sonix_player.log", card_dir);
	snprintf(track_file, sizeof(track_file), "%s/track.flac", card_dir);
	snprintf(report_file, sizeof(report_file), "%s/report.txt", root);
	snprintf(script_file, sizeof(script_file), "%s/fake_daemon.sh", root);
	assert(mkdir(card_dir, 0700) == 0);

	FILE *script = fopen(script_file, "w");
	assert(script != NULL);
	fprintf(script,
			"#!/bin/sh\n"
			"me=$$\n"
			"info=$(\n"
			"  printf 'cwd=%%s\\n' \"$(pwd)\"\n"
			"  for fd in /proc/$me/fd/*; do\n"
			"    n=$(basename \"$fd\")\n"
			"    target=$(readlink \"$fd\" 2>/dev/null || true)\n"
			"    printf 'fd_%%s=%%s\\n' \"$n\" \"$target\"\n"
			"  done\n"
			")\n"
			"printf '%%s\\n' \"$info\" > '%s.tmp'\n"
			"mv '%s.tmp' '%s'\n",
			report_file, report_file, report_file);
	assert(fclose(script) == 0);
	assert(chmod(script_file, 0755) == 0);

	FILE *sd_log = fopen(log_file, "a+");
	assert(sd_log != NULL);
	FILE *track = fopen(track_file, "w+"); // deliberately without O_CLOEXEC
	assert(track != NULL);

	int saved_out = dup(STDOUT_FILENO);
	int saved_err = dup(STDERR_FILENO);
	assert(saved_out >= 0 && saved_err >= 0);
	assert(fcntl(saved_out, F_SETFD, FD_CLOEXEC) == 0);
	assert(fcntl(saved_err, F_SETFD, FD_CLOEXEC) == 0);

	// Mirror logging_attach_sd(): descriptors 1 and 2 point straight at the card.
	assert(dup2(fileno(sd_log), STDOUT_FILENO) >= 0);
	assert(dup2(fileno(sd_log), STDERR_FILENO) >= 0);
	assert(chdir(card_dir) == 0);

	char *argv[] = {script_file, NULL};
	bool ok = spawn_daemon(script_file, argv);

	assert(chdir(orig_cwd) == 0);
	assert(dup2(saved_out, STDOUT_FILENO) >= 0);
	assert(dup2(saved_err, STDERR_FILENO) >= 0);
	close(saved_out);
	close(saved_err);
	fclose(track);
	fclose(sd_log);
	assert(ok);

	for (int waited = 0; waited < 2000 && access(report_file, F_OK) != 0; waited += 20) {
		usleep(20 * 1000);
	}

	char report[2048];
	assert(slurp(report_file, report, sizeof(report)));
	assert(strstr(report, "cwd=/\n") != NULL);
	assert(strstr(report, "fd_0=/dev/null\n") != NULL);
	assert(strstr(report, "fd_1=/dev/null\n") != NULL);
	assert(strstr(report, "fd_2=/dev/null\n") != NULL);
	assert(strstr(report, card_dir) == NULL);

	unlink(report_file);
	unlink(script_file);
	unlink(track_file);
	unlink(log_file);
	rmdir(card_dir);
	rmdir(root);
}

int main(void) {
	car_restart = fake_car_restart; // never the real one here, see the stubs at the top
	test_rx_request_queue();
	test_a2dp_directions();
	test_startup_waits();
	test_process_matching();
	test_alsa_template();
	test_rfkill_lookup();
	test_boot_cleanup_queue();
	test_rx_discoverability_and_phone_pairing();
	test_op_result_is_reported_once();
	test_linked_sink_is_invited();
	test_busy_invitation_is_left_to_bluez();
	test_reconnect_grace_sees_a_new_link();
	test_car_mode();
	test_receiver_stream_invite();
	test_scan_pairing_window();
	test_usb_route_survives_a_radio_teardown();
	test_spawn_daemon_releases_card_descriptors();
	printf("PASS bluetooth (A2DP directions, RX profile/pairing, sink profile invitation, scan visibility, startup, ALSA template, radio switch, boot cleanup and daemon fd isolation)\n");
	return 0;
}
