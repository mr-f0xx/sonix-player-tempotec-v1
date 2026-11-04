#define _GNU_SOURCE
#define HOST_BUILD 1
#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

// Exercise the real first-start cleanup's keep list in an isolated directory.
#include "../src/system/device/factoryreset.c"

static void touch_file(const char *path) {
	int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0600);
	assert(fd >= 0 && close(fd) == 0);
}

int main(void) {
	char root[] = "/tmp/sonix-reset-keep-XXXXXX";
	assert(mkdtemp(root));
	char journal[256], config[256], stock[256];
	assert(snprintf(journal, sizeof(journal), "%s/%s", root, USBHOST_JOURNAL_NAME) < (int)sizeof(journal));
	assert(snprintf(config, sizeof(config), "%s/device_config.ini", root) < (int)sizeof(config));
	assert(snprintf(stock, sizeof(stock), "%s/menu_cfg", root) < (int)sizeof(stock));
	touch_file(journal); touch_file(config); touch_file(stock);
	assert(kept(USBHOST_JOURNAL_NAME));
	clear_folder(root);
	assert(access(journal, F_OK) == 0 && access(config, F_OK) == 0);
	assert(access(stock, F_OK) != 0);
	assert(unlink(journal) == 0 && unlink(config) == 0 && rmdir(root) == 0);
	puts("PASS USB host journal survives first-start stock-data cleanup");
	return 0;
}
