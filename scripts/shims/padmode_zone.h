/*
 * padmode_zone.h -- tapping rbp's pad-mode caption to cycle the pad modes.
 *
 * The operator, 2026-10-10: "add a way to toggle through the pad modes, each time you tap the
 * section above the pads where the label line, toggle through the modes", then "on the glass",
 * then "it should probably be about the same height as what you are using for pad buttons".
 *
 * THE BAND. rbp draws the pad mode's own caption above the pad grid, and hc_zone.h:34-49 is the
 * measurement of where: the caption is y 499..509 and the first cell row starts at 518. A pad
 * cell is 20 rows tall (518..537), so the band is 20 rows as well -- 498..517 -- which puts the
 * drawn caption inside it and stops exactly one row short of the first cell. THAT HEIGHT IS A
 * DECISION and not a measurement, which is why it is written here rather than derived: the
 * caption alone is 11 rows and a finger is not, and padding further down would put a mode change
 * over a pad the operator can see -- the rule hc_zone.h:51-54 states for its own cells.
 *
 * THE X SPANS ARE MEASUREMENTS and are hc_zone.h's, restated rather than referenced because they
 * are literals there too: deck 1's four cells run 11..629, deck 2's 651..1269, and 630..650 is
 * nobody's -- the same split, for the same reason, as HC_DECK_SPLIT.
 *
 * THE CYCLE HOLDS NO STATE, and that is what keeps it honest. rbp's own reader says which mode a
 * deck is in -- UiGetPadMode @0xfd3cc, returning 0 HOT CUE / 1 AUTO BEAT LOOP / 2 SLIP BEAT LOOP /
 * 3 BEAT JUMP, with rbp_abi.h's UI_PADMODE_* block carrying the derivation and the measurement
 * (injecting ch0 notes 27/30/32/34 moved deck 1's value 3 -> 0, 1, 3, 2 exactly) -- and the four
 * mode keycodes are K_HOTCUE 0x4113, K_ALOOP 0x4114, K_SLIPLOOP 0x4115, K_BEATJUMP 0x4116: four
 * in a row, in that same order. So "the next mode" is mode + 1 mod 4 and nothing else, with no
 * counter to drift when rbp changes mode by some other route -- the panel's own four buttons,
 * say -- and a mode that cannot be read sends NOTHING rather than a guess.
 */
#ifndef RBPI4B_PADMODE_ZONE_H
#define RBPI4B_PADMODE_ZONE_H

/* The band, in the menu's own 1280x800 logical space, inclusive at both ends. */
#define PM_BAND_Y0      498
#define PM_BAND_Y1      517

#define PM_DECK1_X0      11     /* deck 1's caption, across its four cells */
#define PM_DECK1_X1     629
#define PM_DECK2_X0     651     /* deck 2's, and 630..650 belongs to neither */
#define PM_DECK2_X1    1269

#define PM_MODES          4     /* HOT CUE, AUTO BEAT LOOP, SLIP BEAT LOOP, BEAT JUMP */

/* Which deck's caption a logical point is on: 1 and *deck 0/1, or 0 for anywhere else -- off the
 * band vertically, outside either deck's span, or in the 21 px between them. */
int padmode_band(int x, int y, int *deck);

/* The keycode that advances `pad_mode` to the next one, or 0 for a mode that cannot be read
 * (-1, or anything outside 0..3). Pure, and test_padmode.c pins the whole cycle -- both
 * directions round it, and that it never returns a keycode outside the four. */
int padmode_next_keycode(int pad_mode);

#endif /* RBPI4B_PADMODE_ZONE_H */
