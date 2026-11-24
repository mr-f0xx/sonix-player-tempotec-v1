#ifndef SDBENCH_H
#define SDBENCH_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// A short, read-only health check for the microSD block device.
//
// The player cannot ask an SD card for SMART data: cheap and counterfeit cards
// lie about their capacity, and worn ones answer with long stalls or read
// errors instead. What it can do is read 4K blocks from the raw partition,
// spread across the card, and report what came back. Nothing is written and
// no filesystem file is created, so a card with no free space (or a read-only
// mount) can still be checked.
//
// The benchmark is deliberately a background job: a failing card can block a
// read for hundreds of milliseconds, and doing that on the interface thread
// is exactly the freeze this feature is meant to diagnose. See sdhealthpage.c
// for the worker and the page that polls it.

// Five seconds total: half sequential, half random. Both use 4K reads.
#define SDBENCH_DURATION_MS 5000u
#define SDBENCH_BLOCK_BYTES 4096u

// A single 4K read this slow is a stall a user can feel, not normal garbage
// collection on a healthy card. It is reported separately from throughput.
#define SDBENCH_SLOW_READ_US 250000u

// Conservative *diagnostic references*, not a pass/fail certification and not
// an SD speed-class test: 10 MB/s is the decimal sequential rate a Class 10 /
// U1 / V10 card must sustain when writing, while A1 specifies 1500 random-read
// IOPS. This utility reads raw 4K blocks and never writes, so neither figure
// is a specification it can certify. They are chosen only because a card well
// below either reference cannot keep a music library's UI responsive; the
// measured rates, errors and stalls are always shown next to the verdict.
#define SDBENCH_MIN_SEQ_BPS (10ull * 1000ull * 1000ull)
#define SDBENCH_MIN_RANDOM_IOPS 1500u

// A real card is far larger; this only stops a bogus device node from being
// sampled as if it were a card.
#define SDBENCH_MIN_CARD_BYTES (8ull * 1024ull * 1024ull)
#define SDBENCH_NO_OFFSET UINT64_MAX

typedef enum {
	SDBENCH_OK = 0, // the run finished; read_errors may still be non-zero
	SDBENCH_NO_CARD,
	SDBENCH_USB_BUSY,
	SDBENCH_OPEN_FAILED,
	SDBENCH_TOO_SMALL,
	SDBENCH_NO_MEMORY,
	SDBENCH_CANCELLED,
	SDBENCH_START_FAILED,
	SDBENCH_STALLED,
} sdbench_status_t;

typedef enum {
	SDBENCH_PHASE_IDLE = 0,
	SDBENCH_PHASE_PROBE,
	SDBENCH_PHASE_SEQUENTIAL,
	SDBENCH_PHASE_RANDOM,
	SDBENCH_PHASE_DONE,
} sdbench_phase_t;

typedef enum {
	SDBENCH_VERDICT_INCOMPLETE = 0,
	SDBENCH_VERDICT_GOOD,
	SDBENCH_VERDICT_BELOW_REFERENCE,
	SDBENCH_VERDICT_SLOW_BLOCKS,
	SDBENCH_VERDICT_READ_ERRORS,
	SDBENCH_VERDICT_REPEATED_DATA,
} sdbench_verdict_t;

typedef struct {
	sdbench_status_t status;
	sdbench_phase_t phase;
	sdbench_verdict_t verdict;
	uint8_t progress; // 0..100, for the page's progress bar
	uint32_t elapsed_ms;
	uint64_t card_bytes;
	bool direct_io; // O_DIRECT worked; otherwise buffered reads evict pages

	// Five sparse probes near 0%, 12%, 50%, 87% and the last 4K block.
	// Repeated, non-uniform content at distant offsets is a fake-capacity hint,
	// not proof: identical filesystem structures can do it too.
	uint32_t probe_reads;
	uint32_t probe_errors;
	uint32_t repeated_probes;

	uint64_t seq_bytes, random_bytes;
	uint32_t seq_reads, random_reads;
	uint64_t seq_bps, random_bps; // bytes per second
	uint32_t random_iops;         // 4K reads per second
	uint32_t max_read_ms;

	uint32_t read_errors; // probes + both timed phases
	uint32_t slow_reads;  // reads at or above SDBENCH_SLOW_READ_US
	uint64_t first_error_offset;
	uint64_t first_slow_offset;
	uint64_t first_repeat_offset;
} sdbench_result_t;

typedef void (*sdbench_progress_cb)(const sdbench_result_t *partial, void *user);

// The runner's I/O boundary. The device build opens the card's block node;
// tests supply a fake card with a controllable clock, bad blocks and repeat
// data. All callbacks run on the calling (worker) thread.
typedef struct {
	// Returns a handle, or NULL when the device cannot be opened. Fills the
	// byte size and whether O_DIRECT is in use.
	void *(*open)(const char *device, uint64_t *size_bytes, bool *direct, void *user);
	// Reads one whole 4K block at a byte offset. False on EIO/short read.
	bool (*read)(void *handle, uint64_t offset, void *buffer, size_t bytes, void *user);
	void (*close)(void *handle, void *user);
	uint64_t (*now_us)(void *user); // monotonic
	bool (*cancelled)(void *user);  // optional
	void *user;
} sdbench_io_t;

// Fills `io` for the real card. `cancel` is a plain bool accessed with GCC
// atomics: the page sets it from the interface thread, the runner reads it
// between 4K reads. NULL means never cancelled.
void sdbench_default_io(sdbench_io_t *io, bool *cancel);

// Runs with the real device. `device` is storage_sd_device()'s block node.
// `out` is always filled, including on failure. Returns true only when the
// timed phases both finished.
bool sdbench_run(const char *device, bool *cancel, uint32_t duration_ms,
				 sdbench_progress_cb progress, void *user, sdbench_result_t *out);

// The same run against an injected I/O layer. `duration_ms` may be shortened
// for tests; zero means SDBENCH_DURATION_MS.
bool sdbench_run_with_io(const sdbench_io_t *io, const char *device, uint32_t duration_ms,
						 sdbench_progress_cb progress, void *user, sdbench_result_t *out);

// The verdict from a completed result. Read errors outrank repeated data,
// which outrank stalls, which outrank the diagnostic rate references.
sdbench_verdict_t sdbench_verdict(const sdbench_result_t *result);

// "12.3 MB/s", "842.1 KB/s", "0 B/s". Decimal units, no translation: a
// number and a unit read the same in all eleven languages.
void sdbench_format_rate(uint64_t bytes_per_s, char *out, size_t out_size);

#endif /* SDBENCH_H */
