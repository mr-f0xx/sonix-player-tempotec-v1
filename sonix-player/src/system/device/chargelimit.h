#ifndef CHARGELIMIT_H
#define CHARGELIMIT_H

#include <stdbool.h>

// Hysteresis for an automatic charge ceiling. When already held, charging
// resumes only three percentage points below the configured ceiling.
bool charge_limit_should_allow(int limit, int battery_percent, bool held);

#endif /* CHARGELIMIT_H */
