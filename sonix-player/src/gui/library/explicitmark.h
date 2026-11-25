#ifndef EXPLICITMARK_H
#define EXPLICITMARK_H

// The marker for a track the file itself marks as explicit. It is drawn as
// part of the name -- after the title, after the album name -- rather than as
// a separate badge: that is what makes it travel with a scrolling title and
// fit on an album tile. The font chain carries the glyph (see fonts.c).
#define EXPLICIT_MARK " 🅴"

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

// Copies `text` into `dst` and, when `marked`, adds the marker. A name that
// fills the buffer loses as many trailing bytes as the marker needs, rather
// than losing the marker or overflowing. The original name stays untouched,
// so callers can keep using it as a database key or a path.
static inline void explicit_decorate(char *dst, size_t dst_size, const char *text, bool marked) {
	if (!dst || dst_size == 0) {
		return;
	}
	size_t len = text ? strlen(text) : 0;
	const size_t mark_len = strlen(EXPLICIT_MARK);
	if (len >= dst_size) {
		len = dst_size - 1;
	}
	if (marked && mark_len + 1 >= dst_size) {
		marked = false; // no room for even the marker on its own
	} else if (marked && len + mark_len >= dst_size) {
		len = dst_size - 1 - mark_len;
	}
	if (text && len > 0) {
		memcpy(dst, text, len);
	}
	if (marked) {
		memcpy(dst + len, EXPLICIT_MARK, mark_len);
		len += mark_len;
	}
	dst[len] = '\0';
}

#endif // EXPLICITMARK_H
