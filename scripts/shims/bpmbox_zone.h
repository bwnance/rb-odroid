/*
 * bpmbox_zone.h -- the BPM box on the performance screen, as a SYNC button.
 *
 * The operator, 2026-10-12: "if i click on the bpm box in performance view do make it turn on/off
 * sync". This module owns the RECTANGLE; pointsrc.c owns what a tap sends.
 *
 * THE TWO RECTANGLES ARE MEASURED, and measured the way the deck badges' were: by cropping /dev/fb0
 * and LOOKING, then cropping the numbers back with a 2 px margin to check that the drawn border
 * hugs the crop's edge. A brightness or hue scan does not work here: the box's border is ORANGE on
 * a deck that is the tempo master (it carries a MASTER tab then) and WHITE on one that is not, so a
 * colour test measures the deck's state rather than the box. The rect does not move:
 *
 *     deck 1   x 520..621   y 643..706      102 x 64
 *     deck 2   x 1160..1261 y 643..706      the same +640
 *
 * DECK 2 IS DECK 1 + 640, exactly -- the same offset the deck badge uses (deckbadge_zone.h), and
 * the same check that those are the drawing and not the window: two independently cropped rectangles
 * agreeing to the pixel.
 *
 * WHAT IS INSIDE IT is the header row ("BPM", plus a MASTER tab when this deck is the master) and
 * the value. It is the whole panel, which is what "the bpm box" means on the glass -- not just the
 * two letters.
 *
 * NOT PADDED OUT TO A FINGER, the rule hc_zone.h:51-54 states for its own cells: the drawn border is
 * the visual promise, and 102 x 64 device pixels is a comfortable target at this scale.
 */
#ifndef RBPI4B_BPMBOX_ZONE_H
#define RBPI4B_BPMBOX_ZONE_H

#define BB_Y0          643
#define BB_Y1          706

#define BB_DECK1_X0    520
#define BB_DECK1_X1    621
#define BB_DECK2_X0   1160      /* deck 1 + 640, which is the deck strips' own offset */
#define BB_DECK2_X1   1261

/* Which deck's BPM box a logical point is on: 1 and *deck 0/1, or 0 for anywhere else. */
int bpmbox_hit(int x, int y, int *deck);

/* Deck `deck`'s box, inclusive, as the same rect bpmbox_hit() tests -- so the hit target and
 * anything drawn over it cannot disagree. Out of range answers an empty rect. */
void bpmbox_rect(int deck, int *x0, int *y0, int *x1, int *y1);

#endif /* RBPI4B_BPMBOX_ZONE_H */
