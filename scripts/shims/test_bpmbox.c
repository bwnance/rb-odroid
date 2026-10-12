/*
 * test_bpmbox.c -- the two BPM boxes, pinned. No Pi, no panel, no rbp.
 *
 * It links the PRODUCTION bpmbox_zone.c. The module is pure -- a point in, a deck out -- which is
 * what lets both rectangles be checked on a host instead of by a thumb on the glass. What is
 * pinned, and why each one costs something if it breaks:
 *
 * 1. BOTH RECTANGLES, every edge in both directions. The box sits between the tempo readout and the
 *    deck strip's right edge, so a rect one column too wide steals taps from the tempo control --
 *    and a tap here turns SYNC ON, which is a change the operator hears.
 * 2. DECK 2 IS DECK 1 + 640. That is the evidence that these are the drawing rather than the window
 *    they were cropped in, and the first thing a mis-typed literal would break. It is the same
 *    offset the deck badge uses.
 * 3. THE HIT TEST AND THE RECTANGLE AGREE at all four corners, so a rect handed to anything else
 *    cannot end up somewhere a finger cannot reach.
 */
#include <stdio.h>

#include "bpmbox_zone.h"

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

static void edge(int deck, int x0, int y0, int x1, int y1, const char *who)
{
    int d = -1;

    CHECK(bpmbox_hit(x0, y0, &d) == 1 && d == deck, "%s: its top-left corner is in", who);
    CHECK(bpmbox_hit(x1, y1, &d) == 1 && d == deck, "%s: its bottom-right corner is in", who);
    CHECK(bpmbox_hit((x0 + x1) / 2, (y0 + y1) / 2, &d) == 1, "%s: its middle is in", who);

    d = -1;
    CHECK(bpmbox_hit(x0 - 1, y0, &d) == 0 && d == -1,
          "%s: one column left is out, and writes no deck", who);
    CHECK(bpmbox_hit(x1 + 1, y0, &d) == 0, "%s: one column right is out", who);
    CHECK(bpmbox_hit(x0, y0 - 1, &d) == 0, "%s: one row above is out", who);
    CHECK(bpmbox_hit(x0, y1 + 1, &d) == 0, "%s: one row below is out", who);
}

static void test_the_boxes(void)
{
    edge(0, BB_DECK1_X0, BB_Y0, BB_DECK1_X1, BB_Y1, "deck 1");
    edge(1, BB_DECK2_X0, BB_Y0, BB_DECK2_X1, BB_Y1, "deck 2");

    /* The tempo readout is to the LEFT of deck 1's box and the strip's right edge is beyond deck
     * 2's -- neither is ours. */
    CHECK(bpmbox_hit(BB_DECK1_X0 - 6, 670, NULL) == 0,
          "the tempo readout beside the box must not toggle SYNC");
    CHECK(bpmbox_hit(BB_DECK2_X1 + 6, 670, NULL) == 0,
          "and nor must the strip's edge past the box");

    /* BETWEEN THE DECKS IS NOBODY'S, and neither are the margins. */
    CHECK(bpmbox_hit(890, 670, NULL) == 0, "between the decks is out");
    CHECK(bpmbox_hit(0, 670, NULL) == 0 && bpmbox_hit(1279, 670, NULL) == 0,
          "the screen's margins are out");
    CHECK(bpmbox_hit(570, BB_Y0 - 1, NULL) == 0 && bpmbox_hit(570, BB_Y1 + 1, NULL) == 0,
          "the rows immediately above and below the box are out");

    CHECK(bpmbox_hit(570, 670, NULL) == 1, "a NULL deck out-parameter is allowed");
}

static void test_the_two_agree(void)
{
    int x0, y0, x1, y1, i;
    int rx[2], ry0[2], rx1[2], ry1[2];

    for (i = 0; i < 2; i++) {
        bpmbox_rect(i, &x0, &y0, &x1, &y1);
        rx[i] = x0; ry0[i] = y0; rx1[i] = x1; ry1[i] = y1;
        CHECK(bpmbox_hit(x0, y0, NULL) == 1 && bpmbox_hit(x1, y1, NULL) == 1 &&
              bpmbox_hit(x1 + 1, y0, NULL) == 0 && bpmbox_hit(x0 - 1, y0, NULL) == 0,
              "deck %d: the rectangle it hands out is the rectangle it answers a finger for", i);
    }

    CHECK(rx[1] - rx[0] == 640 && rx1[1] - rx1[0] == 640,
          "deck 2's box is deck 1's + 640 (got %d and %d)", rx[1] - rx[0], rx1[1] - rx1[0]);
    CHECK(ry0[1] == ry0[0] && ry1[1] == ry1[0], "and the two share their rows");
    CHECK(rx1[0] - rx[0] + 1 == 102 && ry1[0] - ry0[0] + 1 == 64,
          "and each is 102 x 64 as measured (got %d x %d)",
          rx1[0] - rx[0] + 1, ry1[0] - ry0[0] + 1);

    bpmbox_rect(2, &x0, &y0, &x1, &y1);
    CHECK(x0 == 0 && y0 == 0 && x1 == 0 && y1 == 0, "a third deck has no BPM box");
    bpmbox_rect(-1, &x0, &y0, &x1, &y1);
    CHECK(x0 == 0 && x1 == 0, "and neither has nothing");
    bpmbox_rect(0, NULL, NULL, NULL, NULL);   /* must not crash */
    checks++;
}

int main(void)
{
    test_the_boxes();
    test_the_two_agree();

    printf("%s: %d checks, %d failures\n", failures ? "FAIL" : "ok", checks, failures);
    return failures ? 1 : 0;
}
