#define _GNU_SOURCE
#define HOST_BUILD 1

#include <arpa/inet.h>
#include <assert.h>
#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include "src/system/core/config.h"
#include "src/system/device/sysinfo.h"
#include "src/system/device/system.h"
#include "src/system/net/wifi.h"
#include "src/system/streaming/stockkeys.h"
#include "src/system/streaming/streamkeys.h"

// Stubs for device/wifi hooks queried by stockkeys.c
static char stub_sd_root[512];
static bool stub_sd_writable = true;
static bool stub_wifi_connected = false;

const char *storage_sd_root(void) { return stub_sd_root[0] ? stub_sd_root : NULL; }
bool storage_sd_writable(void) { return stub_sd_writable; }
const char *sysinfo_ota_repo(void) { return "mr-f0xx/sonix-player-tempotec-v1"; }
const char *tr(const char *key) { return key; }

void wifi_get_status(wifi_status_t *out) {
	memset(out, 0, sizeof(*out));
	if (stub_wifi_connected) {
		out->state = WIFI_STATE_CONNECTED;
		snprintf(out->ip, sizeof(out->ip), "127.0.0.1");
	} else {
		out->state = WIFI_STATE_OFF;
	}
}

// ---------------------------------------------------------------------------
// Synthetic MIPS32 ELF fixture builder (reproduces stock hiby_player layout)
// ---------------------------------------------------------------------------

static void wr16le(uint8_t *p, uint16_t v) {
	p[0] = (uint8_t)(v & 0xFFu);
	p[1] = (uint8_t)(v >> 8);
}

static void wr32le(uint8_t *p, uint32_t v) {
	p[0] = (uint8_t)(v & 0xFFu);
	p[1] = (uint8_t)((v >> 8) & 0xFFu);
	p[2] = (uint8_t)((v >> 16) & 0xFFu);
	p[3] = (uint8_t)((v >> 24) & 0xFFu);
}

static uint32_t mips_lui(unsigned rt, unsigned imm) { return (0x0Fu << 26) | (rt << 16) | (imm & 0xFFFFu); }
static uint32_t mips_addiu(unsigned rt, unsigned rs, int simm) {
	return (0x09u << 26) | (rs << 21) | (rt << 16) | ((unsigned)simm & 0xFFFFu);
}
static uint32_t mips_sb(unsigned rt, unsigned rs, int simm) {
	return (0x28u << 26) | (rs << 21) | (rt << 16) | ((unsigned)simm & 0xFFFFu);
}

static size_t build_elf_fixture(uint8_t *buf, size_t cap, const char *qid, const char *qsec, const char *tid,
								const char *tsec) {
	memset(buf, 0, cap);
	assert(cap >= 4096);

	const uint32_t base_va = 0x00400000u;
	// Minimal 32-bit LE MIPS ELF header + 1 PT_LOAD covering [0 .. 4096)
	memcpy(buf, "\x7f" "ELF\x01\x01\x01\x00", 8);
	wr16le(buf + 16, 2);
	wr16le(buf + 18, 8);
	wr32le(buf + 20, 1);
	wr32le(buf + 28, 52); // e_phoff
	wr16le(buf + 40, 52);
	wr16le(buf + 42, 32); // e_phentsize
	wr16le(buf + 44, 1);  // e_phnum

	wr32le(buf + 52, 1);       // PT_LOAD
	wr32le(buf + 56, 0);       // p_offset
	wr32le(buf + 60, base_va); // p_vaddr
	wr32le(buf + 64, base_va); // p_paddr
	wr32le(buf + 68, 4096);    // p_filesz
	wr32le(buf + 72, 4096);    // p_memsz

	// .rodata strings starting at 0x600
	size_t ro = 0x600;
	uint32_t client_id_label_va = 0;
	uint32_t token_label_va = 0;

#define PUT_CSTR(str)                                                                                                  \
	do {                                                                                                               \
		size_t _l = strlen(str) + 1;                                                                                   \
		memcpy(buf + ro, (str), _l);                                                                                   \
		ro += (_l + 3u) & ~3u;                                                                                         \
	} while (0)

	PUT_CSTR("http://www.qobuz.com/api.json/0.2/");
	PUT_CSTR("device_manufacturer_id");
	PUT_CSTR(qid);
	PUT_CSTR(qsec);
	PUT_CSTR("https://auth.tidal.com/v1/oauth2/device_authorization");
	PUT_CSTR("urn:ietf:params:oauth:grant-type:device_code");
	PUT_CSTR(tsec);

	token_label_va = base_va + (uint32_t)ro;
	PUT_CSTR("x-tidal-token");
	client_id_label_va = base_va + (uint32_t)ro;
	PUT_CSTR("client_id");

#undef PUT_CSTR

	// Emit MIPS32 code at 0x100: first a decoy token referencing x-tidal-token,
	// then the real 16-char client_id referencing "client_id".
	size_t pc = 0x100;
	const char *decoy = "DecoyToken123456";
	for (size_t i = 0; i < strlen(decoy); i++) {
		wr32le(buf + pc, mips_addiu(2, 0, (unsigned char)decoy[i]));
		pc += 4;
		wr32le(buf + pc, mips_sb(2, 29, 16 + (int)i));
		pc += 4;
	}
	wr32le(buf + pc, mips_sb(0, 29, 16 + (int)strlen(decoy)));
	pc += 4;
	wr32le(buf + pc, mips_lui(5, token_label_va >> 16));
	pc += 4;
	wr32le(buf + pc, mips_addiu(5, 5, (int)(token_label_va & 0xFFFFu)));
	pc += 4;

	pc += 64;
	for (size_t i = 0; i < strlen(tid); i++) {
		wr32le(buf + pc, mips_addiu(3, 0, (unsigned char)tid[i]));
		pc += 4;
		wr32le(buf + pc, mips_sb(3, 29, 48 + (int)i));
		pc += 4;
	}
	wr32le(buf + pc, mips_sb(0, 29, 48 + (int)strlen(tid)));
	pc += 4;
	wr32le(buf + pc, mips_lui(6, client_id_label_va >> 16));
	pc += 4;
	wr32le(buf + pc, mips_addiu(6, 6, (int)(client_id_label_va & 0xFFFFu)));
	pc += 4;

	return 4096;
}

// ---------------------------------------------------------------------------
// Tiny local HTTP server for testing Wi-Fi streaming download + unpack
// ---------------------------------------------------------------------------

typedef struct {
	int listen_fd;
	char dir[512];
	volatile bool stop;
} mini_http_t;

static void *mini_http_main(void *arg) {
	mini_http_t *srv = arg;
	while (!srv->stop) {
		int cfd = accept(srv->listen_fd, NULL, NULL);
		if (cfd < 0) {
			if (errno == EINTR) {
				continue;
			}
			break;
		}
		char req[1024] = {0};
		ssize_t nr = recv(cfd, req, sizeof(req) - 1, 0);
		if (nr > 0 && strncmp(req, "GET /", 5) == 0) {
			char *path = req + 5;
			char *sp = strchr(path, ' ');
			if (sp) {
				*sp = '\0';
			}
			char full[2048];
			snprintf(full, sizeof(full), "%s/%s", srv->dir, path);
			FILE *f = fopen(full, "rb");
			if (f && fseeko(f, 0, SEEK_END) == 0) {
				long sz = (long)ftello(f);
				rewind(f);
				char hdr[256];
				int hl = snprintf(hdr, sizeof(hdr),
								  "HTTP/1.1 200 OK\r\nContent-Length: %ld\r\nConnection: close\r\n\r\n", sz);
				(void)send(cfd, hdr, (size_t)hl, MSG_NOSIGNAL);
				uint8_t buf[32768];
				size_t n;
				while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
					if (send(cfd, buf, n, MSG_NOSIGNAL) <= 0) {
						break;
					}
				}
				fclose(f);
			} else {
				if (f) {
					fclose(f);
				}
				const char *nf = "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
				(void)send(cfd, nf, strlen(nf), MSG_NOSIGNAL);
			}
		}
		close(cfd);
	}
	return NULL;
}

static void test_lzo_corrupt_bounds(void) {
	uint8_t out[64];
	size_t olen = 0;
	// Empty input succeeds with 0 bytes.
	assert(stockkeys_lzo1x_decompress((const uint8_t *)"", 0, out, sizeof(out), &olen) && olen == 0);
	// Valid minimal LZO1X EOF stream (17, 0, 0).
	const uint8_t eof_only[] = {17, 0, 0};
	assert(stockkeys_lzo1x_decompress(eof_only, sizeof(eof_only), out, sizeof(out), &olen) && olen == 0);
	// Truncated / out-of-bounds lookbehind streams must return false cleanly.
	const uint8_t bad_backref[] = {64, 5, 17, 0, 0};
	assert(!stockkeys_lzo1x_decompress(bad_backref, sizeof(bad_backref), out, sizeof(out), &olen));
	const uint8_t overflow_lits[] = {17 + 50, 'a', 'b', 'c'};
	assert(!stockkeys_lzo1x_decompress(overflow_lits, sizeof(overflow_lits), out, 4, &olen));
}

static void test_elf_extraction_and_merge(const char *tmp) {
	uint8_t elf[4096];
	size_t elen = build_elf_fixture(elf, sizeof(elf), "742719831", "0a1b2c3d4e5f60718293a4b5c6d7e8f9",
									"zX9yV8wU7tS6rQ5p", "AbCdEfGhIjKlMnOpQrStUvWxYz0123456789+/AbCdE=");

	stockkeys_pairs_t pairs;
	assert(stockkeys_extract_from_elf(elf, elen, &pairs));
	assert(pairs.has_qobuz && strcmp(pairs.qobuz_app_id, "742719831") == 0);
	assert(strcmp(pairs.qobuz_app_secret, "0a1b2c3d4e5f60718293a4b5c6d7e8f9") == 0);
	assert(pairs.has_tidal && strcmp(pairs.tidal_client_id, "zX9yV8wU7tS6rQ5p") == 0);
	assert(strcmp(pairs.tidal_client_secret, "AbCdEfGhIjKlMnOpQrStUvWxYz0123456789+/AbCdE=") == 0);

	// Seed an existing card streaming-keys.ini with [podcast] and [lastfm] and verify merge keeps them.
	char data_ini[512], card_dir[512], card_ini[540];
	snprintf(data_ini, sizeof(data_ini), "%s/usr_data/streaming-keys.ini", tmp);
	snprintf(card_dir, sizeof(card_dir), "%s/sdcard", tmp);
	assert(mkdir(card_dir, 0755) == 0);
	snprintf(card_ini, sizeof(card_ini), "%s/streaming-keys.ini", card_dir);

	FILE *f = fopen(card_ini, "wb");
	assert(f);
	fputs("[podcast]\napi_key = pod_k\napi_secret = pod_s\n\n[lastfm]\napi_key = lfm_k\napi_secret = lfm_s\n", f);
	fclose(f);

	setenv("SONIX_STREAMKEYS_DATA", data_ini, 1);
	config_init(NULL);
	assert(streamkeys_write_extracted(pairs.qobuz_app_id, pairs.qobuz_app_secret, pairs.tidal_client_id,
									  pairs.tidal_client_secret, card_dir));
	assert(streamkeys_reload(card_dir));
	assert(strcmp(streamkeys_qobuz_app_id(), "742719831") == 0);
	assert(strcmp(streamkeys_tidal_client_id(), "zX9yV8wU7tS6rQ5p") == 0);
	assert(strcmp(streamkeys_podcast_key(), "pod_k") == 0);
	assert(strcmp(streamkeys_lastfm_key(), "lfm_k") == 0);
	assert(access(data_ini, F_OK) == 0);
	assert(access(card_ini, F_OK) == 0);
}

static void test_bundled_stock_13_end_to_end(const char *tmp) {
	const char *part1 = "../../sonix-packer/firmware-input/v1.zip.part1";
	if (access(part1, R_OK) != 0) {
		printf("SKIP bundled stock 1.3 test (%s not present)\n", part1);
		return;
	}

	// 1. Direct file extraction from v1.zip.part1 (+ .part2) -> ZIP -> .upt -> SquashFS -> LZO -> MIPS ELF.
	stockkeys_pairs_t pairs;
	assert(stockkeys_extract_from_file(part1, tmp, &pairs));
	assert(pairs.has_qobuz && strlen(pairs.qobuz_app_id) == 9 && strlen(pairs.qobuz_app_secret) == 32);
	assert(pairs.has_tidal && strlen(pairs.tidal_client_id) == 16 && strlen(pairs.tidal_client_secret) == 44);

	// 2. Worker HTTP download + streaming ZIP inflate + extraction + save to /usr/data & card!
	int lfd = socket(AF_INET, SOCK_STREAM, 0);
	assert(lfd >= 0);
	int one = 1;
	setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
	struct sockaddr_in addr = {0};
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	addr.sin_port = 0;
	assert(bind(lfd, (struct sockaddr *)&addr, sizeof(addr)) == 0);
	assert(listen(lfd, 4) == 0);
	socklen_t alen = sizeof(addr);
	assert(getsockname(lfd, (struct sockaddr *)&addr, &alen) == 0);
	int port = ntohs(addr.sin_port);

	mini_http_t srv = {.listen_fd = lfd, .stop = false};
	snprintf(srv.dir, sizeof(srv.dir), "../../sonix-packer/firmware-input");
	pthread_t srv_tid;
	assert(pthread_create(&srv_tid, NULL, mini_http_main, &srv) == 0);

	char data_ini2[512], card_dir2[512];
	snprintf(data_ini2, sizeof(data_ini2), "%s/http_data/streaming-keys.ini", tmp);
	snprintf(card_dir2, sizeof(card_dir2), "%s/http_card", tmp);
	assert(mkdir(card_dir2, 0755) == 0);
	setenv("SONIX_STREAMKEYS_DATA", data_ini2, 1);
	snprintf(stub_sd_root, sizeof(stub_sd_root), "%s", card_dir2);
	stub_sd_writable = true;
	stub_wifi_connected = true;

	char url[256];
	snprintf(url, sizeof(url), "http://127.0.0.1:%d/v1.zip.part1", port);
	config_set("streaming", "stock_firmware_url", url);

	assert(stockkeys_start());
	while (stockkeys_state() == STOCKKEYS_RUNNING) {
		usleep(20000);
	}
	assert(stockkeys_result() == STOCKKEYS_OK);
	stockkeys_acknowledge();

	assert(streamkeys_reload(card_dir2));
	assert(strcmp(streamkeys_qobuz_app_id(), pairs.qobuz_app_id) == 0);
	assert(strcmp(streamkeys_tidal_client_id(), pairs.tidal_client_id) == 0);
	assert(strcmp(streamkeys_tidal_client_secret(), pairs.tidal_client_secret) == 0);

	// Verify temporary .upt.part was cleaned up.
	char leftover[560];
	snprintf(leftover, sizeof(leftover), "%s/.sonix-stock-keys.upt.part", card_dir2);
	assert(access(leftover, F_OK) != 0);

	srv.stop = true;
	shutdown(lfd, SHUT_RDWR);
	close(lfd);
	pthread_join(srv_tid, NULL);
}

int main(void) {
	char tmp[] = "/tmp/sonix-stockkeys-XXXXXX";
	assert(mkdtemp(tmp));

	test_lzo_corrupt_bounds();
	test_elf_extraction_and_merge(tmp);
	test_bundled_stock_13_end_to_end(tmp);

	char cmd[600];
	snprintf(cmd, sizeof(cmd), "rm -rf '%s'", tmp);
	assert(system(cmd) == 0);
	puts("PASS stockkeys LZO1X, ELF, SquashFS, .upt, split-ZIP & HTTP streaming extraction");
	return 0;
}
