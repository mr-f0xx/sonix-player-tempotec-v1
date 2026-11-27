#ifndef GLASS_H
#define GLASS_H

// The Liquid Glass backdrop.
//
// Liquid Glass draws the interface on a clear pane: the pages add the shared
// screen style at zero opacity, so what shows behind every card, bar and list
// is the bottom layer. This module owns that layer -- a dark ambient ground,
// and over it a blurred, dimmed copy of the playing track's artwork, the same
// treatment the Now Playing deck already gives its controls (cover_blur_fill).
// Menus and lists therefore carry the sleeve softly behind them, exactly as
// the player does.
//
// The blur is made off the UI thread from a copy of the decoded sleeve, the
// way the lyrics background is (see player.c), and installed through gui_post
// with a generation guard so a track change mid-blur never lands a stale
// picture. While no track has artwork the ambient ground shows on its own.
//
// Call glass_init() once from gui_init(), after theme_init(). It registers a
// theme-refresh callback (the backdrop follows the palette) and an artwork
// observer on the player (the backdrop follows the sleeve). Outside Liquid
// Glass the backdrop is hidden and costs nothing.
void glass_init(void);

#endif // GLASS_H
