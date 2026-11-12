#ifndef SYSTEM_PET_PETALARM_H
#define SYSTEM_PET_PETALARM_H

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

// One recurring alarm, stored independently of the pet's game state. Days use
// struct tm's numbering: bit 0 is Sunday, bit 6 Saturday. An empty mask is a
// valid, unarmed selection; all seven bits means every day.
#define PETALARM_ALL_DAYS 0x7fU
#define PETALARM_PLAYLIST_MAX 200
#define PETALARM_LATE_SECS 300

void petalarm_init(void);
bool petalarm_enabled(void);
void petalarm_set_enabled(bool enabled);
int petalarm_minutes(void); // local time of day, 0..1439 (midnight is valid)
void petalarm_set_minutes(int minutes);
unsigned petalarm_days(void);
void petalarm_set_days(unsigned days);
int petalarm_fade_seconds(void); // 0, 10, 30 or 60
void petalarm_set_fade_seconds(int seconds);
const char *petalarm_playlist(void); // empty = resume the current source
bool petalarm_set_playlist(const char *name); // false rather than truncate

// Nothing is scheduled until the clock is trustworthy and at least one day is
// selected. `now` is wall time; conversion to the device's local time zone is
// done here. The next deadline is cached, not recalculated on every tick.
// Returns a due deadline as well as future ones so a caller never suspends
// just as the alarm is about to be handled. 0 means there is no deadline.
bool petalarm_armed(bool clock_set);
time_t petalarm_next(time_t now, bool clock_set);

// Once per occurrence, even across a restart or the repeated hour of a DST
// transition. A late callback within PETALARM_LATE_SECS still rings; a large
// clock jump or a player started after the scheduled minute does not replay
// old alarms. Records the occurrence before returning true, so a crash does
// not ring twice.
bool petalarm_take_due(time_t now, bool clock_set);

#endif /* SYSTEM_PET_PETALARM_H */
