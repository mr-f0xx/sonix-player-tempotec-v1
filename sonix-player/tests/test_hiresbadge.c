#include <assert.h>
#include <stdio.h>
#include "src/gui/nowplaying/hiresbadge.h"

int main(void) {
	assert(!hiresbadge_qualifies(false, false, 24, 192000, false));
	assert(!hiresbadge_qualifies(true, true, 24, 192000, false));
	assert(!hiresbadge_qualifies(true, false, 16, 44100, false));
	assert(!hiresbadge_qualifies(true, false, 16, 48000, false));
	assert(!hiresbadge_qualifies(true, false, 24, 0, false));
	assert(!hiresbadge_qualifies(true, false, 0, 96000, false));
	assert(hiresbadge_qualifies(true, false, 24, 44100, false));
	assert(hiresbadge_qualifies(true, false, 16, 96000, false));
	assert(hiresbadge_qualifies(true, false, 0, 0, true));
	puts("hi-res badge quality checks passed");
	return 0;
}
