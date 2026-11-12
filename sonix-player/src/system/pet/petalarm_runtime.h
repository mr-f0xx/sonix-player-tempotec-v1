#ifndef SYSTEM_PET_PETALARM_RUNTIME_H
#define SYSTEM_PET_PETALARM_RUNTIME_H

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

// Start after the GUI, power manager and restored volume are ready. If the
// remembered track is still being restored, defer playback until it finishes
// so that the restore cannot overwrite the alarm. The existing pet watcher
// calls tick once a second; there is no extra thread or busy loop.
void petalarm_runtime_init(bool restore_pending);
void petalarm_runtime_restore_complete(void);
void petalarm_runtime_tick(time_t now, uint32_t monotonic_ms);
void petalarm_runtime_shutdown(void);

#endif /* SYSTEM_PET_PETALARM_RUNTIME_H */
