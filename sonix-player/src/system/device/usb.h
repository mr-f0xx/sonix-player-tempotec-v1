#ifndef USB_H
#define USB_H

#include <stdbool.h>

// USB mass storage, the way the stock firmware does it.
//
// When a PC is attached, the stock player unmounts the card (through
// sys_server, "MOUNT:UMOUNT:..."), builds a configfs gadget and hands the raw
// block device to the host with the firmware's own script,
// /usr/bin/usb_dev_mass_storage.sh. On unplug it tears the gadget down and
// remounts. This does exactly that -- the scripts and the daemon are already
// on the device; the player's job is only to notice the cable and call them
// in the right order.
//
// Detection polls /sys/class/power_supply/usb/online from a small background
// thread. A wall charger raises the same signal; exporting the gadget then is
// harmless (nothing enumerates it) but unmounting the card for a power brick
// would not be, so the thread waits for a USB *host* -- the UDC state leaves
// "not attached" only when something on the other end actually talks.

// Starts the watcher thread. Pass the SD block device (e.g. /dev/mmcblk0p1)
// and the mount point it lives on; either NULL disables the whole feature
// (host build).
void usb_start(const char *sd_device, const char *sd_mount_point);

// True while the gadget is exported to a host (the card is unmounted then).
bool usb_storage_active(void);

// Hands the USB controller to DAC mode, and takes it back.
//
// While it is handed over, everything in usb.c stands down: no gadget is
// rebuilt, the card is not exported, and the watcher thread does not touch the
// controller at all. Without this the watcher sees a cable, rebuilds the
// mass-storage gadget over the audio one, and the computer both loses the
// audio stream and gains a microSD it should not have.
// Returns false if an external-host override or exported card could not be
// safely returned to the player.
bool usb_set_dac_mode(bool on);
bool usb_dac_mode(void);

// Internal coordination for the explicit external USB-audio host trial.
// Reserve only when no gadget data session, storage, DAC mode or ADB is in
// use. A recovery reservation blocks gadgets until the old role is restored.
bool usb_external_host_reserve(void);
void usb_external_host_release(void);
void usb_external_host_block_recovery(void);
bool usb_external_host_reserved(void);
// A restore must not overwrite an unknown/connected gadget or active ADB.
bool usb_external_host_restore_safe(void);

// Coordination with ADB, whose init script needs the one USB controller for
// its own gadget: yield unbinds the storage gadget before adbd starts, reclaim
// rebuilds and rebinds it after adbd stops (its stop script tears the whole
// configfs down). Both take a card handed to the host back first. Called by
// adb.c around the init scripts.
bool usb_gadget_yield_to_adb(void);
void usb_gadget_reclaim_from_adb(void);

// True while a cable is supplying VBUS (a PC or a charger). Read straight
// from the power-supply nodes, so it is current rather than cached.
bool usb_vbus_present(void);

// Around a system suspend. The storage gadget stays bound to the USB
// controller for as long as the player runs, and a bound gadget is something
// this SoC will not suspend past -- the board reset instead of resuming.
// prepare() hands the controller back before the SoC goes down; restore()
// rebuilds and rebinds the gadget afterwards.
void usb_suspend_prepare(void);
void usb_resume_restore(void);

// The product string: the model's name without the maker's
const char *usb_product(void);

#endif /* USB_H */
