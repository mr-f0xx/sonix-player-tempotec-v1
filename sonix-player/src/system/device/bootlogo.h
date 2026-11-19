#ifndef BOOTLOGO_H
#define BOOTLOGO_H

#include <stdbool.h>

// The picture shown while the player boots, and how it follows the theme.
//
// It is not the player that draws it: by the time the player is running the
// logo has long gone. It is `/etc/init.d/S11jpeg_display_shell`, which runs
// before anything else and picks one of three files:
//
//     theme=$(nanddump -q -s 0x20000 -l 7 /dev/mtd5 -a)
//     if   [ "$theme" == "theme:1" ]; then cmd_jpeg_display /etc/logo1.jpeg
//     elif [ "$theme" == "theme:2" ]; then cmd_jpeg_display /etc/logo2.jpeg
//     else                                 cmd_jpeg_display /etc/logo.jpeg
//
// So the setting does not live in a file at all -- a rootfs mounted read-only
// could not hold it, and /usr/data is not mounted yet when the script runs. It
// lives in raw NAND, in a block of /dev/mtd5 that belongs to nothing else, and
// the stock player writes it with the two commands its own binary carries as
// strings:
//
//     flash_erase -q /dev/mtd5 0x20000 1
//     printf %-256s | nandwrite -q -s 0x20000 -p /dev/mtd5 -
//
// logo1 is the pale one and logo2 the dark one, which is why light theme
// writes 1 and dark writes 2.
//
// Five boot screens do not fit in three branches, so the marker carries two
// fields and the firmware packer puts a small block of our own into that
// script, just before its first cmd_jpeg_display:
//
//     boot_sel=$(nanddump -q -s 0x20008 -l 6 /dev/mtd5 -a 2>/dev/null)
//     if [ "$boot_sel" = "logo:2" ] && [ -f /etc/logo_space.jpeg ]; then ...
//     if [ "$boot_sel" = "logo:3" ] && [ -f /etc/logo_travelling.jpeg ]; then ...
//     if [ "$boot_sel" = "logo:4" ]; then ... /usr/data/bootlogo_custom.jpeg ...
//
// reading the second field at byte 8 of the same block. The marker this player
// writes is therefore "theme:N logo:M light:P", twenty-four bytes: the stock
// pair follows the theme in the first field as it always did, Retrospace is
// theme:3 with logo:1 -- the stock script's fallback file is the one that
// carries it -- and Space, Travelling in space and Custom are logo:2, logo:3
// and logo:4, drawn by the added block before the stock chain is ever reached.
// The third field is the panel brightness for the splash, in percent: the
// stock script raises the backlight to 60 after it draws, so the added block
// writes this level on the way out of the script instead, after that write.
// A device whose script has no added block, an older firmware included, draws
// logo.jpeg at the stock level for the three extra pictures: the graceful end
// of the same idea. A marker with no third field (written by older firmware)
// leaves the stock level alone.
//
// The custom branch is the only one that waits. Its picture is written to the
// UBIFS partition, which is mounted a line or two later in the boot than the
// script itself runs, so the block waits a few seconds for the file to appear
// and draws the stock picture if it never does: a boot that takes a moment
// longer to show a picture beats a boot with no picture in it.
//
// Two things this module is careful about, because raw flash is not a file:
// it reads the marker first and does nothing when it already agrees (NAND has
// a finite number of erase cycles and a theme switch is one tap), and it does
// the writing on a thread of its own (flash_erase takes long enough to be seen
// as a freeze).

// The boot screens Appearance > Boot screen offers. Retrospace is the default,
// and it is also what a device with no marker at all draws, because the stock
// script's fallback file is the one that carries it. Custom is the one picture
// none of them is: a picture of the user's own, copied off the card into the
// writable partition by bootpic.h, at a fixed path the choice block below
// names for itself.
#define BOOTLOGO_STOCK      0
#define BOOTLOGO_RETROSPACE 1
#define BOOTLOGO_SPACE      2
#define BOOTLOGO_TRAVELLING 3
#define BOOTLOGO_CUSTOM     4

// The splash brightness offered with the boot screen under Appearance, in
// percent of the panel's range. 60 is what the stock V1 script writes when it
// has drawn its picture, so that is the level an existing player keeps. The
// choices run from 5 to 100 in five-percent steps: a splash is only a picture,
// not something the user has to read, so the dim end goes well below the
// panel's working floor. The choice lives in the config, but reaches the boot
// script through the marker below -- nothing on the card is mounted then.
#define BOOTLOGO_LIGHT_MIN     5
#define BOOTLOGO_LIGHT_MAX     100
#define BOOTLOGO_LIGHT_STEP    5
#define BOOTLOGO_LIGHT_DEFAULT 60

// Points the boot logo at the theme and at the chosen boot screen: the stock
// pair follows the palette, the other three are one picture either way.
// Returns immediately; the flash work happens on a worker. A no-op when the
// marker already agrees, when the device has no /dev/mtd5, or on the host
// build.
void bootlogo_set(int choice, bool dark);

// What the boot script left in /tmp this boot, and what the marker writer
// made of the flash: two machine-readable lines for the Developer options
// page, or NULL when there is nothing to show yet.
const char *bootlogo_trace(void);
const char *bootlogo_status(void);

#endif /* BOOTLOGO_H */
