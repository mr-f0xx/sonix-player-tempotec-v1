#define _GNU_SOURCE
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static char fake_typec[256], fake_partner[256], fake_tcs[256], fake_id[256], fake_bus[256];
static bool fake_vbus, fake_probe;
static time_t fake_now = 100;
static time_t fake_time(time_t *out) { if (out) *out = fake_now; return fake_now; }

#define TYPEC_PORT_TYPE fake_typec
#define TYPEC_PARTNER fake_partner
#define TCS1421_CFG fake_tcs
#define DWC2_OTG_ID fake_id
#define USB_AUDIO_BUS fake_bus
#define time fake_time
#include "../src/system/audio/usbaudio.c"
#undef time

bool usb_vbus_present(void) { return fake_vbus; }
bool usbhostprobe_active(void) { return fake_probe; }
void usbhostprobe_recover(void) {}

static void pathcat(char *out, size_t size, const char *a, const char *b) {
	assert(strlen(a) + strlen(b) + 1 <= size); strcpy(out, a); strcat(out, b);
}
static void put(const char *path, const char *text) {
	FILE *f = fopen(path, "w"); assert(f && fputs(text, f) >= 0 && fclose(f) == 0);
}
static void expect(const char *path, const char *text) {
	char buf[256]; FILE *f = fopen(path, "r"); assert(f && fgets(buf, sizeof(buf), f)); fclose(f);
	assert(strcmp(buf, text) == 0);
}

int main(void) {
	char root[] = "/tmp/sonix-usbaudio-XXXXXX"; assert(mkdtemp(root));
	pathcat(fake_typec, sizeof(fake_typec), root, "/typec-missing");
	pathcat(fake_partner, sizeof(fake_partner), root, "/partner-missing");
	pathcat(fake_tcs, sizeof(fake_tcs), root, "/tcs");
	pathcat(fake_id, sizeof(fake_id), root, "/id-missing");
	pathcat(fake_bus, sizeof(fake_bus), root, "/bus");
	assert(mkdir(fake_bus, 0700) == 0);
	put(fake_tcs, "Sink\n");

	// The reported log state: VBUS with no readable ID and no device is
	// ambiguous. Neither startup nor later polls force a role.
	fake_vbus = true;
	assert(!usbaudio_init());
	expect(fake_tcs, "Sink\n");
	for (int i = 0; i < 15; i++) { arbitrate_port(false); fake_now++; }
	expect(fake_tcs, "Sink\n");
	assert(!usbaudio_handover_pending());

	// With no VBUS and no partner, restoring the default dual mode is allowed.
	fake_vbus = false;
	assert(usbaudio_init());
	expect(fake_tcs, "NormalDRP\n");

	// A working card is protected even when playback is routed elsewhere;
	// importantly, it must not be reset by reasserting NormalDRP.
	put(fake_tcs, "Source\n");
	arbitrate_port(true);
	assert(usbaudio_handover_pending());
	expect(fake_tcs, "Source\n");

	// During the explicit trial normal arbitration does not touch either role.
	fake_probe = true;
	for (int i = 0; i < 20; i++) { arbitrate_port(false); fake_now++; }
	assert(usbaudio_handover_pending());
	expect(fake_tcs, "Source\n");
	assert(!usbaudio_ensure_dual());
	fake_probe = false;

	// An enumerated audio interface holds the host-side handover even before
	// ALSA exposes a card, including on kernels with no readable ID line.
	char device[256], iface[256], cls[256];
	pathcat(device, sizeof(device), fake_bus, "/1-1");
	pathcat(iface, sizeof(iface), fake_bus, "/1-1:1.0");
	pathcat(cls, sizeof(cls), iface, "/bInterfaceClass");
	assert(mkdir(device, 0700) == 0 && mkdir(iface, 0700) == 0);
	put(cls, "01\n"); put(fake_tcs, "NormalDRP\n");
	for (int i = 0; i < 20; i++) { arbitrate_port(false); fake_now++; }
	assert(usbaudio_handover_pending());
	expect(fake_tcs, "NormalDRP\n");

	// A non-audio host-bus device is no longer mistaken for an empty port.
	put(cls, "03\n");
	assert(partner_present() && port_is_host_side());
	arbitrate_port(false);
	assert(usbaudio_handover_pending());

	assert(unlink(cls) == 0 && rmdir(iface) == 0 && rmdir(device) == 0);
	assert(unlink(fake_tcs) == 0 && rmdir(fake_bus) == 0 && rmdir(root) == 0);
	puts("PASS USB audio arbitration (unknown ID, VBUS, active card and host trial)");
	return 0;
}
