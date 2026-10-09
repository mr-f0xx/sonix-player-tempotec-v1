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
// lives in seven bytes of raw NAND, in a block of /dev/mtd5 that belongs to
// nothing else, and the stock player writes it with the two commands its own
// binary carries as strings:
//
//     flash_erase -q /dev/mtd5 0x20000 1
//     printf %-256s | nandwrite -q -s 0x20000 -p /dev/mtd5 -
//
// logo1 is the pale one and logo2 the dark one, which is why light theme
// writes 1 and dark writes 2.
//
// The third file is what makes a second boot screen possible without touching
// that script at all: the firmware ships the Retrospace picture as
// /etc/logo.jpeg, in place of the stock neutral one, so the branch the script
// falls through when the marker is missing -- or is one it does not know,
// which is what theme:3 is -- shows it. Three files, three outcomes: stock
// light, stock dark, Retrospace. A device that still carries the stock
// script and an older firmware simply draws its own logo.jpeg for theme:3,
// which is the graceful end of the same idea.
//
// Two things this module is careful about, because raw flash is not a file:
// it reads the marker first and does nothing when it already agrees (NAND has
// a finite number of erase cycles and a theme switch is one tap), and it does
// the writing on a thread of its own (flash_erase takes long enough to be seen
// as a freeze).

// The two boot screens Appearance > Boot screen offers. Retrospace is the
// default, and it is also what a device with no marker at all draws, because
// the script's fallback file is the one that carries it.
#define BOOTLOGO_STOCK      0
#define BOOTLOGO_RETROSPACE 1

// Which marker is on the flash now: 1, 2 or 3, or 0 for none/unreadable.
int bootlogo_current(void);

// Points the boot logo at the theme and at the chosen boot screen: the stock
// pair follows the palette, Retrospace is one picture either way. Returns
// immediately; the flash work happens on a worker. A no-op when the marker
// already agrees, when the device has no /dev/mtd5, or on the host build.
void bootlogo_set(int choice, bool dark);

#endif /* BOOTLOGO_H */
