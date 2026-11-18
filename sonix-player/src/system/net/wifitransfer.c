#define _GNU_SOURCE
#include "wifitransfer.h"

#ifdef HOST_BUILD
bool wifitransfer_available(void) { return false; }
wifitransfer_result_t wifitransfer_start(uint32_t accent_rgb) {
	(void)accent_rgb;
	return WIFITRANSFER_START_FAILED;
}
bool wifitransfer_get_enabled(void) { return false; }
bool wifitransfer_running(void) { return false; }
bool wifitransfer_take_upload_complete(void) { return false; }
bool wifitransfer_get_info(wifitransfer_info_t *out) { (void)out; return false; }
void wifitransfer_release_card(void) {}
#else
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "mdns.h"
#include "transferhttp.h"
#include "webpage.h"
#include "wifi.h"
#include "src/system/core/lang.h"
#include "src/system/device/system.h"

#define WT_NAME "sonix-transfer"
#ifndef WT_PORT
#define WT_PORT 80
#endif
#define WT_FALLBACK_PORT 4399
#define ACCENT_TOKEN "#3584e4"
#define ACCENT_PRESSED_TOKEN "#2b6fc4"

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static transfer_http_t *server;
static wifitransfer_info_t info;
static char *page;
static atomic_bool enabled;

// Render once per session, in RAM: the embedded page already contains its
// scripts, icons and styles, and no firmware web tree is needed on disk.
static bool write_text(FILE *out, const char *p, bool js) {
	for (; *p; p++) {
		const char *escaped = NULL;
		switch (*p) {
		case '&': if (!js) escaped = "&amp;"; break;
		case '<': escaped = js ? "\\u003c" : "&lt;"; break;
		case '>': if (!js) escaped = "&gt;"; break;
		case '\'': escaped = js ? "\\'" : "&#39;"; break;
		case '"': escaped = js ? "\\\"" : "&quot;"; break;
		case '\\': escaped = js ? "\\\\" : "&#92;"; break;
		case '\n': escaped = js ? "\\n" : "&#10;"; break;
		case '\r': escaped = js ? "\\r" : "&#13;"; break;
		default: break;
		}
		if ((escaped ? fputs(escaped, out) : fputc((unsigned char)*p, out)) == EOF) return false;
	}
	return true;
}

static bool render(FILE *out, uint32_t rgb) {
	uint32_t r = ((rgb >> 16) & 255) * 4 / 5;
	uint32_t g = ((rgb >> 8) & 255) * 4 / 5;
	uint32_t b = (rgb & 255) * 4 / 5;
	char accent[8], pressed[8];
	snprintf(accent, sizeof(accent), "#%06x", (unsigned)(rgb & 0xffffff));
	snprintf(pressed, sizeof(pressed), "#%06x", (unsigned)((r << 16) | (g << 8) | b));
	const char *p = WIFITRANSFER_PAGE, *run = p;
	while (*p) {
		size_t consumed = 0;
		bool js = false;
		const char *text = NULL;
		char key[256];
		if (p[0] == '{' && p[1] == '{') {
			const char *end = strstr(p + 2, "}}");
			if (end && (size_t)(end - (p + 2)) < sizeof(key)) {
				size_t len = (size_t)(end - (p + 2));
				memcpy(key, p + 2, len);
				key[len] = '\0';
				js = strncmp(key, "js|", 3) == 0;
				text = tr(key + (js ? 3 : 0));
				consumed = len + 4;
			}
		} else if (*p == '#') {
			if (!strncmp(p, ACCENT_TOKEN, 7)) text = accent;
			else if (!strncmp(p, ACCENT_PRESSED_TOKEN, 7)) text = pressed;
			if (text) consumed = 7;
		}
		if (!consumed) { p++; continue; }
		if (fwrite(run, 1, (size_t)(p - run), out) != (size_t)(p - run) || !write_text(out, text, js)) return false;
		p += consumed;
		run = p;
	}
	return fwrite(run, 1, (size_t)(p - run), out) == (size_t)(p - run);
}

static void format_url(char out[WIFITRANSFER_URL_SIZE], const char *name, uint16_t port) {
	if (port == 80) snprintf(out, WIFITRANSFER_URL_SIZE, "http://%s", name);
	else snprintf(out, WIFITRANSFER_URL_SIZE, "http://%s:%u", name, (unsigned)port);
}

bool wifitransfer_available(void) { return true; }
// Keep the power and playback guards active until the card has been released.
bool wifitransfer_get_enabled(void) { return atomic_load(&enabled); }
bool wifitransfer_running(void) { return wifitransfer_get_enabled(); }

bool wifitransfer_get_info(wifitransfer_info_t *out) {
	pthread_mutex_lock(&lock);
	bool live = transfer_http_running(server);
	if (live) *out = info;
	pthread_mutex_unlock(&lock);
	return live;
}

bool wifitransfer_take_upload_complete(void) {
	pthread_mutex_lock(&lock);
	bool complete = transfer_http_take_upload_complete(server);
	pthread_mutex_unlock(&lock);
	return complete;
}

static void stop_locked(void) {
	if (!server) return;
	transfer_http_stop(server);
	server = NULL;
	mdns_stop();
	free(page);
	page = NULL;
	memset(&info, 0, sizeof(info));
	atomic_store(&enabled, false);
}

wifitransfer_result_t wifitransfer_start(uint32_t rgb) {
	wifi_status_t status;
	wifi_get_status(&status);
	char ip[sizeof(info.ip)];
	if (status.state == WIFI_STATE_OFF || !wifi_interface_address(ip, sizeof(ip))) return WIFITRANSFER_NO_WIFI;

	char *html = NULL;
	size_t size = 0;
	FILE *out = open_memstream(&html, &size);
	if (!out) return WIFITRANSFER_START_FAILED;
	bool rendered = render(out, rgb);
	if (fclose(out) != 0 || !rendered) {
		free(html);
		return WIFITRANSFER_START_FAILED;
	}

	// The storage lock spans opening the card and publishing the server. USB
	// and hotplug can never miss a start while trying to release the card.
	// The lock order here and on removal is storage, then transfer.
	int card = storage_card_file_job_begin(false);
	if (card < 0) {
		free(html);
		return WIFITRANSFER_NO_CARD;
	}
	pthread_mutex_lock(&lock);
	if (!server) {
		server = transfer_http_start(card, ip, WT_PORT, WT_NAME ".local", html, size);
		if (!server) server = transfer_http_start(card, ip, WT_FALLBACK_PORT, WT_NAME ".local", html, size);
		if (server) {
			page = html;
			html = NULL;
			uint16_t port = transfer_http_port(server);
			format_url(info.address, ip, port);
			snprintf(info.ip, sizeof(info.ip), "%s", ip);
			snprintf(info.code, sizeof(info.code), "%s", transfer_http_code(server));
			if (mdns_start(WT_NAME, ip)) format_url(info.name, WT_NAME ".local", port);
			atomic_store(&enabled, true);
			printf("wifitransfer: listening at %s\n", info.address);
		}
	}
	bool live = transfer_http_running(server);
	if (!live) stop_locked();
	close(card);
	pthread_mutex_unlock(&lock);
	storage_card_file_job_end();
	free(html);
	return live ? WIFITRANSFER_OK : WIFITRANSFER_START_FAILED;
}

void wifitransfer_release_card(void) {
	pthread_mutex_lock(&lock);
	stop_locked();
	pthread_mutex_unlock(&lock);
}
#endif /* HOST_BUILD */
