/*
 * test_padmode.c -- the pad-mode caption band, pinned. No Pi, no panel, no rbp.
 *
 * It links the PRODUCTION padmode_zone.c. The module is pure -- a point and a mode in, a deck and
 * a keycode out -- which is what lets the band and the cycle be checked on a host rather than by
 * a finger on the glass. What is pinned:
 *
 * 1. THE BAND'S EDGES, in both directions, because the two ways of being wrong cost differently:
 *    a row too high swallows a report rbp wanted, and a row too low puts a mode change over a pad
 *    the operator can see -- the rule hc_zone.h:51-54 states for its own sixteen cells.
 * 2. THE TWO DECKS ARE SEPARATE. 630..650 is nobody's, and a point there answers nothing.
 * 3. THE CYCLE, all the way round and back, and that it never leaves the four mode keycodes: a
 *    fifth value here is a keycode rbp does something else with.
 */
#include <stdio.h>
#include <string.h>

#include "padmode_zone.h"
#include "rbp_abi.h"

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

/* ---------------------------------------------------------------- the band */

static void test_edges(void)
{
    int deck = -1;

    CHECK(padmode_band(100, PM_BAND_Y0, &deck) == 1 && deck == 0,
          "the band's top row, on deck 1, is in");
    CHECK(padmode_band(100, PM_BAND_Y1, &deck) == 1 && deck == 0,
          "and its bottom row is in");
    deck = -1;
    CHECK(padmode_band(100, PM_BAND_Y0 - 1, &deck) == 0 && deck == -1,
          "one row above is out, and does not write the deck");
    CHECK(padmode_band(100, PM_BAND_Y1 + 1, &deck) == 0,
          "one row below is out -- the row before the first pad cell");
    CHECK(padmode_band(100, PM_BAND_Y1 + 1, &deck) == 0,
          "and it stays out");

    /* The first pad cell's own row must NOT be in the band: a tap there is a cue in HOT CUE (or
     * a beat loop in another mode), which is hc_zone's business and not this module's. 518 is
     * hc_zone.h's HC_ROW1_Y0 and the band stops one row short of it. */
    CHECK(padmode_band(100, 518, &deck) == 0,
          "the first pad row is NOT in the band");

    /* The caption rbp draws is inside the band, which is the whole point of the height. */
    CHECK(padmode_band(100, 499, &deck) == 1 && padmode_band(100, 509, &deck) == 1,
          "rbp's own caption row (499..509) is inside the band");

    CHECK(padmode_band(PM_DECK1_X0, 500, &deck) == 1 && deck == 0, "deck 1's left edge");
    CHECK(padmode_band(PM_DECK1_X1, 500, &deck) == 1 && deck == 0, "deck 1's right edge");
    CHECK(padmode_band(PM_DECK1_X1 + 1, 500, &deck) == 0,
          "one column past deck 1 is out");
    CHECK(padmode_band(PM_DECK2_X0, 500, &deck) == 1 && deck == 1, "deck 2's left edge");
    CHECK(padmode_band(PM_DECK2_X1, 500, &deck) == 1 && deck == 1, "deck 2's right edge");
    CHECK(padmode_band(PM_DECK2_X1 + 1, 500, &deck) == 0,
          "one column past deck 2 is out");

    /* BETWEEN THE DECKS IS NOBODY'S. hc_zone's grid has the same 21 px hole (630..650) and the
     * same test, and a tap there changing a deck's pad mode is the kind of thing that is only
     * noticed in the middle of a set. */
    CHECK(padmode_band(640, 500, &deck) == 0, "a tap between the decks is out");
    CHECK(padmode_band(630, 500, &deck) == 0 && padmode_band(650, 500, &deck) == 0,
          "including both ends of the gap");

    CHECK(padmode_band(0, 500, &deck) == 0, "the left margin is out");
    CHECK(padmode_band(1279, 500, &deck) == 0, "and the right one");
    CHECK(padmode_band(100, 0, &deck) == 0, "the top of the screen is out");
    CHECK(padmode_band(100, 799, &deck) == 0, "and the bottom");

    /* NULL is not a deck: the lookup is asked for a point's deck or for nothing at all. */
    CHECK(padmode_band(100, 500, NULL) == 1, "a NULL deck out-parameter is allowed");
}

/* --------------------------------------------------------------- the cycle */

static void test_the_cycle(void)
{
    /* The four keycodes in UiGetPadMode's own order, typed out here rather than taken from
     * padmode_zone.c, so that a reordering there is caught instead of agreed with. */
    static const int mode_key[PM_MODES] = { K_HOTCUE, K_ALOOP, K_SLIPLOOP, K_BEATJUMP };
    int m, back, k, i, j;

    CHECK(padmode_next_keycode(0) == K_ALOOP, "HOT CUE -> AUTO BEAT LOOP");
    CHECK(padmode_next_keycode(1) == K_SLIPLOOP, "AUTO BEAT LOOP -> SLIP BEAT LOOP");
    CHECK(padmode_next_keycode(2) == K_BEATJUMP, "SLIP BEAT LOOP -> BEAT JUMP");
    CHECK(padmode_next_keycode(3) == K_HOTCUE, "BEAT JUMP -> HOT CUE, back to the start");

    /* ALL THE WAY ROUND: every mode advances to the next, and the keycode it hands back is that
     * next mode's own -- the same four the panel's mode buttons drive (map_flx4.c). */
    m = 0;
    for (i = 0; i < PM_MODES; i++) {
        k = padmode_next_keycode(m);
        back = -1;
        for (j = 0; j < PM_MODES; j++)
            if (mode_key[j] == k)
                back = j;
        CHECK(back == (m + 1) % PM_MODES,
              "mode %d advanced to keycode %#x, which is mode %d and not %d",
              m, k, back, (m + 1) % PM_MODES);
        if (back < 0)
            break;
        m = back;
    }
    CHECK(m == 0, "four taps come back to the mode they started from");

    /* AND WHAT DOES NOT FIRE. -1 is what the reader answers when a link of its chain is missing,
     * and anything outside 0..3 is not a mode this module knows; both must send NOTHING, because
     * "send something" is a mode the operator did not ask for. */
    CHECK(padmode_next_keycode(-1) == 0, "a mode that could not be read sends nothing");
    CHECK(padmode_next_keycode(PM_MODES) == 0, "a fifth mode sends nothing");
    CHECK(padmode_next_keycode(99) == 0, "and so does nonsense");
}

int main(void)
{
    test_edges();
    test_the_cycle();

    printf("%s: %d checks, %d failures\n", failures ? "FAIL" : "ok", checks, failures);
    return failures ? 1 : 0;
}
