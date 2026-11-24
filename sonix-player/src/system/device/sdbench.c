#define _GNU_SOURCE // O_DIRECT, posix_fadvise, pread on the 32-bit target

#include "sdbench.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/fs.h> // BLKGETSIZE64
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

// A guard for a broken clock or a device that answers without time passing:
// neither may turn a bounded five-second job into an endless loop.
#define MAX_PHASE_READS 200000u
#define PROGRESS_INTERVAL_US 100000u
#define PROBE_COUNT 5

typedef struct {
	int fd;
	bool direct;
} sdbench_file_t;

typedef struct {
	const sdbench_io_t *io;
	void *handle;
	void *buffer;
	sdbench_result_t *r;
	sdbench_progress_cb progress;
	void *user;
	uint64_t start_us;
	uint64_t last_publish_us;
} run_t;

// ---------------------------------------------------------------------------
// the real device
// ---------------------------------------------------------------------------

static void *default_open(const char *device, uint64_t *size_bytes, bool *direct, void *user) {
	(void)user;
	if (!device || !device[0]) {
		return NULL;
	}

	*size_bytes = 0;
	*direct = false;

	int fd = -1;
#ifdef O_DIRECT
	// O_DIRECT is the difference between measuring the card and measuring the
	// page cache. Some kernels accept the open flag but reject the first read
	// (EINVAL) on a particular block device, so prove it with one aligned 4K
	// read before committing to it. The read is read-only and out of the timed
	// phases.
	int direct_fd = open(device, O_RDONLY | O_CLOEXEC | O_DIRECT);
	if (direct_fd >= 0) {
		void *probe = NULL;
		if (posix_memalign(&probe, SDBENCH_BLOCK_BYTES, SDBENCH_BLOCK_BYTES) == 0) {
			ssize_t n = pread(direct_fd, probe, SDBENCH_BLOCK_BYTES, 0);
			free(probe);
			if (n == (ssize_t)SDBENCH_BLOCK_BYTES) {
				fd = direct_fd;
				*direct = true;
			}
		}
		if (fd < 0) {
			close(direct_fd);
		}
	}
#endif
	if (fd < 0) {
		fd = open(device, O_RDONLY | O_CLOEXEC);
		if (fd < 0) {
			return NULL;
		}
		// No readahead in the fallback: a 4K random benchmark that quietly
		// reads 128K ahead is measuring the wrong thing.
		posix_fadvise(fd, 0, 0, POSIX_FADV_RANDOM);
	}

	sdbench_file_t *file = malloc(sizeof(*file));
	if (!file) {
		close(fd);
		return NULL;
	}
	file->fd = fd;
	file->direct = *direct;

	uint64_t size = 0;
	if (ioctl(fd, BLKGETSIZE64, &size) != 0 || size == 0) {
		off_t end = lseek(fd, 0, SEEK_END);
		if (end > 0) {
			size = (uint64_t)end;
		}
	}
	*size_bytes = size;
	return file;
}

static bool default_read(void *handle, uint64_t offset, void *buffer, size_t bytes, void *user) {
	(void)user;
	sdbench_file_t *file = (sdbench_file_t *)handle;
	size_t done = 0;
	while (done < bytes) {
		ssize_t n = pread(file->fd, (char *)buffer + done, bytes - done, (off_t)(offset + done));
		if (n < 0) {
			if (errno == EINTR) {
				continue;
			}
			return false;
		}
		if (n == 0) {
			return false; // short read: the card stopped answering
		}
		done += (size_t)n;
	}

	// Buffered reads must not be allowed to turn into a RAM benchmark on the
	// second visit to a block. O_DIRECT paths need no eviction.
	if (!file->direct) {
		posix_fadvise(file->fd, (off_t)offset, (off_t)bytes, POSIX_FADV_DONTNEED);
	}
	return true;
}

static void default_close(void *handle, void *user) {
	(void)user;
	sdbench_file_t *file = (sdbench_file_t *)handle;
	if (!file) {
		return;
	}
	close(file->fd);
	free(file);
}

static uint64_t default_now_us(void *user) {
	(void)user;
	struct timespec ts;
	if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
		return 0;
	}
	return (uint64_t)ts.tv_sec * 1000000ull + (uint64_t)ts.tv_nsec / 1000ull;
}

static bool default_cancelled(void *user) {
	if (!user) {
		return false;
	}
	// Plain bool + GCC atomics, the same pattern ota.c uses for its cancel
	// flag. The page stores, the runner loads between reads.
	return __atomic_load_n((bool *)user, __ATOMIC_RELAXED);
}

void sdbench_default_io(sdbench_io_t *io, bool *cancel) {
	if (!io) {
		return;
	}
	memset(io, 0, sizeof(*io));
	io->open = default_open;
	io->read = default_read;
	io->close = default_close;
	io->now_us = default_now_us;
	io->cancelled = default_cancelled;
	io->user = cancel;
}

// ---------------------------------------------------------------------------
// the run
// ---------------------------------------------------------------------------

static void result_defaults(sdbench_result_t *r) {
	memset(r, 0, sizeof(*r));
	r->status = SDBENCH_NO_CARD;
	r->phase = SDBENCH_PHASE_IDLE;
	r->verdict = SDBENCH_VERDICT_INCOMPLETE;
	r->first_error_offset = SDBENCH_NO_OFFSET;
	r->first_slow_offset = SDBENCH_NO_OFFSET;
	r->first_repeat_offset = SDBENCH_NO_OFFSET;
}

static uint64_t now_us(const run_t *s) { return s->io->now_us(s->io->user); }

static bool cancelled(const run_t *s) {
	return s->io->cancelled && s->io->cancelled(s->io->user);
}

static void publish_progress(run_t *s, uint8_t progress, bool force) {
	uint64_t now = now_us(s);
	if (now < s->start_us) {
		now = s->start_us; // a clock that jumps backwards must not wrap
	}
	s->r->elapsed_ms = (uint32_t)((now - s->start_us) / 1000ull);
	s->r->progress = progress > 100 ? 100 : progress;

	bool due = force || now < s->last_publish_us ||
			   now - s->last_publish_us >= PROGRESS_INTERVAL_US;
	if (due) {
		s->last_publish_us = now;
		if (s->progress) {
			s->progress(s->r, s->user);
		}
	}
}

static bool cancel_run(run_t *s) {
	s->r->status = SDBENCH_CANCELLED;
	s->r->phase = SDBENCH_PHASE_DONE;
	s->r->verdict = SDBENCH_VERDICT_INCOMPLETE;
	publish_progress(s, s->r->progress, true);
	return false;
}

static uint32_t block_hash(const void *buffer) {
	const unsigned char *p = (const unsigned char *)buffer;
	uint32_t h = 2166136261u;
	for (size_t i = 0; i < SDBENCH_BLOCK_BYTES; i++) {
		h ^= p[i];
		h *= 16777619u;
	}
	return h;
}

// All-zero, all-0xFF and other uniform fillers are common on an empty card.
// They say nothing about address wrapping, so only varied blocks are compared.
static bool block_uniform(const void *buffer) {
	const unsigned char *p = (const unsigned char *)buffer;
	const unsigned char first = p[0];
	for (size_t i = 1; i < SDBENCH_BLOCK_BYTES; i++) {
		if (p[i] != first) {
			return false;
		}
	}
	return true;
}

static uint32_t rng_next(uint32_t *state) {
	uint32_t x = *state;
	x ^= x << 13;
	x ^= x >> 17;
	x ^= x << 5;
	*state = x;
	return x;
}

static uint64_t gcd64(uint64_t a, uint64_t b) {
	while (b) {
		uint64_t t = a % b;
		a = b;
		b = t;
	}
	return a;
}

static bool read_block(run_t *s, uint64_t block, uint64_t *phase_bytes, uint32_t *phase_reads) {
	uint64_t offset = block * (uint64_t)SDBENCH_BLOCK_BYTES;
	uint64_t before = now_us(s);
	bool ok = s->io->read(s->handle, offset, s->buffer, SDBENCH_BLOCK_BYTES, s->io->user);
	uint64_t after = now_us(s);
	uint64_t latency = after >= before ? after - before : 0;

	if (ok) {
		if (phase_bytes) {
			*phase_bytes += SDBENCH_BLOCK_BYTES;
		}
		if (phase_reads) {
			(*phase_reads)++;
		}
	} else {
		s->r->read_errors++;
		if (s->r->first_error_offset == SDBENCH_NO_OFFSET) {
			s->r->first_error_offset = offset;
		}
	}

	if (latency >= SDBENCH_SLOW_READ_US) {
		s->r->slow_reads++;
		if (s->r->first_slow_offset == SDBENCH_NO_OFFSET) {
			s->r->first_slow_offset = offset;
		}
	}
	uint32_t latency_ms = (uint32_t)(latency / 1000ull);
	if (latency_ms > s->r->max_read_ms) {
		s->r->max_read_ms = latency_ms;
	}
	return ok;
}

static uint64_t probe_block(uint8_t percent, uint64_t blocks) {
	if (percent == 0) {
		return 0;
	}
	if (percent >= 100) {
		return blocks - 1;
	}
	return ((blocks - 1) * (uint64_t)percent) / 100ull;
}

// Reads five sparse blocks before the clock starts. A fake-capacity card often
// wraps high addresses onto low ones; identical varied content at distant
// offsets is a hint worth showing, not a verdict on its own.
static bool run_probes(run_t *s, uint64_t blocks) {
	static const uint8_t percents[PROBE_COUNT] = {0, 12, 50, 87, 100};
	uint32_t hashes[PROBE_COUNT] = {0};
	uint64_t offsets[PROBE_COUNT] = {0};
	bool varied[PROBE_COUNT] = {false};
	int have = 0;

	s->r->phase = SDBENCH_PHASE_PROBE;
	for (int i = 0; i < PROBE_COUNT; i++) {
		if (cancelled(s)) {
			return cancel_run(s);
		}

		uint64_t block = probe_block(percents[i], blocks);
		uint64_t offset = block * (uint64_t)SDBENCH_BLOCK_BYTES;
		s->r->probe_reads++;
		bool ok = read_block(s, block, NULL, NULL);
		publish_progress(s, 0, false);
		if (!ok) {
			s->r->probe_errors++;
			continue;
		}

		uint32_t hash = block_hash(s->buffer);
		bool is_varied = !block_uniform(s->buffer);
		if (is_varied) {
			for (int j = 0; j < have; j++) {
				if (varied[j] && hashes[j] == hash && offsets[j] != offset) {
					s->r->repeated_probes++;
					if (s->r->first_repeat_offset == SDBENCH_NO_OFFSET) {
						s->r->first_repeat_offset = offset;
					}
					break;
				}
			}
		}
		hashes[have] = hash;
		offsets[have] = offset;
		varied[have] = is_varied;
		have++;
	}
	return true;
}

static bool run_phase(run_t *s, bool sequential, uint64_t budget_us, uint64_t blocks,
					  uint64_t *elapsed_out) {
	s->r->phase = sequential ? SDBENCH_PHASE_SEQUENTIAL : SDBENCH_PHASE_RANDOM;
	uint64_t *bytes = sequential ? &s->r->seq_bytes : &s->r->random_bytes;
	uint32_t *reads = sequential ? &s->r->seq_reads : &s->r->random_reads;
	const uint8_t base = sequential ? 0 : 50;
	const uint8_t span = 50;

	// Sequential starts away from the partition table. Random uses a stride
	// coprime with the block count, so every sampled 4K block is distinct until
	// the walk wraps -- a better failing-block sweep than repeated draws.
	uint64_t next = blocks / 7;
	uint64_t stride = 1;
	if (!sequential) {
		uint32_t rng = 0x9e3779b9u; // fixed seed: repeatable between runs
		next = (uint64_t)rng_next(&rng) % blocks;
		stride = (uint64_t)(rng_next(&rng) % (blocks - 1)) + 1;
		stride |= 1;
		while (gcd64(stride, blocks) != 1) {
			stride += 2;
			if (stride >= blocks) {
				stride = 1;
			}
		}
	}

	uint64_t phase_start = now_us(s);
	for (;;) {
		if (cancelled(s)) {
			return cancel_run(s);
		}
		uint64_t now = now_us(s);
		uint64_t elapsed = now >= phase_start ? now - phase_start : 0;
		if (elapsed >= budget_us || *reads >= MAX_PHASE_READS) {
			break;
		}

		uint64_t block = next;
		next = sequential ? (next + 1) % blocks : (next + stride) % blocks;
		read_block(s, block, bytes, reads);

		now = now_us(s);
		elapsed = now >= phase_start ? now - phase_start : 0;
		uint8_t progress = base + (uint8_t)((elapsed * span) / budget_us);
		if (progress > base + span) {
			progress = base + span;
		}
		publish_progress(s, progress, false);
	}

	uint64_t end = now_us(s);
	*elapsed_out = end >= phase_start ? end - phase_start : 1;
	publish_progress(s, base + span, true);
	return true;
}

bool sdbench_run_with_io(const sdbench_io_t *io, const char *device, uint32_t duration_ms,
						 sdbench_progress_cb progress, void *user, sdbench_result_t *out) {
	if (!out) {
		return false;
	}
	result_defaults(out);
	if (!io || !io->open || !io->read || !io->close || !io->now_us) {
		out->status = SDBENCH_OPEN_FAILED;
		return false;
	}
	if (!device || !device[0]) {
		return false; // SDBENCH_NO_CARD
	}

	uint32_t total_ms = duration_ms ? duration_ms : SDBENCH_DURATION_MS;
	if (total_ms > 60000u) {
		total_ms = 60000u;
	}
	uint64_t total_us = (uint64_t)total_ms * 1000ull;
	uint64_t seq_us = total_us / 2ull;
	uint64_t random_us = total_us - seq_us;

	void *buffer = NULL;
	if (posix_memalign(&buffer, SDBENCH_BLOCK_BYTES, SDBENCH_BLOCK_BYTES) != 0) {
		out->status = SDBENCH_NO_MEMORY;
		return false;
	}

	uint64_t size = 0;
	bool direct = false;
	void *handle = io->open(device, &size, &direct, io->user);
	if (!handle) {
		out->status = SDBENCH_OPEN_FAILED;
		free(buffer);
		return false;
	}
	out->card_bytes = size;
	out->direct_io = direct;
	if (size < SDBENCH_MIN_CARD_BYTES) {
		out->status = SDBENCH_TOO_SMALL;
		io->close(handle, io->user);
		free(buffer);
		return false;
	}

	uint64_t blocks = size / (uint64_t)SDBENCH_BLOCK_BYTES;
	run_t s = {
		.io = io,
		.handle = handle,
		.buffer = buffer,
		.r = out,
		.progress = progress,
		.user = user,
	};
	s.start_us = now_us(&s);
	s.last_publish_us = s.start_us;
	out->status = SDBENCH_OK;
	publish_progress(&s, 0, true);

	bool ok = false;
	uint64_t seq_elapsed_us = 1, random_elapsed_us = 1;
	if (!run_probes(&s, blocks)) {
		goto cleanup;
	}
	if (!run_phase(&s, true, seq_us, blocks, &seq_elapsed_us)) {
		goto cleanup;
	}
	if (!run_phase(&s, false, random_us, blocks, &random_elapsed_us)) {
		goto cleanup;
	}

	if (seq_elapsed_us == 0) {
		seq_elapsed_us = 1;
	}
	if (random_elapsed_us == 0) {
		random_elapsed_us = 1;
	}
	out->seq_bps = out->seq_bytes * 1000000ull / seq_elapsed_us;
	out->random_bps = out->random_bytes * 1000000ull / random_elapsed_us;
	out->random_iops = (uint32_t)((uint64_t)out->random_reads * 1000000ull / random_elapsed_us);
	out->status = SDBENCH_OK;
	out->phase = SDBENCH_PHASE_DONE;
	out->verdict = sdbench_verdict(out);
	publish_progress(&s, 100, true);
	ok = true;

cleanup:
	io->close(handle, io->user);
	free(buffer);
	return ok;
}

bool sdbench_run(const char *device, bool *cancel, uint32_t duration_ms,
				 sdbench_progress_cb progress, void *user, sdbench_result_t *out) {
	sdbench_io_t io;
	sdbench_default_io(&io, cancel);
	return sdbench_run_with_io(&io, device, duration_ms, progress, user, out);
}

// ---------------------------------------------------------------------------
// reading the answer
// ---------------------------------------------------------------------------

sdbench_verdict_t sdbench_verdict(const sdbench_result_t *result) {
	if (!result || result->status != SDBENCH_OK || result->phase != SDBENCH_PHASE_DONE ||
		result->seq_reads == 0 || result->random_reads == 0) {
		return SDBENCH_VERDICT_INCOMPLETE;
	}
	if (result->read_errors > 0) {
		return SDBENCH_VERDICT_READ_ERRORS;
	}
	if (result->repeated_probes > 0) {
		return SDBENCH_VERDICT_REPEATED_DATA;
	}
	if (result->slow_reads > 0) {
		return SDBENCH_VERDICT_SLOW_BLOCKS;
	}
	if (result->seq_bps < SDBENCH_MIN_SEQ_BPS || result->random_iops < SDBENCH_MIN_RANDOM_IOPS) {
		return SDBENCH_VERDICT_BELOW_REFERENCE;
	}
	return SDBENCH_VERDICT_GOOD;
}

void sdbench_format_rate(uint64_t bytes_per_s, char *out, size_t out_size) {
	if (!out || out_size == 0) {
		return;
	}
	if (bytes_per_s >= 1000000ull) {
		uint64_t whole = bytes_per_s / 1000000ull;
		uint64_t tenths = ((bytes_per_s % 1000000ull) * 10ull) / 1000000ull;
		snprintf(out, out_size, "%llu.%llu MB/s", (unsigned long long)whole,
				 (unsigned long long)tenths);
	} else if (bytes_per_s >= 1000ull) {
		uint64_t whole = bytes_per_s / 1000ull;
		uint64_t tenths = ((bytes_per_s % 1000ull) * 10ull) / 1000ull;
		snprintf(out, out_size, "%llu.%llu KB/s", (unsigned long long)whole,
				 (unsigned long long)tenths);
	} else {
		snprintf(out, out_size, "%llu B/s", (unsigned long long)bytes_per_s);
	}
}
