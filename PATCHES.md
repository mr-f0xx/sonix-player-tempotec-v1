# Firmware patches

| patch | script | what it fixes |
|---|---|---|
| Touchscreen multitouch | `tools/gt9xx_multitouch_patch.py` | the panel reports five fingers, the driver lets one out |
| Touchscreen multitouch, R1 | `tools/cst8xx_multitouch_patch.py` | the panel reports two fingers, the driver lets one out |


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
in Settings → Appearance, use **16 px** on this board (`bp_screen_corner_radius()`).
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
at the deck's own rectangle (240 wide by `compact_deck_height()` tall) and cut
out of the very crop the screen-sized picture was made from -- `make_backdrop()`
takes the band from the foot of that crop rather than from a crop of the band's
own aspect, because a blur whose colours come from pixels that are not on the
screen meets the photograph above it as a seam of the wrong colour. The band is
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
  one free direction it has — the extra button rows are stretched out by a drag
  in the classic arrangement, where downwards is free, and by a tap on the handle
  under the brightness slider in the stock one, where downwards is the way out;
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

## The charger, with the player off

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
