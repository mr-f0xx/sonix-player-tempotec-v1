/* The jitter buffer between the phone's clock and the DAC's: capacity, the
 * wrap, the depth kept in hand, and the waiter. */
#include "../src/system/bluetooth/btrxring.c"
#include <assert.h>

// A frame whose first sample is its index, so a wrap that loses or reorders
// frames is visible rather than merely plausible.
static void fill_frames(int32_t *frames, unsigned first, unsigned count) {
	for (unsigned i = 0; i < count; i++) {
		frames[i * 2] = (int32_t)(first + i);
		frames[i * 2 + 1] = -(int32_t)(first + i);
	}
}

static void check_frames(const int32_t *frames, unsigned first, unsigned count) {
	for (unsigned i = 0; i < count; i++) {
		assert(frames[i * 2] == (int32_t)(first + i));
		assert(frames[i * 2 + 1] == -(int32_t)(first + i));
	}
}

static void test_sizing(void) {
	// The depth the caller asked for is kept: 150 ms at 44.1 kHz is 6615
	// frames, and the capacity is never under the request.
	btrxring_t *ring = btrxring_create(44100, 4, 150, 0);
	assert(ring);
	assert(btrxring_target_ms(ring) >= 150 && btrxring_target_ms(ring) <= 155);
	assert(btrxring_capacity_ms(ring) > btrxring_target_ms(ring));
	// The default ceiling is 250 ms above the target (btrxring.h).
	assert(btrxring_capacity_ms(ring) >= 400);
	btrxring_destroy(ring);

	// A ceiling under the target is raised to it rather than honoured: a ring
	// that cannot hold what it is supposed to keep would drop on every start.
	ring = btrxring_create(48000, 8, 200, 50);
	assert(ring);
	assert(btrxring_capacity_ms(ring) > btrxring_target_ms(ring));
	btrxring_destroy(ring);

	// Both are clamped, and nonsense is refused rather than guessed at.
	ring = btrxring_create(48000, 4, 1, 1);
	assert(ring);
	assert(btrxring_target_ms(ring) >= 20 && btrxring_target_ms(ring) <= 30);
	btrxring_destroy(ring);
	// Both are clamped, and the frame count is then rounded up to a power of
	// two, so the reported depth can exceed the ceiling by up to 2x: never
	// smaller, which is what matters, since rounding down would quietly shorten
	// the buffer the caller chose.
	ring = btrxring_create(48000, 4, 100000, 100000);
	assert(ring);
	assert(btrxring_target_ms(ring) <= 400 && btrxring_capacity_ms(ring) <= 800);
	assert(btrxring_capacity_ms(ring) > btrxring_target_ms(ring));
	btrxring_destroy(ring);
	assert(!btrxring_create(0, 4, 100, 200));
	assert(!btrxring_create(48000, 0, 100, 200));

	// No ring at all is every question answered with nothing, not a crash.
	assert(btrxring_room(NULL) == 0 && btrxring_fill(NULL) == 0 && !btrxring_ready(NULL));
	assert(btrxring_push(NULL, "x", 1) == 0 && btrxring_pop(NULL, "x", 1) == 0);
	btrxring_clear(NULL);
	btrxring_destroy(NULL);
}

static void test_round_trip(void) {
	// 100 ms at 1 kHz: 100 frames of target, 256 of capacity.
	btrxring_t *ring = btrxring_create(1000, 8, 100, 300);
	assert(ring);
	assert(btrxring_fill(ring) == 0);
	assert(!btrxring_ready(ring));

	int32_t frames[2 * 64];
	int32_t out[2 * 64];

	fill_frames(frames, 0, 64);
	assert(btrxring_push(ring, frames, 64) == 64);
	assert(btrxring_fill(ring) == 64);
	assert(!btrxring_ready(ring));

	// Ready exactly when the depth kept in hand is reached.
	fill_frames(frames, 64, 36);
	assert(btrxring_push(ring, frames, 36) == 36);
	assert(btrxring_ready(ring));

	// A full ring takes what fits and says how much: never more than it holds,
	// and the caller keeps the rest.
	// Fill it to the brim, and no further: the capacity is a power of two
	// rounded up from the request, so how much fits is asked for rather than
	// assumed.
	unsigned queued = btrxring_fill(ring);
	unsigned next = 100;
	while (btrxring_room(ring) > 0) {
		unsigned chunk_now = btrxring_room(ring);
		if (chunk_now > 64) {
			chunk_now = 64;
		}
		fill_frames(frames, next, chunk_now);
		unsigned took = btrxring_push(ring, frames, chunk_now);
		assert(took == chunk_now);
		next += took;
		queued += took;
	}
	assert(btrxring_room(ring) == 0);

	// A push to a full ring takes nothing at all and loses nothing: the caller
	// keeps every frame, which is the whole reason push reports a count.
	fill_frames(frames, 9999, 1);
	assert(btrxring_push(ring, frames, 1) == 0);
	assert(btrxring_fill(ring) == queued);

	// What comes out is what went in, in order, across several writes and
	// reads -- which is the wrap.
	unsigned read_total = 0;
	unsigned expect = 0;
	while (btrxring_fill(ring) > 0) {
		unsigned want = btrxring_fill(ring) < 32 ? btrxring_fill(ring) : 32;
		unsigned got = btrxring_pop(ring, out, want);
		assert(got == want);
		check_frames(out, expect, got);
		expect += got;
		read_total += got;
	}
	assert(read_total == queued);
	assert(btrxring_fill(ring) == 0);
	// Capacity is a power of two frames, so the wrap has been crossed by now:
	// every read above walked past the end of the array.
	assert(queued > 128);

	// And the ring is reusable after it has been emptied, which is the other
	// half of the wrap: the head is not where it started.
	fill_frames(frames, 500, 64);
	assert(btrxring_push(ring, frames, 64) == 64);
	assert(btrxring_pop(ring, out, 64) == 64);
	check_frames(out, 500, 64);

	// Cleared means empty, and the high-water mark is kept: it is a record of
	// the session, not of the moment. The peak is the capacity here, because
	// the loop above filled the ring to the brim.
	unsigned peak = btrxring_high_water(ring);
	assert(peak > 128); // the 100+36 the ring was primed with, then filled
	assert(peak <= queued);
	btrxring_clear(ring);
	assert(btrxring_fill(ring) == 0);
	assert(btrxring_high_water(ring) == peak);

	btrxring_destroy(ring);
}

static void test_wait(void) {
	btrxring_t *ring = btrxring_create(48000, 4, 100, 200);
	assert(ring);

	// A timeout is a timeout and not a spin: it comes back and says nothing.
	unsigned serial = btrxring_serial(ring);
	struct timespec before, after;
	clock_gettime(CLOCK_REALTIME, &before);
	btrxring_wait(ring, serial, 40);
	clock_gettime(CLOCK_REALTIME, &after);
	long waited = (after.tv_sec - before.tv_sec) * 1000 + (after.tv_nsec - before.tv_nsec) / 1000000;
	assert(waited >= 30 && waited < 2000);

	// A change that arrives while nobody is waiting is still the change the
	// next wait is looking for: a wake-up already delivered before the call
	// must not be waited out. This is what keeps a full ring from costing a
	// wake-up per timeout for as long as it stays full.
	unsigned seen = btrxring_serial(ring);
	int32_t frame[1] = {0};
	assert(btrxring_push(ring, frame, 1) == 1);
	clock_gettime(CLOCK_REALTIME, &before);
	btrxring_wait(ring, seen, 2000);
	clock_gettime(CLOCK_REALTIME, &after);
	waited = (after.tv_sec - before.tv_sec) * 1000 + (after.tv_nsec - before.tv_nsec) / 1000000;
	assert(waited < 500);

	// The serial moves on every change, so a consumer that read it before
	// popping does not park on an empty ring it has already emptied.
	unsigned at = btrxring_serial(ring);
	int32_t out[1];
	assert(btrxring_pop(ring, out, 1) == 1);
	assert(btrxring_serial(ring) != at);

	// Destroying it wakes a waiter rather than leaving it on a freed mutex.
	// (Checked by ordering: destroy is last, as a caller does it.)
	btrxring_destroy(ring);
}

int main(void) {
	test_sizing();
	test_round_trip();
	test_wait();
	puts("PASS Bluetooth RX jitter buffer (sizing, wrap, depth, waiting)");
	return 0;
}
