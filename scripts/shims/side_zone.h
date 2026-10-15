/*
 * side_zone.h -- the left/right edge drawers' geometry, hit test and gesture.
 *
 * THE FEATURE. The operator asked for one drawer per edge carrying that deck's
 * transport and that channel's fader: *"can you add a side swipe on each side with
 * cue/play buttons for each deck and a fader control for each channel? deck 1 on
 * the left, deck 2 on the right"*. It exists because CUE, PLAY and the channel
 * faders are otherwise reachable only from the FLX4, and rbp builds its channel
 * faders at ZERO -- so a machine with no FLX4 has no transport and, worse, no way
 * to open a channel at all (docs/09-audio.md:667-714).
 *
 * This is menu_zone.h's sibling and is written to the same rule: pure C, no rbp
 * address, no env, no I/O, no clock. Everything it answers can be pinned on the
 * host, and it says WHICH control; how the key or the fader value reaches rbp is
 * the caller's (pointsrc.c).
 *
 * COORDINATES ARE PANEL-LOCAL, AND THAT IS THE WHOLE MIRROR. The two panels are
 * mirror images, so rather than writing every rectangle twice this module works in
 * a frame whose origin is the panel's OUTER edge: local x = 0 is the screen edge
 * the drawer slides in from and local x grows inward. For the left panel
 * `lx == x`; for the right one `lx == MZ_LOGICAL_W - 1 - x` (side_local_x()).
 * Every rule below -- the entry column, "inward" for the opening swipe, "outward"
 * for the dismissal, the drawn rectangles -- is then written ONCE and holds for
 * both edges. menu_zone.h's coordinates are logical and absolute, pre-reflection;
 * these are local to a panel, and side_abs_x() is the one place that becomes an
 * absolute logical x again.
 *
 * BOTH DRAWERS AT ONCE, which is what the operator asked for one round on -- *"allow
 * for both side panels to be visable at the same time and to accept input"*. Each
 * edge has its own plane (drmband.c holds the card and master once and refcounts
 * them), so the two are independent: either, both, or neither. What is still
 * exclusive is the top BAND's plane, which overlaps a drawer at the corners of the
 * glass -- opening a drawer tears the band's plane down and the last drawer to close
 * gives it back (menu_draw.c's side_plane_sync()). A consequence the operator will
 * see and should not read as a bug.
 *
 * WHAT A PRESS IS OFFERED TO, now that two panels can be out: the drawer whose panel
 * contains it, then the entry column of a SHUT drawer (so the second drawer can be
 * swiped out while the first is up), and then -- if a drawer is still out -- the press
 * itself as an "away" dismissal: anything on the glass BETWEEN the panels puts them away.
 * That last step was here once, was withdrawn (*"the side bar should stay up until i swipe
 * them away"*), and came back on the operator's ask of 2026-10-12: *"if i have the side
 * panels up and i tap outside of it you can hide them"*. Its cost is stated in full at
 * the step itself in side_zone.c. With NO drawer out, an unclaimed press is handed
 * straight back to rbp, exactly as it was while the dismissal was gone.
 */
#ifndef RBPI4B_SIDE_ZONE_H
#define RBPI4B_SIDE_ZONE_H

#include "menu_zone.h"       /* MZ_LOGICAL_W, MZ_LOGICAL_H, MZ_STRIP_Y1, MZ_FEED_* */

/* Which edge. The values are also the deck/channel number minus one and the array
 * index in this module's own state, which is why they are 0/1 and not 1/2. */
#define SZ_LEFT   0
#define SZ_RIGHT  1

/* ---------------------------------------------------------------------------
 * TWO CONTACTS, BECAUSE THE TWO CHANNEL FADERS ARE THE ONE CONTROL PAIR A DJ RIDES
 * WITH BOTH HANDS. The operator, on the glass: *"if i try to drag both volume meters
 * it gets confused and only one of them changes"* (2026-10-06).
 *
 * Every press in this module belongs to a POINTER, and every press's state is keyed
 * by (side, pointer) -- so two fingers on the two faders are two presses that never
 * touch each other's bookkeeping. `SZ_PTR_MAIN` is the primary contact, the one the
 * whole rest of the shim is built around (the band, the USB STOP chooser and rbp's own
 * pointer path all read it and only it); `SZ_PTR_ALT` exists for the drawers alone.
 *
 * WHY IT IS NOT JUST A SECOND SIDE. Until this change the panel's second contact was
 * indistinguishable from the first: the reader took whichever slot's
 * ABS_MT_POSITION_* arrived last, so dragging the RIGHT finger moved the LEFT
 * drawer's fader, and -- because a latched press owns every report of its own life --
 * the right channel never moved at all. Measured on the unit before the fix: slot 0
 * on the left lane, slot 1 dragged alone on the right, and the only line the log
 * produced was `side left fader ch1 -> 198`.
 *
 * WHAT THE ALT POINTER STILL CANNOT DO, stated: it is offered to the DRAWERS and to
 * nothing else. rbp's own pointer path, the band, the USB STOP chooser and the
 * UTILITY gesture are single-pointer by construction and stay that
 * way -- a second contact that lands anywhere but a drawer is dropped, never
 * forwarded, because rbp has one pointer and a second press would be a button it
 * cannot interpret.
 * ------------------------------------------------------------------------- */
#define SZ_PTRS      2
#define SZ_PTR_MAIN  0
#define SZ_PTR_ALT   1

/* ---------------------------------------------------------------------------
 * Geometry, in logical px -- the 1280x800 space every other zone in the shim
 * uses (menu_zone.h). 180 px is ~14% of the width: a real target for a fat finger
 * with 920 px of centre glass left uncovered.
 *
 * The x values below are PANEL-LOCAL (`lx`): 0 is the outer edge on BOTH sides, so
 * "16" is the same inset whether the drawer is left or right. The only absolute
 * numbers are SZ_RX0, which is left-panel arithmetic, and SZ_ENTRY_W, which is
 * quoted against the RIGHT entry column in the note under it.
 * ------------------------------------------------------------------------- */
#define SZ_W      180   /* panel width  */
#define SZ_H      MZ_LOGICAL_H      /* full height: the operator's "full-height drawer" */

/* The right panel's absolute left edge. Its x1 is MZ_LOGICAL_W - 1 by
 * construction, and SZ_RX0 == MZ_LOGICAL_W - SZ_W; test_side.c asserts both. */
#define SZ_RX0    (MZ_LOGICAL_W - SZ_W)

#define SZ_PAD    16    /* the inner inset from the outer edge, for the title */

/* One line of the text atlas, in logical px. A LITERAL here rather than
 * `#include "menu_font.h"`, so this module keeps menu_zone.h's "no dependencies"
 * shape -- menu_zone.h spells MZ_LOGICAL_W as 1280 for exactly this reason, and
 * test_side.c asserts the two numbers agree. The layout is the LINE BOX and not the
 * cap box (menu_font.h's rule), so nothing here follows the ink. */
#define SZ_LINE       24

/* Title (the channel label), AND IT IS A CONTROL NOW. Rows are one line box tall and
 * 12 rows in from the panel's top; test_side.c pins the text against the width it is
 * drawn into, so a label can never overflow the panel it names.
 *
 * THE OPERATOR'S TOGGLE. *"can you have some type of label toggle (fader/pitch) so it
 * switches between a volume fader and a pitch control"* (2026-10-12). The title row is
 * the whole of it -- it is the panel's one piece of idle glass, it already names the
 * strip, and a cell you tap to change what the cell says is the least machinery that
 * can answer the ask. It reads `CH 1 FADER` or `CH 1 PITCH` (side_paint.c), so the
 * label states which control the fader IS at the moment you read it.
 *
 * A TAP TOGGLES; A SWEEP FROM IT STILL DISMISSES. Before this existed the title was
 * SZ_HIT_BG, so an outward sweep that STARTED on it put the drawer away. Turning it
 * into a control could have taken that away silently -- the operator's own gesture,
 * removed by a change they did not ask for -- so the dismissal rule accepts a press
 * that began on the title exactly as it accepts one that began on the background.
 * The two cannot collide: the dismissal needs SZ_CLOSE_PX of outward travel and the
 * toggle needs a press that ended where it began. */
#define SZ_TITLE_X0    SZ_PAD
#define SZ_TITLE_X1    (SZ_W - 1 - SZ_PAD)
#define SZ_TITLE_Y0    12
#define SZ_TITLE_Y1    (SZ_TITLE_Y0 + SZ_LINE - 1)   /* 35 */

/* WHAT THE FADER IS. Per SIDE (per panel), because the two decks are independent and
 * an operator may want the volume on one and the pitch on the other while beatmatching
 * -- which is the whole reason this is panel state and not one global switch.
 *
 * NOT A PRESS FACT, so it is deliberately NOT cleared by side_reset(): that exists for
 * a FINGER that stopped existing, and a drawer that flipped back to volume because the
 * touch device hiccuped would be the panel changing a setting behind the operator. It
 * starts at SZ_MODE_VOL on every launch -- the control the drawer was built for. */
#define SZ_MODE_VOL    0
#define SZ_MODE_PITCH  1

/* ---------------------------------------------------------------------------
 * THE LAYOUT, TOP TO BOTTOM, and every row of it is the operator's own order: a
 * SYNC button at the TOP of the strip, the +/- nudge pair under it, the channel
 * fader and its value, and CUE and PLAY at the BOTTOM -- *"add a beat sync button
 * at the top of the strip"*, *"move the play/cue button to the bottom"*, *"add +/-
 * buttons to nudge the track"*. The bottom is the thumb's natural resting place on a
 * panel held at the edge of the glass, which is the whole reason PLAY moved there.
 *
 *   12.. 35   CH n        the title
 *   44..115   SYNC        full width
 *  128..163   -  |  +     the nudge pair, side by side, HALF SZ_BTN_H
 *  206..229   value       the fader's readout
 *  240..600   fader       the track, top = loud
 *  628..699   CUE         full width
 *  712..783   PLAY        full width
 *
 * Every block is a multiple of SZ_LINE (24) plus its padding, so the panel reads as
 * a column rather than as five unrelated numbers, and the whole stack ends at 783 --
 * SZ_PAD rows clear of the bottom edge, the same margin the title leaves at the top.
 *
 * THE NUDGE PAIR IS THE ONE BLOCK THAT IS NOT SZ_BTN_H -- the operator asked for it
 * at half height (*"the +/- buttons should be half of their current height"*,
 * 2026-10-09), so it keeps its top edge and gives up its bottom half. The rows BELOW
 * it do not move: the space it gave up is now a gap between the nudge pair and the
 * fader's readout, which is where a gap reads as separation between two groups. That
 * was chosen over pulling the readout and the fader up, which would have grown the
 * fader's own 360 px of travel -- a change to how the fader you are riding FEELS, and
 * not something this request asked for.
 * ------------------------------------------------------------------------- */

/* The common button box: full inner width, and SZ_BTN_H tall. 72 rows is the same
 * target menu_zone.h gives a strip button (40) at nearly twice the height, which is
 * what a thumb on a vertical panel wants, and it leaves room for six stacked things
 * in 800 rows. */
#define SZ_BTN_X0      SZ_PAD                        /* 16 */
#define SZ_BTN_X1      (SZ_W - 1 - SZ_PAD)           /* 163 */
#define SZ_BTN_H       72

/* BEAT SYNC IS TWO BUTTONS NOW -- SYNC and MASTER -- the operator's ask of 2026-10-12:
 * *"split the sync button into 2 buttons sync and master in the side panels"*. They sit
 * side by side in the row SYNC had on its own, split the way the nudge pair below them is
 * split: two half-width cells with an 8 px seam, so a press that straddles the middle is
 * nobody's. Together they are the beat-match pair -- MASTER picks the deck the others
 * follow, SYNC makes this one follow it -- which is why they belong beside each other
 * rather than SYNC keeping the full width. */
#define SZ_SYNC_GAP    8
#define SZ_SYNC_X0     SZ_BTN_X0                                    /* 16  */
#define SZ_SYNC_X1     ((SZ_BTN_X0 + SZ_BTN_X1 - SZ_SYNC_GAP) / 2)  /* 85  */
#define SZ_MASTER_X0   (SZ_SYNC_X1 + 1 + SZ_SYNC_GAP)                /* 94  */
#define SZ_MASTER_X1   SZ_BTN_X1                                    /* 163 */
#define SZ_SYNC_Y0     44
#define SZ_SYNC_Y1     (SZ_SYNC_Y0 + SZ_BTN_H - 1)    /* 115 */

/* ---------------------------------------------------------------------------
 * THE TWO ROUND BUTTONS, ABOVE THE FADER: the tempo RANGE (+-6 / +-10 / +-16 / WIDE) and
 * MASTER TEMPO. The operator's ask of 2026-10-12.
 *
 * THEY ARE ON THE PITCH CONTROL, and that is the operator's own correction the same day:
 * *"TR and MT should be on the pitch control"*. So they exist only while the fader is a
 * pitch fader -- which is the only time either of them means anything, since a tempo range
 * and a key lock are both properties of playing a track at a speed. side_mode() is what
 * side_hit() and side_paint() ask, and the repaint gate already carries the mode, so a
 * flip to VOLUME repaints and takes them away with it.
 *
 * THEY ARE ROUND, and that is not decoration: they are the two controls here that CHANGE A
 * SETTING rather than ride one, and a disc is what a settings key looks like on this
 * hardware. They are drawn by sp_fill_round() with a radius of half their width -- nothing
 * else on this panel is round.
 *
 * THE ROW LEAVES A GAP UNDER THE NUDGE PAIR, which is the operator's own ask (*"have some
 * padding b/w the -/+ buttons"*): the nudge pair ends at 163 and the discs begin at 174, so
 * eleven rows separate the two pitch controls. Below them is the value readout's row,
 * which in PITCH mode is empty by construction -- the readout is volume-only -- so 205 is
 * as far down as a disc can go and still be clear of the fader's own grab lane at 206.
 * They are symmetric about the panel's centre, 28 px either side of 89.5. */
#define SZ_RND_D       32                                   /* the disc's diameter */
#define SZ_RND_Y0      174
#define SZ_RND_Y1      (SZ_RND_Y0 + SZ_RND_D - 1)           /* 205 */
#define SZ_RNG_X0      46                                   /* tempo RANGE */
#define SZ_RNG_X1      (SZ_RNG_X0 + SZ_RND_D - 1)           /* 77  */
#define SZ_MT_X0       102                                  /* MASTER TEMPO */
#define SZ_MT_X1       (SZ_MT_X0 + SZ_RND_D - 1)            /* 133 */

/* THE NUDGE PAIR, side by side and full width between them, AND HALF THE HEIGHT of
 * every other button. Two cells of (SZ_BTN_X1 - SZ_BTN_X0 + 1 - SZ_NUDGE_GAP) / 2 = 70
 * px -- the height does not touch their width -- which is wide enough for a thumb and
 * for the drawn bar; the 8 px seam between them is what stops a press that straddles
 * the middle from being ambiguous. The marks are DRAWN, not set: the atlas carries '-'
 * (menu_font.h) and has no '+' at all, and a pair where one sign came from the font and
 * the other from two rectangles would not match -- side_paint.c draws both as bars for
 * exactly that reason.
 *
 * ITS CELL IS NOW THE PANEL'S TIGHTEST VERTICAL TARGET, which is what keeps
 * SP_SIGN_ARM down to a mark that fits in it (side_paint.h): at 36 logical rows a
 * 1280x800 panel gives the cell 36 device rows, so the drawn cross is the thing that
 * decides whether side_paint_ok() will draw the drawer at all on a SHORT panel. */
#define SZ_NUDGE_GAP   8
#define SZ_NUDGE_H     36     /* HALF of SZ_BTN_H -- the operator's ask */
#define SZ_NUDGE_Y0    128
#define SZ_NUDGE_Y1    (SZ_NUDGE_Y0 + SZ_NUDGE_H - 1) /* 163 */
#define SZ_NUDGE_M_X0  SZ_BTN_X0                      /* 16  */
#define SZ_NUDGE_M_X1  ((SZ_BTN_X0 + SZ_BTN_X1 - SZ_NUDGE_GAP) / 2)   /* 85  */
#define SZ_NUDGE_P_X0  (SZ_NUDGE_M_X1 + 1 + SZ_NUDGE_GAP)             /* 94  */
#define SZ_NUDGE_P_X1  SZ_BTN_X1                                      /* 163 */
#define SZ_NUDGE_M_X0  SZ_BTN_X0                      /* 16  */
#define SZ_NUDGE_M_X1  ((SZ_BTN_X0 + SZ_BTN_X1 - SZ_NUDGE_GAP) / 2)   /* 85  */
#define SZ_NUDGE_P_X0  (SZ_NUDGE_M_X1 + 1 + SZ_NUDGE_GAP)             /* 94  */
#define SZ_NUDGE_P_X1  SZ_BTN_X1                                      /* 163 */

/* The fader. The TRACK is what is drawn; the GRAB LANE is wider and taller on
 * purpose so a fat finger lands on it -- it carries SZ_FADER_PAD px of slack either
 * side of the track's own 32 px width, and it reaches UP to the readout's first row
 * so touching the number moves the fader (the promise this header has always made)
 * and DOWN 8 rows past the track for the same reason at the loud end.
 *
 * ITS ENDS ARE DISJOINT FROM EVERY BUTTON: the lane's top (206) is below the nudge
 * pair's last row (163 -- it was 199 until the pair went to half height, so the gap the
 * operator's change opened is 42 rows rather than 6) and its bottom (608) is above
 * CUE's first (628), so a drag can never begin a button press and a button press can
 * never start a drag. That is the same guarantee the old layout made with PLAY, kept by
 * arithmetic: test_side.c asserts both gaps.
 *
 * VERTICAL, track at the top = loud, which is rbp_vu.h's convention (1023 = at the
 * top) and the hardware's. */
#define SZ_FADER_X0     74     /* the drawn track, 32 px wide */
#define SZ_FADER_X1     105
#define SZ_FADER_Y0     240    /* top  = 1023 */
#define SZ_FADER_Y1     600    /* bottom = 0   */
#define SZ_FADER_TRAVEL (SZ_FADER_Y1 - SZ_FADER_Y0)   /* 360 px over 1024 steps */
#define SZ_FADER_PAD    30     /* the grab lane's slack beyond the track */
#define SZ_FADER_GX0    (SZ_FADER_X0 - SZ_FADER_PAD)  /* 44  */
#define SZ_FADER_GX1    (SZ_FADER_X1 + SZ_FADER_PAD)  /* 135 */
#define SZ_FADER_GY0    SZ_READ_Y0                    /* 206: the readout is a grab */
#define SZ_FADER_GY1    (SZ_FADER_Y1 + 8)             /* 608: 8 rows below the track */

/* The value readout, ABOVE the fader now (it was below when PLAY was below). 0..100
 * as a percentage: nothing else tells the operator where the fader is, and it makes a
 * screenshot self-proving. */
#define SZ_READ_X0      66
#define SZ_READ_X1      113
#define SZ_READ_Y0      206
#define SZ_READ_Y1      (SZ_READ_Y0 + SZ_LINE - 1)   /* 229 */

/* CUE and PLAY, at the BOTTOM and full width, in the operator's order: CUE above
 * PLAY, as on the hardware and as the fingers expect. Stacked rather than side by
 * side for the reason the old layout stacked them -- the inner width is
 * SZ_W - 2*SZ_PAD = 148 px, and two 74 px cells would put "PLAY" against the atlas's
 * own 65 px worst case ("USB STOP", menu_zone.h) with nothing to spare. */
#define SZ_CUE_Y0      628
#define SZ_CUE_Y1      (SZ_CUE_Y0 + SZ_BTN_H - 1)     /* 699 */
#define SZ_PLAY_Y0     712
#define SZ_PLAY_Y1     (SZ_PLAY_Y0 + SZ_BTN_H - 1)    /* 783 */

/* ---------------------------------------------------------------------------
 * The entry column, AND THE y GATE UNDER IT IS LOAD-BEARING.
 *
 * A swipe may only START in the outer SZ_ENTRY_W px of the glass while the drawer
 * is shut. 56 px matches MZ_SWIPE_PX/MZ_CLOSE_PX, so the runway is exactly one
 * swipe long.
 *
 * SZ_ENTRY_Y0 IS NOT TIDINESS. The right entry column, local x 0..55, is absolute
 * x 1224..1279 -- EXACTLY the band's seventh column, whose USB STOP cell sits at
 * y 8..47 (menu_zone.h's MZ_BTN_Y0/Y1). A drawer that armed there would swallow a
 * press aimed at the safe eject, and that press STOPS THE OPERATOR'S MEDIA. The
 * gate keeps the two entry zones disjoint in BOTH axes: the band's is x 426..852
 * with y <= MZ_STRIP_Y1 (55), the drawers' is the outer 56 px with y >= 56. So one
 * latched press can arm at most one surface, and the collision cannot return by
 * accident -- but any change to MZ_STRIP_Y1 reopens it and must reopen this gate.
 * ------------------------------------------------------------------------- */
#define SZ_ENTRY_W    56
#define SZ_ENTRY_Y0   (MZ_STRIP_Y1 + 1)   /* 56: below every band row */

#define SZ_SWIPE_PX   56   /* travel inward, dominant axis, that opens */
#define SZ_CLOSE_PX   56   /* travel outward, dominant axis, that dismisses */

/* What a logical point is over, in PANEL-LOCAL terms. SZ_HIT_NONE and SZ_HIT_BG
 * are different answers on purpose: NONE is "outside the panel" (or on a control's
 * own gap) and BG is "inside the panel and not on a control" -- and it is only the
 * second that can start a dismissal swipe.
 *
 * SZ_HIT_NUDGE_M AND _P ARE THE '-' AND '+' CELLS, in the operator's left-to-right
 * reading order: MINUS is the left cell, PLUS the right one. They are not "back" and
 * "forward" in this module -- that mapping is a fact about rbp's tempo slider, not
 * about the panel, and it lives in the caller's send. */
#define SZ_HIT_NONE    0   /* not on the panel */
#define SZ_HIT_SYNC    1
#define SZ_HIT_NUDGE_M 2   /* the '-' cell: move the tempo back   */
#define SZ_HIT_NUDGE_P 3   /* the '+' cell: move the tempo forward */
#define SZ_HIT_CUE     4
#define SZ_HIT_PLAY    5
#define SZ_HIT_FADER   6   /* the grab lane */
#define SZ_HIT_BG      7   /* inside the panel, on no control */
#define SZ_HIT_TITLE   8   /* the header row: the FADER/PITCH toggle */
#define SZ_HIT_MASTER  9   /* the MASTER half of the beat-match pair */
#define SZ_HIT_RANGE  10   /* the round tempo-RANGE button */
#define SZ_HIT_MT     11   /* the round MASTER TEMPO button */

/* What the caller must DO with a swallowed report, beside swallowing it. Cleared on
 * every call that is not a TAKEN report, exactly as menu_feed() clears its button.
 * There is no OPEN/CLOSE action: whether the drawer is out is state the driver
 * reads through side_is_open(), and the caller does not act on it.
 *
 * THE NUDGE IS THREE ACTIONS BECAUSE THE PRESS MOVES THE TEMPO AND ONLY THE LIFT PUTS
 * IT BACK. The pair is a hand on the tempo slider -- pointsrc.c sends
 * K_TEMPO_SLIDER/OP_VALUE, the same message the FLX4's own pitch fader sends, with the
 * fader's position moved by +-SIDE_NUDGE_PCT percent of its travel -- so the pair needs
 * a move down and a move up, and ONE restore that is direction-agnostic because putting
 * the fader back does not care which cell was held. That restore must be emitted on the
 * RELEASE edge, on a slide-off, on the drawer closing and on the touch device vanishing:
 * every one of those is a finger that stopped existing with the tempo still moved, and
 * nothing on the glass could then put it back. That is why side_nudging() exists to
 * reconcile it rather than four call sites each remembering. */
#define SZ_ACT_NONE        0
#define SZ_ACT_SYNC        1   /* send K_SYNC press+release on this side's channel */
#define SZ_ACT_NUDGE_REV   2   /* the '-' cell: move the tempo slider back */
#define SZ_ACT_NUDGE_FWD   3   /* the '+' cell: move the tempo slider forward */
#define SZ_ACT_NUDGE_STOP  4   /* the lift: put the tempo slider back */
#define SZ_ACT_CUE         5   /* send K_CUE press+release on this side's channel */
#define SZ_ACT_PLAY        6   /* send K_PLAY press+release on this side's channel */
#define SZ_ACT_FADER       7   /* send K_FADER/OP_VALUE = *value on this side's channel */
#define SZ_ACT_MODE        8   /* the title was tapped: the mode has ALREADY been flipped
                                * here, and this is the notification. It carries no
                                * message for rbp -- see the note under side_set_mode(). */
#define SZ_ACT_MASTER      9   /* send K_MASTER on this side's channel */
#define SZ_ACT_RANGE      10   /* send K_TEMPO_RANGE: cycle +-6 / +-10 / +-16 / WIDE */
#define SZ_ACT_MT         11   /* send K_MT: toggle master tempo (key lock) */

/* ---------------------------------------------------------------------------
 * The coordinate transform. Local x 0 is the OUTER edge on both sides, so the two
 * panels are one set of rules; these are the only two places the mirror exists.
 * ------------------------------------------------------------------------- */
static inline int side_local_x(int side, int x)
{
    return side == SZ_RIGHT ? (MZ_LOGICAL_W - 1 - x) : x;
}

static inline int side_abs_x(int side, int lx)
{
    return side == SZ_RIGHT ? (MZ_LOGICAL_W - 1 - lx) : lx;
}

/* ---------------------------------------------------------------------------
 * "SCREEN ORDER, NOT PANEL ORDER", and it is ONE rule for the three rows it applies to.
 *
 * THE DRAWER MIRRORS ITS LAYOUT, and three things on it must NOT mirror with it -- all of
 * them rows that are read as words rather than as positions:
 *
 *   - the pitch scale's `-` / `0` / `+` labels, which must stay on the SCREEN's left on
 *     both decks, or the operator re-reads the scale every time they look at the other
 *     one. The operator's ask of 2026-10-12: *"the +/-/0 indicators on the right panel
 *     should also be on the left side not right"*.
 *   - the SYNC | MASTER pair. *"channel 2 should be sync and then master not master and
 *     then sync order"*.
 *   - the tempo-RANGE | MASTER-TEMPO discs, *"same with the TR/MT buttons"*.
 *
 * EVERY ONE OF THOSE IS A TWO-CELL ROW SPLIT SYMMETRICALLY ABOUT THE PANEL'S CENTRE, so
 * "keep the screen order" is exactly "reflect the local x once more" -- and that is all
 * this is. It is its own inverse, which is what lets the HIT test and the PAINTER both go
 * through it: a cell that is swapped cannot then be drawn in one place and pressed in
 * another, which is the only way this could go wrong.
 *
 * Everything else on the panel -- the rail, the marks, the cap, CUE, PLAY, the nudge pair
 * -- mirrors as it always did. */
static inline int side_screen_x(int side, int lx)
{
    return side == SZ_RIGHT ? (SZ_W - 1 - lx) : lx;
}

/* The channel this side drives: the deck number rbp's send_rx_key() wants as its
 * channel argument (1 = deck 1, 2 = deck 2 -- map_flx4.c:910-913). Left is deck 1. */
static inline int side_channel(int side)
{
    return side == SZ_RIGHT ? 2 : 1;
}

/* What a logical point is over. `x` is ABSOLUTE logical (the space pointsrc.c works
 * in); the mirror is applied here so no caller has to remember which edge it is on.
 * Bounds are inclusive at both ends, as menu_button_at()'s are. */
int side_hit(int side, int x, int y);

/* Is a logical point in the entry column with the drawer shut? Absolute x, and
 * y >= SZ_ENTRY_Y0 is the USB STOP gate above. */
int side_entry_in(int side, int x, int y);

/* The fader's maths, exact ints so the boundaries are testable, exposed because the
 * painter needs the inverse to place the handle.

 *   side_fader_v(y)    clamps y to [SZ_FADER_Y0, SZ_FADER_Y1] and maps it to
 *                      0..1023 with 1023 at the top (y = SZ_FADER_Y0).
 *   side_fader_y(v)    the inverse; v is clamped to 0..1023, and v = 1023 gives
 *                      back SZ_FADER_Y0. Round-trips at both extremes.
 *   side_fader_pct(v)  the readout: 0..100, v clamped.
 *
 * 360 px of travel over 1024 steps is finer than a finger can aim, so the mapping
 * is not the limiting quantiser -- the dedup in the caller is. */
int side_fader_v(int y);
int side_fader_y(int v);
int side_fader_pct(int v);

/* The gesture. Returns menu_zone.h's MZ_FEED_* vocabulary -- the same three
 * answers, so pointsrc.c's funnel is written once -- and sets *act / *value for a
 * TAKEN report only:

 *   MZ_FEED_NONE   not ours: rbp gets this report unchanged, *act is SZ_ACT_NONE.
 *   MZ_FEED_TAKEN  swallowed. *act may be CUE, PLAY or FADER (with *value the
 *                  0..1023 position), or NONE.
 *   MZ_FEED_TAP    swallowed, and it was a press the drawer has no use for -- the
 *                  caller replays the whole press from side_tap_point(). Only ever
 *                  answered for a press that began in the entry column, never
 *                  travelled far enough to be a swipe, and left the drawer shut --
 *                  the mirror of menu_zone.h's tap rule, and it exists for the same
 *                  reason: a control rbp draws in the entry column is DELAYED, never
 *                  dropped.
 *
 * The rules, in local coordinates (lx), in full because this is the whole feature:

 *   down, shut, in the entry         -- swallowed and armed for an inward swipe.
 *   column (lx <= SZ_ENTRY_W-1 and
 *   y >= SZ_ENTRY_Y0)
 *   down, shut, anywhere else        -- NOT ours end to end: rbp keeps the whole
 *                                       press natively, with no replay. AND THAT IS
 *                                       UNCHANGED BY THE "AWAY" DISMISSAL: a press is
 *                                       only ever away while a drawer is OUT.
 *   down, out                        -- swallowed; remembers which control it began
 *                                       on. A press on the FADER lane jumps to the
 *                                       landing y and sends that value AT ONCE (the
 *                                       operator's "jump to where you touch"); a
 *                                       press on either nudge cell moves the tempo
 *                                       slider in that cell's direction. A press on
 *                                       the TITLE does nothing at the down edge -- it
 *                                       waits for the lift to know whether it was a tap
 *                                       or the start of a dismissal sweep.
 *   while down, armed & shut         -- opens on inward travel dx >= SZ_SWIPE_PX AND
 *                                       dx > |dy|: predominantly horizontal, so a
 *                                       vertical drag from the edge is not a swipe
 *                                       and rbp's own vertical gestures are
 *                                       untouched.
 *   while down, out, press began     -- the drag: recompute the value and send only
 *   on the fader lane                   when it CHANGES (the caller's dedup, the
 *                                       same one map_flx4.c:623-625 makes).
 *   while down, out, press began     -- a swipe of SZ_CLOSE_PX OUTWARD toward its
 *   on the background OR THE TITLE      own edge, same dominant-axis test, closes
 *                                       it mid-press. THE TITLE IS INCLUDED because it
 *                                       used to be background and an outward sweep from
 *                                       the header is a gesture the operator already
 *                                       has; the toggle needs a press that ended where
 *                                       it began, so travel and a tap cannot be
 *                                       confused for one another.
 *   release, started out             -- fires SYNC, CUE or PLAY only if the press
 *                                       began AND ended on the same box (slide-off
 *                                       cancels, menu_zone.h's fire-on-release
 *                                       rule). A press that began on a NUDGE cell puts
 *                                       the tempo back (SZ_ACT_NUDGE_STOP) whether or
 *                                       not the finger is still on the cell, and does
 *                                       NOT fire the button a slide-off landed on --
 *                                       a nudge that wandered is not a button press.
 *                                       A press that began and ended on the TITLE
 *                                       flips the fader's mode (SZ_ACT_MODE) by the
 *                                       same rule -- tap, not travel.
 *                                       The ONE exception to fire-on-release is a press
 *                                       that began on the FADER lane: the value has
 *                                       already been sent.
 *
 *   THE DRAWER STAYS OUT IN EVERY ONE OF THOSE CASES. It used to close on any
 *   release but the fader's; with a SYNC button, a nudge pair and the operator's
 *   *"allow for both side panels to be visable at the same time"*, a self-closing
 *   panel is unusable -- SYNC then NUDGE then PLAY would be three swipes. What closes
 *   a drawer is now only ever TWO acts: a swipe outward from its own background (above),
 *   and -- while a drawer is out -- a press on the glass between the panels, which is
 *   the "away" dismissal side_feed_any() answers.
 *
 *   THE "AWAY" PRESS IS THE ONE PRESS THIS MODULE LATCHES THAT IS NOT A SIDE'S, and it
 *   is in side_feed_any() rather than here because a press between the panels belongs to
 *   neither drawer. It is answered TAKEN on every edge once latched, so the whole press
 *   is swallowed and rbp is never handed an up for a down it did not see.

 *   release, started shut, opened    -- the drawer STAYS OPEN: lifting the finger
 *                                       does not take it away.
 *   release, started shut, never     -- MZ_FEED_TAP; the caller replays.
 *   opened
 *
 * A press is swallowed for its whole life or not at all (latched at the down edge):
 * a press that starts outside never becomes ours when it wanders in.
 *
 * ONE SURFACE AT A TIME, and that is now the BAND against the DRAWERS rather than one
 * drawer against the other. The caller's funnel is window -> band -> side, and this
 * module's closed-entry arm is gated on `!menu_is_open()` alone: with two drawers able
 * to be out together, a drawer may be swiped out while the other is already up, so a
 * gate on `side_any_open()` here would make the second panel unreachable -- which is
 * exactly the feature the operator asked for. The band's own arm keeps its
 * `&& !side_any_open()`, and band-first is still deliberate: an open band swallows at
 * full width and must answer first, and this module's `!menu_is_open()` gate is what
 * stops a drawer stealing a dismissing tap at the left edge (y >= SZ_ENTRY_Y0) before
 * the band can close itself.
 *
 * `ptr` is WHICH FINGER (SZ_PTR_MAIN / SZ_PTR_ALT). It selects one press's own
 * bookkeeping and nothing else -- the gesture rules above are identical for both, so
 * the second contact gets the whole drawer: it can ride a fader, press SYNC, hold a
 * nudge cell, swipe a shut drawer out and sweep one away. Only the state is separate. */
int side_feed(int side, int ptr, int down, int x, int y, int *act, int *value);

/* THE NUDGE, RECONCILED RATHER THAN REMEMBERED. Returns what THIS side's nudge is
 * doing right now: -1 while the '-' cell is held, +1 while the '+' cell is, 0 when
 * neither is.
 *
 * It exists because the moved tempo is state rbp holds rather than this process, so
 * "a cell is held" can be lost -- by a slide-off, by the panel closing, or by the touch
 * device vanishing with a finger still down -- and a tempo left moved has nothing on
 * the glass able to put it back. The caller calls this on every report path and once in
 * the device-loss path (beside side_reset_all()) and turns any change into the matching
 * send, so there is exactly one place that can be wrong instead of four. side_feed()'s
 * SZ_ACT_NUDGE_* answers are the edges of the same state and are how a caller that acts
 * only on reports gets told; this is how a caller that has to reconcile gets the
 * truth. */
int side_nudging(int side);

/* The funnel's ONE call: consult the drawers and report which one answered.
 *
 * WHICH ONE IS NOT THE CALLER'S PROBLEM, and getting it wrong is not subtle. The
 * order is fixed and each step is load-bearing:
 *
 *   1. the drawer whose OPEN PANEL contains the point -- its own controls are
 *      offered first, and it is the only step that can fire one;
 *   2. failing that, the ENTRY COLUMN of a SHUT drawer, left then right -- so the
 *      second panel can be swiped out while the first is already up. (While both are
 *      shut the two columns are disjoint and the order cannot matter; while one is
 *      open its column still lies outside the other's panel, so the columns stay
 *      disjoint in every state. The open drawer is simply not a candidate at this
 *      step, which is what makes the check one `if` per side rather than a tangle.)
 *   3. failing that, THE "AWAY" DISMISSAL, while a drawer is out: the press is latched
 *      by its own pointer and its RELEASE closes BOTH drawers. It is the operator's ask
 *      of 2026-10-12 -- *"if i have the side panels up and i tap outside of it you can
 *      hide them"* -- and it was a step here once before, withdrawn after *"the side bar
 *      should stay up until i swipe them away"*. What it costs is stated in full in
 *      side_zone.c: the first tap on rbp's own glass while a drawer is out is spent
 *      closing the panels and rbp does not get it.
 *
 *      WITH NO DRAWER OUT THIS STEP DOES NOT EXIST and nothing is latched: a press on
 *      the glass rbp draws for itself -- the centre of the screen, most of what the
 *      operator touches -- is handed back untouched, on its down edge and on its release
 *      alike, so rbp's own UI keeps working exactly as it did while the dismissal was
 *      gone.
 *
 * Sets *side_out to SZ_LEFT or SZ_RIGHT for a report either drawer took, or -1 for
 * MZ_FEED_NONE; *act / *value are side_feed()'s, and are cleared on every path that
 * is not that drawer's TAKEN answer.
 *
 * `ptr` is which finger, exactly as in side_feed(). Each pointer runs this ladder
 * over its OWN latched state, so the second contact's reports are never routed by
 * the first contact's press -- which is the defect this parameter exists to remove. */
int side_feed_any(int ptr, int down, int x, int y, int *side_out, int *act,
                  int *value);

/* Is this drawer out? Read by the painter and by the driver's plane sync. */
int side_is_open(int side);

/* Is EITHER drawer out? This is the gate menu_zone.c's closed-entry arm needs --
 * the band must not arm a swipe while a drawer owns the plane. One call rather than
 * two so the gate cannot be written with a side missing. */
int side_any_open(void);

/* WHICH CONTROL THIS DRAWER'S FADER IS, SZ_MODE_VOL or SZ_MODE_PITCH.
 *
 * side_zone.c OWNS THE MODE. A tap on the title flips it there and *then* reports
 * SZ_ACT_MODE, so the panel's own state machine stays inside the module that owns it --
 * the alternative, making the caller flip it, would put one half of a state machine in
 * pointsrc.c and the other half here. SZ_ACT_MODE is therefore a NOTIFICATION and not
 * an instruction: it carries no message for rbp, and a caller that does nothing with it
 * still gets a working toggle.
 *
 * `side_set_mode()` exists for the caller's own tests and for a future surface that
 * wants to set the mode rather than cycle it -- both host tests drive it directly to
 * restore SZ_MODE_VOL between scenarios, because the mode deliberately survives
 * side_reset(). */
int  side_mode(int side);
void side_set_mode(int side, int mode);

/* The control currently under the finger, SZ_HIT_* -- what the painter highlights.
 * SZ_HIT_NONE when no press of ours is down. BOTH POINTERS ARE ASKED, the primary
 * first, because a highlight is a fact about the panel and either hand can put one
 * there; asking only the primary would leave the second finger's press invisible. */
int side_pressed(int side);

/* Where the press that side_feed() just answered MZ_FEED_TAP for began, in ABSOLUTE
 * logical px. Read only immediately after that answer, the only moment the state it
 * reads is meaningful; the caller emits both edges there rather than at the release,
 * because rbp never saw the movement and a drifted finger is still a tap on the
 * thing it started on. ABSOLUTE, not local -- the caller feeds it straight to
 * tscfake_emit(), which works in the logical space. */
void side_tap_point(int side, int *x, int *y);

/* Forget everything for one side, and for both. Called where the touch device is
 * lost (beside menu_reset()): a drawer left open by a finger that went away with the
 * device would be a drawer nothing can dismiss.
 *
 * "EVERYTHING" HERE MEANS EVERY PRESS. It does not mean the mode: whether the fader is
 * a volume or a pitch control is the operator's setting and not a fact about a finger,
 * so side_reset() leaves side_mode() alone. A host test that toggles the mode must put
 * it back with side_set_mode() itself. */
void side_reset(int side);
void side_reset_all(void);

#endif /* RBPI4B_SIDE_ZONE_H */
