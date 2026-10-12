/*
 * deckmark.h -- the playing-deck marker, drawn into rbp's OWN frame.
 *
 * WHY THIS EXISTS. The badge ring is composited by a DRM overlay plane, and on this unit a plane
 * can be reported by the kernel as bound and enabled and never reach the screen (see
 * drmband.h's DRM_BAND_SKIP_OVERLAYS). The sibling project full-touch-ui-xdj-rx3 avoids that class
 * of problem entirely by drawing into the buffer the player is about to present -- "no overlay
 * plane, no flicker, rbp file untouched". Its hook needs rbp-internal addresses this tree does not
 * have, but WE DO NOT NEED THEM: fb_shim.c already intercepts FBIO_WAITFORVSYNC, which is rbp's real
 * frame boundary (57.1/s), and calls into the shim at the ENTRY of the wait -- the code there
 * records why that is the right side of the frame ("the entry of the wait comes immediately after a
 * completed draw"). And because DFB_PRESENT=off, the layer surface IS fb0: rbp draws straight into
 * the page this module draws into, with no back/front buffer between them.
 *
 * So: one call per frame, a few hundred bytes of memcpy, no plane, no vtable, no rbp address.
 *
 * WHAT IT DRAWS is deckmark_zone.c's bar, for any deck whose PLAY_SOLID bit is set, and only on the
 * performance screen -- the same gate the ring uses, because a marker left over another screen
 * would be a green bar across whatever else rbp draws at those rows.
 */
#ifndef RBPI4B_DECKMARK_H
#define RBPI4B_DECKMARK_H

/* THE TICK THREAD'S ENTRY POINT -- menu_draw.c's paint loop, which already runs at POINT_MENU_MS.
 * This is the one that OWNS THE PLANE: it sets up, shows and hides the bar's overlay plane, which
 * is the route with no ceiling (the hardware composites it; nothing writes over it; there is
 * nothing to repair). A deck whose plane cannot be had falls back to the page route here.
 *
 * NOT CALLABLE FROM rbp's render thread: it makes DRM ioctls. That is what deckmark_seam() is for. */
void deckmark_tick(void);

/* THE RENDER THREAD'S ENTRY POINT -- fb_shim.c's FBIO_WAITFORVSYNC dispatch. It does only the page
 * route's cheap part (a few pixel reads and, if rbp's sweep took the bar, a memcpy), and nothing at
 * all for a deck the plane is already carrying. Safe before anything has been decided: the first
 * call anywhere is where the page is mapped, once. */
void deckmark_seam(void);

/* Forget the bars without drawing: used by the shim's teardown so a test or a restart cannot leave
 * a stale "on" state behind. Does NOT restore the pixels -- callers that need the page clean use
 * deckmark_seam() with nothing playing. */
void deckmark_reset(void);

#endif /* RBPI4B_DECKMARK_H */
