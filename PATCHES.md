# Firmware patches

| patch | script | what it fixes |
|---|---|---|
| Touchscreen multitouch | `tools/gt9xx_multitouch_patch.py` | the panel reports five fingers, the driver lets one out |
| Touchscreen multitouch, R1 | `tools/cst8xx_multitouch_patch.py` | the panel reports two fingers, the driver lets one out |


## Bluetooth TX discovery and car pairing

The V1 Bluetooth search now requests classic BR/EDR inquiry explicitly through
BlueZ 5.54's per-client discovery filter, with no UUID or RSSI gate. It runs for
up to 30 seconds and makes the player discoverable during that window, then
restores RX/SonixLink visibility. Stop, pairing and mode changes interrupt the
search; stale completions cannot cancel a newer request. Unsupported discovery
filters fall back to the adapter default, while real start failures are reported.

Hardware validation pending: on the Tesla, start the player's Search, then open
**Bluetooth → Add New Device → Start Search**. Confirm each device appears on the
other, pairing completes, and playback works. Also recheck headphones and RX
mode. Host marshalling/lifecycle tests are not a radio interoperability test.

## TempoTec V1 240×320 interface profile

The build of this fork is the TempoTec V1 profile. There is no runtime choice
left to make: `src/gui/board_profile.c` answers from what was compiled, host
builds included, and the HiBy R1 and R3 Pro II builds the profile was written
beside are not part of this tree. The host simulator opens at the same 240×320:

```bash
./sonix_player_host
```

The profile is more than a font substitution. It makes `gui_config_t` use the
real 240×320 panel, a 24 px status bar and 6 px page padding; remaps the named UI
font faces to compact raster sizes; and gives shared headers, settings rows,
grids and status icons compact geometry.

Rounded screen corners, the optional black quarter-circles on the system layer
in Settings → Appearance, start at **16 px** on this board
(`bp_screen_corner_radius()`); a slider under the switch moves them from 2 to
20 px ([A choice of corners](#a-choice-of-corners-and-the-original-play-button)).
The mask the helpers were originally written beside is 28 px on a 480-wide
panel; 28 px here would swallow the 24 px status bar, and 14 px (half of 28 by
pixel count) undershoots the 2" IPS, whose visible corner is about 2 mm. The masks are A8, built the first time
the option is turned on, and stay above busy veils and the screensaver.

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

The Game Boy / Game Boy Color emulator had its own geometry, written for the
480-wide panels only: the picture was the panel's full width (160×144 at 3×,
480×432), the D-pad was 220 px a side and the in-game menu was a 360 px card,
all of which ran off a 240 px screen. The V1 now fits the picture to **240×216**
with nearest-neighbour sampling: it fills the panel width at 1.5× while keeping
the Game Boy aspect ratio, trading uniform pixel-square sizes for a much larger
playing area. A compact 102 px D-pad and 46 px A/B buttons sit in the 104 rows
below it. The physical Play/Pause key supplies Game Boy Start; the previous and
next skip keys supply Select, so those two controls no longer take touchscreen
space. The in-game menu is the 200 px card this interface uses for modals on the
V1 instead of a scaled 360. The zone that opens that menu is the middle 96×72
of the picture, still where a playing hand never rests. Every touch rectangle
stays inside the panel, and no two of them overlap (the emulator's reader takes
the first zone a finger is in, so an overlap between two buttons would silently
prefer one of them). The shader choices (the pixel grid of the real panels) are
left out on this board; palettes, GBC colour correction and save states are
unaffected. See the layout comment at the top of
`src/gui/gearboy/gearboyplay.c` and the scale note in
`src/system/gearboy/gearboy.h`.

Now Playing on the V1 shows the blurred artwork behind the **whole screen**, as
Studio does on every player, with a horizontally centred square sleeve (about
150×150 px in the standard arrangement, its top aligned with the visible back
chevron and at least 18 px of air before the track-info row; a little taller
while the alternative arrangement keeps the deck to the waveform, clocks and
transport, soft-clipped corners throughout) floating on it; the source badges
and alternative-layout pills travel on the sleeve itself. The cover panel and the control deck are
both transparent on this board, so there is no horizontal edge under the
sleeve where the panel colour used to meet the deck's own blurred block. The
deck is not a fixed height: `compact_deck_height()` adds up the four rows --
names, bar, clocks, transport -- from the line heights of the fonts in force
(and the shorter 44 px waveform of the alternative layout), so the transport is
never lifted into the queue-position row above it. The full panel stays the
swipe and lyrics-gesture hit target.

Album Art Max's measuring was reaching the deck. `aam_strip_fit()` shares the
strip's row between the two names and the queue position, and it is called
from every track change and every change of place in the queue, whatever
arrangement is up. The deck's first row is not the strip: on the V1 that
column is a hundred and twenty-three pixels wide once the star and the format
line have theirs, and the strip's room is a hundred and seventy-nine. Written
onto the deck's labels the shares became a cap, and a long pair was held to
ninety-nine and eighty of the hundred and twenty-three it had -- "For Detroit
(14KT Remix)" cut at seventeen letters and "Conway The Machine" at fourteen,
neither of them at the panel's edge but well short of it. The function now
returns unless Album Art Max is the arrangement in force, which is the only
one whose labels it is measuring for, and the deck's names keep the whole
column the layout gave them. `aam_take_back()` still hands the widths back on
the way out.

The fourth arrangement, **Immersive**, is the sleeve at the bezel: the cover
panel, the clipped frame and the picture all take the size of the screen, the
corners square off, and the deck is emptied of its own fill so what stands
behind the controls is the artwork itself rather than a panel of the theme's
colour. The
controls keep the rows they have in the standard arrangement -- the deck's
height is the same, and the two clocks, the queue position and the transport do
not move. The picture above the deck is left at its own brightness -- a veil
over it is a veil over the thing the arrangement exists for -- and the chevron
and the source marks are read on it in white, as they are over every sleeve in
the player. A flat black scrim at half alpha used to cover the whole screen
here, and bands of gradient were tried before it -- a ramp's ends are a step in
the alpha's slope, and on this panel each of them draws a visible line across
the photograph -- but both darken a picture that the deck's frost already
carries the chrome on, so neither is part of the arrangement now. The
picture is asked for at 240×320 with COVER fit and drawn one pixel to one, so
the sleeve is not resampled on every frame.

What the controls stand on is a **frosted band**: the foot of the same
photograph, blurred lighter than every other blurred copy in the player --
`BACKDROP_BAND_BLUR_RADIUS` and `BACKDROP_BAND_BLUR_PASSES`, 4 and 2 against 9
and 3, so the photograph's colour masses stay standing in the glass instead of
washing out -- and dimmed to the darkness the type reads on
(`IMMERSIVE_FROST_PCT`, 55, against the 42 the blurs a title is read on
elsewhere get). The 80 the stock look glows at was tried first and turned a
pale sleeve into a light wash on the device, with the cream title and the
amber clocks floating on it; what the sleeve keeps of itself in the band at 55
is hue rather than light, and a tint in `make_backdrop()` -- each channel
pulled away from the pixel's own grey by `BACKDROP_BAND_SAT_PCT`, 140 -- sees
to it that the hue survives the dim: a dark ground of some colour, which is
what light type has always wanted here, and not a light ground of none.
The band is asked for
at the deck's own rectangle with a reach above it (240 wide by
`compact_deck_height()` plus `IMMERSIVE_FROST_OVERHEAD`, 12, tall) and cut
out of the very crop the screen-sized picture was made from -- `make_backdrop()`
takes the band from the foot of that crop rather than from a crop of the band's
own aspect, because a blur whose colours come from pixels that are not on the
screen meets the photograph above it as a seam of the wrong colour. Those
twelve pixels above the deck frost the sleeve's own foot along with the band,
so the first row of the deck is not flush against the frost's edge: the
photograph itself, blurred, a reach rather than a stretch of the band. The
band is
an image object of the artwork's frame, built before the words' page: over the
picture, under the source marks, under the deck, under the sheet the lyrics
slide in on. It is frosted glass and not a panel -- the artwork stays where it
is and is seen through it, softly -- and where it meets the sharp picture above
it is the one edge on the screen, the edge of the frost. The words' page
borrows Studio's look while it is up, and the band stays down under it: two
frosts on one screen would be one too many. With no artwork the band stays down
and the page is its own colour with the theme's ink, exactly as the standard
arrangement draws it.

The fifth arrangement, **Poster**, is the sleeve at the width of the screen
with the deck on its own ground below it: the picture is asked for at the
square of the panel's width and drawn one pixel to one, held at the top of the
panel with square corners and no inset, so it reaches the bezel on three sides
and nothing is cropped off its own sides and nothing is stretched -- which is
the difference from Immersive, where the photograph fills the screen and, the
screen not being square, takes its crop from the two sides. What the deck
leaves no room for is not in the picture either: the deck stands in front of
the sleeve's foot on the theme's own opaque panel colour rather than on the
blurred copy or on a frost, so the page reads as two grounds, the photograph
above and the controls below, with the one edge between them at the top of the
deck. The split of the panel is the standard arrangement's own
(`compact_deck_height()`), so the sleeve is as large as the deck allows and no
larger; the chrome on the deck keeps the theme's colours while the chevron and
the source marks take their chance in white over the sharp part, as they do
over every sleeve in the player. The favourite marker is the heart rather than
the star here, as it is in Immersive, but drawn a step under the star's size
(`POSTER_HEART_SCALE`, 22 px against 26 on the V1): the heart fills more of its
32 px canvas than the star does, and on a deck that is a row of type rather
than a picture to stand over, the smaller mark reads as a marker and not as a
second headline.

The one edge the arrangement leaves -- where the foot of the sleeve meets the
top of the deck -- is softened rather than drawn: a gradient from nothing to
the deck's own colour stands over the last `POSTER_FADE_HEIGHT` pixels of the
picture (36, the length of the ramp in the reference), so the photograph dims
into the controls and the two read as one ground. It is a style of two stops
with per-stop opacity (`poster_fade_grad`), retinted to the panel colour on
every theme change (`poster_fade_paint`), and up only while Poster has a
sleeve to melt (`poster_fade_apply`, from `cover_show()`): an empty sleeve
keeps its own foot, and the other arrangements keep the edges they have. Without artwork the empty sleeve is the
cover's own colour from edge to edge, and the blurred copy stays down: the
deck no longer shows it through, and the sleeve covers everything above it.

The sixth arrangement, **Album Art Max** ("AAM"), is the sleeve instead of the
controls: the picture at the full width of the panel and everything between the
status bar and the strip under it, with the transport gone and the first row's
worth of chrome above it. It is the one arrangement built for the physical
keys — the concept it comes from says so in as many words ("it trades media
controls for bigger AA (assumes you'll just use the physical controls)") — so
what is left on the page is what is *not* transport: the repeat mode, the
overflow menu and the star, in a 40 px row under the status bar with the format
line at its left end, and the title with the place in the queue on a black
strip at the foot. That strip is the deck itself, made one line tall, so nothing
above it is a panel: the sleeve reaches the strip's top edge and the blur behind
the page shows only through the header row. The rows are worked out in
`aamlayout.c`, which knows nothing about LVGL: the sleeve is decoded at the box
it will be drawn in before anything has been laid out, so the arithmetic cannot
be read back off the widgets and is a module of its own -- with the tests that
hold it to the V1's numbers (`make -C sonix-player/tests aamlayout`). On the V1
the sleeve comes back 240 x 235 in the flat shape and 228 x 235 in Leftie.

It is also the one arrangement that **keeps the status bar**, which the player
otherwise hides so that artwork can run to the bezel. With the transport off
the panel, the bar's play/pause mark is the only thing saying which way the
hardware key will go, and in the concept the bar is part of the drawing. The
chevron moves with it: `back_btn_player_top_inset()` in `switcher.c`, which is
the player's own top offset plus whatever the arrangement needs above it, so
the way back sits centred in the header row rather than in the status bar.

The three marks of that row are laid out as a set and not three times by eye:
three 36 px slots on one pitch (`AAM_HEAD_PITCH`, 30 px), the rightmost of them
keeping `AAM_HEAD_END_PAD` (10 px) between the star and the panel's edge, and
all three glyphs drawn at the same `AAM_HEAD_GLYPH` (20 px) out of the canvases
they were generated on -- the mode glyph is 30 px and the star 32, and at their
native sizes inside equal slots the mode read as the heavier button, which is
what it looked like on the device. The pitch is what puts air between them: at
26 px each glyph had 6 px of black on either side and the row read as three
marks crowding one another, so it is 30 px now -- half a glyph of air apiece --
and the test holds the pitch to that rather than to the slot positions alone.
The format line still has its 84 px beside the row (the marks begin at 142). The way back keeps the same air as the
outermost of them, and measured the same way: the chevron is drawn in the middle
third of its 36 px canvas, so padding its *button* to match the star's put the
stroke visibly further in. `back_btn_player_ink_inset()` shifts the glyph by
what the padding asks minus where its own stroke begins -- read out of the image
at run time, so regenerating the icons cannot leave a stale number behind. The menu's three lines are drawn to match:
18 px wide, 3 px to a line, on a 6 px pitch. The format line's room is measured
from the marks themselves (`aam_head_marks_left()`), so the line cannot run
under them. Both numbers live in `aamlayout.c` with the rows, and the test that
holds them also holds the row to the 6 px between glyphs it is drawn with.

Progress is the concept's "very minimal scrub bar": an 8 px band along the foot
of the picture, its foot on the picture's own edge, square-ended, and **a
stretch of colour** rather than a line -- the played part starts a step lighter
than the accent at the first second and deepens to it as it runs to the head
(`paint_progress_tint()`, which is also where the theme's colour or the sleeve's
is decided). The head is a tab, a block of the accent the height of the band and
a little wider, not a disc. The unplayed part is the picture under half black,
so the band reads on any sleeve without a track being drawn round it
(`aam_slider_style()`; the deck's own ringed, shadowed bar is put back by the
other half of the same function). The band is 8 px and its reach is not: an
`lv_obj_set_ext_click_area()` of 8 around it gives the thumb something to find
without the band itself getting heavier -- and the block the band stands in is
cut to that same reach, because a press is looked for among a parent's children
only while it is inside the parent's own box, and a reach that hung outside the
block would be a reach into nothing. The block takes no presses itself, so the
8 px it borrows from the foot of the sleeve is borrowed only on the way past.

The two clocks are not on the page at all. They come up in the middle of the
artwork, large, **only while the track is being scrubbed** — the picture dropped
to half its brightness behind them — and that is either a finger on the band
(the slider's own press events) or a held skip key, which the playback side
already reports through `device_state_scrub_active()`. Both ask the same
function, `aam_scrub_apply()`, from the poll and from the slider's callback, so
the veil is right whichever route a scrub takes.

**Leftie**, the concept's second drawing, stands the band up along the left
edge instead of laying it along the foot: the artwork gives up `AAM_RAIL_W`
(12 px) of width and the thumb travels vertically, which is the whole of the
argument for it. It is a setting of its own in the same Display options page
(`screen/player_aam_leftie`), and because the picture is then a different
shape, changing it re-measures the page and asks for the artwork again at the
new box rather than letting LVGL resample it on every frame.

The strip carries **the artist first, then the title**, the place in the queue
against its right edge: `Motorcycle` in the record's colour, `Big Ideas` in the
theme's ink, `3/13` in the record's colour too. Who it is before what it is
called -- the order the concept's own strip is read in.

The row is shared out by measurement rather than in thirds
(`aam_strip_shares()`, in `aamlayout.c` and under test): a name that fits gets
exactly the width of its own words and is not made to scroll; when the two
together overrun the row, whichever of them is short keeps its words whole and
the other takes what is left, and only when both are long is the row split in
proportion to what they need. Thirds are what the first attempt did, and a third
of a 240 px panel is seven letters -- on the device the artist of a real record
came up `Knxwled`. The queue position is measured too, rather than reserving the
width of the longest count it could ever hold: the pixels it does not need are
pixels the names get. It is re-shared on every track change and every change of
place in the queue, from `aam_strip_fit()`.

Nothing on that row scrolls unless it has to. `scrolltext.c` now decides the
long mode itself, from the width of the words against the width of the box, and
decides it again whenever either changes (`scroll_fit()`); before, every one of
its labels was a circular scroll whether or not it fitted, which left a short
line measured for an animation it never needed.

Album Art Max draws its marks in the sleeve's colour, and the theme only when
there is no artwork to take a colour from (`aam_accent()`): the band, the
artist's name, the heart, the format line and the count are one ink, by
construction rather than by three matching edits. The favourite mark is a
**heart** in this arrangement, not the star: there is no transport in the row to
outweigh it, and the one thing up there that is about the record rather than
about playing it may as well look like it.

The strip is a row of its own inside the deck rather than three children of the
deck's column, so the artist follows a title of any length instead of standing
at a position worked out from the last one; a one-pixel strut with flex-grow
pushes the queue position to the right end. Everything is reparented and
re-styled rather than rebuilt, as in every other arrangement on this page, and
`aam_take_back()` gives each widget back to the row it was built in -- the title
and the artist to the column (or to the alternative arrangement's pills), in
their original order, the queue position under the bar, and the artist line's
colour to its own style.

The transport the arrangement trades away comes back **on the picture**: a
double tap on the left third of the sleeve is the previous button, a double tap
on the right third the next one (`aam_sleeve_tap_cb()`), both routed through
`player_screen_prev()` / `player_screen_next()` so that a station, a podcast, a
book and the three-second rule behave exactly as they do everywhere else. One
tap does nothing, and the middle third does nothing either -- the gesture has to
be unambiguous, because the same press begins the drag that puts the player
away. The pair is LVGL's own `LV_EVENT_DOUBLE_CLICKED`, counted by the input
device in its configured double-click time rather than by a timer here. It hangs on the panel the sheet is dragged by, not on the sleeve frame:
that frame is deliberately not clickable, and making it so would take the drag's
presses away from it. The band, which sits over the foot of the picture with a
reach of its own, keeps its presses.

AAM is measured for the 240 × 320 V1 panel and offered only there
(`bp_is_tempotec_v1()` builds the pill, and `update_layout()` falls back to the
standard arrangement elsewhere); every number in it is one of the 24 px status
bar, the 40 px header row and the strip's one line of 15 px type. The three keys
it adds to the language files are `musicsettings_layout_aam` and
`musicsettings_aam_leftie` with its note, in all eleven of them.

The **screensaver** follows the same split. Dark and Light keep the arrangement
they have always had: the artwork stops where the blurred strip begins, and the
title, the artist and the time are read on the player's own blurred copy in
light type. The two palettes added for the V1 draw it the other way round:
Amber on Black and Tokyo Night both let the artwork run the **whole** panel,
centre-cropped, and lay a translucent field of the palette's own colour over
its foot — Amber's `#000000` at half alpha, Tokyo Night's `#16161e` at 60%, a
step heavier because the violet, cyan and blue inks are dimmer than the cream
and amber ones and have less headroom over a pale sleeve. The blurred copy is
never borrowed there: a field is a colour, not a blur.

The type in that field takes the roles the palette's own Now Playing deck gives
the same things. Tokyo Night: the track name's violet `#bb9af7` for the title,
the artist line's cyan `#7dcfff` for the artist, the accent blue `#7aa2f7` for
the clock — the screensaver's own rule, which Amber set when the clock there
became its signal amber — and the palette's ordinary icon colour `#c0caf5` for
the chevron that says how to dismiss it, at the same 70% it has always worn.
Amber: cream, amber and signal orange, unchanged. A track with no artwork
behind either field sits on the palette's page colour rather than on black, so
the same screen reads the same way whether the sleeve is there or not.

The field's colour and every line of ink are written in one place, in the
screensaver's theme-refresh callback, where they were already written for
Amber. Adding Tokyo Night beside it is a second branch in three functions —
`metadata_over_art()`, `set_info_strip_backdrop()` and that callback — and
touches the Amber path nowhere.

**Framed artwork.** The album source has always drawn the player's own sleeve.
That sleeve is decoded at its own size, about 156 px a side on the V1, so
covering the screensaver's larger area means enlarging it. In Dark and Light
it is enlarged by half again, to 240 × 188. Under the palettes' field it is
doubled, to 240 × 320, and on a 240 px panel a picture enlarged that much is
made of blocks.

Settings → Screen → Screensaver now has a second row under Album and Pictures:
*Album art: Fill / Framed*, saved as `screen/screensaver_style`. Fill is the
default, and it is the arrangement described above, unchanged.

Framed decodes the track's artwork once more, from the file, in the player's
own order: embedded first, then a cover beside the track. The decode goes
straight to the frame: the whole picture fitted inside 160 × 160 with
`COVER_FIT_CONTAIN`, never enlarged. It is then drawn one pixel to one, in a
frame with the sleeve's 14 px corners. The frame sits 40 px from either edge
and 14 px above and below, over the strip's default 132 px. The 480-wide panels
get a 400 px frame. A file that only carries a small picture is shown at its
own size.

The decode is a fourth job of the cover loader. It is asked for on the redraw
that leaves the next wake's frame in the framebuffer as the panel goes dark,
so the wake is still a pointer swap. Until it lands, the player's sleeve
stands in, at most one to one. Immersive's screen-shaped crop is never used as
a stand-in, because framed it would be a slice of the record.

The copy is let go of in five cases:

- the player lets go of its picture (the release hook);
- the style changes;
- the source changes;
- the screensaver is dismissed;
- the switch goes off.

In every theme the words sit on the page rather than on a strip. Dark and
Light lay the frame on the player's full-screen blurred copy when it has one,
which is every arrangement but Immersive. The palettes use their own page
colour and their own ink.

**Pictures from the card.** The Screensaver folder only accepts pictures no
larger than the panel. That rule came from the HiBy builds, where the panel is
also the size the note promises. The V1's language files are the ones the
upstream tree shipped, so its note promised pictures up to 480x800, while its
240x320 panel let nothing that large through, and "No pictures to show" came up
over a folder of pictures made exactly to the note.

On the V1 the limit is now the note's 480x800. The cover loader crops and
scales each picture to fill the panel, through the same scaled decode album
covers use. The screensaver test reads the size from the note in every
language and checks the limit against it.

Two smaller fixes go with it:

- The header check read only the first 8 KB of each file. An editor or phone
  that keeps its metadata (Exif with its thumbnail, a colour profile, XMP)
  puts the image size further in than that, so such pictures were turned away
  whatever their size. The check now steps from segment to segment with
  seeks, however far in the frame header sits.
- Names starting with a dot are skipped. A Mac leaves a "._" file beside every
  picture it copies to a FAT card, and each one cost a read and one of the
  random pick's eight tries.

**OLED** is the third palette added for the V1: true black, white type, white
as its accent and `#a0a0a0` for the second line, with nothing filled grey. The
page, the status bar, the Now Playing deck, the cards and the idle pills and
buttons are all `#000000`, so the layouts are carried by the type and the
glyphs alone. Now Playing goes the same way: the blurred copy of the sleeve the
V1 lays behind the standard and alternative arrangements, and behind the
lyrics, is left out, so the sleeve stands on black (`backdrop_blur_wanted()`,
asked again on a theme change, since the same picture otherwise keeps the
backdrop it has). Studio and Immersive keep theirs: there the artwork is what
the arrangement is. What would vanish with its fill gets something else
instead. A press flashes `#303030`, the only grey that is ever a fill, and only
under a finger. A quiet control (an off switch's track, a dialog's Cancel, an
idle quick-panel circle) is drawn as a 2 px `#606060` ring. A disabled switch
is faded to 40% rather than given the default theme's wash towards light grey,
which on a black track is a grey fill. A switch that is on is one solid white
pill: its ring takes the accent, and so does the track under the indicator, so
the two rounded edges have nothing dark to blend into where they meet. Tracks
stay visible, `#303030` under the progress bar and the secondary text at 40%
under a slider, since a bar with no rest reads as a stray line. Every grey the
theme draws is a multiple of eight, which RGB565 keeps exactly; a step in
between lands on different levels in the five-bit and six-bit channels and
comes out faintly green or magenta.

It takes the same compact layouts and function-matched glyphs as Tokyo Night
and Amber, with one exception: the home screen has no title or tagline, and its
grid starts directly under the status bar, the six tiles sharing the height the
header took. It is single-hue like Amber, so the places that kept Amber
monochrome — the status glyphs, the codec colours, the balanced-output gold,
the Now Playing deck, the screensaver's field — ask `theme_is_monochrome()`
now, and the deck's roles come back as steps of grey: `#888888` for the star
and the overflow, `#606060` for the idle shuffle. A white accent is also the
first one white type does not read on, so whatever is drawn on an accent fill —
a selected pill, a confirm button, the keyboard's accept key, the knob of a
switch that is on — takes `theme_on_accent()`: black above a luma of 200, white
below it. Every earlier accent, Amber's `#ffad2e` included, stays below the
line and keeps its white.

Two board matters that are not layout live beside the profile. The V1 kernel
has one switch node, `/sys/class/switch/headset`, and no `balance` node; with
nothing in either socket the headset node reads `1`, which the logic written for
the HiBy nodes took for a plug. `sysinfo_model_t` therefore carries
`headset_switch_idle_one`, set for the V1, and `headphone_jack_state_raw()` on
such a model treats `1` as no
plug and any other value as a 3.5 mm headset (never balanced: that route has
not been exercised on this board), logging the raw value to stderr whenever it
changes so the 4.4 mm socket's encoding can be read off a real plug later.

Which is also why pulling the cable out does not stop the music, and cannot be
made to. `player_output_unplugged()` -- the pause the page performs when the
output it was playing to disappears -- is wired to the two things that can
report a disappearance: the USB-C DAC, watched by the status bar's poll, and
the Bluetooth headphones, whose link the radio layer sees drop. The two sockets
cannot join them: the node above reads `1` with both sockets empty *and* with a
plug in either of them (measured on the board), so the change that poll waits
for never arrives -- and `lineout_check_jack()` has the same hole and says so in
as many words. It is the socket wiring, not a missing check: there is no signal
to read, and polling harder does not find one. **Auto power-off** is not the
answer either, by design, since it only acts when nothing is playing -- the one
case that is not draining the battery. What a forgotten player is answered by is
the **sleep timer** (`Music > Sleep timer`, and its own under audiobooks and
podcasts): it pauses when its stretch is up, counts only while something is
playing, and keeps its setting between sessions.

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
   level the panel is *looked at through*; a white reset frame at five percent
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

and a directory can be named instead, to work straight on an unpacked rootfs:

```bash
python3 tools/cst8xx_multitouch_patch.py <unpacked-rootfs>/module_driver
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

## Editing the queue

The track menu's queue page used to be a list you could only tap: one entry
started, the rest read. It is now the same list the library pages have --
long press to choose, a corner button to take the chosen entries out, a grip to
move one -- and the edits are made in the queue itself rather than in anything
that mirrors it.

### The queue becomes its own list

A library-backed queue is a handle over row ids, not a list of paths, and the
query it writes down cannot say "all of this except those three". So the first
edit turns it into what a queue of paths already is: every entry copied out
through `library_index_window()` in windows of 256, the playing order carried
over on top (the shuffle deal included), and the position playback is at
following the entry it was at rather than the slot number. `materialise_source()`
in `src/system/playback/playlist.c` does that, once per editing session's first
edit; a queue that came from "add to queue" or from the file browser is left
alone. If the copy cannot be allocated the edit fails and the queue is exactly
as it was.

Two operations sit on top of it:

* `playlist_remove_at(int index)`, indices in playback order. When it was the
  entry playing, the position stays on the slot the next entry now fills, and
  the page decides what that means.
* `playlist_move(int from, int to)`. Playback follows the entry it was on, so
  moving the playing row does not restart the track; moving a row across it
  shifts the position by one.

Both bump `revision` and clear `chain_valid`, so the on-disk queue is written
again by `device_state_queue_changed()` -- called by the page straight after the
edit, not left to the next track change.

### Choosing rows

Long press starts a selection, exactly as the library lists do. What is chosen
is kept as **one flag per queue position**, not a set of paths, because the same
track can sit in a queue twice and "remove the second copy" has to mean
something. A tap on a row takes it in or out; when the last one goes out the
mode leaves by itself. The title row then shows two corner buttons -- trash in
slot 1, close in slot 0 -- and asks for two button slots rather than none.

Removal is bottom-up: position 7 is still position 7 whatever happened to
position 2. The confirmation names the count (`selection_one_track` or
`selection_tracks_count` under the `trackmenu_remove_from_queue` title), and
after it the rows are rebuilt, the queue is persisted and a toast says how many
went. If the playing track was one of them, the track that followed it starts;
if nothing followed, or nothing is left, playback stops instead of stepping back
to the record before.

### Moving a row

Only the grip at the right of a row drags. A list that reorders on any downward
swipe is a list that can no longer be scrolled, and a queue can be hundreds of
rows long. The drag engages after `QUEUE_DRAG_ENGAGE_PX` (6 px) of travel, then
turns the list's own scrolling off (`LV_DIR_NONE`) for the duration, marks the
carried row with the accent, and follows the finger. Within `QUEUE_EDGE_PX`
(56 px) of either end it scrolls the list `QUEUE_EDGE_STEP` (18 px) at a time,
so a row can be carried past the window. An accent rule sits where the row would
land, and the drop uses the same index the rule described. A tap or a long press
on the grip is swallowed (`lv_event_stop_bubbling`) so that reaching for the
handle never starts the track.

## Deleting tracks from the card

Off by default: **Music → Settings → Library → Allow deleting tracks**
(`library/allow_delete` in the config, `musicsettings_allow_delete()` in the
interface). Of everything a selection can do this is the one that destroys
something, and it is not a thing to discover by pressing the button beside "add
to the favourites".

With it on, a track list's selection gains a trash button; the library lists
size their corner strip for five buttons instead of four, and the heading gives
up the room (36 px buttons, 4 px apart, on the V1). Only a list of tracks offers
it: an album or an artist row is not a file, and `panel_can_delete()` asks both
that and the setting.

The confirmation comes first and carries the count. A row is deletable when it
names a path under the card root and is not one track of a cue sheet --
`sheet.cue?track=3` is not something to `unlink()`, and the sheet is somebody
else's selection. Files that go are handed to `library_remove_tracks()`
(`src/system/library/library.c`), which forgets the media rows, the star, the
play count and the saved position for exactly those paths, then prunes the links
and the names left empty and bumps `GEN_MEDIA`. Nothing waits for the next
scan: the list the user just cleaned would otherwise keep answering with rows
for files that are no longer there.

A partial result is reported as such (`medialist_some_tracks_not_deleted`); a
complete failure says so instead of claiming a success.

## Play albums in random order

The circle-play menu of an album list gains a fourth action while **Play albums
back to back** is on: one record is drawn from the list, played through from its
first track, and -- with the chaining on -- the records that follow are drawn at
random too, each in its own running order. What is random is the order of the
records, not of the tracks. The row's visibility is settled each time the menu
opens, since the switch that governs it lives on another page.

It is stored as `player/album_chaining_random`, beside `player/album_chaining`,
and is only read while the chaining is on; turning the chaining off clears it,
because an order for records that no longer follow one another is state with no
meaning. Both survive a reboot, like the chaining itself, so a random walk
continues across a power cycle in the middle of a record.

The draw is a reservoir sample over `library_for_each(LIBRARY_LIST_ALBUMS)`
(`album_random_cb()`), so the walk is one pass over the album names and nothing
is copied. The record that just ended is never drawn again. Backwards stays the
alphabetical step: there is no "previous" record in an order that was drawn at
random.

The action fills the queue with that one record, not with every track of every
album: the chaining already decides the next record when the current one ends,
and a library of ten thousand tracks should not be copied into the queue to
play twenty of them. It also turns shuffle off -- a shuffled queue would
scramble the record that was just drawn -- and clears the random order again
when the plain "Play in random order" or "Play in sequence" is chosen from the
same menu, since either is an explicit statement about what follows.

### Keys

| what | key |
|---|---|
| the setting | `library/allow_delete` |
| the album order | `player/album_chaining_random` |
| settings row, and its note | `musicsettings_allow_delete`, `musicsettings_allow_delete_note` |
| trash button, and its ask | `medialist_delete_from_card`, `medialist_delete_note` |
| its results | `medialist_track_deleted`, `medialist_tracks_deleted`, `medialist_some_tracks_not_deleted`, `medialist_cannot_delete_the_tracks` |
| the menu row | `medialist_play_albums_in_random_order` |
| the queue page | `trackmenu_remove_from_queue`, `trackmenu_move_failed` |

The eleven new strings are appended to all eleven `.ini` files in
`sonix-packer/assets/V1/usr/resource/sonix/language/`, under a comment
naming the three features. Everything else the three features say -- `delete`,
`remove`, `cancel`, `selection_one_track`, `selection_tracks_count` -- was
already translated.

## Podcasts on the card

The Podcasts section's corner button opens the podcasts kept under the card's
`Podcast` folder -- one folder per podcast, exactly as the downloader writes
them (`podcastdl.h`). Until now that list only described downloads: an episode
copied onto the card from a computer was listed under its file name and dated
by the mtime the copy left behind, which is the moment of the copy and not the
publication. This change makes a hand-copied folder read like the music
library reads a track -- from the file's own tags.

An episode's row is resolved once per open of the list (`episode_row_read()`
in `src/gui/streaming/podcastsaved.c`), and the answer comes from different
places:

* **A downloaded episode** carries the sidecar the downloader wrote beside it
  ("<file>.tags"), which is the catalogue speaking and wins -- a podcast
  enclosure is often an MP3 carrying another episode's tags, or no tags at
  all. The title is the sidecar's, and the date is the file's mtime, which
  podcastdl stamps with the publication date when the download lands.
* **A file copied onto the card** has no sidecar, and its own tags speak: the
  title tag, and the date tag at whatever precision it carries. What the tags
  leave out falls back to the file's name and its mtime.

The date is the new word in `song_metadata_t`: `date` keeps the tag's own
text ("2026", "2026-03-12", "2026-03-12T08:00:00"), gathered from the same
places already read for `year` -- the Vorbis/APE `DATE`, ID3v2's `TYER`,
`TDRC` and `TDRL` (and `TDAT`, whose day and month join TYER's year after the
frame walk), the ID3v1 year, the WAV `ICRD`, the MP4 `©day`, and a `date=`
line in a sidecar, which like the other sidecar fields wins outright. When a
file carries the same date twice at different precision -- TYER's bare year
and TDRC's full moment in one tag -- the fuller one wins whatever order the
frames arrive in.

The text is read by `src/system/library/tagdate.c`, new and small on purpose:
every shape the taggers write (dashes, slashes, the compact `20260312`, one-
and two-digit parts, a time or a zone or an ID3v2.4 range after the date),
what is missing stays missing -- "2026-13" is a year, never the first of
January invented -- and the parts resolve to one moment for sorting with the
gaps settled on the first of the month, of January. The episode list sorts by
that moment newest first and formats the date at its own precision: "12 March
2026", "March 2026" or "2026". `make -C sonix-player/tests tagdate` checks the
reader headlessly.

The covers are unchanged and already did the right thing: a podcast row shows
its folder's `cover.jpg`, and with none there the artwork embedded in the
first episode's tags (`albumart_load_dir_candidate()`).

The screen is renamed with it. It held only downloads before and said so; it
holds everything on the card now.

### Keys

| what | key |
|---|---|
| the screen title | `podcast_on_card` |
| its empty message | `podcast_on_card_empty` |

The two strings are appended to all eleven `.ini` files in
`sonix-packer/assets/V1/usr/resource/sonix/language/`, under a
`; --- podcasts on the card` comment, and the two they replace
(`podcast_downloaded`, `podcast_no_downloads`) are removed from the same
block. Everything else this screen says -- `podcast_one_episode`,
`podcast_episodes_2`, `podcast_delete_download`, the month names -- was
already translated.

## Ten languages

The image carries ten language files instead of eleven: English, Italiano,
Deutsch, Español, Français, Nederlands, Polski, Português BR, Українська and
日本語. `Chinese.ini` and `Russian.ini` are out of
`sonix-packer/assets/V1/usr/resource/sonix/language/` (2762 lines of text), and
nothing anywhere keeps a second list of them.

Japanese was dropped once with them and has come back. The translation is the
original one, restored from the tree it left in, with the two tags the
interface has gained since (`appearance_stock_gestures` and its note) added to
it. What never left is what that removal deliberately kept -- the sort
collation for kana and CJK titles, the metadata reader's Chinese code pages,
the `ru`/`ja`/`zh` regional tags a podcast feed may declare, and the ebook
reader's Rodin faces.

That is the whole of what the picker needed. It has always been the folder:
`lang_list()` scans it for `*.ini` and reads each file's own `[Language: …]`
header for the name it shows, so a language that is not there is not offered,
and an eleventh dropped in beside them would be. The first-boot panel sizes
itself from the same call.

What did need changing is the other end of a choice. A device updated from a
build that had one of the two selected keeps that name in `[ui] language`,
with no file behind it. `lang_init()` used to fall back to Italiano on any
unreadable file; it now tries English -- the first-boot language -- and then
Italiano, and writes the name it settled on back to the settings, so the
choice is corrected once instead of on every boot and the settings page stops
offering a row that cannot be honoured. The fallback to tags is kept for what
it was for: a folder with no language files at all is a broken install, not a
dropped language, and bare tags are the honest sign of it.

The Japanese UI face goes with the language. `japanese.otf` -- FOT-Rodin -- is
chained in front of the main face while the interface is in Japanese, gated on
`lang_current() == "Japanese"`: kana, kanji and the CJK punctuation and
full-width forms draw from Rodin rather than MiSans, whose kanji are the
Chinese shapes, and a MiSans tail behind Rodin covers the kanji Rodin does not
have. The gate, the CJK letter test and the glyph callbacks around it live in
`src/gui/fonts/fonts.c`, and `fonts_set_language()` runs ahead of `lang_set()`
in the language page so the relabelled pages are measured with the right
faces. In any other language the chain is silent. The two files ship either
way: the ebook reader sets a book whose `dc:language` is `ja` in them,
because Bookerly has no kana or kanji at all and the reader's faces carry no
fallback chain.

What stayed gone from the original eleven-language build:

* the Ko-fi/Boosty choice in the easter egg, which read
  `lang_current() == "Russian"`. One code now, and
  `usr/resource/sonix/gui/boosty-qr-code.png` (85 KB) is gone with it;
* the `ru` and `zh` rows of the interface-language to ISO-code table the
  podcast directory is asked in -- the `ja` row came back with the language;
* the Russian and Chinese greetings, and their language files.

The UI tests that walk the language folder count ten, and an eleventh
language is meant to fail them until its texts are checked the same way.

Left alone on purpose: the Russian keyboard layout (a way to type Cyrillic in
the search box, unrelated to the interface language), the sort collation for
kana and CJK titles, the metadata reader's Chinese code pages, and the
`ru`/`ja`/`zh` regional tags a podcast feed may still declare.

## The gestures, as a choice rather than a replacement

The two sheets this interface pulls from the edge of the screen are the control
centre and Cover Flow. The fork has always put the control centre under the
status bar — pull down to open it — and Cover Flow at the foot of the main menu,
pulled up from the bottom band. The original TempoTec firmware has them the
other way round: Cover Flow comes down from the top edge of any page, and the
control centre rises from the bottom.

That arrangement was built, and it is the one the stock firmware has, but it
does not get to be the default. A player that has been updated should keep the
gesture it already knows; moving both sheets to the opposite edges in a software
update is how a device goes back to its box. So both arrangements are in the
build and **Settings → Appearance → Stock gestures** picks between them, off by
default, which is the fork's own.

`src/gui/shell/gestures.c` is the whole of the setting: `gestures_stock()` and
`gestures_set_stock()`, over `[ui] stock_gestures` in the device config, read
once and kept because the answer is asked for on every press that lands on the
status bar or the bottom edge of the screen. Nothing is rebuilt when it changes.
Both sheets ask it as they open, place their grip and their cards for the answer
and park a screen away from that edge, so the next pull uses the new edges and
the one after that would use them back again:

* the control centre parks below the bottom edge in the stock arrangement and
  above the top edge in the classic one, keeps its grabber under the status bar
  in the first and at the foot of the sheet in the second, and gives the drag the
  one free direction it has — a drag that is mostly vertical and towards the edge
  the sheet came from carries it away in both arrangements, and the extra buttons
  are on a second page that is swiped sideways, so no direction has to be shared
  with them (see "The control centre's second page");
* Cover Flow parks above the top edge in the stock arrangement and below the
  bottom edge in the classic one, with its grip at the foot of the sheet in the
  first and under the status bar in the second, and the row is laid out around
  the viewport that moves with it.

The pull that starts each one moves with the sheet. In the classic arrangement
the status bar pulls the control centre down and the bottom band of the main menu
pulls Cover Flow up; the band is the grid and the visible mini-player rather than
a strip of their own, because a strip pinned over the last pixels of the home
cards would eat their taps, and the flag `coverflow_drag_active()` is what keeps
the tap that ends that pull from opening the card underneath. In the stock
arrangement the status bar pulls Cover Flow down and the bottom edge of the
screen pulls the control centre up, watched at the input device so that the same
last pixels still take their taps.

The pages that hide the status bar keep the top pull they have always had where
it belongs to them. The now-playing page hides it so the artwork can run to the
top edge, and a strip of that artwork where the bar would be pulls whichever
sheet the arrangement puts at the top — the control centre in the classic one,
Cover Flow in the stock one — because hiding the bar must not take the gesture
away with it. The reader and the two games hide it behind a full-screen picture
instead, and they keep their own presses.

## The control centre's second page

The control centre has held sixteen buttons for some time, but only eight were
visible: the other eight were behind a stretch of the card that was pulled down
over the now-playing card (classic gestures) or unfolded with a handle (stock
gestures). That was a gesture borrowed from the way the sheet itself moves, and
it was a different gesture in each arrangement. It is replaced by what a reader
of a phone's home screen already knows: **two pages, side by side, swiped
sideways**.

`pages_view` is a window the width of the card's inside and `row_h` high, with
`LV_SCROLL_SNAP_START`, `scroll_one`, no elasticity and no momentum, so one flick
is one page and a page cannot be left half-turned. It holds `QP_PAGE_COUNT`
(`QP_SLOT_COUNT / QP_SLOT_VISIBLE`, so two) wrapping flexes of four to a line --
the very flex the single row used to be -- and `order_apply()` puts the in-use
slot *i* on page *i / 8*. The brightness (or volume) row stays where it is under
the pages. With nothing past the eighth place, scrolling is switched off and the
panel looks and behaves exactly as before; with something there, the card grows
by `DOTS_ROOM` at the expense of the now-playing card's height and a pair of dots
appears under the slider. The dots are what is shown and the strip round them is
what a tap finds: it turns to the next page. `panel_prepare()` always opens on the
first page.

The sheet's own drag has a rule of its own to keep from fighting the swipe: it
engages only when the finger has moved eight pixels towards the close direction
*and* further vertically than horizontally, and not at all once LVGL has claimed
the press for the page row. A thumb that starts a sideways swipe on a button and
drifts upwards on the way no longer takes the whole sheet with it.

### Ten new buttons

The second page is for things that were wanted with the player in the hand and
were several taps deep in Settings. All reuse the setting's own API, so nothing
is duplicated: **airplane** (see below), **crossfeed**, **soundfield**, **mono**
(`*_set_enabled()`, the same as the audio pages), **replaygain** (off, or the
mode last saved in `[audio] replaygain_last`, and the track already loaded is
measured again at once as the playback page does), **keylock** and **led**
(`[power] lock_keys_screen_off` / `led_on` through `powersettings_apply()`),
**rotate** (`display_set_rotated()` and `[screen] rotate_180`, with the
"unavailable" popup where the display cannot do it), **screen_off** and **power**
(the sheet goes away, then `power_screen_off()` / `powermenu_show()`).

Airplane mode is not a setting of its own, only two others switched at once, so
it is lit while it is *true*: `[quickpanel] airplane` is set **and** both radios
are off. Switching either radio back on from anywhere ends it without anyone
having to say so. The way into it writes down what was on in
`airplane_wifi`/`airplane_bt`, and the way out restores exactly that.

A long press on a toggle with a page of its own opens it (`second_page_hold_cb`
looks the page up at the press, since not every page exists when the buttons are
built). The Soundfield, Crossfeed, playback and power pages now read their
switches again when they come up, because the control centre changes the same
settings from outside; `musicsettings_soundfield_screen()` and
`musicsettings_crossfeed_screen()` are the two new accessors.

### Edit mode

A long press on the controls card where there is no button (`edit_enter_cb`
checks the event's target: the card, `pages_view`, a page or the dots; a button's
own long press still opens its page) calls `edit_set(true)`. Every tile gets an
outline, the picked-up one a heavier one, and the level row is swapped for a Done
button of the same height, so nothing moves. A transparent `edit_overlay` is laid
over `pages_view`: it takes the taps off the tiles without touching their
handlers, hit-tests the current page's children itself (`edit_tile_at`), and turns
a sideways swipe into a page turn since the page row can no longer be scrolled
through it. `edit_tap()` picks a tile up, or swaps it with the picked-up one in
`slots[]` and goes through `layout_changed()`, which saves and re-applies like the
settings page's moves. Swaps were chosen over a drag because the lists are kept
compact and a swap needs no drop target. `panel_prepare()` always leaves edit
mode, so the sheet never opens in it. The swap logic is covered in
`test_quickpanel_layout.c`; the overlay and the long press need a device.

### Layout and migration

The ten buttons are appended to the enum after `QP_BTN_PEQ_PROFILE`; their keys
(`airplane`, `crossfeed`, `soundfield`, `replaygain`, `mono`, `keylock`, `led`,
`rotate`, `screen_off`, `power`) are what is saved and are never to be renamed.
A **saved** layout is never rearranged: the ten join the list of buttons waiting
in Settings, like the sleep timers did. A config that has never carried a layout
at all gets the panel as it ships: page one is the eight it has always opened
with and page two is the PEQ, airplane, crossfeed, ReplayGain, mono, key lock,
screen off and power buttons, with the remaining buttons in "not in use".

With 27 buttons the list of buttons left out can hold more than the two keys
(`hidden`, `hidden_more`) allow, so `hidden_more2` and `hidden_more3` carry the
rest, eight names to a key as before (a setting is 127 characters). Older builds
read the first two and ignore the others, which loses nothing they knew of.

`order_apply()` also gained `button_available()`: a button whose module is off in
Plugins (MSEB, EQ, PEQ, Mono, Soundfield, Crossfeed) is hidden and the page
closes up round it. Before, `order_apply()` un-hid every button in the grid, which
undid the hiding done when the buttons were built.

## The control centre's slider, as brightness or as volume

The bar under the control centre's round buttons has always set the panel
brightness, which is the stock firmware's brightness bar in the place it has
always had. Some people reach for the volume far more often than for the
backlight, and the volume keys are on the side of the device rather than on the
screen they are already touching. **Settings → Screen → Volume in control
centre** gives them the same bar for the output volume, off by default.

It is one row and one slider, not two that take turns: the setting changes what
the slider does rather than which of two is on screen, so nothing is rebuilt and
nothing moves. `quickpanel_volume_slider()` and `quickpanel_set_volume_slider()`
are the whole of the switch, over `[quickpanel] volume_slider`; the sheet reads
it again at every pull, so a change is in effect at the next one, and the
settings row also applies it at once in case the sheet is already built.

As the volume bar the row keeps the three glyphs and the warning colour of the
volume pill over the player — silent below 1%, quiet below 55%, loud above it,
the fill turning red past the same 55% — so the bar says the same thing in the
control centre as it does over the player. The slider reads and writes
`get_volume_percent()`/`set_volume_percent()`, the pair the keys, the pill and
Bluetooth remotes already share: it therefore follows the output profile in use,
respects the per-socket volume limit and holds still on line out, where the level
is fixed. The poll that repaints the panel keeps the knob in step with a level
moved from outside while the sheet is up, and never moves it under a finger. The
number in the status bar above it is refreshed on every step, since it is the
same level and would otherwise lag the sound by up to a second.

## The control centre's card as a way into the player

The now-playing card under the control centre's buttons showed what was playing
and carried its transport, but the one thing a card shaped like that invites --
a tap on the track to open it -- did nothing. The title and the artist are now
that way in, the way they are on a lock-screen widget: a tap on either brings
Now Playing up, and the panel goes away as it does when a long press opens a
page from a circle.

Two details make it work with the sheet it sits on. The order is the one
`open_page()` already uses for the pages -- the control centre goes down first
and the player comes up behind it -- because the player is a sheet on the top
layer: opened under a control centre still on screen, it would be drawn beneath
the panel's veil rather than over the page. When the player is already there,
because the panel was pulled down over it, closing the panel is the whole job
and the sheet is revealed rather than reopened.

And the taps belong to the labels, not to the card. LVGL labels are not
clickable, and one line of text is a thin target on a 240 x 320 panel, so each
label is made clickable with its hit area grown past the line on every side --
the transport row is built after them, and LVGL offers a press to the last of
the overlapping children first, so the buttons keep their own. The press still
bubbles to the sheet, so a drag that starts on the title moves the panel like a
drag anywhere else, and the guard the circles use (`circle_guard_cb`) keeps the
release that ends such a drag from also opening the player.

## A metronome, and the eighth tile

The More menu gained a metronome: 30 to 250 BPM, a bar of one to seven beats,
and a tap tempo. It is a page of two cards, the way the settings pages are
built -- the tempo with its readout, its slider and its two buttons, then the
bar as a row of seven -- laid out from `bp_pick()` so it stays inside a 240 x
320 panel, and painted by hand where its colours follow the state: the chosen
beat, the button that is running, the dot that is lit.

The click is not a file. There is no tone generator anywhere else in the
player, and shipping a sample would mean shipping a decoder path for one
sound, so `metronomeclick.c` synthesises two short bursts -- a bright one on
the downbeat, a lower one between -- and writes them into the external PCM the
way the emulator's sound does: 44.1 kHz, stereo, 16-bit, in ten-millisecond
blocks, on a thread of its own, with `audio_stop()` and `radio_stop()` before
the output is taken, because one PCM has one writer.

Two decisions are what keep it a metronome rather than a ticking noise. The
tempo is kept in frames, not in sleeps: each pass writes a block and the write
blocks until the DAC has room, so the stream itself is the clock, and the beat's
position is carried as a fraction so it is not truncated once per block. And
the onset of a click is placed on its own sample inside the block rather than
on a block boundary, so a tempo change moves the click by at most one block
(ten milliseconds) and never by a whole beat.

The page does not time anything. It asks the audio thread how many beats it has
written and which of the bar the last one was, and lights the dot for that beat
-- which is why the flash cannot drift from the ear, and why a bar of three
follows a bar of seven without the page guessing where it is.

The tempo and the bar go into the config file, written when the drag ends, when
the bar changes, when the click starts and when the page is left, but only if
the value is not what is already there: a tap tempo that saved on every tap
would rewrite the card four times a second at the top of the scale. Leaving the
page stops the click, because the page is the only place it can be stopped
from.

### The eighth tile, and a rule that did not know about three columns

The More page drew eight entries in a 3 x 3 grid badly: the tile rule that
widens the last entry to fill its row -- written for five entries in a 2 x 3
page, where the last one would otherwise leave a card-sized hole -- also fired
at eight entries, where the last tile is not alone on its row at all. The wide
tile took a fourth row of its own, past the foot of a page that cannot be
scrolled, and the metronome was the entry nobody could reach.

The rule now asks whether the last entry really would be alone: it widens only
when the count leaves one place on the last row, which is the case it was
written for. Behaviour at five of six and at seven of nine is unchanged; at 2 x
3 with four entries the third row stays deliberately empty, in all three copies
of the layout code (`gridpage_set_layout`, `gridpage_build`,
`gridpage_replace_entries`), which now share the one predicate instead of
repeating it.

## A pet that eats music

The More menu has a small creature whose mood is the listening: play, and it
gets happier and livelier; leave the player quiet, and both drift down. It
grows through five stages, keeps a daily streak, pays in **Musical Notes** for
the minutes it heard, and spends them on a wardrobe.

Version 2 is about a harder question than the first one: what should a second
of the player's day be worth? The first version paid for any second with the
status at PLAYING, which meant a muted file, a volume nobody could hear and an
album left looping overnight all fed the pet just as well as a record actually
listened to. The second version pays for listening that happened, and stops
paying at the point where more of it stops meaning anything.

### What a second is worth

`petlisten.c` turns one observation of the player into the few facts the pet
cares about, and it owns every rule about what counts:

- **Audible** means the player is producing music *and* the output is at or
  above `PET_VOLUME_FLOOR` (10 percent). The percentage read is the effective
  one -- `device_state_get_volume_percent()`, the same number the volume
  overlay shows -- so a hardware attenuation, a Bluetooth sink or the line-out
  lock cannot make a quiet player look loud. Muted is zero, and zero is not
  listening.
- **Late callbacks are capped at five seconds.** The interface thread can sleep
  through a file being opened or a big cover being decoded; a gap of a minute
  is not a minute of proven listening, and paying for it would make the timer
  itself the exploit.
- **A track that ends by itself completes** -- heard to 90 percent, and at
  least 30 seconds long -- while one that is skipped does not. Three completed
  tracks of the same album in a row are an album run.
- **Focus** is thirty minutes of uninterrupted audible listening. A pause, a
  mute or a skip ends it; one or two seconds of a stopped player between tracks
  is forgiven, because that is what changing track looks like and an album is
  heard across those gaps. It is worth a lift of the heart meter on top of the
  music that earned it, as is a record heard through.
- **Night** is midnight to five, in the player's own time zone.

Nothing in `petlisten.c` draws, reads the config or asks for the time: the
caller passes `now`, which is what lets `tests/test_petlisten.c` drive the
rules frame by frame with no audio hardware.

### The day: a ceiling, and a bonus for something new

A minute of credited listening is a minute of XP. The day holds at most four
hours of it (`PET_DAILY_XP_SECS`), and that number is the whole anti-farming
answer: an album on repeat all evening is worth exactly as much as the first
four hours, and a muted player or one left playing to an empty room is worth
nothing at all. Past the ceiling the meters still move -- the music is real --
and the daily goal still counts; only progression and notes stop.

A track from an album or a genre the pet has not heard today pays 20 percent
more. It is small on purpose: it rewards the listener who puts on something new
without making a diverse library a requirement. The day's genres and albums are
kept as eight 32-bit hashes each (FNV-1a of the trimmed, lowercased tag) rather
than as text, because the settings file is read by people and two short lines
of numbers are cheaper neighbours than sixteen strings.

The daily goal is fifteen minutes, counted at local midnight. Meeting it
continues a streak. A day missed ends it -- unless a **streak freeze** covers
that day, which is what the freeze is for; one freeze covers exactly one day,
and only the day right after the last one.

### Mood without a punishment

The pet gets sleepy after forty-eight hours without music. Sleepy is not dead
and not "sad": the eyes close, the "z z" appears, and the page says so. A pat
is welcome but does not wake it -- only a track does. There is no death, no
decay into an unusable page, and no way to lose the pet; a player who puts the
device away for a month comes back to a sleepy creature that wakes on the first
song.

Happiness rises a point per twenty seconds of music and falls one per thirty
minutes of silence; energy rises one per twelve seconds and falls one per ten
minutes. A pet with no energy left earns happiness at half rate until music
feeds it again, which is the only thing hunger changes.

### Notes, the wardrobe and the achievements

A credited minute is a note, capped with the XP at 240 a day, and achievements
pay a bonus on top. The wardrobe is a sheet over the pet page, opened by the
star in the title corner, with the balance as its first row:

| item | notes | what it is |
|---|---|---|
| Party hat | 120 | an outfit |
| Headphones | 200 | an outfit |
| Scarf | 150 | an outfit |
| Crown | 500 | the long-term purchase |
| Sunset room | 180 | a room theme behind the creature |
| Night room | 240 | the second room theme |
| Streak freeze | 150 | one missed day, forgiven |

Buying an outfit puts it on, and tapping an owned one puts it back on; rooms
are chosen the same way and are independent of the outfit. A freeze is a stock
-- bought every time, never worn, and shown as a count. The page shows the
balance beside the star, and the wardrobe shows it again with the price of
everything not yet bought.

What is bought is drawn, not just recorded. The four outfits are small pieces
of the same shapes the creature is made of -- a hat with a brim, headphones
with cups over the ears, a scarf round the neck, a crown with three points --
placed from the head's own coordinates so they fit every stage, and a hat hides
the ears and the hatchling's antenna rather than letting them poke through it.
The rooms are a wash of colour over the habitat card: meadow green, sunset
orange, night blue. All of it is fixed-colour rather than themed, for the same
reason the creature is: a hat that changed colour with the theme would stop
being the hat that was bought.

Achievements are awarded by the watcher, toasted by the page, and worth notes
of their own: **Night Owl** (a minute of listening between midnight and five),
**Deep Diver** (three completed tracks of one album without a skip), **Polymath**
(five genres in a day) and **Deep Focus** (the thirty uninterrupted minutes).
The last two also lift the heart meter by ten and six points, which is what
makes a long sitting -- or an album end to end -- feel different from the same
half hour of skipping.

### Where it lives, and what it does not cost

There is no daemon and no thread. `petwatch.c` owns one `lv_timer` at 1 Hz,
created at startup beside the status bar's own timers; the interface thread is
already awake several times a second, so the pet adds one callback to a loop
that is running anyway. The page's repaint is a second timer, and that one
exists only while the page is on the glass.

The watcher is deliberately *not* registered with `power_pause_in_standby()`:
most listening happens with the panel off, and a paused watcher would credit
none of it. It reads the same predicate the rest of the player uses for "music
is coming out" -- local files and streams through `audio_get_status()`, the
radio through `radio_is_playing()`, a phone through
`btreceiver_is_streaming()` -- and the tags through
`device_state_loaded_metadata()`, which is documented as no copy and no sysfs.

The state is written after two minutes of music, on a goal, a stage or an
achievement, once per track (a discovery changes the day's hashes), when the
page is left, after a purchase, and in `power_shutdown()` -- never once a
second.

### Tested without a screen, and without audio

`pet.c` knows nothing about LVGL, audio or the config store: it is given `now`,
the local day, the seconds heard and the track's bonus, and returns events.
`tests/test_pet.c` drives that second by second -- the feeds and the decay, the
daily ceiling and the next day's reset, the discovery bonus and the fraction it
carries between ticks, all five stages, all five moods including the sleep
after forty-eight hours, the goal and the freeze-assisted streak, the wallet,
the wardrobe, the achievements, the volume floor, and the round trip through
`device_config.ini`. `tests/test_petlisten.c` does the same for the rules: the
floor, the late-callback cap, a completed album, a skip that starts the run
over, short tracks, focus, and the night window. `tests/ui/test_tools_pages.c`
opens the page on both panels, checks that nothing leaves the glass, that the
words are translated, that the button pets, and that the wardrobe sells a hat
and a freeze and closes again; the strings are in all eleven language files.

### The clock that has never been set

A player whose clock is still at the epoch adopts the pet at the epoch, and the
first real `time()` after setup would look like fifty-six years of neglect. The
watcher remembers whether `clock_is_set()` was true at startup; when it becomes
true for the first time, every timestamp the pet holds is moved by the same
amount. The same shift is done at boot when the clock is already true but the
stored timestamps are still from before 2020.

### Upgrading from the first pet, and what is not here yet

The state grew, in the same `[pet]` section, and an existing pet keeps
everything that made it what it was: its adoption time, XP (and so its stage),
happiness, energy, streak and day count are read unchanged, and the new keys --
notes, wardrobe, achievements, the day's discovery hashes -- fall back to zero.
The one visible difference on the first day is the goal: fifteen minutes
instead of twenty.

What is not in this version, and should be said plainly:

- **Genre-gated unlocks** (ten hours of jazz for the fedora, synthwave for a
  room) are not implemented. It needs per-genre listening hours kept in the
  settings file, and genre tags on a card are frequently empty or spelled three
  ways; a gate that never opens because the tags said "Jazz/Fusion" is worse
  than an honest price in notes.
- **Per-player skin and visualizer cosmetics** are not here either; the player
  skins are their own subsystem.
- **BPM** is not in the tag set this firmware reads, so the mood animation is
  driven by the genre's words rather than by tempo.
- **Listening outside the player** -- another app, another device -- is
  invisible to it, by construction.

What to expect, honestly: the pet is a spinner for listening, and the ceiling
means it is a four-hour-a-day spinner. A song left looping all night buys
nothing after four hours, which is the point. With the panel off it keeps
counting, at the cost of one wake-up a second the audio pipeline has already
paid for. And it is a hobby: nothing here touches the audio path, the library,
or the shape of the settings file beyond its own section.

## The pet, second pass: your animal, your name

> **Superseded.** The three animals, the chooser that picked between them and
> everything that drew them -- this pass, *drawn again*, *drawn as itself*,
> *three of it* and the animal parts of *on a 240 MHz budget* -- were replaced by
> one Pikachu; see *The pet, one Pikachu* below. They stay as the record of how
> the first animals were built, and why.

The first pet was a fixed creature called Momo, in an egg for its first half
hour, drawn on a page of outlined cards. Three of those were the user's first
three remarks: they wanted to pick the animal, they wanted to name it, and they
wanted no white borders. The fourth was that the page had to *fit* the 240x320
panel rather than be squeezed onto it.

### The egg, gone

The egg stage was the pet's first thirty minutes and nothing else: an oval that
could be neither patted nor fed differently, followed by a "hatching" that the
player could not miss and could not cause. It also made one store of the
creature's age say two things -- "no animal yet" and "an animal, but new" -- and
the page had to draw both. It is gone: `PET_STAGE_BABY` is the first stage, the
chosen animal is the animal from the first minute, and the stages that remain
are hatchling, child, teen and adult. The clock the rest of it runs on is
unchanged (30 minutes, 4 hours, 15 hours), and a pet that was an egg when it
saved comes back as a hatchling, which is where it would have been anyway.

### Cat, dog, dragon, fish

`pet_species_t` is stored as `[pet] species`, rejected when it is `NONE` or out
of range (an out-of-range value is reset to `NONE`, so a corrupt file asks the
question again rather than inventing an animal), and never inferred: a pet with
no species opens on the chooser. The four animals are the same body with
different parts drawn on it -- pointed ears, a tail that curls, a muzzle, horns,
fins -- so they cost the image nothing and every stage, item and mood works for
all four. Only the fish changes the scene: it swims in water rather than sitting
in a room, so the wash over the card becomes the teal of the aquarium and the
sand, the weeds and three slow bubbles come out; the room themes tint the water
instead of the air. Its shadow is removed, because a fish swims.

The names are the animal's, in the language files: `pet_species_cat`, `_dog`,
`_dragon`, `_fish`. `pet_species_tag()` maps the enum to them, and `NONE` maps
to `pet_species`, which no screen shows -- the chooser is what a species-less
pet sees.

### A name you choose

`pet_set_name()` trims the string, cuts it to 24 bytes on a UTF-8 boundary
rather than through a character, and falls back to "Momo" if nothing is left.
The name lives in `[pet] name` and is written by `petwatch_set_name()`, which
also saves. `pet_name()` is the reader, and it never returns an empty string.
The keyboard is the same one the AirPods and the Wi-Fi pages use, in its own
layer over the chooser: enter accepts and closes, the corner X cancels, and the
hardware back closes the keyboard before it closes the sheet.

### The page, measured against the panel

The habitat is one card, no border, radius 14 on this board (22 on the 480-wide
ones), sized `screen_width - 2 * padding` by whatever is left of the panel after
the title and the two footer lines. On the V1 that is 220x202, and everything in
it is placed from those two numbers:

| where | what |
|---|---|
| top left | the name (20 px), the stage under it (14 px, dim) |
| top right | a heart and a bar and its number, a bolt and a bar and its number, the wallet of notes under them |
| middle | the animal, 104x96, pat it by touching it |
| bottom left | a music note and the track it is feeding on, one ellipsized line |
| bottom right | an X that leaves the page |

The stage line says the stage and not the mood, because "Hatchling - Happy"
does not fit in the 108 px between the name's column and the meters, and the
face is already saying how it feels. The two footer lines under the card carry
the day's minutes and the streak. Nothing is outlined: the meters are bars on
the card's own colour, the corner controls are glyphs, and the only shape with
an edge is the animal.

The animal is built from the shell's own primitives -- discs, rounded bars and
arcs -- and placed from the body's size, one body per stage (46x40 as a
hatchling, 72x56 as an adult, a fifth shorter for a fish). The wardrobe's
headwear hides the ears and the horns, because two hats on one head is a hat and
a mistake.

### Slow on purpose

The animation is one bob per breath of the root, one sway of the tail (or the
fish's tail fin), and, while music is playing, two notes drifting up with the
bubbles in the aquarium. The durations are chosen for a processor that draws
this panel at a handful of frames a second: a breath is 2.6 seconds with nothing
playing and 1.1 to 1.9 with music, the tail takes eight seconds to go and come
back, the notes take four or five, the bubbles nine to fourteen. Nothing uses a
spring or an overshoot path; everything eases in and out. The animations are
started when the page is loaded and deleted when it is unloaded, and restarting
them is what a change of vibe or of species does.

### The chooser, and the test that opens it

The chooser is a sheet over the page rather than a page of its own: the title,
the name as a row that opens the keyboard, and four tiles in two rows of two --
wide enough for the longest animal name in any of the eleven languages
("Дракон"). The chosen tile is the lit one, in the accent colour, instead of a
word saying "selected". The pencil opens it, the pencil closes it, the X closes
it, and the chevron closes the keyboard, then the sheet, then the page.

`tests/ui/test_tools_pages.c` opens the page with no species at all: the
chooser is up, its four animals and its name row are there, and the page it
covers has not moved. It picks the cat, checks the sheet is gone and the page is
220x202 inside the panel, taps the animal itself (there is no button any more)
and checks that `pets` went up by one, opens the chooser with the pencil and
closes it with the same, then opens the wardrobe with the star, buys a hat and
two freezes, and leaves the page. `tests/test_pet.c` covers the rest without a
screen: the species round trip including the corrupt value, the name's trimming,
its byte cap and its default, and the stages starting at the hatchling.

## The pet, third pass: a character, a game, and a limit

Three things the first two passes did not have. The pet had moods that changed
with the track, but no *character*; notes came only from minutes listened; and a
listener flicking through a library earned exactly what a listener sitting
through an album earned.

### Where a character comes from

The obvious implementation -- "the genre that is playing decides the
temperament" -- was not used, because it makes the pet flicker: a shuffled
library would change its character four times a minute and the page would be
noise. A temperament is a month's habit, so that is what is stored: one entry a
day, the family that day was mostly spent in, in a ring thirty days long.

```
[pet] genre_history = 020000000000000000000000000031
```

Four families, sorted from the tag the way the page's vibe already sorts it
(sub-strings, case-insensitive), with one order that matters: **calm first**,
so "lo-fi hip hop" is calm rather than upbeat; **then the curious ones**, so
"indie pop" and "alternative rock" are exploring rather than chart music; then
what is meant to be danced to. Anything unrecognised is `OTHER` and counts for
nothing. With fewer than three days of recognisable listening there is no
temperament at all: a pet does not have a personality on its first evening, and
the page draws nothing under the stage until it does. The most days wins; a tie
goes to the family heard most recently. A month with no listening in it empties
the ring, so a temperament is recent listening or nothing.

The ring is shifted rather than indexed by day, which is what makes an absence
work: a device off for three days gets three `OTHER`s in a row, and one off for
two months gets an empty ring.

### What each character is worth

- **Upbeat** (pop, dance, electronic, rock, hip hop). Energy fills at 8 seconds
  a point instead of 12, and every two minutes of *credited* music a note or two
  turns up on the floor, six times in ten. The roll is a hash of the day and the
  bucket, not a random number generator: the same listening always pays the
  same, so the suite can predict it and a player cannot farm a bad seed. The day
  boundary and the daily XP cap come first, so the drops stop where the XP does.
- **Calm** (jazz, classical, ambient, lo-fi, acoustic, soul). A drifting
  soundscape: four motes rising through the card over eleven to nineteen
  seconds. It is a picture, and the documentation says so -- there is no
  synthesiser in this player, and a "soundscape" that were actually audible
  would be a second audio path on a MIPS handheld. What the month of jazz buys
  is the slow, quiet room the pet sits in.
- **Curious** (indie, alternative, punk, metal, experimental). After
  forty-five minutes of uninterrupted listening, once a day, it finds something:
  notes worth twenty to sixty, and one day in three a streak freeze instead --
  the wardrobe's rarest item, as a find rather than a purchase. The session
  length is the one `petlisten.c` already measures for the Deep Focus
  achievement, so a pause, a mute or a skip ends the forage as well.

### The tap-along

Sixteen beats at the metronome's 100 BPM, tapped on the glass, once a day. A
note falls down a lane towards a line; a tap within 120 ms of the beat is worth
two points, within 260 ms one, and anything else is lost. The score is the share
of the thirty-two points the game could pay, and it buys two notes for turning
up plus one per ten points, a fifth of the score in happiness, a tenth in
energy, and a streak freeze for 90 or more. The whole sheet is the drum -- the
label, the lane and the note are all not clickable, so a press anywhere falls
through to one full-screen button -- because a 240 x 320 panel is not a place to
ask for precision on a 16 px target.

The timer runs at 40 ms only while the sheet is open, and it is deleted when the
sheet closes. The note's position is arithmetic on `lv_tick_elaps()`, not an
LVGL animation, because the game needs to *know* where the note is when a finger
arrives; a beat nobody tapped is a miss on the same timer.

**Once a day** is `[pet] rhythm_day` compared to the local day, the same rule
the daily goal uses: it resets at local midnight and not by a timer.

### Skipping, and being dizzy

A skip is a track that was *heard* and abandoned before ninety percent of it,
and long enough to have been a track at all -- `petlisten.c` now reports it,
using the same arithmetic that already decides whether an album run continues.
Skipping is not a crime: five abandoned tracks in ten minutes is a listener
looking for something. The sixth is a listener flicking, and the pet is dizzy
for fifteen minutes.

While dizzy, `pet_advance()` credits nothing -- no XP, no notes, no stage, and
the day's XP cap does not move -- but the meters still fill, because the music
is real; it is the progression that waits. The face shows crosses for eyes and
the card says "Too much skipping".

The window is a ring of the last eight skips. It lives in memory and is not
saved: a reboot is not a way to grind skips, and it is also not a way to dodge
what you did a minute before switching off. The deadline itself
(`[pet] dizzy_until`) is saved, so a player who reboots while dizzy comes back
dizzy.

### What the state grew, and what an old pet loses

`genre_history`, `dizzy_until`, `rhythm_day`, `rhythm_best` and `forage_day`,
all in `[pet]` and all defaulting to zero. A pet from the previous build has no
history, which means no temperament for its first three days -- and since a
temperament is only ever a bonus, nothing is taken away. The day's family tally
is deliberately *not* persisted: a reboot mid-day costs that day's count, which
three-day rule cannot notice.

### Tests

`tests/test_pet.c` covers the arithmetic without a screen: the family of a dozen
real tag strings including the two that must sort towards "curious", the ring
filling over days, the three-day minimum, the tie that goes to the most recent
day, a month away emptying it, an Upbeat breakfast of 8-second energy points and
deterministic drops that pay the same twice, the skip that becomes dizzy on the
sixth and not before, XP and notes stopping while dizzy while the meters keep
filling, the fifteen minutes expiring, a skip ten minutes old no longer
counting, foraging (and not foraging for a non-Curious pet), the tap-along
paying once a day and never lowering the best score, and the round trip through
the settings file including a corrupt history digit.

`tests/test_petlisten.c` adds the skip rule itself: a track heard and abandoned
is a skip, a track that ended is not, a track only ever seen muted is not, a
sting is not, and the session length is reported for the forager.

`tests/ui/test_tools_pages.c` opens the tap-along from the third corner control,
checks it is on the panel, presses OK with no beats hit, and checks it paid the
two notes for turning up and will not pay again today.

## The pet, drawn again

The first two versions of the creature were pastel blobs: filled circles with no
line round them, which on a 240 x 320 panel read as one animal with four sets of
ears. On the real screen, in the light of the room they were photographed in,
they were worse than the mock-ups suggested -- a pale shape on a dark card, with
nothing to separate the ears from the head.

The drawing is now the interface's own idiom, the one its icons are cut in: a
flat fill inside a heavy ink line.

- **Every part with an edge wears a 2 px outline** in the same near-black
  (`style_outline_width` on the object, so it follows the shape's radius). The
  eyes and the catchlights do not: a 2 px line on a 4 px glint is a smudge.
- **Back to front.** Tail, fins, ears and horns are created before the body, so
  the body's line runs over them and an ear does not wear a stripe where it
  meets the head.
- **Four silhouettes, not four colours.** The cat has squares stood on a corner
  for ears with a pink inner ear; the dog has long floppy ears hanging past the
  jaw and the widest muzzle; the dragon has cream horns and a spade on its tail;
  the fish is a pill with a diamond tail fin, a fin on top and one under its
  belly, and no shadow, because a fish swims.
- **Bigger.** The root is 140 x 108 instead of 104 x 96 and the hatchling's body
  78 x 64 instead of 46 x 40: the animal fills the card rather than sitting in
  the middle of it. The adult is 100 x 82.
- **The light belly** is no longer outlined -- it is a patch on the body, not an
  object in front of it -- and the fish has none at all, since a gold fish with
  a pale belly reads as a puppy.

The aquarium was the other half of the complaint. A coloured wash with a line of
sand is a background; a tank has furniture:

- a 7 px band of light where the water meets the air, at the top of the card;
- sand in two tones, a wider, darker strip under a lighter one, with three
  pebbles and a rock standing in it;
- three weeds of different heights, one of them a lighter green, each with two
  leaves, planted clear of the status line's column so the words stay readable;
- six bubbles instead of three, of three sizes, rising on staggered delays.

The water itself is a 25 % tint rather than the 60 % it was at first: at 60 %
the card became a pale blue sheet and "Hatchling" disappeared into it. The
furniture does the work of saying *water*; the tint only has to say *not air*.

The header's three corner controls are also smaller: the glyph is drawn at five
ninths of the 36 px slot (20 px) instead of three quarters (27 px). A pencil
that is three quarters of its button looks like it is falling out of the header,
which is what the photograph showed.

## The pet, drawn as itself

The four animals were one animal with four sets of attachments. A round body,
two ears bolted to the top corners, a tail: a cat was a potato with a hat on, and
so was a dragon. This pass is about the silhouette, and about giving each of
them somewhere to live.

### A head on a body

Every animal is two masses now: a trunk -- the round bottom it sits on and the
shoulders its paws hang from -- and a head, drawn over it. The head is most of
the animal, the way a small animal is, and the ink line around it cuts across
the shoulders. That seam is what makes it read as a head on a body rather than
as one potato, and it cost nothing: the parts were always separate objects.

The fish has no such seam. It is one long body with the head on the front of it,
the way a fish is, and its fins do the rest.

### The shapes a rectangle cannot be

Two things went wrong before this pass, and both of them had the same cause.

A **turned object loses its outline**. LVGL draws a rotated widget through a
temporary layer the size of the widget's own box, and the outline -- which is
drawn *outside* the box -- is clipped away by it. Every ear, tail and fin in
this page is turned, so every one of them was drawn without a line: soft, dim,
and not part of the set. They now wear a border instead, which is drawn inside
the box and survives the turn.

A **turned object loses its corners**, for the same reason. A square turned
forty-five degrees comes out as an octagon, which is why the old ears read as
rounded blobs however they were placed. So the four shapes that have to be
pointed are not rectangles at all: they are drawn. `petpage.c` has a small
shape type -- a box, up to six triangles in it, and a colour each -- and a
DRAW_MAIN callback that hands them to `lv_draw_triangle()`: the ink triangle
first, grown about its middle by a quarter, the fill on top. The cat's ears
(and the pink inside them), the dragon's horns, its crest and its wings, the
fish's fan and its dorsal fin, and the two mountains behind the dragon are all
triangles.

It costs nothing at rest: nothing is allocated while the page is up, nothing is
animated, and a shape is only drawn when the animal has changed -- which the
cached redraw from the previous pass already made rare.

### Four animals, four faces

What each of them is made of now, beyond the head and the trunk:

| | ears | face | behind | tail |
|---|---|---|---|---|
| cat | two triangles leaning out, pink inside | small pink nose, two whiskers a side | a tail hooked up behind the hip | two lengths, curling |
| dog | hanging bars, out in the air to the jaw | wide soft muzzle, a black nose | a short tail, up | one bar |
| dragon | two swept horns and a three-spike crest | a long deep-green muzzle, flat nose | wings behind the shoulders | a bar ending in a spade |
| fish | none | two eyes on the front of the body | a fan, a dorsal fin, a belly fin | the fan itself |

The whiskers lie flat on purpose: a two-pixel bar turned forty degrees is
clipped down to two rows and disappears.

### Four homes

A cat on a green card and a dragon on a green card were the same card. Each
animal now has three or four flat things of its own behind it, in colours that
do not follow the theme -- a pet's rug is not going to be Tokyo Night's blue --
built once with the page and shown or hidden by species:

- **cat**: a rug, a plant in a corner, a ball of yarn with a thread coming off it.
- **dog**: grass along the bottom, a picket fence, a bone.
- **dragon**: two peaks, a strip of rock, two embers.
- **fish**: the tank it already had.

Eighteen objects for all four together; only the ones belonging to the current
animal are ever drawn. The light theme needed three of the older colours
darkened -- the sand, the fence and the bone -- because cream on a white card is
not a fence.

### The box grew a hat's height

The root the animal is built in is taller than the animal by the height of a
party hat (126 px for a hundred-pixel animal), and the root is now placed from
the feet rather than wherever its container laid it out: the animal stands on the
same floor line the four homes are drawn on, and a hat has somewhere to go. A
child cannot be drawn outside its parent, and LVGL's overflow flag does not help
here -- it extends a parent by its draw extension, and a plain box has none.

## The pet, on a 240 MHz budget

The page was correct before this pass and busier than it needed to be. What
follows is what was changed, and the two items from the same list that were
turned down, with the arithmetic that turned them down.

### Two frames, not an interpolation

There is no `lv_anim` left in `petpage.c`. The animal has two frames -- breathe
in, breathe out -- and the page's own one-second timer swaps them: the root moves
two pixels (three while music plays), the tail leans one way and then the other,
the two notes trade places and opacity, and the equalizer swaps its bar heights.
The bubbles and the calm soundscape's motes are the same idea as arithmetic: a
running offset, each on its own phase, wrapped off the top edge where nothing can
be seen of them. That is twelve integer fields written into a dozen objects once
a second, and no renderer work at all between ticks.

The tail is the one part that has to know its resting angle to lean from it, so
the angles live next to the species that own them rather than in the animation.

### Conditional redraws

`paint()` is now the rare path -- page load, theme change, a new animal, a hat --
and the one-second path is `paint_values()`. It keeps a small `shown_t` of what
is on the glass and compares:

| what | redrawn when |
|---|---|
| the animal, the room, the theme's colours | its stage, mood, species, hat or room changed |
| the two bars and their numbers | the number changed |
| the wallet | the number changed |
| "Today x of 15 min", the streak | the number changed |
| the status line | the text it would say is different |
| the note/bars in the status line | listening started or stopped |

The stage name, the name and the temperament word are written once. Nothing in
the timer calls `lv_label_set_text` with a string that is already there, and
nothing rebuilds the fifty objects the animal is made of because a clock moved.

### Flat, as it already was

The card has no shadow (`shadow_width` 0), no gradient and no gradient stop: one
`bg_color` at one opacity, one radius, and the water is the same flat fill in a
different colour. The outline on each part of the animal is style, not
rasterisation of a vector path. Nothing was added here; it is stated because the
question is fair.

### The equalizer

The status line's still note is replaced, while music plays, by three 3 px bars
in two frames (`{4, 9, 6}` and `{9, 5, 10}` pixels) in the same green. Four
objects, one timer tick, no visualiser: the point is feedback, and at this size
a bar chart is the whole story.

### The wallet, aligned

`♪ 500` used to sit left-justified under the two meters with a stray digit
wrapping below it. It is now right-aligned with `50` and `80`, in the same
column, one line high, and the meters gave up four pixels of bar to make room.

### Turned down: a pre-baked indexed sprite sheet

This is the one item on the list that was not done, and it was measured rather
than argued about. The animal is 48 LVGL objects -- a disc and a rounded bar each
-- which is about 6 KB of RAM for the whole pet and no flash at all.

A sprite sheet that replaced it would need, at the tile size this panel uses
(96 x 96 for an adult, 64 x 64 for a hatchling), a separate picture for every
species and stage and both frames: four species x four stages x two frames = 32
tiles, and then the wardrobe multiplies it -- a party hat, headphones, a scarf
and a crown each change the head, so the honest count is 5 x 32 = 160 tiles.
At one byte per pixel that is 160 x 6 KB = **about 1 MB of flash**, against 6 KB
of RAM for the objects, in an image that is already carrying its fonts and icons.
The renderer would also gain nothing: an image and a rounded rectangle are both
one blit into the same draw buffer, and the objects are redrawn only when the
animal changes, which the cache above made rare.

What a sprite sheet *would* buy is detail -- outlines that curve, a face that is
drawn rather than composed. If that is what is wanted, it is an art problem with
an art budget, not an optimisation, and it should be decided as one.

> **Revisited.** With one animal the multiplication above is gone: see *What it
> costs* under *The pet, one Pikachu*.

### Turned down: sampling the player every 10-30 seconds

The watcher already does no polling of its own: it is one LVGL timer at 1 Hz
reading the player's cached state -- the same predicate the rest of the
interface uses for "music is coming out" -- and doing about forty integer
operations with it.

Sampling less often would break rules the pet is built on rather than save
anything worth measuring. `petlisten.c` refuses to credit a callback that slept
through the music for more than `PETLISTEN_MAX_CREDIT` (five seconds), precisely
so a suspended player cannot claim an hour; a ten-second tick would be refused
by that rule or force it to be broken. The five-minute focus, the thirty-second
"did the track end or was it skipped" question and the daily goal are all
measured in seconds.

The page's timer, which *did* have work worth removing, is paused in standby
(`power_pause_in_standby`) and deleted when the page is unloaded. The watcher's
timer deliberately is not: it has to hear the music that is playing with the
panel off.

### Turned down: a page-level dim after fifteen seconds

The shell already owns the screen's brightness: it has a screensaver, an
auto-off, and it pauses timers in standby. A second dimmer inside the pet's page
would fight the first one -- two things setting the same backlight, on timers
that do not know about each other -- and the pet page's own CPU use is already
zero while the panel is asleep, because its timer is paused there and its frames
stop when the page is unloaded.

What *is* here is the quiet part: with the page open and nothing playing, the
animal breathes twice a minute and nothing else moves.

## The pet, three of it

Three corrections to the animals above: a grey cat, no dragon, and a fish that
reads as a fish.

### The dragon, and the number it left behind

The species was an `int` in the settings file and the dragon was third of four,
so deleting it is not a matter of deleting a case from a `switch`: the enum
would renumber the fish onto three, and every owner who had a dragon would find
a goldfish in its tank the next time the page opened. Nothing in a version-1
file says which build wrote it.

The file has a shape version now. `pet_save()` writes `state_version = 2`;
`pet_load()` reads the old numbers only when the key is missing or older than
two -- three was a dragon, four was a fish -- and a dragon's owner is offered the
cat rather than a fish. The test sets the key to 1 by hand and asserts all three
readings, because this is the one part of the change with no way back.

> Since removed along with the species itself: nothing reads or writes
> `state_version` or `species` any more (see *One animal, so no species*).

### Grey

The cat was cream, the dog sandy yellow and the fish gold: three warm fills in a
row, told apart by their ears alone. The cat is grey now -- `0xC3C7CC` with
`0xA2A7AE` for the paws and `0xE6E9EC` for anything pale -- which is a colour
nothing else in the set has, and the pink nose and ears still do the rest.

It also cost both animals their tails. A cat's was two turned bars that did not
meet, and cream on cream had hidden the seam; grey showed it as a stick
floating by the shoulder. The dog's was one turned bar hanging off its hip,
drawn through a layer the size of its own box and coming out with a cut edge --
which is what was read as an artifact on its right. A tail behind an animal
facing you is invisible, and a tail beside it is a turned bar, so neither has
one now. The cat is its ears, whiskers and nose; the dog its ears and snout; the
fish keeps its tail, because a fish is drawn side on and a tail is what makes it
one.

### What was in the corners

The cat's plant was a round mass with two turned bars standing on top of it: the
bars lost their ends to their own boxes, and the two of them read as ears. It is
a potted plant now -- pot, a mass of leaves, and three leaves drawn as triangles
standing out of the top of the mass, overlapping so the notches between them
stay shallow.

The dog's bone was a turned bar with its two knobs left straight, so the ends
did not meet it and it read as a pale squiggle lying in the grass. It is drawn
square on now, a bar and four knobs, and a bone. `PET_SCENE_PARTS` went to 24 to
hold the extra pieces; six more objects that are never all drawn costs less than
the wrong shape in the corner does.

### The sheet headings and the back button

The back button is not part of a page. It lives in the shell, is drawn above
every numbered screen, and sits at the top left on the same line the page
titles use. A sheet -- the chooser, the tap-along, the name editor -- is a
full-screen object, so it covers the page's title row but *not* that button;
anything it puts in the corner is written over. The three hand-built headings
went in at the page's own padding, and the chevron landed on the first word:
"‹ur pet".

`settingsrow_title()` had always computed the right place, but as a number
inside one function. It is two functions now -- `settingsrow_header_text_left()`
and `_top()` -- used by both the page titles and the sheets, so there is one
definition of where a heading starts and every heading in the interface starts
there. `test_tools_pages` asserts the chooser's heading begins to the right of
the corner button's box, which is the bug in one line.

### The plant's line

The cat's pot, its mass of leaves and the middle of the leaf fan were placed one
at a time, and had drifted two pixels apart from each other: a plant that leans.
All three are placed from one `plant_cx` now, and the leaves are mirrored about
the middle of their box.

### The fish that stayed behind

A fish is one drawn shape, and that shape is placed only while the pet *is* a
fish -- but it was never hidden when it stopped being one. The object kept the
last box it had been given and the triangles still in it, so a player who was a
fish and changed their mind found an orange sliver of it jammed in behind the
new animal's head, looking like a bag hung off its shoulder. One `show()` fixes
it. Every other part of the animal is shown or hidden by species on every
paint; this one was added with the shape and missed the rule.

### The fish

The fish was one long oval with a fan stuck on the end. The fins were right; the
body was not a fish.

It is one drawn shape now, four triangles fanned out of the snout: a flat face
in front, the back laid over the top, a narrow peduncle behind, the belly
hanging under. Hexagonal, which is as close to the oval a fish is as four
straight lines come. It has a forked tail on the peduncle -- two lobes and a
notch, where a single triangle was a fan -- a dorsal fin standing out of its
back, a pectoral under its belly, a gill behind its head, and the profile's one
eye. `lv_arc` draws the gill: an arc is the one curve in this page that is not a
rounded rectangle.

Two drawing bugs came with it, both worth writing down.

**The fills are drawn larger than their triangles.** With the fill at exactly
the size of its ink outline, the anti-aliasing along a shared edge is about half
a pixel of each, and the ink underneath came through as a hairline across the
body: three triangles in a bag, not a fish. The fills are pushed at 106 per cent
and the seams are gone. The ink still sits at 124 per cent, so the outline
survives.

**Everything drawn before the body is under it.** The dorsal fin and the
pectoral had been created with the other parts behind the trunk, which is right
for a fin that meets a body -- the join is hidden -- and wrong for a fin that was
placed entirely inside it. Both stick out now, and the gill was moved into the
face group, because a gill drawn before the body is a gill nobody sees.

### A head that was most of the animal

The cat's head was 84 per cent of the width of its body and the dog's 90, so at
every stage the animal was a head with a little of itself hanging under it --
worst at the adult, where the head is 92 pixels across on a 110 pixel body. The
cat's is 76 now and the dog's 82, and both sit four per cent deeper on the
trunk, so that a smaller head still meets the shoulders instead of resting on
them. Everything on the face -- the eyes, the nose, the mouth, the muzzle, how
far the whiskers reach -- is a percentage of the head's width, so the face came
down with it and is the same face. The hat does not: it is a fixed brim and a
fixed cone, and it still sits on the crown.

### A rounder fish

The fish's body was four triangles fanned out of the snout: a hexagon, and it
looked like one. A shape can carry an outline now. `pet_shape_t` holds a run of
corners beside its triangles, and the draw callback joins them with the same
two-pixel ink every other part wears; the hull is fourteen corners, which at
this size is an oval. A line does not care how many corners it has, so a dozen
sides cost what four did.

Filling fourteen corners with triangles brought the old seam problem back, and
the six per cent enlargement only ever worked for triangles as chunky as those
four were: a fan out of the snout is thin wedges, half a pixel of anti-aliasing
along each, and the water came through between them as hairlines. Each fill now
reaches two corners on instead of one, so neighbours lap over each other by a
whole wedge and there is no shared edge left to show -- two triangles of the
same flat colour that only touch leave a line, and two that overlap leave
nothing. No ink is needed under them either: the outline is one line round the
lot, drawn after the fills.

The body is longer than it is tall now -- 90 per cent of the trunk's width and
four pixels more than its height -- rather than as wide as it was high, which is
a ball with a tail on it however many fins are stuck to it, and the tail's hub
moved under the peduncle. The pectoral fin is gone: a fin on the side of a fish,
seen from the side, is a triangle drawn on its belly, and it read as exactly
that. The forked tail, the dorsal, the gill and the one eye say fish without it.

### The dog's ears

Five passes at the same pair of ears, which between them say more about this
page than the ears do. They have to be flat -- one fill inside the same ink
outline everything else wears -- and the dog is the same colour as its head, so
the only thing that separates an ear from the head is a line, and the only thing
that separates an ear from hair is its silhouette.

- **Over the head**: each flap's top edge crossed the crown and its inner edge
  ran down the side of the face. Two of those on a round head are a bob haircut
  with a parting, and that is what it looked like.
- **Beside the head**: pushed out until the ear only touched it at the top edge,
  the ear hung in the air, and an ear that is not on the head is not a dog's ear.
- **With a big pale inside**: the cream of the muzzle pulled in from the lobe's
  own corners. It reached down beside the muzzle and the dog grew a beard.
- **Standing up at the corners, tall**: the first pair that changed the head's
  outline, and the right idea -- and tall and narrow with it, so the dog was a
  rabbit.

What is there now is the last one, corrected: **short, wide and rounded**, out of
each top corner, leaning outward, twelve per cent of a head above the crown and
a quarter of its height down through it. Short and wide is the whole difference
between a dog's ear and a rabbit's.

Two details make them sit right. They are drawn **before** the head, so the
head's own line closes them at the root: an ear with its top edge out in the air
leaves a step in the silhouette, and a baseline drawn across the dome is a cut.
And each carries a **small pale inside** -- the cat's trick, a colour inside the
outline that only an ear has -- at the size these ears have room for it, a fifth
of the ear's width in from its edge.

The table is in head units: x from the middle of the head in per cents of half
its width, so a hundred is the edge of the head, and y from the top in per cents
of its height, negative above it. Read that way it says what the ear is, and it
scales with the animal. The box it used to live in sat a fixed eight pixels out
from the head, which at the hatchling -- whose head is sixty-four across -- was a
quarter of a head out in the air.

## The pet, and the small things it does

The animal was already a page with a face, a mood and two meters. What it was
missing was the ordinary half of a toy: a reason to press a button that has
nothing to do with it, and something to find when you come back.

### The keys that never reached the interface

`handle_key_press()` in `system.c` applied a volume step and told the interface
what the level was; nothing else ever saw the key. So a volume press could not
be answered by anything on screen. `gui_key_t` has `GUI_KEY_VOL_UP` and
`GUI_KEY_VOL_DOWN` now, posted from `apply_volume_step()` beside the step
itself -- which covers the side buttons, the cable remote, the headphone remote
and the mapped keys, because all of them go through that one function.

`key_async_cb()` answers them at the top, before the Game Boy, the book, the USB
DAC or the receiver can claim the press, and returns: a volume key is not a
transport key and the step has already been taken. Next and Previous were
announced all along, so the page only had to be told after them.

### A reaction is a shift and a rotation

There is no third timer. The page already flips between two frames every second;
a reaction is two more numbers on the same object -- a vertical or horizontal
shift, or the left arm's angle -- applied in `apply_frame()` and expired by the
one-second page timer three seconds later. A jump is up five pixels on the first
frame and two on the second; a duck is the same downwards; a lean is four to the
right or the left; a wave turns the arm up fifty degrees.

A tap picks one of three from the clock: `petwatch_now() % 3`. A coin flip is
enough to make a tap a surprise and it costs no state to remember.

### The click that a hold leaves behind

LVGL reports a hold's release as `LV_EVENT_CLICKED` as well as `RELEASED`, so
the tap handler would have run at the end of every Petting Mode and petted the
animal a second time. It is swallowed for two seconds -- `swallow_until` -- and
the easter egg on the version row documents the same trap.

Petting Mode itself is a meter of three hearts, one every six hundred
milliseconds, and the third pays. It is on the only new timer the page grew,
and the timer deletes itself when the finger is gone. The bonus is
`PET_HOLD_HAPPY_GAIN` (6) and it is once a day, kept as `hold_day` in the state:
a heart that refills by holding a finger down is not worth six points.

### What the genre puts on it

`pet_vibe_for_genre()` already read the tags for how the animal moves; the same
reading now says what it wears, in `pet_genre_look()` and with the same
first-match order, so "lo-fi hip hop" gets the cup rather than the chain. Four
accessories, flat, one at a time: shades for the electronic family, gold at the
neck for hip hop, studs for rock, a cup on the floor for the calm ones. A scarf
owns the neck and wins.

The first drawing of the chain put it at the scarf's own height, which on an
animal that is mostly head is across its muzzle: a gold moustache. There is no
neck to speak of, so both the chain and the collar sit on the chest at
`head_bottom - 1`, just under the chin.

The volume is worn on the face instead: past eighty per cent the eyes and their
glint go wide, between twenty and forty the pet rests under a heavy lid. Neither
is a mood -- sad, asleep and dizzy keep the faces they had, because a mood is the
state's and a volume is the player's.

### The box

One sheet, opened from a small chest that sits in the corner of the card all
day, with a gold latch when there is something in it. Three features share it,
and all three are un-farmable by tapping:

- **The fortune**, once a day. `pet_fortune()` draws one of six from the day
  itself, so it is the same line all day and a different one at midnight and
  asking twice costs nothing. Answering it -- "I did it" -- pays fifty notes
  once a day. The pet cannot check whether an album from before 2000 was played,
  which is the point: a fortune is an invitation, not a task.
- **The note game**, once an hour. Six notes cross one line at a fixed speed,
  about nine seconds in all; five notes for turning up and one more for each
  note caught. The note is a position on a 30 ms timer and a hit is a distance
  from the middle of the lane. No FFT, no audio, nothing to measure.
- **The finds.** Every half hour of music that was actually credited turns
  something up, counted in `pet_drop_add()` from the seconds `petlisten` has
  already decided were heard. Three at most: with the box full the listening
  stops counting, so a week away comes back as a box and not a heap. The four
  kinds come round in turn rather than at random, because a shelf with one of
  each on it is worth looking at and the order costs no state. A find is saved
  at once rather than on the two-minute throttle: it is the kind of thing a
  player switches the device off to go and look at.

A find appears on the floor beside the animal and is tapped into the shelf --
the input is the item, not a button. The floor cue and the box are the two
things on the card that are tapped rather than the animal, and neither is a
fourth corner control: the title row already carries three, and a fourth would
have been the smallest thing on the page.

### The hour and the month

`clock_is_set()` decides whether the card trusts the clock at all: with no time
set, the card stays as it was. With it, `localtime_r()` gives an hour and a
month, and after eight in the evening a wash of forty per cent black goes over
the card -- *under* the animal, so the pet stays as bright as it was and only
the room goes dark -- a moon comes up where the sun was, and a small lamp stands
at the far end of the floor. The moon is an arc, not a disc: a disc with a bite
taken out of it needs a second disc the colour of whatever is behind it, and
what is behind it changes with the room.

The month decides what falls: rain in spring, leaves in autumn, snow in winter,
nothing in summer, five small things each on its own phase, moved by the frame
the page already draws. The page asks for the hour every second and
`paint_clock()` answers from what it last drew unless the hour, the month or the
animal has changed -- so a card with nothing falling costs nothing at all.

Fourteen new strings, in eleven languages.

## The pet, one Pikachu

The pet was a choice of three animals, each assembled out of discs, bars and
triangles. It is one animal now, and that animal is a Pikachu, drawn as the pixel
art it was taken from: a front-facing figure on a 37 x 40 grid in six colours,
with a zig-zag tail on the left and a tall black-tipped ear at the top right. The
cat, the dog and the fish, the chooser that picked between them, the aquarium, the
cat's room, the dog's yard and every shape they were built from are gone. In their
place the card behind the animal is a small Pokémon-style route -- sky, hills,
trees, tall grass and a Poké Ball -- and the More menu's tile is the Pikachu's head.

### The picture, exactly

`src/gui/pet/pikachu.c` holds the picture as forty rows of thirty-seven letters --
`K` ink, `Y` yellow, `O` orange, `R` red, `N` brown, `W` white, `_` nothing -- read
off the source image cell by cell and checked against it pixel for pixel. It has
one yellow and six colours in all (`pikachu.h`: ink `252923`, yellow `F4D601`,
orange `F5901B`, red `F40202`, brown `82440A`, white `F4FBF3`). The background and
the eight cells of gap in the tail are transparent, so the card shows through
them, and the only white in it is the glint in each eye.

The preprocessor expands the letters at compile time into two ARGB8888 arrays:
`pikachu_2x` (74 x 80, two pixels a cell) and `pikachu_3x` (111 x 120, three),
23,680 and 53,280 bytes -- 76,960 together, in read-only data and nothing in RAM.
A hatchling is drawn from the first and everybody else from the second, so the
same picture starts small and round and has room to stand up in later.

It is two arrays, rather than one that is scaled, because LVGL's own scaler is not
exact and exactness is the whole of pixel art. Measured: asked for double the size
of the 37 x 40 picture it produced 73 x 78, and for triple, 109 x 117, resampled
between the pixels in both. The page never scales the picture: it sets one of the
two as an image and the image is blitted 1:1. `check_pet_picture()` in the page
test holds the page to that. It compares every solid pixel of the real render, at
both sizes, with the array it was made from, so a scaler creeping back in -- even
one that asks for 250/256 -- fails the suite.

### What is over it

The face is the picture's and stays the picture's, except while the pet is being
fed (*Fed, he is surprised*, below). What the pet feels and what it
wears are cells drawn on the same grid by one more object, a plain child of the
root with a draw callback (`gear_draw_cb`). A *stamp* is a few strings of letters
in the picture's alphabet and the wardrobe's; a run of one letter is one
`lv_draw_rect`; and the cells are as big as the picture's, two or three pixels, so
a stamp lands on the picture the way a sticker lands on graph paper. Nothing is
rotated, scaled or smoothed. What is stamped is decided when the animal is painted
and the callback only draws that list. A stamp can have a second place on the
animation's other frame, which is how the stars shift and the small heart floats,
and it can be there on one frame and gone on the other, which is how the mouth
chews and the sparks take turns.

| state | drawn |
|---|---|
| asleep | shut eyes -- a black line with yellow above and below, which is the one mood that changes the picture itself, the glints going with the eyes -- and a "z z" over the ear |
| sad | a blue sweat drop at the temple |
| dizzy | three gold stars between the ears; two drift and one sits in the notch beside the far ear, clear of the wallet's note in the corner of the card |
| blissful | a big heart and a small one that floats |
| fed (music is audible, awake) | the surprised face: round, wide eyes with a glint each, a mouth open on a pink tongue and a narrower one in turn, a spark and a bolt either side that swap places |
| wardrobe | party hat, crown, headphones or scarf, one at a time |
| genre | mirror shades, a chain with a pendant, a studded collar, or a cup of coffee on the floor |
| a dark card | the black tip of the far ear, which stands above the sky, in true black |

The scarf owns the neck, so a chain or a collar gives way to it, and the shades sit
under a hat. The hearts that pop over a tapped head and fill under a held finger are
the shell's own heart, recoloured to the picture's red.

The crown is seven cells wide where the hat is nine. At the grown size the wallet's
note, in the corner of the card, stands over the right of the gap between the ears,
and a crown nine wide covered a quarter of it; the third dizzy star sits in the notch
beside the far ear for the same reason.

Dropped on purpose: the volume faces (wide eyes past eighty per cent, a heavy lid
between twenty and forty) and the brows. Each is a different face, and the face is
the thing that is meant to be exactly the picture. `petwatch_volume()`, which only
the volume faces read, went with them, and there is no arm to wave with, so the wave
reaction shakes the animal a few pixels from side to side. Listening shows in the
notes that drift up beside the animal, in the equalizer under it and, since the
pet is fed by it, in the face (next).

### Fed, he is surprised

"Fed" is the page's own word for music being audible -- `petwatch_listening()`, the
reason the line at the foot of the card says *Feeding on ...*. The Poké Ball, the
note game and the taps are not feeding. While it is so and the pet is awake,
`paint_creature()` stamps the surprised Pikachu of the meme over the picture:

- two eyes, five cells square and round, over the picture's own, with a white glint
  at the top left of each;
- a mouth at (18, 18) that hides the picture's own (a yellow cell at each end of its
  first row covers the ends of it) and opens on a pink tongue: wide, with a dark red
  inside, on one frame; narrow, with the tongue and a lip, on the other. They take
  turns, so he chews. No cell of a mouth is on row 23, where the chest's orange is;
- a spark and a bolt of electricity either side of the animal, in the sky beside the
  head and the tail, which swap on the two frames. A sad pet has the right side
  for its sweat drop, so it has only the left;
- the root steps two pixels to one side on one frame and to the other on the next
  (`apply_frame()`): a dance, on top of the breath. A reaction under way owns the
  root instead, and a jump leaves its shadow on the floor.

A scarf, a chain or a collar is over the chin, and would hide the lower rows of the
wide mouth, so those pets get the narrow one on both frames. Shades are drawn
after the face and cover the eyes whatever the eyes are doing, so with shades the
mouth and the sparks are all there is to see. A sleeper is not fed: its eyes are
shut, and it keeps them shut.

### The shades

The shades are one stamp, `stamp_shades`, seventeen cells wide: two mirror lenses --
a dark blue, with the sky's blue under a white glint that runs from the top right
to the bottom left -- in a black frame, the bridge between them, and an arm to
each side that reaches the outline of the head. They are big enough to cover the
eyes of every face the pet can have, so no eye shows round the edge of a lens.
Shades cannot blink, so when a blink is due the glint grows a cross.

### The far ear

The black tip of the far ear stands above the sky, on the card itself. The picture's
black is `#252923`, a dark grey, and a dark card is about as light as that (OLED's
black with the Meadow room's wash over it is `#1F2B23`): the tip could not be seen
and the ear ended in the air. On the dark themes the tip's nineteen cells are drawn
again over the picture's own, in true black (`PET_TIP`, `#000000`), which is darker
than the card on every theme and room, though only just on OLED in the Night room,
where the card is about `#0D111B`; on Light they stay the picture's black, and the
hatchling, whose ear is inside the sky, has none. `check_pet_picture()`, which holds
the render to the stored array, knows about it: every solid pixel is the array's,
except on those cells, where it asks for the black.

The first try was a light grey (`#858B88`): it could be seen, but it read as a grey
ear and not a black tip. Other tips were drawn and thrown out: a cloud behind it,
which kept the ink black but looked like a white flag and ran into a four- or
five-digit wallet value in the corner of the card; and rings and halos of other
colours round the black tip, which read as a hollow outline. A one-pixel light edge
round a black tip would hold up on the darkest card; it was left off because the
plain black is cleaner.

### Alive between the breaths

Two things happen that nobody asked for, both on the page's own tick
(`life_tick()`, called from `advance_frame()`), with a small linear generator for
the dice, seeded the same way each time so the tests can count on it:

- **A blink**, every three to six seconds: a line where each eye was, no glint --
  the wide eyes of a feeding pet shut the same way, and shades glint instead -- for
  140 milliseconds. That is quicker than the tick, so the blink has a one-shot
  timer of its own, made when the lids go down and gone when they come up (and
  deleted with the rest of the page's animation when the page is left). A sleeper
  does not blink.
- **A fidget**, every eight to fourteen seconds, when nothing else is asking for
  him -- awake, not fed, no reaction under way, no finger down and no sheet open: a
  hop, a lean to one side or a shake, for two seconds. They are the reactions the
  keys and the taps start, started by the animal.

Together with the dance, that is the whole of it. Nothing runs more than once a
second but the blink, which redraws one object the size of the picture twice every
few seconds.

`petpage_blinking()` is there for the tests, since a blink lasts a tenth of a
second. `SONIX_PAGE_FILM=<prefix>` makes `test_tools_pages` write a film of the
grown animal -- fed in shades, fed bare, and left alone for forty seconds -- as one
PPM every tenth of a second; nothing is asserted on it.

### One animal, so no species

`pet_species_t`, `pet_species()`, `pet_set_species()`, `pet_species_tag()`,
`petwatch_set_species()` and the `[pet] species` key are gone, with the chooser's
tiles and caption, the sheet that opened by itself on a pet nobody had chosen, and
five language keys in all eleven language files (`pet_species`, `pet_species_cat`,
`_dog`, `_fish` and `pet_choose_pet`). The sheet behind the pencil is the name row
and nothing else, and the page opens straight onto the pet. The default name is
now Pikachu rather than Momo -- one constant, `PET_DEFAULT_NAME` in `pet.h`, used
by `pet_name()`, `pet_set_name()` and the page's first label -- and a name someone
has already given their pet, Momo included, is theirs and stays.

Nothing reads or writes `species` or `state_version` any more: the first said which
animal and the second said how to renumber it. A settings file from an older build
keeps both lines untouched -- the store keeps every key it loads and writes them
back, in a table with room for three times what is used -- so a player who goes
back to an older firmware finds its animal where it left it.

The cat's rug, plant and yarn, the fish's tank, the dog's yard -- grass along the
foot of the card, a darker strip under it and a picket fence at the side -- and the
colours they used (`PET_WATER_*`, `PET_SAND*`, `PET_PEBBLE`, `PET_WEED*`,
`PET_BUBBLE*`, `PET_CAT_*`, `PET_PLANT`, `PET_YARN`, `PET_GRASS*`, `PET_FENCE`,
`PET_INK`, `PET_CHEEK`) are gone, and so is `PET_SCENE_PARTS`, the table of two
dozen loose shapes the three of them were kept in. The first find was still called
a bone in the code: `PET_DROP_BONE` is `PET_DROP_NOTE` now, as it has been drawn
for a while, and its value is unchanged, so a shelf already filled keeps its count.

### The route behind it

The card behind the animal is a piece of a Pokémon game's overworld, in the same
flat pixel style as the animal: a sky that comes up out of the card, two ranges of
hills, a round tree and a pine, a field in three bands with tufts and three flowers,
tall grass in front of it, and a platform under the feet where the shadow lies. It
is **one object** (`scenery`, with a draw callback), 37 rows of three-pixel cells
from the animal's line down to the line of words, drawn from the same kind of
stamps as the overlay -- the cells are the picture's own size, so the route's
pixels and the animal's are the same pixels. What the yard needed a dozen shapes
for is one callback and some tables.

What it shows is decided in one place, `scene_refresh()`, from three facts: the
room worn, the hour, and the month.

| when | sky | in it |
|---|---|---|
| a day from June to February | blue, four bands | a sun, two clouds |
| the sunset room | violet to orange | a low orange sun, behind the hills |
| after eight in the evening, or the night room | navy | a moon, stars (two of them twinkle) and, from eight to seven by the clock, a lit lamp |
| March to May, by day | grey | a rain cloud, no sun, and the rain falling |

The hills, the ground and the trees take a tint of the light they are in -- navy at
night, orange at sunset, grey in the rain -- while the sun, the moon, the stars and
the lamp keep their own colours, so a lamp is lit and a moon is bright. The dark of
the evening is a wash under the scenery, over the card the name and the words are
on, not over the route and not over the animal. The top six rows of the route fade
from the card's own colour into the sky, which is why the same page sits as well on
the Light theme's white card as on the black one and on Tokyo Night's blue.

The tests read it back off the screen: a cell of sky, of the sun, of the grass and of
the platform by day; a dark sky, a moon, no sun and a warm lamp at night; a grey sky
and no sun in the rain; both rooms; and the same cells under the other four themes.
Each of the three scenes has a snapshot (`-night`, `-spring`, `-sunset`), and so
does each theme (`-light`, `-tokyo`, `-amber`, `-oled`).

A redraw of any part of the band -- the animal's two frames, a falling flake --
queues the whole route again: about three hundred small rectangles, whether or not
the dirty area touches them, because a draw callback cannot see the dirty area
(`lv_layer_t`'s clip is private) and so cannot skip what lies outside it; the
renderer clips each one. That is negligible on the host and has not been measured
on the device.

### The Poké Ball

The box in the corner is a Poké Ball now, on the grass in the same place: seven
cells wide, a red top, a black band and a button. It is the same button -- a tap
opens the fortune, the tap-along and the shelf -- and it says what the box said by
colour: the button is white, and it is gold with a three-cell sparkle while
something is in it, which is today's fortune not yet carried out, a find on the
floor or the hour's game still to play. The find that turns up beside it is
unchanged.

### The More menu's tile

The tile had a generic pet glyph. It is a Pikachu's head now, drawn by
`tools/pet_icon.py` in the picture's own six colours (`assets/icons/pet.svg`) and put
on the violet tile that Dark and Light show by `tools/make_more_tiles.py`, a little
larger on it than a line glyph would be because the ears would be lost otherwise.
The same drawing, as one white shape with the face cut out of it, is `icon_pet` in
`peticon.c`, for the themes that show function glyphs. `icon_menu_pet_data` in
`icons.c` was replaced in place -- `svg_to_lvgl.py` needs cairo and would rewrite the
whole file for one picture -- with the new tile resampled to 112 pixels the way the
old array was made from the old tile (checked: the old array equals that resample of
the old `pet.png`). `screenshots/more.png` is a photograph of a device and still
shows the old glyph.

### What it costs

| | before | after |
|---|---|---|
| the animal | 48 objects, about 6 KB of RAM | 4 objects: the picture, the overlay, the shadow and the sleeper's "z z" |
| `petpage.c`, object code (with the route) | 60.8 KB | 42.1 KB |
| `petpage.c`, static RAM | 6.8 KB | 4.1 KB |
| `petpage.c`, source | 4,055 lines | 3,661 lines |
| the pictures | none | 76,960 bytes of read-only data |

(Host `-O2` build of the one file; the arm code is denser and the pictures are not.
The surprised face, the shades, the ear tip, the blink and the fidget are 3.6 KB
of that code and 0.6 KB of the RAM.) Still one callback a second and two frames, no
`lv_anim`, and nothing redrawn that has not changed -- but for the blink, one timer
of 140 milliseconds every few seconds.

The earlier entry that turned down a pre-baked sprite sheet -- about a megabyte,
for four species x four stages x two frames x five wardrobe states -- is made moot
rather than contradicted. With one species the multiplication is gone: the stage is
a size, the two frames are the root's own movement, and the wardrobe is drawn over
the picture instead of being baked into it. What is baked is two pictures, and
that is 75 KB.

### Tests

`test_tools_pages` no longer opens a chooser. The page opens onto the pet, the name
sheet is hidden until the pencil is pressed, and `check_pet_picture()` holds the
render to the stored picture at both sizes, under all five themes. Every mood, garment and accessory has a
snapshot (`-sleeping`, `-sad`, `-dizzy`, `-blissful`, `-hat`, `-crown`, `-phones`,
`-scarf`, `-scarf-chain`, `-adult` and `-name`, beside the ones that were there) and
one cell that only it paints is read back off the screen by its place in the grid,
so a stamp that drifts off the grid, or never appears, fails. `test_pet` loses its
species tests and the migration's.

The surprised face, the dance, the blink and the fidget are read back the same way,
with the page run a second at a time (`pet_second()`). The feeding face is asserted
on both frames -- the two mouths, the sparks swapping, the glints and the edges of
the eyes -- bare, in shades, in a scarf, a chain and a collar (the narrow mouth) and
asleep (no face at all). The dance must be two pixels either way and alternate;
the shadow must stay on the floor in a jump; a blink must come within ten seconds in
each of the three kinds of eyes and be over two hundred milliseconds later; and a
fidget must come, between four and twenty-four seconds of it in a minute. Move the
tip a cell, take the neck rule out, stop the blink or the dance, or shift a lid by one
cell, and the page test stops. `pet_view_take()` lets the layout catch up with a
step before it reads where the picture is.

All of the moods, garments and accessories are done twice, once as a hatchling and
once as an adult (`-adult-sleeping`, `-adult-crown` and so on), because the grown
animal is the one that reaches the wallet's note. The note's pixels are the only
light grey in a window above the head, so the test records them from the plain
adult and asserts that every mood and garment leaves each one as it was. (Put the
nine-wide crown back and it stops there.)

### What did not change

Everything else about the pet -- what feeds it, the meters, the notes and the
wardrobe's prices, the fortune, the tap-along and the shelf behind the ball, and the
dizzy rule -- is as it was. The other More tiles' pictures, which is the rest of
`icons.c`, are untouched, and so is `petlisten.c`.

## The charger, with the player off## The charger, with the player off

The stock player comes up on a battery screen when the device was switched off
and a cable is plugged in: the charger powers the board, and the thing the user
plugged in for is the charge, not the library. Replacing the stock binary means
that boot lands in this player instead, so the same boot has to be recognised
and answered the same way.

### What the hardware does, and does not, say

Nothing in the kernel reports whether the board was powered by the key or by
VBUS. The AXP2101 knows, in the power-on-source field of its status register,
but that register is not exposed on this firmware, and the kernel offers no boot
reason either. What *can* be known is what the device was doing before it
started, and that is the question actually being asked: a boot with a cable in
is a charger boot only if the player was off when the cable went in.

So the player writes a marker beside the settings, on `/usr/data` -- the one
partition that is writable and always there:

```
/usr/data/.sonix_player_running
```

It goes down as the player's own startup begins and `power_shutdown()` takes it
off, first, before the databases are closed and the card is unmounted. A boot
with a cable in and **no** marker is therefore a device that was off and has
just been put on a charger:

| how the device started | marker | what comes up |
|---|---|---|
| Power off (menu, auto-off, flat battery), then the cable goes in | absent | the charging screen |
| Power off, then the power key | absent, no cable | the player |
| Reboot from the menu, a crash, a firmware update | present | the player |
| Power cut, cell pulled -- no clean shutdown | present | the player |
| Factory reset (recovery wipes `/usr/data` on the next boot) | absent | the charging screen, if the cable is in |

The marker says "this device was running the player", not "the player was
switched off purposefully", because that is the answer that keeps a battery
screen off a reboot: a crash leaves the marker, and the device comes back to the
player the user was using. The cost is the fourth row -- a player whose power
was cut with the cable in boots the player rather than the battery screen -- and
the fifth, which is the recovery script's wipe of `/usr/data` and the one thing
here that no code in the player can observe. Both are rare, and both are
answered by holding power for a moment, which is what the screen would have
asked for anyway.

### The screen

`chargescreen.c` runs before the services start, on purpose: it reads the gauge
through `battery_read_now()`, the cable through `usb_vbus_present()`, and the
power key off its own evdev node, so no input thread is running to answer a key
it is using. A hold would otherwise open the power menu of a player that has not
been built yet.

It draws a battery whose fill is the level, the percentage, and one of three
states -- charging, charger connected with nothing going in (full, or held at
the charge limit), or no cable -- then darkens itself after ten seconds. **Press
power** to bring it back; **hold power** for the same 1.2 s the power menu's
long press waits for, and the player starts. With the cable taken out while the
screen is dark it switches the player off instead: a device that was off, was
woken by a cable and no longer has one has nowhere to be.

The panel is blanked and woken the way `power.c` does it, which on this board is
not optional: the SLCD leaves its memory white on reset and loses a frame pushed
before it is listening, so the backlight is held at zero across the unblank and
the settle, and brought up over a frame that is already in the framebuffer. The
code is short and repeats `power_screen_on()`'s order deliberately; the long note
on that function is the reason for each step.

Every line on the screen is given the panel's width and told to wrap. The three
states are short in English and not in every language -- the Ukrainian for
"charger connected" is wider than the glass at 22 px -- and a label that sized
itself to its text would simply leave the panel. That is not theory: the
headless test below found it on the first run, in the state line.

`[power] charge_screen = 0` in `device_config.ini` turns the screen off, and
`[power] charge_screen_seconds` moves its timeout (ten, floor five). On the
simulator `SONIX_CHARGE_SCREEN=1 ./sonix_player_host` brings it up, with P as
the power key.

What this cannot add is the boot itself: a board whose PMIC does not power it up
when VBUS arrives stays off, and no amount of software will see the cable.

`make -C sonix-player/tests ui` builds and runs `test_chargescreen`, which draws
the screen on the real renderer at both panel sizes: every object inside the
glass, the fill within a pixel of the reading it was given and never past the
box at 100%, `--` and an empty bar for a gauge that answered nothing (never
`0%`), and the three state lines in each of the ten languages -- stepped through
one reading at a time, because the screen only repaints a line that has
changed.

## The last five percent

A player that runs until the cell is empty does not switch itself off. The
regulator drops out from under it with the card's databases open and the position
unwritten, which is how a card acquires a database it has to repair on the next
boot -- if it does.

`batteryguard.c` is the rule, and it is its own module with no LVGL, no sysfs and
no clock, so it can be driven by a test (`make -C sonix-player/tests
batteryguard`) rather than only on a device with a battery on its way flat:

* **5%** warns, once per discharge. The warning is the red notice card, held for
  eight seconds instead of the usual three, and the panel is brought back up
  first if it was dark -- in standby, which is exactly when a warning is needed,
  there is nothing on screen to warn with. The wake is not a loan: ten seconds
  after the card goes up the panel goes dark again, because a warning that
  leaves the panel lit for the rest of the discharge costs more than the percent
  it is about. A press or a touch in the meantime hands the screen back to the
  user, and it stays on as long as the ordinary screen-off timeout says.
* **1%** goes down through `power_shutdown()`: the same way as the menu's Power
  off, which is the point of doing it at one instead of at none.
* Neither happens on the charger. The question is the cable, not "a charge is
  going in": a battery held at the 80% charge limit reports exactly that, and the
  cable is still carrying the player.
* An unreadable gauge (`"!!"`, or any non-numeric answer) is not a flat battery,
  and does nothing at all. The thresholds are on the reading the status bar's
  own poll already refreshes, so the guard costs no sysfs traffic of its own.

### Standby, and the one state it cannot see

The tick that carries the guard runs with the screen on, off, or while something
is playing, which is what "also during standby" comes down to on the device. The
exception is suspend-to-RAM: nothing runs there at all, so a player that went to
sleep at 12% could reach 1% with nobody watching.

Below 10% and off the cable, `suspend_if_idle()` therefore refuses to sleep, with
the reason in the log. The screen stays dark as it was -- the brightness of a
dark screen is not the cost the sleep saves -- and the player waits, awake, for
either the cable or the guard's shutdown at 1%. Above 10% the ordinary standby
applies: a full battery is allowed its sleep, and a device left for weeks on a
shelf is still going to die the way any device left for weeks on a shelf does.


## Russian comes back

`Russian.ini` is back in `sonix-packer/assets/V1/usr/resource/sonix/language/`,
beside the ten that stayed, so the picker offers Русский again in Settings
and on the first-boot panel. Only Chinese is still out. As before, the picker
needed nothing told to it: `lang_list()` scans the folder for `*.ini` and
reads each file's `[Language: …]` header for the name it shows.

The text is the recovered file, brought up to date rather than retranslated:
strings added while Russian was gone -- the stock-gestures row and its note,
the control-centre volume row, ListenBrainz, Smart Mix, the metronome and the
battery guard -- are translated too, and its lines are back in the order the
other files share, so all eleven line up. Nothing else needed a font or a
layout: Cyrillic was already drawn for Ukrainian and for tags, and the Russian
keyboard layout never left.

The collateral keyed off the name comes back with it: the first-boot greeting
("Добро пожаловать" -- still the wide one the welcome page is sized around),
the `ru` row in the podcast directory's language table, and the Ko-fi/Boosty
choice in the easter egg, with
`usr/resource/sonix/gui/boosty-qr-code.png` beside the Ko-fi code. Ko-fi takes
no cards issued in Russia, so the Russian interface gets Boosty again and
every other language gets Ko-fi.

The `lang_init()` fallback written for the removal stays, for the one name
that is still gone: a device whose settings say Chinese starts in English and
has the name written back once. A device that kept Russian in its settings and
skipped the builds without it simply loads Russian again; one that ran those
builds was already corrected to English, and picks Русский from the list like
any other change.

The UI tests that walk the language folder asserted ten files; they now
assert eleven, and a twelfth language is meant to fail them until its texts
are checked the same way. The main-menu test switches into Russian as it does
into Ukrainian, and checks the name the file declares for itself.

## Tidal port from qir3k's fork

The Tidal work from `qir3k/sonix-player-tempotec-v1` is replayed here as
22 path-filtered cherry-picks. Each keeps its original author, message and a
`(cherry picked from commit …)` trailer, but carries only the Tidal modules and
their two new helpers (`listcache` and `streamkeep`): `dfa8a97`, `e8cbce0`,
`870cbb8`, `1e1e101`, `54b9f46`, `1fa0648`, `50a838d`, `9f305ed`, `ac35cf9`,
`c0f8ea5`, `05d8710`, `c6e7d53`, `c20256c`, `b1baa7c`, `0865ab5`, `6a65666`,
`d135561`, `e314702`, `c9becf9`, `e8b4c21`, `339b433` and `b008638`. Unrelated
fork work — YouTube Music, radio and podcast redesigns, Bluetooth, themes and
release machinery — is deliberately not part of the port.

Together those commits bring the 240 px Tidal layout, a search page with its
last eight queries, favourite artists, playlist-track removal, configurable
home rows, card-backed list caches, bulk row selection, serialized API and sync
workers, queued/cancellable downloads, a Downloaded browser, "Keep on card"
saving with tags and artwork, storage-failure reporting, and an SD-card-backed
playback cache for hi-res DASH streams.

The integration commit adds the shared hooks the filtered commits need on this
newer upstream: a writable `device_state` prepare callback (and matching Qobuz,
podcast and Subsonic signatures), the track-menu actions, path-backed media-list
subtitles, dynamic-title realignment, the shared row-duration formatter, cache
cleanup at startup and card removal, and the new translation keys in every
language file. It also adds the `lang.h` include `streamkeep.c` needs for `tr()`.

## The screen coming back on, and the music over Bluetooth

Issue #188: with music playing over Bluetooth — headphones fed from here or a
phone streaming to here — pressing the power button to light the screen up is
heard as a short interruption, a stutter or a crackle, every time. Nothing about
the way the player *stops* the screen does it; only the way it *starts* it.

### What is actually in that moment

Waking the panel is not a neutral act on this board. `power_screen_on()` writes
`0` to the framebuffer's blank node, and the driver behind that node does not
merely light a backlight: it resets the panel, brings its VCC and VCCIO back up
at the PMIC, re-initialises the glass (the 120 ms of sleep-out delays that
`WAKE_PANEL_SETTLE_MS` exists to wait out) and re-arms the scan-out. Everything
on the board shares that moment. It is where the CS43198's stale volume register
came from — the distortion of issue #167, patched for by rewriting the volume at
the end of the wake — and it is where the Bluetooth module's link is disturbed:
a stream being encoded for a pair of headphones (A2DP source) loses packets on
the way out, a stream being decoded from a phone (A2DP sink) loses them on the
way in, and both are heard as the same thing. The power-*down* is reported to do
neither. A rail going away is quiet; a rail coming back up is not, and that
asymmetry is exactly what the two reports say.

Worth saying, because it was looked at first and is not it: the Wi-Fi radio.
`wifi_wanted()` deliberately counts a connected pair of Bluetooth headphones, so
the radio is never parked out from under a live link and `unpark_radios()` has
nothing to bring up at a wake. Also not it: the volume. The wake's
`set_volume_percent()` is deduplicated on the Bluetooth side before it reaches
bluealsa, so no AVRCP write goes out on a level that has not moved.

### The fix: the panel's power state follows the stream

While a Bluetooth stream is playing, the screen going dark no longer powers the
panel down. The backlight is taken to a true zero — which is the whole of what
the user sees — and the panel is left unblanked with its last frame still being
scanned out to the glass. The wake is then the backlight coming back up over a
frame that never left it: no reset, no panel power-up, no settle window, no
`FBIOPUT_VSCREENINFO` on a display that is still running, and nothing at all
happening to a rail or a radio at the moment the button is pressed.

Three details make it a state and not a hack:

- **It follows the stream, not the backlight.** The state is re-read on every
  power tick (`panel_hold_tick()`): if a stream starts behind a panel that was
  already blanked — resuming from the headphones with the screen dark, which is
  the same bug one press later — the panel is brought up then and there, behind
  a backlight at zero, so that the wake the user is about to ask for does not
  have to. If the music stops (a pause, the end of the album, the headphones
  walking away) the panel is powered down for real again, `PANEL_HOLD_QUIET_MS`
  (4 s) later, so a track change, a skip or the gap between two gapless tracks
  is not a power cycle.
- **The wake skips the parts that exist for a blanked panel.** A held panel was
  never reset and never lost a frame, so `power_screen_on()` repaints once — the
  page underneath may have changed while nobody was watching — waits a single
  scan-out rather than the two the blanked path waits for, and fades up. The
  white-flash machinery is not merely unnecessary there, it would be wrong: it
  re-arms a mode that was never torn down.
- **It is a trade, made on purpose.** An unblanked panel, its controller and a
  touch controller that stays awake behind it cost a few tens of milliwatts that
  a blanked one would not — for as long as the stream lasts, and not one tick
  longer. A card that would rather have the milliwatts than the steady stream
  sets `[screen] panel_hold = 0` in `device_config.ini` and gets the old
  behaviour back, stutter and all. The suspend path puts a held panel down
  before it writes `mem` regardless, so nothing about standby changes.

One smaller thing rides along: the wake no longer rewrites the CS43198's volume
when the sound is leaving over the air. There is no register of the DAC's in that
path at all — it is the one output with nothing to put back — and the call ends
in a write to bluealsa's PCM object, through the same daemon that is encoding the
track, which is the last thing a wake should be doing while the music plays. The
Bluetooth *receiver* is not that case: there the DAC is playing, and the rewrite
stays exactly where it was.

The log says which of the two wakes a card took — `power: screen on
(brightness=…, wake …ms)` with `, panel kept` appended for the held one, and a
`power: screen off with the panel kept powered for the Bluetooth stream` line
when the hold goes in. A held wake is that one repaint and a fade against the
quarter second the blanked one spends holding a dark panel through its own
power-up, which is the difference the listener hears.

## Two PEQ profiles and the Control Centre quick switch

Issue #187 asks for different corrections for wired IEMs and Bluetooth earbuds
without replacing one curve every time the headphones change. PEQ now has two
persistent profiles. The PEQ page selects either one; the numbered Control
Centre button cycles between them, and a long press opens the PEQ page. The
button's fill follows the selected profile's own enable state. It appends to
the existing button list: an empty grid place is used as-is, while a full grid
keeps the new control in Settings → More → Control centre until room is made.
No saved button order is rewritten on upgrade.

Each profile owns all ten bands, preamp and the PEQ enable switch. The active
settings stay in `[peq]`, with `profile = 1` or `2`; the other curve lives in
its own numbered `[peq_profile_*]` section. An existing `[peq]` becomes Profile
1 without a boot-time write, and Profile 2 starts flat and off. Switching
archives the outgoing curve under its own number before publishing the other,
so restarting from either profile preserves both. Older builds can still read
the last active curve from `[peq]`. APO preset files remain shared, but import,
export and reset operate on the selected profile; switching never enables PEQ
on the user's behalf. Graphic EQ and MSEB are unchanged.

The audio path takes a coherent profile snapshot under a short mutex, outside
disk I/O. It uses a try-lock and retains the previous coefficients for a buffer
when an edit is in progress, rather than blocking playback. The settings table
has room for the inactive profile's 52 additional keys. Both PEQ screens
refresh after a quick switch, including an open band editor. The two new
translation keys are present in all eleven shipped languages.

Host coverage: `make -C sonix-player/tests eq` checks profile persistence from
either slot, independent enable/preamp/bands, scoped presets/reset, legacy
upgrade and actual PCM processing. `make -C sonix-player/tests peq-ui` checks
the real page in eleven languages and five themes at 240×320; `quickpanel-layout`
checks the production layout migration and moves. Both are included in `ui`.
These host tests do not replace an on-device listening and touch check.

## USB DAC controls in the Control Centre

Issue #209 asks to restore access to the audio controls while the player is a
USB DAC. The Control Centre can now be pulled over DAC mode. Its now-playing
card becomes a DAC status card (waiting/starting, or the host's PCM/DSD rate)
and hides the local transport, so a play tap cannot start a second audio
source over the USB stream. The regular quick controls remain available,
including graphic EQ, MSEB, PEQ/profile, gain and line-out; the DAC page's own
back guard still asks before leaving DAC mode. Cover Flow remains unavailable
while the DAC owns the page.

The USB audio thread now runs incoming 32-bit PCM through the same live EQ
engine as decoded tracks, so the existing GEQ, PEQ and MSEB toggles change what
is sent to the internal DAC. Native DSD is not PCM and bypasses the filters
byte-for-byte. `make -C sonix-player/tests usbdac-eq` checks the production
format gate against the DSP engine, including the DSD no-op; the quickpanel
layout suite also checks that DAC no longer blocks the Control Centre while
file-transfer and Bluetooth-receiver modes remain blocked. The tests do not
replace an on-device touch and listening check.

## External USB DACs that only worked in the trial

Two USB-C dongles were reported against a V1: a JCally and, later, an ESR. Both
did nothing on the port; both worked at once when developer options ran **Try
external USB DAC**. That is the whole of the evidence, and it is enough -- two
unrelated devices, one behaviour, one workaround that changes exactly two
writes.

The reason is the role rather than the code. `arbitrate_port()` leaves the
TCS1421 in `NormalDRP` while the port is empty, which is what lets a phone be
handed the card and is what the stock player does by default. A USB-C sound
card does not ask for the host role on the CC lines -- it simply waits to be
enumerated -- so with the port dual and nothing acting as source, the far end
is never supplied and never appears. Forcing `Source` on the TCS1421 and `host`
on the x1600 PHY supplies it, which is exactly what the trial does.

So the trial's writes are now available as a setting, not as a twenty-second
diagnosis:

- **Developer options → External USB DAC** switches on `[usb] host_dac` in
  `device_config.ini`. While it is on, `usbhostmode.c` asks
  `usbhostprobe_begin_held()` for the port: the same journal, the same
  Source/host writes, the same refusal when ADB, DAC mode, an export, a
  connected UDC or an unknown PHY is in the way -- with **no deadline and no
  unplug rule**. A dongle that re-enumerates, or is plugged in the next
  morning, finds the port still supplying it. Only the switch ends it.
- The row says which of those it is: *held*, *waiting for the controller*,
  *busy* (kept, retried every three seconds), or *unsupported* (this kernel
  exposes no role that may be forced -- not retried, since nothing about it can
  change while the player runs). "Switched on" and "a DAC is being driven" are
  different sentences, and the second only comes from an ALSA card.
- ADB, DAC mode, a storage export, suspend, shutdown, a firmware update and a
  factory reset release the port through `usbhostmode_release()`. ADB, DAC mode
  and shutdown keep the setting, so the port returns to holding a DAC when they
  are finished; a firmware update and a factory reset clear it, because neither
  the recovery they boot into nor the firmware after them would know it
  inherited a forced role. A failed release is reported and nothing else is
  allowed the controller.
- A reservation taken away by someone else (`usb_external_host_yielded()`) is
  how the held role learns that ADB or DAC mode needs the port: a held role has
  no second chance to notice, unlike the trial, which re-checks every second.
- `[power] standby_mem` no longer waits for it: with no cable on the port the
  role is given back on the way into the suspend and taken again after the
  wake, rather than keeping a player in a pocket awake all night or blocking the
  sleep behind a permanently "in use" controller.
- The twenty-second trial stays, one row below, and refuses while the setting
  holds the port.

The old note about the trial posed one condition that fits these two reports
badly: "connect **only** a confirmed bus-powered DAC; a charger or computer must
not be on the port". That stays -- `Source` is a promise to supply 5 V made
before anything is identified -- and it is why this remains a setting somebody
switches on rather than something the poll does by itself.

Not verified here: no board is in this environment. The reasoning is from the
V1 1.3 kernel in this repository (`sw_switch_hsotg` on the x1600 PHY, the
`tcs1421_add.ko` module and its four role names) and from the two reports. What
still needs a device: that the role survives a played track, a standby and a
reboot; that a DAC re-enumerates after a suspend with the setting on; and what
the port does when a charger is plugged in while it is held. `make -C
sonix-player/tests usbhost` now runs `test_usbhostmode` (hold, yield, retry and
release, with the config and the probe replaced) and the held-mode half of
`test_usbhostprobe` (no timeout, no unplug release), on top of the existing
suites. Neither models the electrical behaviour of the socket.

### On the V1: recognised at once, then silent after about ninety seconds

The first board report of the setting. Both dongles behaved the same way: the
ESR was found immediately and played for about a minute and a half, then the
sound stopped with the player still playing and the top bar still showing the
USB-C glyph; unplugging and replugging it brought it back for another minute and
a half; the JCally did the same. Swapping one dongle for the other took about
ten seconds to be noticed, and a few times a dongle was not found at all.

So the role acquisition works -- the part this change was for. What fails
afterwards is everything downstream of it, and the three things below are the
ones this repository can actually point at. There is no board here: each is
reasoned from the code, and the log line added last is what tells them apart on
the next run.

- **The held role is now checked, not just written.** `Source` on the TCS1421 is
  a request to the controller's own state machine, not a setting that stays: the
  driver re-runs its role decision on every attach and detach, and the role it
  started in is `NormalDRP`. Two bad reads a second apart -- nothing on the host
  bus *and* the attribute not reading `Source` -- write both controls again, no
  more often than every five seconds. A device enumerated on the bus is proof
  the far end is alive whatever the attribute says, and is never rewritten under.
- **A USB device is no longer "played" out of the jack.** `open_pcm_device()`
  falls back to `default` -- the CS43198 -- when the device it was asked for
  will not open, which is right for headphones that walked out of range and
  wrong for a dongle the player is itself the host of: the sound goes into a
  socket nobody is listening to while the interface keeps the USB-C glyph up,
  which is exactly the shape of the report. The fallback is now Bluetooth's
  alone. The USB side gets ten seconds of retries instead of two -- a dongle
  that has just re-enumerated needs seconds, not milliseconds, before its card
  is back -- and a track that still cannot open says so and stops rather than
  playing somewhere else.
- **The wake no longer rewrites the volume chain under a live USB stream.**
  `power_screen_on()` re-applies the volume for the CS43198's benefit after every
  blank. That call is the whole chain, and on the USB route it ends in the
  dongle's own control; the CS43198 is not in that path at all and a dongle on
  the bus was never blanked. It is skipped while the USB card is the output.
- **A second dongle is a second device.** Both land on the lowest free card
  number, usually card 1, so the number stayed put across a swap and the new
  device kept the old one's name and level. The poll now reads the bus name the
  card hangs off (`1-1`) and routes again when that has moved.
- **The health line says what the stream is doing.** Once per `HEALTH_PERIOD_MS`
  (10 s) per track it now carries the PCM's state and how much is still queued:
  `stream RUNNING with 4096 frames queued (92 ms)`. A RUNNING stream whose queue
  does not go down is a device being handed frames it is not consuming; a
  PREPARED one never started. That single clause is what separates a dead dongle
  from a stalled decoder on the next log.

`make -C sonix-player/tests usbhost` covers the first, the fourth and the
`read_card_bus()` half of the fourth: `test_usbhostprobe` drives the rewrite on
its slower beat and the "device on the bus is never rewritten" rule, and
`test_usbaudio_arbitration` resolves a card's device link both ways. The audio
fallback and the wake are compile-checked only -- there is no ALSA here and the
host simulator cannot be linked without it.

### On the V1: the ninety seconds, and a hiss under the music

The log from that report (`sonix_player-neu.log`) has both answers in it, and
neither is where the guesses above were pointing.

**The ninety seconds is Bluetooth being parked.** The sequence is in the log
five times, to the tenth of a second:

```
13:31:13.577 power: screen off after 30037 ms without input (timeout 30000 ms)
13:32:13.442 power: parking Bluetooth after 60115 ms unused
13:32:13.485 audio[N]: the output moves from 'plughw:3,0' to 'default'
13:32:14.092 volume: usb-c -> 3.5 mm, 47%
```

Thirty seconds to a dark screen, a minute more before the radio is parked, and
300 milliseconds after `park_bluetooth_now()` -> `bluetooth_set_enabled_transient(false)`
-> `bring_stack_down()` -> `apply_output_routing()` the output was set to
`default` -- the CS43198 -- because there was no A2DP sink. A dongle on the
USB-C port lost the route the moment a radio it had nothing to do with was
switched off. The player reopens the PCM on a route change and on the next
track, so the music moved to the jack while the USB-C glyph stayed up, and
`usbaudio_poll()` saw `active_card` still set and did nothing to correct it.
That is why it looked like a dead dongle rather than a re-route.

It is not a role problem at all, which is why re-writing `Source` and extending
the open budget last time did not help. `apply_output_routing()` now leaves a
`plughw:`/`hw:` route alone when it has no sink to point at: `usbaudio.c` owns
that route and clears it when the port empties. A bluealsa route is still
cleared, because that daemon is going away under it.

**The hiss is where the level is applied.** Both dongles are quiet but not
silent with no music, the louder one is worse, and neither a phone nor the
stock firmware does it. That is the signature of the player holding the device's
own mixer at 0 dB and turning the *samples* down instead: the music drops 21.5 dB
at the log's 57% while the dongle's output amplifier still runs at full gain, so
the music ends up that much closer to the amplifier's own noise floor. A phone
turns the dongle's control down; the stock firmware does too. Nothing is broken
in the dongle, and a quieter-output device hisses less for exactly this reason.

`[usb] hardware_volume` now defaults to **1**: the device's control takes the
nearest step at or above the target and the samples take the remainder, so the
level still moves in the player's own fine steps while the attenuation happens
where the noise floor is. On the log's `PCM Playback Volume` (0..50 over
-50.00..0.00 dB, so 1 dB a step) index 57% puts -21.0 dB on the control and
-0.5 dB on the samples, against 0 dB and -21.5 dB before: the same loudness at
the ear and 21 dB less hiss.

Setting `hardware_volume = 0` gets the old behaviour back, which is still the
right answer for a device whose own control is worse than its noise floor. The
guard rails stay: a scale that cannot be believed, a control that reads back
unchanged, or a write that fails all fall back to the samples carrying the whole
level, as before.

One bug fell out of testing the split. The "asked for something below the top
and it is still at the top" check used a dead band of a twentieth of the range,
which on a short control (0..10 over 10 dB) is two whole steps -- so a write that
landed exactly where it was told was judged to have done nothing and the control
was given up. A headset with a coarse control would have been pushed back onto
the samples and started hissing again. The dead band is now capped at a quarter
of the range and also requires the read-back to differ from what was asked for.

Tests: `test_usbaudio_volume` (new, in `make usbhost`) covers the split against
a fake mixer -- the device taking the level, a coarse control still moving
smoothly, a lying dB descriptor, a control that will not move, the old default
on request, no control at all, and mute. `test_bluetooth` gains the routing case:
a `plughw:` and an `hw:` route both survive a radio teardown, the jack is still
taken back, a bluealsa route is still cleared, and a sink being connected still
wins. Both were checked to fail against the old code before the fix.

## Bluetooth drop-outs with Wi-Fi joined, and a USB-C DAC that was never found

Two reports from one test session on a V1. Neither could be reproduced here --
there is no board in this environment -- and the log that goes with them was not
attached, so both changes below are reasoned from the code and the description,
and both need a listening / plug-in check on the device.

### Bluetooth stutters only while Wi-Fi is *associated*

The session: Wi-Fi joined and Bluetooth streaming, drop-outs; Wi-Fi on but not
joined, clean; Wi-Fi off, clean. The lock-screen blip in the same session is
issue #188, already handled by the held panel.

What the Bluetooth log of that session (14:56-15:08) shows: the player was the
**receiver** (an iPad streaming AAC 44.1 kHz to it, player is the piconet master),
and the loss is upstream of the player's decoded-audio path. The RTP warnings
alone do not distinguish RF loss from controller, UART, or host loss.
`bluealsa: Missing RTP packets` appears ten times in the two minutes with Wi-Fi
joined (14:59:27-15:01:28), and not once in the two clean stretches that follow
(15:01:45-15:03:06, 15:03:19-15:05:00). The events do not fall on a two-second
beat, so the status poll is not what drops them. The controller is a Broadcom
BCM4343A1 -- the same chip carries the Wi-Fi radio, with one antenna -- and it
reports that it supports Set AFH Host Channel Classification.

So the change that matters is `btafh.c`: while an A2DP link is up and Wi-Fi is
joined on a 2.4 GHz channel, the power tick (`bt_afh_follow_wifi()`) sends that
HCI command requesting that Bluetooth channels under the access point's channel
(centre +-12 MHz, `freq=` from `wpa_cli status`) be avoided. It is resent every
fifteen seconds while in force and cleared when the link or the Wi-Fi join goes.
A 5 GHz network or an unknown channel masks nothing. A successful write only
sends the request; check the controller's Command Complete in the Bluetooth log.
The piconet master controls the hop set, and controller support and role affect
whether the request is used. `make -C sonix-player/tests btafh` checks the map.

Alongside it, the smaller change from the first pass stays: with an A2DP link up
the Wi-Fi worker polls at the standby period whatever the screen is doing and does
not ask for `signal_poll` (the driver reading the link from the chip, only to move
the bars). It removes the player's own traffic from the link and cannot hurt;
`make -C sonix-player/tests wifi` checks it. The log does not show it to be the
cause.

Neither is verified on a board. Check: stream from the phone with Wi-Fi joined and
count `Missing RTP packets` in a Bluetooth log over a couple of minutes against the
same song with Wi-Fi off; the log also carries the `bluetooth: AFH:` line and the
controller's reply to the command.

### USB-C: a JCally did not enumerate on the TempoTec V1

The `0311-sonix_player-neu.log` report does **not** establish the earlier theory
that a three-second sink write cut off a slow JCally. At 23:52:47-48 it reports
`NormalDRP`, `otg_id -1`, VBUS present, and no device on the host bus. There is
no USB VID/PID, ALSA USB card, `snd-usb-audio` error or logged sink write for
that attempt. VBUS is also raised by a charger or computer; it identifies
neither the accessory nor the data role. The cause of the missing enumeration
and whether another DAC works on this exact build remain unproven.

The V1 1.3 kernel bundled in the repository was inspected, not the running
kernel on the reporting device. It exposes a PHY `sw_switch_hsotg` control with
`host`, `device` and `none` commands and uses `dwc2_new`; the old `jz-dwc2`
`otg_id` path is not present in that image. The live sysfs path, role behavior,
and electrical effect still need hardware confirmation. Related R1 drivers
are not evidence of what the V1 actually does.

- Automatic arbitration treats an unreadable ID as unknown, avoids a startup
  role change when VBUS/port use is ambiguous, protects an enumerated audio
  interface or card even when Bluetooth owns playback, and logs ID, TCS mode,
  VBUS, UDC state and enumerated host devices. It does **not** force host/source
  merely because VBUS is high. A detected USB sound card is not reset by
  reasserting dual mode.
- Developer options offers **Try external USB DAC**, an experimental, explicit
  opt-in. It requests TCS `Source` and the compatible x1600 PHY's `host` mode;
  this is a diagnostic attempt, **not a proven JCally fix**. Connect **only**
  a confirmed bus-powered DAC: unplug computers and chargers and turn ADB off.
  Never plug a computer or charger into the port while the trial is active. The
  action is refused for unsupported/ambiguous PHYs, an unknown or connected UDC,
  ADB, DAC mode, storage export or another owner. Only the player's own idle,
  disconnected storage gadget is parked and later rebound.
- If no ALSA USB card appears, the previous TCS setting and PHY automatic mode
  are restored after 20 seconds (45 seconds if an audio interface was seen).
  A detected card keeps the override until unplugged; the same menu action
  changes to **Stop USB DAC host** for manual cancellation. A failed restore
  retains the controller reservation and retries; no Sonix gadget is allowed to
  claim it meanwhile. A strictly validated journal on `/usr/data` records the
  previous setting before any override and is synced with its parent directory.
  Recovery runs before Sonix starts ADB, after player crashes or reboots; stock
  init scripts may nevertheless claim the port earlier, in which case recovery
  waits rather than disturbing them. Invalid journals or failed restoration
  fail closed; keep computers and chargers disconnected until the port is
  reported restored. Shutdown attempts restoration, and factory reset/firmware
  update are refused if the role remains unresolved. Recovery relies on the
  settings partition being mounted when Sonix starts; V1 mount order, filesystem
  durability and power-cycle behavior still need hardware verification.
- Storage export independently no longer sends a mounted or lazily-detached
  card to a PC. It requires a matching visible mount, a normal unmount, an
  exclusive block claim and no remaining raw-device fd (including a FUSE
  daemon). On failure it restores services or lets the card supervisor recover,
  and waits for a cable replug rather than repeatedly interrupting playback.
  A LUN must be verified empty before remounting. ADB keeps the card mounted;
  the old stale-ADB composite fallback is not used. External privileged programs
  could still bypass these process-local checks; actual gadget, card and FUSE
  behavior on V1 must be tested.
- Shutdown now checks for open card descriptors before reopening its suspended
  log. The earlier log listed the log fds only *after* reopening the log, so it
  did not prove the cause of the busy unmount. The warning that mentioned an
  MP2731 charger is now generic: V1 has a separate AXP2101 charging path.
- The log's `input: registered devices` section was blank. The shared file
  reader assumed everything supports seeking to a meaningful end, which fails
  for procfs and some configfs files. It now reads to EOF and treats read errors
  as errors, also making USB gadget-attribute reads more reliable.
- `geniusbuild: result 3, 4096 tracks` is the existing intentional Smart Mix
  input limit, not proof of out-of-memory. Supporting this 26k-track library
  requires a separate bounded large-library design; this patch does not change
  that limit. The logged Bluetooth underruns predate the separate coexistence
  changes and still need on-device verification.

Host tests: `make -C sonix-player/tests usbhost readfile`, using private mocked
sysfs, mount/claim and LUN fixtures, and real non-seekable procfs/pipe reads,
plus the regular regression suites. These do not model the V1's electrical
behavior, kernel driver timing, real card export or
USB enumeration. Test on hardware with a known-good USB-C OTG adapter and a new
full log including the `usbaudio:`, `usbhost:` and `kernel:` lines.

## Bluetooth reconnect after the adapter was left powered off

A bug-report log (`bluetooth-neu.log`, TempoTec V1) shows the controller being
reset and powered off about eight seconds after the stack comes up, with nothing
on the player's side asking for it (`New Settings: 0x0ad4`, Powered clear). The
reconnect to the last headphones then ran three times and was refused each time
with `org.bluez.Error.NotReady` -- this side, not the headphones -- and nothing
turned the adapter back on. The headphones were not the problem in that session.

`reconnect_last()` now checks `Powered` through `ensure_adapter_powered()` before
the grace period and before every attempt, and powers the adapter back on (waiting
up to five seconds for bluez to confirm) when it is off. The log says which of
the three happened: `the adapter is not powered; powering it back on`, then
`the adapter is powered again after N ms`, or `the adapter did not power on`.

The log does not identify the process which sent the power-off. Inspection of
the bundled V1 1.3 image now shows that the stock `bt_init` explicitly sets the
adapter Powered Off near the end of its background run, making it a plausible
source; the log alone does not prove it. The startup changes below prevent that
script racing Sonix in newly packed firmware. This does not touch the separate
"Host is down" / page-timeout failures to the headphones, which are the
headphones not answering.

Needs an on-device check: power the player on with the headphones on and off,
and confirm the reconnect either lands or logs one of the three lines above.

## Bluetooth receiver coexistence and startup ownership

The later V1 log (`0311-bluetooth-neu.log`) has 17 BlueALSA missing-RTP
warnings (23 packets in total) during received audio, and no Set AFH Host
Channel Classification command. It also shows two `bluetoothd` launches, one
refused the D-Bus name, followed by an adapter power-off and a `NotReady`
reconnect. The RTP warnings locate loss before local DAC output, but do not
identify whether RF, the controller, UART or host is responsible. The log alone
does not prove that Wi-Fi was joined or that AFH would cure the loss.

- `bluetooth_a2dp_link_active()` counts either a headphone PCM or an incoming
  receiver PCM, including paused links. It drives Wi-Fi polling, radio parking
  and the AFH follower. The existing `bluetooth_audio_active()` remains
  output-only, so a phone sending audio cannot redirect local playback to a
  nonexistent headphone. The cross-thread Wi-Fi streaming flag is atomic.
- The follower retries failed restrictions **and clears**, refreshes a sent
  restriction every 15 seconds, and reasserts or clears it after a radio park
  rather than assuming a reset. It logs requests, not claimed controller
  acceptance. Change-only diagnostics record whether Wi-Fi is associated and
  its frequency (not the SSID); repeated HCI errors are suppressed.
- Closing the D-Bus connection now clears both PCM directions, transport and
  media state after the reader has stopped, preventing a late signal or a
  previous receiver link from being resurrected after a stack restart.
- Startup waits for a running stock `bt_init` to finish, including when a shell
  invoked it, and will not start a second patchram while an existing one is
  still alive. A switch-off can cancel the waits. Boot-time teardown shares the
  Bluetooth worker with power jobs, so a quick switch-on cannot be torn down by
  a separate cleanup thread.
- Newly packed V1 firmware overlays `S80_bt_init` with a no-op. In the bundled
  V1 1.3 stock image that hook only launches `bt_init` on start and `bt_done` on
  stop; the separate S30 D-Bus init and the original binaries are preserved.
  The stock initializer also creates `/usr/data/alsa.conf`: Sonix now creates
  the same default when needed without replacing an existing template; the
  existing per-device rewrite is unchanged. Stock `bt_init`'s `/tmp/bt_init_ok`
  marker and legacy HiByLink SDP record are not
  created by the replacement; Sonix uses its own Bluetooth stack and SonixLink.
  Other stock revisions and any external consumers of those artifacts need an
  on-device check.

Host tests: `make -C sonix-player/tests bluetooth btafh wifi basebuild` and
address/undefined-behavior-sanitized versions of Bluetooth, AFH and Wi-Fi. The
actual V1 1.3 archive's init scripts were also checked against the tar overlay;
MIPS32r2 cross-compilation of the changed translation units succeeded with an
independent Clang-based toolchain. This is not a complete linked firmware build.

**Still needs hardware:** test cold boot with Bluetooth on and off, rapid toggles,
headphone and phone-receiver connections, reconnect after sleep, and Wi-Fi
joined versus off with the same source. In `btmon`, inspect Command Complete for
Set AFH Host Channel Classification and compare loss counts; even an accepted
command does not by itself prove a changed hop set or an audible improvement.

## A happier music pet

The dark ring on Pikachu's chest was the hip-hop chain's pendant, not a hole in
his sprite. The pendant is gone; the chain is lower and ends in gold links, so
it no longer interrupts the face or resembles a hole. The accessory now also
updates when the track's genre changes without leaving the page.

An awake pet listening to music now has an open, upturned smile and a pink
tongue instead of the surprised chewing face. A scarf or collar gets a compact
smile above the neckwear; sleep and shades still work as before. The two music
notes were created but never shown: they now appear behind the pet when music
is audible and disappear when it stops, drifting on the existing frame timer.

A tap or completed hold now gives a small hop and bobbing heart instead of a
text toast, including during the reward cooldown. The existing petting and
once-daily grooming rules are unchanged. The headless page test checks the
smile, pendant-free chain, playback transitions, and the visual petting
feedback on the V1 renderer; the appearance should also be checked on device.

## Built on the real stock firmware 1.3

The stock image the build used until now was committed as "1.3" but was not:
its `usr/resource/config.json` says product version 1.2, and its kernel is the
earlier build. The official V1 1.3 OTA is now in `sonix-packer/firmware-input`
as `v1.zip.part1` + `v1.zip.part2`, two byte-split pieces of one zip, and every
build starts from it. Hashes and preparation steps are in
[the firmware input README](sonix-packer/firmware-input/README.md).

What 1.3 changes, from a file-by-file comparison of the two stock images:

- **A compatibility marker.** `ota_v0/ota_compatibility.in`
  (`compatibility=tempotec_v1_x1600_auto`) sits beside `ota_update.in`. The
  earlier package has no such file. Nothing in the 1.3 rootfs or kernel reads
  it, so whatever checks it sits outside the update. This is probably the
  recovery/updater on units shipped with 1.3, but that cannot be inspected
  from the OTA. An image built on the 1.2 package lacks the marker, which is
  one plausible reason it did not work on 1.3.
- **The kernel.** `#120` of 13 October 2026. Compared with the 1.2 kernel, the
  only functional addition found is one more SPI NAND chip (Zbit
  `ZB35Q01CYIG`). A unit built with that chip needs the 1.3 kernel, the second
  plausible reason. The partition table comes from u-boot, not the kernel.
- **The rootfs.** Rebuilt drivers, busybox, `sys_server`, `dmrd` (QPlay 3) and
  the Bluetooth, USB and Wi-Fi scripts. The new script logic is gated on AIC8800
  radios and the HiBy R1 (`/run/aic_*_power_gate`, `/run/r1_wireless_type`),
  so it stays inactive on the Broadcom-based V1. Unchanged: the launcher
  (`hiby_player.sh`), `S92_03_start_music_player`, `S11jpeg_display_shell`
  (the boot-screen patch lands at the same line 16, report identical),
  `S80_bt_init`, the LED script, `etc/ota_bin` and the recovery scripts.
  `bluealsa`'s options and codecs are identical. Every `sys_server` command and
  ALSA control Sonix uses is still there.

The packer needed no change to install Sonix on 1.3. A local build on each
base differs only by the stock differences and the stamped
`base_build_version`. Changes made to build on the 1.3 base:

- `prepare_v1_stock.sh` joins split archives: `NAME.part1, NAME.part2, ...` and
  `NAME.001, NAME.002, ...`, in numeric order. A gap in the numbering is an
  error rather than a silently short archive, and a folder walk opens each set
  once. It prints the prepared `v1.upt`'s SHA-256.
- The packer keeps every file of the stock package except the rootfs: the
  kernel chunks and their chain, `ota_config.in`, `ota_v0.ok` and the
  compatibility marker. It says which marker it carries over and checks the
  marker is unchanged before writing the image. In `ota_update.in` only the
  rootfs entry's size and MD5 are rewritten in place. The stock file's layout,
  including the empty line it ends on (missing from the previous retyped
  heredoc), is kept as is.
- `verify_upt.sh` reports the OTA config, the compatibility marker and the
  TempoTec product version. `--stock STOCK.upt` requires every package file
  outside the rootfs to be byte-identical to the stock update's, and
  `ota_update.in` to differ only in the rootfs size and MD5. The workflow runs
  it on every image. The build report names the stock version, its SHA-256
  and the marker, and says whether the image is a CI build or a release.
- Bluetooth finds the radio's rfkill switch by name, as 1.3's `bt_init` does
  (`get_rfkill_index "bluetooth" "0"`), instead of always writing `rfkill0`. The
  lowest-numbered switch whose name contains "bluetooth" wins, and `rfkill0`
  is the fallback, so a V1 whose switch is `rfkill0` behaves exactly as before.

Host tests: `make -C sonix-player/tests bluetooth basebuild stock-prepare`.
Locally, the 1.3 OTA was prepared from its pieces, verified (kernel SHA-256
`a598b189…f6230`, product version 1.3, marker present), packed with a stand-in
player binary, and the result verified with `--sonix-v1 --stock`.

**Confirmed on hardware:** the image built on 1.3 was flashed on a V1 running
stock firmware 1.3 and works, so images built on the 1.3 OTA are no longer
test builds. This is the base of release 1.5.0. Keep the official 1.3 `v1.upt`
and its checksum for recovery all the same.

## A choice of corners, and the original play button

Two small additions to Settings. Neither changes anything until it is chosen.

**How round the screen corners are.** *Rounded corners* in Settings → Appearance
had one arc, the 16 px of `bp_screen_corner_radius()`, sized to the 2" panel's
glass. The V1's shell reads squarer than that in the hand, so the card now has
a slider under the switch while the corners are on: 2 to 20 px in steps of 2
(`CORNERS_RADIUS_MIN`, `_MAX` and `_STEP` in `corners.h`), with the radius as
`N px` under the name. It is the toggle-and-slider card the timed options
already use (`settingsrow_toggle_slider()`), and it stands at 16 px until
something else is chosen, so anybody who already had the corners on sees
exactly the corners they had. The top stop stays under the 24 px status bar: an
arc taller than the bar would start to cut into its clock and battery.

The radius is kept in the config alone (`ui/corner_radius`) and asked for each
time it is wanted, so there is no second copy to fall out of step with it. A
number written by hand is pulled into range and a key that was never written
gives the board's arc. Moving the slider draws the four A8 masks again at the
new size in the buffers they already have. The image descriptors are therefore
the same pointers LVGL already holds, so each is dropped from the image cache
(`lv_image_cache_drop()`, which `lvgl.h` no longer pulls in) and each corner
has its source set again: that is what makes the object take the new size, and
repaint the old area, which is the larger one when the arc has shrunk. While the
corners are off the new radius is only remembered; the masks are redrawn when
they are next turned on.
`make -C sonix-player/tests corners` draws every stop into a framebuffer driven
the way the panel is (only what was invalidated is redrawn) and holds each
update to a full redraw, and `tests/ui/test_main_menu.c` drives the slider
itself.

**The original play button.** The player this port started from drew play and
pause as a glyph on a filled disc, as big as the button. This fork had taken the
disc away and left the bare glyph in the accent, like previous and next beside
it. **Settings → Music → Display options → Legacy play button**
(`screen/player_legacy_play`, off by default) puts the disc back.

The drawing is `playbutton_paint()` in `src/gui/nowplaying/playbutton.c`, split
out of `player.c` -- which is far too large to build without a device -- so that
it can be drawn and measured on its own. `player.c`'s `paint_play_button()` calls
it from every path that can move one of its colours: the glyph changing, a new
sleeve, a new palette, the cover tint and the switch itself. Plain is exactly
what it was: no body, the glyph at its own size, the accent. Legacy makes the
button a circle (`LV_RADIUS_CIRCLE`) and scales the glyph to 55 % of the
button's width, never above the size it was drawn at: the bitmaps are drawn for
a button with nothing behind them, and at 40 px in a 44 px disc the triangle's
corners would touch the rim. Pressing darkens the disc and the glyph through
LVGL's own button theme; nothing of the button's own is added to that.

The disc is the original's: white over artwork and in the dark palettes, and the
palette's pressed surface in the light one when there is no sleeve behind it to
stand out against. Poster's deck stands on its own ground and not on the sleeve,
so there it counts as no sleeve. The glyph stays the accent for as long as it
can be told from the disc, and that is a measured rule, not a list. Contrast is
worked out on relative luminance with the gamma taken as 2 (it needs no libm,
and chooses between two colours about as well as sRGB's own curve does, a little
pessimistically):

- the accent keeps the original look while it holds **2.5:1** on the disc. The
  original shipped the default blue on the light theme's grey disc at 2.8, so a
  floor of 3 would have changed the one case everybody has, and made the button
  look different with and without a sleeve;
- below that -- yellow, lime, green and cyan accents, Tokyo Night's violet,
  OLED's white on a white disc -- the disc takes the accent, and the glyph is
  white on it while that holds **3:1** (blue, pink, brown, Amber's colour) and
  black where it does not, which on such a disc is 7:1 or better. That is not
  what `theme_on_accent()` gives labels on accent fills (white on all but the
  palest): a label can lean on its words, this glyph is all that tells play from
  pause;
- Amber on Black and OLED, whose point is one colour, always fill the disc with
  it: a white disc in the middle of the page would be a second.

Measured with the exact sRGB curve, the worst glyph/disc pair over every accent
preset in every palette is 2.65:1, and over 600,000 random accents the worst is
2.42:1 (a hot magenta on the light grey disc); once the disc has become the
accent, it is never under 3:1.
`make -C sonix-player/tests playbutton` runs the real function on a real button
and reads pixels back: the disc round and nothing past its rim, the glyph
centred and clear of it, plain and legacy alike, the press, switching back and
forth with no disc left behind, every palette and accent held to the contrast
above, and the two new texts in all eleven language files.
`SONIX_PLAYBUTTON_SNAPSHOTS=<prefix>` also writes a contact sheet of the looks.

**Readings to check.** "Legacy" here means the filled circle behind the glyph
that the original player drew, and the corner choice rounds the screen's own
corners, not the cards and tiles inside it. Both are written down so that a
different reading can be asked for. Neither has been seen on the glass.
## Multi-file CUE sheets (issue #229)

A sheet with `FILE "01. Song.flac" WAVE / TRACK 01 / INDEX 01 00:00:00`
followed by a second `FILE` is valid. Sonix formerly stopped parsing at the
second `FILE` and displayed `01` when that first track lacked its own `TITLE`.
It now resolves each track's own audio file, resets the playback window at file
boundaries, and uses a one-track-per-file filename (minus a matching numeric
prefix and extension) as the title when no track title is supplied. Explicit
track titles take precedence; multi-track single-file images keep their previous
number fallback. The scanner, folder browser, queue, remote browse, format
detection and artwork lookup use the corresponding file and suppress duplicate
raw audio rows. `make -C sonix-player/tests tags` covers a multi-file UTF-8
fixture, metadata lookup, format detection and the single-file boundary case.
An on-device rescan and playback check is still needed.

## Remember the folder browser

The file browser remembers its last visited folder and scroll position across a
restart, independently of the playing queue. On first use, with no usable saved
folder, it tries the remembered local track's folder when Remember track is on,
then Music, then the card root. CUE virtual paths are resolved to their sheet's
folder. The home Browse tile and the Music page share the same position.

A separate atomic state file beside device_config.ini holds a full-length path,
including names the INI format cannot represent. Saved paths are checked against
the mounted card, including symlinks; a missing card or deleted folder cannot
send the browser outside the card or overwrite the saved position at boot. Scroll
writes are debounced and flushed when leaving the page or shutting down. Folder
reads remain asynchronous, and the saved offset is clamped when a listing has
shrunk. `make -C sonix-player/tests browserstate` checks persistence and path
fallbacks. The reboot and scroll behavior still needs checking on a V1.

## Bluetooth RX: PCM width, stalled capture and AVRCP state

A Lenovo Legion Tab was reported to play LDAC, AAC, SBC and aptX in RX mode,
while aptX HD was badly corrupted. The old receiver opened every input except
32-bit as S16, but allocated and passed 24-bit data to the DAC when BlueALSA
reported 24 bits. The client now asks the ALSA BlueALSA plug for left-aligned
S32 for either 24-bit BlueALSA representation and passes four-byte samples to
the external audio path; 16-bit remains widened as before. This needs listening
on the V1: the host test stubs the plug, not the firmware's ALSA conversion.

The capture handle is nonblocking, with one bounded poll and a sleep for
spurious readiness instead of a potentially unbounded ALSA wait at FIFO
priority. Stop and rapid re-entry serialize ownership on the worker rather than
on the interface. These changes address plausible RX freeze/recovery paths, but
are not a diagnosis of the reported long-session lockup or of a Galaxy Fold 8
connection dropping after roughly 30 seconds. A Mac M4 was also reported to
work with AAC but not SBC; the exact cause of that failure is not yet known.

The receiver now uses request tickets for fresh PCM information, coalesces
queued reads, and keeps metadata revisions separate from stream-format replies.
AVRCP metadata is tied to the current source, cleared on invalidation and
protected against an older in-flight D-Bus reply. The RX page compares each
change counter and flag separately, so two changes cannot cancel in a sum.
`make -C sonix-player/tests bluetooth` exercises the actual capture session with
fake ALSA, queue/ticket logic, and D-Bus metadata signals. It cannot substitute
for a V1, BlueALSA and the reported senders. A Bluetooth log taken immediately
after each separate failure is needed to distinguish link-level disconnects,
codec negotiation and RX-worker failures.
