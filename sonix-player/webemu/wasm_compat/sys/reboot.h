#ifndef WEBEMU_WASM_COMPAT_SYS_REBOOT_H
#define WEBEMU_WASM_COMPAT_SYS_REBOOT_H

#include <errno.h>

#define RB_AUTOBOOT 0x01234567
#define RB_HALT_SYSTEM 0xcdef0123
#define RB_POWER_OFF 0x4321fedc

static inline int reboot(int cmd) {
	(void)cmd;
	errno = EPERM;
	return -1;
}

#endif /* WEBEMU_WASM_COMPAT_SYS_REBOOT_H */
