/*
 * deckmark_zone.c -- the bar's rectangle. No page, no rbp, no clock: a deck in, a rect out, which
 * is what lets test_deckmark.c pin it on a host.
 */
#include "deckmark_zone.h"

int deckmark_bar(int deck, int y0, int y1, int *x0, int *out_y0, int *x1, int *out_y1)
{
    int a, b;

    if (deck == 0) {
        a = DECKMARK_DECK1_X0;
        b = DECKMARK_DECK1_X1;
    } else if (deck == 1) {
        a = DECKMARK_DECK2_X0;
        b = DECKMARK_DECK2_X1;
    } else {
        return 0;                       /* no strip, no bar, and nothing written to the outs */
    }

    if (y1 < y0) {                      /* a caller that was handed a reversed pair meant this */
        int t = y0;

        y0 = y1;
        y1 = t;
    }

    if (x0)
        *x0 = a;
    if (out_y0)
        *out_y0 = y0;
    if (x1)
        *x1 = b;
    if (out_y1)
        *out_y1 = y1;
    return 1;
}
