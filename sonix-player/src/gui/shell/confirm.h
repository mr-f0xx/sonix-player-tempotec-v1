#ifndef CONFIRM_H
#define CONFIRM_H

#include "src/gui/shell/gui.h"

#include "lvgl/lvgl.h"

// A modal yes/no card over a dimmed page: the shape every "are you sure?"
// in the interface uses. One exists at a time; asking again replaces it.
//
// The action runs after the card has closed, so it can switch screens or
// start a long job without pulling the dialog out from under itself.

void confirm_init(gui_config_t *cfg);

// `message` may be NULL for a title-only question. `ok_label` is the accent
// button's text ("Scan", "Delete", ...); NULL uses "OK".
void confirm_show(const char *title, const char *message, const char *ok_label, void (*on_ok)(void *user),
				  void *user);

// Extended variant: both buttons have a callback. `cancel_label` may be NULL to
// keep the default \"Cancel\" text; `on_cancel` is called when the grey button
// is pressed. Either callback may be NULL. The card is hidden before either runs.
void confirm_show_with_cancel(const char *title, const char *message, const char *ok_label,
							  void (*on_ok)(void *user), void *user, const char *cancel_label,
							  void (*on_cancel)(void *user), void *cancel_user);

// The same card with one button: something the user has to be told, not asked.
// Dismissing it does nothing beyond closing it.
void confirm_notice(const char *title, const char *message);

void confirm_close(void);

// Whether the card is up. A page with a back guard of its own asks before it
// takes the chevron: a question on top of the page is the first thing to put
// away, not the page itself.
bool confirm_visible(void);

#endif /* CONFIRM_H */
