/*
 * test_deckmark.c -- the playing bar's geometry and its painting, pinned. No Pi, no rbp, no page.
 *
 * It links the PRODUCTION deckmark_zone.c and deckmark_paint.c. Both are pure -- a deck in, a rect
 * out; a page, a rect and a colour in, pixels written -- which is what lets the bar be checked on a
 * host instead of by an eye 20 feet from a 10" screen. What is pinned, and what each one costs if
 * it breaks:
 *
 * 1. BOTH RECTS, edges in both directions, and deck 2 = deck 1 + 640. A bar one column wide reaches
 *    into the other deck's strip and says the wrong deck is playing.
 * 2. A DECK THAT HAS NO STRIP GETS NO BAR. deckmark.c draws through out-parameters, so a wrong
 *    answer here is a bar drawn at the screen's corner -- the failure mode with no other symptom.
 * 3. THE PAGE OUTSIDE THE BAR IS NOT TOUCHED. This paints into rbp's framebuffer, so an overrun is
 *    not a cosmetic bug: it is us damaging the player's own drawing, one row of the strip at a time.
 *    Checked pixel by pixel around the whole rectangle, not just at a corner.
 * 4. SAVE -> FILL -> RESTORE IS EXACTLY A ROUND TRIP, byte for byte. The bar has to be able to go
 *    away without leaving a green scar, and "exactly" is the only standard that guarantees it.
 */
#include <stdio.h>
#include <string.h>

#include "deckmark_paint.h"
#include "deckmark_zone.h"

static int checks, failures;

#define CHECK(cond, ...)                                                      \
    do {                                                                      \
        checks++;                                                             \
        if (!(cond)) {                                                        \
            failures++;                                                       \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);                       \
            printf(__VA_ARGS__);                                              \
            printf("\n");                                                     \
        }                                                                     \
    } while (0)

/* ---------------------------------------------------------------- the rectangle */

static void test_the_bars(void)
{
    int x0, y0, x1, y1;

    x0 = y0 = x1 = y1 = -1;
    CHECK(deckmark_bar(0, 780, 799, &x0, &y0, &x1, &y1) == 1, "deck 1 has a bar");
    CHECK(x0 == 11 && x1 == 629, "deck 1 spans its strip (got %d..%d)", x0, x1);
    CHECK(y0 == 780 && y1 == 799, "and the rows it was told (got %d..%d)", y0, y1);

    {
        int ax0, ay0, ax1, ay1;

        CHECK(deckmark_bar(1, 780, 799, &ax0, &ay0, &ax1, &ay1) == 1, "deck 2 has a bar");
        CHECK(ax0 == 651 && ax1 == 1269, "deck 2 spans its strip (got %d..%d)", ax0, ax1);
        CHECK(ay0 == 780 && ay1 == 799, "and shares the rows");
        CHECK(ax0 - x0 == 640 && ax1 - x1 == 640,
              "deck 2 is deck 1 + 640 (got %d and %d)", ax0 - x0, ax1 - x1);
    }

    /* A DECK WITH NO STRIP WRITES NOTHING. deckmark.c reads these outs whatever happens, so the
     * sentinels have to survive -- a bar at 0,0 is the failure this check exists for. */
    x0 = y0 = x1 = y1 = -77;
    CHECK(deckmark_bar(2, 780, 799, &x0, &y0, &x1, &y1) == 0, "a third deck has no bar");
    CHECK(x0 == -77 && y0 == -77 && x1 == -77 && y1 == -77, "and writes nothing to the outs");
    CHECK(deckmark_bar(-1, 780, 799, &x0, &y0, &x1, &y1) == 0, "and neither has nothing");

    /* A REVERSED PAIR IS THE CALLER'S TRANSPOSITION, not a request for an empty rectangle: the
     * painting loops would draw nothing and the marker would silently not exist. */
    CHECK(deckmark_bar(0, 799, 780, &x0, &y0, &x1, &y1) == 1, "a reversed pair still yields a bar");
    CHECK(y0 == 780 && y1 == 799, "with its rows the right way round (got %d..%d)", y0, y1);

    x0 = y0 = x1 = y1 = -1;
    deckmark_bar(0, 780, 799, NULL, NULL, NULL, NULL);   /* must not crash */
    checks++;
}

/* ---------------------------------------------------------------- the pixel form */

static void test_the_pixels(void)
{
    /* The accent green, in the project's own 0xRRGGBB palette form, must come out as the RGB565
     * word the rest of the shims already draw the ring with: menu_pixel(16, MENU_PLAY) = 0x2e88.
     * If these two ever disagree, the bar and the ring are two different greens. */
    CHECK(deckmark_pixel(16, 0x28d246u) == 0x2e88u,
          "the accent green encodes to the ring's own RGB565 word (got %#06x)",
          deckmark_pixel(16, 0x28d246u));
    CHECK(deckmark_pixel(16, 0x000000u) == 0x0000u, "black stays black");
    CHECK(deckmark_pixel(16, 0xffffffu) == 0xffffu, "white saturates");
    CHECK(deckmark_pixel(32, 0x28d246u) == 0x28d246u, "32 bpp carries the triple through");
    CHECK(deckmark_pixel(24, 0x28d246u) == 0, "a depth nobody draws in encodes to nothing");

    CHECK(deckmark_row_bytes(16, 11, 629) == 619 * 2, "a 16 bpp row is two bytes a pixel");
    CHECK(deckmark_row_bytes(32, 11, 629) == 619 * 4, "a 32 bpp row is four");
    CHECK(deckmark_row_bytes(16, 629, 11) == 0, "a reversed pair has no row");
    CHECK(deckmark_row_bytes(8, 11, 629) == 0, "and neither has a depth we cannot draw");
    CHECK(deckmark_rect_bytes(16, 2560, 11, 780, 629, 799) == 619 * 2 * 20,
          "twenty rows of a 16 bpp bar");
    CHECK(deckmark_rect_bytes(16, 0, 11, 780, 629, 799) == 0, "a zero pitch is refused");
    CHECK(deckmark_rect_bytes(16, 2560, 11, 799, 629, 780) == 0, "and so is a reversed pair");
}

/* ---------------------------------------------------------------- the page */

#define PW 1280
#define PH 800
#define PST 2560                                   /* BYTES per row, as the fb reports */
#define SENT 0xa5a5u                               /* what the page holds where we have not drawn */

#define DM_KEEP_MAX (619 * 2 * 48)                 /* the widest strip, the most rows */

static unsigned short page[PH * (PST / 2)];
static unsigned char  keep[DM_KEEP_MAX];

static void page_reset(void)
{
    int i;

    for (i = 0; i < PH * (PST / 2); i++)
        page[i] = SENT;
}

static unsigned short at(int x, int y)
{
    return ((unsigned short *)((unsigned char *)page + (size_t)y * PST))[x];
}

static void test_the_painting(void)
{
    int x0, y0, x1, y1, x, y, out = 0;

    deckmark_bar(0, 780, 799, &x0, &y0, &x1, &y1);
    page_reset();

    CHECK(deckmark_fill((unsigned char *)page, PST, 16, x0, y0, x1, y1, 0x2e88u) == 1,
          "the bar fills");
    CHECK(at(x0, y0) == 0x2e88 && at(x1, y1) == 0x2e88 && at((x0 + x1) / 2, (y0 + y1) / 2) == 0x2e88,
          "its corners and middle are the colour");
    CHECK(at(x0 - 1, y0) == SENT && at(x1 + 1, y0) == SENT,
          "and the columns either side are untouched");
    CHECK(at(x0, y0 - 1) == SENT && at(x1, y0 - 1) == SENT,
          "and the row above is untouched");
    /* THE ROW BELOW IS CHECKED ON A RECT WITH ROOM UNDER IT, because the default bar's bottom row
     * IS the page's last row -- that is a property of the default's placement, not of the painter,
     * and reading past the array to prove it would be a test that cannot fail honestly. */
    page_reset();
    CHECK(deckmark_fill((unsigned char *)page, PST, 16, x0, 700, x1, 719, 0x2e88u) == 1,
          "a bar with a row under it fills");
    CHECK(at(x0, 699) == SENT && at(x1, 699) == SENT && at(x0, 720) == SENT && at(x1, 720) == SENT,
          "and neither the row above nor the row below it moved");
    CHECK(at(x0 - 1, 710) == SENT && at(x1 + 1, 710) == SENT,
          "nor the columns beside it, at a row that is not the page's edge");

    page_reset();
    deckmark_bar(0, 780, 799, &x0, &y0, &x1, &y1);
    CHECK(deckmark_fill((unsigned char *)page, PST, 16, x0, y0, x1, y1, 0x2e88u) == 1,
          "the default bar fills again for the whole-page scan");

    /* EVERY PIXEL OF THE PAGE, so an overrun into a neighbouring row cannot pass on a corner. */
    for (y = 0; y < PH; y++)
        for (x = 0; x < PW; x++) {
            int inside = (x >= x0 && x <= x1 && y >= y0 && y <= y1);

            if (inside ? (at(x, y) != 0x2e88) : (at(x, y) != SENT))
                out++;
        }
    CHECK(out == 0, "the fill wrote the bar and nothing else on the whole page (%d wrong)", out);

    /* SAVE -> FILL -> RESTORE IS EXACT. The page is dirtied outside the bar first, so a restore that
     * rewrote more than the rectangle would be caught by the comparison. */
    page_reset();
    page[(y0 + 2) * (PST / 2) + 400] = 0x1234;
    page[(y1 + 5) * (PST / 2) + 700] = 0x5678;
    CHECK(deckmark_save((const unsigned char *)page, PST, 16, x0, y0, x1, y1, keep) == 1,
          "the bar's pixels are saved");
    CHECK(deckmark_fill((unsigned char *)page, PST, 16, x0, y0, x1, y1, 0x2e88u) == 1, "then filled");
    CHECK(deckmark_restore((unsigned char *)page, PST, 16, x0, y0, x1, y1, keep) == 1,
          "and put back");
    out = 0;
    for (y = 0; y < PH; y++)
        for (x = 0; x < PW; x++) {
            unsigned short want = SENT;

            if (x == 400 && y == y0 + 2)
                want = 0x1234;
            if (x == 700 && y == y1 + 5)
                want = 0x5678;
            if (at(x, y) != want)
                out++;
        }
    CHECK(out == 0, "and the page is byte-identical afterwards (%d wrong)", out);

    /* THE REFUSALS: a null page, a null keeper, a depth we do not draw in, and a zero pitch. */
    CHECK(deckmark_fill(NULL, PST, 16, x0, y0, x1, y1, 1) == 0, "a null page is refused");
    CHECK(deckmark_save((const unsigned char *)page, PST, 16, x0, y0, x1, y1, NULL) == 0,
          "a null keeper is refused");
    CHECK(deckmark_restore((unsigned char *)page, PST, 16, x0, y0, x1, y1, NULL) == 0,
          "and so is a null source");
    CHECK(deckmark_fill((unsigned char *)page, PST, 8, x0, y0, x1, y1, 1) == 0,
          "a depth we cannot draw is refused");
    CHECK(deckmark_fill((unsigned char *)page, 0, 16, x0, y0, x1, y1, 1) == 0,
          "a zero pitch is refused");
    CHECK(deckmark_fill((unsigned char *)page, PST, 16, x0, y1, x1, y0, 1) == 0,
          "and a reversed pair draws nothing at all");
}

int main(void)
{
    test_the_bars();
    test_the_pixels();
    test_the_painting();

    printf("%s: %d checks, %d failures\n", failures ? "FAIL" : "ok", checks, failures);
    return failures ? 1 : 0;
}
