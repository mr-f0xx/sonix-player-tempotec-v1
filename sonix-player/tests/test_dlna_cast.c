// Drives the production cast path over real sockets. The Python side is the
// renderer -- an SSDP responder, a device description, AVTransport and
// ConnectionManager -- and then the television, fetching what it was told to
// play. Only Wi-Fi's interface address is substituted; the media server, the
// DIDL and the HTTP responses are the ones the device ships.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "src/system/net/wifi.h"
#include "src/system/remote/dlna_cast.h"

bool wifi_interface_address(char *out, size_t size) {
	if (!out || size < 10) return false;
	snprintf(out, size, "127.0.0.1");
	return true;
}

// The cast code reads the status from the same place the page does, so nothing
// else of Wi-Fi is needed here.
void wifi_get_status(wifi_status_t *out) {
	if (!out) return;
	memset(out, 0, sizeof(*out));
	out->state = WIFI_STATE_CONNECTED;
	snprintf(out->ip, sizeof(out->ip), "127.0.0.1");
}

int main(void) {
	setvbuf(stdout, NULL, _IOLBF, 0);
	char line[1200];
	while (fgets(line, sizeof(line), stdin)) {
		line[strcspn(line, "\r\n")] = '\0';
		if (strcmp(line, "scan") == 0) {
			dlna_cast_scan();
			puts("SCAN");
		} else if (strcmp(line, "scanning") == 0) {
			printf("SCANNING %d\n", dlna_cast_scan_running() ? 1 : 0);
		} else if (strcmp(line, "list") == 0) {
			dlna_cast_target_t targets[DLNA_CAST_MAX_TARGETS];
			int count = dlna_cast_targets(targets, DLNA_CAST_MAX_TARGETS);
			printf("TARGETS %d %d\n", count, dlna_cast_selected());
			for (int i = 0; i < count; i++) printf("TARGET %d %s\n", i, targets[i].name);
		} else if (strncmp(line, "select ", 7) == 0) {
			int index = atoi(line + 7);
			dlna_cast_select(index);
			printf("SELECT %d %d\n", index, dlna_cast_selected());
		} else if (strncmp(line, "cast ", 5) == 0) {
			printf("CAST %d\n", dlna_cast_play(line + 5) ? 1 : 0);
		} else if (strcmp(line, "stop") == 0) {
			dlna_cast_stop();
			puts("STOP");
		} else if (strcmp(line, "status") == 0) {
			char status[200];
			dlna_cast_status(status, sizeof(status));
			printf("STATUS %s\n", status);
		} else if (strcmp(line, "quit") == 0) {
			break;
		} else {
			puts("UNKNOWN");
		}
	}
	return 0;
}

// http.c and tls.c take their strings through tr(); the strings themselves are
// not what this test is about.
const char *tr(const char *key) { return key; }
