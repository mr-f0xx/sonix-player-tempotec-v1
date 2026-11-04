#ifndef USBEXPORT_H
#define USBEXPORT_H

#include <stdbool.h>

// Detach the *known mounted* filesystem with a normal, synchronous umount and
// hold an exclusive block-device claim until the gadget has released its LUN.
// Never uses MNT_FORCE/MNT_DETACH. Returns an fd or -1; *unmounted says whether
// the caller must remount on failure. Fails closed on unknown mount/claim state.
int usbexport_detach_and_claim(const char *device, const char *mountpoint, bool *unmounted);

#endif
