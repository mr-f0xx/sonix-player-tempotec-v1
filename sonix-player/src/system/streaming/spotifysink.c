#include "spotifysink.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "src/system/audio/audio.h"
#include "src/system/core/config.h"
#include "src/system/core/utils.h"

// What Spotify's streams are: 44100 Hz stereo, and what librespot's pipe
// backend writes for us (`--format S16`). Decoded by the receiver, so this
// player only ever copies bytes.
#define SINK_RATE 44100
#define SINK_CHANNELS 2
#define SINK_FRAME_BYTES (SINK_CHANNELS * 2)

// The samples the receiver writes are 16-bit. If the card will not take them,
// 32-bit is tried as a fallback and the samples are widened on the way -- the
// same path the AirPlay receiver takes in the other order.
#define SINK_OUT_BITS 16

// How long the ring is: about 1.5 seconds at 44100 stereo. It is latency, not
// a buffer to hide a bad network in, so it stays small -- but it has to cover a
// track change (the receiver stops writing, decodes the next one, starts
// writing again) without the DAC falling silent in between.
#define SINK_RING_FRAMES (SINK_RATE * 3 / 2)
#define SINK_RING_BYTES (SINK_RING_FRAMES * SINK_FRAME_BYTES)

// One read from the FIFO and one write to the DAC.
#define SINK_CHUNK_BYTES 4096

// How long a push waits for room before dropping the oldest audio. Dropping is
// the last resort: it means the DAC has not taken a byte for this long.
#define SINK_RING_WAIT_MS 3000

// How often the threads look at the stop flag, and how long the reader waits
// for the receiver to open the FIFO again after it paused.
#define SINK_POLL_MS 200
#define SINK_REOPEN_MS 250

// Silence after which the DAC is handed back although nobody asked. Long enough
// to sit through the gap of a track change, short enough that a receiver paused
// for a minute is not holding the amplifier awake for nothing.
#define SINK_IDLE_HANDBACK_MS 3000

// The ALSA buffer under the ring. The ring is the slack in front of the card;
// this is what absorbs the interface thread not being scheduled.
#define SINK_ALSA_BUFFER_MS 200

// ---------------------------------------------------------------------------

static char sink_path[256];

static volatile bool stopping = true;
static volatile bool running;
static pthread_t reader_thread;
static pthread_t play_thread;
static bool reader_live;
static bool play_live;

static void (*pause_receiver_cb)(void);

// Audio arriving, and the DAC held for it. These are the two things the rest of
// the player asks about (the lock below is the one that protects them).
static pthread_mutex_t mark_lock = PTHREAD_MUTEX_INITIALIZER;
static int64_t last_audio_ms; // 0 until the first bytes ever arrive
static bool holds_output;

// Local playback asked for the DAC: read and cleared by the playback thread,
// which is the only thread allowed to close it.
static volatile bool release_requested;

// ---------------------------------------------------------------------------

static int64_t sink_now_ms(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void mark_audio(void) {
	pthread_mutex_lock(&mark_lock);
	last_audio_ms = sink_now_ms();
	pthread_mutex_unlock(&mark_lock);
}

int spotifysink_silent_ms(void) {
	// A sink that is not running is not listening to anything, however recently
	// it heard something: the last second before a stop is not playing.
	if (!running) {
		return -1;
	}

	pthread_mutex_lock(&mark_lock);
	int64_t last = last_audio_ms;
	pthread_mutex_unlock(&mark_lock);

	if (last == 0) {
		return -1; // nothing has ever arrived: an absence, not a silence
	}
	int64_t silent = sink_now_ms() - last;
	return silent > 0 ? (int)silent : 0;
}

bool spotifysink_playing(void) {
	if (!running) {
		return false;
	}
	int silent = spotifysink_silent_ms();
	return silent >= 0 && silent < 1000;
}

bool spotifysink_holds_output(void) {
	pthread_mutex_lock(&mark_lock);
	bool held = holds_output;
	pthread_mutex_unlock(&mark_lock);
	return held;
}

// ---------------------------------------------------------------------------
// the ring between the FIFO and the DAC
// ---------------------------------------------------------------------------

static uint8_t ring[SINK_RING_BYTES];
static size_t ring_read;
static size_t ring_fill;

// Both condvars are statically initialized, which fixes the clock their waits
// are read against at CLOCK_REALTIME (a condvar made with pthread_cond_init()
// and a NULL attribute gets the same one). So the deadlines below come from
// deadline_in_ms(), which is on that clock, and not from a helper of its own on
// CLOCK_MONOTONIC, which is what this file had: on a device whose wall clock
// says October 2026, "now plus 200 ms" read off the monotonic clock is a
// billion seconds in the past, every timed wait returns ETIMEDOUT on the spot,
// and the two threads that should have been asleep spin instead. The playback
// thread spins at real-time priority -- the kernel prints "sched: RT throttling
// activated", the interface is left with what the throttling leaves it, and the
// device freezes. Nothing on a development machine shows it: there the two
// clocks differ by uptime, and an uptime of a few hours is a deadline the
// condvar is still waiting for.
static pthread_mutex_t ring_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t ring_data = PTHREAD_COND_INITIALIZER;
static pthread_cond_t ring_space = PTHREAD_COND_INITIALIZER;
static size_t dropped_frames;

static void ring_reset(void) {
	pthread_mutex_lock(&ring_lock);
	ring_read = 0;
	ring_fill = 0;
	dropped_frames = 0;
	pthread_mutex_unlock(&ring_lock);
}

// Waits for room, and only stops waiting when the DAC has plainly stopped
// draining. This wait is what paces librespot: it blocks in write() to the FIFO
// while this is blocked here.
static void ring_push(const uint8_t *data, size_t n) {
	if (n > SINK_RING_BYTES) {
		data += n - SINK_RING_BYTES;
		n = SINK_RING_BYTES;
	}

	pthread_mutex_lock(&ring_lock);

	// On the condvar's own clock: see the note above ring_lock.
	struct timespec until;
	deadline_in_ms(&until, SINK_RING_WAIT_MS);
	while (n > SINK_RING_BYTES - ring_fill && !stopping) {
		if (pthread_cond_timedwait(&ring_space, &ring_lock, &until) == ETIMEDOUT) {
			break;
		}
	}

	if (n > SINK_RING_BYTES - ring_fill) {
		size_t drop = n - (SINK_RING_BYTES - ring_fill);
		// Whole frames only: an odd number of bytes would swap left and right
		// for the rest of the session.
		drop += (SINK_FRAME_BYTES - drop % SINK_FRAME_BYTES) % SINK_FRAME_BYTES;
		if (drop > ring_fill) {
			drop = ring_fill;
		}
		ring_read = (ring_read + drop) % SINK_RING_BYTES;
		ring_fill -= drop;
		dropped_frames += drop / SINK_FRAME_BYTES;
		printf("spotify: the output is behind; %u frames dropped so far\n", (unsigned)dropped_frames);
	}

	size_t write_at = (ring_read + ring_fill) % SINK_RING_BYTES;
	size_t first = SINK_RING_BYTES - write_at;
	if (first > n) {
		first = n;
	}
	memcpy(ring + write_at, data, first);
	if (n > first) {
		memcpy(ring, data + first, n - first);
	}
	ring_fill += n;

	pthread_cond_signal(&ring_data);
	pthread_mutex_unlock(&ring_lock);
}

// Whole frames only, up to `max`. Waits up to SINK_POLL_MS for something to
// arrive; returns 0 when nothing did.
static size_t ring_pop(uint8_t *out, size_t max) {
	pthread_mutex_lock(&ring_lock);

	if (ring_fill < SINK_FRAME_BYTES && !stopping) {
		struct timespec until;
		// On the condvar's own clock: see the note above ring_lock.
		deadline_in_ms(&until, SINK_POLL_MS);
		pthread_cond_timedwait(&ring_data, &ring_lock, &until);
	}

	size_t n = ring_fill < max ? ring_fill : max;
	n -= n % SINK_FRAME_BYTES;

	size_t first = SINK_RING_BYTES - ring_read;
	if (first > n) {
		first = n;
	}
	memcpy(out, ring + ring_read, first);
	if (n > first) {
		memcpy(out + first, ring, n - first);
	}
	ring_read = (ring_read + n) % SINK_RING_BYTES;
	ring_fill -= n;

	if (n > 0) {
		pthread_cond_signal(&ring_space);
	}
	pthread_mutex_unlock(&ring_lock);
	return n;
}

// ---------------------------------------------------------------------------
// the FIFO reader
//
// Its whole job is to never be busy with anything else: the receiver's
// back-pressure is this thread's timing, and a reader that stops to think is a
// receiver that stutters.
// ---------------------------------------------------------------------------

static void *reader_worker(void *unused) {
	(void)unused;

	// Ordinary priority on purpose. SCHED_IDLE would let the interface starve
	// this thread, and a reader that falls behind is a FIFO that fills up and
	// backs librespot off -- audible as a stutter rather than as slowness in
	// something nobody is watching. It sleeps in poll() the rest of the time.
	uint8_t buffer[SINK_CHUNK_BYTES];
	int fd = -1;

	while (!stopping) {
		if (fd < 0) {
			// The write end comes and goes: librespot opens it at each start
			// of playback and closes it when it pauses, so the read end is
			// reopened rather than kept across a session. O_NONBLOCK for the
			// open itself, so a FIFO is never a thread parked forever.
			fd = open(sink_path, O_RDONLY | O_NONBLOCK);
			if (fd < 0) {
				usleep(SINK_REOPEN_MS * 1000);
				continue;
			}
		}

		struct pollfd p = {.fd = fd, .events = POLLIN};
		int ready = poll(&p, 1, SINK_POLL_MS);

		if (stopping) {
			break;
		}
		if (ready <= 0) {
			continue;
		}
		if (p.revents & (POLLERR | POLLHUP | POLLNVAL)) {
			// Every writer is gone: the receiver paused, stopped, or is
			// between tracks with its sink closed. Close and wait for it.
			close(fd);
			fd = -1;
			usleep(SINK_REOPEN_MS * 1000);
			continue;
		}
		if (!(p.revents & POLLIN)) {
			continue;
		}

		ssize_t n = read(fd, buffer, sizeof(buffer));
		if (n > 0) {
			ring_push(buffer, (size_t)n);
			mark_audio();
		} else if (n == 0) {
			close(fd);
			fd = -1;
			usleep(SINK_REOPEN_MS * 1000);
		} else if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
			close(fd);
			fd = -1;
			usleep(SINK_REOPEN_MS * 1000);
		}
	}

	if (fd >= 0) {
		close(fd);
	}
	return NULL;
}

// ---------------------------------------------------------------------------
// the thread that feeds the DAC
// ---------------------------------------------------------------------------

static void *play_worker(void *unused) {
	(void)unused;

	// Real time, like the player's own playback thread and the AirPlay
	// receiver's: while the receiver plays, this thread IS the playback path,
	// and the only slack under it is the ring and the card's own buffer. It
	// cannot spin: it sleeps on the ring when there is nothing to play and in
	// the write when the card is full.
	//
	//   [spotify]
	//   rt_priority = 10   the playback thread's policy and priority
	//               = 0    no real-time at all
	int rt = (int)config_get_int("spotify", "rt_priority", 10);
	if (rt > 0) {
		thread_be_realtime("spotify sink", rt);
	}

	uint8_t chunk[SINK_CHUNK_BYTES];
	int32_t wide[SINK_CHUNK_BYTES / 2];
	bool device_open = false;
	bool widen = false;

	while (!stopping) {
		// Something local is starting and the device is being handed over: what
		// is still in the ring is a second and a half of a track that is not
		// going to be played, and holding it would make the handover wait for
		// the ring to drain.
		//
		// Answered here, before the pop, and not on the pass that finds nothing
		// to play: the reader can refill the ring between those two things --
		// it does so constantly while the receiver streams -- and the request
		// would then be handed to audio that was about to be dropped. A local
		// track would wait the sink's whole silence timeout for a device that
		// nobody is using.
		//
		// A request is answered by this block and by nothing else. It is only
		// ever made while the sink is holding the output (see
		// spotifysink_release_output), so with nothing open there is nothing to
		// hand back and the asker has already been told so.
		if (release_requested) {
			release_requested = false;
			ring_reset();
			if (device_open) {
				printf("spotify: output handed back (asked for)\n");
				audio_external_end();
				device_open = false;
				pthread_mutex_lock(&mark_lock);
				holds_output = false;
				pthread_mutex_unlock(&mark_lock);
				continue;
			}
		}

		size_t n = ring_pop(chunk, sizeof(chunk));

		if (n == 0) {
			int silent = spotifysink_silent_ms();
			bool idle = silent >= SINK_IDLE_HANDBACK_MS;

			if (device_open && (idle || stopping)) {
				printf("spotify: output handed back (%s)\n", stopping ? "stopping" : "silent");
				audio_external_end();
				device_open = false;
				pthread_mutex_lock(&mark_lock);
				holds_output = false;
				pthread_mutex_unlock(&mark_lock);
			}
			continue;
		}

		if (!device_open) {
			// One PCM, one writer. The phone can start casting while a local
			// track is playing, and that track has to give the device up --
			// the same thing the AirPlay receiver does the moment its first
			// bytes arrive.
			if (audio_get_status() == AUDIO_STATUS_PLAYING) {
				printf("spotify: playback started; pausing the local track\n");
				audio_pause();
			}

			widen = false;
			if (!audio_external_begin_latency(SINK_RATE, SINK_CHANNELS, SINK_OUT_BITS, SINK_ALSA_BUFFER_MS)) {
				// 32 bits, left-justified, is what the stock player asks for
				// and therefore the path this hardware is known to take. If
				// the card refuses the receiver's native 16, widen instead.
				if (audio_external_begin_latency(SINK_RATE, SINK_CHANNELS, 32, SINK_ALSA_BUFFER_MS)) {
					widen = true;
					printf("spotify: the output would not take 16 bits; widening to 32\n");
				} else {
					fprintf(stderr, "spotify: could not open the output; dropping what arrives\n");
					continue; // the FIFO carries on being drained regardless
				}
			}
			device_open = true;
			pthread_mutex_lock(&mark_lock);
			holds_output = true;
			pthread_mutex_unlock(&mark_lock);
		}

		int frames = (int)(n / SINK_FRAME_BYTES);
		if (widen) {
			const int16_t *src = (const int16_t *)(const void *)chunk;
			for (int i = 0; i < frames * SINK_CHANNELS; i++) {
				wide[i] = (int32_t)((uint32_t)(uint16_t)src[i] << 16);
			}
			audio_external_write(wide, frames);
		} else {
			audio_external_write(chunk, frames);
		}
	}

	if (device_open) {
		audio_external_end();
	}
	pthread_mutex_lock(&mark_lock);
	holds_output = false;
	pthread_mutex_unlock(&mark_lock);
	return NULL;
}

// ---------------------------------------------------------------------------
// starting and stopping
// ---------------------------------------------------------------------------

bool spotifysink_start(const char *fifo_path, void (*pause_receiver)(void)) {
	if (running) {
		return true;
	}
	if (!fifo_path || !fifo_path[0]) {
		return false;
	}

	snprintf(sink_path, sizeof(sink_path), "%s", fifo_path);
	pause_receiver_cb = pause_receiver;

	// The FIFO first: librespot opens the write end when playback starts, and
	// opening a FIFO for writing with no reader on the other side blocks until
	// there is one. The reader is opened below, before the receiver is spawned.
	unlink(sink_path);
	if (mkfifo(sink_path, 0666) != 0) {
		fprintf(stderr, "spotify: mkfifo(%s): %s\n", sink_path, strerror(errno));
		return false;
	}
	// The receiver runs as the player's own user, so the mode mkfifo() left is
	// already the one that matters; widening it past the process's umask is
	// what keeps a receiver started another way able to open it, and a
	// filesystem that refuses is worth a line rather than a failed sink.
	if (chmod(sink_path, 0666) != 0) {
		fprintf(stderr, "spotify: chmod(%s): %s\n", sink_path, strerror(errno));
	}

	pthread_mutex_lock(&mark_lock);
	last_audio_ms = 0;
	holds_output = false;
	pthread_mutex_unlock(&mark_lock);

	ring_reset();
	release_requested = false;
	stopping = false;

	reader_live = pthread_create(&reader_thread, NULL, reader_worker, NULL) == 0;
	play_live = pthread_create(&play_thread, NULL, play_worker, NULL) == 0;
	if (!reader_live || !play_live) {
		fprintf(stderr, "spotify: could not start the sink threads\n");
		stopping = true;
		pthread_mutex_lock(&ring_lock);
		pthread_cond_broadcast(&ring_data);
		pthread_cond_broadcast(&ring_space);
		pthread_mutex_unlock(&ring_lock);
		if (reader_live) {
			pthread_join(reader_thread, NULL);
			reader_live = false;
		}
		if (play_live) {
			pthread_join(play_thread, NULL);
			play_live = false;
		}
		unlink(sink_path);
		return false;
	}

	running = true;
	return true;
}

void spotifysink_stop(void) {
	if (!running && !reader_live && !play_live) {
		return;
	}

	stopping = true;

	// Out of both waits: the reader from its poll and the playback thread from
	// the ring, so neither has to run out its timer.
	pthread_mutex_lock(&ring_lock);
	pthread_cond_broadcast(&ring_data);
	pthread_cond_broadcast(&ring_space);
	pthread_mutex_unlock(&ring_lock);

	if (reader_live) {
		pthread_join(reader_thread, NULL);
		reader_live = false;
	}
	if (play_live) {
		pthread_join(play_thread, NULL);
		play_live = false;
	}

	// Belt and braces: the playback thread hands it back on its way out, but a
	// thread that never started would leave the DAC held open.
	audio_external_end();

	pthread_mutex_lock(&mark_lock);
	holds_output = false;
	pthread_mutex_unlock(&mark_lock);

	unlink(sink_path);
	ring_reset();
	running = false;
}

bool spotifysink_release_output(void) {
	if (!running || !spotifysink_holds_output()) {
		return true; // nothing is holding it
	}

	// Tell the phone, too. A DAC handed over under a playing librespot is a
	// session that keeps its position and its queue but stops making sound --
	// the same thing the local player does when it is the one giving way.
	if (pause_receiver_cb) {
		pause_receiver_cb();
	}

	pthread_mutex_lock(&ring_lock);
	release_requested = true;
	pthread_cond_broadcast(&ring_data); // out of its wait, so it lets go now
	pthread_mutex_unlock(&ring_lock);
	return true;
}
