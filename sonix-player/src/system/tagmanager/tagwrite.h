#ifndef TAGWRITE_H
#define TAGWRITE_H

#include <stddef.h>
#include <stdint.h>

// Replaces one non-empty field, leaving other metadata and audio intact.
// Writes a temporary file beside the original and atomically replaces it only
// after a complete, flushed copy. Unsupported layouts are rejected unchanged.
typedef enum { TAGWRITE_TRACK, TAGWRITE_GENRE, TAGWRITE_TITLE, TAGWRITE_ARTIST } tagwrite_field_t;
typedef enum { TAGWRITE_OK, TAGWRITE_INVALID, TAGWRITE_UNSUPPORTED, TAGWRITE_IO } tagwrite_result_t;
tagwrite_result_t tagwrite_set(const char *path, tagwrite_field_t field, const char *value);
// Replace only front-cover images; retain other pictures and audio.
tagwrite_result_t tagwrite_cover(const char *path, const uint8_t *jpeg, size_t size, int width, int height);
#endif
