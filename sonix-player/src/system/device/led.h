#ifndef LED_H
#define LED_H

#include <stdbool.h>

// The RGB status LED.
//
// On the TempoTec V1 and the R3 Pro II the light is an SGM31324 RGB controller
// on I2C bus 0. The firmware's leds_sgm31324_add.ko owns it and registers a
// table of preset colour patterns, exposed as small integers on
// /sys/class/leds/sgm31324-leds/led_pattern; the stock binary just writes the
// pattern number for the state it is in. The numbers below come out of its
// logical->pattern table (FUN_0047ce00) and its callers:
//
//   1   aqua           device on, nothing playing (written at stock startup)
//   2   yellow         PCM at or below 48 kHz
//   3   aqua           PCM above 48 up to 192 kHz
//   4   orange         PCM above 192 kHz
//   5   off            the logical-0 state (all-zero registers)
//   6   red breathing  charging: the stock LED state machine forces this,
//                      last, whenever its charger flag is set; the register
//                      block (blink period, ~50% duty, ramp, current on the
//                      red channel only) is the charging pulse
//   11  white          DSD (the white LED on GPIO PF11, wled_pattern_id=11)
//   8   green          MQA
//   9   blue           MQA Studio
//   10  magenta        MQA rendering
//
// This player does better than writing a pattern number: led_init() drives the
// controller directly over I2C (led_i2c.h), which is what lets it fade in on
// power-on, cross-fade between colours, breathe the charging red in hardware
// and fade out on the way down. The firmware driver is disabled by the packer
// (sonix_firmware_packer.sh step 6b) so the controller is free from boot; where
// it is still loaded, led_i2c_init() unloads it. The pattern numbers above
// remain the fallback, used only when the controller cannot be taken over or on
// the R1, which has no SGM31324 at all.
//
// Stealth and suspend take priority over everything; otherwise charging and
// fully charged keep the stock charger priority. In Smart LED mode a brief
// low-battery or upload notice can interrupt the normal playback/idle colour.
// Smart LED playback effects cap at the brightness setting. Only the optional
// Rainbow Breath style replaces the sample-rate colour during PCM playback.
//
// The playback colour follows the stock sample-rate bands, regardless of the
// codec or bit depth:
//
//   PCM at or below 48 kHz       yellow
//   PCM above 48 up to 192 kHz   aqua
//   PCM above 192 kHz            orange
//   DSD                          white
//
// The R1 has no RGB controller: leds_pwm_add.ko registers two plain LEDs,
// /sys/class/leds/red and /sys/class/leds/blue. There every lit state becomes
// blue at brightness 50 and off stays off, as its stock player does, and the
// charging red is the red LED with its "breathing" trigger. The same states,
// priorities and switches apply; only the output differs. Which player it is
// comes from the nodes present, the pattern node winning.
//
// Everything degrades to a no-op when the sysfs nodes are missing (the host
// build, or firmware without the module loaded).

// Probes the LED and reads the saved settings before its first render. Call
// on the UI thread before creating the Power page. State updates and notices
// also belong on that thread; the worker handles animation and I2C writes.
void led_init(void);

// Red breathing while charging, green when full or held at the charge limit.
// The caller supplies a single settled reading: rewriting breathing restarts
// the controller's ramp, and transient readings can make the red stutter.
void led_set_charge_state(bool present, bool charging);

// Picks the state for what is playing (shown when not charging). `playing`
// false or rate 0 falls back to the idle aqua. PCM colour follows the stock
// rate bands: <=48 kHz yellow, >48 through 192 kHz aqua, >192 kHz orange.
// `dsd` true paints a DSD source white, regardless of its DoP carrier rate;
// `podcast` true paints purple instead. Cheap to call repeatedly: only writes
// on change.
void led_update_playback(bool playing, int sample_rate, bool podcast, bool dsd);

// Wi-Fi transfer active: a blinking blue. Stays lit with the screen off, since
// it indicates the server is running, and overrides the darken-in-standby
// option.
void led_set_wifi_transfer(bool active);

// Bluetooth receiver mode: blue at full brightness. Same argument as the
// transfer above, and the same exemption from the standby option -- nothing
// plays through audio.c in that mode, so led_update_playback() reads a stopped
// player, and the LED went out twenty seconds after the screen did with a
// computer still sending music into the jack.
void led_set_bt_receiver(bool active);

// DAC mode. Nothing plays through audio.c there, so led_update_playback() sees
// a stopped player and the LED went out twenty seconds after the screen did,
// with the computer still sending music. While the host streams, the rate gets
// the colour a local track of that rate would; while the mode is merely up, the
// LED stays on the idle colour rather than being darkened by the standby
// option -- the same reasoning as the transfer above.
void led_set_dac(bool active, bool streaming, int sample_rate);

// The LED master switch. Off keeps the LED dark whatever else is
// going on -- with one exception, the same one the stock player makes: a
// charge in progress still shows its red, because a device that looks dead
// on the charger is worse than a lit LED. On (the default) restores the
// normal behaviour, including the standby option below.
void led_set_enabled(bool enabled);

// A separate blackout override, including charge indications. Does not change
// the master switch, standby option or brightness preferences. Saved by caller.
void led_set_stealth(bool enabled);

// Smart LED mode: PCM playback effects and short, distinct event notices.
// Requires the direct RGB controller. Saved by caller.
void led_set_visualizer(bool enabled);

typedef enum {
	LED_VISUALIZER_NONE = 0, // no active PCM effect, not a saved choice
	LED_VISUALIZER_BASS_PULSE = 1,
	LED_VISUALIZER_ENERGY_GLOW = 2,
	LED_VISUALIZER_RAINBOW_BREATH = 3,
} led_visualizer_style_t;

// Selected PCM style, independent of the Smart LED switch and event notices.
// Bass Pulse is the default. The caller saves it as power.led_visualizer_style.
void led_set_visualizer_style(led_visualizer_style_t style);
led_visualizer_style_t led_visualizer_style(void);

// Call on the existing low-battery warning or a completed Wi-Fi upload. If
// hidden by Stealth, charging or a higher priority notice, do not queue it.
void led_notify_low_battery(void);
void led_notify_upload_complete(void);

// The darken-in-standby option: with it on, the LED goes dark after
// twenty seconds in standby (screen off) with nothing playing. A charge in
// progress is never darkened by it -- the red keeps breathing -- and any
// playback or wake lights the LED right back up. Re-evaluated on the same
// polls as the rest, so the switch-off lands within a few seconds.
void led_set_idle_off(bool enabled);

// Screen state, fed by power.c: the standby option only ever darkens the LED
// while the screen is off. Waking restarts the idle window.
void led_set_standby(bool standby);

// The brightness slider, 1-5, used as the level index itself -- the doc's
// `led LEVEL ...`: brightness 5 is `led 5`. Only the direct path uses it; the
// R1's plain LEDs have no such control. Saved by the caller.
void led_set_brightness(int brightness);

// The transition speed, 20-2000 ms in 10 ms steps: how long a fade-in,
// fade-out or cross-fade takes. Only the direct path uses it.
void led_set_transition_ms(int ms);

// Whether the direct RGB effects are available after led_init(). On the
// pattern/PWM fallback and plain host builds only the static controls apply.
bool led_effects_available(void);

// Fades the LED out before power-off or suspend, canceling any active notice.
// Resume renders the current state again, including the saved blackout.
void led_shutdown(void);
void led_suspend(void);
void led_resume(void);

#endif /* LED_H */
