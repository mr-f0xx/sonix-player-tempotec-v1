# Collecting logs for a bug report

Everything below is about the player as it runs on the device. Paths are the
paths on the device; the card is the microSD, mounted at `/mnt/sd_0`.

## The short version

1. **Settings → System → About**, tap **Build number** five times, until it says
   developer options are on.
2. **Settings → Developer options → Log to microSD** → on.
3. Reproduce the bug.
4. Take **`.local/sonix_player.log`** from the root of the card. `.local` is a
   hidden folder, so allow hidden files in your file manager (or `adb pull
   /mnt/sd_0/.local/sonix_player.log`).

That one file is the answer to "what log do I send" in almost every case. Every
line is stamped with the time to the millisecond, a date line is printed at the
start and whenever the day changes, the kernel's warnings and errors are copied
into it as `kernel:` lines about once a second, and a crash prints its own
report into it.

Five things about it that decide whether it is useful:

- **It only starts when you turn it on.** Lines from before you switched it on
  do not exist. Turn it on first, *then* reproduce.
- **It is never truncated or rotated.** It is opened in append mode and grows
  for as long as the switch is on. Delete it (or rename it) right before
  reproducing, so the file you send is the bug and not last week.
- **It is flushed to the card about once a second.** Wait two or three seconds
  after the bug before pulling the card or switching to USB mass storage — the
  writer thread syncs the file every second, so only the last second is ever at
  risk.
- **A crash or a reboot is reported in the *next* run's log, not the previous
  one.** At every start the player reads what the kernel kept from before —
  out-of-memory kills, oopses, pstore records, `/proc/last_kmsg` — and writes it
  into the top of the new log as `kernel:` lines. If the device restarts, send
  the log as it is *after* it has come back, and say so.
- **While the card is exported over USB the log is held in memory** and appended
  afterwards, so the export window itself is missing from the file. That is
  expected, not a lost log.

Turn the switch off when you are done: it is a constant trickle of writes to
your music card.

## When the log is not enough: the bundle

For anything that is not visible from the player alone — a freeze, a reboot, a
kernel or driver problem, Wi-Fi, Bluetooth, touch, audio — the useful thing is a
bundle: the log plus the state of the device at the moment you took it.

Enable **Developer options → ADB**, connect the player over USB, then from a
computer with `adb`:

```sh
adb push sonix-player/tools/sonix-bugreport.sh /tmp/
adb shell sh /tmp/sonix-bugreport.sh
adb pull /mnt/sd_0/.local/bugreport-*.txt
```

It writes one file beside the player's log (`/mnt/sd_0/.local/bugreport-<date
-time>.txt`, or `/tmp/` when there is no card) and prints the path. It collects:
device and build, kernel log and pstore, memory and processes, mounts and card
state, the player log, the Bluetooth log, the boot trace, the settings, network,
audio, display and input, USB state. Passwords, PSKs, tokens, cookies and logins
are replaced with `<redacted>` on the way out; the file ends with a short form to
fill in (what happened, expected, numbered steps, time, build).

Two knobs, set in the environment:

```sh
adb shell "SONIX_LOG_LINES=20000 sh /tmp/sonix-bugreport.sh"   # longer tails (default 4000)
adb shell "SONIX_FULL_LOG=1     sh /tmp/sonix-bugreport.sh"    # whole logs, not tails
```

## Symptom → what else to send

| Symptom | Add |
|---|---|
| Bluetooth: pairing, connecting, drops, no audio | **Developer options → Bluetooth log** on, reproduce, send `.local/bluetooth.log` (and `.local/bluetooth.old.log` if there was a lot of it — it rotates at 8 MB). It has btmon's HCI traffic, `bluetoothd`/`bluealsa` syslog and the player's own `bluetooth:` lines on one timeline. Also the device you connect *to*: phone or headphones, make and model. |
| Crash during a library scan | **Developer options → Database log** on (under the log switch). The scan then names every file it reads, so the log ends at the file that caused it. Give the file if you can. |
| Player restarts, device reboots, hard freeze | The log *after* the restart (see above), plus a bundle for `dmesg` and pstore. A SIGKILL from the out-of-memory killer leaves nothing behind in the player's own output — only the kernel's buffer has it. |
| Wrong or missing boot screen | **Developer options** shows the boot trace in its own row; the bundle includes `/tmp/.bootlogo_trace`. Say which boot screen is selected and what you got instead. |
| Layout, fonts, missing glyphs, colors | Screenshots: **volume up + power** saves to `Screenshots/` on the card. The bundle's display and input sections cover the panel size and rotation. |
| Touch: dead zones, ghost taps, scrolling | The bundle's input section (`/proc/bus/input/devices`, `/dev/input`). For a stock-vs-patched driver question, see [PATCHES.md](../PATCHES.md). |
| Wi-Fi, DLNA, AirPlay, Wi-Fi file transfer | The bundle's network section. It prints `wpa_cli status` and the network list — no PSKs. Say which access point band and what the other end is. |
| Audio glitches, wrong output, DAC | The bundle's audio section (`/proc/asound`, mixer). Add the Bluetooth log if it happens over Bluetooth. |
| A USB-C DAC, dongle or headphones the player does not see | Whether **Developer options → External USB DAC** was on, the maker and model of the dongle, and the start of the player log, which prints the setting's state as `usbhost: startup: ...`. The log also carries `usbaudio:` port lines and, for a DAC that does enumerate, the card it became. |
| A faint hiss under the music on a USB-C DAC, louder on one dongle than another, absent on a phone or on the stock firmware | Nothing to collect -- it is a setting, not a fault. The player used to hold the dongle's own volume control at full gain and turn the samples down instead, so the music sat close to the amplifier's noise floor. `[usb] hardware_volume` now defaults to 1, which gives the control the work; set it to 0 only for a device whose own control is worse than its noise floor. **The `usbaudio: volume goes to '…'` line** names the control it found, and a dongle with none at all will always do it on the samples. |
| A USB-C DAC that stops when the screen has been dark for a while, usually around ninety seconds, with the player still playing | The whole log, and the last few lines before the sound stops. Look for `power: parking Bluetooth after …` immediately followed by `audio[…]: the output moves from 'plughw:N,0' to 'default'`: that pairing is the idle park taking a route that belongs to the dongle. Fixed -- on a current build it should not appear at all. |
| A USB-C DAC that is silent below some volume level and then comes to life | The `usbaudio:` lines from when the dongle was plugged in. **`stops following below -X dB; past that the samples do the work`** is the measurement of where its own volume control gives up, and the smoother of the two halves of the knob is below that; `still follows its whole range` means it was never a problem for this device. If a dongle goes quiet *above* the measured point, that is worth reporting with the log -- it means the measurement and the device disagree. |
| A USB-C DAC that plays and then goes silent with the player still playing | The log across the whole stretch, from the moment it was plugged in to a few seconds after the sound stops. The line to look at is the 10-second `audio[…]: … s at …  Hz … stream …` one: it names the stream's state and how much is still queued, which is what separates the three causes — a dongle that stopped consuming, a stream that never started, and a track that stopped being decoded. `usbhost: … written again` means the port had gone back to its own default and was taken back; `usbaudio: … falling back` must not appear for a dongle at all. A `replug` that fixes it for the same time again points at the device, not at the player. |
| Battery, charging, standby, power-off | The player log around the event, with the time you plugged or unplugged it. |

## Always write down, next to the log

- Numbered steps that reproduce it, starting from a cold boot if that matters.
- How often: every time, one in five, once.
- **When** it happened, as the device's own clock had it. The log is stamped
  with local time, and the device's clock is not your phone's clock; a time lets
  the log be read even when it is days long.
- The **firmware build** (Settings → System → About, or
  `/usr/resource/sonix/components/system-info.json`), and the exact model.
- What is on the card: filesystem (exFAT/FAT32), make, and roughly how many
  tracks. Whether it still happens with no card inserted, if you can try.

## Before you attach anything

- Never attach `streaming-keys.ini`, or a build made with your own credentials.
- The bundle redacts, but read it once: a file name, an SSID or a playlist title
  can still be personal. Rename or cut whatever you would not post.
- Library logs name the files on your card.
- If the file is big, gzip it. Bigger is better than truncated — a tail cut in
  the wrong place removes the lines that matter.

### Bluetooth receiver drop-outs with Wi-Fi associated

On the V1, Bluetooth and 2.4 GHz Wi-Fi share the radio. A successful AFH
channel-classification command does not guarantee that incoming A2DP packets
will arrive in time. If the Bluetooth receiver is open and Wi-Fi is only
idly associated, Sonix temporarily **disconnects the Wi-Fi association**
(`wpa_cli disconnect`) and reconnects it when receiver mode closes. It does
not turn off the shared chip, change the saved network or change the Wi-Fi
switch. A network service the player tracks (streaming, transfers, servers, downloads)
keeps Wi-Fi connected instead; deliberately using the Wi-Fi controls during
receiver mode also overrides this protection until the mode closes. Other
network clients, such as an SSH connection, are not tracked and will be
interrupted while the association is suspended.

To keep Wi-Fi associated even in Bluetooth receiver mode, put
`receiver_wifi_priority = 0` in the `[bluetooth]` section of
`/usr/data/device_config.ini` and restart the player. If audio still drops out with
this protection, compare the `bluealsa: Missing RTP packets` and
`audio: the external source ran dry` lines in a Bluetooth/player log from
before and after, with the same source and placement. The policy is not a
radio-firmware repair and has not been validated on a physical V1 here.

### Bluetooth receiver: stuttering, or a codec that sounds worse than it should

The audio arriving over A2DP is decoded by bluealsa, read by the player, and
written to the DAC from a buffer of its own (`receiver_jitter_ms`, 150 ms by
default; see *Bluetooth receiver buffers* below). Two different faults sound
similar and the log separates them:

- `btreceiver: session over: N frames played, S starved reads, D frames
  dropped, peak buffer P ms` — printed once when a session ends, and only when
  something went wrong. **Starved reads** mean the DAC outran the incoming
  audio: the link, or the player's own scheduling, was late. **Dropped frames**
  mean the opposite, the phone's clock ahead of the DAC's, which over a 150 ms
  buffer and a phone at the same nominal rate means a broken rate negotiation
  rather than jitter.
- `btreceiver: buffer P ms of C (peak M), N frames in, D dropped` — every ten
  seconds while receiving. A buffer that sits near its ceiling for a whole
  session with `D` climbing is the clock case above; one that sits near zero
  is a link that is not delivering.

For **LDAC quality that rises and falls** rather than breaking up, the phone is
the one deciding the bit rate, using the only signal it has: whether its
packets are being taken. The player-side counters above are the same signal
seen from this end. `--ldac-abr` (the default) is the adaptive mode; to pin the
rate and take jitter out of the comparison, set **Settings → Wireless → LDAC
quality** to *High*, *Standard* or *Mobile* and listen again — a fixed rate that
still fluctuates is a decode or scheduling problem here, and a fixed rate that
is clean is the adaptive controller doing what the link told it to.

A link that connects and stays **silent** is a different line again:
`btreceiver: N frames have arrived from <phone> and none of <codec> has reached
the DAC; moving the link off it`. The receiver waits six seconds for that
before acting, because a slow start and a decoder that will not run look the
same that long, and then renegotiates onto another codec the link offers. That
is the aptX case on some senders (bluez-alsa's aptX decoder spins on a packet it
cannot decode instead of failing); if it is logged and the sound then comes back
on a different codec, the codec is the fault, not the radio. If the line is
followed by `no other codec to move to`, the link offers only the broken one and
nothing more can be done from this side.

`bluealsa: Dropping PCM frames: PCM overrun` in the Bluetooth log (Settings →
Developer options → Bluetooth log) is the daemon's side of the same question:
it means the player did not drain bluealsa's own FIFO in time, which points at
the player rather than at the link.

### Bluetooth receiver buffers

Everything between the phone and the ear is a config key, and the defaults are
deliberate rather than wide:

| key | default | what it is |
|---|---|---|
| `receiver_capture_ms` | 80 | bluealsa's side: the capture buffer the plugin fills |
| `receiver_output_ms` | 120 | the DAC's side: the ALSA buffer the player writes into |
| `receiver_jitter_ms` | 150 | the ring between the phone's clock and the DAC's |
| `receiver_ring_ms` | 400 | the ring's hard ceiling |
| `receiver_rt_priority` | 10 | `SCHED_RR` priority of both receiver threads; 0 = none |
| `io_rt_priority` | 12 | `SCHED_FIFO` priority of bluealsa's decode thread; 0 = none |

All six go in the `[bluetooth]` section of `/usr/data/device_config.ini`. The
first four are latency: raising them trades a delay in the ear for tolerance of
a bad link, and for anything with a picture on the phone that trade is a bad
one past a couple of hundred milliseconds. The last two are a single core's
scheduling order; the receiver's own thread and bluealsa's decoder should not
both be able to be behind the interface at the same time.
