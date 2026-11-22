// Unit tests for networkcredentials.c — the remembered passwords behind
// Network folders. Modeled on test_subsonic.c's credential tests: the file
// next to the config, written atomically, mode 0600, cleared on uncheck.
#include "src/system/net/networkcredentials.h"
#include "src/system/net/networkshare.h"
#include "src/system/core/config.h"
#include "src/system/core/json.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static char dir[64];
static char config_file[1024];
static char cred_file[1024];
static struct stat st;

static void init(void) {
	snprintf(config_file, sizeof(config_file), "%s/device.ini", dir);
	(void)config_init(config_file); // missing file means defaults, as at first boot
	snprintf(cred_file, sizeof(cred_file), "%s/network-credentials.json", dir);
	networkcredentials_init();
}

static void get(share_kind_t kind, char *out, size_t size, bool *remembered) {
	networkcredentials_get(kind, out, size, remembered);
}

static void expect_stored(share_kind_t kind, const char *expected) {
	char value[SHARE_LOGIN_MAX];
	bool remembered = false;
	get(kind, value, sizeof(value), &remembered);
	if (expected) {
		assert(remembered && strcmp(value, expected) == 0);
	} else {
		assert(!remembered && value[0] == '\0');
	}
}

static void check_file(void) {
	assert(stat(cred_file, &st) == 0 && (st.st_mode & 0777) == 0600);
	FILE *f = fopen(cred_file, "r");
	assert(f);
	char text[8192];
	size_t n = fread(text, 1, sizeof(text) - 1, f);
	assert(fclose(f) == 0);
	text[n] = '\0';
	json_doc_t doc;
	assert(json_parse(text, &doc));
	int root = json_root(&doc);
	char webdav[SHARE_LOGIN_MAX], ftp[SHARE_LOGIN_MAX];
	json_str_exact(&doc, json_get(&doc, root, "webdav"), webdav, sizeof(webdav));
	json_str_exact(&doc, json_get(&doc, root, "ftp"), ftp, sizeof(ftp));
	json_free(&doc);
	if (webdav[0]) assert(strcmp(webdav, "secret") == 0);
	if (ftp[0]) assert(strcmp(ftp, "ftp-secret") == 0);
}

int main(void) {
	snprintf(dir, sizeof(dir), "/tmp/sonix-networkcreds-XXXXXX");
	assert(mkdtemp(dir));
	init();

	// No file: nothing remembered, no error.
	expect_stored(SHARE_WEBDAV, NULL);
	expect_stored(SHARE_FTP, NULL);

	// Remember a WebDAV password: written atomically with 0600 perms.
	assert(access(cred_file, F_OK) != 0);
	char error[192];
	assert(networkcredentials_apply(SHARE_WEBDAV, "secret", true, error, sizeof(error)));
	assert(error[0] == '\0');
	assert(access(cred_file, F_OK) == 0);
	check_file();
	expect_stored(SHARE_WEBDAV, "secret");
	expect_stored(SHARE_FTP, NULL);

	// Same for FTP: the file holds both now.
	assert(networkcredentials_apply(SHARE_FTP, "ftp-secret", true, error, sizeof(error)));
	check_file();

	// Real JSON round-trip: re-init reads it back.
	networkcredentials_init();
	expect_stored(SHARE_WEBDAV, "secret");
	expect_stored(SHARE_FTP, "ftp-secret");

	// Unremember FTP: the file stays, the WebDAV entry stays.
	assert(networkcredentials_apply(SHARE_FTP, "", false, error, sizeof(error)));
	assert(access(cred_file, F_OK) == 0);
	networkcredentials_init();
	expect_stored(SHARE_WEBDAV, "secret");
	expect_stored(SHARE_FTP, NULL);

	// Unremember everything: the file goes away.
	assert(networkcredentials_apply(SHARE_WEBDAV, "", false, error, sizeof(error)));
	assert(access(cred_file, F_OK) != 0);
	networkcredentials_init();
	expect_stored(SHARE_WEBDAV, NULL);
	expect_stored(SHARE_FTP, NULL);

	// The guard works even for a session with no password to remember.
	assert(!networkcredentials_apply(SHARE_WEBDAV, "bad\npassword", true, error, sizeof(error)));
	assert(strstr(error, "cannot be saved"));
	assert(!networkcredentials_apply((share_kind_t)42, "x", true, error, sizeof(error)));
	assert(strstr(error, "Invalid share protocol"));

	// An oversized or unreadable file changes nothing.
	FILE *f = fopen(cred_file, "w");
	assert(f);
	for (int i = 0; i < 9000; i++) fputc('a', f);
	assert(fclose(f) == 0);
	networkcredentials_init();
	expect_stored(SHARE_WEBDAV, NULL);
	networkcredentials_init();

	f = fopen(cred_file, "w");
	assert(f);
	assert(fputs("{\"webdav\":\"secret\"}", f) >= 0);
	assert(fclose(f) == 0);
	networkcredentials_init();
	expect_stored(SHARE_WEBDAV, "secret");

	f = fopen(cred_file, "w");
	assert(f);
	assert(fputs("{\"webdav\":123}", f) >= 0); // a non-string value is not a password
	assert(fclose(f) == 0);
	networkcredentials_init();
	expect_stored(SHARE_WEBDAV, NULL);

	f = fopen(cred_file, "w");
	assert(f);
	assert(fputs("{\"ftp\":\"p\\u0000avoid\"}", f) >= 0); // embedded NUL is refused
	assert(fclose(f) == 0);
	networkcredentials_init();
	expect_stored(SHARE_FTP, NULL);

	// A symlink at the credential path is not followed.
	assert(unlink(cred_file) == 0 && symlink(config_file, cred_file) == 0);
	networkcredentials_init();
	expect_stored(SHARE_WEBDAV, NULL);
	assert(unlink(cred_file) == 0);

	// An undeletable file (it is a directory) makes the cleanup fail.
	assert(mkdir(cred_file, 0700) == 0);
	networkcredentials_init();
	expect_stored(SHARE_WEBDAV, NULL);
	assert(!networkcredentials_apply(SHARE_WEBDAV, "", false, error, sizeof(error)));
	assert(rmdir(cred_file) == 0);

	// Remembering an invalid password leaves the file alone.
	assert(networkcredentials_apply(SHARE_WEBDAV, "secret", true, error, sizeof(error)));
	networkcredentials_init();
	expect_stored(SHARE_WEBDAV, "secret");

	// The page guard: the file is still there.
	assert(access(cred_file, F_OK) == 0);
	assert(unlink(cred_file) == 0);
	assert(rmdir(dir) == 0);
	puts("PASS Network folders remembered passwords");
	return 0;
}
