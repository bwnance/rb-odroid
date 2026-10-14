/*
 * settings_zone.c -- the SETTINGS panel's geometry, its rows and its gesture.
 *
 * See settings_zone.h for what it is for, why the rows are the switch rather than this
 * file, and why the target is the value and not the row. Nothing here reaches rbp, the
 * framebuffer or a clock: the whole module is the box, the rows, the two hit tests and
 * the arithmetic that turns a press into "which row".
 *
 * THERE IS NO HOLD AND SO NO FLASH. The chooser's three seconds and its blink exist
 * because ejecting is destructive; a row here moves a word in a file that the next tap
 * moves back, so a tap is complete at the release -- which is why this file is a third
 * of the USB STOP chooser's length -- the box that held the same three-second gesture until
 * 2026-10-14 -- and carries none of its label or box state.
 */
#include "settings_zone.h"
#include "menu_font.h"    /* menu_text_width(): how wide a value draws, which is what
                           * decides whether the box may be painted at all. A
                           * compile-time table and nothing else -- see the header. */

/* The rows in row-number order. A character the atlas cannot draw would ship as a gap
 * rather than as a failure, which is why test_settings.c walks every character of every
 * one of these against menu_font.h. */
static const char *const row_labels[SB_ROWS] = {
    "MIXER ROUTE", "USB1 STOP", "USB2 STOP", "SHARE", "ENCODE", "POINTER INJECT"
};

/* ---------------------------------------------------------------------------
 * THE VALUES, ONE TABLE PER ROW, and the tables are the whole of what a row can read.
 *
 * Writing them as tables rather than as a switch is what lets TWO things that must
 * agree be derived from one place: the readout (settings_row_text) and the widest value
 * the row can ever show (settings_row_widest_value), which is the number the paint
 * refusal measures the row against. A state-dependent refusal would be worse than
 * useless here -- the caller asks it once and paints only if it answers yes, so a value
 * that outgrew the row would take the WHOLE PANEL off the glass.
 * lesson, arrived at the same way.
 *
 * THE FIRST ENTRY OF EACH TABLE IS THE DEFAULT, and it is also what a NULL state reads.
 * That is deliberate and it is checked: settings_row_text() with no state, and with a
 * state of all zeros, must give the same strings, so a caller that has not managed to
 * read the files yet draws the shipped picture rather than a panel of blanks.
 * ------------------------------------------------------------------------- */
/* "USB 1 & USB 2" was a table entry until 2026-10-14; the two USB rows read the STICK's
 * own name now, out of the state, and -- when the slot has media but the stick has no label
 * of its own -- a dash, because the row is about the stick and its label is what the
 * operator asked it to show. An EMPTY slot reads EMPTY and is drawn dim. */
#define ST_USB_NONAME "--"
#define ST_USB_EMPTY  "EMPTY"

static const char *const val_mixer[]   = { "INTERNAL", "EXTERNAL" };
/* "USB 1 & USB 2" AND NOT "USB 1 + USB 2", which is what it was written as first. The 19 px
 * atlas (menu_font.h) is letters, digits and ./-_:?=&@# -- there is NO PLUS SIGN in it, and
 * a character the atlas does not carry draws as a GAP. So the plus version would have
 * shipped reading "USB 1  USB 2" with a hole in the middle: a value that is silently not
 * the one anyone wrote. The ampersand is in the set (index 70) and reads the same way.
 * test_settings.c walks every character of every value against menu_font_index() and is
 * what caught it -- which is that check's whole purpose. */
static const char *const val_share[]   = { "OFF", "ON" };
static const char *const val_encode[]  = { "RAW", "HWJPEG" };
static const char *const val_pointer[] = { "OFF", "ON" };

/* NULL for a row that does not read from a table -- the two USB rows, whose value is the
 * stick's name out of the state. Every function below has to allow for that. */
static const char *const *const row_values[SB_ROWS] = {
    val_mixer, NULL, NULL, val_share, val_encode, val_pointer
};

/* THE YARDSTICK each row's value column is measured against, which is NOT always its widest
 * possible string: a stick's name is the OPERATOR's to write and can be long, so it is CUT TO
 * FIT at paint time (settings_text_clip()) rather than refused -- the rule a box keeps
 * exactly, and for its reason: the size refusal is asked ONCE and paints only if it answers
 * yes, so a long name would take the whole panel off the glass. What is refused here is a
 * column too narrow to draw the value every row is guaranteed to have, so the USB rows are
 * measured against "EMPTY" -- what they read with nothing in the slot -- and a name that does
 * not fit is trimmed. */
static const char *const row_yardstick[SB_ROWS] = {
    "INTERNAL", ST_USB_EMPTY, ST_USB_EMPTY, "OFF", "HWJPEG", "OFF"
};

/* The word a tap writes, per row, INDEXED THE SAME WAY the display table is: the value
 * at index i says what the row reads, and next_word[i] says what a tap on that reading
 * asks for. NULL for a row that writes nothing -- the USB row raises a box.
 *
 * THE TWO CASES ARE THE FILE'S CASE AND NOT THE DISPLAY'S. The viewer's switch files
 * hold what confscreen.py writes ("on"/"off", "raw"/"hwjpeg") and the mixer's holds
 * audioshim's ("internal"/"external"), and none of them is upper case; the upper case
 * belongs to the readout. Writing SHARE's word as "ON" because that is what the row
 * reads would be a file vncserve compares against "on" and never matches. */
static const char *const next_mixer[]   = { "external", "internal" };
static const char *const next_share[]   = { "on", "off" };
static const char *const next_encode[]  = { "hwjpeg", "raw" };
static const char *const next_pointer[] = { "on", "off" };

/* SIX ENTRIES, ONE PER ROW, and getting this wrong is silent in the worst way: the table is
 * indexed by row number, so a missing entry slides every row after it onto its neighbour's
 * word -- "SHARE -> hwjpeg" -- and the last row reads PAST THE ARRAY. That is exactly what
 * happened when the second USB row was added and this table was not, and test_settings.c's
 * per-row value checks are what caught it. */
static const char *const *const row_next[SB_ROWS] = {
    next_mixer, NULL, NULL, next_share, next_encode, next_pointer
};

/* ---------------------------------------------------------------------------
 * State. All of it is cleared by settings_reset(); the touch thread is the only writer,
 * exactly as it is for menu_zone.c's band and fx_zone.c's picker, and menu_draw.c
 * only ever reads settings_is_open() and settings_pressed().
 *
 * `was_down` is the edge detector every feeder here carries, and it does the same second
 * job every feeder's here does: it suspends the timeout, so the panel cannot close under a
 * finger that has swallowed a press rbp is still owed a release for.
 *
 * `press_row` is the anchor and `cur_value` is what the painter highlights. They are
 * separate on purpose and there are three values between them, which is the whole of the
 * value-only target: a press can begin on a row's frame (`press_row` set, `press_value`
 * clear), and that press must neither fire nor dismiss.
 * ------------------------------------------------------------------------- */
static int was_down;
static int open_state;
static int press_row;            /* the row the press began on, 1..SB_ROWS, or 0 */
static int press_value;          /* ...and whether it began on that row's VALUE */
static int cur_value;            /* the value rect under the finger right now, or 0 */
static unsigned long long deadline_ms;
/* THE HOLD, and it belongs to the two USB rows alone. `hold_row` is the one piece of state
 * that outlives a press report, and it is set only by a
 * press on a USB row rbp says is live: "there is a hold" and "there is a live device under a
 * finger that has not moved" cannot disagree. `flash_on` is the blink, and it is STATE rather
 * than a function of a clock because this module has no clock,
 * so the phase is advanced in the tick and read back by the painter. */
static int hold_row;             /* ST_ROW_USB1, ST_ROW_USB2, or 0 */
static unsigned long long hold_since_ms;
static int flash_on;             /* 1 while the held row draws PRESSED */

/* The clock is the caller's and it is 64 bits wide on purpose: `long` is four bytes on
 * this target and a monotonic millisecond count in one wraps every 4.29 s, which is the
 * trap pointsrc.c's own shim_now_ms() documents. The comparison is a signed difference
 * against a stored instant, never a comparison of two instants. */
static int expired(unsigned long long now_ms)
{
    return (long long)(now_ms - deadline_ms) >= 0;
}

/* Has the running hold reached the operator's three seconds yet? */
static int hold_reached(unsigned long long now_ms)
{
    return (long long)(now_ms - hold_since_ms - ST_HOLD_MS) >= 0;
}

/* Which half of the blink the hold is in. PRESSED first and for a whole half period, so a
 * tap -- however short -- still shows the operator that their finger landed on something
 * live. */
static int flash_phase(unsigned long long now_ms)
{
    return ((now_ms - hold_since_ms) / ST_HOLD_FLASH_MS) & 1ull ? 0 : 1;
}

const char *settings_title(void)
{
    return SB_TITLE;
}

const char *settings_row_label(int row)
{
    if (row < 1 || row > SB_ROWS)
        return 0;
    return row_labels[row - 1];
}

int settings_row_ejects(int row)
{
    return row == ST_ROW_USB1 || row == ST_ROW_USB2;
}

int settings_text_clip(const char *text, int max_px, char *out, int max)
{
    int w = 0, k = 0;

    if (!out || max <= 0)
        return 0;
    out[0] = '\0';
    if (!text || max_px <= 0)
        return 0;
    while (text[k] && k < max - 1) {
        int adv = menu_font_adv((unsigned char)text[k], (unsigned char)text[k + 1]);

        if (w + adv > max_px)
            break;
        w += adv;
        out[k] = text[k];
        k++;
    }
    out[k] = '\0';
    return k;
}

int settings_act_channel(int row)
{
    if (row == ST_ROW_USB1)
        return 1;
    if (row == ST_ROW_USB2)
        return 2;
    return 0;                      /* every other row writes a file, and sends nothing */
}

/* WHICH ENTRY OF THE ROW'S TABLE IS IN FORCE.
 *
 * The mixer row is the one that is not a copy of a bit, and the readout it produces is
 * the decision the header records: with no recognised mixer on the bus the route in
 * effect is INTERNAL whatever the file says, because audioshim stays there rather than
 * sending the decks out on pairs nothing is listening to. So `present` is asked FIRST
 * and it gates the file's word entirely -- a unit with the file saying `external` and no
 * mixer plugged in reads INTERNAL, which is what it is playing. */
static int row_value_index(int row, const struct settings_state *S)
{
    switch (row) {
    case ST_ROW_MIXER:
        if (!S || !S->mixer_present)
            return 0;
        return S->mixer_external ? 1 : 0;
    case ST_ROW_SHARE:   return (S && S->share_on)   ? 1 : 0;
    case ST_ROW_ENCODE:  return (S && S->encode_hw)  ? 1 : 0;
    case ST_ROW_POINTER: return (S && S->pointer_on) ? 1 : 0;
    default:             return -1;        /* the USB rows: no table (settings_row_text) */
    }
}

/* The device slot a USB row is about, 0-based, or -1 for a row that is not one. */
static int row_usb_slot(int row)
{
    if (row == ST_ROW_USB1)
        return 0;
    if (row == ST_ROW_USB2)
        return 1;
    return -1;
}

const char *settings_row_text(int row, const struct settings_state *S)
{
    int slot = row_usb_slot(row);

    if (slot >= 0) {
        /* THE STICK'S OWN NAME, which is the operator's ask of 2026-10-14: "the setting
         * should be the USB label name". An empty slot says so, and a stick with no label
         * of its own reads a dash rather than borrowing the row's own "USB 1" back -- the
         * name column is the stick's, and a stick that has no name has no name. */
        if (!S || !S->usb_live[slot])
            return ST_USB_EMPTY;
        return S->usb_name[slot][0] ? S->usb_name[slot] : ST_USB_NONAME;
    }
    {
        int i = row_value_index(row, S);

        if (i < 0)
            return 0;
        return row_values[row - 1][i];
    }
}

const char *settings_row_widest_value(int row)
{
    const char *const *tab;
    const char *best;
    int i, n;

    if (row < 1 || row > SB_ROWS)
        return 0;
    tab = row_values[row - 1];
    if (!tab)
        return row_yardstick[row - 1];      /* a USB row: its guaranteed text, not a name */
    n = 2;      /* every table here is two entries, and test_settings.c pins that */
    best = tab[0];
    for (i = 1; i < n; i++)
        if (menu_text_width(tab[i]) > menu_text_width(best))
            best = tab[i];
    return best;
}

int settings_row_live(int row, const struct settings_state *S)
{
    switch (row) {
    case ST_ROW_MIXER:
        /* THE OPERATOR'S OWN RULE, 2026-10-14: with no recognised digital mixer on the
         * bus the row refuses the tap and is drawn dim. The choice only means something
         * while there is a device to hand the mixing to -- and with one there, BOTH
         * readings are useful (rbp's own Sound Color and Beat FX ride the master, so a
         * deck that wants one wants INTERNAL even with the mixer attached). */
        return S && S->mixer_present ? 1 : 0;
    case ST_ROW_USB1:
    case ST_ROW_USB2:
        /* Nothing to stop is nothing to offer -- the chooser dims a button whose device rbp
         * reports absent, and this is that question asked of ONE slot. It is also what stops
         * a hold from even starting on an empty row (settings_feed()). */
        return (S && S->usb_live[row_usb_slot(row)]) ? 1 : 0;
    case ST_ROW_SHARE:
    case ST_ROW_ENCODE:
    case ST_ROW_POINTER:
        return 1;
    default:
        return 0;
    }
}

const char *settings_next_value(int row, const struct settings_state *S)
{
    int i;

    if (row < 1 || row > SB_ROWS || !row_next[row - 1])
        return 0;
    /* A dim row answers nothing. The fire already refuses it (settings_feed()), and this
     * is the same refusal one door along, so a caller that reaches here without asking
     * settings_row_live() still cannot move a control that is not operable. */
    if (!settings_row_live(row, S))
        return 0;
    i = row_value_index(row, S);
    return row_next[row - 1][i];
}

/* ---------------------------------------------------------------- geometry */

int settings_row_at(int x, int y)
{
    int i;

    if (x < SB_ROW_X0 || x > SB_ROW_X1)
        return 0;
    for (i = 0; i < SB_ROWS; i++) {
        int y0 = SB_ROW_TOP(i), y1 = y0 + SB_ROW_H - 1;

        if (y >= y0 && y <= y1)
            return i + 1;
    }
    return 0;
}

int settings_value_at(int x, int y)
{
    int row = settings_row_at(x, y);

    if (!row)
        return 0;
    if (x < SB_VALUE_X0 || x > SB_VALUE_X1)
        return 0;                  /* the label half: the frame, and nothing else */
    return row;
}

void settings_row_rect(int row, int *x0, int *y0, int *x1, int *y1)
{
    if (row < 1 || row > SB_ROWS) {
        if (x0) *x0 = 0;
        if (y0) *y0 = 0;
        if (x1) *x1 = -1;
        if (y1) *y1 = -1;
        return;
    }
    if (x0) *x0 = SB_ROW_X0;
    if (y0) *y0 = SB_ROW_TOP(row - 1);
    if (x1) *x1 = SB_ROW_X1;
    if (y1) *y1 = SB_ROW_TOP(row - 1) + SB_ROW_H - 1;
}

void settings_value_rect(int row, int *x0, int *y0, int *x1, int *y1)
{
    if (row < 1 || row > SB_ROWS) {
        if (x0) *x0 = 0;
        if (y0) *y0 = 0;
        if (x1) *x1 = -1;
        if (y1) *y1 = -1;
        return;
    }
    if (x0) *x0 = SB_VALUE_X0;
    if (y0) *y0 = SB_ROW_TOP(row - 1);
    if (x1) *x1 = SB_VALUE_X1;
    if (y1) *y1 = SB_ROW_TOP(row - 1) + SB_ROW_H - 1;
}

void settings_box_rect(int *x0, int *y0, int *x1, int *y1)
{
    if (x0) *x0 = SB_X0;
    if (y0) *y0 = SB_Y0;
    if (x1) *x1 = SB_X1;
    if (y1) *y1 = SB_Y1;
}

/* ------------------------------------------------------------------ the state */

void settings_open(unsigned long long now_ms)
{
    open_state = 1;
    deadline_ms = now_ms + SB_TIMEOUT_MS;
    /* No press is inherited: the tap that raised this fired on a release, so there is
     * nothing for a finger to be the continuation of. */
    was_down = 0;
    press_row = 0;
    press_value = 0;
    cur_value = 0;
    hold_row = 0;
    flash_on = 0;
}

void settings_close(void)
{
    open_state = 0;
    was_down = 0;
    press_row = 0;
    press_value = 0;
    cur_value = 0;
    hold_row = 0;
    flash_on = 0;
}

int settings_is_open(void)
{
    return open_state;
}

int settings_hold_row(void)
{
    return open_state ? hold_row : 0;
}

int settings_hold_flash(void)
{
    return open_state && hold_row ? flash_on : 0;
}

unsigned long long settings_hold_ms(unsigned long long now_ms)
{
    if (!open_state || !hold_row)
        return 0;
    return now_ms - hold_since_ms;
}

int settings_pressed(void)
{
    if (!open_state)
        return 0;
    /* IT IS 0 FOR THE ROW WHOSE HOLD IS RUNNING, and that is the only
     * way the blink has somewhere to blink to: a steady finger highlight and the lit half of
     * the flash are the same pair of pixels, so a row that was both would show nothing. */
    return hold_row ? 0 : cur_value;
}

unsigned long long settings_deadline(void)
{
    return deadline_ms;
}

int settings_tick(const struct settings_state *S, unsigned long long now_ms, int *act)
{
    if (act)
        *act = ST_ACT_NONE;

    if (!open_state)
        return ST_TICK_NONE;

    /* THE HOLD FIRST, and before the timeout -- a hold that completes at the same instant the
     * panel was due to expire is still a completed hold. */
    if (hold_row && hold_reached(now_ms)) {
        if (settings_row_live(hold_row, S)) {
            if (act)
                *act = hold_row;
            /* THE PANEL DOES NOT CLOSE HERE, and the reason is the release still to come: the
             * finger that made this hold is by definition still down, so a release report is on
             * its way and the panel has swallowed every report of that press. Closing now would
             * clear `was_down` and the release would reach rbp as an up for a down it never saw
             * ([[declined-press-must-still-see-release]]). So the press is DISOWNED instead --
             * anchor, highlight and hold all go -- and the release finds nothing anchored to it
             * and takes the ordinary "began outside a row" path, which closes the panel and
             * sends nothing. The hold cannot re-fire either, since there is no hold left. */
            press_row = 0;
            press_value = 0;
            cur_value = 0;
            hold_row = 0;
            flash_on = 0;
            return ST_TICK_EJECT;
        }
        /* The stick went away under the finger. The hold is off, nothing is sent, and the
         * panel stays up -- the operator is looking at a row that has just gone dim and may
         * want the other one. */
        hold_row = 0;
        flash_on = 0;
    }

    /* NOT WHILE A FINGER IS DOWN. A press the panel swallowed has been withheld from rbp
     * for its whole life and rbp is going to be given its release; closing mid-press would
     * clear `was_down` and hand that release back as a report with no press behind it
     * ([[declined-press-must-still-see-release]]). Deferring costs nothing -- the release
     * closes the panel on its own path a moment later, or leaves it up if it began on a
     * row. */
    if (!was_down && expired(now_ms)) {
        settings_close();
        return ST_TICK_TIMEOUT;
    }

    flash_on = hold_row ? flash_phase(now_ms) : 0;
    return hold_row ? ST_TICK_FLASH : ST_TICK_UP;
}

void settings_reset(void)
{
    settings_close();
    deadline_ms = 0;
}

/* ------------------------------------------------------------------ the input */

int settings_feed(const struct settings_state *S, int down, int x, int y,
                  unsigned long long now_ms, int *act)
{
    if (act)
        *act = ST_ACT_NONE;

    if (!open_state)
        return MZ_FEED_NONE;      /* shut: this module answers for nothing at all */

    if (!down) {
        int row;

        if (!was_down)
            return MZ_FEED_TAKEN; /* a release we never saw the press for: still
                                   * ours, because the panel is up and rbp must not
                                   * hear either edge of anything. */
        was_down = 0;

        /* Fire on the release, and only for a press that began and ended on the SAME
         * row's value. The row is taken from the RELEASE's own coordinates and not from
         * `cur_value`, for every feeder's reason: a fast flick can lift at a moved
         * position with no move report in between, and a release that moved must not act
         * on the value the finger has already left. */
        row = settings_value_at(x, y);

        /* A HOLD THAT COMPLETED WITH NO TICK IN BETWEEN, which is the chooser's branch
         * written here for its reason: the caller slices its wait while the panel is up, so
         * this is nearly unreachable -- which is exactly why it is here. The three seconds
         * are a property of the CLOCK, and a release arriving at 3001 ms must not be refused
         * because nobody looked at 3000. */
        if (hold_row && press_value && row == press_value &&
            hold_row == press_value && hold_reached(now_ms) &&
            settings_row_live(press_value, S)) {
            if (act)
                *act = press_value;
            hold_row = 0;
            flash_on = 0;
            press_row = 0;
            press_value = 0;
            cur_value = 0;
            return MZ_FEED_TAKEN;
        }
        hold_row = 0;
        flash_on = 0;

        if (press_value && row == press_value && settings_row_live(press_value, S) &&
            !settings_row_ejects(press_value)) {
            /* The anchor and the highlight go -- the finger has LIFTED, and a cur_value left
             * standing would have the painter showing a highlight for a finger that is not
             * there. THE PANEL STAYS UP, so several settings can be flipped in one visit and
             * the value that just changed is on the glass in front of the operator. */
            press_row = 0;
            press_value = 0;
            cur_value = 0;
            if (act)
                *act = row;
            return MZ_FEED_TAKEN;
        }

        if (!press_row) {
            /* Began outside every row -- the title, the rule, the padding, the margin,
             * the glass outside the box. The only release that puts the panel away
             * without acting on anything. */
            settings_close();
            return MZ_FEED_TAKEN;
        }

        /* BEGAN ON A ROW, and either not on its value or not on that value any more. THE
         * PANEL STAYS UP, and this is the operator's "tap the value only" -- a finger that
         * landed on a label was reading, not dismissing. It is the one place this module
         * deliberately departs from the chooser's "a release that began off every button
         * closes the box".
         *
         * A USB ROW LANDS HERE WHENEVER THE HOLD DID NOT COMPLETE, which is the whole of "if
         * they release it before three seconds don't eject it": a short press on an eject row
         * sends NOTHING and leaves the panel standing, so the operator can take a fresh hold
         * or wait for the timeout. */
        press_row = 0;
        press_value = 0;
        cur_value = 0;
        return MZ_FEED_TAKEN;
    }

    if (!was_down) {
        was_down = 1;
        press_row = settings_row_at(x, y);
        press_value = settings_value_at(x, y);
        cur_value = press_value;
        /* A HOLD STARTS ONLY ON A LIVE EJECT ROW. rbp's answer is asked here, at the press --
         * and then AGAIN at the fire and at the tick, so a stick that goes away mid-hold is
         * refused by the clock and not by a stale latch. This is the chooser's rule to the
         * letter; on every other row a press is just a press, and the tap that follows is the
         * whole gesture. */
        if (press_value && settings_row_ejects(press_value) &&
            settings_row_live(press_value, S)) {
            hold_row = press_value;
            hold_since_ms = now_ms;
            flash_on = 1;         /* show the press before the first tick arrives */
        } else {
            hold_row = 0;
        }
        return MZ_FEED_TAKEN;
    }

    /* Still the same press: track which value the finger is over now. This is the HIGHLIGHT
     * and nothing else -- what fires is anchored to where the press STARTED.
     *
     * LEAVING THE VALUE CANCELS A HOLD, and it does not come back on a return: a hold is a
     * finger held still on one button, and a finger that wanders off and back is a different
     * gesture. Letting it resume would make the anchor something the finger could leave and
     * return to at no cost, which is one step from an eject the operator did not watch
     * themselves make. */
    cur_value = settings_value_at(x, y);
    if (hold_row && cur_value != hold_row) {
        hold_row = 0;
        flash_on = 0;
    }
    return MZ_FEED_TAKEN;
}
