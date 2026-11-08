#ifndef STATUSBARLAYOUT_H
#define STATUSBARLAYOUT_H

#include <stdbool.h>

// Whether the player actually uses the loaded blur behind the controls. A
// stale backdrop of the wrong shape is ignored during layout transitions.
static inline bool statusbarlayout_uses_blur(bool poster, bool vinyl, bool studio, bool immersive,
		bool oled, bool full_backdrop, bool band_backdrop) {
	if (poster || vinyl) return false;
	if (immersive) return band_backdrop;
	if (studio) return full_backdrop;
	return !oled && full_backdrop;
}

#endif
