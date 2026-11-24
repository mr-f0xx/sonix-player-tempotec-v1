// The 5-second card benchmark with a fake block device: deterministic clock,
// bad blocks, stalls, repeated data, cancellation and the real-file fallback.
#include "src/system/device/sdbench.h"

#include <assert.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef struct {
	uint64_t now;
	uint64_t size;
	bool open_ok;
	bool direct;
	uint32_t latency_us;
	uint64_t fail_block;
	bool has_fail_block;
	uint64_t slow_block;
	bool has_slow_block;
	uint32_t slow_latency_us;
	uint64_t cancel_at_us;
	bool uniform;
	bool same_varied;
	uint32_t reads;
} fake_t;

static unsigned progress_calls;
static uint8_t last_progress;

static void progress_cb(const sdbench_result_t *partial, void *user) {
	(void)user;
	progress_calls++;
	last_progress = partial->progress;
}

static void *fake_open(const char *device, uint64_t *size_bytes, bool *direct, void *user) {
	(void)device;
	fake_t *f = (fake_t *)user;
	f->now = 0;
	f->reads = 0;
	if (!f->open_ok) {
		return NULL;
	}
	*size_bytes = f->size;
	*direct = f->direct;
	return f;
}

static bool fake_read(void *handle, uint64_t offset, void *buffer, size_t bytes, void *user) {
	(void)user;
	fake_t *f = (fake_t *)handle;
	uint64_t block = offset / SDBENCH_BLOCK_BYTES;
	f->reads++;
	f->now += f->latency_us;
	if (f->has_slow_block && block == f->slow_block) {
		f->now += f->slow_latency_us;
	}
	if (f->has_fail_block && block == f->fail_block) {
		return false;
	}

	unsigned char *p = (unsigned char *)buffer;
	if (f->same_varied) {
		for (size_t i = 0; i < bytes; i++) {
			p[i] = (unsigned char)(i * 7u + 3u);
		}
	} else if (f->uniform) {
		memset(p, 0, bytes);
	} else {
		// Mix the whole block number into every byte. A simple block*31+i
		// repeats every 256 blocks and would make two sparse probes look like
		// a wrapped fake-capacity card by accident.
		uint64_t mix = block ^ (block >> 7) ^ (block >> 13) ^ (block >> 21);
		for (size_t i = 0; i < bytes; i++) {
			p[i] = (unsigned char)(mix + i * 31u);
		}
	}
	return true;
}

static void fake_close(void *handle, void *user) {
	(void)handle;
	(void)user;
}

static uint64_t fake_now(void *user) { return ((fake_t *)user)->now; }

static bool fake_cancelled(void *user) {
	fake_t *f = (fake_t *)user;
	return f->cancel_at_us != 0 && f->now >= f->cancel_at_us;
}

static bool run_fake(fake_t *f, uint32_t duration_ms, sdbench_result_t *out) {
	sdbench_io_t io = {
		.open = fake_open,
		.read = fake_read,
		.close = fake_close,
		.now_us = fake_now,
		.cancelled = fake_cancelled,
		.user = f,
	};
	progress_calls = 0;
	last_progress = 255;
	return sdbench_run_with_io(&io, "fake-card", duration_ms, progress_cb, NULL, out);
}

static fake_t healthy_fake(void) {
	return (fake_t){
		.size = 64ull * 1024ull * 1024ull,
		.open_ok = true,
		.direct = true,
		.latency_us = 250, // 16.384 MB/s and 4000 IOPS
	};
}

static void test_format(void) {
	char text[32];
	sdbench_format_rate(0, text, sizeof(text));
	assert(strcmp(text, "0 B/s") == 0);
	sdbench_format_rate(999, text, sizeof(text));
	assert(strcmp(text, "999 B/s") == 0);
	sdbench_format_rate(1500, text, sizeof(text));
	assert(strcmp(text, "1.5 KB/s") == 0);
	sdbench_format_rate(1024000, text, sizeof(text));
	assert(strcmp(text, "1.0 MB/s") == 0);
	sdbench_format_rate(16384000, text, sizeof(text));
	assert(strcmp(text, "16.3 MB/s") == 0);
	sdbench_format_rate(10500000, text, sizeof(text));
	assert(strcmp(text, "10.5 MB/s") == 0);
}

static void test_healthy(void) {
	fake_t f = healthy_fake();
	sdbench_result_t r;
	assert(run_fake(&f, 20, &r));
	assert(r.status == SDBENCH_OK && r.phase == SDBENCH_PHASE_DONE);
	assert(r.verdict == SDBENCH_VERDICT_GOOD);
	assert(r.progress == 100 && r.elapsed_ms >= 20);
	assert(r.card_bytes == f.size && r.direct_io);
	assert(r.probe_reads == 5 && r.probe_errors == 0 && r.repeated_probes == 0);
	assert(r.seq_reads > 0 && r.random_reads > 0);
	assert(r.seq_bytes == (uint64_t)r.seq_reads * SDBENCH_BLOCK_BYTES);
	assert(r.random_bytes == (uint64_t)r.random_reads * SDBENCH_BLOCK_BYTES);
	assert(r.read_errors == 0 && r.slow_reads == 0 && r.max_read_ms == 0);
	assert(r.seq_bps == 16384000ull && r.random_bps == 16384000ull);
	assert(r.random_iops == 4000u);
	assert(r.first_error_offset == SDBENCH_NO_OFFSET);
	assert(r.first_slow_offset == SDBENCH_NO_OFFSET);
	assert(r.first_repeat_offset == SDBENCH_NO_OFFSET);
	assert(progress_calls >= 2 && last_progress == 100);

	char text[32];
	sdbench_format_rate(r.seq_bps, text, sizeof(text));
	assert(strcmp(text, "16.3 MB/s") == 0);
}

static void test_failing_blocks(void) {
	// One bad 4K read during the sequential phase. The run still finishes and
	// the result says where the first failure was.
	fake_t f = healthy_fake();
	f.has_fail_block = true;
	f.fail_block = f.size / SDBENCH_BLOCK_BYTES / 7 + 2; // third sequential read
	sdbench_result_t r;
	assert(run_fake(&f, 20, &r));
	assert(r.status == SDBENCH_OK && r.verdict == SDBENCH_VERDICT_READ_ERRORS);
	assert(r.read_errors == 1 && r.probe_errors == 0);
	assert(r.first_error_offset == f.fail_block * SDBENCH_BLOCK_BYTES);
	assert(r.seq_reads > 0 && r.random_reads > 0);

	// A 300 ms stall is a suspicious block, not just a low average.
	f = healthy_fake();
	f.has_slow_block = true;
	f.slow_block = f.size / SDBENCH_BLOCK_BYTES / 7 + 1;
	f.slow_latency_us = 300000;
	assert(run_fake(&f, 20, &r));
	assert(r.status == SDBENCH_OK && r.verdict == SDBENCH_VERDICT_SLOW_BLOCKS);
	assert(r.slow_reads == 1 && r.max_read_ms >= 300);
	assert(r.first_slow_offset == f.slow_block * SDBENCH_BLOCK_BYTES);
	assert(r.random_reads > 0); // the second phase still ran
}

static void test_counterfeit_hints(void) {
	// Distant probes returning the same varied 4K data are a fake-capacity
	// hint. Uniform empty-card blocks must not be mistaken for it.
	fake_t f = healthy_fake();
	f.same_varied = true;
	sdbench_result_t r;
	assert(run_fake(&f, 20, &r));
	assert(r.status == SDBENCH_OK && r.verdict == SDBENCH_VERDICT_REPEATED_DATA);
	assert(r.repeated_probes > 0 && r.read_errors == 0);
	assert(r.first_repeat_offset != SDBENCH_NO_OFFSET);

	f = healthy_fake();
	f.uniform = true;
	assert(run_fake(&f, 20, &r));
	assert(r.repeated_probes == 0 && r.verdict == SDBENCH_VERDICT_GOOD);

	// A slow but error-free card is called out against the diagnostic references.
	f = healthy_fake();
	f.latency_us = 2000; // 2.048 MB/s and 500 IOPS
	assert(run_fake(&f, 20, &r));
	assert(r.status == SDBENCH_OK && r.verdict == SDBENCH_VERDICT_BELOW_REFERENCE);
	assert(r.seq_bps < SDBENCH_MIN_SEQ_BPS && r.random_iops < SDBENCH_MIN_RANDOM_IOPS);
	assert(r.slow_reads == 0 && r.read_errors == 0);
}

static void test_incomplete(void) {
	sdbench_result_t r;
	fake_t f = healthy_fake();
	f.cancel_at_us = 3000;
	assert(!run_fake(&f, 20, &r));
	assert(r.status == SDBENCH_CANCELLED && r.phase == SDBENCH_PHASE_DONE);
	assert(r.verdict == SDBENCH_VERDICT_INCOMPLETE);
	assert(r.progress < 100 && last_progress < 100);
	assert(r.random_reads == 0 && r.elapsed_ms >= 3);

	f = healthy_fake();
	f.open_ok = false;
	assert(!run_fake(&f, 20, &r));
	assert(r.status == SDBENCH_OPEN_FAILED && progress_calls == 0);

	f = healthy_fake();
	f.size = 1024ull * 1024ull;
	assert(!run_fake(&f, 20, &r));
	assert(r.status == SDBENCH_TOO_SMALL && r.card_bytes == f.size);

	f = healthy_fake();
	sdbench_io_t io = {
		.open = fake_open, .read = fake_read, .close = fake_close,
		.now_us = fake_now, .cancelled = fake_cancelled, .user = &f,
	};
	assert(!sdbench_run_with_io(&io, NULL, 20, NULL, NULL, &r));
	assert(r.status == SDBENCH_NO_CARD);
	assert(!sdbench_run_with_io(&io, "fake-card", 20, NULL, NULL, NULL));

	// Read errors outrank every other verdict.
	memset(&r, 0, sizeof(r));
	r.status = SDBENCH_OK;
	r.phase = SDBENCH_PHASE_DONE;
	r.seq_reads = r.random_reads = 1;
	r.seq_bps = SDBENCH_MIN_SEQ_BPS;
	r.random_iops = SDBENCH_MIN_RANDOM_IOPS;
	assert(sdbench_verdict(&r) == SDBENCH_VERDICT_GOOD);
	r.read_errors = 1;
	r.repeated_probes = 1;
	r.slow_reads = 1;
	assert(sdbench_verdict(&r) == SDBENCH_VERDICT_READ_ERRORS);
	assert(sdbench_verdict(NULL) == SDBENCH_VERDICT_INCOMPLETE);
}

static void test_real_file(void) {
	// The default I/O layer on a sparse regular file: BLKGETSIZE64 fails,
	// lseek() supplies the size, O_DIRECT is used or falls back safely, and a
	// whole run completes without touching page-cache-only shortcuts.
	char path[] = "/tmp/sonix-sdbench-XXXXXX";
	int fd = mkstemp(path);
	assert(fd >= 0);
	assert(ftruncate(fd, (off_t)SDBENCH_MIN_CARD_BYTES) == 0);
	assert(close(fd) == 0);

	bool cancel = false;
	__atomic_store_n(&cancel, false, __ATOMIC_RELAXED);
	sdbench_result_t r;
	assert(sdbench_run(path, &cancel, 30, NULL, NULL, &r));
	assert(r.status == SDBENCH_OK && r.progress == 100);
	assert(r.card_bytes == SDBENCH_MIN_CARD_BYTES);
	assert(r.seq_reads > 0 && r.random_reads > 0);
	assert(r.seq_bps > 0 && r.random_iops > 0);
	assert(r.read_errors == 0 && r.probe_errors == 0 && r.repeated_probes == 0);
	assert(r.elapsed_ms > 0);
	assert(unlink(path) == 0);
}

int main(void) {
	test_format();
	test_healthy();
	test_failing_blocks();
	test_counterfeit_hints();
	test_incomplete();
	test_real_file();
	puts("PASS sd card benchmark (4K sequential/random, probes, failing blocks, counterfeit hints, cancel and real I/O)");
	return 0;
}
