#include "browserstate.h"

#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define STATE_PATH_MAX 512

typedef struct {
	char magic[8];
	int32_t scroll;
	char folder[STATE_PATH_MAX];
} browser_state_disk_t;

bool browser_state_save(const char *file, const char *folder, int scroll) {
	if (!file || !file[0] || !folder || !folder[0] || strlen(folder) >= STATE_PATH_MAX) return false;
	char temp[PATH_MAX];
	if (snprintf(temp, sizeof(temp), "%s.tmp", file) >= (int)sizeof(temp)) return false;
	browser_state_disk_t state = {{'S','N','X','B','R','W','1','\0'}, scroll < 0 ? 0 : scroll, {0}};
	memcpy(state.folder, folder, strlen(folder) + 1);
	FILE *f = fopen(temp, "wb");
	if (!f) return false;
	bool ok = fwrite(&state, sizeof(state), 1, f) == 1 && fflush(f) == 0 && fsync(fileno(f)) == 0;
	if (fclose(f) != 0) ok = false;
	if (ok && rename(temp, file) != 0) ok = false;
	if (!ok) unlink(temp);
	return ok;
}

bool browser_state_load(const char *file, char *folder, size_t size, int *scroll) {
	if (!file || !folder || !size || !scroll) return false;
	FILE *f = fopen(file, "rb");
	if (!f) return false;
	browser_state_disk_t state;
	bool ok = fread(&state, sizeof(state), 1, f) == 1 && fgetc(f) == EOF && !ferror(f);
	fclose(f);
	if (!ok || memcmp(state.magic, "SNXBRW1", 8) != 0 || !memchr(state.folder, '\0', sizeof(state.folder)) ||
	    !state.folder[0] || strlen(state.folder) >= size || state.scroll < 0) return false;
	memcpy(folder, state.folder, strlen(state.folder) + 1);
	*scroll = state.scroll;
	return true;
}

static bool inside(const char *root, const char *path) {
	size_t n = strlen(root);
	return strcmp(root, "/") == 0 ? path[0] == '/' :
	       strncmp(root, path, n) == 0 && (path[n] == '\0' || path[n] == '/');
}

bool browser_state_directory(const char *root, const char *folder, char *out, size_t size) {
	if (!root || !folder || !out || !size) return false;
	char real_root[PATH_MAX], real_folder[PATH_MAX];
	struct stat st;
	if (!realpath(root, real_root) || !realpath(folder, real_folder) ||
	    !inside(real_root, real_folder) || stat(real_folder, &st) != 0 || !S_ISDIR(st.st_mode)) return false;
	// Rebase the canonical suffix onto the configured mount spelling so the
	// browser can walk back to its root even when the mount is a symlink.
	const char *suffix = real_folder + strlen(real_root);
	if (strcmp(real_root, "/") == 0) suffix = real_folder + 1;
	int n = snprintf(out, size, "%s%s%s", root,
	                 strcmp(real_root, "/") == 0 && *suffix && strcmp(root, "/") != 0 ? "/" : "", suffix);
	return n >= 0 && (size_t)n < size;
}

bool browser_state_choose(const char *root, const char *saved, int saved_scroll,
                          const char *track, char *out, size_t size, int *scroll) {
	if (!root || !out || !size || !scroll) return false;
	*scroll = 0;
	if (saved && browser_state_directory(root, saved, out, size)) {
		*scroll = saved_scroll > 0 ? saved_scroll : 0;
		return true;
	}
	if (track && track[0] && strlen(track) < STATE_PATH_MAX) {
		char parent[STATE_PATH_MAX];
		struct stat st;
		memcpy(parent, track, strlen(track) + 1);
		char *slash = strrchr(parent, '/');
		if (slash && stat(track, &st) == 0 && S_ISREG(st.st_mode)) {
			if (slash == parent) parent[1] = '\0';
			else *slash = '\0';
			if (browser_state_directory(root, parent, out, size)) return true;
		}
	}
	char music[STATE_PATH_MAX];
	if (snprintf(music, sizeof(music), "%s/Music", root) < (int)sizeof(music) &&
	    browser_state_directory(root, music, out, size)) return true;
	// The card may not be mounted yet; the existing browser can show its root.
	if (strlen(root) >= size) return false;
	memcpy(out, root, strlen(root) + 1);
	return true;
}
