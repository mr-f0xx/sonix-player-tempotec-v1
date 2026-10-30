#ifndef QOBUZDOWNLOAD_H
#define QOBUZDOWNLOAD_H
#include "qobuz.h"
/* One permanent transfer, no queue. Callback runs on the worker, never the UI.
 * Error is valid only during callback. Root and metadata are copied at start. */
bool qobuzdownload_start(const char *root, const qobuz_track_t *track,
                        void (*done)(const char *error));
bool qobuzdownload_busy(void);
void qobuzdownload_cancel(void);
#endif
