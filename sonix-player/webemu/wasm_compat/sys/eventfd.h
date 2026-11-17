#ifndef WEBEMU_WASM_COMPAT_SYS_EVENTFD_H
#define WEBEMU_WASM_COMPAT_SYS_EVENTFD_H

#include <errno.h>
#include <stdint.h>

typedef uint64_t eventfd_t;

#define EFD_SEMAPHORE 1
#define EFD_CLOEXEC 02000000
#define EFD_NONBLOCK 00004000

static inline int eventfd(unsigned int initval, int flags) {
	(void)initval;
	(void)flags;
	errno = ENOSYS;
	return -1;
}

#endif /* WEBEMU_WASM_COMPAT_SYS_EVENTFD_H */
