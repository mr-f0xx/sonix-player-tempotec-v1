#include "btrxring.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

// Bounds on the depth, in milliseconds. The floor is a few periods' worth at
// any rate this path sees; the ceiling is where a receiver starts to be felt
// as a delay rather than heard as a buffer.
#define TARGET_MS_MIN 20
#define CAPACITY_MS_MIN 40
#define CAPACITY_MS_MAX 400
// How far ahead of the target the ceiling is kept when the caller did not say
// otherwise: room for four or five seconds' worth of drift at any plausible
// pair of clocks, and nothing like enough to matter for lip sync.
#define CAPACITY_MS_HEADROOM 250

struct btrxring {
	pthread_mutex_t lock;
	pthread_cond_t changed;

	unsigned char *data;
	unsigned capacity; // frames
	unsigned target;   // frames
	unsigned head;     // read cursor, in frames
	unsigned fill;     // frames queued
	unsigned high_water;

	unsigned rate;
	unsigned frame_bytes;

	unsigned serial; // bumped on every change, for the waiter
	bool closed;
};

static unsigned ms_to_frames(unsigned rate, unsigned ms) {
	if (rate == 0) {
		return 0;
	}
	return (unsigned)(((unsigned long long)rate * ms) / 1000u);
}

static unsigned frames_to_ms(unsigned rate, unsigned frames) {
	if (rate == 0) {
		return 0;
	}
	return (unsigned)(((unsigned long long)frames * 1000u) / rate);
}

static unsigned round_up_pow2(unsigned value) {
	unsigned n = 1;
	while (n < value && n < 0x80000000u) {
		n <<= 1;
	}
	return n;
}

btrxring_t *btrxring_create(unsigned rate, unsigned frame_bytes, unsigned target_ms, unsigned capacity_ms) {
	if (rate == 0 || frame_bytes == 0) {
		return NULL;
	}

	if (target_ms < TARGET_MS_MIN) {
		target_ms = TARGET_MS_MIN;
	}
	// The depth kept in hand is bounded by the same ceiling as the ring, so a
	// caller asking for more delay than the buffer may ever hold gets the
	// ceiling rather than a ring that can never be primed.
	if (target_ms > CAPACITY_MS_MAX - TARGET_MS_MIN) {
		target_ms = CAPACITY_MS_MAX - TARGET_MS_MIN;
	}
	// The ceiling is never allowed under the target: a ceiling beneath the
	// depth the ring is supposed to hold would mean the first burst after the
	// start is already a drop.
	if (capacity_ms == 0) {
		capacity_ms = target_ms + CAPACITY_MS_HEADROOM;
	}
	if (capacity_ms < target_ms + TARGET_MS_MIN) {
		capacity_ms = target_ms + TARGET_MS_MIN;
	}
	if (capacity_ms < CAPACITY_MS_MIN) {
		capacity_ms = CAPACITY_MS_MIN;
	}
	if (capacity_ms > CAPACITY_MS_MAX) {
		capacity_ms = CAPACITY_MS_MAX;
	}

	unsigned target = ms_to_frames(rate, target_ms);
	unsigned capacity = ms_to_frames(rate, capacity_ms);
	if (target < 1) {
		target = 1;
	}
	if (capacity <= target) {
		capacity = target * 2;
	}
	// A power of two so the wrap is a mask rather than a division, on a core
	// that is also running an LVGL paint and a codec decode. Never smaller than
	// what was asked for: rounding down would quietly shorten the buffer the
	// caller chose.
	capacity = round_up_pow2(capacity);

	if ((size_t)capacity > SIZE_MAX / frame_bytes) {
		return NULL;
	}

	btrxring_t *ring = calloc(1, sizeof(*ring));
	if (!ring) {
		return NULL;
	}
	ring->data = malloc((size_t)capacity * frame_bytes);
	if (!ring->data) {
		free(ring);
		return NULL;
	}
	pthread_mutex_init(&ring->lock, NULL);
	// The default clock, as everywhere else in this player (deadline_in_ms()):
	// asking for the monotonic one is a condition variable attribute some
	// libcs accept and then ignore, and an ignored one makes every timed wait
	// return at once -- a spin, which is the failure this whole file is here to
	// avoid.
	pthread_cond_init(&ring->changed, NULL);
	ring->capacity = capacity;
	ring->target = target;
	ring->rate = rate;
	ring->frame_bytes = frame_bytes;
	return ring;
}

void btrxring_destroy(btrxring_t *ring) {
	if (!ring) {
		return;
	}
	// Wakes a consumer parked in btrxring_wait() so it can notice the session
	// is over, before the memory it is waiting on goes away. The caller owns the
	// join; this only gets it moving.
	pthread_mutex_lock(&ring->lock);
	ring->closed = true;
	ring->serial++;
	pthread_cond_broadcast(&ring->changed);
	pthread_mutex_unlock(&ring->lock);

	pthread_mutex_destroy(&ring->lock);
	pthread_cond_destroy(&ring->changed);
	free(ring->data);
	free(ring);
}

void btrxring_clear(btrxring_t *ring) {
	if (!ring) {
		return;
	}
	pthread_mutex_lock(&ring->lock);
	ring->head = 0;
	ring->fill = 0;
	ring->serial++;
	pthread_cond_broadcast(&ring->changed);
	pthread_mutex_unlock(&ring->lock);
}

unsigned btrxring_room(btrxring_t *ring) {
	if (!ring) {
		return 0;
	}
	pthread_mutex_lock(&ring->lock);
	unsigned room = ring->capacity - ring->fill;
	pthread_mutex_unlock(&ring->lock);
	return room;
}

unsigned btrxring_fill(btrxring_t *ring) {
	if (!ring) {
		return 0;
	}
	pthread_mutex_lock(&ring->lock);
	unsigned fill = ring->fill;
	pthread_mutex_unlock(&ring->lock);
	return fill;
}

unsigned btrxring_push(btrxring_t *ring, const void *frames, unsigned count) {
	if (!ring || !frames || count == 0) {
		return 0;
	}

	pthread_mutex_lock(&ring->lock);
	unsigned room = ring->capacity - ring->fill;
	unsigned take = count < room ? count : room;

	unsigned tail = (ring->head + ring->fill) & (ring->capacity - 1);
	// Two pieces when the write wraps; the queue is a power of two so one mask
	// does the arithmetic.
	unsigned first = ring->capacity - tail;
	if (first > take) {
		first = take;
	}
	const unsigned char *src = frames;
	memcpy(ring->data + (size_t)tail * ring->frame_bytes, src, (size_t)first * ring->frame_bytes);
	if (take > first) {
		memcpy(ring->data, src + (size_t)first * ring->frame_bytes, (size_t)(take - first) * ring->frame_bytes);
	}
	ring->fill += take;
	if (ring->fill > ring->high_water) {
		ring->high_water = ring->fill;
	}
	ring->serial++;
	pthread_cond_broadcast(&ring->changed);
	pthread_mutex_unlock(&ring->lock);
	return take;
}

unsigned btrxring_pop(btrxring_t *ring, void *out, unsigned count) {
	if (!ring || !out || count == 0) {
		return 0;
	}

	pthread_mutex_lock(&ring->lock);
	unsigned take = count < ring->fill ? count : ring->fill;
	if (take > 0) {
		unsigned first = ring->capacity - ring->head;
		if (first > take) {
			first = take;
		}
		unsigned char *dst = out;
		memcpy(dst, ring->data + (size_t)ring->head * ring->frame_bytes, (size_t)first * ring->frame_bytes);
		if (take > first) {
			memcpy(dst + (size_t)first * ring->frame_bytes, ring->data, (size_t)(take - first) * ring->frame_bytes);
		}
		ring->head = (ring->head + take) & (ring->capacity - 1);
		ring->fill -= take;
		ring->serial++;
	}
	pthread_mutex_unlock(&ring->lock);
	return take;
}

unsigned btrxring_serial(btrxring_t *ring) {
	if (!ring) {
		return 0;
	}
	pthread_mutex_lock(&ring->lock);
	unsigned serial = ring->serial;
	pthread_mutex_unlock(&ring->lock);
	return serial;
}

void btrxring_wait(btrxring_t *ring, unsigned serial, int timeout_ms) {
	if (!ring) {
		return;
	}
	if (timeout_ms < 0) {
		timeout_ms = 0;
	}

	struct timespec until;
	clock_gettime(CLOCK_REALTIME, &until);
	long long ns = (long long)until.tv_nsec + (long long)timeout_ms * 1000000LL;
	until.tv_sec += (time_t)(ns / 1000000000LL);
	until.tv_nsec = (long)(ns % 1000000000LL);

	pthread_mutex_lock(&ring->lock);
	// Re-checked against the serial the caller saw rather than against a flag:
	// a wake the producer already made before this call is the very change
	// being waited for, so it must not be waited out.
	while (!ring->closed && ring->serial == serial) {
		if (pthread_cond_timedwait(&ring->changed, &ring->lock, &until) != 0) {
			break;
		}
	}
	pthread_mutex_unlock(&ring->lock);
}

unsigned btrxring_capacity_frames(btrxring_t *ring) {
	if (!ring) {
		return 0;
	}
	pthread_mutex_lock(&ring->lock);
	unsigned frames = ring->capacity;
	pthread_mutex_unlock(&ring->lock);
	return frames;
}

bool btrxring_ready(btrxring_t *ring) {
	if (!ring) {
		return false;
	}
	pthread_mutex_lock(&ring->lock);
	bool ready = ring->fill >= ring->target;
	pthread_mutex_unlock(&ring->lock);
	return ready;
}

unsigned btrxring_fill_ms(btrxring_t *ring) {
	if (!ring) {
		return 0;
	}
	pthread_mutex_lock(&ring->lock);
	unsigned ms = frames_to_ms(ring->rate, ring->fill);
	pthread_mutex_unlock(&ring->lock);
	return ms;
}

unsigned btrxring_target_ms(btrxring_t *ring) {
	if (!ring) {
		return 0;
	}
	pthread_mutex_lock(&ring->lock);
	unsigned ms = frames_to_ms(ring->rate, ring->target);
	pthread_mutex_unlock(&ring->lock);
	return ms;
}

unsigned btrxring_capacity_ms(btrxring_t *ring) {
	if (!ring) {
		return 0;
	}
	pthread_mutex_lock(&ring->lock);
	unsigned ms = frames_to_ms(ring->rate, ring->capacity);
	pthread_mutex_unlock(&ring->lock);
	return ms;
}

unsigned btrxring_high_water(btrxring_t *ring) {
	if (!ring) {
		return 0;
	}
	pthread_mutex_lock(&ring->lock);
	unsigned high = ring->high_water;
	pthread_mutex_unlock(&ring->lock);
	return high;
}
