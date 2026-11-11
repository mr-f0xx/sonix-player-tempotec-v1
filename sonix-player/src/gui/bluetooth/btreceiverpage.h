#ifndef BTRECEIVERPAGE_H
#define BTRECEIVERPAGE_H

#include "src/gui/shell/gui.h"

#include "lvgl/lvgl.h"

// Bluetooth receiver mode: the page that owns the device while a phone or a
// computer is playing through it.
//
// The same trap as the DAC page, for the same reason. While it is receiving,
// the music is not this player's: the transport keys are sent over AVRCP to
// the machine the music is on, the now playing screen is not reachable because
// it is about a track that is not there, and leaving asks first -- walking out
// of this page silences the room.
//
// The control centre is reachable, and shows what is being received in place of
// the local transport. It is there for the sound: volume and brightness, the
// equaliser on and off, and the equaliser's own pages -- the receiver's audio
// goes through the same chain as a track -- which it opens without ending the
// mode (btreceiversession.h). What would end it asks first.
extern lv_obj_t *btreceiverpage_screen;

void btreceiverpage_init(gui_config_t *cfg);

#endif /* BTRECEIVERPAGE_H */
