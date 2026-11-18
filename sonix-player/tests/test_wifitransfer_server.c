// Drive the production session and HTTP server on an ephemeral loopback port.
// Only the V1's radio, mount lock, and mDNS are substituted. The Python test
// sends requests and checks the actual files in the temporary card directory.
#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>

#include "src/system/core/lang.h"
#include "src/system/device/system.h"
#include "src/system/net/mdns.h"
#include "src/system/net/wifi.h"
#include "src/system/net/wifitransfer.h"

static const char *card;
static bool wifi_on = true, card_mounted = true, card_lock_held, mdns_live;

void wifi_get_status(wifi_status_t *out) { out->state = wifi_on ? WIFI_STATE_CONNECTED : WIFI_STATE_OFF; }
bool wifi_interface_address(char *out, size_t size) {
	if (!wifi_on) return false;
	snprintf(out, size, "127.0.0.1");
	return true;
}
int storage_card_file_job_begin(bool writable) {
	assert(!writable);
	assert(!card_lock_held);
	if (!card_mounted) return -1;
	int fd = open(card, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	card_lock_held = fd >= 0;
	return fd;
}
void storage_card_file_job_end(void) {
	assert(card_lock_held);
	card_lock_held = false;
}
bool mdns_start(const char *name, const char *ip) {
	assert(card_lock_held && !mdns_live && !strcmp(name, "sonix-transfer") && !strcmp(ip, "127.0.0.1"));
	mdns_live = true;
	return true;
}
void mdns_stop(void) { mdns_live = false; }
const char *tr(const char *key) {
	if (!strcmp(key, "Trasferimento Wi-Fi")) return "Wi-Fi transfer";
	if (!strcmp(key, "Codice non valido o scaduto.")) return "Test's code \"bad\": </script>\\end";
	return key;
}

int main(int argc, char **argv) {
	assert(argc == 2);
	card = argv[1];
	setvbuf(stdout, NULL, _IOLBF, 0);
	assert(wifitransfer_available() && !wifitransfer_get_enabled());
	char cmd[40];
	while (fgets(cmd, sizeof(cmd), stdin)) {
		if (!strcmp(cmd, "start\n")) {
			assert(wifitransfer_start(0x12b3aa) == WIFITRANSFER_OK);
			wifitransfer_info_t info;
			assert(wifitransfer_get_enabled() && wifitransfer_running() && wifitransfer_get_info(&info));
			printf("READY %s %s %s\n", info.address, info.name, info.code);
		} else if (!strcmp(cmd, "stop\n")) {
			wifitransfer_release_card();
			wifitransfer_release_card();
			wifitransfer_info_t info;
			assert(!wifitransfer_get_info(&info) && !wifitransfer_get_enabled() && !wifitransfer_running() && !mdns_live);
			puts("STOPPED");
		} else if (!strcmp(cmd, "completion\n")) {
			printf("COMPLETION %d\n", wifitransfer_take_upload_complete());
		} else if (!strcmp(cmd, "no-card\n")) {
			card_mounted = false;
			assert(wifitransfer_start(0) == WIFITRANSFER_NO_CARD && !wifitransfer_get_enabled());
			card_mounted = true;
			puts("NO CARD");
		} else if (!strcmp(cmd, "no-wifi\n")) {
			wifi_on = false;
			assert(wifitransfer_start(0) == WIFITRANSFER_NO_WIFI && !wifitransfer_get_enabled());
			wifi_on = true;
			puts("NO WIFI");
		} else {
			break;
		}
	}
	wifitransfer_release_card();
	return 0;
}
