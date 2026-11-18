#ifndef WIFITRANSFER_H
#define WIFITRANSFER_H

#include <stdbool.h>
#include <stdint.h>

#define WIFITRANSFER_URL_SIZE 80

typedef struct {
	char address[WIFITRANSFER_URL_SIZE]; // numeric URL, for the QR and browsers without mDNS
	char name[WIFITRANSFER_URL_SIZE];    // optional .local URL
	char ip[16];
	char code[16];
} wifitransfer_info_t;

typedef enum {
	WIFITRANSFER_OK,
	WIFITRANSFER_NO_WIFI,
	WIFITRANSFER_NO_CARD,
	WIFITRANSFER_START_FAILED,
} wifitransfer_result_t;

bool wifitransfer_available(void); // off in host emulators
// Binds the listener and creates a fresh pair code. No files or configuration
// are written to internal storage. Call on the interface thread for tr().
wifitransfer_result_t wifitransfer_start(uint32_t accent_rgb);
bool wifitransfer_get_enabled(void);
bool wifitransfer_running(void);
// Consumes a completion from the live session; never fires on switch-off.
bool wifitransfer_take_upload_complete(void);
// A consistent snapshot for the displayed URL, code and QR, only while live.
bool wifitransfer_get_info(wifitransfer_info_t *out);

// Stops accepting/serving requests and closes all card descriptors before
// returning. Used both by the page and before USB export, removal or shutdown.
void wifitransfer_release_card(void);

#endif /* WIFITRANSFER_H */
