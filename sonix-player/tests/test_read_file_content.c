#define _GNU_SOURCE
#include "src/system/core/utils.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

// The kernel's real-time limit, as rt_throttle_ensure() finds it in a
// directory standing in for /proc/sys/kernel.
static void put(const char *dir, const char *name, const char *text) {
	char path[256];
	snprintf(path, sizeof(path), "%s/%s", dir, name);
	FILE *f = fopen(path, "w");
	assert(f && fputs(text, f) >= 0 && fclose(f) == 0);
}

static long get(const char *dir, const char *name) {
	char path[256];
	snprintf(path, sizeof(path), "%s/%s", dir, name);
	FILE *f = fopen(path, "r");
	long value = 0;
	assert(f && fscanf(f, "%ld", &value) == 1 && fclose(f) == 0);
	return value;
}

static void test_rt_throttle(void) {
	char dir[] = "/tmp/sonix-rt-XXXXXX";
	assert(mkdtemp(dir));
	char runtime[256], period[256];
	snprintf(runtime, sizeof(runtime), "%s/sched_rt_runtime_us", dir);
	snprintf(period, sizeof(period), "%s/sched_rt_period_us", dir);

	// Nothing to read: said, and nothing invented.
	assert(rt_throttle_ensure(dir, true) == 0);

	// As Linux ships it: reported and left alone.
	put(dir, "sched_rt_runtime_us", "950000\n");
	put(dir, "sched_rt_period_us", "1000000\n");
	assert(rt_throttle_ensure(dir, true) == 950000);
	assert(get(dir, "sched_rt_runtime_us") == 950000);

	// Switched off, which is the case that lets one thread stop the device:
	// switched on at 95 % of the period.
	put(dir, "sched_rt_runtime_us", "-1\n");
	assert(rt_throttle_ensure(dir, true) == 950000);
	assert(get(dir, "sched_rt_runtime_us") == 950000);

	// A runtime as long as the period is the same thing by another name, and
	// the 95 % is of the period that is there.
	put(dir, "sched_rt_runtime_us", "500000\n");
	put(dir, "sched_rt_period_us", "500000\n");
	assert(rt_throttle_ensure(dir, true) == 475000);
	assert(get(dir, "sched_rt_runtime_us") == 475000);

	// Told to leave it: reported as off, and left.
	put(dir, "sched_rt_runtime_us", "-1\n");
	put(dir, "sched_rt_period_us", "1000000\n");
	assert(rt_throttle_ensure(dir, false) == -1);
	assert(get(dir, "sched_rt_runtime_us") == -1);

	// No period file: the kernel's default period is assumed.
	assert(unlink(period) == 0);
	assert(rt_throttle_ensure(dir, true) == 950000);

	// A value that cannot be written is not reported as set. (Root writes
	// through a read-only mode, so there the case proves nothing and is skipped.)
	put(dir, "sched_rt_runtime_us", "-1\n");
	assert(chmod(runtime, 0444) == 0);
	if (geteuid() != 0) {
		assert(rt_throttle_ensure(dir, true) == -1);
		assert(get(dir, "sched_rt_runtime_us") == -1);
	}

	assert(unlink(runtime) == 0 && rmdir(dir) == 0);
}

int main(void) {
	test_rt_throttle();
	char path[] = "/tmp/sonix-read-file-XXXXXX";
	int fd = mkstemp(path);
	assert(fd >= 0);

	char *contents = read_file_content(path);
	assert(contents && contents[0] == '\0');
	free(contents);

	// Cross both the initial allocation and its growth boundary.
	char data[10000];
	for (size_t i = 0; i < sizeof(data); i++) data[i] = (char)('a' + i % 26);
	assert(write(fd, data, sizeof(data)) == (ssize_t)sizeof(data));
	contents = read_file_content(path);
	assert(contents && strlen(contents) == sizeof(data));
	assert(memcmp(contents, data, sizeof(data)) == 0);
	free(contents);
	assert(close(fd) == 0 && unlink(path) == 0);
	errno = 0;
	assert(read_file_content(path) == NULL && errno == ENOENT);

	// Real procfs entries report a zero stat size and cannot seek to the end.
	contents = read_file_content("/proc/self/status");
	assert(contents && strstr(contents, "Name:") && strstr(contents, "Pid:"));
	free(contents);

	// A pipe is another explicitly non-seekable source with finite contents.
	int pipes[2];
	assert(pipe(pipes) == 0);
	assert(write(pipes[1], "usb-data", 8) == 8 && close(pipes[1]) == 0);
	char procfd[64];
	snprintf(procfd, sizeof(procfd), "/proc/self/fd/%d", pipes[0]);
	contents = read_file_content(procfd);
	assert(contents && strcmp(contents, "usb-data") == 0);
	free(contents);
	assert(close(pipes[0]) == 0);

	// Opening a directory may succeed, but a read error is never empty data.
	errno = 0;
	assert(read_file_content("/tmp") == NULL && errno == EISDIR);
	puts("PASS read_file_content (empty, growing, non-seekable and failed reads) and the real-time limit");
	return 0;
}
