#include "usb.h"

#include "src/system/device/adb.h"
#include "src/system/device/power.h"
#include "src/system/device/sysinfo.h"
#include "src/system/device/system.h"
#include "src/system/device/usbexport.h"

#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <mntent.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <unistd.h>

#include "src/system/library/audiobookdb.h"
#include "src/system/library/library.h"
#include "src/system/core/logging.h"
#include "src/system/bluetooth/btlog.h"
#include "src/system/device/sysserver.h"
#include "src/system/audio/usbaudio.h"
#include "src/system/audio/usbhostprobe.h"
#include "src/system/core/utils.h"
#include "src/system/streaming/radio.h"
#include "src/gui/nowplaying/cover.h"

#include "lvgl/lvgl.h"

#include "src/gui/library/browser.h"
#include "src/gui/shell/gui.h"
#include "src/system/audio/audio.h"
#include "src/system/playback/device_state.h"
#include "src/system/library/geniusdb.h"


// The ADB init script (S440adb) builds its own gadget under this directory
// and binds it to the device's one USB controller. That matters here: with
// ADB enabled the mass-storage script's own gadget can never bind (the
// controller is busy). The card stays mounted while ADB is enabled; do not
// insert a raw medium into ADB's gadget or interrupt an active shell.
#define ADB_GADGET "/sys/kernel/config/usb_gadget/adb_demo"
#ifndef USB_UDC_CLASS
#define USB_UDC_CLASS "/sys/class/udc"
#endif
#ifndef USB_MOUNTS
#define USB_MOUNTS "/proc/mounts"
#endif

// VBUS detection. The first node is what the stock binary polls; the second
// is the charger's own (the stock reads that one too). Which of them actually
// flips to 1 on this hardware differs between drivers, so *all* of them are
// polled and any reading 1 means a cable -- the same any-supply logic the
// battery scan uses, which is why the charge icon worked while a single-node
// poll here saw nothing.
static const char *const VBUS_CANDIDATES[] = {
	"/sys/class/power_supply/usb/online",
	"/sys/class/power_supply/mp2731-charger/online",
};

// How often the cable is looked at. Attaching feels immediate at one second;
// the check is two sysfs reads.
#define POLL_SECONDS 1
#define POLL_SECONDS_STANDBY 4

static char sd_device[128];
static char sd_mount[256];

static atomic_bool storage_active;
// True from reservation until the external-host trial has restored the PHY
// and Type-C mode. The watcher reads it without taking storage_lock.
static atomic_bool external_host_reserved;

// Export and restore run on the watcher thread, and a restore is also asked for
// by the threads that start DAC mode and switch ADB. One at a time: a restore
// landing in the middle of an export would remount the card under a host that
// is about to be handed it.
static pthread_mutex_t storage_lock = PTHREAD_MUTEX_INITIALIZER;

// Counts the times the controller has changed hands (DAC mode, ADB). The
// watcher forgets what it knew about the cable when it moves: an eject done by
// the host before the handover says nothing about the device after it, and a
// card taken back by someone else is no longer exported.
static unsigned owner_serial;

static void owner_changed(void) { __atomic_add_fetch(&owner_serial, 1, __ATOMIC_SEQ_CST); }

// Tracks whether our LUN may have received the raw device, including a
// partially failed write. Only a verified empty LUN permits remounting.
static enum { EXPORT_NONE, EXPORT_OWN_GADGET } export_mode;
// Held from the successful normal unmount until the LUN is known to be empty.
static int export_claim_fd = -1;

// What the PC should see in its "device connected" notification: these two
// ids, and the strings below, named after the model this player is running on.
#define USB_ID_VENDOR "0x32BB"
// The product id the stock firmware gives its storage gadget (config.json calls
// it usb_pid, and keeps 0x0004 for the DAC's dac_pid). They were the same number
// here, so a host that had already met this device as a sound card was being
// shown a card reader wearing the same identity.
#define USB_ID_PRODUCT "0x0101"
#define USB_MANUFACTURER "HiBy"

// The product string: the model's name without a maker's prefix, which on this
// board is "TempoTec V1" as written in system-info.json.
const char *usb_product(void) {
	const sysinfo_model_t *model = sysinfo_model();
	if (!model) {
		return "TempoTec V1";
	}
	const char *name = model->name;
	return strncasecmp(name, "HiBy ", 5) == 0 ? name + 5 : name;
}

// What the LUN answers to a SCSI INQUIRY: vendor (8), product (16), revision
// (4). Without it the mass-storage function reports "Linux / File-Stor Gadget",
// which is what a bare gadget looks like; the stock writes its own, and some
// hosts -- Android among them -- are fussier about a card reader than a PC is.
static void usb_inquiry_string(char *out, size_t out_size) {
	char product[40];
	snprintf(product, sizeof(product), "%s U-DISK", usb_product());
	snprintf(out, out_size, "%-8s%-16.16s0100", USB_MANUFACTURER, product);
}

#define STORAGE_GADGET "/sys/kernel/config/usb_gadget/android0"
#ifndef STORAGE_UDC_PATH
#define STORAGE_UDC_PATH STORAGE_GADGET "/UDC"
#endif

// Protected by storage_lock; set only when the host trial parked our own
// disconnected gadget. It is rebound after normal role arbitration permits it.
static bool host_parked_storage;
static bool gadget_write_udc(const char *text);
static bool gadget_rebind(void);

bool usb_storage_active(void) { return storage_active; }

// ---------------------------------------------------------------------------
// UI notifications: the worker thread must not touch LVGL, so everything the
// user sees goes through gui_post onto the interface thread.
// ---------------------------------------------------------------------------

static void popup_connected_cb(void *unused) {
	(void)unused;
	gui_notify_popup("usb_storage_shared");
}

static void popup_disconnected_cb(void *unused) {
	(void)unused;
	// Covers the unplug and the PC-side eject alike.
	gui_notify_popup("usb_storage_available_again");
	browser_refresh(); // the PC may have changed the card content
}

static void popup_export_failed_cb(void *unused) {
	(void)unused;
	gui_notify_popup("usb_storage_export_failed");
}

static bool vbus_present(void) {
	for (size_t i = 0; i < sizeof(VBUS_CANDIDATES) / sizeof(VBUS_CANDIDATES[0]); i++) {
		char *content = read_file_content(VBUS_CANDIDATES[i]);
		if (!content) {
			continue;
		}
		bool on = content[0] == '1';
		free(content);
		if (on) {
			return true;
		}
	}
	return false;
}

// True while the ADB gadget exists and is bound to the USB controller (its
// UDC file holds the controller's name). Bound means the controller is busy
// and the mass-storage script's own gadget could never attach.
static bool adb_gadget_bound(void) {
	char *content = read_file_content(ADB_GADGET "/UDC");
	if (!content) {
		return false;
	}
	content[strcspn(content, "\r\n")] = '\0';
	bool bound = content[0] != '\0';
	free(content);
	return bound;
}

// Set while DAC mode owns the USB controller: nothing in this file may touch
// the gadget then. Without it the watcher thread sees a cable, calls
// ensure_storage_gadget() and rebuilds the mass-storage gadget straight over
// the audio one, taking the stream with it and handing the computer a card it
// should not have. Honoured in the two places that build or bind anything.
static atomic_bool dac_owns_usb;

// Entering and leaving DAC mode moves the card back and forth exactly the way
// a cable being plugged in does, which would announce "Memoria di nuovo
// disponibile" or "Collegato al PC" for something the user did not do and
// cannot see. The popups are silenced for the duration of the transition.
static atomic_bool suppress_popups;
static void ensure_storage_gadget(void);
static bool take_card_back(const char *why);

bool usb_set_dac_mode(bool on) {
	// The flag goes up before anything else: setting it afterwards leaves a
	// window in which the watcher thread can wake, see a cable and re-export
	// the card this call is in the middle of taking back.
	suppress_popups = true;
	if (on) {
		dac_owns_usb = true;
		// The gadget must not take a controller whose forced host override
		// could not be cleared. Cancellation serializes against a trial that
		// is starting at the same time.
		if (!usbhostprobe_cancel()) {
			dac_owns_usb = false;
			suppress_popups = false;
			return false;
		}
		// Take the card back from the computer. In DAC mode the player is a
		// sound card and nothing else: handing over the microSD at the same
		// time is not a bonus, it is the card being pulled out from under the
		// player while it works.
		//
		// Only when it was handed over. A restore with nothing exported still
		// unmounts and remounts the card the player is reading from.
		if (!take_card_back("DAC mode takes the controller")) {
			dac_owns_usb = false;
			suppress_popups = false;
			return false;
		}
	}
	dac_owns_usb = on;
	owner_changed();
	fprintf(stderr, "usb: controller %s\n", on ? "handed to DAC mode" : "back from DAC mode");
	if (!on) {
		// Entering DAC mode took the controller from ADB's gadget as well. With
		// ADB on it goes back to that one (adb_keep_controller(), at the next
		// poll): binding the card reader first would only be undone a second
		// later.
		if (adb_switched_on() && adb_is_running()) {
			fprintf(stderr, "usb: ADB is on; its gadget gets the controller back\n");
		} else {
			pthread_mutex_lock(&storage_lock);
			if (!usb_external_host_reserved()) ensure_storage_gadget();
			pthread_mutex_unlock(&storage_lock);
		}
	}
	suppress_popups = false;
	return true;
}

bool usb_dac_mode(void) {
	return dac_owns_usb;
}

// ---------------------------------------------------------------------------
// The Type-C role
//
// This is a dual-role port: the board carries a FUSB302B on the CC lines
// (module_driver/fusb302b.sh loads it with otg_id_gpio=PB24) and an MP2731 that
// can push 5 V back out, so the socket can be either end of a cable. Left to
// itself the controller advertises both and negotiates, which against a PC is
// no contest -- a PC only ever sources -- and this device lands as the
// peripheral every time.
//
// Against a phone it is a contest, and one this device tends to win. A phone is
// dual-role too and Android implements Try.SNK, meaning it would rather be the
// sink; so the two agree that the DAP is the host, the boost turns on, the DAP
// charges the phone, and the card is never offered to anything. From the user's
// side: "nothing happens". The stock player has exactly one function for this
// (set_usb_working_mode at 0x481f20 in hiby_player): it writes "sink" or "dual"
// to /sys/class/typec/port0/port_type and re-applies the saved choice at every
// boot, which is the "USB working mode: Device / Host" item in its menu.
//
// The host half is in use as well: USB-C headphones and USB DACs live on it
// (usbaudio.c). So the role is not pinned -- it is arbitrated, in
// arbitrate_port() over there, and the rule is that the port is dual while it
// is empty and becomes sink again a few seconds after something attaches that
// does not turn into a sound card. A phone is exactly that case; a port pinned
// dual for good could never connect to one.
//
// This function is the sink half of that rule, kept here because it also runs
// before the gadget is bound: it is the role that decides whether the
// controller has a gadget side at all.
//
// The R1 has none of this: no FUSB302B and no Type-C class, but a TCS1421 set
// through its own platform attribute. Its role is looked after entirely by the
// arbitration in usbaudio.c, and this function does nothing there.
// ---------------------------------------------------------------------------
static void typec_force_sink(void) {
	static bool announced;

	// Not while a DAC is being driven. This one line is the whole of the "the
	// sound stops a few seconds after the screen comes back on" bug: waking the
	// panel makes the loop below rebuild the storage gadget, rebuilding the
	// gadget writes "sink" here, and writing "sink" ends the host session the
	// headphones are living in. The port belongs to usbaudio.c while there is
	// something on it.
	if (usbaudio_active()) {
		return;
	}

	// Nor while something that may be a DAC is still coming up on the port: a
	// dongle powered from this port spends its first seconds starting, and a
	// sink write now takes its power away with the host role.
	if (usbaudio_handover_pending() || usb_external_host_reserved()) {
		return;
	}

	for (int port = 0; port < 4; port++) {
		char path[64];
		snprintf(path, sizeof(path), "/sys/class/typec/port%d/port_type", port);
		if (access(path, W_OK) != 0) {
			continue;
		}

		FILE *f = fopen(path, "w");
		if (!f) {
			continue;
		}
		bool ok = fputs("sink", f) >= 0;
		if (fclose(f) != 0) {
			ok = false;
		}
		if (!announced) {
			announced = true;
			fprintf(stderr, "usb: %s <- sink (%s)\n", path, ok ? "ok" : strerror(errno));
		}
		return;
	}

	if (!announced) {
		announced = true;
		fprintf(stderr, "usb: no writable /sys/class/typec/portN/port_type; leaving the role alone\n");
	}
}

// Builds the storage gadget -- descriptors with this device's real name, a
// mass-storage function with no medium in it -- and binds it to the USB
// controller. Idempotent: every step checks before it acts, so this both
// creates the gadget at startup and rebuilds it after ADB tore the configfs
// down.
//
// It is bound with an empty medium rather than only while exporting because a
// bound gadget is what lets the controller's state tell a PC from a wall
// charger: a PC enumerates the medium-less card reader and the UDC state reads
// CONFIGURED, a charger only supplies power and never does. The stock player
// makes the same distinction the same way, with its UAC gadget. Exporting is
// then just writing the block device into lun.0/file (the medium clicks in);
// un-exporting writes it empty again. The gadget itself never unbinds.
static void ensure_storage_gadget(void) {
	if (dac_owns_usb || usbaudio_active()) {
		return;
	}

	// The role first, every time: the gadget below is only worth binding to a
	// controller that is going to be the peripheral end of the cable.
	typec_force_sink();

	char inquiry[32];
	usb_inquiry_string(inquiry, sizeof(inquiry));
	const char *product = usb_product();

	char cmd[2048];
	// The descriptors are written every time the gadget is about to be bound,
	// not only when it is created. DAC mode builds its audio function in this
	// same gadget (usbdac.c) and leaves its own class, configuration name and
	// power draw behind; a card reader bound over those looks to the host like
	// an audio device with a disk in it, and a phone gives up on it.
	snprintf(cmd, sizeof(cmd),
			 "[ -d /sys/kernel/config/usb_gadget ] || mount -t configfs none /sys/kernel/config; "
			 "mkdir -p %s/strings/0x409 %s/configs/c.1/strings/0x409 && cd %s && "
			 "if ! grep -q '[a-zA-Z0-9]' UDC; then "
			 "echo 0x00 > bDeviceClass; "
			 "echo 0x00 > bDeviceSubClass; "
			 "echo 0x00 > bDeviceProtocol; "
			 "echo 0x0200 > bcdUSB; "
			 "echo 0x0100 > bcdDevice; "
			 "echo %s > idVendor; "
			 "echo %s > idProduct; "
			 "echo '%s' > strings/0x409/manufacturer; "
			 "echo '%s' > strings/0x409/product; "
			 "echo '%s %s' > strings/0x409/serialnumber; "
			 "echo 500 > configs/c.1/MaxPower; "
			 "echo 0x80 > configs/c.1/bmAttributes; "
			 "echo storage > configs/c.1/strings/0x409/configuration; "
			 "for l in configs/c.1/*; do "
			 "[ -L \"$l\" ] && [ \"$l\" != configs/c.1/mass_storage.0 ] && rm -f \"$l\"; "
			 "done; "
			 "fi; "
			 "if [ ! -d functions/mass_storage.0 ]; then "
			 "mkdir functions/mass_storage.0; "
			 "echo 0 > functions/mass_storage.0/lun.0/ro; "
			 "echo 1 > functions/mass_storage.0/lun.0/removable; "
			 "echo 1 > functions/mass_storage.0/lun.0/nofua; "
			 "echo 0 > functions/mass_storage.0/lun.0/cdrom; "
			 "echo '' > functions/mass_storage.0/lun.0/file; "
			 // Not every kernel has this attribute, and the shell reports a
			 // redirection it cannot make itself -- past the 2>/dev/null, which
			 // only silences the command -- so a kernel without it printed
			 // "can't create ...: Permission denied" into the log at every
			 // setup. Writing it only where it exists keeps the log clean on
			 // both kinds.
			 "[ -e functions/mass_storage.0/lun.0/inquiry_string ] && "
			 "echo '%s' > functions/mass_storage.0/lun.0/inquiry_string; "
			 "fi; "
			 "[ -e configs/c.1/mass_storage.0 ] || ln -s functions/mass_storage.0 configs/c.1; "
			 "grep -q '[a-zA-Z0-9]' UDC || echo $(ls /sys/class/udc/ | head -n1) > UDC",
			 STORAGE_GADGET, STORAGE_GADGET, STORAGE_GADGET, USB_ID_VENDOR, USB_ID_PRODUCT, USB_MANUFACTURER,
			 product, USB_MANUFACTURER, product, inquiry);
	int rc = system(cmd);
	(void)rc;
}

// Makes the host enumerate the gadget again, without rebuilding it: the same
// soft-disconnect toggle DAC mode uses (usbdac.c). Silently does nothing on a
// controller with no such node, where the medium simply appears the way it
// always has.
static void storage_reenumerate(void) {
	static const char *const PATHS[] = {
		"/sys/class/usb_gadget/android0/soft_disconnect",
		STORAGE_GADGET "/soft_disconnect",
		NULL,
	};

	for (int i = 0; PATHS[i]; i++) {
		if (access(PATHS[i], W_OK) != 0) {
			continue;
		}
		char cmd[256];
		snprintf(cmd, sizeof(cmd), "echo 1 > %s", PATHS[i]);
		int rc = system(cmd);
		(void)rc;
		usleep(300 * 1000);
		snprintf(cmd, sizeof(cmd), "echo 0 > %s", PATHS[i]);
		rc = system(cmd);
		(void)rc;
		fprintf(stderr, "usb: re-enumerated with the medium in, via %s\n", PATHS[i]);
		return;
	}
}

// True when a HOST has actually enumerated the bound gadget -- the check that
// tells a PC apart from a wall charger. VBUS alone cannot: a charger raises
// it too, and exporting on VBUS meant every plug-in-to-charge stopped the
// music and unmounted the card mid-listen.
// The name of the one USB device controller, or false when there is none.
static bool first_udc(char *out, size_t out_size) {
	DIR *dir = opendir(USB_UDC_CLASS);
	if (!dir) {
		return false;
	}
	bool found = false;
	struct dirent *de;
	while (!found && (de = readdir(dir)) != NULL) {
		if (de->d_name[0] == '.') {
			continue;
		}
		snprintf(out, out_size, "%s", de->d_name);
		found = true;
	}
	closedir(dir);
	return found;
}

static bool udc_configured(void) {
	char path[PATH_MAX];
	int n = snprintf(path, sizeof(path), "%s/13500000.otg_new/state", USB_UDC_CLASS);
	if (n < 0 || (size_t)n >= sizeof(path)) return false;
	char *content = read_file_content(path);
	if (!content) {
		// Firmware revision with another controller name: take the first one.
		//
		// By hand, not with popen. This runs once a second for as long as the
		// screen is on and there is no cable, and popen forks the whole player
		// to run a shell -- fifty-odd megabytes of page tables on a device with
		// fifty-six, once a second, next to a thread that has to hand the card
		// a period of audio on time. A directory and a small read cost nothing.
		char name[NAME_MAX + 1];
		if (!first_udc(name, sizeof(name))) {
			return false;
		}
		n = snprintf(path, sizeof(path), "%s/%s/state", USB_UDC_CLASS, name);
		if (n < 0 || (size_t)n >= sizeof(path)) return false;
		content = read_file_content(path);
		if (!content) {
			return false;
		}
		content[strcspn(content, "\r\n")] = '\0';
		bool configured = strcasecmp(content, "configured") == 0;
		free(content);
		return configured;
	}
	content[strcspn(content, "\r\n")] = '\0';
	bool configured = strcasecmp(content, "configured") == 0;
	free(content);
	return configured;
}

// This V1 kernel reports USB_UDC_DRIVER in uevent only when a gadget driver
// is bound. Unlike state, that also detects an *idle* unknown gadget. Refuse
// unfamiliar kernels/drivers; only our own disconnected android0 can be parked.
static bool udcs_disconnected(bool *own_bound) {
	DIR *dir = opendir(USB_UDC_CLASS);
	if (!dir) return false;
	int count = 0;
	bool safe = true;
	*own_bound = false;
	struct dirent *de;
	for (;;) {
		errno = 0;
		de = readdir(dir);
		if (!de) { if (errno) safe = false; break; }
		if (de->d_name[0] == '.') continue;
		if (++count != 1) { safe = false; break; } // the supported V1 has one
		char path[PATH_MAX];
		int n = snprintf(path, sizeof(path), "%s/%s/state", USB_UDC_CLASS, de->d_name);
		if (n < 0 || (size_t)n >= sizeof(path)) { safe = false; break; }
		char *state = read_file_content(path);
		if (!state) { safe = false; break; }
		state[strcspn(state, "\r\n")] = '\0';
		bool disconnected = strcasecmp(state, "not attached") == 0 || strcasecmp(state, "disconnected") == 0;
		free(state);
		if (!disconnected) { safe = false; break; }

		n = snprintf(path, sizeof(path), "%s/%s/uevent", USB_UDC_CLASS, de->d_name);
		if (n < 0 || (size_t)n >= sizeof(path)) { safe = false; break; }
		char *uevent = read_file_content(path);
		if (!uevent) { safe = false; break; }
		bool saw_name = false, saw_driver = false, known_driver = false;
		char *save = NULL;
		for (char *line = strtok_r(uevent, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
			line[strcspn(line, "\r")] = '\0';
			if (strncmp(line, "USB_UDC_NAME=", 13) == 0) {
				if (saw_name || !line[13]) { safe = false; break; }
				saw_name = true;
			} else if (strncmp(line, "USB_UDC_DRIVER=", 15) == 0) {
				if (saw_driver) { safe = false; break; }
				saw_driver = true;
				known_driver = strcmp(line + 15, "android0") == 0;
			}
		}
		free(uevent);
		if (!safe || !saw_name || (saw_driver && !known_driver)) { safe = false; break; }
		if (saw_driver) {
			char *bound = read_file_content(STORAGE_UDC_PATH);
			if (!bound) { safe = false; break; }
			bound[strcspn(bound, "\r\n")] = '\0';
			*own_bound = strcmp(bound, de->d_name) == 0;
			free(bound);
			if (!*own_bound) { safe = false; break; }
		}
	}
	if (closedir(dir) != 0) safe = false;
	return count == 1 && safe;
}

// Caller holds storage_lock. Never take another gadget away to make room.
static void resume_parked_storage_locked(void) {
	if (!host_parked_storage || usb_external_host_reserved() || dac_owns_usb || storage_active ||
		adb_switched_on() || adb_is_running() || usbaudio_handover_pending()) return;
	char *bound = read_file_content(STORAGE_UDC_PATH);
	bool already_bound = bound && bound[0] && bound[0] != '\n' && bound[0] != '\r';
	free(bound);
	if (already_bound || gadget_rebind()) {
		host_parked_storage = false;
		fprintf(stderr, "usbhost: the idle storage gadget is available again\n");
	}
}

bool usb_external_host_reserve(void) {
	// The UI must not wait behind an export's unmount/remount. The same lock
	// excludes a watcher operation which has already started.
	if (pthread_mutex_trylock(&storage_lock) != 0) return false;
	bool own = false, parked_now = false;
	bool ok = !atomic_load(&external_host_reserved) && !dac_owns_usb && !storage_active &&
		!usbaudio_handover_pending() && !adb_switched_on() && !adb_is_running() &&
		!adb_gadget_bound() && udcs_disconnected(&own);
	if (ok && own) {
		// A disconnected UDC may still have our gadget bound. Park only that
		// gadget, then verify that nothing else took the controller.
		// A failed/short sysfs write may nevertheless have unbound it, so
		// remember to reconcile the gadget even if the write reports failure.
		host_parked_storage = parked_now = true;
		bool unbound = gadget_write_udc("");
		bool still_bound = false;
		ok = unbound && udcs_disconnected(&still_bound) && !still_bound &&
			!dac_owns_usb && !storage_active && !usbaudio_handover_pending() &&
			!adb_switched_on() && !adb_is_running() && !adb_gadget_bound();
	}
	if (ok) atomic_store(&external_host_reserved, true);
	else if (parked_now) resume_parked_storage_locked();
	pthread_mutex_unlock(&storage_lock);
	if (!ok) fprintf(stderr, "usbhost: port is busy or its UDC state is unknown; trial refused\n");
	return ok;
}

void usb_external_host_release(void) {
	pthread_mutex_lock(&storage_lock);
	atomic_store(&external_host_reserved, false);
	resume_parked_storage_locked();
	pthread_mutex_unlock(&storage_lock);
}
void usb_external_host_block_recovery(void) { atomic_store(&external_host_reserved, true); }
bool usb_external_host_reserved(void) { return atomic_load(&external_host_reserved); }

bool usb_external_host_restore_safe(void) {
	pthread_mutex_lock(&storage_lock);
	bool own = false;
	// wanted/DAC flags can represent a request waiting for this restoration;
	// inspect actual gadgets and the running daemon, not those request flags.
	bool ok = !storage_active && !adb_is_running() && !adb_gadget_bound() && udcs_disconnected(&own);
	if (ok && own) {
		// Another process may have rebound our idle gadget after a crash. It
		// is safe to park only after the UDC is verified disconnected. A
		// reported-failed write can still have changed the binding.
		host_parked_storage = true;
		ok = gadget_write_udc("");
		if (ok) {
			bool still_bound = false;
			ok = udcs_disconnected(&still_bound) && !still_bound && !adb_is_running() && !adb_gadget_bound();
		}
	}
	pthread_mutex_unlock(&storage_lock);
	return ok;
}

// Takes the storage gadget off the USB controller. Used both to hand the
// controller to ADB and to stop paying for the PHY while the device sits in
// standby with no cable.
// Binds the gadget to the controller, or lets go of it. Two writes to one file:
// the controller's name to take it, an empty line to give it back.
//
// Written straight, not through a shell, and for the same reason as first_udc()
// above: this pair runs on the standby and wake path, which is exactly when
// music is playing with the screen dark. A fork here is heard.
static bool gadget_write_udc(const char *text) {
	FILE *f = fopen(STORAGE_UDC_PATH, "w");
	if (!f) {
		return false;
	}
	bool ok = fprintf(f, "%s\n", text) >= 0;
	if (fclose(f) != 0) {
		ok = false; // the driver refusing the bind surfaces here, at close
	}
	return ok;
}

static void gadget_unbind(void) { gadget_write_udc(""); }

// Configfs rejects writing an empty UDC to an already-unbound gadget. Treat
// that case (and an absent gadget) as success, but never hide a failed read or
// a failed unbind of a gadget that really is attached.
static bool gadget_unbind_if_bound(void) {
	char *bound = read_file_content(STORAGE_UDC_PATH);
	if (!bound) {
		if (access(STORAGE_UDC_PATH, F_OK) == 0) return false;
		return errno == ENOENT;
	}
	bound[strcspn(bound, "\r\n")] = '\0';
	bool attached = bound[0] != '\0';
	free(bound);
	return !attached || gadget_write_udc("");
}

// Puts the gadget back on the controller. False when there is no gadget to put
// back -- it has never been built, or something removed it -- and the caller
// falls back to building one.
static bool gadget_rebind(void) {
	char udc[NAME_MAX + 1];
	if (access(STORAGE_UDC_PATH, W_OK) != 0 || !first_udc(udc, sizeof(udc))) {
		return false;
	}
	return gadget_write_udc(udc);
}

// Called right before ADB's init script starts: two gadgets cannot bind at
// once.
//
// A card handed to the host comes back first. With ADB on the card stays with
// the player (see the watcher), and unbinding under an exported medium left it
// unmounted here and gone from the host, until the cable was pulled.
bool usb_gadget_yield_to_adb(void) {
	if (!usbhostprobe_cancel()) {
		fprintf(stderr, "usb: cannot start ADB while the host override is awaiting restoration\n");
		return false;
	}
	if (!take_card_back("ADB is starting")) return false;
	pthread_mutex_lock(&storage_lock);
	if (usb_external_host_reserved()) {
		pthread_mutex_unlock(&storage_lock);
		return false;
	}
	owner_changed();
	bool ok = gadget_unbind_if_bound();
	pthread_mutex_unlock(&storage_lock);
	return ok;
}

// Called after ADB shuts down. Its stop script also unmounts the whole
// configfs, so the gadget is rebuilt from nothing.
//
// A card that was riding on ADB's gadget comes back first; the watcher then
// hands it to the host again through the card reader, if a host is there.
void usb_gadget_reclaim_from_adb(void) {
	if (!take_card_back("ADB stopped")) return;
	pthread_mutex_lock(&storage_lock);
	if (!usb_external_host_reserved() && !dac_owns_usb) {
		// The composite restore binds ADB's gadget again, and with adbd gone
		// that gadget has nothing left to carry but still holds the controller.
		if (!adb_is_running()) {
			FILE *f = fopen(ADB_GADGET "/UDC", "w");
			if (f) {
				fputs("\n", f);
				fclose(f);
			}
		}
		ensure_storage_gadget();
	}
	owner_changed();
	pthread_mutex_unlock(&storage_lock);
}

// ADB switched on and adbd up, and yet its gadget is not on the controller:
// the card reader's is, or nothing is. Hands the controller over.
//
// It happens at boot. With the switch left on, adbd is started first, by the
// firmware's script, and it binds its gadget on its own a moment later (on
// the R1 adbd runs /sbin/usb_adb_enable.sh once its endpoints are open). The
// card reader's gadget is built during that moment, takes the one controller,
// and adbd's bind fails for good: a PC then finds an empty card reader and no
// ADB, until the switch is turned off and on again.
//
// Checked on every poll, and cheap when all is well: one read of the ADB
// gadget's UDC file. adbd is looked for in /proc only when that is empty, and
// nothing is written unless the gadget is ready to bind (its functionfs is
// mounted, which adbd needs before it can write its descriptors).
static void adb_keep_controller(void) {
	if (!adb_switched_on() || adb_gadget_bound() || access(ADB_GADGET "/functions/ffs.adb", F_OK) != 0) {
		return;
	}
	if (!adb_is_running()) {
		return;
	}

	char udc[NAME_MAX + 1];
	if (!first_udc(udc, sizeof(udc))) {
		return;
	}
	gadget_unbind(); // the card reader lets go; failing when it holds nothing is fine

	bool ok = false;
	FILE *f = fopen(ADB_GADGET "/UDC", "w");
	if (f) {
		ok = fprintf(f, "%s\n", udc) >= 0;
		if (fclose(f) != 0) {
			ok = false;
		}
	}

	// Once per outcome: adbd not yet ready is retried every poll.
	static int said = -1;
	if (said != (int)ok) {
		said = ok;
		fprintf(stderr, "usb: ADB is on but its gadget was not bound; %s\n",
				ok ? "handed it the controller" : "adbd is not ready yet, retrying");
	}
}

// ---------------------------------------------------------------------------
// Around a system suspend
//
// The storage gadget is deliberately kept bound to the USB controller at all
// times (see above: a bound gadget is what lets the controller tell a PC from
// a wall charger). That is fine while the board is awake and fatal when it
// tries to sleep: suspending with a live gadget on the controller makes the
// device answer with a reset instead of a resume, and the player starts from
// scratch.
//
// So the controller is handed nothing at all before the SoC goes down, and the
// gadget is rebuilt on the way back. The stock player does not need this: it
// only builds a gadget while a host is actually attached.
// ---------------------------------------------------------------------------

// The cable, as the USB subsystem itself sees it -- not as the battery poll's
// five-second-old "charging" reading sees it. The suspend rule needs the
// truthful answer: a PC that enumerates the card must never have the gadget
// pulled out from under it.
bool usb_vbus_present(void) { return vbus_present(); }

void usb_suspend_prepare(void) {
	if (!sd_device[0]) {
		return; // USB mode was never started (host build, or no card)
	}
	pthread_mutex_lock(&storage_lock);
	if (!usb_external_host_reserved()) {
		// Every gadget, not just ours: ADB may own the controller.
		int rc = system("for u in /sys/kernel/config/usb_gadget/*/UDC; do echo '' > $u 2>/dev/null; done");
		(void)rc;
		fprintf(stderr, "usb: gadgets unbound for suspend\n");
	}
	pthread_mutex_unlock(&storage_lock);
}

void usb_resume_restore(void) {
	if (!sd_device[0]) {
		return;
	}
	pthread_mutex_lock(&storage_lock);
	if (!usb_external_host_reserved()) {
		ensure_storage_gadget();
		fprintf(stderr, "usb: storage gadget rebound after resume\n");
	}
	pthread_mutex_unlock(&storage_lock);
}

#ifndef STORAGE_LUN_PATH
#define STORAGE_LUN_PATH STORAGE_GADGET "/functions/mass_storage.0/lun.0/file"
#endif

// Verify the actual LUN after a write. A failed/partial removal must not be
// followed by remounting a device that the computer might still be using.
static bool write_lun(const char *value) {
	FILE *f = fopen(STORAGE_LUN_PATH, "w");
	if (!f) return false;
	bool ok = fprintf(f, "%s\n", value) >= 0;
	if (fclose(f) != 0) ok = false;
	if (!ok) return false;
	f = fopen(STORAGE_LUN_PATH, "r");
	if (!f) return false;
	char actual[PATH_MAX];
	bool got = fgets(actual, sizeof(actual), f) != NULL;
	if (!got) actual[0] = '\0';
	bool read_ok = !ferror(f);
	fclose(f);
	if (!read_ok) return false;
	actual[strcspn(actual, "\r\n")] = '\0';
	if (strcmp(actual, value) == 0) return true;
	// file_path() resolves symlinks, so compare block-device identities too.
	struct stat a, b;
	return value[0] && actual[0] && stat(value, &a) == 0 && stat(actual, &b) == 0 &&
		S_ISBLK(a.st_mode) && S_ISBLK(b.st_mode) && a.st_rdev == b.st_rdev;
}

// 1 if mounted, 0 if absent, -1 if it cannot be checked. A read failure must
// not be mistaken for evidence that a raw card is safe to hand to a host.
static int mount_status(void) {
	char wanted[PATH_MAX];
	if (!realpath(sd_mount, wanted)) return -1;
	FILE *f = setmntent(USB_MOUNTS, "r");
	if (!f) return -1;
	int found = 0;
	struct mntent current, *entry;
	char line[8192];
	for (;;) {
		errno = 0;
		entry = getmntent_r(f, &current, line, sizeof(line));
		if (!entry) { if (errno || ferror(f)) found = -1; break; }
		if (strcmp(entry->mnt_dir, wanted) == 0) { found = 1; break; }
	}
	if (ferror(f)) found = -1;
	endmntent(f);
	return found;
}

// True when the host has ejected the medium: on a removable lun the "safely
// remove" on the PC closes the gadget's backing file, so lun.0/file reads
// empty. That is the signal to take the card back -- otherwise the player
// sits saying "no card" until the cable is physically pulled.
static bool host_ejected(void) {
	char *content = read_file_content(STORAGE_LUN_PATH);
	if (!content) {
		return false; // node unreadable: assume still exported, do nothing rash
	}
	content[strcspn(content, "\r\n")] = '\0';
	bool ejected = content[0] == '\0';
	free(content);
	return ejected;
}

// Reopen services only after the card is mounted again. A failed remount is
// left to the card supervisor; do not create databases on the bare internal
// directory. Called with storage_lock and storage_active held.
static bool resume_card(bool host_changed_card) {
	if (!storage_usb_begin()) {
		// Shutdown has taken ownership. The LUN is already clear; release
		// our claim but do not remount or reopen services during power-off.
		if (export_claim_fd >= 0) close(export_claim_fd);
		export_claim_fd = -1;
		storage_active = false;
		return true;
	}
	int status = mount_status();
	if (status < 0) {
		fprintf(stderr, "usb: cannot inspect the card mount; keeping it reserved\n");
		storage_usb_end();
		return false;
	}
	if (host_changed_card && status == 1) {
		// This should not occur while our exclusive claim is held. Never use
		// lazy detach here: an old filesystem still in use is not safe to read.
		fprintf(stderr, "usb: unexpected mount after export; removing it normally\n");
		if (umount(sd_mount) != 0 || mount_status() != 0) {
			fprintf(stderr, "usb: unexpected mount is busy or unknown; keeping the card reserved\n");
			storage_usb_end();
			return false;
		}
		status = 0;
	}
	if (export_claim_fd >= 0) {
		close(export_claim_fd);
		export_claim_fd = -1;
	}

	for (int attempt = 0; attempt < 6 && status == 0; attempt++) {
		if (attempt) sleep(1);
		if (!(sysserver_available() && sysserver_mount(sd_device, sd_mount) == 0)) {
			// Match startup's ordering: prefer read/write ntfs-3g to the
			// kernel's read-only ntfs fallback.
			char remount[1600];
			snprintf(remount, sizeof(remount),
					 "mount -t exfat,vfat,ext4,ext3,ext2,ntfs3 %s %s 2>/dev/null || "
					 "ntfs-3g %s %s -o force,remove_hiberfile,big_writes,noatime 2>/dev/null || "
					 "mount -t ntfs %s %s",
					 sd_device, sd_mount, sd_device, sd_mount, sd_device, sd_mount);
			int mrc = system(remount);
			(void)mrc;
		}
		status = mount_status();
		if (status < 0) break;
	}
	bool mounted = status == 1;
	fprintf(stderr, "usb: card remounted on %s: %s\n", sd_mount, mounted ? "yes" : "NO");
	if (mounted) {
		char db_path[512];
		snprintf(db_path, sizeof(db_path), "%s/.local/library.db", sd_mount);
		library_open(db_path);
		snprintf(db_path, sizeof(db_path), "%s/.local/audiobooks.db", sd_mount);
		audiobookdb_open(db_path);
		snprintf(db_path, sizeof(db_path), "%s/.local/genius.db", sd_mount);
		geniusdb_open(db_path);
		logging_resume_after_usb();
		btlog_card_attach(sd_mount);
		cover_set_cache_dir(sd_mount);
		radio_store_open(sd_mount);
	}
	// The LUN is empty now. If the card could not be mounted, allow the
	// supervisor to detach its old state and recover on a subsequent poll.
	storage_active = false;
	if (mounted) {
		library_card_returned(sd_mount);
		if (host_changed_card && !suppress_popups) gui_post(popup_disconnected_cb, NULL);
	}
	storage_usb_end();
	return true;
}

// Hands the card to the host only after a normal, verified unmount and an
// exclusive claim. A failed attempt rolls back; it is not repeated every poll.
static bool storage_export(void) {
	fprintf(stderr, "usb: host attached, preparing to export %s\n", sd_device);
	if (!storage_usb_begin()) return false; // shutdown already owns the card
	storage_active = true; // keep the card supervisor from remounting mid-export
	storage_usb_end();

	device_state_note_storage_gone();
	audio_stop();
	library_close();
	audiobookdb_close();
	geniusdb_close();
	logging_suspend_for_usb();
	btlog_card_release();
	cover_set_cache_dir(NULL);
	radio_store_close();

	bool unmounted = false;
	export_claim_fd = usbexport_detach_and_claim(sd_device, sd_mount, &unmounted);
	if (export_claim_fd < 0) {
		fprintf(stderr, "usb: export abandoned; card was%s unmounted\n", unmounted ? "" : " not");
		(void)resume_card(false);
		return false;
	}

	// Serialize the final handover with power-off as well as hotplug.
	if (!storage_usb_begin()) {
		close(export_claim_fd);
		export_claim_fd = -1;
		storage_active = false;
		return false;
	}
	// ADB deliberately keeps the card mounted. It may have been requested
	// while the unmount ran, so check again before touching the LUN. Likewise,
	// a disconnected PC or a new USB owner must not receive the raw device.
	if (dac_owns_usb || usb_external_host_reserved() || usbaudio_handover_pending() ||
		adb_switched_on() || adb_is_running() || adb_gadget_bound() || !udc_configured()) {
		storage_usb_end();
		fprintf(stderr, "usb: USB ownership changed; export abandoned\n");
		(void)resume_card(false);
		return false;
	}

	export_mode = EXPORT_OWN_GADGET; // a partial write may already have opened it
	bool inserted = write_lun(sd_device);
	storage_usb_end();
	if (!inserted) {
		fprintf(stderr, "usb: medium insertion failed; clearing the LUN\n");
		if (write_lun("")) {
			export_mode = EXPORT_NONE;
			(void)resume_card(false);
		} else {
			fprintf(stderr, "usb: LUN removal could not be verified; card stays reserved\n");
		}
		return false;
	}
	storage_reenumerate();
	int lrc = system("for u in /sys/class/udc/*/state; do echo \"usb: udc $u: $(cat $u)\" >&2; done");
	(void)lrc;
	if (!suppress_popups) gui_post(popup_connected_cb, NULL);
	return true;
}

// Returns false without remounting if the LUN might still own the raw card.
static bool storage_restore(void) {
	fprintf(stderr, "usb: host detached, reclaiming %s\n", sd_device);
	if (export_mode == EXPORT_OWN_GADGET && !write_lun("")) {
		fprintf(stderr, "usb: LUN removal could not be verified; card stays reserved\n");
		return false;
	}
	// Keep the mode until the card is safely remounted (or handed back to the
	// supervisor), so a transient failure can be retried.
	bool was_shared = export_mode == EXPORT_OWN_GADGET;
	if (!resume_card(was_shared)) return false;
	export_mode = EXPORT_NONE;
	return true;
}

// Takes the card back from the host if it was handed over, from any thread.
static bool take_card_back(const char *why) {
	pthread_mutex_lock(&storage_lock);
	bool ok = true;
	if (storage_active) {
		fprintf(stderr, "usb: %s; taking the card back from the host\n", why);
		ok = storage_restore();
	}
	pthread_mutex_unlock(&storage_lock);
	return ok;
}

typedef enum { EXPORT_SKIPPED, EXPORT_FAILED, EXPORT_OK } export_result_t;
// The watcher's own export and restore, under the same lock.
static export_result_t locked_export(void) {
	pthread_mutex_lock(&storage_lock);
	export_result_t result = EXPORT_SKIPPED;
	if (!usb_external_host_reserved() && !dac_owns_usb && !usbaudio_handover_pending()) {
		result = storage_export() ? EXPORT_OK : EXPORT_FAILED;
	}
	pthread_mutex_unlock(&storage_lock);
	return result;
}

static bool locked_restore(void) {
	pthread_mutex_lock(&storage_lock);
	bool ok = !storage_active || storage_restore();
	pthread_mutex_unlock(&storage_lock);
	return ok;
}

static void *usb_thread(void *arg) {
	(void)arg;
	thread_be_background("usb watcher");

	bool exported = false;
	// Set after the PC ejects the medium: the card is back but the cable is
	// still in, and re-exporting now would undo the eject the user just asked
	// for. Cleared when VBUS finally drops.
	bool await_unplug = false;

	// Said once, not once a second.
	bool said_adb = false;

	// The gadget is unbound from the controller while the device sits in
	// standby with no cable (see below).
	bool parked = false;

	unsigned seen_owner = __atomic_load_n(&owner_serial, __ATOMIC_SEQ_CST);

	for (;;) {
		// DAC mode owns the controller: stand completely down. Not just "do
		// not rebuild" -- do not look, do not export, do not park. Everything
		// this loop does ends in a write to the gadget, and every one of those
		// writes would land on the audio gadget instead.
		if (dac_owns_usb) {
			sleep(1);
			continue;
		}

		// And the same for a DAC or a pair of headphones on the port: the
		// controller is the host's then, and everything below ends in a write
		// to a gadget that has no business existing while it is.
		if (usbaudio_active() || usb_external_host_reserved()) {
			sleep(1);
			continue;
		}

		// Slower while the panel is dark. A cable being plugged in is
		// something the user just did, so a second's resolution is worth
		// having with the screen on; in standby it is one more thread waking
		// the CPU every second for the whole night.
		sleep(power_screen_is_on() ? POLL_SECONDS : POLL_SECONDS_STANDBY);

		// A host-side audio interface or card may have appeared while this
		// thread slept. Check again *before* rebinding ADB, parking a gadget or
		// exporting the card. The interface can precede its ALSA card, and the
		// latter can be present even when Bluetooth owns the playback route.
		if (dac_owns_usb || usbaudio_active() || usbaudio_handover_pending() || usb_external_host_reserved()) {
			continue;
		}

		// If an audio handover or ADB delayed putting our idle gadget back
		// after a host trial, resume it once the other owner has finished.
		pthread_mutex_lock(&storage_lock);
		resume_parked_storage_locked();
		pthread_mutex_unlock(&storage_lock);

		// The controller changed hands since the last look: whatever was
		// known about the cable belongs to the gadget before. And a card
		// taken back by DAC mode or ADB is not exported any more, whatever
		// this loop last did with it: believing otherwise read the emptied
		// medium as an eject by the host, and the card was not offered again
		// until the cable was pulled.
		unsigned owner = __atomic_load_n(&owner_serial, __ATOMIC_SEQ_CST);
		if (owner != seen_owner) {
			seen_owner = owner;
			await_unplug = false;
			said_adb = false;
		}
		if (exported && !storage_active) {
			exported = false;
		}

		pthread_mutex_lock(&storage_lock);
		if (!usb_external_host_reserved() && !dac_owns_usb && !usbaudio_handover_pending()) {
			adb_keep_controller();
		}
		pthread_mutex_unlock(&storage_lock);

		// A host that has enumerated this device is a cable, whatever the
		// charger says. On a phone the two can disagree: the power role and the
		// data role are separate on Type-C, so a phone can act as the host
		// while the charger node reads nothing at all -- and treating that as
		// "no cable" is how the gadget ended up being unbound from under a host
		// that was talking to it.
		bool vbus = vbus_present() || udc_configured();

		if (!vbus) {
			if (exported) {
				if (!locked_restore()) continue;
				exported = false;
			}
			await_unplug = false;

			// No cable, panel dark: take the gadget off the controller. A
			// bound gadget keeps the USB PHY and the OTG block clocked --
			// /sys/kernel/debug/clk/clk_summary shows gate_usbphy and
			// gate_otg alive a minute into standby with nothing plugged in --
			// and that is current spent waiting for a cable that is not there.
			// The stock player only ever builds a gadget once a host has
			// turned up; this is the same idea, but only in standby, so
			// nothing about plugging a cable into a device in use changes.
			//
			// Never while ADB is running: the controller is ADB's then, and
			// reclaiming it would cut the connection.
			//
			// And not while audio is open either. Both of these are
			// housekeeping for a cable that is not attached: nothing is
			// waiting for the gadget, so there is no reason to touch the
			// controller in the middle of a track -- a rebind runs the whole
			// gadget script, shell and all, and costs a hole in the sound.
			// Both happen at the next poll after playback stops, and a cable
			// turning up is handled below whatever this decided.
			if (audio_device_is_open()) {
				continue;
			}

			pthread_mutex_lock(&storage_lock);
			if (!usb_external_host_reserved() && !dac_owns_usb && !usbaudio_handover_pending()) {
				if (!parked && !power_screen_is_on() && !adb_is_running()) {
					gadget_unbind();
					parked = true;
					fprintf(stderr, "usb: no cable and the screen is dark; gadget unbound\n");
				} else if (parked && power_screen_is_on()) {
					// Unless ADB has been switched on meanwhile: its gadget wins.
					if (!adb_switched_on() && !gadget_rebind()) {
						ensure_storage_gadget();
					}
					parked = false;
					fprintf(stderr, "usb: awake again; gadget rebound\n");
				}
			}
			pthread_mutex_unlock(&storage_lock);
			continue;
		}

		// A cable turned up while the gadget was parked: put it back before
		// anything else, or the host will never see this device.
		if (parked) {
			// Serialize the whole rebind with an explicit external-host trial.
			pthread_mutex_lock(&storage_lock);
			if (!usb_external_host_reserved() && !dac_owns_usb && !usbaudio_handover_pending()) {
				typec_force_sink();
				if (!adb_switched_on() && !gadget_rebind()) {
					ensure_storage_gadget();
				}
				parked = false;
				fprintf(stderr, "usb: cable in; gadget rebound\n");
			}
			pthread_mutex_unlock(&storage_lock);
			continue; // give the controller a poll to enumerate
		}

		if (exported) {
			if (host_ejected()) {
				fprintf(stderr, "usb: host ejected the medium, reclaiming the card\n");
				if (locked_restore()) {
					exported = false;
					await_unplug = true;
				}
			}
			continue;
		}

		if (await_unplug) {
			continue;
		}

		// ADB is on: the cable is here to carry a shell, not to be a card
		// reader. Exporting the card now unmounts it from under that shell --
		// which is why /mnt/sd_0 came back "Read-only file system" over ADB --
		// and the stock firmware does not do it either. Leave the card where
		// it is.
		if (adb_is_running()) {
			if (!said_adb) {
				fprintf(stderr, "usb: ADB is running; the card stays mounted (no mass storage)\n");
				said_adb = true;
			}
			continue;
		}
		said_adb = false;

		// VBUS alone is not a PC -- a wall charger raises it too, and taking
		// the card away (stopping the music with it) for a power brick was
		// exactly the "track would not start while charging" bug. Only a host
		// that has enumerated the gadget counts.
		if (!udc_configured()) {
			continue;
		}

		export_result_t result = locked_export();
		exported = storage_active;
		if (result == EXPORT_FAILED) {
			// Do not repeatedly stop playback and close databases while the
			// same PC remains attached. Replug to try again.
			if (!exported) await_unplug = true;
			gui_post(popup_export_failed_cb, NULL);
		}
	}

	return NULL;
}

void usb_start(const char *device, const char *mount_point) {
	if (!device || !device[0] || !mount_point || !mount_point[0]) {
		fprintf(stderr, "usb: no card device/mount known; USB mode disabled\n");
		return; // host build, or no card: nothing to export
	}

	bool any_vbus = false;
	for (size_t i = 0; i < sizeof(VBUS_CANDIDATES) / sizeof(VBUS_CANDIDATES[0]); i++) {
		bool have = access(VBUS_CANDIDATES[i], R_OK) == 0;
		fprintf(stderr, "usb: vbus node %s: %s\n", VBUS_CANDIDATES[i], have ? "present" : "missing");
		any_vbus |= have;
	}
	if (!any_vbus) {
		fprintf(stderr, "usb: no VBUS node found; USB mode disabled\n");
		return;
	}
	if (access("/sys/kernel/config", F_OK) != 0) {
		fprintf(stderr, "usb: no configfs on this kernel; USB mode disabled\n");
		return;
	}

	// The port is a peripheral from here on, whatever is at the other end of
	// the cable. The stock player re-applies its own saved choice at every boot
	// for the same reason: the role is not remembered across one.
	typec_force_sink();

	snprintf(sd_device, sizeof(sd_device), "%s", device);
	snprintf(sd_mount, sizeof(sd_mount), "%s", mount_point);

	// The empty-medium gadget goes up right away, unless ADB owns the
	// controller -- then its gadget provides the enumeration signal instead.
	// "Owns" includes the moment before adbd has bound it: started just
	// before this, it may not have got that far, and a card reader bound now
	// would keep it off the controller (see adb_keep_controller()).
	pthread_mutex_lock(&storage_lock);
	if (!usb_external_host_reserved() && !adb_gadget_bound() && !(adb_switched_on() && adb_is_running())) {
		ensure_storage_gadget();
	}
	pthread_mutex_unlock(&storage_lock);

	pthread_t thread;
	if (pthread_create(&thread, NULL, usb_thread, NULL) == 0) {
		pthread_detach(thread);
		fprintf(stderr, "usb: watching for a USB host (exporting %s)\n", sd_device);
	}
}
