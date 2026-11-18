#ifndef CHARGESCREEN_H
#define CHARGESCREEN_H

#include "lvgl/lvgl.h"

#include "src/system/device/system.h"

// The battery screen a charger boot opens on.
//
// Run instead of the player's own startup when the device was switched off and
// the cable went in (chargemode.h decides that, and explains why it has to be
// decided from what the last shutdown left behind). It takes the display over
// with one dark screen -- a battery filled to the current level, the percentage
// and what the charger is doing -- and runs its own small loop until the user
// holds the power key.
//
// What the key does, and the whole of the interaction:
//
//   press   brings the screen back after it has gone dark on its own, and
//           resets its timeout while it is up;
//   hold    starts the player: this function returns, and main.c carries on
//           into its ordinary startup with the screen taken down again.
//
// The screen darkens itself after [power] charge_screen_seconds (ten by
// default). With the cable taken out while it is dark, the player switches off
// instead: a device that was off, was woken by a cable and no longer has one
// has nowhere to be. With the screen dark it keeps polling the gauge, slowly --
// a charge is a slow thing, and the two sysfs reads it costs every couple of
// seconds are nothing beside a panel that is not lit.
//
// Nothing here needs the services: the gauge is read directly through
// battery_read_now(), the cable through charger_attached(), and the key through
// the evdev node the panel's own power button lives on. That is deliberate --
// it runs before the input threads exist, so that no other part of the player
// answers a key the charging screen is using.
//
// `battery_cfg` is what system_start_services() would be given, so the
// simulator's two override files work here too. On the way out the last
// charge frame stays on the glass (the interface is drawn onto the same fb
// pages), so the panel is never left on a reset-white screen waiting for the
// main menu.
void chargescreen_run(lv_display_t *disp, const battery_config_t *battery_cfg);

// Whether this screen left the panel up with a drawn frame on it (see
// chargescreen_run()). main.c passes the answer to power_init() as
// power_config_t.panel_up, so the panel is not unblanked a second time on the
// way into the player -- that write resets the glass to white behind a lit
// backlight. False for an ordinary boot.
bool chargescreen_panel_lit(void);

// The drawing half, without the loop and without the panel: the screen's
// objects built on the active screen, and one reading painted into them. In
// here because the loop's own body is the part that needs a power key, and the
// layout is the part worth checking on a machine that has none -- see
// tests/ui/test_chargescreen.c. `chargescreen_build` uses `disp` for the panel
// size and leaves the objects up; `chargescreen_show` may be called with any
// reading, including `have` false for a gauge that said nothing.
void chargescreen_build(lv_display_t *disp);
void chargescreen_show(bool have, int percent, bool charging, bool cable);

#endif /* CHARGESCREEN_H */
