#ifndef WEBEMU_WASM_COMPAT_SYS_SYSCALL_H
#define WEBEMU_WASM_COMPAT_SYS_SYSCALL_H

#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>

#ifndef SYS_gettid
#define SYS_gettid 186
#endif

static inline long syscall(long number, ...) {
	if (number == SYS_gettid) {
		return (long)(uintptr_t)pthread_self();
	}
	return -1;
}

#endif /* WEBEMU_WASM_COMPAT_SYS_SYSCALL_H */
