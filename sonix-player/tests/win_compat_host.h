#ifndef WIN_COMPAT_HOST_H
#define WIN_COMPAT_HOST_H

// Windows-host shims for the few POSIX calls the streaming modules use. The
// device's glibc has all of them; mingw does not. Included by force
// (-include) when a test suite links real module code on a Windows host, and
// compiled nowhere on Linux, where the real calls exist.

#ifdef _WIN32

#include <direct.h>
#include <io.h>
#include <ctype.h>
#include <time.h>

#define mkdir(path, mode) win_mkdir((path), (mode))
#define lstat(path, st) stat((path), (st))
#define fchmod(fd, mode) win_fchmod((fd), (mode))
#define gmtime_r(timep, result) win_gmtime_r((timep), (result))
#define localtime_r(timep, result) win_localtime_r((timep), (result))
#define strcasestr(haystack, needle) win_strcasestr((haystack), (needle))
#define fsync(fd) win_fsync(fd)
#define usleep(usec) win_usleep(usec)
#define malloc_trim(pad) ((void)(pad), 0)
#define mkdtemp(tmpl) win_mkdtemp(tmpl)
#define sync() ((void)0)
// A POSIX rename() replaces the destination. mingw's _rename refuses an
// existing file, which is exactly the case config.c's atomic save hits on the
// second write of a run, and a settings write that silently fails makes a test
// measure defaults instead of what it set.
#define rename(from, to) win_rename((from), (to))
#define pread(fd, buf, count, offset) win_pread((fd), (buf), (count), (offset))

int win_mkdir(const char *path, int mode);
int win_fchmod(int fd, unsigned mode);
struct tm *win_gmtime_r(const time_t *timep, struct tm *result);
struct tm *win_localtime_r(const time_t *timep, struct tm *result);
int win_fsync(int fd);
int win_usleep(unsigned usec);
char *win_mkdtemp(char *tmpl);
char *win_strcasestr(const char *haystack, const char *needle);
int win_rename(const char *from, const char *to);
long long win_pread(int fd, void *buf, size_t count, long long offset);

#endif /* _WIN32 */

#endif /* WIN_COMPAT_HOST_H */
