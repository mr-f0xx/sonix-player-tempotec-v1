#ifndef AXPCHARGE_H
#define AXPCHARGE_H

#include <stdbool.h>

// Charging on the TempoTec V1, whose AXP2101 PMIC is also its charger and
// which has no MP2731. The PMIC controls are written over I2C; the percentage
// limit is enforced in power.c using the PMIC's charge-enable bit.
//
// The Power page exposes separate voltage/current switches alongside the
// automated percentage limit. PMIC registers are written over I2C (bus 0,
// address 0x34):
//
//   * the voltage ceiling: target voltage 4.20 V instead of the stock 4.40 V
//     (REG64h), a secondary safety cap. The player also enforces the selected
//     percentage limit using the PMIC's charge-enable bit (REG18h[1]); voltage
//     alone can still report 100% on some battery gauges.
//   * the current limit: charge current at most 500 mA (REG62h). Never raises
//     a current that is already lower.
//
// Switching either off writes back what was there before. Both are asserted
// again every few seconds from the power tick.

// True on a player charged by the PMIC alone: by the model table when
// system-info.json names one, otherwise by the MP2731's node being absent.
// Decides which controls the Power page shows.
bool axpcharge_applies(void);

// Applies both switches at once. Does nothing where axpcharge_applies() is
// false.
void axpcharge_set(bool limit_voltage, bool limit_current);

// Enables or disables battery charging without cutting power to the player.
// The desired state is reasserted by axpcharge_tick().
void axpcharge_set_charging_enabled(bool enabled);

// Re-asserts the switches, at most every few seconds. Called from the power
// tick.
void axpcharge_tick(void);

// True while software has disabled charging at the percentage ceiling, or the
// voltage cap is on and the PMIC reports charge done. As of the last update.
bool axpcharge_holding(void);

#endif /* AXPCHARGE_H */
