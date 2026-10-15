/*
 * side_zone.c -- the left/right edge drawers' geometry, hit test and gesture.
 *
 * See side_zone.h for the rules in full, the measurements behind every rectangle
 * and the reason this is a pure module. Nothing here reaches rbp, the framebuffer,
 * the environment or the clock; it is menu_zone.c's shape with the gesture rotated
 * onto the x axis and one instance of the state per edge.
 *
 * THE TWO EDGES SHARE ONE SET OF RULES because they are written in PANEL-LOCAL x
 * (side_zone.h): local 0 is the outer edge on both sides, so "inward" is +lx and
 * "outward toward its own edge" is -lx, whichever drawer it is. The mirror appears
 * exactly twice -- side_local_x() and side_abs_x() -- and nothing below knows which
 * edge it is on except through those.
 */
#include "side_zone.h"

#include <string.h>          /* memset, in side_reset() */

/* ---------------------------------------------------------------------------
 * State: ONE PRESS PER (SIDE, POINTER). Same fields as menu_zone.c:34-42, plus the
 * fader's own bookkeeping, because this gesture has a control that is a value rather
 * than a button.
 *
 * `swallow` is the latch, set once at the down edge and cleared once at the release.
 * `armed` is narrower: "this press began in the entry column and the drawer is
 * shut", the only press that can open it. `press_hit` is the control the press began
 * on -- the anchor for fire-on-release and for the drag; `cur_hit` is the control
 * under the finger now, which is what the painter highlights. `sent_v` is the last
 * fader value THIS PRESS sent, which is the dedup: a resting finger re-sends
 * nothing, and it is reset at every down edge so the first report of a new press is
 * never suppressed by the last press's value.
 *
 * EVERYTHING IN THERE BELONGS TO A FINGER, so there is one instance per pointer and
 * two fingers never share a field. That is the whole repair for the operator's *"if
 * i try to drag both volume meters it gets confused and only one of them changes"*:
 * before the second index existed, the two contacts were the same press, and the one
 * that latched first (the left edge, always, because the ladder runs i = 0 first)
 * swallowed every report of both.
 *
 * `open_state` is NOT here -- whether a DRAWER is out is a fact about the panel and
 * not about a finger, so it lives in sz_open[] below. Two hands on one drawer must
 * both see it out and only one of them needs to sweep it away.
 * ------------------------------------------------------------------------- */
struct side_st {
    int was_down;
    int swallow;
    int armed;
    int started_open;
    int start_lx;      /* local: the anchor for the swipe/close distance */
    int start_y;
    int start_abs;     /* absolute logical: what side_tap_point() replays */
    int press_hit;
    int cur_hit;
    int sent_v;        /* -1 when this press has sent no value yet */
};

static struct side_st st[2][SZ_PTRS];

/* Is this drawer out? Per PANEL, shared by both pointers. */
static int sz_open[2];

/* WHICH CONTROL EACH DRAWER'S FADER IS (SZ_MODE_VOL / SZ_MODE_PITCH). Per panel, like
 * sz_open[] and for the same reason -- it is a fact about the strip and not about a
 * finger -- but unlike sz_open[] it is NOT cleared by side_reset(): see side_zone.h. */
static int sz_mode[2];

/* A PRESS THAT BEGAN OFF EVERY OPEN PANEL, latched at its down edge so its release can
 * put the drawers away -- the "away" dismissal, back at the operator's ask (2026-10-12):
 * *"if i have the side panels up and i tap outside of it you can hide them"*.
 *
 * PER POINTER, like everything else here: two hands can each have a press down, and one
 * of them tapping the glass while the other rides a fader is the ordinary case, not a
 * corner. The latch is what makes the whole press ours -- a press that is swallowed must
 * see its own release, or rbp is handed an up for a down it never saw. */
static int away_down[SZ_PTRS];

int side_mode(int side)
{
    if (side != SZ_LEFT && side != SZ_RIGHT)
        return SZ_MODE_VOL;
    return sz_mode[side];
}

void side_set_mode(int side, int mode)
{
    if (side != SZ_LEFT && side != SZ_RIGHT)
        return;
    /* Anything that is not SZ_MODE_PITCH is SZ_MODE_VOL: the mode is a two-state
     * control and a caller passing a stray number must not make it a third. */
    sz_mode[side] = (mode == SZ_MODE_PITCH) ? SZ_MODE_PITCH : SZ_MODE_VOL;
}

/* ---------------------------------------------------------------------------
 * THE "AWAY" PRESS CAME BACK, AND IT IS THE ONLY LATCHED PRESS THAT IS NOT A SIDE'S.
 *
 * It was here once (as `away_down`), the operator removed it -- *"the side bar should stay
 * up until i swipe them away"* -- and on 2026-10-12 they asked for it again: *"if i have
 * the side panels up and i tap outside of it you can hide them"*. So a press beginning off
 * every open panel is latched by its own pointer and its release closes BOTH drawers.
 *
 * WHAT IT COSTS, stated once and in full: while a drawer is out, the first tap on the
 * glass rbp draws for itself is spent closing the panels and rbp does not get it. That is
 * the whole of the trade, and it is the operator's own ask.
 *
 * WITH NO DRAWER OPEN NOTHING IS LATCHED and the centre of the screen behaves exactly as
 * it did with the dismissal gone: the press is handed back untouched on both edges.
 *
 * AND IT IS CLEARED BY side_reset_all(), which is the one place that has to know about it
 * -- a finger lost with a dismissing press down would otherwise leave the next press of
 * that pointer swallowed by a drawer that had long since gone.
 * ------------------------------------------------------------------------- */

static int iabs(int v)
{
    return v < 0 ? -v : v;
}

/* THE ACT A PLAIN BUTTON FIRES, or SZ_ACT_NONE for anything that is not one. This list IS
 * the fire-on-release set -- SYNC, MASTER, CUE, PLAY and the two discs above the fader --
 * and it is one function rather than a chain of ?: so that adding a button is adding a
 * case and not editing a conditional that four others already depend on.
 *
 * It is a pure function of the hit, deliberately: the RULE (must have begun and ended on
 * the same box) lives at the caller, which is the only place that knows what the press
 * did in between. */
static int side_button_act(int hit)
{
    switch (hit) {
    case SZ_HIT_SYNC:   return SZ_ACT_SYNC;
    case SZ_HIT_MASTER: return SZ_ACT_MASTER;
    case SZ_HIT_CUE:    return SZ_ACT_CUE;
    case SZ_HIT_PLAY:   return SZ_ACT_PLAY;
    case SZ_HIT_RANGE:  return SZ_ACT_RANGE;
    case SZ_HIT_MT:     return SZ_ACT_MT;
    default:            return SZ_ACT_NONE;
    }
}

int side_hit(int side, int x, int y)
{
    int lx, sx;

    if (side != SZ_LEFT && side != SZ_RIGHT)
        return SZ_HIT_NONE;
    lx = side_local_x(side, x);
    if (lx < 0 || lx > SZ_W - 1 || y < 0 || y > SZ_H - 1)
        return SZ_HIT_NONE;

    /* THE BUTTONS FIRST, THEN THE FADER LANE, and the order cannot matter because
     * the boxes are disjoint by construction: the lane's top (SZ_FADER_GY0 = 206) is
     * BELOW the nudge pair's last row (199) and its bottom (SZ_FADER_GY1 = 608) is
     * ABOVE CUE's first row (628), so a press in the lane is never inside a button
     * and vice versa. test_side.c asserts both gaps.
     *
     * A press in the SEAM between the two nudge cells (8 px, x 86..93) is answered
     * BG and not a nudge: it is a deliberately ambiguous point and the safe answer is
     * "nothing", not "whichever cell the arithmetic rounded toward".
     *
     * The readout at y 206..225 sits inside the lane's own top slack, so touching the
     * number is a grab and not background -- the friendlier of the two answers, and
     * the one this header has promised since the fader existed. */
    if (lx >= SZ_TITLE_X0 && lx <= SZ_TITLE_X1 &&
        y >= SZ_TITLE_Y0 && y <= SZ_TITLE_Y1)
        return SZ_HIT_TITLE;
    /* THE THREE ROWS THAT KEEP THEIR SCREEN ORDER are read through side_screen_x(), which
     * reflects the panel-local x once more on the right drawer -- see its note in
     * side_zone.h. `sx` is this point as those rows see it: the same range on BOTH decks,
     * so the cell the operator sees on the screen's left is always the same control. */
    sx = side_screen_x(side, lx);

    /* THE BEAT-MATCH PAIR, side by side in the row SYNC used to have to itself. The 8 px
     * seam between them is nobody's, the same rule the nudge pair's seam follows. */
    if (y >= SZ_SYNC_Y0 && y <= SZ_SYNC_Y1) {
        if (sx >= SZ_SYNC_X0 && sx <= SZ_SYNC_X1)
            return SZ_HIT_SYNC;
        if (sx >= SZ_MASTER_X0 && sx <= SZ_MASTER_X1)
            return SZ_HIT_MASTER;
    }
    /* THE TWO DISCS, AND ONLY WHILE THE FADER IS A PITCH FADER -- *"TR and MT should be on
     * the pitch control"*. This is the ONE place side_hit() reads this module's own state
     * rather than only its arguments, and it has to: a control that is not drawn must not
     * be hittable, or a press on the blank glass where a disc used to be would be swallowed
     * by a button that is not there.
     *
     * THEIR HIT IS THEIR BOX ON PURPOSE -- a disc that answered only inside its own outline
     * would have four dead corners that look like a button, and no finger aims at a corner
     * of a circle. */
    if (side_mode(side) == SZ_MODE_PITCH && y >= SZ_RND_Y0 && y <= SZ_RND_Y1) {
        if (sx >= SZ_RNG_X0 && sx <= SZ_RNG_X1)
            return SZ_HIT_RANGE;
        if (sx >= SZ_MT_X0 && sx <= SZ_MT_X1)
            return SZ_HIT_MT;
    }
    if (lx >= SZ_BTN_X0 && lx <= SZ_BTN_X1) {
        if (y >= SZ_CUE_Y0 && y <= SZ_CUE_Y1)
            return SZ_HIT_CUE;
        if (y >= SZ_PLAY_Y0 && y <= SZ_PLAY_Y1)
            return SZ_HIT_PLAY;
    }
    /* THE NUDGE PAIR, AND IT KEEPS ITS SCREEN ORDER TOO: '-' on the screen's left and '+'
     * on its right, on both decks -- the operator's own correction of 2026-10-12, *"the
     * +/- on the pitch strip should always be in that order"*. A pair whose cells swap
     * sides is a pair you have to look at twice every time you move to the other deck, and
     * -/+ is a word rather than a position. So these go through side_screen_x() like the
     * three rows above. */
    if (y >= SZ_NUDGE_Y0 && y <= SZ_NUDGE_Y1) {
        if (sx >= SZ_NUDGE_M_X0 && sx <= SZ_NUDGE_M_X1)
            return SZ_HIT_NUDGE_M;
        if (sx >= SZ_NUDGE_P_X0 && sx <= SZ_NUDGE_P_X1)
            return SZ_HIT_NUDGE_P;
    }
    if (lx >= SZ_FADER_GX0 && lx <= SZ_FADER_GX1 &&
        y >= SZ_FADER_GY0 && y <= SZ_FADER_GY1)
        return SZ_HIT_FADER;

    return SZ_HIT_BG;
}

int side_entry_in(int side, int x, int y)
{
    int lx;

    if (side != SZ_LEFT && side != SZ_RIGHT)
        return 0;
    lx = side_local_x(side, x);
    /* The y gate is the USB STOP defence, not tidiness -- see side_zone.h. The
     * right entry column is local 0..55 == absolute 1224..1279, which is exactly the
     * band's seventh column. */
    return lx >= 0 && lx <= SZ_ENTRY_W - 1 && y >= SZ_ENTRY_Y0 && y <= SZ_H - 1;
}

/* The value, as an exact int -- 1023 at the top, 0 at the bottom, clamped outside
 * the track so a finger that wanders past either end holds the end value instead of
 * wrapping. 1023 * 360 fits an int with four orders of magnitude to spare. */
int side_fader_v(int y)
{
    if (y <= SZ_FADER_Y0)
        return 1023;
    if (y >= SZ_FADER_Y1)
        return 0;
    return (1023 * (SZ_FADER_Y1 - y)) / SZ_FADER_TRAVEL;
}

/* The inverse, for the painter's handle. Round-trips at both extremes because
 * 1023 * SZ_FADER_TRAVEL / 1023 is exact. */
int side_fader_y(int v)
{
    if (v < 0)
        v = 0;
    if (v > 1023)
        v = 1023;
    return SZ_FADER_Y1 - (v * SZ_FADER_TRAVEL) / 1023;
}

int side_fader_pct(int v)
{
    if (v < 0)
        v = 0;
    if (v > 1023)
        v = 1023;
    return (v * 100) / 1023;
}

int side_is_open(int side)
{
    if (side != SZ_LEFT && side != SZ_RIGHT)
        return 0;
    return sz_open[side];
}

int side_any_open(void)
{
    return sz_open[SZ_LEFT] || sz_open[SZ_RIGHT];
}

int side_pressed(int side)
{
    int ptr;

    if (side != SZ_LEFT && side != SZ_RIGHT)
        return SZ_HIT_NONE;
    /* Only a press this module is swallowing has a highlight, and `was_down` keeps a
     * press that has been released out of it. Both pointers are asked, primary
     * first: a highlight is a fact about the PANEL, and either hand can put one
     * there. (A second finger on a button cannot be the only thing highlighted and
     * be invisible to the painter -- the panel would look untouched while a nudge was
     * moving the tempo under it.) */
    for (ptr = 0; ptr < SZ_PTRS; ptr++) {
        if (!st[side][ptr].swallow || !st[side][ptr].was_down)
            continue;
        return st[side][ptr].cur_hit;
    }
    return SZ_HIT_NONE;
}

void side_tap_point(int side, int *x, int *y)
{
    if (side != SZ_LEFT && side != SZ_RIGHT)
        side = SZ_LEFT;
    /* THE PRIMARY POINTER'S ANCHOR, deliberately. A TAP is the one answer the caller
     * REPLAYS to rbp, and rbp has one pointer, so the second contact's tap is dropped
     * rather than replayed (pointsrc.c's pointer_report_alt). Reading a pointer that
     * can never produce a replay would be reading a field nobody asked about. */
    if (x)
        *x = st[side][SZ_PTR_MAIN].start_abs;
    if (y)
        *y = st[side][SZ_PTR_MAIN].start_y;
}

/* Is a nudge running on this side, and which way? -1 '-' held, +1 '+' held, 0 none.
 *
 * This is the single source of truth for the nudge, and it is DERIVED rather than
 * remembered -- from the press that is down and the cell it began on -- because the
 * failure mode is asymmetric: a press that ended without its lift reaching us leaves
 * the tempo moved, and nothing the operator can do on the glass puts it back. Reading
 * it back off the same fields the gesture already uses means there is no second copy to
 * fall out of step. The drawer must still be out, which it always is for a press of
 * ours on a panel, but stating it is free and it makes the answer correct even if that
 * ever changes. */
int side_nudging(int side)
{
    int ptr;

    if (side != SZ_LEFT && side != SZ_RIGHT)
        return 0;
    if (!sz_open[side])
        return 0;
    /* BOTH POINTERS, primary first. The caller turns any CHANGE in this answer into
     * the matching send, so a pointer this function could not see would have its
     * nudge ended -- and the tempo put back -- on the next report: the exact failure
     * the field exists to prevent, arriving from the other hand. */
    for (ptr = 0; ptr < SZ_PTRS; ptr++) {
        struct side_st *s = &st[side][ptr];

        if (!s->swallow || !s->was_down)
            continue;
        if (s->press_hit == SZ_HIT_NUDGE_P)
            return 1;
        if (s->press_hit == SZ_HIT_NUDGE_M)
            return -1;
    }
    return 0;
}

void side_reset(int side)
{
    int ptr;

    if (side != SZ_LEFT && side != SZ_RIGHT)
        return;
    sz_open[side] = 0;
    for (ptr = 0; ptr < SZ_PTRS; ptr++) {
        memset(&st[side][ptr], 0, sizeof st[side][ptr]);
        st[side][ptr].sent_v = -1;
    }
}

void side_reset_all(void)
{
    int ptr;

    side_reset(SZ_LEFT);
    side_reset(SZ_RIGHT);
    /* ...AND THE AWAY LATCH, which does not belong to a side. It is cleared here rather
     * than in side_reset() because it is not one panel's state, and it MUST be cleared --
     * a finger lost with a panel-dismissing press down would leave the next press of that
     * pointer swallowed by a drawer that had long since gone. */
    for (ptr = 0; ptr < SZ_PTRS; ptr++)
        away_down[ptr] = 0;
}

int side_feed(int side, int ptr, int down, int x, int y, int *act, int *value)
{
    struct side_st *s;
    int lx;

    if (act)
        *act = SZ_ACT_NONE;
    if (value)
        *value = 0;
    if (side != SZ_LEFT && side != SZ_RIGHT)
        return MZ_FEED_NONE;
    if (ptr < 0 || ptr >= SZ_PTRS)
        return MZ_FEED_NONE;
    s = &st[side][ptr];

    if (!down) {
        if (!s->was_down)
            return MZ_FEED_NONE;      /* a release we never saw the press for */
        s->was_down = 0;
        if (s->swallow) {
            int tap = 0;
            int a = SZ_ACT_NONE;

            /* Fire on the release, and only for a press that started AND ended on
             * the same box. `press_hit` is 0 (SZ_HIT_NONE) for any press that began
             * in the entry column or off the panel, so the swipe that opened the
             * drawer can never fire -- which is why it is captured at the down edge
             * rather than recomputed here.
             *
             * THE DRAWER IS NOT CLOSED HERE. It used to be: any release but the
             * fader's put it away. That was written when CUE and PLAY were the whole
             * panel, and it is wrong now -- the operator asked for a SYNC button and a
             * nudge pair beside them, and a panel that closes after every control
             * makes "SYNC, then nudge, then PLAY" three swipes. What puts a drawer
             * away is now only ever explicit: the outward background swipe (below), or
             * a press that begins off every open panel -- the "tap away" -- which
             * side_feed_any() answers. Neither can happen while this press is latched,
             * so a press that began with the drawer out never closes it. */
            if (s->started_open) {
                /* THE PLAIN BUTTONS, all of them by ONE rule: a press that began AND ended
                 * on the same box. A slide-off cancels -- menu_zone.h's fire-on-release
                 * rule -- which is why cur_hit is compared at all, and the reason a finger
                 * that rolls from SYNC onto MASTER fires neither. side_button_act() is the
                 * list; this is the rule. */
                int btn = (s->cur_hit == s->press_hit)
                          ? side_button_act(s->press_hit) : SZ_ACT_NONE;

                if (btn != SZ_ACT_NONE)
                    a = btn;
                else if (s->press_hit == SZ_HIT_TITLE && s->cur_hit == s->press_hit) {
                    /* THE TOGGLE. It flips the mode HERE, not in the caller, so the
                     * whole of this panel's state machine stays inside this module --
                     * side_zone.h's side_mode() note carries the argument. The same
                     * press-that-ended-where-it-began rule as SYNC/CUE/PLAY, so a
                     * finger that rolled off the header toggles nothing; and a press
                     * that travelled is the dismissal sweep instead, which is checked
                     * while the finger is still down and has already put the drawer
                     * away by the time this runs, leaving cur_hit 0 here. */
                    sz_mode[side] = (sz_mode[side] == SZ_MODE_PITCH)
                                    ? SZ_MODE_VOL : SZ_MODE_PITCH;
                    a = SZ_ACT_MODE;
                }
                else if (s->press_hit == SZ_HIT_NUDGE_M ||
                         s->press_hit == SZ_HIT_NUDGE_P)
                    /* THE NUDGE ALWAYS ENDS, and the restore is emitted whether or
                     * not the finger is still on the cell it started on: the press
                     * moved the tempo and only the lift puts it back, so a restore
                     * that outlives its press is the one failure here the operator
                     * could not undo by lifting a finger. A slide-off therefore
                     * cancels the nudge and fires no button -- a nudge that wandered
                     * is not a press. */
                    a = SZ_ACT_NUDGE_STOP;
            } else if (!sz_open[side]) {
                /* Began in the entry column and the drawer is still shut, so it
                 * never travelled far enough to be a swipe: a tap, and the one report
                 * the drawer takes and gives back. `!sz_open` is the whole of "and
                 * never opened it" for the same reason menu_zone.h gives: a press
                 * that begins in the entry column cannot satisfy the outward
                 * dismissal rule, so sz_open can only be 1 here if this very press
                 * opened it -- and then it is a swipe, not a tap. */
                tap = 1;
            }
            s->swallow = 0;
            s->armed = 0;
            s->started_open = 0;
            s->press_hit = 0;
            s->cur_hit = 0;
            s->sent_v = -1;
            if (tap)
                return MZ_FEED_TAP;
            if (act)
                *act = a;
            return MZ_FEED_TAKEN;
        }
        s->armed = 0;
        s->started_open = 0;
        return MZ_FEED_NONE;
    }

    lx = side_local_x(side, x);

    if (!s->was_down) {
        /* The down edge: decide once whether this press is ours. */
        s->was_down = 1;
        s->start_lx = lx;
        s->start_y = y;
        s->start_abs = x;
        s->sent_v = -1;
        s->started_open = sz_open[side];
        if (sz_open[side]) {
            /* Out: every press is ours, wherever it lands -- a press that dismisses
             * the drawer must not also press what is under it. */
            s->swallow = 1;
            s->armed = 0;
            s->press_hit = side_hit(side, x, y);
            s->cur_hit = s->press_hit;
        } else if (side_entry_in(side, x, y) && !menu_is_open()) {
            /* The entry column, and it is the ONLY arm the gate applies to.
             * Everything below (a press that started outside) is rbp's, and
             * everything above (a press that started with the drawer out) is decided
             * by sz_open alone.
             *
             * THE GATE IS THE BAND AND NOTHING ELSE. It used to carry
             * `&& !side_any_open()` as well, from when one plane meant one drawer at a
             * time; with two planes (drmband.c's shared device) the second panel has
             * to be reachable while the first is up, which is the feature the operator
             * asked for -- *"allow for both side panels to be visable at the same time
             * and to accept input"*. side_feed_any() is what keeps the routing honest
             * instead: an OPEN drawer's own panel is offered the press before any
             * entry column is, so this arm can only ever fire for a drawer that is
             * shut. */
            s->swallow = 1;
            s->armed = 1;
            s->press_hit = 0;
            s->cur_hit = 0;
        } else {
            /* Not ours, and never becomes ours: a press that starts outside is
             * returned untouched end to end (side_zone.h's latch rule). */
            s->swallow = 0;
            s->armed = 0;
            s->press_hit = 0;
            s->cur_hit = 0;
        }
        if (!s->swallow)
            return MZ_FEED_NONE;

        /* JUMP TO WHERE YOU TOUCH, and it is sent at the DOWN edge rather than on the
         * first motion: the operator's chosen model is absolute, so the landing point
         * IS the value and a finger that comes to rest without moving must still
         * have moved the fader. `sent_v` records it so the first motion report --
         * which the device may send at the same y -- re-sends nothing. */
        if (s->press_hit == SZ_HIT_FADER) {
            int v = side_fader_v(y);

            s->sent_v = v;
            if (act)
                *act = SZ_ACT_FADER;
            if (value)
                *value = v;
        } else if (s->press_hit == SZ_HIT_NUDGE_M || s->press_hit == SZ_HIT_NUDGE_P) {
            /* THE NUDGE MOVES THE TEMPO HERE, AND IT DOES NOT REPEAT. The press sends
             * one position and the LIFT puts the fader back, so one message per edge
             * is the whole protocol -- there is no repeat clock in this module and
             * none is wanted: a repeat would be a step the operator could not control,
             * and how far the tempo moves is the caller's SIDE_NUDGE_PCT and not how
             * long the cell was held. side_nudging() derives the truth from `press_hit`
             * and `was_down` so the restore cannot be lost. */
            if (act)
                *act = (s->press_hit == SZ_HIT_NUDGE_P) ? SZ_ACT_NUDGE_FWD
                                                        : SZ_ACT_NUDGE_REV;
        }
        return MZ_FEED_TAKEN;
    }

    /* Still the same press. */
    if (s->armed) {
        int dx = lx - s->start_lx;          /* inward is positive, on both edges */
        int dy = iabs(y - s->start_y);

        /* Predominantly horizontal, exactly as the band's opening swipe is
         * predominantly vertical: a drag down the edge is not a swipe and must reach
         * rbp's own controls untouched. */
        if (dx >= SZ_SWIPE_PX && dx > dy) {
            sz_open[side] = 1;
            s->armed = 0;
            /* press_hit stays 0: the finger is over the drawer now, but the press
             * began outside it, so lifting here opens and does nothing else. */
            s->cur_hit = side_hit(side, x, y);
        }
        return MZ_FEED_TAKEN;               /* armed implies swallowed */
    }

    if (s->swallow && sz_open[side]) {
        if (s->press_hit == SZ_HIT_FADER) {
            /* The drag. Absolute every time -- no accumulation, no anchoring -- and
             * sent only when the value CHANGES, which is the same dedup map_flx4.c
             * makes for the hardware fader: a resting finger is silent, and a
             * full-height drag is at most a few hundred sends. */
            int v = side_fader_v(y);

            s->cur_hit = SZ_HIT_FADER;
            if (v != s->sent_v) {
                s->sent_v = v;
                if (act)
                    *act = SZ_ACT_FADER;
                if (value)
                    *value = v;
            }
            return MZ_FEED_TAKEN;
        }
        if (s->press_hit == SZ_HIT_BG || s->press_hit == SZ_HIT_TITLE) {
            /* A swipe OUTWARD toward this drawer's own edge dismisses it. Outward is
             * -lx, so the distance is start_lx - lx and never negative for a finger
             * still inside the panel; a finger that has left the panel simply gets a
             * larger number, which is still a dismissal.
             *
             * THE TITLE IS INCLUDED, and that is not a detail: before the toggle
             * existed the header was background, so an outward sweep starting on it
             * put the drawer away. Making it a control would have taken that gesture
             * from the operator without being asked to, and the two rules cannot be
             * confused anyway -- this one needs SZ_CLOSE_PX of travel and the toggle
             * needs a press that ended where it began. */
            int out = s->start_lx - lx;
            int dy = iabs(y - s->start_y);

            if (out >= SZ_CLOSE_PX && out > dy) {
                sz_open[side] = 0;
                s->press_hit = 0;
                s->cur_hit = 0;
                return MZ_FEED_TAKEN;
            }
        }
        s->cur_hit = side_hit(side, x, y);
        return MZ_FEED_TAKEN;
    }

    return s->swallow ? MZ_FEED_TAKEN : MZ_FEED_NONE;
}

int side_feed_any(int ptr, int down, int x, int y, int *side_out, int *act,
                  int *value)
{
    int i, v;

    if (side_out)
        *side_out = -1;
    if (act)
        *act = SZ_ACT_NONE;
    if (value)
        *value = 0;
    if (ptr < 0 || ptr >= SZ_PTRS)
        return MZ_FEED_NONE;

    /* 0. A LATCHED PRESS OWNS ALL OF ITS OWN REPORTS, whichever side latched it and
     * whether or not the point is still on that side's panel. This comes first
     * because it is the only honest way to answer the two cases the geometry cannot:
     * a finger that wandered off the panel it began on (still that drawer's press),
     * and a swipe out of a shut drawer's entry column (which lands on the glass
     * BETWEEN the panels, where no panel test would claim it).
     *
     * AND IT IS THIS POINTER'S OWN PRESS. The `ptr` index is what makes the sentence
     * above true rather than almost true: with one state for both contacts, the
     * finger that latched first owned the other one's reports too, which is exactly
     * the operator's *"drag both volume meters ... only one of them changes"*.
     *
     * AND EVERY PRESS ENDS ON EVERY SIDE THAT SAW IT. The test is `was_down` and NOT
     * `swallow`, which is the difference between "owns this press" and "was offered
     * this press": a drawer that declined a report still recorded the down edge (that
     * is the latch rule -- a press that starts outside must never become the drawer's
     * by wandering into its entry column), so something has to tell it the finger has
     * gone. A declining side returns MZ_FEED_NONE, so the loop goes on to the owner,
     * and a decliner always PRECEDES the taker in this ordering (steps 1 and 2 both
     * run i = 0 first), so the release reaches everyone before it returns. */
    for (i = 0; i < 2; i++) {
        if (!st[i][ptr].was_down)
            continue;
        v = side_feed(i, ptr, down, x, y, act, value);
        if (v != MZ_FEED_NONE) {
            if (side_out)
                *side_out = i;
            return v;
        }
    }

    /* 0b. THE "AWAY" LATCH OWNS EVERY REPORT OF ITS OWN PRESS, and this is second only
     * because step 0 owns presses a DRAWER latched. A press is swallowed for its whole
     * life or not at all (the module's latch rule), so a motion of an away press must not
     * be re-offered below: a finger that put a panel away and then wandered onto the OTHER
     * edge would otherwise be read as a fresh swipe on a shut drawer's entry column, and
     * the lift would come back a TAP and replay a press to rbp that rbp never had a down
     * for. That is the whole reason this is here and not with the latch below. */
    if (away_down[ptr]) {
        if (down)
            return MZ_FEED_TAKEN;              /* still this press, still swallowed */
        away_down[ptr] = 0;
        sz_open[SZ_LEFT] = sz_open[SZ_RIGHT] = 0;
        return MZ_FEED_TAKEN;
    }

    /* 1. A FRESH PRESS ON AN OPEN DRAWER'S OWN PANEL -- and it is the only step that
     * can fire a control. The `side_hit() != SZ_HIT_NONE` test is what keeps a press
     * in the middle of the glass OUT of this step: without it, a drawer would swallow
     * every press on the screen. And with the "away" dismissal gone, that press is not
     * swallowed anywhere at all -- it goes on to rbp, which is what keeps the centre of
     * the screen alive while a drawer is out. Every open drawer is tried, because with
     * two panels out either one may own the point -- and only one can, 920 px apart. */
    if (down) {
        for (i = 0; i < 2; i++) {
            if (!sz_open[i])
                continue;
            if (side_hit(i, x, y) == SZ_HIT_NONE)
                continue;
            v = side_feed(i, ptr, down, x, y, act, value);
            if (v != MZ_FEED_NONE) {
                if (side_out)
                    *side_out = i;
                return v;
            }
        }
    }

    /* 2. A FRESH PRESS IN THE ENTRY COLUMN OF A SHUT DRAWER, so the second panel can
     * be swiped out while the first is already up -- the operator's "both at once". A
     * shut drawer's column lies at its own edge, outside the other's panel, so steps 1
     * and 2 cannot both claim a point. */
    if (down) {
        for (i = 0; i < 2; i++) {
            if (sz_open[i])
                continue;
            v = side_feed(i, ptr, down, x, y, act, value);
            if (v != MZ_FEED_NONE) {
                if (side_out)
                    *side_out = i;
                return v;
            }
        }
    }

    /* 3. THE "AWAY" DISMISSAL, AND IT IS BACK. It was here once and the operator removed
     * it -- *"the side bar should stay up until i swipe them away"* -- and on 2026-10-12
     * they asked for it again: *"if i have the side panels up and i tap outside of it you
     * can hide them"*. Reached only by a FRESH press, because steps 1 and 2 have already
     * claimed every press that lands on an open panel or in a shut drawer's entry column,
     * and once latched the press is owned by 0b above.
     *
     * THE COST IS THE OLD ONE AND IT IS WORTH STATING: while a drawer is out, the FIRST
     * tap on the glass rbp draws for itself is spent closing the panels and rbp does not
     * get it. That is what "tap outside to hide them" means; a tap the operator wants rbp
     * to act on is then the next one.
     *
     * THE PRIMARY HAND ONLY, and that is the second contact's own contract rather than a
     * detail: side_zone.h says the alt pointer is "offered to the DRAWERS and to nothing
     * else", and a second finger that happened to rest on the glass while the operator
     * read a panel must not put the panel away. So the latch is only ever set for
     * SZ_PTR_MAIN.
     *
     * WITH NO DRAWER OPEN THERE IS NO LATCH AND NOTHING CHANGES: a tap on rbp's own screen
     * is handed back untouched on both edges, exactly as it is with the dismissal gone. */
    if (down && ptr == SZ_PTR_MAIN &&
        (sz_open[SZ_LEFT] || sz_open[SZ_RIGHT])) {
        away_down[ptr] = 1;
        return MZ_FEED_TAKEN;
    }

    /* NOTHING HERE CLAIMED IT, and no panel is out: a press on the glass rbp draws for
     * itself -- the centre of the screen, most of what the operator touches -- is handed
     * back untouched, on its down edge and on its release alike. Nothing latches it, so
     * nothing has to unlatch it. The other dismissal is the outward background sweep in
     * side_feed(). */
    return MZ_FEED_NONE;
}
