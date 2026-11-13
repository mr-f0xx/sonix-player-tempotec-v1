#ifndef YTPODCASTPLAYED_H
#define YTPODCASTPLAYED_H

#include <stdbool.h>

void ytpodcastplayed_init(void);
bool ytpodcastplayed_get(const char *video_id);
bool ytpodcastplayed_set(const char *video_id, bool played);

#endif
