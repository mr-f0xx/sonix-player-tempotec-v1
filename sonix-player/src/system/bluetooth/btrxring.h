#ifndef BTRXRING_H
#define BTRXRING_H

#include <stdbool.h>
#include <stdint.h>

// The buffer between what arrives over Bluetooth and what the DAC plays.
//
// In receiver mode two clocks are running that nobody has tied together: the
// sending phone's audio clock, which decides how fast PCM is decoded, and the
// CS43198's own oscillator, which decides how fast it is played. There is no
// resampler anywhere in the path, and there cannot be a useful one -- the
// sender chose the rate and this device must play it at that rate.
//
// Until this existed, the capture thread read bluealsa and wrote the DAC with
// the same call, so the two clocks met head on inside a 120 ms ALSA buffer and
// a 64 KiB FIFO in the daemon. Whenever the DAC asked for frames the sender had
// not produced yet -- a bursty RTP delivery, a Wi-Fi scan on the shared
// antenna, the interface redrawing the panel -- the DAC ran dry and the
// listener heard a stutter; whenever the reverse happened the daemon's FIFO
// filled and bluealsa threw decoded frames away. Both are audible, and for
// LDAC the first one is worse than audible: a sink that does not drain keeps
// retransmissions piling up in the phone's socket, and the phone's adaptive bit
// rate answers that by dropping to a lower LDAC quality. That is what "the
// sound gets worse for a while" is.
//
// So the two are separated by this: a ring of decoded frames, filled by the
// thread that drains bluealsa and emptied by a thread whose only job is to keep
// the DAC fed. The ring's occupancy is the amount of slack the link is allowed
// to jitter by, and it is reported in the log so the next report can say which
// side of the path is short of time.
//
// One producer thread and one consumer thread; the mutex inside is for the
// handover and for the counters, not for throughput.

typedef struct btrxring btrxring_t;

// `target_ms` is the depth kept in hand before the DAC is first fed -- the
// slack for a late or bursty delivery. `capacity_ms` is the hard ceiling and
// must be comfortably larger. Both are rounded up to whole frames, and both
// are clamped: below a few milliseconds there is nothing to absorb, and above
// a quarter of a second a receiver is adding a delay the sender's own
// controls (and any video on the phone) will not thank it for.
//
// NULL when the buffer cannot be allocated, which is a reason to refuse the
// session rather than to play without one.
btrxring_t *btrxring_create(unsigned rate, unsigned frame_bytes, unsigned target_ms, unsigned capacity_ms);
void btrxring_destroy(btrxring_t *ring);

// Frames the ring can take right now, and frames it is holding.
unsigned btrxring_room(btrxring_t *ring);
unsigned btrxring_fill(btrxring_t *ring);

// Writes up to `count` frames and returns how many were taken, which is less
// than `count` only when the ring is full. The caller keeps the remainder.
unsigned btrxring_push(btrxring_t *ring, const void *frames, unsigned count);

// Reads up to `count` frames into `out` and returns how many were there.
unsigned btrxring_pop(btrxring_t *ring, void *out, unsigned count);

// Throws away what is queued. For a session that is being restarted.
void btrxring_clear(btrxring_t *ring);

// Waits until the ring has changed since `serial` was read (see
// btrxring_serial()), or the timeout runs out. Returns the serial to pass next
// time. Waiting on a change rather than on a level is what keeps a spurious
// wake-up from turning into a spin on the one core, the same rule the rest of
// the Bluetooth path follows.
unsigned btrxring_serial(btrxring_t *ring);
void btrxring_wait(btrxring_t *ring, unsigned serial, int timeout_ms);

// Frames the ring was built to hold. For a caller that has to bound a read by
// what could ever fit, rather than by what happens to be free.
unsigned btrxring_capacity_frames(btrxring_t *ring);

// True once the ring holds the depth it was asked to keep in hand. Until then
// the consumer must not take anything: that wait is what turns the start of a
// session into one clean delay instead of a run of underruns while the phone
// fills the pipe.
bool btrxring_ready(btrxring_t *ring);

// Milliseconds the ring's current contents represent, for the log.
unsigned btrxring_fill_ms(btrxring_t *ring);
unsigned btrxring_target_ms(btrxring_t *ring);
unsigned btrxring_capacity_ms(btrxring_t *ring);

// The most the ring has held at once since it was made, in frames: how close
// the session came to a hard ceiling that has no good answer (dropping frames
// there is exactly what the buffer exists to avoid).
unsigned btrxring_high_water(btrxring_t *ring);

#endif /* BTRXRING_H */
