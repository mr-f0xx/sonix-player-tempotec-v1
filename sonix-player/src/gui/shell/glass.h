#ifndef GLASS_H
#define GLASS_H

// The Liquid Glass backdrop.
//
// Liquid Glass draws the interface on a clear pane: the pages add the shared
// screen style at zero opacity, so what shows behind every card, bar and list
// is the bottom layer. This module owns that layer -- a dark ambient ground
// and, over it, the supplied abstract-glass wallpaper, dimmed so the frosted
// panels read as glass and white labels stay legible.
//
// The backdrop is CONSTANT: it never follows the playing track's artwork, so
// the main menu (and every other page) sits on the same wallpaper whatever is
// listening. The Now Playing deck keeps its own artwork treatment separately.
//
// Call glass_init() once from gui_init(), after theme_init(). It registers a
// theme-refresh callback (the backdrop follows the palette). Outside Liquid
// Glass the backdrop is hidden and costs nothing.
void glass_init(void);

#endif // GLASS_H
