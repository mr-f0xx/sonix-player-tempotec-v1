#ifndef BACKLIGHT_H
#define BACKLIGHT_H

#define BACKLIGHT_MAX_DEFAULT 100

// Keep the V1's stock PWM driver off its full-duty endpoint (issue #267).
// At max_brightness the PWM takes a constant-output path; after a screen-off
// cycle users report a brief flash followed by a dark screen. One raw step
// below maximum stays on the working PWM path. Apply this only at the hardware
// boundary, so an existing 100% setting and the sliders' range remain intact.
// Zero must stay zero: neither a dark screen nor a wake hold is an on-level.
static inline long backlight_safe_level(long value, long max) {
	if (max <= 0) {
		max = BACKLIGHT_MAX_DEFAULT;
	}
	// A one-level device has no nonzero interior level; do not turn it off.
	long limit = max > 1 ? max - 1 : max;
	if (value < 0) {
		return 0;
	}
	return value > limit ? limit : value;
}

#endif // BACKLIGHT_H
