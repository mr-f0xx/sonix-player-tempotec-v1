// Verifies that usbdac.c's gadget_build() writes the TempoTec V1 DAC identity
// (distinct from mass storage), configures the UAC2 descriptors and binds the
// UDC, and enables dynamic_feedback on the V1's /sys/class/uac_sa/uac_sa node.
#define _GNU_SOURCE 1

#define TEST_ROOT "/tmp/sonix-usbdac-gadget-test"
#define GADGET TEST_ROOT "/android0"
#define UAC_UDC_CLASS TEST_ROOT "/udc"
#define UAC_CLASS_PRIMARY TEST_ROOT "/class_uac_sa"
#define UAC_CLASS_FALLBACK TEST_ROOT "/class_android_usb_missing"

#include "../src/system/audio/usbdac.c"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

const char *usb_product(void) { return "TempoTec V1"; }
const char *tr(const char *key) { return key; }

static void touch_file(const char *path, const char *initial) {
	FILE *f = fopen(path, "w");
	assert(f);
	if (initial) {
		assert(fputs(initial, f) >= 0);
	}
	assert(fclose(f) == 0);
}

static void expect_file(const char *path, const char *expected) {
	char buf[256] = {0};
	FILE *f = fopen(path, "r");
	assert(f);
	size_t n = fread(buf, 1, sizeof(buf) - 1, f);
	fclose(f);
	buf[n] = '\0';
	assert(strcmp(buf, expected) == 0);
}

static void cleanup_tree(void) {
	// Best-effort cleanup of the fixed test tree before and after the run.
	unlink(GADGET "/configs/c.1/uac_sa.a");
	unlink(GADGET "/configs/c.1/mass_storage.0");
	unlink(GADGET "/configs/c.1/MaxPower");
	unlink(GADGET "/configs/c.1/bmAttributes");
	unlink(GADGET "/configs/c.1/strings/0x409/configuration");
	rmdir(GADGET "/configs/c.1/strings/0x409");
	rmdir(GADGET "/configs/c.1/strings");
	rmdir(GADGET "/configs/c.1");
	rmdir(GADGET "/configs");
	unlink(GADGET "/strings/0x409/manufacturer");
	unlink(GADGET "/strings/0x409/product");
	unlink(GADGET "/strings/0x409/serialnumber");
	rmdir(GADGET "/strings/0x409");
	rmdir(GADGET "/strings");
	unlink(UAC_FUNC "/c_chmask");
	unlink(UAC_FUNC "/c_ssize");
	unlink(UAC_FUNC "/c_srate");
	unlink(UAC_FUNC "/p_chmask");
	rmdir(UAC_FUNC);
	rmdir(GADGET "/functions/mass_storage.0");
	rmdir(GADGET "/functions");
	unlink(GADGET "/bDeviceClass");
	unlink(GADGET "/bDeviceSubClass");
	unlink(GADGET "/bDeviceProtocol");
	unlink(GADGET "/bcdUSB");
	unlink(GADGET "/bcdDevice");
	unlink(GADGET "/idVendor");
	unlink(GADGET "/idProduct");
	unlink(GADGET "/UDC");
	rmdir(GADGET);
	rmdir(UAC_UDC_CLASS "/13500000.otg_new");
	rmdir(UAC_UDC_CLASS);
	unlink(UAC_CLASS_PRIMARY "/dynamic_feedback");
	rmdir(UAC_CLASS_PRIMARY);
	rmdir(TEST_ROOT);
}

int main(void) {
	cleanup_tree();

	assert(mkdir(TEST_ROOT, 0700) == 0);
	assert(mkdir(GADGET, 0700) == 0);
	assert(mkdir(GADGET "/functions", 0700) == 0);
	assert(mkdir(GADGET "/functions/mass_storage.0", 0700) == 0);
	assert(mkdir(UAC_FUNC, 0700) == 0);
	assert(mkdir(GADGET "/strings", 0700) == 0);
	assert(mkdir(GADGET "/strings/0x409", 0700) == 0);
	assert(mkdir(GADGET "/configs", 0700) == 0);
	assert(mkdir(GADGET "/configs/c.1", 0700) == 0);
	assert(mkdir(GADGET "/configs/c.1/strings", 0700) == 0);
	assert(mkdir(GADGET "/configs/c.1/strings/0x409", 0700) == 0);
	assert(mkdir(UAC_UDC_CLASS, 0700) == 0);
	assert(mkdir(UAC_UDC_CLASS "/13500000.otg_new", 0700) == 0);
	assert(mkdir(UAC_CLASS_PRIMARY, 0700) == 0);

	// Seed with mass-storage VID/PID left behind by usb.c (and empty files for
	// attributes whose DAC values are shorter, since configfs attributes do not
	// retain trailing bytes from prior writes the way regular files do).
	touch_file(GADGET "/bDeviceClass", "");
	touch_file(GADGET "/bDeviceSubClass", "");
	touch_file(GADGET "/bDeviceProtocol", "");
	touch_file(GADGET "/bcdUSB", "");
	touch_file(GADGET "/bcdDevice", "");
	touch_file(GADGET "/idVendor", "0x32BB");
	touch_file(GADGET "/idProduct", "0x0101");
	touch_file(GADGET "/UDC", "");
	touch_file(GADGET "/strings/0x409/manufacturer", "HiBy");
	touch_file(GADGET "/strings/0x409/product", "");
	touch_file(GADGET "/strings/0x409/serialnumber", "");
	touch_file(GADGET "/configs/c.1/MaxPower", "");
	touch_file(GADGET "/configs/c.1/bmAttributes", "0x80");
	touch_file(GADGET "/configs/c.1/strings/0x409/configuration", "");
	assert(symlink(GADGET "/functions/mass_storage.0", GADGET "/configs/c.1/mass_storage.0") == 0);

	touch_file(UAC_FUNC "/c_chmask", "");
	touch_file(UAC_FUNC "/c_ssize", "");
	touch_file(UAC_FUNC "/c_srate", "");
	touch_file(UAC_FUNC "/p_chmask", "3");
	touch_file(UAC_CLASS_PRIMARY "/dynamic_feedback", "0");

	assert(gadget_build());

	expect_file(GADGET "/idVendor", "0x22E1");
	expect_file(GADGET "/idProduct", "0xF008");
	expect_file(GADGET "/bDeviceClass", "239");
	expect_file(GADGET "/bDeviceSubClass", "2");
	expect_file(GADGET "/bDeviceProtocol", "1");
	expect_file(GADGET "/bcdUSB", "0x200");
	expect_file(GADGET "/bcdDevice", "0x100");
	expect_file(GADGET "/strings/0x409/manufacturer", "TempoTec");
	expect_file(GADGET "/strings/0x409/product", "TempoTec V1");
	expect_file(GADGET "/configs/c.1/MaxPower", "10");
	expect_file(GADGET "/configs/c.1/bmAttributes", "0xC0");
	expect_file(GADGET "/configs/c.1/strings/0x409/configuration", "uac_sa");
	expect_file(UAC_FUNC "/c_chmask", "0x03");
	expect_file(UAC_FUNC "/c_ssize", "2");
	expect_file(UAC_FUNC "/p_chmask", "0");
	expect_file(GADGET "/UDC", "13500000.otg_new");
	expect_file(UAC_CLASS_PRIMARY "/dynamic_feedback", "1");

	struct stat st;
	assert(lstat(GADGET "/configs/c.1/mass_storage.0", &st) != 0);
	assert(lstat(GADGET "/configs/c.1/uac_sa.a", &st) == 0 && S_ISLNK(st.st_mode));

	gadget_teardown();
	assert(lstat(GADGET "/configs/c.1/uac_sa.a", &st) != 0);

	cleanup_tree();
	puts("PASS usbdac gadget (VID/PID, UAC2 descriptors, UDC bind and uac_sa dynamic_feedback)");
	return 0;
}
