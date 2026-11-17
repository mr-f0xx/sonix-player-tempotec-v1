#ifndef WEBEMU_WASM_COMPAT_MALLOC_H
#define WEBEMU_WASM_COMPAT_MALLOC_H

#include_next <malloc.h>
#include <stddef.h>

static inline int malloc_trim(size_t pad) {
	(void)pad;
	return 0;
}

#endif /* WEBEMU_WASM_COMPAT_MALLOC_H */
