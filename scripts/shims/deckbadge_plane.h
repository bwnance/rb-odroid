/*
 * deckbadge_plane.h -- the deck badges' play ring, on a DRM overlay plane like the band's.
 *
 * WHY A PLANE AND NOT A PAINT INTO THE PAGE. This is the third attempt, and the first two failed
 * for the reason drmband.h states for the band: rbp presents its own buffer, so anything written
 * into it is either erased by the next present or flickers against it. The band was "repaired,
 * not presented" until it got a plane; the drawers have planes for the same reason (drmband.c:
 * "an RGB565 plane is OPAQUE, so two drawers composited..."); and a ring drawn into the page
 * never appeared at all, because the buffer menu_draw.c's view points at is not the one that is
 * scanned out. A plane scanning out of its own buffer is untouched by any of that.
 *
 * TWO PLANES, one per deck, 95 x 50 each: deckbadge_zone.h's measured box and nothing else. Per
 * deck rather than one wide plane because the two rings come and go independently -- deck 2 can
 * start while deck 1 is stopped -- and a hidden plane is exactly "draw nothing", which lets rbp's
 * own badge show through untouched.
 *
 * OPAQUE, SO THE BADGE IS COPIED IN. An RGB565 plane composites over what is beneath it, and the
 * ring is a box outline with a see-through middle -- so the plane carries a copy of the 95x50 rbp
 * already has there, taken from the running screen when the ring goes up, with the ring drawn over
 * it. Taken once per transition and not per tick: a shown plane is not touched by rbp, so it stays
 * as it was put up until this module takes it down.
 */
#ifndef RBPI4B_DECKBADGE_PLANE_H
#define RBPI4B_DECKBADGE_PLANE_H

/* Put each deck's ring up or take it down, to match `playing[deck]` and being on `on_screen`.
 * Idempotent: calling it with the state it is already in does nothing, which is what lets the
 * paint thread call it every tick and pay nothing.
 *
 * The first call in a process sets the planes up, and a failure there is logged and then
 * remembered as "no ring on this unit" -- a diagnostic on a working player must never be the
 * reason it stops. */
void deckbadge_plane_sync(const int *playing, int on_screen);

/* The ring on, or all of it off. Called from the teardown path; safe at any time. */
void deckbadge_plane_teardown(void);

#endif /* RBPI4B_DECKBADGE_PLANE_H */
