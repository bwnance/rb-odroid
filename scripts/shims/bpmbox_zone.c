/*
 * bpmbox_zone.c -- see bpmbox_zone.h for the rectangles, how they were measured and why a colour
 * scan cannot find them.
 *
 * Pure, like hc_zone.c and deckbadge_zone.c: a point in, a deck out. Nothing here reads rbp, the
 * framebuffer or a clock, so test_bpmbox.c pins both boxes on a host.
 */
#include "bpmbox_zone.h"

int bpmbox_hit(int x, int y, int *deck)
{
    if (y < BB_Y0 || y > BB_Y1)
        return 0;
    if (x >= BB_DECK1_X0 && x <= BB_DECK1_X1) {
        if (deck)
            *deck = 0;
        return 1;
    }
    if (x >= BB_DECK2_X0 && x <= BB_DECK2_X1) {
        if (deck)
            *deck = 1;
        return 1;
    }
    return 0;
}

void bpmbox_rect(int deck, int *x0, int *y0, int *x1, int *y1)
{
    int a = 0, b = 0, c = 0, d = 0;

    if (deck == 0) {
        a = BB_DECK1_X0;
        c = BB_DECK1_X1;
        b = BB_Y0;
        d = BB_Y1;
    } else if (deck == 1) {
        a = BB_DECK2_X0;
        c = BB_DECK2_X1;
        b = BB_Y0;
        d = BB_Y1;
    }
    if (x0) *x0 = a;
    if (y0) *y0 = b;
    if (x1) *x1 = c;
    if (y1) *y1 = d;
}
