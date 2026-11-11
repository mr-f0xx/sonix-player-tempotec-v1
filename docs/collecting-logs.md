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

### Bluetooth receiver: stutter, the phone's volume, and a freeze

These are the lines the receiver writes to `sonix_player.log` (the player's log,
not `bluetooth.log`) and what each one says about the three things reported on
firmware 1.6.1. None of them has been seen on a physical V1 yet: what is below
is what to look for, not what was found.

**Stutter.** The count that matters is `audio: external source finished; device
handed back (N underruns)` at the end of every receiver session, and the lines
`audio: the external source ran dry` while it plays. Two more say why:

- `btreceiver: aptX gets deeper buffers than the other codecs: 160 ms in and 360
  ms out` -- the deeper aptX buffers are in force for this session. aptX is the
  one codec that gets them; SBC, AAC, LDAC and aptX HD keep the receiver's
  ordinary 80 ms in and 120 ms out.
- `btreceiver: the capture ring overran (N since it opened); the output was not
  taking frames as fast as they arrived` -- audio was thrown away on the way in,
  which is a stutter that has nothing to do with the output running dry. Said at
  most every two seconds, so the number is the count to read.

The two buffers are settings in the `[bluetooth]` section of
`/usr/data/device_config.ini`, restart the player after changing them:

```
[bluetooth]
receiver_aptx_capture_ms = 160
receiver_aptx_output_ms  = 360
```

`0` turns the extra room off for aptX. Both are a floor: a larger
`receiver_capture_ms` or `receiver_output_ms` still wins. The price is delay --
240 ms more at the output, and up to 80 ms more when the capture side backs up
-- which a film will show as sound arriving late. A log that still shows aptX
underruns with the deeper buffers is worth sending: it means the arrival of the
audio is not the whole story.

**The phone's volume.** Every time a new receiving stream appears, a link or a
codec change that builds it again, the player says whether the phone's volume
will be applied on this side:

- `bluetooth: RX stream on <mac> (new stream): the phone's volume would only
  have muted at zero; it is now applied here, level 111/127 left, 111/127 right`
  -- the stream started in the daemon's native-volume mode, in which the
  player's decoder only silences at exactly zero, and was switched. A log that
  has this line on a connection where the volume felt like an on/off switch is
  the confirmation.
- `... the phone's volume is applied here, level ...` -- it was already.
- `... and turning the software volume on failed` / `its volume settings did not
  answer 5 times; leaving them as bluealsa has them` -- the daemon would not say
  or would not take it; the stream is left as it was.

The word after the address is `new stream` for a stream the phone's own codec
change or a reconnection made, and `after the codec change` for one made by the
codec button on the receiver page.

**A freeze.** The log is written once a second, so the last second before a
hang can be missing; what is there is still the best evidence. For a hang in
receiver mode, send both `sonix_player.log` and `bluetooth.log`, and note
whether the screen was on and what was on it. Two lines bracket the moment a
stream starts and are the last things to look for:

- `btreceiver: first audio N ms after the output opened` -- the stream got as
  far as sound. If it is the last line of a log, the hang was after the sound
  started; if the log ends before it, the hang was while the DAC was being
  opened.
- `btreceiver: <mac> says it is playing and nothing has arrived in 3000 ms; this
  PCM is not the live one` -- the first-frame check handing the DAC back and
  opening it again. It is counted from the moment the phone says it is playing,
  and no longer from the moment the output opened; a log with this line within a
  second of the phone starting to play is the behaviour this build fixed, and
  means the fix is not running.

**The control centre over the receiver.** Three lines trace the receiver being
parked behind the equaliser and brought back:

- `btreceiver: staying on behind a page opened from the control centre`
- `btreceiver: back on the receiver's page`
- `btreceiver: the way back to the receiver's page is gone; ending the receiver`
  -- the user went somewhere (the main menu, another section) the chevron
  cannot lead back from, and the receiver ended there.

A log in which the receiver stops with none of these lines, right after a page
was opened from the control centre, is a bug worth reporting with the page's name.
