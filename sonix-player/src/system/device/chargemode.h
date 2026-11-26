#ifndef CHARGEMODE_H
#define CHARGEMODE_H

#include <stdbool.h>

// The boot that happens because a charger was plugged into a player that was
// switched off.
//
// The stock firmware answers that boot with a battery screen rather than with
// the library: the cable powers the board, and what the user wanted to know was
// how much charge is going in. This player replaces the stock one from its
// first instruction, so the same boot has to be recognised here, and answered
// with the charging screen (see chargescreen.h) -- press power to look at it
// again after it has gone dark, hold power to start the player after all.
//
// Telling that boot apart from a player boot is the whole question, and the
// hardware does not answer it: nothing in the kernel says whether the power
// came from the key or from VBUS. What can be known is what the device was
// doing before it started, and that is written down on the one partition that
// survives the power going away:
//
//   * the player writes a marker beside the settings as its own startup
//     continues -- "this device was running the player";
//   * power_shutdown() -- the menu's Power off, the automatic shutdown, the
//     battery guard at one percent -- takes it off again.
//
// A boot with a cable in and no marker is therefore a player that was switched
// off and then put on a charger. Everything else keeps the marker and comes up
// as it always did: a reboot from the menu, a crash and a firmware update all
// leave the device running the player, where the user left it. A board whose
// power was cut -- the cell pulled -- keeps the marker too and boots the player;
// that is the price of not putting a battery screen over every reboot. The one
// boot that loses the marker without a clean shutdown is the first one after a
// factory reset, whose recovery script wipes /usr/data; it shows the battery
// screen if a cable is in, and a hold on the power key is the way past it.
//
// [power] charge_screen = 0 in device_config.ini turns the whole thing off.
//
// What no software can add is the boot itself: a board whose PMIC does not
// power it up when VBUS arrives stays off, and nothing here is ever run.

// Whether this boot is a charger insertion, and the charging screen should run
// instead of the player's own startup.
//
// Cheap and side-effect free apart from a stat() and the VBUS reads; called
// once, before the services start.
bool chargemode_wanted(void);

// The player is what this device is running: the marker goes down. Called once
// on every path into the ordinary startup -- after the charging screen has
// handed over, and on a boot that was not a charger insertion at all.
void chargemode_player_started(void);

// The player is going down for real. Called at the top of power_shutdown(),
// while the settings partition is still writable and mounted: with the marker
// gone, the next cable-in is a charger boot.
void chargemode_player_stopping(void);

#endif /* CHARGEMODE_H */
