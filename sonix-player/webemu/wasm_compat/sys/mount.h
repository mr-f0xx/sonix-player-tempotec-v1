#ifndef WEBEMU_WASM_COMPAT_SYS_MOUNT_H
#define WEBEMU_WASM_COMPAT_SYS_MOUNT_H

#include <errno.h>

#define MS_RDONLY 1
#define MS_NOSUID 2
#define MS_NODEV 4
#define MS_NOEXEC 8
#define MS_SYNCHRONOUS 16
#define MS_REMOUNT 32
#define MS_NOATIME 1024
#define MS_BIND 4096

#define MNT_FORCE 1
#define MNT_DETACH 2
#define MNT_EXPIRE 4

static inline int mount(const char *source, const char *target, const char *fstype,
						unsigned long mountflags, const void *data) {
	(void)source;
	(void)target;
	(void)fstype;
	(void)mountflags;
	(void)data;
	errno = EPERM;
	return -1;
}

static inline int umount(const char *target) {
	(void)target;
	errno = EPERM;
	return -1;
}

static inline int umount2(const char *target, int flags) {
	(void)target;
	(void)flags;
	errno = EPERM;
	return -1;
}

#endif /* WEBEMU_WASM_COMPAT_SYS_MOUNT_H */
