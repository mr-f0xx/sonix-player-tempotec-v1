#ifndef USBHOSTPROBE_H
#define USBHOSTPROBE_H

#include <stdbool.h>

// Kept across the player's first-start cleanup so recovery can run before a
// gadget is allowed to claim a port left in forced host mode.
#define USBHOST_JOURNAL_NAME ".sonix-usb-host-probe"

// An explicit, temporary V1 host-role retry for an external USB audio device.
// This is not automatic detection: VBUS without a readable OTG ID cannot tell
// a DAC from a charger. The user must confirm that only a DAC is connected.
typedef enum {
	USBHOST_PROBE_STARTED,
	USBHOST_PROBE_BUSY,
	USBHOST_PROBE_UNSUPPORTED,
	USBHOST_PROBE_FAILED,
} usbhost_probe_result_t;

typedef enum {
	USBHOST_EVENT_NONE,
	USBHOST_EVENT_CARD,
	USBHOST_EVENT_TIMEOUT,
	USBHOST_EVENT_REMOVED,
	USBHOST_EVENT_RESTORE_FAILED,
	USBHOST_EVENT_RESTORED,
} usbhost_probe_event_t;

usbhost_probe_result_t usbhostprobe_begin(void);
usbhost_probe_event_t usbhostprobe_poll(bool audio_card, bool audio_interface);
// True only once both the PHY override and the original Type-C mode are back.
// If restoration fails, ownership is retained and the poll retries it.
bool usbhostprobe_cancel(void);
bool usbhostprobe_active(void);
// True while a failed/incomplete restore is still retaining the controller.
bool usbhostprobe_restore_pending(void);
// Call before Sonix starts a USB gadget. A journal on the settings partition
// retains an interrupted override across a player crash or reboot. Earlier
// stock init scripts can still claim the port; recovery then fails closed.
void usbhostprobe_recover(void);

#endif
