/*
 * deckbadge_zone.h -- the deck's badge box on the performance screen, as a play/stop button.
 *
 * The operator, 2026-10-10: the rectangle enclosing `DECK ( 1 )` beside the track title should
 * also function as a play/stop button, "with a green border matching the behavior in the side
 * panels". This module owns the RECTANGLE only; pointsrc.c owns the keycode, and the frame's
 * colour is the drawer's (side_paint.c's lit PLAY green).
 *
 * THE TWO RECTANGLES ARE A MEASUREMENT, re-derived from a live /dev/fb0 capture on 2026-10-10.
 * A BRIGHTNESS THRESHOLD CANNOT FIND THEM: the whole deck strip is one grey panel, so what
 * separates the badge box is a SHADE change -- the box is luminance 24 and the title area beside
 * it is 32. Scanning rows through it for that edge gives x = 106 on deck 1 and x = 746 on deck 2,
 * and scanning columns gives y = 584 above and y = 634 below:
 *
 *     deck 1   x  11..105   y 584..633      95 x 50
 *     deck 2   x 651..745   y 584..633      95 x 50
 *
 * DECK 2 IS DECK 1 + 640, exactly, and that is the check that these are the drawing rather than
 * the window they were looked for in -- two independently scanned rectangles agreeing to the
 * pixel is the same evidence hc_zone.h collects for its twenty-four numbers. The arcs of the
 * badge sit inside (`(` at x 32..37, and x 672..675 on deck 2) and the title's first glyph starts
 * at x 110, so this box holds `DECK ( 1 )` and nothing else.
 *
 * It is deliberately NOT padded out to a finger's size. The drawn box is the visual promise, the
 * rule hc_zone.h:51-54 states for its own cells, and 95 x 50 device pixels is already a
 * comfortable target at this scale.
 */
#ifndef RBPI4B_DECKBADGE_ZONE_H
#define RBPI4B_DECKBADGE_ZONE_H

#define DB_Y0          584
#define DB_Y1          633

#define DB_DECK1_X0     11
#define DB_DECK1_X1    105
#define DB_DECK2_X0    651      /* deck 1 + 640, which is the deck strips' own offset */
#define DB_DECK2_X1    745

/* Which deck's badge a logical point is on: 1 and *deck 0/1, or 0 for anywhere else. */
int deckbadge_hit(int x, int y, int *deck);

/* Deck `deck`'s badge rectangle, inclusive, as the same rect deckbadge_hit() tests -- so the
 * hit target and whatever is drawn over it cannot disagree. Out of range answers an empty rect. */
void deckbadge_rect(int deck, int *x0, int *y0, int *x1, int *y1);

#endif /* RBPI4B_DECKBADGE_ZONE_H */
