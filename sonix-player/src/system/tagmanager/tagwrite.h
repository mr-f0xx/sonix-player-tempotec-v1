#ifndef TAGWRITE_H
#define TAGWRITE_H

// Replaces one non-empty field, leaving other metadata and audio intact.
// Writes a temporary file beside the original and atomically replaces it only
// after a complete, flushed copy. Unsupported layouts are rejected unchanged.
typedef enum { TAGWRITE_TRACK, TAGWRITE_GENRE } tagwrite_field_t;
typedef enum { TAGWRITE_OK, TAGWRITE_INVALID, TAGWRITE_UNSUPPORTED, TAGWRITE_IO } tagwrite_result_t;
tagwrite_result_t tagwrite_set(const char *path, tagwrite_field_t field, const char *value);
#endif
