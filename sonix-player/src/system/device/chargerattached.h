#ifndef CHARGERATTACHED_H
#define CHARGERATTACHED_H

#include <stdbool.h>

// True while a charger is attached.
//
// Not the same question as "is a charge going in", which is the trap this
// exists for: a cell held at its configured limit, or simply full, takes
// nothing while the cable is still in, and the two VBUS nodes (usb,
// mp2731-charger) are the same PMIC the panel's rails go through, so their
// reading wobbles around a panel event -- a read taken in that window can say
// "no cable" with one plugged in. So all of it is consulted:
//
//   * any non-battery supply reporting itself online;
//   * any non-battery supply holding a USB-sized voltage (real VBUS is about
//     five volts; a battery is 4.35 V at best and never reaches it);
//   * the battery itself in any state other than discharging or unknown --
//     "Full" or "Not charging" with a cable in is the charge limit doing its
//     job, not an empty socket.
//
// Used for the boot that decides between the charging screen and the player
// (chargemode.c) and for the cable the charging screen watches for an unplug
// (chargescreen.c). It never says "yes" without one of the three, and it is
// cheap enough to poll on the screen's own cadence.
bool charger_attached(void);

// The same decision taken against `dir` instead of /sys/class/power_supply --
// the form the test drives, against a temporary directory of fake supplies.
bool charger_attached_at(const char *dir);

#endif /* CHARGERATTACHED_H */
