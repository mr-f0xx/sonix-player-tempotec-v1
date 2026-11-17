#ifndef WEBEMU_WASM_COMPAT_LIMITS_H
#define WEBEMU_WASM_COMPAT_LIMITS_H

#include_next <limits.h>

#ifndef NAME_MAX
#define NAME_MAX 255
#endif

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#endif /* WEBEMU_WASM_COMPAT_LIMITS_H */
