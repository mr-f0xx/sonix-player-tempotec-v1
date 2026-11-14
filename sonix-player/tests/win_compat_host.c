// The shims win_compat_host.h declares. Windows host only: on Linux this file
// compiles to nothing, and the tests keep using the real calls.
#ifdef _WIN32

#include "win_compat_host.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <windows.h>
#include <errno.h>
#include <windows.h>

int win_mkdir(const char *path, int mode) {
	(void)mode;
	return _mkdir(path);
}

int win_fchmod(int fd, unsigned mode) {
	// The 0600 permission of the session sidecar: there is nothing on a
	// Windows host that reads it, so the call only has to succeed.
	(void)fd;
	(void)mode;
	return 0;
}

struct tm *win_gmtime_r(const time_t *timep, struct tm *result) {
	return gmtime_s(result, timep) == 0 ? result : NULL;
}

struct tm *win_localtime_r(const time_t *timep, struct tm *result) {
	return localtime_s(result, timep) == 0 ? result : NULL;
}

int win_fsync(int fd) {
	// mingw has no fsync; _commit flushes the same descriptor to the disk.
	return _commit(fd);
}

int win_usleep(unsigned usec) {
	Sleep(usec / 1000u);
	return 0;
}

// The suites hardcode "/tmp" (POSIX); on this host that is the current drive's
// \tmp, and host security has been seen refusing one process while allowing
// another -- a big test binary gets EACCES where a small probe succeeds. The
// CRT temp directory (honouring TMP/TEMP when the caller sets it) always
// works, so a /tmp template moves there first. The rewrite needs room in the
// caller's buffer beyond the literal, which is why the suites size their
// template arrays at 600 bytes.
#define WIN_MKDTEMP_BUF 600

static char *make_dir_from(char *tmpl, size_t len, unsigned *seed) {
	for (unsigned attempt = 0; attempt < 1000; attempt++) {
		unsigned r = GetTickCount() ^ ((*seed)++ * 2654435761u) ^ GetCurrentProcessId();
		for (int i = 0; i < 6; i++) {
			tmpl[len - 6 + (size_t)i] = (char)('a' + (r % 26));
			r /= 26;
		}
		if (_mkdir(tmpl) == 0) return tmpl;
	}
	return NULL;
}

char *win_mkdtemp(char *tmpl) {
	// mingw has no mkdtemp: fill the XXXXXX in and take the first folder that
	// does not already exist, the same contract the POSIX call has.
	static unsigned seed;
	if (!tmpl) return NULL;
	size_t len = strlen(tmpl);
	if (len < 6 || strcmp(tmpl + len - 6, "XXXXXX") != 0) return NULL;
	// Kept, because the CRT temp folder is only where this is TRIED first: a
	// host that refuses it is not a reason to stop, when the suite's own
	// template names a folder that is writable.
	char original[WIN_MKDTEMP_BUF];
	if (len + 1 > sizeof(original)) return NULL;
	snprintf(original, sizeof(original), "%s", tmpl);
	if (strncmp(tmpl, "/tmp/", 5) == 0) {
		char temp[MAX_PATH];
		DWORD n = GetTempPathA(sizeof(temp), temp);
		if (n > 0 && n < sizeof(temp)) {
			if (temp[n - 1] != '\\' && temp[n - 1] != '/') {
				temp[n++] = '/';
			}
			size_t keep = len - 5; // the basename, XXXXXX included
			if (n + keep < WIN_MKDTEMP_BUF) {
				memmove(tmpl + n, tmpl + 5, keep + 1);
				memcpy(tmpl, temp, n);
				len = n + keep;
			}
		}
	}
	if (make_dir_from(tmpl, len, &seed)) return tmpl;
	// The temp folder would not have it: the template as the suite wrote it.
	if (len != strlen(original)) {
		snprintf(tmpl, WIN_MKDTEMP_BUF, "%s", original);
		return make_dir_from(tmpl, strlen(original), &seed);
	}
	return NULL;
}

int win_rename(const char *from, const char *to) {
	if (MoveFileExA(from, to, MOVEFILE_REPLACE_EXISTING)) return 0;
	errno = EACCES;
	return -1;
}

char *win_strcasestr(const char *haystack, const char *needle) {
	size_t needle_len = strlen(needle);
	if (!needle_len) return (char *)haystack;
	for (; *haystack; haystack++) {
		size_t i = 0;
		while (i < needle_len && tolower((unsigned char)haystack[i]) == tolower((unsigned char)needle[i])) {
			i++;
		}
		if (i == needle_len) return (char *)haystack;
	}
	return NULL;
}

long long win_pread(int fd, void *buf, size_t count, long long offset) {
	HANDLE h = (HANDLE)_get_osfhandle(fd);
	if (h == INVALID_HANDLE_VALUE) {
		errno = EBADF;
		return -1;
	}
	OVERLAPPED ov;
	memset(&ov, 0, sizeof(ov));
	ov.Offset = (DWORD)(offset & 0xFFFFFFFF);
	ov.OffsetHigh = (DWORD)((unsigned long long)offset >> 32);
	DWORD bytes_read = 0;
	if (!ReadFile(h, buf, (DWORD)count, &bytes_read, &ov)) {
		DWORD err = GetLastError();
		if (err == ERROR_HANDLE_EOF) return 0;
		errno = EIO;
		return -1;
	}
	return (long long)bytes_read;
}

void cpupower_yield_if_stressed(void) {}

#endif /* _WIN32 */
