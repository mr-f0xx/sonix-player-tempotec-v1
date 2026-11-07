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

const char *config_get(const char *section, const char *key, const char *fallback) {
	(void)section;
	(void)key;
	return fallback;
}

void config_set(const char *section, const char *key, const char *value) {
	(void)section;
	(void)key;
	(void)value;
}

bool config_save(void) { return true; }

void audio_set_output_device(const char *pcm) { (void)pcm; }

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

bool btstack_connect_a2dp(const char *address, int timeout_ms) {
	(void)address;
	(void)timeout_ms;
	return true;
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
		address_out[0] = '\0';
	}
	return false;
}

int btstack_codecs(const char *address, char out[][BT_CODEC_NAME_MAX], int max, char *selected, size_t selected_size) {
	(void)address;
	(void)out;
	(void)max;
	if (selected && selected_size) {
		selected[0] = '\0';
	}
	return 0;
}

bool btstack_select_codec(const char *address, const char *codec) {
	(void)address;
	(void)codec;
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

int main(void) {
	test_rx_request_queue();
	test_a2dp_directions();
	test_startup_waits();
	test_process_matching();
	test_alsa_template();
	test_rfkill_lookup();
	test_boot_cleanup_queue();
	test_rx_discoverability_and_phone_pairing();
	test_op_result_is_reported_once();
	test_scan_pairing_window();
	printf("PASS bluetooth (A2DP directions, RX profile/pairing, scan visibility, startup, ALSA template, radio switch and boot cleanup)\n");
	return 0;
}
