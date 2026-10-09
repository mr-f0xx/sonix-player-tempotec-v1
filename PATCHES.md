# Firmware patches

| patch | script | what it fixes |
|---|---|---|
| Touchscreen multitouch | `tools/gt9xx_multitouch_patch.py` | the panel reports five fingers, the driver lets one out |
| Touchscreen multitouch, R1 | `tools/cst8xx_multitouch_patch.py` | the panel reports two fingers, the driver lets one out |


## TempoTec V1 240×320 interface profile

The target build of this fork defaults to the TempoTec V1 profile. At runtime,
`BOARD=tempotec_v1` (also `tempotec-v1` or `tv1`) selects it explicitly, while
`BOARD=hiby_r1` or `BOARD=hiby_r3proii` forces the regular layout. For the host
simulator, this is enough to exercise the V1 path:

```bash
SONIX_PANEL=240x320 ./sonix_player_host
```

The profile is more than a font substitution. It makes `gui_config_t` use the
real 240×320 panel, a 24 px status bar and 6 px page padding; remaps the named UI
font faces to compact raster sizes; and gives shared headers, settings rows,
grids and status icons compact geometry.

The face those sizes are drawn from is not part of the profile any more. The V1
had Neon 80s as its primary face; it is now one of the choices in
Settings → Appearance → Font, beside MiSans (the default, and the face the
interface was designed with) and the other families the image carries, with the
card's own `Fonts` folder listed after them. Each family keeps MiSans at the end
of its fallback chain for the scripts it has no letters for, and the whole face
is rebuilt and the pages relaid out when the choice changes — see the comment at
the top of `src/gui/fonts/fonts.c`. Header actions use 36 px slots. The
Music landing page keeps Settings plus one overflow menu in the title row;
Playlists/Browse, Favourites and Search remain available inside that menu
without allowing four actions to reduce the heading to `Musi…`.

Now Playing on the V1 shows the blurred artwork behind the **whole screen**, as
Studio does on every player, with a centred square sleeve (about 154×154 px,
soft-clipped corners) floating on it; the source badges and alternative-layout
pills travel on the sleeve itself. The cover panel and the control deck are
both transparent on this board, so there is no horizontal edge under the
sleeve where the panel colour used to meet the deck's own blurred block. The
deck is not a fixed height: `compact_deck_height()` adds up the four rows --
names, bar, clocks, transport -- from the line heights of the fonts in force
(and the shorter 44 px waveform of the alternative layout), so the transport is
never lifted into the queue-position row above it. The full panel stays the
swipe and lyrics-gesture hit target.

Two board matters that are not layout live beside the profile. The V1 kernel
has one switch node, `/sys/class/switch/headset`, and no `balance` node; with
nothing in either socket the headset node reads `1`, which the HiBy logic took
for a plug. `sysinfo_model_t` therefore carries `headset_switch_idle_one`, set
for the V1, and `headphone_jack_state_raw()` on such a model treats `1` as no
plug and any other value as a 3.5 mm headset (never balanced: that route has
not been exercised on this board), logging the raw value to stderr whenever it
changes so the 4.4 mm socket's encoding can be read off a real plug later.
### The white flash on wake, and what it actually was

The panel is an SLCD: its glass shows the contents of its own memory, and a
reset leaves that memory white. Writing `0` to `/sys/class/graphics/fb0/blank`
resets and re-initialises the panel and re-arms the controller's scan-out, and
the write returns before the panel's own init sequence has finished -- the
sleep-out delays alone are up to 120 ms. A frame pushed before the panel is
listening is lost without a word.

Which is why the first three attempts at this did not fix it. They were all
about the backlight: hold it at 0 (PR #12), hold it at `BRIGHTNESS_MIN`, wait
for `FBIO_WAITFORVSYNC` to report two scan-outs, raise the fallback to 300 ms
and distrust a vsync answer that comes back too fast (PR #14, #15). Every one
of them painted the interface into the framebuffer *before* the unblank. That
leaves the controller nothing to carry to the glass after it, so the panel sat
on its reset white until something unrelated -- the status-bar clock, a button
-- next happened to paint, and the backlight faded up over *that*. Holding the
backlight down cannot cover a glass that is still white when it is released,
however long the hold is.

`power_screen_on()` now does three things, in this order:

1. **The backlight goes to a true zero, not to the floor, before the unblank
   and stays there** until the picture is on the glass. `BRIGHTNESS_MIN` is a
   level the panel is *looked at through*; a white reset frame at one percent
   is still a white frame. Zero is safe here as well as darker: the blank the
   driver already performs puts the PWM at zero with the panel unpowered
   behind it, and the screen stays dark for as long as the screen is off. The
   same zero is left behind at the screen-off blank, so the level the driver
   itself restores at an unblank is already a dark one. Overridable as
   `[screen] wake_hold_level` in `device_config.ini` for a board whose driver
   needs the floor back (set it to 1).
2. **The panel's own init is waited out, still dark** (`WAKE_PANEL_SETTLE_MS`,
   120 ms), before a frame is pushed at it.
3. **A frame is pushed after the unblank, twice** (`wake_repaint()`: the kick
   from `main.c` that re-arms the video mode and the scan-out, then a full
   repaint that pans to the frame), `WAKE_PUSH_SETTLE_MS` apart, because a push
   that arrives before the panel is listening is lost in silence and there is
   no way to ask. Only then does `backlight_hold_until_panel_shows()` end the
   hold -- on two `FBIO_WAITFORVSYNC` scan-outs, believed only when they cost
   what two scan-outs cost, or on a fallback now that stands in for those
   frames rather than for the whole wake. A credible two-frame answer now has
   a 50 ms minimum total hold instead of 90 ms; the fallback remains longer.
   The configured level then fades up over the finished picture.

With a working vsync path the wake takes about a quarter of a second of dark
screen from the unblank to the fade; the conservative fallback takes a little
over a third. The backlight stays at zero until the confirmed frame is ready,
so the wait stays invisible; what was visible was the white.

Two-column, three-row pages with five entries let the fifth tile span the last
row. Compact tile captions are one fixed line with an ellipsis and no extra
line spacing, so long translations such as “Artistes d’album” cannot draw
through the card edge. Popovers are width-bounded and scroll when necessary,
and the on-screen keyboard uses a 144 px compact tray with narrower action
keys. Quick Settings retains eight controls in a 2×4 layout; its buttons,
artwork and two card heights are reduced to fit the 320 px sheet.

The **Scan** page sizes its music note to what the three lines under it leave.
The source art is 128 px, most of the gap between the page title and the
Cancel/OK button on a 320 px panel, and a centred flex column overflows at both
ends at once: the note ran up under the title while the name of the file being
read was clipped at the bottom — the column does not scroll, so nothing was
pushed off the page, it was cut. The note is now measured against the count, the
status line and that file name, and both its layout box and its draw scale are
set from the space left over: on the V1 about a quarter of the panel width, the
proportion the art has on the 480 px players, and on those the 128 px it was
always drawn at. The status line wraps inside the panel instead of running past
both edges, and the file name is one line ending in an ellipsis — which
`LV_LABEL_LONG_DOT` only draws when the label has a pinned height, so a long
name used to take a second line that was the one being cut. The fit runs again
whenever the interface font or text size changes, since that is where the line
heights come from.

The **A–Z index strip** draws as many letters as the strip is tall enough to
hold rather than all twenty-eight. On the 240×320 panel about fourteen fit; the
rest of the alphabet stays reachable because a collapsed strip maps a press by
position across the letters' span instead of by which label it landed on. Laid
out in full, the column overflowed the bar — LVGL's flex layout has no negative
gaps — so the lower letters were drawn outside it and the bottom of the strip
jumped to the wrong rows. The strip is 20 px wide on the V1 and the big letter
under the finger is an 88×84 card.

Settings pages whose layout is written in pixels rather than built from
`settingsrow` rows now carry compact counterparts for every one of those
numbers: Date and time (five rollers, the zone list, the pills and Confirm),
the first-boot language panel, the equaliser band card (which scrolls sideways
on the V1 instead of squeezing ten columns into 216 px), MSEB, the parametric
graph, brightness and keyboard type, System's microSD card, Processes, Artist
exceptions, the firmware-update card, the screensaver strip and the two
drag-to-reorder pages (Control centre, Keyboard layout). **Remap buttons** has
no V1 photo and no room for a 480-pixel one, so on that board it is the same
page as a plain list: one row per button, its action on the right, the same
chooser on a tap.

### Touch input is intentionally not guessed

This profile does **not** hard-code a `/dev/input/eventN` node or a speculative
axis transform. Event numbering and the controller's reported ABS/MT ranges
must first be captured from the actual V1 firmware. Once that report is known,
the device-specific runtime branch belongs in the launcher/input path rather
than in the screen-layout profile.


## Touchscreen multitouch

### What the hardware actually does

The R3 Pro II's panel is a **Goodix GT967**, and it reports up to five
contacts. That is not inferred from a datasheet: with the driver module
unloaded, reading register `0x814E` over I2C returns `0x81` with one finger
down and counts up to `0x85` with five.

So the contacts reach the SoC. They are lost in the driver.

### Cause 1: the frame is thrown away

`gt9xx_touch.sh` loads the module with `gtp_max_touch_number=1`, and in
`goodix_ts_work_func` that number is not a cap that clamps - it is a test that
discards:

```
lw    a0, gtp_max_touch_number
andi  s1, s1, 0xf          # contacts the controller reports in this frame
sltu  v0, a0, s1           # more than the limit?
bnez  v0, <exit>           # then drop the whole frame
```

With the limit at 1, the instant a second finger lands the driver stops
reporting **anything at all** - not "the first finger only", the entire frame
is dropped. The same number also sizes the I2C read the driver performs per
interrupt (`max_touch * 8` bytes), so even without the test it would never ask
the controller for a second contact.

### Cause 2: the rate limit inside the loop

Raising the number is still not enough, and this is the part that is a genuine
bug rather than a configuration choice.

`goodix_ts_work_func` walks the contacts of a frame in a loop, and inside that
loop there is a check against `jiffies`:

```
lw    s8, jiffies
bnez  v0, +8
sw    s8, last_jiffies     # first time only
lw    v0, last_jiffies
addiu v0, v0, 2
subu  v0, v0, s8           # last_jiffies + 2 - jiffies
bgez  v0, <next contact>   # >= 0 -> skip the report
...
sw    s8, last_jiffies     # reached only when a contact was reported
```

Read it as: report a contact only if at least three jiffies have passed since
the last contact was reported. At `HZ=100` that is 30 ms.

The loop over a frame's contacts takes microseconds. `jiffies` does not move in
that time. So the first contact of a frame is reported, `last_jiffies` is set
to the current tick, and every remaining contact of the *same frame* fails the
test and is skipped.

The check reads like a debounce that was meant to sit around the whole handler
and ended up inside the per-contact loop instead. Wherever it came from, in
this position it can only ever mean "one contact per frame".

## The fix

Copy the two files `gt9xx_touch.ko` and `gt9xx_touch.sh` out of the firmware, put them
beside the script, and run the python script:

```bash
python3 tools/gt9xx_multitouch_patch.py
```

**`gt9xx_touch.sh`** - `gtp_max_touch_number` goes from 1 to 5, so the frame
survives and the I2C read is long enough to carry five contacts.

**`gt9xx_touch.ko`** - the `bgez` that skips the report is replaced with a
`nop`, four bytes, at `.text + 0x186c`:

```
 before   1868:  005e1023   subu  v0,v0,s8
          186c:  04410046   bgez  v0,<next contact>
          1870:  8d560024   lw    s6,36(t2)

 after    1868:  005e1023   subu  v0,v0,s8
          186c:  00000000   nop
          1870:  8d560024   lw    s6,36(t2)
```

The instruction at `0x1870` is the branch's delay slot, so it already ran on
both paths; with the branch gone it simply runs as the next instruction and
execution falls into the reporting path. The arithmetic above it still runs and
its result is now discarded, which costs two instructions per contact and keeps
the patch to a single word.

The script finds that word by searching for the three-instruction sequence
rather than seeking to a fixed offset, and refuses to touch a module where the
sequence is missing or appears more than once.

### Using it

With no argument the script works on whatever sits in its own directory:

```bash
# report the state, change nothing
python3 tools/gt9xx_multitouch_patch.py --check

# apply
python3 tools/gt9xx_multitouch_patch.py

# put the stock behaviour back
python3 tools/gt9xx_multitouch_patch.py --revert
```

A directory can be named instead, to work straight on an unpacked rootfs
without copying anything:

```bash
python3 tools/gt9xx_multitouch_patch.py rootfs/module_driver
```

Either file on its own is enough to do that half of the job, and the script
says plainly that the other half is still missing.

Both files are copied to `*.orig` beside themselves before the first write, and
running the script twice does nothing the second time.

Then put the files back in the rootfs, repack and flash.


### What the player does with it

`src/system/gearboy/gbinput.c` is the only consumer. LVGL's evdev driver is a
pointer device - one contact, one position - so while a game runs the panel
changes owner: `panel_touch_enable(false)` parks LVGL's indev, the emulator
opens the same node itself and reads the multitouch protocol, tracking up to
five contacts. On exit everything goes back.

With an unpatched driver the emulator says so in the log and falls back to one finger at a
time:

```
gearboyplay: single-finger controls (multitouch not available)
```

Everything else in the player is single-touch by design and is unaffected
either way.

## Touchscreen multitouch, R1


### What the hardware actually does

The R1's panel is a **Hynitron CST8xx**, and it reports **two** contacts. Two
is measured, not assumed: with the immediate below set to `5` instead, a
three-finger test still only ever produced tracking ids 0 and 1.

So asking for five here would be asking for something the glass does not have.
The patch asks for two.

### Not the cause: `cst_max_touch_number`

`cst8xx_touch.sh` loads the module with `cst_max_touch_number=1`, which looks
exactly like the R3 Pro II's problem and is not it. The parameter is declared,
it has its `__param` entry, it lands in `.bss` - and **no instruction in the
module ever reads it**. Every relocation in `.text`, `.init.text`,
`.text.unlikely` and `.exit.text` was checked; the symbol is referenced by the
parameter table and by nothing else.

Changing that line alone therefore does nothing at all.

### The cause: one immediate in `hyn_ts_init`

The driver does not consult the parameter, it writes its own number into the
field the rest of the module reads, at file offset `0x171C`
(`.init.text + 0x128`):

```
 0128:  24020001   addiu v0,zero,1
 012c:  ae220058   sw    v0,0x58(s1)      # pdata->max_touch_number = 1
```

That field is written once, there, and read back in exactly two places. Between
them it decides five things:

```
hyn_ts_init        read_len = 6 * n + 3
                   buffer   = kmalloc(6 * n + 4)
                   contacts = kmalloc(28 * n)
hyn_irq_handler    if (n < (buf[2] & 0xf)) return;    drop a fuller frame
                   for (i = 0; i < n; i++)            the parse loop
```

The frame test is the R3 Pro II's behaviour again - a second finger does not
lose a contact, it loses the whole frame - but here the I2C read length, both
allocations, the test and the loop all come off the same number. So raising it
widens them together, and nothing is left inconsistent.

`6*2+3 = 15` is exactly the two-contact frame. The header is `buf[0..2]` with
the count in the low nibble of `buf[2]`, and each contact is six bytes from
`buf[3]`:

```
buf[3] bits 3..0 : x high      buf[3] bits 7..6 : event
buf[4]           : x low
buf[5] bits 3..0 : y high      buf[5] bits 7..4 : finger id
buf[6]           : y low
buf[7]           : pressure
buf[8] bits 7..4 : touch major
```

so the second contact ends at `buf[14]`, the fifteenth byte.

### The fix

One byte, the immediate:

```
 before   0128:  24020001   addiu v0,zero,1
          012c:  ae220058   sw    v0,0x58(s1)

 after    0128:  24020002   addiu v0,zero,2
          012c:  ae220058   sw    v0,0x58(s1)
```

`li v0,1` on its own occurs eight times in this module, so it is not something
to search for. The script anchors on the pair - the `addiu` together with the
`sw` that consumes it - which occurs once, and refuses to touch a module where
it is missing or appears more than once.

**`cst8xx_touch.sh`** - `cst_max_touch_number` goes from 1 to 2 as well. It
changes nothing, for the reason above; it is rewritten only so that the insmod
line and the module do not tell the next reader two different stories.
`--check` reports the number and never judges the patch by it.

### Using it

The same shape as the other one. With no argument it works on whatever sits in
its own directory:

```bash
python3 tools/cst8xx_multitouch_patch.py --check
python3 tools/cst8xx_multitouch_patch.py
python3 tools/cst8xx_multitouch_patch.py --revert
```

and a directory can be named instead, to work straight on an unpacked rootfs or
on the packer's assets:

```bash
python3 tools/cst8xx_multitouch_patch.py assets/R1/module_driver
```

Both files are copied to `*.orig` beside themselves before the first write, and
running the script twice does nothing the second time.

Note that the packer copies `module_driver/` as it finds it and does not run
this script, so the patched module is what has to live in the assets tree.

### What the player does with it

The same `gbinput.c`, unchanged: this driver speaks protocol A too. Per contact
it sends `ABS_MT_TRACKING_ID`, `ABS_MT_PRESSURE`, `ABS_MT_TOUCH_MAJOR`,
`ABS_MT_POSITION_X`, `ABS_MT_POSITION_Y`, then `SYN_MT_REPORT`; `BTN_TOUCH` and
`SYN_REPORT` close the packet.

Two differences worth writing down, neither of which needed code:

* there is no `ABS_MT_SLOT`, so the reader never switches to protocol B, and no
  `ABS_X`/`ABS_Y` either;
* `ABS_MT_TRACKING_ID` carries the real finger id and is **never** `-1`. A
  finger lifting is said by a frame with fewer contacts and by `BTN_TOUCH 0`,
  both of which the reader already treats as the last word.

`GBINPUT_MAX_CONTACTS` stays 5 on both players. It is an array size, not a
request, and a driver that reports two fills two slots.
