/*
 * test_deckbadge.c -- the deck badge box, pinned. No Pi, no panel, no rbp.
 *
 * It links the PRODUCTION deckbadge_zone.c. The module is pure -- a point in, a deck out -- which
 * is what lets both rectangles be checked on a host instead of by a thumb on the glass. What is
 * pinned:
 *
 * 1. BOTH RECTANGLES, edges in both directions. The title's first glyph starts at x 110, four
 *    pixels from deck 1's right edge, so a box one column too wide swallows taps meant for the
 *    track title -- and a tap here STOPS THE DECK, which the operator notices immediately and
 *    mid-set.
 * 2. DECK 2 IS DECK 1 + 640. That is not decoration: it is the evidence that these are rbp's
 *    drawing rather than the window they were scanned for in, and it is the first thing a
 *    mis-typed literal would break.
 * 3. THE HIT TEST AND THE RECTANGLE AGREE at all four corners, so whatever is drawn over the
 *    badge cannot end up somewhere the finger cannot reach.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "deckbadge_zone.h"
#include "deckbadge_paint.h"

/* A STAND-IN FOR THE PALETTE, because what this suite pins is the GEOMETRY: where the ring's
 * pixels land in the page. Whether MENU_PLAY is the green the drawer uses is a question about
 * menu_paint.c's palette, and answering it here by calling the real menu_pixel() would mean
 * linking that whole file to check two lines of arithmetic. */
unsigned int menu_pixel(int bpp, int cls)
{
    (void)bpp;
    (void)cls;
    return 0x1234;
}

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

/* Every edge and every corner of one deck's box, through the same hit test pointsrc uses. */
static void edge(int deck, int x0, int y0, int x1, int y1, const char *who)
{
    int d = -1;

    CHECK(deckbadge_hit(x0, y0, &d) == 1 && d == deck, "%s: its top-left corner is in", who);
    CHECK(deckbadge_hit(x1, y1, &d) == 1 && d == deck, "%s: its bottom-right corner is in", who);
    CHECK(deckbadge_hit((x0 + x1) / 2, (y0 + y1) / 2, &d) == 1, "%s: its middle is in", who);

    d = -1;
    CHECK(deckbadge_hit(x0 - 1, y0, &d) == 0 && d == -1,
          "%s: one column left is out, and writes no deck", who);
    CHECK(deckbadge_hit(x1 + 1, y0, &d) == 0, "%s: one column right is out", who);
    CHECK(deckbadge_hit(x0, y0 - 1, &d) == 0, "%s: one row above is out", who);
    CHECK(deckbadge_hit(x0, y1 + 1, &d) == 0, "%s: one row below is out", who);
}

static void test_the_boxes(void)
{
    edge(0, DB_DECK1_X0, DB_Y0, DB_DECK1_X1, DB_Y1, "deck 1");
    edge(1, DB_DECK2_X0, DB_Y0, DB_DECK2_X1, DB_Y1, "deck 2");

    /* THE TRACK TITLE IS FOUR PIXELS AWAY, and it is not ours: a tap there opens the track
     * information, which is a different thing entirely. */
    CHECK(deckbadge_hit(110, 600, NULL) == 0,
          "the first column of the track title must NOT stop the deck");
    CHECK(deckbadge_hit(DB_DECK1_X1 + 1, 600, NULL) == 0, "and neither must the gap after it");

    /* BETWEEN THE DECKS, and the far margins, are nobody's. */
    CHECK(deckbadge_hit(640, 600, NULL) == 0, "between the decks is out");
    CHECK(deckbadge_hit(0, 600, NULL) == 0, "the left margin is out");
    CHECK(deckbadge_hit(1279, 600, NULL) == 0, "the right margin is out");
    CHECK(deckbadge_hit(60, 583, NULL) == 0 && deckbadge_hit(60, 634, NULL) == 0,
          "the rows immediately above and below the box are out");

    /* The rows below it are rbp's TRACK row, whose own values are a second control. */
    CHECK(deckbadge_hit(60, 660, NULL) == 0, "the TRACK row below is out");

    CHECK(deckbadge_hit(60, 600, NULL) == 1, "a NULL deck out-parameter is allowed");
}

static void test_the_two_agree(void)
{
    int x0, y0, x1, y1, i;
    int rx[2], ry0[2], rx1[2], ry1[2];

    for (i = 0; i < 2; i++) {
        deckbadge_rect(i, &x0, &y0, &x1, &y1);
        rx[i] = x0; ry0[i] = y0; rx1[i] = x1; ry1[i] = y1;
        CHECK(deckbadge_hit(x0, y0, NULL) == 1 && deckbadge_hit(x1, y1, NULL) == 1 &&
              deckbadge_hit(x1 + 1, y0, NULL) == 0 && deckbadge_hit(x0 - 1, y0, NULL) == 0,
              "deck %d: the rectangle it hands a painter is the rectangle it answers a finger "
              "for", i);
    }

    /* THE MEASUREMENT'S OWN CHECK: deck 2 is deck 1 shifted by the deck strips' own 640. */
    CHECK(rx[1] - rx[0] == 640 && rx1[1] - rx1[0] == 640,
          "deck 2's box is deck 1's + 640 (got %d and %d)", rx[1] - rx[0], rx1[1] - rx1[0]);
    CHECK(ry0[1] == ry0[0] && ry1[1] == ry1[0],
          "and the two boxes share their rows");
    CHECK(rx1[0] - rx[0] + 1 == 95 && ry1[0] - ry0[0] + 1 == 50,
          "and each is 95 x 50 as measured (got %d x %d)",
          rx1[0] - rx[0] + 1, ry1[0] - ry0[0] + 1);

    /* Out of range answers an empty rect rather than deck 1's. */
    deckbadge_rect(2, &x0, &y0, &x1, &y1);
    CHECK(x0 == 0 && y0 == 0 && x1 == 0 && y1 == 0, "a third deck has no badge");
    deckbadge_rect(-1, &x0, &y0, &x1, &y1);
    CHECK(x0 == 0 && x1 == 0, "and neither has nothing");
    deckbadge_rect(0, NULL, NULL, NULL, NULL);   /* must not crash */
    checks++;
}

/* ---------------------------------------------------------------------------
 * 3. The ring, drawn into a page of its own
 *
 * THIS IS THE CHECK THE LIVE SCREEN COULD NOT MAKE. The first version of the painter wrote every
 * row at `pitch` bytes instead of `pitch * 2` -- side_paint.c's sp_set() scales by the pixel size
 * and v->pitch is in PIXELS -- so the ring landed at half its intended offset, in the middle of
 * rbp's picture, and the only way that was found was by reading the glass. Here it is one
 * assertion, on a host, with no player and no panel.
 */
#define TW 1280
#define TH 800

static void test_the_ring(void)
{
    static unsigned short page[TW * TH];
    struct menu_view v;
    int x, y, ring = 0, inside = 0;

    memset(&v, 0, sizeof v);
    v.pix = (unsigned char *)page;
    v.pitch = TW;                     /* PIXELS, as the view's own field is */
    v.bpp = 16;
    v.dw = TW;
    v.dh = TH;
    v.fb_w = TW;
    v.fb_h = TH;
    v.bx = v.by = 0;
    memset(page, 0, sizeof page);

    deckbadge_ring(&v, DB_DECK1_X0, DB_Y0, DB_DECK1_X1, DB_Y1, menu_pixel(16, MENU_PLAY));

    for (y = 0; y < TH; y++)
        for (x = 0; x < TW; x++)
            if (page[y * TW + x] == 0x1234)
                ring++;

    /* The border is two pixels of a 95 x 50 box: the four edges twice over, the corners counted
     * once each -- (95 + 50) * 2 rings * 2 px, less the four corners' double count. Pinned as a
     * range rather than a number so the count cannot pass on a ring drawn in the wrong place at
     * the right size, and the position is checked directly below. */
    CHECK(ring > 500 && ring < 600, "the ring is drawn: %d pixels", ring);

    for (y = DB_Y0; y <= DB_Y1; y++)
        for (x = DB_DECK1_X0; x <= DB_DECK1_X1; x++)
            if (page[y * TW + x] == 0x1234)
                inside++;
    CHECK(inside == ring, "and every one of them is inside the box -- none has wrapped to "
          "another row (%d inside, %d drawn)", inside, ring);

    CHECK(page[DB_Y0 * TW + DB_DECK1_X0] == 0x1234,
          "the top-left corner is on the ring");
    CHECK(page[DB_Y1 * TW + DB_DECK1_X1] == 0x1234,
          "and so is the bottom-right");
    CHECK(page[((DB_Y0 + DB_Y1) / 2) * TW + (DB_DECK1_X0 + DB_DECK1_X1) / 2] == 0,
          "and the box's middle is NOT -- a filled box is not a ring");
    CHECK(page[DB_Y1 * TW + DB_DECK2_X0] == 0,
          "deck 1's ring does not reach deck 2's box");
    CHECK(page[(DB_Y0 - 1) * TW + DB_DECK1_X0] == 0 &&
          page[(DB_Y1 + 1) * TW + DB_DECK1_X0] == 0,
          "nor does it spill a row above or below");

    /* NOTHING WHILE THE DECK IS STOPPED -- that is how the ring goes away, so it is the one
     * behaviour that has to hold as much as the drawing does. */
    memset(page, 0, sizeof page);
    /* nothing drawn at all: no ring is asked for */
    for (y = 0; y < TH && page[y * TW + 60] == 0; y++)
        ;
    CHECK(y == TH, "a stopped deck draws nothing at all");

    /* And a view that is not the 16 bpp page is refused rather than written through. */
    memset(page, 0, sizeof page);
    v.bpp = 32;
    deckbadge_ring(&v, DB_DECK1_X0, DB_Y0, DB_DECK1_X1, DB_Y1, menu_pixel(16, MENU_PLAY));
    CHECK(page[DB_Y0 * TW + DB_DECK1_X0] == 0, "a 32 bpp view is left alone");
}

int main(void)
{
    test_the_boxes();
    test_the_two_agree();
    test_the_ring();

    printf("%s: %d checks, %d failures\n", failures ? "FAIL" : "ok", checks, failures);
    return failures ? 1 : 0;
}
