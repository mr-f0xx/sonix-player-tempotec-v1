#ifndef HIRESBADGE_H
#define HIRESBADGE_H

#include <stdbool.h>

// The badge's design is shared by every layout; visibility and corner remain
// per-layout settings. The stacked text is the original look and the default
// for configs written before this choice existed.
typedef enum {
	HIRESBADGE_TWO_LINE = 0,
	HIRESBADGE_ONE_LINE,
	HIRESBADGE_INITIALS,
	HIRESBADGE_LOGO,
	HIRESBADGE_STYLE_COUNT,
} hiresbadge_style_t;

static inline hiresbadge_style_t hiresbadge_style_from_int(int value) {
	return value >= HIRESBADGE_TWO_LINE && value < HIRESBADGE_STYLE_COUNT
		? (hiresbadge_style_t)value : HIRESBADGE_TWO_LINE;
}

// NULL for the image design: it carries the official artwork, not a label.
static inline const char *hiresbadge_text(hiresbadge_style_t style) {
	switch (hiresbadge_style_from_int(style)) {
	case HIRESBADGE_ONE_LINE: return "HI-RES";
	case HIRESBADGE_INITIALS: return "HR";
	case HIRESBADGE_TWO_LINE: return "HI-\nRES";
	default: return NULL;
	}
}

// Match the now-playing quality marks: above 16 bit or 48 kHz, or DSD.
// Unknown and lossy streams must not be inferred from their decoded PCM.
static inline bool hiresbadge_qualifies(bool available, bool lossy, int bits, int rate, bool dsd) {
	return available && !lossy && (dsd || (bits > 0 && rate > 0 && (bits > 16 || rate > 48000)));
}

// A left-corner badge only displaces the chevron when it is actually shown.
static inline bool hiresbadge_back_right(bool edge_layout, bool qualifies, bool left_position, bool right_position, bool source) {
	return edge_layout && qualifies && (left_position || (right_position && source));
}

#endif
