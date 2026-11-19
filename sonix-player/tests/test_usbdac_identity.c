// Exercise the real descriptor initialization without a USB controller.
#define _GNU_SOURCE 1
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>

static char vendor[16], product[16];
static int fail_id, writes;
static int fake_open(const char *path, int flags, ...) {
	assert(flags == O_WRONLY);
	int id = strstr(path, "/idVendor") ? 10 : 11;
	assert(strstr(path, "/idVendor") || strstr(path, "/idProduct"));
	if (id == fail_id) { errno = EACCES; return -1; }
	return id;
}
static ssize_t fake_write(int fd, const void *data, size_t n) {
	assert(fd == 10 || fd == 11);
	assert(n < sizeof(vendor));
	char *out = fd == 10 ? vendor : product;
	memcpy(out, data, n); out[n] = '\0'; writes++;
	return n;
}
static int fake_close(int fd) { assert(fd == 10 || fd == 11); return 0; }
#define open fake_open
#define write fake_write
#define close fake_close
#include "../src/system/audio/usbdac.c"
#undef open
#undef write
#undef close

const char *tr(const char *text) { (void)text; return "gadget failed: %s"; }

int main(void) {
	// Both a fresh gadget and a reused storage/ADB gadget get the audio IDs.
	assert(gadget_set_audio_identity());
	assert(strcmp(vendor, "0x32BB") == 0 && strcmp(product, "0x0004") == 0);
	strcpy(vendor, "0x0000"); strcpy(product, "0x0101");
	assert(gadget_set_audio_identity());
	assert(strcmp(vendor, "0x32BB") == 0 && strcmp(product, "0x0004") == 0);
	writes = 0; fail_id = 10;
	assert(!gadget_set_audio_identity() && writes == 0);
	assert(state.error[0]);
	writes = 0; fail_id = 11;
	assert(!gadget_set_audio_identity() && writes == 1);
	puts("PASS USB DAC identity (fresh, storage transition, descriptor failures)");
	return 0;
}
