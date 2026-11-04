#define _GNU_SOURCE
#include "src/system/core/utils.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int main(void) {
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
	puts("PASS read_file_content (empty, growing, non-seekable and failed reads)");
	return 0;
}
