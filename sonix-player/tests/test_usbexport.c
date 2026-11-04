#define _GNU_SOURCE
#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <mntent.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/types.h>
#include <unistd.h>

static char test_mounts[256], test_proc[256], test_mountpoint[256];
static const char *test_device = "/dev/sonix-test-card";
static int unmount_calls, open_calls, current_claim = -1;
static bool fail_unmount, leave_mount, fail_claim, wrong_claim, raw_fd_open;
static dev_t test_rdev;

static void put(const char *path, const char *text) {
	FILE *f = fopen(path, "w");
	assert(f && fputs(text, f) >= 0 && fclose(f) == 0);
}
static int fake_stat(const char *path, struct stat *st) {
	if (strcmp(path, test_device) == 0 || (raw_fd_open && strstr(path, "/fd/5"))) {
		memset(st, 0, sizeof(*st));
		st->st_mode = S_IFBLK | 0600;
		st->st_rdev = test_rdev;
		return 0;
	}
	return stat(path, st);
}
static int fake_fstat(int fd, struct stat *st) {
	if (fd == current_claim) {
		memset(st, 0, sizeof(*st));
		st->st_mode = S_IFBLK | 0600;
		st->st_rdev = wrong_claim ? makedev(179, 9) : test_rdev;
		return 0;
	}
	return fstat(fd, st);
}
static int fake_open(const char *path, int flags, ...) {
	assert(strcmp(path, test_device) == 0);
	assert((flags & (O_RDONLY | O_EXCL | O_CLOEXEC)) == (O_RDONLY | O_EXCL | O_CLOEXEC));
	open_calls++;
	if (fail_claim) { errno = EBUSY; return -1; }
	current_claim = open("/dev/null", O_RDONLY | O_CLOEXEC);
	return current_claim;
}
static int fake_umount(const char *path) {
	assert(strcmp(path, test_mountpoint) == 0);
	unmount_calls++;
	if (fail_unmount) { errno = EBUSY; return -1; }
	if (!leave_mount) put(test_mounts, "rootfs / rootfs rw 0 0\n");
	return 0;
}
static unsigned int fake_sleep(unsigned int n) { (void)n; return 0; }

#define USBEXPORT_MOUNTS test_mounts
#define USBEXPORT_PROC test_proc
#define stat(...) fake_stat(__VA_ARGS__)
#define fstat(...) fake_fstat(__VA_ARGS__)
#define open(...) fake_open(__VA_ARGS__)
#define umount(...) fake_umount(__VA_ARGS__)
#define sleep(...) fake_sleep(__VA_ARGS__)
#include "../src/system/device/usbexport.c"
#undef stat
#undef fstat
#undef open
#undef umount
#undef sleep

static void pathcat(char *out, size_t size, const char *a, const char *b) {
	assert(strlen(a) + strlen(b) + 1 <= size);
	strcpy(out, a); strcat(out, b);
}
static void mounted_on(const char *source, const char *extra) {
	FILE *f = fopen(test_mounts, "w"); assert(f);
	fprintf(f, "rootfs / rootfs rw 0 0\n%s %s exfat rw 0 0\n%s", source, test_mountpoint, extra ? extra : "");
	assert(fclose(f) == 0);
}
static void reset(void) {
	fail_unmount = leave_mount = fail_claim = wrong_claim = raw_fd_open = false;
	unmount_calls = open_calls = 0;
	current_claim = -1;
	mounted_on(test_device, NULL);
}

int main(void) {
	test_rdev = makedev(179, 1);
	char root[] = "/tmp/sonix-usbexport-XXXXXX"; assert(mkdtemp(root));
	pathcat(test_mounts, sizeof(test_mounts), root, "/mounts");
	pathcat(test_proc, sizeof(test_proc), root, "/proc");
	pathcat(test_mountpoint, sizeof(test_mountpoint), root, "/card");
	assert(mkdir(test_mountpoint, 0700) == 0 && mkdir(test_proc, 0700) == 0);
	char pid[256], fds[256], raw[256];
	pathcat(pid, sizeof(pid), test_proc, "/123");
	pathcat(fds, sizeof(fds), pid, "/fd");
	pathcat(raw, sizeof(raw), fds, "/5");
	assert(mkdir(pid, 0700) == 0 && mkdir(fds, 0700) == 0);
	put(raw, "fake raw fd");
	bool unmounted;

	put(test_mounts, "rootfs / rootfs rw 0 0\n"); // includes an earlier lazy detach
	assert(usbexport_detach_and_claim(test_device, test_mountpoint, &unmounted) < 0);
	assert(!unmounted && unmount_calls == 0 && open_calls == 0);
	mounted_on("/dev/not-the-card", NULL);
	assert(usbexport_detach_and_claim(test_device, test_mountpoint, &unmounted) < 0);
	assert(!unmounted && unmount_calls == 0);
	mounted_on(test_device, "/dev/sonix-test-card /elsewhere exfat rw 0 0\n");
	assert(usbexport_detach_and_claim(test_device, test_mountpoint, &unmounted) < 0);
	assert(!unmounted && unmount_calls == 0);

	reset(); fail_unmount = true;
	assert(usbexport_detach_and_claim(test_device, test_mountpoint, &unmounted) < 0);
	assert(!unmounted && unmount_calls == 3 && open_calls == 0);
	reset(); leave_mount = true;
	assert(usbexport_detach_and_claim(test_device, test_mountpoint, &unmounted) < 0);
	assert(unmounted && unmount_calls == 1 && open_calls == 0);
	reset(); fail_claim = true;
	assert(usbexport_detach_and_claim(test_device, test_mountpoint, &unmounted) < 0);
	assert(unmounted && unmount_calls == 1 && open_calls == 3);
	reset(); wrong_claim = true;
	assert(usbexport_detach_and_claim(test_device, test_mountpoint, &unmounted) < 0);
	assert(unmounted && open_calls == 3);
	reset(); raw_fd_open = true; // a FUSE daemon still holds the block device
	assert(usbexport_detach_and_claim(test_device, test_mountpoint, &unmounted) < 0);
	assert(unmounted && open_calls == 3);

	reset();
	int fd = usbexport_detach_and_claim(test_device, test_mountpoint, &unmounted);
	assert(fd >= 0 && unmounted && unmount_calls == 1 && open_calls == 1);
	assert(fcntl(fd, F_GETFD) >= 0); // claim remains held for the USB LUN
	assert(close(fd) == 0);

	reset();
	assert(unlink(raw) == 0 && rmdir(fds) == 0 && rmdir(pid) == 0 && rmdir(test_proc) == 0);
	assert(usbexport_detach_and_claim(test_device, test_mountpoint, &unmounted) < 0);
	assert(unmounted); // unknown /proc ownership fails closed
	assert(unlink(test_mounts) == 0 && rmdir(test_mountpoint) == 0 && rmdir(root) == 0);
	puts("PASS usb export (normal unmount, exclusive claim and FUSE-open safeguards)");
	return 0;
}
