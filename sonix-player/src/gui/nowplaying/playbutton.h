#ifndef PLAYBUTTON_H
#define PLAYBUTTON_H

#include <stdbool.h>

#include "lvgl/lvgl.h"

// How the now-playing page draws its play/pause button, in either of two looks.
//
// Plain is the one this port has always had: the glyph alone, in the accent
// colour, on a button with no body -- the same as previous and next beside it.
//
// Legacy is the one the original player drew: the same glyph on a filled disc
// as big as the button, with the glyph scaled down to a little over half of it
// (the bitmaps are drawn for a button with nothing behind them, and at their own
// size the corners of the triangle would touch the rim). It is the Music >
// Display options switch of that name, off unless it is turned on.
//
// Kept apart from player.c, which is far too big to build without a device,
// so that the disc can be drawn and measured on its own (see test_playbutton.c).
// The caller owns the button, the image inside it and what the image shows --
// play, pause or stop -- and calls this again whenever a colour it passes in
// can have moved.
//
//   button     the lv_btn, square, its side set in pixels
//   glyph      the lv_image inside it, centred
//   legacy     which look
//   accent     the colour the plain look draws the glyph in
//   white_disc the disc the original player used: white while the page stands
//              over artwork or in a dark palette, the palette's pressed surface
//              otherwise
//   album_tinted  the accent comes from the album: keep the disc white and
//                 use the album colour on the glyph, even for pale colours
//
// When the accent is not from the album, the legacy glyph keeps the accent for as long as it can be told apart from
// the disc: 2.5:1, which holds the default blue on the light palette's grey disc
// (2.8, as the original shipped it). Where it cannot -- OLED's white on white,
// Tokyo Night's pale violet, a yellow, lime or green accent on white -- the disc
// takes the accent instead, and the glyph is white on it while that holds 3:1
// and black where it does not (7:1 or better). That is not the white
// theme_on_accent() gives all but the palest accents: a label on an accent
// button can lean on its words, this glyph is all that tells play from pause.
// Without an album tint, Amber on Black and OLED always fill the disc with
// their colour: they draw everything in one hue by default.
//
// A finger on the button darkens it -- the disc and the glyph alike -- through
// LVGL's own button theme, as it does the plain look's glyph, so there is no
// pressed style here.
void playbutton_paint(lv_obj_t *button, lv_obj_t *glyph, bool legacy, lv_color_t accent, bool white_disc,
					  bool album_tinted);

#endif
