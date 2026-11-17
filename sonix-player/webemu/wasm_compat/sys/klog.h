#ifndef WEBEMU_WASM_COMPAT_SYS_KLOG_H
#define WEBEMU_WASM_COMPAT_SYS_KLOG_H

#include <errno.h>

static inline int klogctl(int type, char *bufp, int len) {
	(void)type;
	(void)bufp;
	(void)len;
	errno = EPERM;
	return -1;
}

#endif /* WEBEMU_WASM_COMPAT_SYS_KLOG_H */
