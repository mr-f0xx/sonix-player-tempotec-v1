#ifndef WEBEMU_WASM_COMPAT_LINUX_FS_H
#define WEBEMU_WASM_COMPAT_LINUX_FS_H

// Emscripten's musl sysroot ships no Linux kernel UAPI headers, so the one
// ioctl sdbench.c needs is spelled out here (same treatment as linux/rtc.h).
// On the web the request reaches MEMFS, which returns ENOTTY, and the caller
// falls back to lseek() for the file size.

#include <stddef.h>
#include <sys/ioctl.h>

#define BLKGETSIZE64 _IOR(0x12, 114, size_t)

#endif /* WEBEMU_WASM_COMPAT_LINUX_FS_H */
