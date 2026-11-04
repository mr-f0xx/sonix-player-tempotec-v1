#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static char test_udc_class[256], test_storage_udc[256];
static bool fake_adb_wanted, fake_adb_running, fake_handover;
static bool reflect_storage_binding, fail_storage_write, partial_storage_unbind;

static FILE *fake_fopen(const char *path, const char *mode) {
	if (fail_storage_write && strcmp(path, test_storage_udc) == 0 && strcmp(mode, "w") == 0) {
		if (partial_storage_unbind) {
			FILE *real = fopen(path, mode);
			assert(real && fputs("\n", real) >= 0 && fclose(real) == 0);
			return fopen("/dev/full", "w"); // close reports failure after effect
		}
		errno = EACCES;
		return NULL;
	}
	return fopen(path, mode);
}

#define USB_UDC_CLASS test_udc_class
#define STORAGE_UDC_PATH test_storage_udc
#define fopen fake_fopen
#include "../src/system/device/usb.c"
#undef fopen

bool adb_switched_on(void) { return fake_adb_wanted; }
bool adb_is_running(void) { return fake_adb_running; }
bool usbaudio_handover_pending(void) { return fake_handover; }

char *read_file_content(const char *path) {
	// On a real kernel, uevent reflects the gadget driver immediately after a
	// configfs UDC write. Reproduce that for the owned-gadget tests.
	if (reflect_storage_binding && strstr(path, "/uevent")) {
		FILE *f = fopen(test_storage_udc, "r");
		char value[64] = "";
		if (f) { (void)fgets(value, sizeof(value), f); fclose(f); }
		return strdup(value[0] && value[0] != '\n' ?
			"USB_UDC_NAME=dwc2\nUSB_UDC_DRIVER=android0\n" : "USB_UDC_NAME=dwc2\n");
	}
	FILE *f = fopen(path, "r");
	if (!f) return NULL;
	char buf[4096];
	size_t n = fread(buf, 1, sizeof(buf) - 1, f);
	fclose(f);
	buf[n] = '\0';
	return strdup(buf);
}

static void pathcat(char *out, size_t size, const char *a, const char *b) {
	assert(strlen(a) + strlen(b) + 1 <= size);
	strcpy(out, a);
	strcat(out, b);
}
static void put(const char *path, const char *text) {
	FILE *f = fopen(path, "w");
	assert(f && fputs(text, f) >= 0 && fclose(f) == 0);
}
static void expect(const char *path, const char *text) {
	char *value = read_file_content(path);
	assert(value && strcmp(value, text) == 0);
	free(value);
}

int main(void) {
	char root[] = "/tmp/sonix-usbowner-XXXXXX";
	assert(mkdtemp(root));
	pathcat(test_udc_class, sizeof(test_udc_class), root, "/udcs");
	pathcat(test_storage_udc, sizeof(test_storage_udc), root, "/storage-UDC");
	assert(!usb_external_host_reserve()); // missing UDC is unknown, not disconnected
	assert(mkdir(test_udc_class, 0700) == 0);
	assert(!usb_external_host_reserve()); // no UDC

	char udc[256], state[256], event[256];
	pathcat(udc, sizeof(udc), test_udc_class, "/udc0");
	pathcat(state, sizeof(state), udc, "/state");
	pathcat(event, sizeof(event), udc, "/uevent");
	assert(mkdir(udc, 0700) == 0);
	put(event, "USB_UDC_NAME=dwc2\n");
	put(test_storage_udc, "\n");
	put(state, "configured\n");
	assert(!usb_external_host_reserve());
	put(state, "addressed\n");
	assert(!usb_external_host_reserve());
	put(state, "surprise\n");
	assert(!usb_external_host_reserve());
	put(state, "not attached\n");
	assert(unlink(event) == 0);
	assert(!usb_external_host_reserve()); // cannot inspect bound gadget
	put(event, "OTHER=value\n");
	assert(!usb_external_host_reserve()); // unsupported kernel uevent
	put(event, "USB_UDC_NAME=dwc2\nUSB_UDC_DRIVER=g_mass_storage\n");
	assert(!usb_external_host_reserve()); // unknown idle gadget
	put(event, "USB_UDC_NAME=dwc2\nUSB_UDC_DRIVER=android0\n");
	assert(!usb_external_host_reserve()); // claimed owner does not match configfs

	put(event, "USB_UDC_NAME=dwc2\n");
	fake_adb_wanted = true; assert(!usb_external_host_reserve()); fake_adb_wanted = false;
	fake_adb_running = true; assert(!usb_external_host_reserve()); fake_adb_running = false;
	fake_handover = true; assert(!usb_external_host_reserve()); fake_handover = false;
	atomic_store(&dac_owns_usb, true); assert(!usb_external_host_reserve()); atomic_store(&dac_owns_usb, false);
	atomic_store(&storage_active, true); assert(!usb_external_host_reserve()); atomic_store(&storage_active, false);
	assert(pthread_mutex_lock(&storage_lock) == 0);
	assert(!usb_external_host_reserve()); // do not race an ongoing export
	assert(pthread_mutex_unlock(&storage_lock) == 0);

	assert(usb_external_host_reserve());
	assert(usb_external_host_reserved());
	assert(usb_external_host_restore_safe());
	put(state, "configured\n");
	assert(!usb_external_host_restore_safe()); // do not interrupt an attached PC
	put(state, "not attached\n");
	fake_adb_running = true; assert(!usb_external_host_restore_safe()); fake_adb_running = false;
	assert(!usb_external_host_reserve());
	usb_external_host_release();
	assert(!usb_external_host_reserved());

	// Park and later restore only our own disconnected idle storage gadget.
	put(test_storage_udc, "udc0\n");
	reflect_storage_binding = true;
	fail_storage_write = true;
	assert(!usb_external_host_reserve());
	expect(test_storage_udc, "udc0\n");
	assert(!host_parked_storage); // it was never actually unbound
	partial_storage_unbind = true;
	assert(!usb_external_host_reserve());
	assert(host_parked_storage); // failed write did apply; failed rebind is retried
	expect(test_storage_udc, "\n");
	fail_storage_write = partial_storage_unbind = false;
	usb_external_host_release();
	assert(!host_parked_storage);
	expect(test_storage_udc, "udc0\n");
	assert(usb_external_host_reserve());
	assert(host_parked_storage);
	expect(test_storage_udc, "\n");
	fake_handover = true; // an audio interface still owns the port after restore
	usb_external_host_release();
	assert(host_parked_storage);
	expect(test_storage_udc, "\n");
	fake_handover = false;
	assert(pthread_mutex_lock(&storage_lock) == 0);
	resume_parked_storage_locked();
	assert(pthread_mutex_unlock(&storage_lock) == 0);
	assert(!host_parked_storage);
	expect(test_storage_udc, "udc0\n");

	// Crash recovery also parks our own idle gadget if another process rebound
	// it; an unknown gadget or connected PC is still left untouched.
	usb_external_host_block_recovery();
	assert(usb_external_host_restore_safe());
	assert(host_parked_storage);
	expect(test_storage_udc, "\n");
	usb_external_host_release();
	assert(!host_parked_storage);
	expect(test_storage_udc, "udc0\n");

	// ADB may take over after the trial parked our gadget. An already-empty
	// UDC needs no write (real configfs rejects a second unbind).
	fail_storage_write = true;
	assert(!gadget_unbind_if_bound());
	fail_storage_write = false;
	assert(gadget_unbind_if_bound());
	expect(test_storage_udc, "\n");
	fail_storage_write = true;
	assert(gadget_unbind_if_bound());
	fail_storage_write = false;

	// The V1 has one controller: don't select an arbitrary one on other boards.
	char udc1[256]; pathcat(udc1, sizeof(udc1), test_udc_class, "/udc1");
	assert(mkdir(udc1, 0700) == 0);
	assert(!usb_external_host_reserve());
	assert(rmdir(udc1) == 0);

	assert(unlink(state) == 0 && unlink(event) == 0 && rmdir(udc) == 0);
	assert(unlink(test_storage_udc) == 0 && rmdir(test_udc_class) == 0 && rmdir(root) == 0);
	puts("PASS usb ownership (UDC, unknown gadgets, activity and parked-gadget recovery)");
	return 0;
}
