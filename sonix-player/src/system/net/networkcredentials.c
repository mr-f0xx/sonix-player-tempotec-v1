#define _GNU_SOURCE 1
#include "networkcredentials.h"

#include "src/system/core/config.h"
#include "src/system/core/json.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static char path[600];
static char stored[2][SHARE_LOGIN_MAX];

static const char *key_for(share_kind_t kind) { return kind == SHARE_FTP ? "ftp" : "webdav"; }

static bool clean_password(const char *text, size_t cap) {
	if (!text || strnlen(text, cap) >= cap) return false;
	for (const unsigned char *p = (const unsigned char *)text; *p; p++) {
		if (*p < 32 || *p == 127) return false;
	}
	return true;
}

static bool write_string(FILE *f, const char *s) {
	if (fputc('"', f) == EOF) return false;
	for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
		if (*p == '"' || *p == '\\') {
			if (fputc('\\', f) == EOF || fputc(*p, f) == EOF) return false;
		} else if (*p < 32) {
			if (fprintf(f, "\\u%04x", *p) < 0) return false;
		} else if (fputc(*p, f) == EOF) return false;
	}
	return fputc('"', f) != EOF;
}

static bool write_file(const char *tmp) {
	int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW | O_CLOEXEC, 0600);
	if (fd < 0) return false;
	if (fchmod(fd, 0600) != 0) {
		close(fd);
		unlink(tmp);
		return false;
	}
	FILE *f = fdopen(fd, "w");
	if (!f) {
		close(fd);
		unlink(tmp);
		return false;
	}
	bool ok = fputs("{", f) >= 0;
	bool first = true;
	for (int kind = 0; kind < 2 && ok; kind++) {
		if (!stored[kind][0]) continue;
		if (!first) ok = fputs(",", f) >= 0;
		first = false;
		if (!ok) break;
		ok = write_string(f, key_for((share_kind_t)kind));
		if (ok) ok = fputc(':', f) != EOF;
		if (ok) ok = write_string(f, stored[kind]);
	}
	if (ok) ok = fputs("}\n", f) >= 0;
	if (!ok) {
		fclose(f);
		unlink(tmp);
		return false;
	}
	if (fflush(f) != 0 || fsync(fd) != 0) {
		fclose(f);
		unlink(tmp);
		return false;
	}
	if (fclose(f) != 0) {
		unlink(tmp);
		return false;
	}
	if (rename(tmp, path) != 0) {
		unlink(tmp);
		return false;
	}
	return true;
}

// Writes the file while at least one password is stored; unlinks it otherwise.
static bool sync_file(void) {
	if (!stored[0][0] && !stored[1][0]) {
		if (!path[0]) return true;
		return unlink(path) == 0 || errno == ENOENT;
	}
	if (!path[0]) return false;
	char tmp[sizeof(path) + 8];
	snprintf(tmp, sizeof(tmp), "%s.tmp", path);
	return write_file(tmp);
}

void networkcredentials_init(void) {
	pthread_mutex_lock(&lock);
	path[0] = '\0';
	stored[0][0] = stored[1][0] = '\0';
	const char *config = config_path();
	if (config && config[0]) {
		char cfg[sizeof(path)];
		snprintf(cfg, sizeof(cfg), "%s", config);
		char *slash = strrchr(cfg, '/');
		if (slash) {
			*slash = '\0';
			size_t dir_len = strlen(cfg);
			if (dir_len && dir_len + 1 + sizeof("network-credentials.json") - 1 < sizeof(path)) {
				memcpy(path, cfg, dir_len);
				path[dir_len] = '/';
				memcpy(path + dir_len + 1, "network-credentials.json", sizeof("network-credentials.json"));
			}
		}
	}
	int fd = path[0] ? open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC) : -1;
	if (fd >= 0) {
		struct stat st;
		if (fstat(fd, &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 0 && st.st_size < (off_t)8192) {
			FILE *f = fdopen(fd, "r");
			if (f) {
				char text[8192];
				memset(text, 0, sizeof(text));
				size_t n = fread(text, 1, sizeof(text) - 1, f);
				text[n] = '\0';
				json_doc_t doc;
				if (!ferror(f) && json_parse(text, &doc)) {
					int root = json_root(&doc);
					for (int kind = 0; kind < 2; kind++) {
						int tok = json_get(&doc, root, key_for((share_kind_t)kind));
						char value[SHARE_LOGIN_MAX];
						if (json_str_exact(&doc, tok, value, sizeof(value)) && clean_password(value, sizeof(value))) {
							snprintf(stored[kind], sizeof(stored[kind]), "%s", value);
						}
					}
					json_free(&doc);
				}
				fclose(f);
				fd = -1;
			}
		}
		if (fd >= 0) close(fd);
	}
	pthread_mutex_unlock(&lock);
}

void networkcredentials_get(share_kind_t kind, char *out, size_t size, bool *remembered) {
	if (out && size) out[0] = '\0';
	if (remembered) *remembered = false;
	if (kind != SHARE_WEBDAV && kind != SHARE_FTP) return;
	pthread_mutex_lock(&lock);
	if (out && size) {
		snprintf(out, size, "%s", stored[kind]);
	}
	if (remembered) *remembered = stored[kind][0] != '\0';
	pthread_mutex_unlock(&lock);
}

bool networkcredentials_apply(share_kind_t kind, const char *password, bool remember,
						  char *error, size_t error_size) {
	if (error && error_size) error[0] = '\0';
	if (kind != SHARE_WEBDAV && kind != SHARE_FTP) {
		if (error && error_size) snprintf(error, error_size, "Invalid share protocol.");
		return false;
	}
	const char *pw = password ? password : "";
	pthread_mutex_lock(&lock);
	bool ok;
	if (remember && pw[0]) {
		if (!clean_password(pw, SHARE_LOGIN_MAX)) {
			pthread_mutex_unlock(&lock);
			if (error && error_size) snprintf(error, error_size, "The password contains characters that cannot be saved.");
			return false;
		}
		snprintf(stored[kind], sizeof(stored[kind]), "%s", pw);
		ok = sync_file();
	} else {
		stored[kind][0] = '\0';
		ok = sync_file();
	}
	pthread_mutex_unlock(&lock);
	if (!ok && error && error_size) {
		snprintf(error, error_size, "Could not save or remove the password file.");
	}
	return ok;
}
