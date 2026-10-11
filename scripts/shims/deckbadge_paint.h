/*
 * deckbadge_paint.h -- the green ring on a deck's badge, while that deck is playing.
 *
 * The other half of deckbadge_zone.h's ask: the rectangle around `DECK ( 1 )` is a play/stop
 * button, and this is the frame that says so -- the same accent and the same lit thickness the
 * side panel's PLAY wears (side_paint.c's sp_frame_w with an accent class and SP_FRAME_LIT), so
 * the two surfaces read the same way.
 *
 * IT DRAWS ON THE PAGE, and that is why there is no "take it down" call. rbp repaints its own
 * screen at its own rate and the shim redraws this at ~1 ms, so page-drawn content is visible
 * only for as long as the painter keeps drawing it: stop calling this and rbp's next frame
 * erases the ring by itself. The whole contract is therefore `playing == 0` means nothing is
 * drawn -- there is no state, no saved pixels and nothing to restore.
 *
 * PERFORMANCE SCREEN ONLY, which is the caller's business and not this file's: the badge is drawn
 * by rbp on that screen, and a ring left over any other screen would be a green box round
 * something else entirely.
 */
#ifndef RBPI4B_DECKBADGE_PAINT_H
#define RBPI4B_DECKBADGE_PAINT_H

#include "menu_paint.h"     /* struct menu_view, menu_pixel() */

/* Draw a two-pixel ring just inside the inclusive rect (x0,y0)-(x1,y1), in `pv`, into `v`.
 *
 * ONE IMPLEMENTATION, TWO CALLERS, and that is the point: the plane route (deckbadge_plane.c,
 * which is what actually reaches the glass) draws the ring into its own buffer, and the host test
 * draws it into a buffer of its own -- so the arithmetic that puts the ring on the box is pinned
 * on a host rather than only on the unit. A page-route painter used to sit here; it was removed
 * with the route, because a paint into rbp's buffer is erased by rbp's own present (that is what
 * drmband.h's header says the band had to stop doing). */
void deckbadge_ring(const struct menu_view *v, int x0, int y0, int x1, int y1, unsigned int pv);

#endif /* RBPI4B_DECKBADGE_PAINT_H */
