/*
 * padmode_zone.c -- see padmode_zone.h for the band, why it is that height, and the cycle.
 *
 * Pure, like hc_zone.c and fx_zone.c: nothing here reads rbp, the framebuffer or a clock. The
 * point and the mode arrive as arguments and the answer leaves as a deck and a keycode, which is
 * what lets test_padmode.c pin the band and the cycle on a host with no Pi, no panel and no
 * player.
 */
#include "padmode_zone.h"

#include "rbp_abi.h"    /* K_HOTCUE, K_ALOOP, K_SLIPLOOP, K_BEATJUMP */

/* THE ENUM ORDER, WRITTEN ONCE. UiGetPadMode's 0..3 and these four keycodes happen to be in the
 * same order, and it is stated as a table rather than as arithmetic on K_HOTCUE so that a
 * keycode inserted or moved between them cannot silently renumber the cycle -- the four are
 * consecutive today and nothing in rbp promises that. */
static const int mode_key[PM_MODES] = {
    K_HOTCUE, K_ALOOP, K_SLIPLOOP, K_BEATJUMP
};

int padmode_band(int x, int y, int *deck)
{
    if (y < PM_BAND_Y0 || y > PM_BAND_Y1)
        return 0;
    if (x >= PM_DECK1_X0 && x <= PM_DECK1_X1) {
        if (deck)
            *deck = 0;
        return 1;
    }
    if (x >= PM_DECK2_X0 && x <= PM_DECK2_X1) {
        if (deck)
            *deck = 1;
        return 1;
    }
    return 0;
}

int padmode_next_keycode(int pad_mode)
{
    /* A mode that could not be read is -1, and "send something" is the answer that costs the
     * operator a mode they did not ask for. There is no default that fires. */
    if (pad_mode < 0 || pad_mode >= PM_MODES)
        return 0;
    return mode_key[(pad_mode + 1) % PM_MODES];
}
