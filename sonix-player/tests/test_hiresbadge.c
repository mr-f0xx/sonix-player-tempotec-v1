#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
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
	assert(hiresbadge_style_from_int(HIRESBADGE_TWO_LINE) == HIRESBADGE_TWO_LINE);
	assert(hiresbadge_style_from_int(HIRESBADGE_ONE_LINE) == HIRESBADGE_ONE_LINE);
	assert(hiresbadge_style_from_int(HIRESBADGE_INITIALS) == HIRESBADGE_INITIALS);
	assert(hiresbadge_style_from_int(HIRESBADGE_LOGO) == HIRESBADGE_LOGO);
	assert(hiresbadge_style_from_int(-1) == HIRESBADGE_TWO_LINE);
	assert(hiresbadge_style_from_int(HIRESBADGE_STYLE_COUNT) == HIRESBADGE_TWO_LINE);
	assert(hiresbadge_style_from_int(INT_MIN) == HIRESBADGE_TWO_LINE);
	assert(hiresbadge_style_from_int(INT_MAX) == HIRESBADGE_TWO_LINE);
	assert(strcmp(hiresbadge_text(HIRESBADGE_TWO_LINE), "HI-\nRES") == 0);
	assert(strcmp(hiresbadge_text(HIRESBADGE_ONE_LINE), "HI-RES") == 0);
	assert(strcmp(hiresbadge_text(HIRESBADGE_INITIALS), "HR") == 0);
	assert(hiresbadge_text(HIRESBADGE_LOGO) == NULL);
	puts("hi-res badge quality and design checks passed");
	return 0;
}
