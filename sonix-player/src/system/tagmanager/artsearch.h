#ifndef ARTSEARCH_H
#define ARTSEARCH_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#define ARTSEARCH_MAX_RESULTS 8
#define ARTSEARCH_MAX_IMAGE (2u * 1024u * 1024u)
typedef struct {
	char album[192];
	char artist[192];
	char url[1024];
} artsearch_result_t;
// Blocking network calls: run off the UI thread. Search text is sent to Apple.
bool artsearch_find(const char *query, artsearch_result_t *results, int *count);
bool artsearch_download(const char *url, uint8_t **data, size_t *size, int *width, int *height);
#endif
