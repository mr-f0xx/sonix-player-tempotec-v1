#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static char fake_udcs[256], fake_mounts[256], fake_lun[256], fake_card[256];
static bool claim_succeeds, claim_unmounts, fail_lun_write, switch_adb_during_claim;
static bool adb_wanted, adb_running, handover;
static int stop_calls, closes, opens, mount_calls, begin_calls, end_calls, claim_fd = -1;

static void set_mount(bool mounted) {
	FILE *f = fopen(fake_mounts, "w"); assert(f);
	fprintf(f, "rootfs / rootfs rw 0 0\n");
	if (mounted) fprintf(f, "/dev/test-card %s exfat rw 0 0\n", fake_card);
	assert(fclose(f) == 0);
}
static FILE *fake_fopen(const char *path, const char *mode) {
	if (fail_lun_write && strcmp(path, fake_lun) == 0 && strcmp(mode, "w") == 0) {
		errno = EACCES; return NULL;
	}
	return fopen(path, mode);
}
static int fake_system(const char *command) { (void)command; return 0; }
#define USB_UDC_CLASS fake_udcs
#define USB_MOUNTS fake_mounts
#define STORAGE_LUN_PATH fake_lun
#define fopen fake_fopen
#define system fake_system
#include "../src/system/device/usb.c"
#undef fopen
#undef system

bool adb_switched_on(void) { return adb_wanted; }
bool adb_is_running(void) { return adb_running; }
bool usbaudio_handover_pending(void) { return handover; }
bool storage_usb_begin(void) { begin_calls++; return true; }
void storage_usb_end(void) { end_calls++; }
int usbexport_detach_and_claim(const char *device, const char *mountpoint, bool *unmounted) {
	assert(strcmp(device, "/dev/test-card") == 0 && strcmp(mountpoint, fake_card) == 0);
	*unmounted = claim_unmounts;
	if (claim_unmounts) set_mount(false);
	if (switch_adb_during_claim) adb_wanted = true;
	if (!claim_succeeds) return -1;
	claim_fd = open("/dev/null", O_RDONLY | O_CLOEXEC);
	assert(claim_fd >= 0);
	return claim_fd;
}
bool sysserver_available(void) { return true; }
int sysserver_mount(const char *device, const char *mountpoint) {
	assert(strcmp(device, "/dev/test-card") == 0 && strcmp(mountpoint, fake_card) == 0);
	mount_calls++; set_mount(true); return 0;
}
void device_state_note_storage_gone(void) {}
void audio_stop(void) { stop_calls++; }
void library_close(void) { closes++; }
void audiobookdb_close(void) { closes++; }
void geniusdb_close(void) { closes++; }
void radio_store_close(void) { closes++; }
void logging_suspend_for_usb(void) { closes++; }
void btlog_card_release(void) { closes++; }
void cover_set_cache_dir(const char *path) { if (path) opens++; else closes++; }
bool library_open(const char *path) { (void)path; opens++; return true; }
bool audiobookdb_open(const char *path) { (void)path; opens++; return true; }
bool geniusdb_open(const char *path) { (void)path; opens++; return true; }
bool radio_store_open(const char *path) { (void)path; opens++; return true; }
void logging_resume_after_usb(void) { opens++; }
void btlog_card_attach(const char *path) { (void)path; opens++; }
bool library_card_returned(const char *path) { (void)path; return true; }
bool gui_post(void (*callback)(void *), void *data) { (void)callback; (void)data; return true; }
void gui_notify_popup(const char *text) { (void)text; }
void browser_refresh(void) {}
char *read_file_content(const char *path) {
	FILE *f = fopen(path, "r"); if (!f) return NULL;
	char buf[4096]; size_t n = fread(buf, 1, sizeof(buf) - 1, f); fclose(f); buf[n] = 0;
	return strdup(buf);
}

static void pathcat(char *out, size_t size, const char *a, const char *b) {
	assert(strlen(a) + strlen(b) + 1 <= size); strcpy(out, a); strcat(out, b);
}
static void put(const char *path, const char *text) {
	FILE *f = fopen(path, "w"); assert(f && fputs(text, f) >= 0 && fclose(f) == 0);
}
static void expect(const char *path, const char *text) {
	char *value = read_file_content(path); assert(value && strcmp(value, text) == 0); free(value);
}
static void reset(void) {
	assert(!storage_active && export_mode == EXPORT_NONE && export_claim_fd == -1);
	claim_succeeds = claim_unmounts = fail_lun_write = switch_adb_during_claim = false;
	adb_wanted = adb_running = handover = false;
	stop_calls = closes = opens = mount_calls = begin_calls = end_calls = 0;
	claim_fd = -1;
	put(fake_lun, "\n"); set_mount(true);
}

int main(void) {
	char root[] = "/tmp/sonix-usbstorage-XXXXXX"; assert(mkdtemp(root));
	pathcat(fake_udcs, sizeof(fake_udcs), root, "/udcs");
	pathcat(fake_mounts, sizeof(fake_mounts), root, "/mounts");
	pathcat(fake_lun, sizeof(fake_lun), root, "/lun");
	pathcat(fake_card, sizeof(fake_card), root, "/card");
	assert(mkdir(fake_udcs, 0700) == 0 && mkdir(fake_card, 0700) == 0);
	char udc[256], state[256];
	pathcat(udc, sizeof(udc), fake_udcs, "/udc0");
	pathcat(state, sizeof(state), udc, "/state");
	assert(mkdir(udc, 0700) == 0); put(state, "configured\n");
	snprintf(sd_device, sizeof(sd_device), "%s", "/dev/test-card");
	snprintf(sd_mount, sizeof(sd_mount), "%s", fake_card);

	// A busy mount never reaches the LUN. Services reopen on the still-mounted
	// card and the caller can suppress retries for this connection.
	reset();
	assert(!storage_export());
	assert(!storage_active && export_mode == EXPORT_NONE && export_claim_fd == -1);
	assert(stop_calls == 1 && closes == 7 && opens == 7 && mount_calls == 0);
	assert(begin_calls == end_calls);
	expect(fake_lun, "\n");

	// If the normal unmount succeeded but the block claim failed, remount and
	// reopen rather than exporting an unclaimed or still-open device.
	reset(); claim_unmounts = true;
	assert(!storage_export());
	assert(!storage_active && mount_calls == 1 && opens == 7);
	expect(fake_lun, "\n");

	// ADB requested while unmounting wins before the LUN write.
	reset(); claim_succeeds = claim_unmounts = switch_adb_during_claim = true;
	assert(!storage_export());
	assert(!storage_active && mount_calls == 1 && export_claim_fd == -1);
	expect(fake_lun, "\n");

	// A failed LUN write/clear must keep the claim and not remount. Once the
	// attribute becomes writable, the normal restore can release and recover.
	reset(); claim_succeeds = claim_unmounts = fail_lun_write = true;
	assert(!storage_export());
	assert(storage_active && export_mode == EXPORT_OWN_GADGET && export_claim_fd >= 0);
	assert(mount_calls == 0 && opens == 0);
	fail_lun_write = false;
	assert(storage_restore());
	assert(!storage_active && export_claim_fd == -1 && mount_calls == 1 && opens == 7);
	expect(fake_lun, "\n");

	// A successful export holds the claim until the verified LUN removal.
	reset(); claim_succeeds = claim_unmounts = true;
	assert(storage_export());
	assert(storage_active && export_mode == EXPORT_OWN_GADGET && export_claim_fd >= 0);
	expect(fake_lun, "/dev/test-card\n");
	fail_lun_write = true;
	assert(!storage_restore());
	assert(storage_active && export_claim_fd >= 0 && mount_calls == 0);
	fail_lun_write = false;
	assert(storage_restore());
	assert(!storage_active && export_claim_fd == -1 && mount_calls == 1 && opens == 7);
	assert(begin_calls == end_calls);
	expect(fake_lun, "\n");

	assert(unlink(state) == 0 && rmdir(udc) == 0 && rmdir(fake_udcs) == 0);
	assert(unlink(fake_lun) == 0 && unlink(fake_mounts) == 0 && rmdir(fake_card) == 0 && rmdir(root) == 0);
	puts("PASS USB storage rollback (services, ownership and verified LUN removal)");
	return 0;
}
