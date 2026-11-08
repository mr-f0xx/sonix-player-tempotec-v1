#include "usbexport.h"

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
#include <sys/types.h>
#include <unistd.h>

#ifndef USBEXPORT_MOUNTS
#define USBEXPORT_MOUNTS "/proc/mounts"
#endif
#ifndef USBEXPORT_PROC
#define USBEXPORT_PROC "/proc"
#endif

// Requiring a visible, matching mount *before* a normal unmount matters: a
// previously lazy-detached mount is absent from /proc/mounts but can still be
// in use. An already-missing mount is not proof it is safe to export.
static bool mounts_match(const char *mountpoint, dev_t device, bool before_unmount) {
	char canonical[PATH_MAX];
	if (!realpath(mountpoint, canonical)) return false;
	FILE *f = setmntent(USBEXPORT_MOUNTS, "r");
	if (!f) return false;
	bool target = false, valid = true;
	struct mntent current, *entry;
	char line[8192];
	for (;;) {
		errno = 0;
		entry = getmntent_r(f, &current, line, sizeof(line));
		if (!entry) { if (errno || ferror(f)) valid = false; break; }
		struct stat st;
		bool same_device = stat(entry->mnt_fsname, &st) == 0 && S_ISBLK(st.st_mode) && st.st_rdev == device;
		bool same_mount = strcmp(entry->mnt_dir, canonical) == 0;
		if (same_mount) {
			if (target || !same_device) { valid = false; break; }
			target = true;
		} else if (same_device) {
			// Includes another visible mount or bind mount of this device.
			valid = false;
			break;
		}
	}
	if (ferror(f)) valid = false;
	endmntent(f);
	return valid && (before_unmount ? target : !target);
}

static bool decimal_name(const char *name) {
	if (!*name) return false;
	for (; *name; name++) if (*name < '0' || *name > '9') return false;
	return true;
}

// FUSE filesystems such as ntfs-3g can keep a raw block fd after their mount
// vanishes. Exclusive kernel claims alone do not detect an ordinary raw fd.
// Inspect process fds as well; inaccessible/unknown state refuses export.
// skip_fd is our own exclusive claim, if it has already been opened.
static bool other_block_fd_open(dev_t device, int skip_fd) {
	DIR *proc = opendir(USBEXPORT_PROC);
	if (!proc) return true;
	bool busy = false;
	struct dirent *process;
	for (;;) {
		errno = 0;
		process = readdir(proc);
		if (!process) { if (errno) busy = true; break; }
		if (!decimal_name(process->d_name)) continue;
		char dirpath[PATH_MAX];
		int n = snprintf(dirpath, sizeof(dirpath), "%s/%s/fd", USBEXPORT_PROC, process->d_name);
		if (n < 0 || (size_t)n >= sizeof(dirpath)) { busy = true; break; }
		DIR *fds = opendir(dirpath);
		if (!fds) {
			if (errno == ENOENT || errno == ESRCH) continue; // process exited
			busy = true;
			break;
		}
		struct dirent *fd;
		for (;;) {
			errno = 0;
			fd = readdir(fds);
			if (!fd) { if (errno) busy = true; break; }
			if (!decimal_name(fd->d_name)) continue;
			if (skip_fd >= 0 && strtol(process->d_name, NULL, 10) == (long)getpid() &&
				strtol(fd->d_name, NULL, 10) == skip_fd) continue;
			char path[PATH_MAX];
			n = snprintf(path, sizeof(path), "%s/%s", dirpath, fd->d_name);
			if (n < 0 || (size_t)n >= sizeof(path)) { busy = true; break; }
			struct stat st;
			if (stat(path, &st) != 0) {
				if (errno == ENOENT || errno == ESRCH) continue; // fd closed
				busy = true;
				break;
			}
			if (S_ISBLK(st.st_mode) && st.st_rdev == device) { busy = true; break; }
		}
		closedir(fds);
		if (busy) break;
	}
	closedir(proc);
	return busy;
}

static bool path_under_mount(const char *target, const char *root, size_t root_len) {
	return strncmp(target, root, root_len) == 0 && (target[root_len] == '/' || target[root_len] == '\0');
}

static void log_mount_holders(const char *mountpoint) {
	char root[PATH_MAX];
	if (!realpath(mountpoint, root)) return;
	size_t root_len = strlen(root);
	DIR *proc = opendir(USBEXPORT_PROC);
	if (!proc) return;
	struct dirent *process;
	while ((process = readdir(proc)) != NULL) {
		if (!decimal_name(process->d_name)) continue;
		char comm[64] = "";
		char path[PATH_MAX], target[PATH_MAX];
		int n = snprintf(path, sizeof(path), "%s/%s/comm", USBEXPORT_PROC, process->d_name);
		if (n > 0 && (size_t)n < sizeof(path)) {
			FILE *f = fopen(path, "r");
			if (f) {
				if (fgets(comm, sizeof(comm), f)) comm[strcspn(comm, "\r\n")] = '\0';
				fclose(f);
			}
		}
		n = snprintf(path, sizeof(path), "%s/%s/cwd", USBEXPORT_PROC, process->d_name);
		if (n > 0 && (size_t)n < sizeof(path)) {
			ssize_t len = readlink(path, target, sizeof(target) - 1);
			if (len > 0) {
				target[len] = '\0';
				if (path_under_mount(target, root, root_len)) {
					fprintf(stderr, "usb:   busy on %s: %s (pid %s %s, cwd)\n", root, target, process->d_name,
							comm[0] ? comm : "?");
				}
			}
		}
		char dirpath[PATH_MAX];
		n = snprintf(dirpath, sizeof(dirpath), "%s/%s/fd", USBEXPORT_PROC, process->d_name);
		if (n < 0 || (size_t)n >= sizeof(dirpath)) continue;
		DIR *fds = opendir(dirpath);
		if (!fds) continue;
		struct dirent *fd;
		while ((fd = readdir(fds)) != NULL) {
			if (!decimal_name(fd->d_name)) continue;
			n = snprintf(path, sizeof(path), "%s/%s", dirpath, fd->d_name);
			if (n < 0 || (size_t)n >= sizeof(path)) continue;
			ssize_t len = readlink(path, target, sizeof(target) - 1);
			if (len <= 0) continue;
			target[len] = '\0';
			if (path_under_mount(target, root, root_len)) {
				fprintf(stderr, "usb:   busy on %s: %s (pid %s %s, fd %s)\n", root, target, process->d_name,
						comm[0] ? comm : "?", fd->d_name);
			}
		}
		closedir(fds);
	}
	closedir(proc);
}

int usbexport_detach_and_claim(const char *device, const char *mountpoint, bool *unmounted) {
	if (!unmounted) return -1;
	*unmounted = false;
	struct stat st;
	if (!device || !mountpoint || stat(device, &st) != 0 || !S_ISBLK(st.st_mode) ||
		!mounts_match(mountpoint, st.st_rdev, true)) {
		fprintf(stderr, "usb: card mount/device is missing or ambiguous; export refused\n");
		return -1;
	}
	dev_t rdev = st.st_rdev;
	bool detached = false;
	for (int attempt = 0; attempt < 3; attempt++) {
		if (umount(mountpoint) == 0) { detached = true; break; }
		int error = errno;
		fprintf(stderr, "usb: normal unmount failed: %s\n", strerror(error));
		if (error != EBUSY || attempt == 2) {
			if (error == EBUSY) log_mount_holders(mountpoint);
			break;
		}
		sleep(1);
	}
	if (!detached) return -1; // never fall back to a forced or lazy unmount
	*unmounted = true;
	if (!mounts_match(mountpoint, rdev, false)) {
		fprintf(stderr, "usb: card is still mounted or mount state is unknown; export refused\n");
		return -1;
	}
	for (int attempt = 0; attempt < 3; attempt++) {
		int fd = open(device, O_RDONLY | O_EXCL | O_CLOEXEC);
		if (fd >= 0) {
			if (fstat(fd, &st) == 0 && S_ISBLK(st.st_mode) && st.st_rdev == rdev &&
				!other_block_fd_open(rdev, fd)) return fd;
			close(fd);
		} else if (errno != EBUSY) {
			break;
		}
		if (attempt < 2) sleep(1);
	}
	fprintf(stderr, "usb: card device could not be exclusively claimed or is still open; export refused\n");
	return -1;
}
