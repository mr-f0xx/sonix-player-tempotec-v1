#ifndef HIRESBADGE_H
#define HIRESBADGE_H

#include <stdbool.h>

// Match the now-playing quality marks: above 16 bit or 48 kHz, or DSD.
// Unknown and lossy streams must not be inferred from their decoded PCM.
static inline bool hiresbadge_qualifies(bool available, bool lossy, int bits, int rate, bool dsd) {
	return available && !lossy && (dsd || (bits > 0 && rate > 0 && (bits > 16 || rate > 48000)));
}

#endif
