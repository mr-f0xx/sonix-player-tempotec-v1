#ifndef WEBEMU_WASM_COMPAT_SYS_PRCTL_H
#define WEBEMU_WASM_COMPAT_SYS_PRCTL_H

#include <stdarg.h>

#define PR_SET_PDEATHSIG 1
#define PR_SET_NAME 15
#define PR_GET_NAME 16
#define PR_SET_DUMPABLE 4

static inline int prctl(int option, ...) {
	(void)option;
	return 0;
}

#endif /* WEBEMU_WASM_COMPAT_SYS_PRCTL_H */
