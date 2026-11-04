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

bool config_save(void) { return true; }

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

int main(void) {
	test_a2dp_directions();
	test_startup_waits();
	test_process_matching();
	test_alsa_template();
	test_boot_cleanup_queue();
	printf("PASS bluetooth (A2DP directions, startup, ALSA template and boot cleanup)\n");
	return 0;
}
