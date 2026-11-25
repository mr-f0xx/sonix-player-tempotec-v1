# Battery profiler

Open **Settings → System → Battery profiler**. While visible, it refreshes every two seconds. It shows the battery level, the AXP2101's battery voltage in mV, and the PMIC's programmed charge-current limit in mA. They are read-only: opening this page does not change any charging, playback or sleep settings.

The programmed limit is **not** current flowing into the cell. The AXP2101 has no public current ADC, and the bundled 1.3 `axp_battery/current_now` returns a fixed 200,000 µA, not a sensor reading. Neither that value nor the driver's fixed “Good” health value is displayed as telemetry. The V1 does not report measured charge current, full-charge capacity or battery wear.

Below the live readings, the profiler shows the last charge date and time, whether the battery reached its charge target or was unplugged, and two durations since then. **On-time** is how long the player has been powered on, including standby and suspend, but not time spent switched off. **Active time** counts only while the screen is on or audio is playing (local, radio, Bluetooth receiver or tape), so a night spent suspended stays in the on-time but never in the active time. When comparing two setups, use the active time: it shows how much was actually used, not how long ago the player was charged.

## Compare two playback setups

1. Unplug USB, even if it is only used for data. Play a local track through 3.5 mm, with low gain, PEQ off and Wi-Fi off. Leave the profiler open until **Discharge trend estimate** appears, then choose **Save baseline**. For a quicker but less reliable comparison, both runs can use **Fuel gauge estimate** instead.
2. Try 4.4 mm balanced, high gain, PEQ and Wi-Fi. Return to the profiler if you left it. On stock firmware the jack switch does not distinguish both wired sockets, so the page may only say **Wired**. After swapping jacks, use **Resample load**. The saved baseline stays in memory until you replace it or reboot.
3. Once the same estimate method is available for the new load, **vs. baseline** shows the difference in equivalent mA and percent. A positive difference means the current setup is estimated to draw more. Keep listening level, tracks, battery level and screen use as similar as possible. This compares the whole player; to isolate one setting, change only that setting.

## Where the estimates come from

**Fuel gauge estimate** is the initial forecast, when the stock CW2015 driver (`cw201x`) provides a valid time-to-empty. That driver reports *minutes* even though the Linux sysfs convention is seconds. The gauge may take time to react to a load change.

**Discharge trend estimate** takes over after two complete 1% drops under the current load. It can take several percent of discharge to show up. Keep playback and settings steady and leave the profiler open. The sampler continues while the player is awake, even with the screen dark, but does not prevent standby. It starts a new window after a suspend gap, cable connection, gauge jump or tracked load change. The page does not compare estimates from different methods.

Both methods project runtime to Sonix's 1% shutdown threshold. The draw in mA assumes TempoTec's rated 1400 mAh cell capacity; it is not measured current or a measurement of how much capacity your cell still has.

The simulator does not provide hardware telemetry. Use a physical V1 to validate readings and a battery-side meter if you need actual current or power measurements.
