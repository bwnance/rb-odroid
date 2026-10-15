/*
 * deckbadge_zone.c -- see deckbadge_zone.h for the rectangles, where they came from and why the
 * drawn box is not padded.
 *
 * Pure, like hc_zone.c and padmode_zone.c: a point in, a deck out, and a rect out. Nothing here
 * reads rbp, the framebuffer or a clock, so test_deckbadge.c pins both decks on a host.
 */
#include "deckbadge_zone.h"

int deckbadge_hit(int x, int y, int *deck)
{
    if (y < DB_Y0 || y > DB_Y1)
        return 0;
    if (x >= DB_DECK1_X0 && x <= DB_DECK1_X1) {
        if (deck)
            *deck = 0;
        return 1;
    }
    if (x >= DB_DECK2_X0 && x <= DB_DECK2_X1) {
        if (deck)
            *deck = 1;
        return 1;
    }
    return 0;
}

void deckbadge_rect(int deck, int *x0, int *y0, int *x1, int *y1)
{
    int a = 0, b = 0, c = 0, d = 0;

    if (deck == 0) {
        a = DB_DECK1_X0;
        c = DB_DECK1_X1;
        b = DB_Y0;
        d = DB_Y1;
    } else if (deck == 1) {
        a = DB_DECK2_X0;
        c = DB_DECK2_X1;
        b = DB_Y0;
        d = DB_Y1;
    }
    if (x0) *x0 = a;
    if (y0) *y0 = b;
    if (x1) *x1 = c;
    if (y1) *y1 = d;
}
