// The real worker and libcurl transport, against the loopback fixtures in
// run_networkmusic_tests.py. Only the board's mounted-card gate is replaced.
#define _GNU_SOURCE 1
#include "src/system/device/system.h"
#include "src/system/streaming/networkmusic.h"

#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static const char *card;
static bool held;

const char *storage_sd_root(void) { assert(held); return card; }
int storage_card_file_job_begin(bool writable) {
	assert(writable && !held);
	int fd = open(card, O_DIRECTORY | O_RDONLY | O_CLOEXEC);
	held = fd >= 0;
	return fd;
}
void storage_card_file_job_end(void) { assert(held); held = false; }

static void validate_addresses(void) {
	share_server_t s;
	assert(networkshare_server_init(&s, SHARE_WEBDAV, "HTTPS://HOMESERVER:0443/dav/A%20B", "user", "s e:c#ret", NULL));
	assert(!strcmp(s.origin, "https://homeserver") && !strcmp(s.root, "/dav/A B/"));
	assert(!strcmp(s.password, "s e:c#ret"));
	char path[SHARE_FOLDER_MAX];
	assert(networkshare_child_folder(&s, s.root, "Album #1", path, sizeof(path)) && !strcmp(path, "/dav/A B/Album #1/"));
	assert(!networkshare_child_folder(&s, "/dav/", "Sibling", path, sizeof(path)));
	assert(!networkshare_child_folder(&s, s.root, "..", path, sizeof(path)));
	assert(!networkshare_child_folder(&s, s.root, "Bad/Name", path, sizeof(path)));
	assert(!networkshare_child_folder(&s, s.root, "Bad\nName", path, sizeof(path)));
	assert(networkshare_server_init(&s, SHARE_FTP, "ftp://[::1]:2121/", "", "", NULL));
	assert(!strcmp(s.origin, "ftp://[::1]:2121") && !strcmp(s.root, "/"));
	assert(!networkshare_server_init(&s, SHARE_FTP, "https://nas/", "", "", NULL));
	assert(!networkshare_server_init(&s, SHARE_WEBDAV, "https://nas/", "", "pass", NULL));
	assert(!networkshare_server_init(&s, SHARE_WEBDAV, "https://user:pass@nas/", "", "", NULL));
	assert(!networkshare_server_init(&s, SHARE_WEBDAV, "https://nas:65536/", "", "", NULL));
	assert(!networkshare_server_init(&s, SHARE_WEBDAV, "https://nas/folder?query", "", "", NULL));
	assert(!networkshare_server_init(&s, SHARE_WEBDAV, "https://nas/%2e%2E/folder", "", "", NULL));
	assert(!networkshare_server_init(&s, SHARE_WEBDAV, "https://nas/%2F/folder", "", "", NULL));
	assert(!networkshare_server_init(&s, SHARE_FTP, "ftp://nas/%0D%0A", "", "", NULL));
	assert(!networkshare_server_init(&s, SHARE_FTP, "ftp://nas/", "user\r\n", "pass", NULL));
}

static const char *const status_labels[] = {
	[SHARE_OK] = "OK", [SHARE_BAD_ADDRESS] = "BAD_ADDRESS", [SHARE_UNAVAILABLE] = "UNAVAILABLE",
	[SHARE_NETWORK_ERROR] = "NETWORK_ERROR", [SHARE_TIMEOUT] = "TIMEOUT", [SHARE_AUTH_ERROR] = "AUTH_ERROR",
	[SHARE_CERT_ERROR] = "CERT_ERROR", [SHARE_SERVER_ERROR] = "SERVER_ERROR", [SHARE_BAD_LIST] = "BAD_LIST",
	[SHARE_LIST_TOO_LARGE] = "LIST_TOO_LARGE", [SHARE_CANCELLED] = "CANCELLED", [SHARE_NO_CARD] = "NO_CARD",
	[SHARE_DISK_ERROR] = "DISK_ERROR", [SHARE_CARD_FULL] = "CARD_FULL", [SHARE_FILE_TOO_LARGE] = "FILE_TOO_LARGE",
	[SHARE_PATH_TOO_LONG] = "PATH_TOO_LONG", [SHARE_EXISTS] = "EXISTS", [SHARE_BUSY] = "BUSY", [SHARE_NO_MEMORY] = "NO_MEMORY",
};

static networkmusic_result_t wait_result(share_status_t status) {
	networkmusic_result_t out = {.status = status};
	if (status == SHARE_OK) {
		for (int i = 0; i < 9000; i++) {
			if (networkmusic_take_result(&out)) return out;
			usleep(5000);
		}
		assert(!"worker timed out");
	}
	return out;
}

int main(int argc, char **argv) {
	if (argc == 1) {
		validate_addresses();
		return 0;
	}
	assert(argc == 8);
	const char *mode = argv[1], *target = argv[7];
	share_kind_t kind = !strcmp(argv[2], "ftp") ? SHARE_FTP : SHARE_WEBDAV;
	share_server_t server;
	assert(networkshare_server_init(&server, kind, argv[3], argv[5], argv[6], getenv("SONIX_TEST_SHARE_CA")));
	card = argv[4];
	networkmusic_result_t out;
	if (!strcmp(mode, "list") || !strcmp(mode, "album")) {
		out = wait_result(networkmusic_list_start(&server, server.root));
		if (!strcmp(mode, "album") && out.status == SHARE_OK) {
			share_listing_t *listing = out.listing;
			out = wait_result(networkmusic_save_start(&server, server.root, listing->entries, listing->count));
			free(listing);
		}
	} else {
		share_entry_t entry = {0};
		assert(strlen(target) < sizeof(entry.name));
		strcpy(entry.name, target);
		share_status_t status = networkmusic_save_start(&server, server.root, &entry, 1);
		if (status == SHARE_OK && (!strcmp(mode, "cancel") || !strcmp(mode, "release"))) {
			networkmusic_progress_t progress;
			for (int i = 0; i < 1000; i++) {
				networkmusic_progress(&progress);
				if (progress.bytes_done) break;
				usleep(5000);
			}
			assert(progress.bytes_done > 0 && networkmusic_busy());
			if (!strcmp(mode, "release")) networkmusic_release_card();
			else networkmusic_cancel();
		}
		out = wait_result(status);
	}
	assert(!held && !networkmusic_busy());
	networkmusic_release_card(); // idempotent across shutdown and USB attempts
	printf("%s\n", status_labels[out.status]);
	if (out.listing) {
		printf("%s\n", out.listing->truncated ? "truncated" : "complete");
		for (size_t i = 0; i < out.listing->count; i++) {
			printf("%c\t%s\n", out.listing->entries[i].directory ? 'd' : 'f', out.listing->entries[i].name);
		}
		free(out.listing);
	} else {
		printf("%zu %zu\n%s\n", out.saved, out.existing, out.path);
	}
	return 0;
}
