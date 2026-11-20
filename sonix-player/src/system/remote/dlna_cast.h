#ifndef DLNA_CAST_H
#define DLNA_CAST_H

#include <stdbool.h>
#include <stddef.h>

#define DLNA_CAST_MAX_TARGETS 8
#define DLNA_CAST_NAME_MAX 96

typedef struct {
	char name[DLNA_CAST_NAME_MAX];
} dlna_cast_target_t;

// UPnP control-point side of DLNA. Kept separate from dlna.c, which remains
// the original receiver implementation.
void dlna_cast_scan(void);
bool dlna_cast_scan_running(void);
int dlna_cast_targets(dlna_cast_target_t *out, int max);
int dlna_cast_selected(void);
void dlna_cast_select(int index);

// Starts sending the given local file to the selected renderer. Network work
// happens on a worker thread; the caller may be the UI thread.
bool dlna_cast_play(const char *path);
void dlna_cast_stop(void);
void dlna_cast_status(char *out, size_t size);

#endif
