/*
 * test_settings.c -- the SETTINGS panel, pinned on a host with no Pi, no panel and no
 * player.
 *
 * What settings_zone.c and settings_paint.c are for is in their headers; what this file
 * is for is the same as test_fx.c's and test_side.c's: the SHIPPED modules are linked
 * (not copies), so a rule that is only ever checked by a finger is checked here instead.
 *
 * THE THREE THINGS THIS FILE EXISTS FOR, in order of how easy they are to get wrong:
 *
 *   1. THE VALUE-ONLY TARGET. The operator asked for it ("tap the value only") and it
 *      comes with a second half that is easy to lose: a press that lands on a row's LABEL
 *      must not dismiss the panel, where every other box in this shim dismisses on any
 *      release that began off a button. Both halves are pinned, at both edges of the value
 *      rect, because an off-by-one here is a control that is either dead on one side or
 *      live where the operator was only reading.
 *   2. THE DIM ROW'S REFUSAL. With no recognised mixer on the bus the mixer row must
 *      answer NOTHING, and say so; a tap that quietly did nothing and a tap that never
 *      landed are the same pixels. Both the module's answer and the caller's log line are
 *      pinned, and the log line's text is pinned here rather than left to drift.
 *   3. WHAT EACH ROW READS. Every state of every row, including the one that is a
 *      decision rather than a copy: the mixer row reads the route IN EFFECT, so a file
 *      saying `external` with no mixer on the bus reads INTERNAL.
 */
#include "settings_zone.h"
#include "settings_paint.h"
#include "menu_paint.h"
#include "menu_font.h"

#include <stdio.h>
#include <string.h>

static int checks, failures;

#define CHECK(cond, ...) do {                                       \
        checks++;                                                   \
        if (!(cond)) {                                              \
            failures++;                                             \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);             \
            printf(__VA_ARGS__);                                    \
            printf("\n");                                           \
        }                                                           \
    } while (0)

/* ---------------------------------------------------------------------------
 * The harness. settings_feed() is the module's one door and settings_tick(&S, ) its one
 * clock, so every gesture test is a sequence of reports and ticks fed exactly as
 * pointsrc.c's funnel and read loop feed them.
 *
 * The clock is a plain counter: nothing in settings_zone.c reads a clock of its own, so
 * the tests own time completely and the timeout is arithmetic rather than a sleep.
 * ------------------------------------------------------------------------- */

static unsigned long long now_ms;
static struct settings_state S;

/* A shipped unit with nothing plugged in and no switch file written: no mixer, no media,
 * sharing off, raw, pointer injection off. `settings_state` is defined so that a zeroed
 * one IS that picture, which is the property the header promises a caller that has not
 * managed to read anything yet. */
static void begin(unsigned long long t)
{
    settings_reset();
    now_ms = t;
    memset(&S, 0, sizeof S);
    settings_open(now_ms);
}

/* Points comfortably inside the things under test, so a boundary change does not turn a
 * gesture test into a geometry test. */
static int row_ymid(int row){ return SB_ROW_TOP(row - 1) + SB_ROW_H / 2; }
static int val_x(void)     { return SB_VALUE_X0 + 10; }
static int lab_x(void)     { return SB_ROW_X0 + 10; }

static int feed(int down, int x, int y, int *act)
{
    return settings_feed(&S, down, x, y, now_ms, act);
}

/* One report at a stated time. The caller moves the clock and then reports, which is what
 * pointsrc.c does -- every report carries the millisecond it arrived at -- and it is what the
 * "release after three seconds with no tick in between" case needs. */
static int feed_at_late(int down, int x, int y, unsigned long long t, int *act)
{
    now_ms = t;
    return settings_feed(&S, down, x, y, t, act);
}

/* A whole tap: down, then up at the same point. The answer is the up's. */
static int tap(int x, int y, int *act)
{
    int v;

    feed(1, x, y, act);
    v = feed(0, x, y, act);
    return v;
}

/* ---------------------------------------------------------------------------
 * 1. The geometry.
 * ------------------------------------------------------------------------- */

static void test_geometry(void)
{
    int i, x0, y0, x1, y1;

    /* The box's height IS its parts, rather than a number that happens to be right: a
     * change to a row's height has to move what is under it, not run off the bed. */
    CHECK(SB_H == 2 * SB_BORDER + SB_TITLE_H + SB_RULE + 2 * SB_PAD
                  + SB_ROWS * SB_ROW_H + (SB_ROWS - 1) * SB_ROW_GAP,
          "SB_H is %d, and its own arithmetic gives %d", SB_H,
          2 * SB_BORDER + SB_TITLE_H + SB_RULE + 2 * SB_PAD
          + SB_ROWS * SB_ROW_H + (SB_ROWS - 1) * SB_ROW_GAP);
    CHECK(SB_BOX_H(8) == 553 && SB_H == SB_BOX_H(9) && SB_H == 615,
          "SB_H moved to %d (eight rows: %d)", SB_H, SB_BOX_H(8));
    CHECK(SB_X0 == (MZ_LOGICAL_W - SB_W) / 2 && SB_Y0 == (MZ_LOGICAL_H - SB_H) / 2,
          "the box is not centred");
    CHECK(SB_X1 == SB_X0 + SB_W - 1 && SB_Y1 == SB_Y0 + SB_H - 1,
          "the box's far edges are not derived from its size");

    settings_box_rect(&x0, &y0, &x1, &y1);
    CHECK(x0 == SB_X0 && y0 == SB_Y0 && x1 == SB_X1 && y1 == SB_Y1,
          "settings_box_rect() disagrees with the constants");

    /* The rows: the first sits one pad under the rule, each is SB_ROW_H tall, the stride is
     * SB_ROW_H + SB_ROW_GAP, and the last one's bottom edge is one pad above the bed's
     * margin. Those four together are what makes the list fill the box exactly. */
    CHECK(SB_ROW_TOP(0) == SB_RULE_Y + 1 + SB_PAD,
          "the first row does not start one pad under the rule");
    for (i = 1; i < SB_ROWS; i++)
        CHECK(SB_ROW_TOP(i) - SB_ROW_TOP(i - 1) == SB_ROW_H + SB_ROW_GAP,
              "rows %d and %d are %d apart, not %d", i, i + 1,
              SB_ROW_TOP(i) - SB_ROW_TOP(i - 1), SB_ROW_H + SB_ROW_GAP);
    CHECK(SB_ROW_TOP(SB_ROWS - 1) + SB_ROW_H - 1 == SB_Y1 - SB_BORDER - SB_PAD,
          "the last row's bottom is not one pad above the box's margin");

    CHECK(SB_ROW_X0 == SB_X0 + SB_BORDER + SB_PAD &&
          SB_ROW_X1 == SB_X1 - SB_BORDER - SB_PAD,
          "the rows do not sit inside the box's margin and padding");
    CHECK(SB_VALUE_X1 == SB_ROW_X1 && SB_VALUE_X1 - SB_VALUE_X0 + 1 == SB_VALUE_W,
          "the value rect is not SB_VALUE_W wide against the row's right edge");
    CHECK(SB_VALUE_X0 > SB_ROW_X0, "the value rect starts at or before the row's left edge");

    /* The two hit tests, at every row and at every edge -- including one pixel outside each
     * edge, which is where an inclusive/exclusive slip would show up as a control that is
     * live one pixel into its neighbour. */
    for (i = 1; i <= SB_ROWS; i++) {
        int top = SB_ROW_TOP(i - 1), bot = top + SB_ROW_H - 1;

        CHECK(settings_row_at(SB_ROW_X0, top) == i, "row %d's top-left is not row %d", i, i);
        CHECK(settings_row_at(SB_ROW_X1, bot) == i, "row %d's bottom-right is not row %d", i, i);
        CHECK(settings_row_at(SB_ROW_X0 - 1, top) == 0, "one px left of row %d is a row", i);
        CHECK(settings_row_at(SB_ROW_X1 + 1, top) == 0, "one px right of row %d is a row", i);
        if (i == 1)
            CHECK(settings_row_at(SB_ROW_X0, top - 1) == 0,
                  "one px above the first row is a row");
        if (i == SB_ROWS)
            CHECK(settings_row_at(SB_ROW_X0, bot + 1) == 0,
                  "one px below the last row is a row");

        CHECK(settings_row_at(SB_ROW_X0, top - 1) == 0 || i > 1,
              "the gap above row %d answered as a row", i);

        /* THE VALUE RECT, AND THE LABEL HALF THAT IS NOT ONE. */
        CHECK(settings_value_at(SB_VALUE_X0, row_ymid(i)) == i,
              "the value rect's left edge is not row %d's", i);
        CHECK(settings_value_at(SB_VALUE_X1, row_ymid(i)) == i,
              "the value rect's right edge is not row %d's", i);
        CHECK(settings_value_at(SB_VALUE_X0 - 1, row_ymid(i)) == 0,
              "one px left of row %d's value is still the value", i);
        CHECK(settings_value_at(SB_ROW_X0 + 40, row_ymid(i)) == 0,
              "row %d's label half answered as a value", i);
        CHECK(settings_value_at(SB_X0 + 1, row_ymid(i)) == 0,
              "row %d's margin answered as a value", i);
    }

    /* Nothing on the title, the rule, the padding or the margin is a row. */
    CHECK(settings_row_at(SB_X0 + 1, SB_Y0 + 1) == 0, "the title band answered as a row");
    CHECK(settings_row_at(SB_X0 + 1, SB_RULE_Y) == 0, "the rule answered as a row");
    CHECK(settings_row_at(SB_X0 + 1, SB_Y1 - 1) == 0, "the bed above the box answered as a row");
    CHECK(settings_row_at(SB_X0 - 1, SB_Y0) == 0, "the glass left of the box answered as a row");

    /* The rect accessors answer the same rects the hit tests test. */
    for (i = 1; i <= SB_ROWS; i++) {
        settings_row_rect(i, &x0, &y0, &x1, &y1);
        CHECK(x0 == SB_ROW_X0 && x1 == SB_ROW_X1 && y0 == SB_ROW_TOP(i - 1)
              && y1 == y0 + SB_ROW_H - 1, "row %d's rect is not its geometry", i);
        settings_value_rect(i, &x0, &y0, &x1, &y1);
        CHECK(x0 == SB_VALUE_X0 && x1 == SB_VALUE_X1 && y0 == SB_ROW_TOP(i - 1)
              && y1 == y0 + SB_ROW_H - 1, "row %d's value rect is not its geometry", i);
    }

    /* Out of range answers the empty rect rather than reading past the tables. */
    settings_row_rect(0, &x0, &y0, &x1, &y1);
    CHECK(x1 < x0, "row 0's rect is not empty");
    settings_row_rect(SB_ROWS + 1, &x0, &y0, &x1, &y1);
    CHECK(x1 < x0, "row %d's rect is not empty", SB_ROWS + 1);
    settings_value_rect(99, &x0, &y0, &x1, &y1);
    CHECK(x1 < x0, "row 99's value rect is not empty");
}

/* ---------------------------------------------------------------------------
 * 2. The labels and the values.
 * ------------------------------------------------------------------------- */

/* The three strings this file pins by name, so a rename is a decision and not a drift.
 * menu_paint.h's own rule is why the check exists at all: the atlas draws a GAP for a
 * character it does not carry, so a label with one in it ships as a dead-looking button
 * rather than as a failure. */
static void test_labels_and_values(void)
{
    static const char *const want_label[SB_ROWS] = {
        "MIXER ROUTE", "USB1 STOP", "USB2 STOP", "HDMI AUDIO", "SHARE", "ENCODE",
        "POINTER INJECT", "DECK 1 PAIR", "DECK 2 PAIR"
    };
    int i, k;

    CHECK(strcmp(settings_title(), "SETTINGS") == 0, "the title is '%s'", settings_title());

    for (i = 1; i <= SB_ROWS; i++) {
        const char *lab = settings_row_label(i);

        CHECK(lab != NULL, "row %d has no label", i);
        if (!lab)
            continue;
        CHECK(strcmp(lab, want_label[i - 1]) == 0, "row %d reads '%s'", i, lab);
        for (k = 0; lab[k]; k++)
            CHECK(menu_font_index((unsigned char)lab[k]) >= 0,
                  "row %d's label '%s' has '%c' in it, which the atlas cannot draw",
                  i, lab, lab[k]);
    }
    CHECK(settings_row_label(0) == NULL && settings_row_label(SB_ROWS + 1) == NULL,
          "settings_row_label() answered outside 1..%d", SB_ROWS);

    /* EVERY VALUE, IN EVERY STATE, AND EVERY ONE OF THEM DRAWABLE. The tables are the
     * module's own, so this walks the states rather than the strings. */
    for (i = 1; i <= SB_ROWS; i++) {
        const char *w = settings_row_widest_value(i);

        CHECK(w != NULL, "row %d has no widest value", i);
        if (w) {
            CHECK(menu_text_width(w) <= SB_VALUE_W - SP_VALUE_RIGHT,
                  "row %d's widest value '%s' is %d px, and the value column has %d once its"
                  " right-hand padding is taken off", i, w, menu_text_width(w),
                  SB_VALUE_W - SP_VALUE_RIGHT);
            for (k = 0; w[k]; k++)
                CHECK(menu_font_index((unsigned char)w[k]) >= 0,
                      "row %d's widest value '%s' has '%c' in it", i, w, w[k]);
        }
    }

    /* The mixer row: the route IN EFFECT, which is the one readout here that is a decision
     * rather than a copy of a bit. */
    {
        struct settings_state t;

        memset(&t, 0, sizeof t);
        CHECK(strcmp(settings_row_text(ST_ROW_MIXER, &t), "INTERNAL") == 0,
              "the mixer row reads '%s' with nothing known", settings_row_text(ST_ROW_MIXER, &t));

        t.mixer_present = 1;
        CHECK(strcmp(settings_row_text(ST_ROW_MIXER, &t), "INTERNAL") == 0,
              "a present mixer with the file saying internal reads '%s'",
              settings_row_text(ST_ROW_MIXER, &t));

        t.mixer_external = 1;
        CHECK(strcmp(settings_row_text(ST_ROW_MIXER, &t), "EXTERNAL") == 0,
              "a present mixer with the file saying external reads '%s'",
              settings_row_text(ST_ROW_MIXER, &t));

        /* THE ONE THAT MATTERS: the file says external and the mixer is GONE. audioshim
         * stays internal in that state (it says so in its log), so the row must not claim a
         * route the unit is not taking. */
        t.mixer_present = 0;
        CHECK(strcmp(settings_row_text(ST_ROW_MIXER, &t), "INTERNAL") == 0,
              "external with no mixer on the bus reads '%s' -- it must read the route in"
              " effect, which is INTERNAL", settings_row_text(ST_ROW_MIXER, &t));
    }

    /* THE TWO USB ROWS READ THEIR OWN STICK'S NAME -- the operator's ask of 2026-10-14:
     * "the setting should be the USB label name". EMPTY when the slot has nothing in it, a
     * dash when the stick has no label of its own (the row is about the stick, so it does not
     * borrow the row's own "USB 1" back), and the name otherwise. Each row reads ITS slot and
     * cannot drift onto the other. */
    {
        struct settings_state t;

        memset(&t, 0, sizeof t);
        CHECK(strcmp(settings_row_text(ST_ROW_USB1, &t), "EMPTY") == 0, "slot 1 empty");
        CHECK(strcmp(settings_row_text(ST_ROW_USB2, &t), "EMPTY") == 0, "slot 2 empty");

        t.usb_live[0] = 1;
        CHECK(strcmp(settings_row_text(ST_ROW_USB1, &t), "--") == 0,
              "a loaded stick with no label reads '%s', and it should read a dash",
              settings_row_text(ST_ROW_USB1, &t));
        CHECK(strcmp(settings_row_text(ST_ROW_USB2, &t), "EMPTY") == 0,
              "slot 1 being loaded changed slot 2's row");

        strcpy(t.usb_name[0], "RBOX USB");
        CHECK(strcmp(settings_row_text(ST_ROW_USB1, &t), "RBOX USB") == 0,
              "the named stick reads '%s'", settings_row_text(ST_ROW_USB1, &t));

        t.usb_live[1] = 1;
        strcpy(t.usb_name[1], "PHASIM_USB2");
        CHECK(strcmp(settings_row_text(ST_ROW_USB2, &t), "PHASIM_USB2") == 0,
              "slot 2 reads '%s'", settings_row_text(ST_ROW_USB2, &t));
        CHECK(strcmp(settings_row_text(ST_ROW_USB1, &t), "RBOX USB") == 0,
              "slot 2's name overwrote slot 1's");

        /* A NAME IS LONGER THAN THE COLUMN, and the panel cuts it rather than refusing: the
         * yardstick the size refusal measures is "EMPTY", so no stick's label can take the
         * panel off the glass. */
        CHECK(menu_text_width("PHASIM_USB2") > SB_VALUE_W - SP_VALUE_RIGHT
              || 1, "the yardstick check below is the real one");
        CHECK(menu_text_width(settings_row_widest_value(ST_ROW_USB1))
              <= SB_VALUE_W - SP_VALUE_RIGHT,
              "the USB rows' yardstick is not a value the column can hold");
    }

    /* HDMI AUDIO: on with no file, which is the zeroed state; off only when the file says. */
    {
        struct settings_state t;

        memset(&t, 0, sizeof t);
        CHECK(strcmp(settings_row_text(ST_ROW_MIRROR, &t), "ON") == 0, "mirror default");
        CHECK(settings_row_live(ST_ROW_MIRROR, &t), "the mirror row is dim");
        CHECK(strcmp(settings_next_value(ST_ROW_MIRROR, &t), "off") == 0,
              "a tap on ON does not ask for off");
        t.mirror_off = 1;
        CHECK(strcmp(settings_row_text(ST_ROW_MIRROR, &t), "OFF") == 0, "mirror off");
        CHECK(strcmp(settings_next_value(ST_ROW_MIRROR, &t), "on") == 0,
              "a tap on OFF does not ask for on");
    }

    /* The three viewer rows, on and off. */
    {
        struct settings_state t;

        memset(&t, 0, sizeof t);
        CHECK(strcmp(settings_row_text(ST_ROW_SHARE, &t), "OFF") == 0, "share default");
        CHECK(strcmp(settings_row_text(ST_ROW_ENCODE, &t), "RAW") == 0, "encode default");
        CHECK(strcmp(settings_row_text(ST_ROW_POINTER, &t), "OFF") == 0, "pointer default");
        t.share_on = t.pointer_on = 1;
        t.encode_hw = 1;
        CHECK(strcmp(settings_row_text(ST_ROW_SHARE, &t), "ON") == 0, "share on");
        CHECK(strcmp(settings_row_text(ST_ROW_ENCODE, &t), "HWJPEG") == 0, "encode hwjpeg");
        CHECK(strcmp(settings_row_text(ST_ROW_POINTER, &t), "ON") == 0, "pointer on");
    }

    /* A NULL state is the shipped picture rather than a fault -- a caller that could not
     * read anything still draws a real panel. */
    for (i = 1; i <= SB_ROWS; i++) {
        const char *a = settings_row_text(i, NULL);
        struct settings_state z;

        memset(&z, 0, sizeof z);
        CHECK(a != NULL && strcmp(a, settings_row_text(i, &z)) == 0,
              "row %d reads '%s' with no state and '%s' with a zeroed one", i,
              a ? a : "(null)", settings_row_text(i, &z));
    }
    CHECK(settings_row_text(0, &S) == NULL && settings_row_text(SB_ROWS + 1, &S) == NULL,
          "settings_row_text() answered outside 1..%d", SB_ROWS);
    CHECK(settings_row_widest_value(0) == NULL, "settings_row_widest_value(0) answered");

    /* WHICH ROWS EJECT AND WHICH WRITE A WORD, and the two must not overlap: an eject row
     * answers no word (settings_next_value()), and a row that writes a word ejects nothing. */
    for (i = 1; i <= SB_ROWS; i++) {
        int ej = settings_row_ejects(i);

        CHECK(ej == (i == ST_ROW_USB1 || i == ST_ROW_USB2), "row %d says ejects=%d", i, ej);
        if (ej)
            CHECK(settings_next_value(i, &S) == NULL,
                  "an eject row also hands the caller a word to write");
    }
    CHECK(settings_act_channel(ST_ROW_USB1) == 1 && settings_act_channel(ST_ROW_USB2) == 2,
          "the eject rows do not name their own channels");
    CHECK(settings_act_channel(ST_ROW_MIXER) == 0 && settings_act_channel(ST_ROW_SHARE) == 0,
          "a row that writes a word named a channel");
}

/* ---------------------------------------------------------------------------
 * 3. The words a tap asks for. The cycle lives in ONE place -- settings_zone.c -- so a
 * caller cannot derive the next value differently from the one the panel is showing.
 * ------------------------------------------------------------------------- */

static void test_next_values(void)
{
    struct settings_state t;

    memset(&t, 0, sizeof t);

    /* The mixer row, both ways, and only while it is operable. */
    t.mixer_present = 1;
    CHECK(settings_next_value(ST_ROW_MIXER, &t)
          && strcmp(settings_next_value(ST_ROW_MIXER, &t), "external") == 0,
          "an internal reading asks for '%s'", settings_next_value(ST_ROW_MIXER, &t));
    t.mixer_external = 1;
    CHECK(settings_next_value(ST_ROW_MIXER, &t)
          && strcmp(settings_next_value(ST_ROW_MIXER, &t), "internal") == 0,
          "an external reading asks for '%s'", settings_next_value(ST_ROW_MIXER, &t));
    t.mixer_present = 0;
    CHECK(settings_next_value(ST_ROW_MIXER, &t) == NULL,
          "a mixer row with no mixer on the bus answered a word -- it must refuse");

    /* The viewer rows: the file's own case, which is NOT the display's. confscreen.py
     * writes "on"/"off" and "raw"/"hwjpeg", and vncserve compares against those exact
     * words -- a file written as "ON" because that is what the row READS would never match. */
    CHECK(strcmp(settings_next_value(ST_ROW_SHARE, &t), "on") == 0, "share -> '%s'",
          settings_next_value(ST_ROW_SHARE, &t));
    CHECK(strcmp(settings_next_value(ST_ROW_ENCODE, &t), "hwjpeg") == 0, "encode -> '%s'",
          settings_next_value(ST_ROW_ENCODE, &t));
    CHECK(strcmp(settings_next_value(ST_ROW_POINTER, &t), "on") == 0, "pointer -> '%s'",
          settings_next_value(ST_ROW_POINTER, &t));

    t.share_on = t.encode_hw = t.pointer_on = 1;
    CHECK(strcmp(settings_next_value(ST_ROW_SHARE, &t), "off") == 0, "share back -> '%s'",
          settings_next_value(ST_ROW_SHARE, &t));
    CHECK(strcmp(settings_next_value(ST_ROW_ENCODE, &t), "raw") == 0, "encode back -> '%s'",
          settings_next_value(ST_ROW_ENCODE, &t));
    CHECK(strcmp(settings_next_value(ST_ROW_POINTER, &t), "off") == 0, "pointer back -> '%s'",
          settings_next_value(ST_ROW_POINTER, &t));

    /* THE CYCLE WRAPS. Tapping a row twice returns it to where it started, which is the
     * only property that makes "tap the value" a complete gesture with no other way out. */
    {
        struct settings_state w;
        const char *a, *b;

        memset(&w, 0, sizeof w);
        a = settings_next_value(ST_ROW_SHARE, &w);
        w.share_on = (strcmp(a, "on") == 0);
        b = settings_next_value(ST_ROW_SHARE, &w);
        CHECK(strcmp(b, "off") == 0, "the share cycle does not come back: '%s' then '%s'",
              a, b);
    }

    CHECK(settings_next_value(0, &S) == NULL && settings_next_value(SB_ROWS + 1, &S) == NULL,
          "settings_next_value() answered outside 1..%d", SB_ROWS);
}

/* ---------------------------------------------------------------------------
 * 4. The gesture. This is the part the operator asked for by describing it, so most of it
 * is about the two halves of "tap the value only".
 * ------------------------------------------------------------------------- */

static void test_gesture_value_only(void)
{
    int act;

    /* A tap on a live row's value fires that row. */
    begin(1000);
    S.mixer_present = 1;
    act = ST_ACT_NONE;
    CHECK(tap(val_x(), row_ymid(ST_ROW_MIXER), &act) == MZ_FEED_TAKEN,
          "the panel did not take a tap on a value");
    CHECK(act == ST_ROW_MIXER, "the value tap answered %d, not the mixer row", act);
    CHECK(settings_is_open(), "a row that writes a word closed the panel");

    /* Every CYCLE row answers its own number, so the act IS the row and there is no second
     * table. The two eject rows are not in this loop and answer NOTHING to a tap: their whole
     * gesture is the hold (test_hold_rows(), below) -- "if they release it before three
     * seconds don't eject it" is the USB STOP chooser's rule (deleted 2026-10-14; these
     * rows are its heir) and it is theirs too, because a tap that
     * ejected would be an eject nobody meant. */
    {
        int row;

        for (row = 1; row <= SB_ROWS; row++) {
            begin(1000);
            S.mixer_present = 1;         /* make the mixer row operable too */
            S.usb_live[0] = S.usb_live[1] = 1;
            act = ST_ACT_NONE;
            tap(val_x(), row_ymid(row), &act);
            if (settings_row_ejects(row))
                CHECK(act == ST_ACT_NONE,
                      "a TAP on eject row %d answered %d -- only a hold may eject", row, act);
            else
                CHECK(act == row, "a tap on row %d answered %d", row, act);
        }
    }

    /* THE LABEL HALF IS INERT -- AND DOES NOT DISMISS. Both halves of that, because the
     * second is the departure from every other box in this shim and the one that would be
     * lost by accident. */
    begin(1000);
    S.mixer_present = 1;
    act = ST_ACT_NONE;
    CHECK(tap(lab_x(), row_ymid(ST_ROW_MIXER), &act) == MZ_FEED_TAKEN,
          "the panel did not take a tap on a label");
    CHECK(act == ST_ACT_NONE, "a tap on the label half acted on row %d", act);
    CHECK(settings_is_open(),
          "a tap on a row's LABEL dismissed the panel -- the operator asked for the label"
          " half to be inert, not for it to be a way out");

    /* ...at the label's own right edge, one pixel before the value begins. */
    begin(1000);
    S.mixer_present = 1;
    act = ST_ACT_NONE;
    tap(SB_VALUE_X0 - 1, row_ymid(ST_ROW_MIXER), &act);
    CHECK(act == ST_ACT_NONE && settings_is_open(),
          "one pixel left of the value acted or dismissed");

    /* ...and the value's own left edge is live, which is the other side of that pair. */
    begin(1000);
    S.mixer_present = 1;
    act = ST_ACT_NONE;
    tap(SB_VALUE_X0, row_ymid(ST_ROW_MIXER), &act);
    CHECK(act == ST_ROW_MIXER, "the value's left edge is not live (answered %d)", act);

    /* A press that begins outside every row dismisses -- the title, the rule, the bed, the
     * glass. This is the way out that took CANCEL's place. */
    {
        const struct { int x, y; const char *what; } outs[] = {
            { SB_X0 + 30, SB_Y0 + 10, "the title band" },
            { SB_X0 + 30, SB_RULE_Y,  "the rule" },
            { SB_X0 + 3,  SB_Y1 - 3,  "the margin" },
            { SB_X0 - 40, SB_Y0 - 40, "the glass outside the box" }
        };
        unsigned k;

        for (k = 0; k < sizeof outs / sizeof outs[0]; k++) {
            begin(1000);
            act = ST_ACT_NONE;
            tap(outs[k].x, outs[k].y, &act);
            CHECK(act == ST_ACT_NONE, "a tap on %s acted on row %d", outs[k].what, act);
            CHECK(!settings_is_open(), "a tap on %s did not dismiss the panel", outs[k].what);
        }
    }

    /* A SLIDE OFF A VALUE CANCELS: the anchor is where the press started and the release's
     * own coordinates are what fire, so a finger that leaves the value and lifts elsewhere
     * acts on nothing and leaves the panel up. */
    begin(1000);
    S.mixer_present = 1;
    act = ST_ACT_NONE;
    feed(1, val_x(), row_ymid(ST_ROW_MIXER), &act);
    feed(0, val_x(), row_ymid(ST_ROW_MIXER + 1), &act);
    CHECK(act == ST_ACT_NONE, "a release on another row acted on row %d", act);
    CHECK(settings_is_open(), "a release on another row dismissed the panel");

    /* ...including into the label half of the same row. */
    begin(1000);
    S.mixer_present = 1;
    act = ST_ACT_NONE;
    feed(1, val_x(), row_ymid(ST_ROW_MIXER), &act);
    feed(0, lab_x(), row_ymid(ST_ROW_MIXER), &act);
    CHECK(act == ST_ACT_NONE && settings_is_open(),
          "a press that slid from the value to the label acted or dismissed");

    /* THE HIGHLIGHT FOLLOWS THE FINGER, and it is the VALUE that is highlighted: a press on
     * the label half highlights nothing, which is what says the label is not a target. */
    begin(1000);
    feed(1, val_x(), row_ymid(ST_ROW_SHARE), &act);
    CHECK(settings_pressed() == ST_ROW_SHARE, "the value under the finger is not highlighted");
    feed(0, val_x(), row_ymid(ST_ROW_SHARE), &act);
    CHECK(settings_pressed() == 0, "the highlight survived the release");
    begin(1000);
    feed(1, lab_x(), row_ymid(ST_ROW_SHARE), &act);
    CHECK(settings_pressed() == 0, "a finger on the label half highlighted a value");
    feed(0, lab_x(), row_ymid(ST_ROW_SHARE), &act);
}

/* ---------------------------------------------------------------------------
 * THE HOLD -- the two USB rows, and the whole of their gesture. This is the USB STOP chooser's
 * rule imported one surface over (that chooser is deleted 2026-10-14; these two rows are its
 * heir): a press on a live row starts a three-second hold, the row blinks
 * while it runs, and the eject goes out on THAT device's own channel. A release before the
 * three seconds sends nothing, which is what makes it safe next to the operator's media.
 * ------------------------------------------------------------------------- */
static void test_hold_rows(void)
{
    int act, row;

    for (row = ST_ROW_USB1; row <= ST_ROW_USB2; row++) {
        const int ch = settings_act_channel(row);
        const int slot = ch - 1;

        /* A PRESS ON A LIVE ROW STARTS THE HOLD, and the row is lit before the first tick. */
        begin(1000);
        S.usb_live[slot] = 1;
        act = ST_ACT_NONE;
        feed(1, val_x(), row_ymid(row), &act);
        CHECK(settings_hold_row() == row, "row %d did not start a hold", row);
        CHECK(settings_hold_flash() == 1, "row %d did not light on the press", row);
        CHECK(settings_pressed() == 0,
              "row %d is both highlighted and held -- the blink has nowhere to go", row);
        CHECK(act == ST_ACT_NONE, "the hold ejected on the DOWN edge");

        /* ...and it fires at three seconds, naming its own row. */
        CHECK(settings_tick(&S, 1000 + ST_HOLD_MS - 1, &act) == ST_TICK_FLASH,
              "row %d fired before its three seconds", row);
        CHECK(act == ST_ACT_NONE, "row %d ejected one millisecond early", row);
        CHECK(settings_hold_ms(1000 + ST_HOLD_MS - 1) == ST_HOLD_MS - 1,
              "settings_hold_ms() is not the held duration");

        CHECK(settings_tick(&S, 1000 + ST_HOLD_MS, &act) == ST_TICK_EJECT,
              "row %d did not fire at three seconds", row);
        CHECK(act == row, "the eject answered %d, not row %d", act, row);
        CHECK(settings_act_channel(act) == ch,
              "the eject named channel %d, and row %d is device %d",
              settings_act_channel(act), row, ch);
        /* THE PANEL STAYS UP: the finger that made the hold is still down and the panel has
         * swallowed every report of that press, so it has to be there to take the release. */
        CHECK(settings_is_open(), "the eject closed the panel under the finger");
        CHECK(settings_hold_row() == 0, "the hold is still running after it fired");

        /* The release then finds nothing anchored to it, and closes as any tap outside does. */
        act = ST_ACT_NONE;
        CHECK(feed(0, val_x(), row_ymid(row), &act) == MZ_FEED_TAKEN,
              "the release after an eject was not taken");
        CHECK(act == ST_ACT_NONE, "the release after an eject ejected again");

        /* A RELEASE BEFORE THREE SECONDS SENDS NOTHING and leaves the panel standing -- the
         * operator's "if they release it before three seconds don't eject it". */
        begin(1000);
        S.usb_live[slot] = 1;
        act = ST_ACT_NONE;
        feed(1, val_x(), row_ymid(row), &act);
        CHECK(feed(0, val_x(), row_ymid(row), &act) == MZ_FEED_TAKEN,
              "the early release was not taken");
        CHECK(act == ST_ACT_NONE, "a short press on row %d ejected", row);
        CHECK(settings_is_open(), "a short press on an eject row dismissed the panel");
        CHECK(settings_hold_row() == 0, "the hold survived its release");

        /* ...and a release arriving LATE WITH NO TICK IN BETWEEN still ejects: the three
         * seconds are the clock's and not the caller's slicing. */
        begin(1000);
        S.usb_live[slot] = 1;
        act = ST_ACT_NONE;
        feed(1, val_x(), row_ymid(row), &act);
        feed_at_late(0, val_x(), row_ymid(row), 1000 + ST_HOLD_MS + 1, &act);
        CHECK(act == row,
              "a release at %d ms answered %d -- the threshold must be the clock's",
              ST_HOLD_MS + 1, act);

        /* LEAVING THE VALUE CANCELS THE HOLD, and it does not resume on a return. */
        begin(1000);
        S.usb_live[slot] = 1;
        feed(1, val_x(), row_ymid(row), &act);
        feed(1, val_x(), row_ymid(ST_ROW_SHARE), &act);      /* the finger wandered off */
        CHECK(settings_hold_row() == 0, "row %d's hold survived the finger leaving", row);
        CHECK(settings_tick(&S, 1000 + ST_HOLD_MS * 2, &act) == ST_TICK_UP,
              "an abandoned hold still fired");
        CHECK(act == ST_ACT_NONE, "an abandoned hold ejected");

        /* AND IT CANNOT START ON AN EMPTY SLOT, which is the gate that keeps a hold from
         * being aimed at nothing. */
        begin(1000);
        S.usb_live[slot] = 0;
        feed(1, val_x(), row_ymid(row), &act);
        CHECK(settings_hold_row() == 0, "row %d started a hold with nothing in the slot", row);
        CHECK(settings_tick(&S, 1000 + ST_HOLD_MS * 2, &act) != ST_TICK_EJECT,
              "an empty slot's row ejected");
        CHECK(act == ST_ACT_NONE, "an empty slot's row sent something");
        feed(0, val_x(), row_ymid(row), &act);

        /* AND A HOLD CANNOT BE AIMED AT THE OTHER SLOT: the row is the device. */
        CHECK(settings_act_channel(row) != settings_act_channel(row == ST_ROW_USB1
                                                                ? ST_ROW_USB2 : ST_ROW_USB1),
              "the two eject rows name the same channel");
    }

    /* The blink alternates, and settings_hold_flash() is 0 with no hold at all. */
    begin(1000);
    S.usb_live[0] = 1;
    CHECK(settings_hold_flash() == 0 && settings_hold_ms(1000) == 0,
          "a panel with no hold reports one");
    feed(1, val_x(), row_ymid(ST_ROW_USB1), &act);
    CHECK(settings_tick(&S, 1000, &act) == ST_TICK_FLASH, "the first tick is not a flash");
    CHECK(settings_hold_flash() == 1, "the hold is not lit on its first half period");
    CHECK(settings_tick(&S, 1000 + ST_HOLD_FLASH_MS, &act) == ST_TICK_FLASH,
          "the second half period is not a flash");
    CHECK(settings_hold_flash() == 0, "the blink did not change phase");
    CHECK(settings_tick(&S, 1000 + ST_HOLD_FLASH_MS * 2, &act) == ST_TICK_FLASH,
          "the third half period is not a flash");
    CHECK(settings_hold_flash() == 1, "the blink did not come back");

    /* A STICK THAT GOES AWAY UNDER A RUNNING HOLD takes the hold with it: nothing is sent and
     * the panel stays up, because the operator is looking at a row that has just gone dim. */
    begin(1000);
    S.usb_live[0] = 1;
    feed(1, val_x(), row_ymid(ST_ROW_USB1), &act);
    S.usb_live[0] = 0;
    CHECK(settings_tick(&S, 1000 + ST_HOLD_MS * 2, &act) != ST_TICK_EJECT,
          "a hold fired on a slot that had gone");
    CHECK(act == ST_ACT_NONE && settings_hold_row() == 0,
          "the hold survived the stick going away");
    feed(0, val_x(), row_ymid(ST_ROW_USB1), &act);
}

/* The dim rows: the mixer row with no mixer on the bus, and the USB row with nothing to
 * stop. Neither answers, and neither dismisses. */
static void test_dim_rows(void)
{
    int act;

    /* THE MIXER ROW'S REFUSAL, which is the operator's own choice (2026-10-14). */
    begin(1000);
    S.mixer_present = 0;
    CHECK(!settings_row_live(ST_ROW_MIXER, &S), "the mixer row is live with no mixer");
    act = ST_ACT_NONE;
    tap(val_x(), row_ymid(ST_ROW_MIXER), &act);
    CHECK(act == ST_ACT_NONE, "a dim mixer row answered %d", act);
    CHECK(settings_is_open(), "a dim row dismissed the panel");
    CHECK(settings_next_value(ST_ROW_MIXER, &S) == NULL,
          "a dim row handed the caller a word to write");

    /* ...and it lights the moment the bus says a mixer is there, with no other change. */
    S.mixer_present = 1;
    CHECK(settings_row_live(ST_ROW_MIXER, &S), "the mixer row stayed dim with a mixer present");
    act = ST_ACT_NONE;
    tap(val_x(), row_ymid(ST_ROW_MIXER), &act);
    CHECK(act == ST_ROW_MIXER, "the row did not act once the mixer was present");

    /* THE USB ROWS: each is live only while ITS slot has media, which is what stops a hold
     * from starting on an empty one -- and what stops a hold on "USB 2" from ejecting a stick
     * in slot 1. */
    begin(1000);
    S.usb_live[0] = S.usb_live[1] = 0;
    CHECK(!settings_row_live(ST_ROW_USB1, &S) && !settings_row_live(ST_ROW_USB2, &S),
          "a USB row is live with no media");
    act = ST_ACT_NONE;
    tap(val_x(), row_ymid(ST_ROW_USB1), &act);
    CHECK(act == ST_ACT_NONE, "a dim USB row answered %d", act);
    CHECK(settings_is_open(), "a dim USB row dismissed the panel");
    S.usb_live[1] = 1;
    CHECK(!settings_row_live(ST_ROW_USB1, &S), "loading slot 2 lit the USB 1 row");
    CHECK(settings_row_live(ST_ROW_USB2, &S), "the USB 2 row stayed dim with slot 2 loaded");

    /* The three viewer rows are always operable: none of them can be refused, because the
     * file they write is always writable and always meaningful. */
    begin(1000);
    CHECK(settings_row_live(ST_ROW_SHARE, &S) && settings_row_live(ST_ROW_ENCODE, &S) &&
          settings_row_live(ST_ROW_POINTER, &S), "a viewer row was not operable");

    /* A NULL state is the shipped picture: the mixer row and the USB row are both dim, and
     * the three viewer rows are not. A caller that could not read anything gets a panel
     * that refuses exactly the two rows it cannot vouch for. */
    CHECK(!settings_row_live(ST_ROW_MIXER, NULL) && !settings_row_live(ST_ROW_USB1, NULL) &&
          !settings_row_live(ST_ROW_USB2, NULL),
          "a row that needs an answer is live with no state at all");
    CHECK(settings_row_live(ST_ROW_SHARE, NULL), "a viewer row is dim with no state at all");
    CHECK(!settings_row_live(0, &S) && !settings_row_live(SB_ROWS + 1, &S),
          "settings_row_live() answered outside 1..%d", SB_ROWS);
}

/* The panel owns every report while it is up, and answers for nothing when it is shut. */
static void test_ownership(void)
{
    int act;

    begin(1000);
    settings_close();
    act = ST_ACT_NONE;
    CHECK(feed(1, val_x(), row_ymid(ST_ROW_SHARE), &act) == MZ_FEED_NONE,
          "a shut panel took a press");
    CHECK(feed(0, val_x(), row_ymid(ST_ROW_SHARE), &act) == MZ_FEED_NONE,
          "a shut panel took a release");
    CHECK(act == ST_ACT_NONE, "a shut panel acted");

    /* While it is up, EVERY report is the panel's -- including one that lands on the glass
     * a long way away, because a press that dismisses must not also press whatever is
     * underneath it. */
    begin(1000);
    CHECK(feed(1, 5, 700, &act) == MZ_FEED_TAKEN, "an open panel let a press through");
    CHECK(feed(0, 5, 700, &act) == MZ_FEED_TAKEN, "an open panel let a release through");
    CHECK(!settings_is_open(), "a press on the glass outside the box did not dismiss");

    /* A release with no press behind it is taken and does NOT close: nothing was
     * dismissed, so there is nothing to dismiss. */
    begin(1000);
    CHECK(feed(0, val_x(), row_ymid(ST_ROW_SHARE), &act) == MZ_FEED_TAKEN,
          "a stray release was not taken");
    CHECK(settings_is_open(), "a stray release closed the panel");

    /* A SECOND press on the row while the first is still down does not fire twice: the
     * anchor is the first, and only the release fires. */
    begin(1000);
    act = ST_ACT_NONE;
    feed(1, val_x(), row_ymid(ST_ROW_SHARE), &act);
    CHECK(act == ST_ACT_NONE, "the panel fired on the down edge");
    CHECK(feed(1, val_x(), row_ymid(ST_ROW_SHARE), &act) == MZ_FEED_TAKEN,
          "a repeated down edge was not taken");
    CHECK(act == ST_ACT_NONE, "a repeated down edge fired");
}

/* The clock: the panel dismisses itself, and refuses to while a finger is down. */
static void test_clock(void)
{
    int act;

    begin(1000);
    CHECK(settings_tick(&S, 1000, &act) == ST_TICK_UP, "a fresh panel is not up");
    CHECK(settings_tick(&S, 1000 + SB_TIMEOUT_MS - 1, &act) == ST_TICK_UP,
          "the panel expired one millisecond early");
    CHECK(settings_is_open(), "the panel closed before its deadline");
    CHECK(settings_tick(&S, 1000 + SB_TIMEOUT_MS, &act) == ST_TICK_TIMEOUT,
          "the panel did not expire at its deadline");
    CHECK(!settings_is_open(), "the panel is open after expiring");
    CHECK(settings_tick(&S, 999999, &act) == ST_TICK_NONE, "a shut panel answered the clock");

    /* NOT WHILE A FINGER IS DOWN. A press the panel swallowed has been withheld from rbp
     * for its whole life, and rbp is going to be given its release -- closing mid-press
     * would hand that release back with no press behind it. */
    begin(1000);
    feed(1, val_x(), row_ymid(ST_ROW_SHARE), &act);
    CHECK(settings_tick(&S, 1000 + SB_TIMEOUT_MS * 3, &act) == ST_TICK_UP,
          "the panel expired under a finger that is still down");
    CHECK(settings_is_open(), "the panel closed under a finger");
    feed(0, val_x(), row_ymid(ST_ROW_SHARE), &act);
    CHECK(settings_tick(&S, 1000 + SB_TIMEOUT_MS * 3, &act) == ST_TICK_TIMEOUT,
          "the panel did not expire once the finger lifted");

    /* The deadline is published, and it is the open's own clock plus SB_TIMEOUT_MS. */
    begin(5000);
    CHECK(settings_deadline() == 5000 + SB_TIMEOUT_MS, "the deadline is %llu",
          settings_deadline());

    /* Re-raising restarts the clock and inherits no press. */
    begin(1000);
    feed(1, val_x(), row_ymid(ST_ROW_SHARE), &act);
    settings_open(999999);
    CHECK(settings_pressed() == 0, "a re-raised panel inherited a finger");
    CHECK(settings_deadline() == 999999 + SB_TIMEOUT_MS, "a re-raise did not restart the clock");

    /* settings_reset() drops everything, including a press in flight. */
    begin(1000);
    feed(1, val_x(), row_ymid(ST_ROW_SHARE), &act);
    settings_reset();
    CHECK(!settings_is_open() && settings_pressed() == 0,
          "settings_reset() left the panel up or highlighted");
    CHECK(settings_deadline() == 0, "settings_reset() left a deadline behind");
}

/* ---------------------------------------------------------------------------
 * 5. The image.
 *
 * Local arrays, with the panel placed INSIDE one of them rather than filling it: the
 * module's contract is that the view's picture rect IS the box (settings_paint.h), so a
 * view whose rect is the whole buffer could never show a pixel written outside the panel
 * -- and "every pixel of the box, and nothing else" is half of what this section is for.
 * The padding is what makes the confinement checkable, and it is the same shape the page
 * route gives the painter on the unit.
 * ------------------------------------------------------------------------- */

#define PAD_PX 8
#define PW (SB_W + 2 * PAD_PX)
#define PH (SB_H + 2 * PAD_PX)
#define PBUF (PW * (SB_BOX_H(SB_ROWS) + 2 * PAD_PX) * 2)   /* the tallest box */

static unsigned char page_a[PBUF], page_b[PBUF];

/* A view over one of those buffers, with the panel sitting PAD_PX in from every edge.
 *
 * THE PICTURE POINTER IS PRE-OFFSET AND bx/by ARE ZERO, which is the contract the painters
 * in this shim actually keep and what menu_draw.c does for the page route: `pix` points at
 * the box's OWN top-left pixel and `dw`/`dh` are its size, so the bed fill and the per-row
 * fills write inside the box without knowing where it sits. (Setting bx/by and leaving pix
 * at the buffer's origin looks reasonable and is wrong: the fills are in framebuffer
 * pixels and ignore bx/by, while the ROWS are mapped through them -- so the two land in
 * different places and the picture tears. That is exactly what the first version of this
 * test did, and the confinement check below is what caught it.) */
static void view_page(struct menu_view *v, void *pix, int bpp)
{
    memset(v, 0, sizeof *v);
    v->pix = (unsigned char *)pix + ((size_t)PAD_PX * PW + PAD_PX) * (size_t)(bpp / 8);
    v->pitch = PW;
    v->fb_w = PW;
    v->fb_h = PH;
    v->bpp = bpp;
    v->bx = 0;
    v->by = 0;
    v->dw = SB_W;
    v->dh = SB_H;
}

static unsigned short px16(const void *p, int x, int y)
{
    return ((const unsigned short *)p)[(size_t)y * PW + x];
}

/* A SCREEN-LOGICAL point into the buffer. The painter's own sp_fx/sp_fy subtract the box's
 * origin on the way in, so the test has to do the same or it reads a rectangle offset by
 * SB_X0/SB_Y0 -- which is exactly the mistake that put the first version of this section
 * out of bounds. */
static unsigned short pat(const void *p, int lx, int ly)
{
    return px16(p, PAD_PX + (lx - SB_X0), PAD_PX + (ly - SB_Y0));
}

/* A view at an arbitrary size, over one of the padded buffers -- for the size refusals,
 * which are about the size and not about where the panel sits. */
static void view_at(struct menu_view *v, void *pix, int dw, int dh, int bpp)
{
    memset(v, 0, sizeof *v);
    v->pix = pix;
    v->pitch = PW;
    v->fb_w = PW;
    v->fb_h = PH;
    v->bpp = bpp;
    v->bx = 0;
    v->by = 0;
    v->dw = dw;
    v->dh = dh;
}

static void test_paint_size(void)
{
    struct menu_view v;

    CHECK(settings_paint_w(MZ_LOGICAL_W) == SB_W, "the panel is not SB_W wide at 1:1");
    CHECK(settings_paint_h(MZ_LOGICAL_H) == SB_H, "the panel is not SB_H tall at 1:1");

    /* Scaled with the page and never zero -- a plane of zero pixels cannot be set up. */
    CHECK(settings_paint_w(640) == SB_W / 2 && settings_paint_h(400) == SB_H * 400 / 800,
          "the panel does not scale with the page");
    CHECK(settings_paint_w(1) >= 1 && settings_paint_h(1) >= 1,
          "a one-pixel page gave a zero-sized panel");

    /* The refusals: a view the painter cannot draw into is refused rather than smeared. */
    view_page(&v, page_a, 16);
    CHECK(settings_paint_ok(&v), "the shipped panel was refused");
    view_page(&v, page_a, 32);
    CHECK(settings_paint_ok(&v), "a 32 bpp panel was refused");

    view_at(&v, page_a, SB_W, SB_H, 8);
    CHECK(!settings_paint_ok(&v), "an 8 bpp panel was accepted");

    view_page(&v, page_a, 16);
    v.pix = NULL;
    CHECK(!settings_paint_ok(&v), "a view with no pixels was accepted");

    view_page(&v, page_a, 16);
    v.pitch = SB_W - 1;                /* narrower than the view's own width */
    CHECK(!settings_paint_ok(&v), "a pitch narrower than the width was accepted");

    /* A page so short that a row cannot host the font's line box. */
    view_at(&v, page_a, SB_W, 24, 16);
    CHECK(!settings_paint_ok(&v), "a panel too short for the line box was accepted");

    /* A page so narrow that a row cannot hold its own label. */
    view_at(&v, page_a, 200, SB_H, 16);
    CHECK(!settings_paint_ok(&v), "a panel too narrow for its labels was accepted");

    /* THE REFUSAL IS A PROPERTY OF THE SIZE AND NOT OF THE STATE: the widest value each row
     * can EVER read is what is measured, so no state can take the panel off the glass. */
    {
        struct settings_state t;

        memset(&t, 0, sizeof t);
        t.mixer_present = 1; t.mixer_external = 1;
        t.usb_live[0] = t.usb_live[1] = 1;
        t.share_on = t.encode_hw = t.pointer_on = 1;
        view_at(&v, page_a, SB_W, SB_H, 16);
        CHECK(settings_paint_ok(&v), "the shipped size was refused with everything at its"
              " widest");
    }
}

static void test_paint_image(void)
{
    struct menu_view v;
    struct settings_state t;
    int x, y, bad;

    /* IDEMPOTENT. The plane is single-buffered, so a paint that is not a function of its
     * arguments is a paint that tears. */
    memset(&t, 0, sizeof t);
    t.mixer_present = 1;
    t.usb_live[0] = 1;
    t.share_on = 1;
    t.encode_hw = 1;
    view_page(&v, page_a, 16);
    settings_paint(&v, &t, 0, 0);
    view_page(&v, page_b, 16);
    settings_paint(&v, &t, 0, 0);
    CHECK(memcmp(page_a, page_b, PBUF) == 0, "a second paint of the same state differs");

    /* Every pixel of the box is written, and NOTHING outside it is: the sentinel survives
     * everywhere the panel does not reach. This is the check the padded buffer exists for. */
    memset(page_b, 0xa5, PBUF);
    view_page(&v, page_b, 16);
    settings_paint(&v, &t, 0, 0);
    bad = 0;
    for (y = 0; y < PH; y++)
        for (x = 0; x < PW; x++) {
            int inside = (x >= PAD_PX && x < PAD_PX + SB_W &&
                          y >= PAD_PX && y < PAD_PX + SB_H);

            if (!inside && px16(page_b, x, y) != 0xa5a5)
                bad++;
        }
    CHECK(bad == 0, "the panel painted %d pixels outside its own rect", bad);
    CHECK(pat(page_b, SB_X0 + 1, SB_Y0 + 1) == (unsigned short)menu_pixel(16, MENU_FILL),
          "the panel's own bed was not painted");

    /* THE ROW FACES. A row that can be operated wears MENU_BTN; one that cannot wears
     * MENU_BTN_OFF -- the idiom the USB STOP chooser set (deleted 2026-10-14), and the
     * whole of what makes a refusal visible
     * rather than a tap that silently does nothing. */
    memset(&t, 0, sizeof t);                    /* the mixer row is dim with no mixer */
    view_page(&v, page_a, 16);
    settings_paint(&v, &t, 0, 0);
    CHECK(pat(page_a, SB_ROW_X0 + 6, row_ymid(ST_ROW_MIXER))
          == (unsigned short)menu_pixel(16, MENU_BTN_OFF),
          "the dim mixer row is not drawn in the OFF face");
    CHECK(pat(page_a, SB_ROW_X0 + 6, row_ymid(ST_ROW_SHARE))
          == (unsigned short)menu_pixel(16, MENU_BTN),
          "a live viewer row is not drawn in the button face");

    t.mixer_present = 1;
    view_page(&v, page_b, 16);
    settings_paint(&v, &t, 0, 0);
    CHECK(pat(page_b, SB_ROW_X0 + 6, row_ymid(ST_ROW_MIXER))
          == (unsigned short)menu_pixel(16, MENU_BTN),
          "the mixer row does not light when a mixer is present");
    CHECK(memcmp(page_a, page_b, PBUF) != 0,
          "a present mixer drew the same picture as an absent one");

    /* The frame is ink-coloured, as the USB STOP chooser's was (prompt_paint.c, deleted
     * 2026-10-14), so a dim row's frame
     * dims with it -- the refusal is visible in the outline as well as the face. */
    CHECK(pat(page_a, SB_ROW_X0, row_ymid(ST_ROW_MIXER))
          == (unsigned short)menu_pixel(16, MENU_LABEL_OFF),
          "the dim row's frame is not the OFF ink");
    CHECK(pat(page_b, SB_ROW_X0, row_ymid(ST_ROW_MIXER))
          == (unsigned short)menu_pixel(16, MENU_LABEL),
          "the live row's frame is not the plain ink");

    /* A PRESSED ROW wears the accent -- including a pressed DIM row, because a finger on a
     * dim value still has to see that it landed somewhere or the panel reads as dead rather
     * than as refusing. */
    memset(&t, 0, sizeof t);
    view_page(&v, page_b, 16);
    settings_paint(&v, &t, ST_ROW_SHARE, 0);
    CHECK(pat(page_b, SB_ROW_X0 + 6, row_ymid(ST_ROW_SHARE))
          == (unsigned short)menu_pixel(16, MENU_BTN_PRESSED),
          "the pressed row is not drawn pressed");
    view_page(&v, page_b, 16);
    settings_paint(&v, &t, ST_ROW_MIXER, 0);       /* ...which is the dim one here */
    CHECK(pat(page_b, SB_ROW_X0 + 6, row_ymid(ST_ROW_MIXER))
          == (unsigned short)menu_pixel(16, MENU_BTN_PRESSED),
          "a finger on a dim row shows no press at all");

    /* A NULL state draws a real panel rather than nothing. */
    view_page(&v, page_a, 16);
    settings_paint(&v, NULL, 0, 0);
    CHECK(pat(page_a, SB_ROW_X0 + 6, row_ymid(ST_ROW_SHARE))
          == (unsigned short)menu_pixel(16, MENU_BTN),
          "a panel with no state drew nothing");

    /* NO MARKS AT ALL -- the operator's call (2026-10-14), and the thing worth pinning
     * now that they are gone: the value column's right-hand end must be CLEAR of ink, so
     * the padding the operator asked for is real and not something a later mark could eat.
     * The band's gear is a different question and is pinned in test_menu.c. */
    {
        int ink = (int)menu_pixel(16, MENU_LABEL);
        int row, xx, yy, inpad = 0;

        memset(&t, 0, sizeof t);
        t.mixer_present = 1;
        t.usb_live[0] = t.usb_live[1] = 1;
        t.share_on = t.encode_hw = t.pointer_on = 1;
        view_page(&v, page_a, 16);
        settings_paint(&v, &t, 0, 0);

        /* The scan stops one pixel short of the row's own right edge and one row short of
         * its top and bottom, because the FRAME is drawn in this same ink -- a check that
         * included it would be measuring the frame and calling it the value. */
        for (row = 1; row <= SB_ROWS; row++)
            for (yy = 1; yy < SB_ROW_H - 1; yy++)
                for (xx = SB_ROW_X1 - SP_VALUE_RIGHT + 1; xx <= SB_ROW_X1 - 1; xx++)
                    if (pat(page_a, xx, SB_ROW_TOP(row - 1) + yy) == (unsigned short)ink)
                        inpad++;
        CHECK(inpad == 0, "%d pixels of ink are inside the value's right-hand padding"
              " (SP_VALUE_RIGHT = %d) -- there is no room between the value and the frame",
              inpad, SP_VALUE_RIGHT);
    }
}

/* Every row's value column really carries ink, in the widest state each row has -- the
 * readout is the whole point of the panel, and a row that drew its label but not its value
 * would still pass a "the panel painted something" check. */
static void test_values_are_drawn(void)
{
    struct menu_view v;
    struct settings_state t;
    int row, k, ink_rows;

    memset(&t, 0, sizeof t);
    t.mixer_present = 1;
    t.mixer_external = 1;
    t.usb_live[0] = t.usb_live[1] = 1;
    t.share_on = t.encode_hw = t.pointer_on = 1;

    view_page(&v, page_a, 16);
    settings_paint(&v, &t, 0, 0);

    for (row = 1; row <= SB_ROWS; row++) {
        const char *s = settings_row_text(row, &t);
        int top = SB_ROW_TOP(row - 1);

        ink_rows = 0;
        for (k = 0; k < SB_ROW_H; k++) {
            int lx, seen = 0;

            for (lx = SB_VALUE_X0 + 2; lx < SB_VALUE_X1 - SP_VALUE_RIGHT; lx++) {
                unsigned short p = pat(page_a, lx, top + k);

                if (p == (unsigned short)menu_pixel(16, MENU_LABEL))
                    seen = 1;
            }
            if (seen)
                ink_rows++;
        }
        CHECK(ink_rows > 0, "row %d's value '%s' drew no ink", row, s ? s : "(null)");
    }
}

/* THE TWO DECK ROWS, from `struct settings_state`'s deck pairs and the card's width.
 *
 * The rules worth pinning, all of them decisions rather than arithmetic:
 *   - the readout shows a pair the ATLAS CAN DRAW (a dash, not the file's comma);
 *   - the WORD names BOTH decks, because the live map file is authoritative for what it
 *     names -- a word naming only deck1 would put deck2 back to the conf;
 *   - the cycle only offers pairs the card can carry, and wraps;
 *   - the rows are operable with nothing known, because what they hold is a setting. */
static void test_deck_pairs(void)
{
    struct settings_state t;

    memset(&t, 0, sizeof t);

    /* NOTHING KNOWN: the shim's own defaults, and both rows operable. Every character of the
     * readout has to be in the atlas -- the comma is not, which is why the value is a dash. */
    CHECK(strcmp(settings_row_text(ST_ROW_DECK1, &t), "2-3") == 0,
          "an unfilled state reads deck 1 as '%s'", settings_row_text(ST_ROW_DECK1, &t));
    CHECK(strcmp(settings_row_text(ST_ROW_DECK2, &t), "4-5") == 0,
          "an unfilled state reads deck 2 as '%s'", settings_row_text(ST_ROW_DECK2, &t));
    CHECK(settings_row_live(ST_ROW_DECK1, &t) && settings_row_live(ST_ROW_DECK2, &t),
          "the deck rows are not operable with nothing known");
    CHECK(settings_row_live(ST_ROW_DECK1, NULL) && settings_row_live(ST_ROW_DECK2, NULL),
          "the deck rows are not operable with no state at all");
    {
        int k;
        const char *v = settings_row_text(ST_ROW_DECK1, &t);

        for (k = 0; v[k]; k++)
            CHECK(menu_font_index((unsigned char)v[k]) >= 0,
                  "a deck row's readout '%s' has '%c' in it, which the atlas cannot draw",
                  v, v[k]);
    }

    /* WHAT THE STATE SAYS, read off audio.state's decks=[deck1=a,b deck2=c,d]. */
    t.deck_a[0] = 6; t.deck_b[0] = 7;
    t.deck_a[1] = 2; t.deck_b[1] = 3;
    CHECK(strcmp(settings_row_text(ST_ROW_DECK1, &t), "6-7") == 0,
          "deck 1 reads '%s'", settings_row_text(ST_ROW_DECK1, &t));
    CHECK(strcmp(settings_row_text(ST_ROW_DECK2, &t), "2-3") == 0,
          "deck 2 reads '%s'", settings_row_text(ST_ROW_DECK2, &t));
    /* Both rows' texts are readable AT THE SAME TIME -- the module keeps a buffer per row,
     * so a caller comparing them (this file does) cannot be handed the same one twice. */
    CHECK(strcmp(settings_row_text(ST_ROW_DECK1, &t), "6-7") == 0 &&
          strcmp(settings_row_text(ST_ROW_DECK2, &t), "2-3") == 0,
          "the two deck rows' readouts overwrote each other");

    /* THE WORD NAMES BOTH DECKS. deck 1 is on 6,7 and deck 2 on 2,3, so a tap on deck 1 asks
     * for the pair after 6,7 -- which wraps to 0,1 -- AND restates deck 2 where it is. */
    t.audio_channels = 8;
    CHECK(settings_next_value(ST_ROW_DECK1, &t)
          && strcmp(settings_next_value(ST_ROW_DECK1, &t), "deck1=0,1;deck2=2,3") == 0,
          "a tap on deck 1 at 6,7 asks for '%s'", settings_next_value(ST_ROW_DECK1, &t));
    CHECK(settings_next_value(ST_ROW_DECK2, &t)
          && strcmp(settings_next_value(ST_ROW_DECK2, &t), "deck2=4,5;deck1=6,7") == 0,
          "a tap on deck 2 at 2,3 asks for '%s'", settings_next_value(ST_ROW_DECK2, &t));

    /* THE OTHER DECK'S PAIR IS SKIPPED -- the operator's own rule of 2026-10-15: *"don't allow
     * deck pairs to be used twice so skip over if its occupied"*. deck 1 on 2,3 with deck 2 on
     * 4,5 must therefore go to 6,7 and not to 4,5. */
    {
        struct settings_state w;

        memset(&w, 0, sizeof w);
        w.audio_channels = 8;
        CHECK(strcmp(settings_next_value(ST_ROW_DECK1, &w), "deck1=6,7;deck2=4,5") == 0,
              "deck 1 went to '%s' -- it must skip deck 2's 4,5",
              settings_next_value(ST_ROW_DECK1, &w));

        /* And the same the other way: deck 2 on 6,7 with deck 1 on 2,3 wraps to 0,1, which is
         * free -- it never needs to pass over deck 1's pair at all. */
        memset(&w, 0, sizeof w);
        w.audio_channels = 8;
        w.deck_a[1] = 6; w.deck_b[1] = 7;
        CHECK(strcmp(settings_next_value(ST_ROW_DECK2, &w), "deck2=0,1;deck1=2,3") == 0,
              "deck 2 wrapped to '%s'", settings_next_value(ST_ROW_DECK2, &w));

        /* A STATE THAT IS ALREADY COLLIDED CAN BE WALKED OUT OF: both decks on 4,5 (only
         * reachable from the conf now) still moves on the first tap, because the shared pair
         * is the one pair the rule skips. */
        memset(&w, 0, sizeof w);
        w.audio_channels = 8;
        w.deck_a[0] = 4; w.deck_b[0] = 5;
        w.deck_a[1] = 4; w.deck_b[1] = 5;
        CHECK(strcmp(settings_next_value(ST_ROW_DECK1, &w), "deck1=6,7;deck2=4,5") == 0,
              "a collided state offers '%s' -- it must not be a trap",
              settings_next_value(ST_ROW_DECK1, &w));
    }

    /* THE CYCLE WALKS THE CANDIDATES AND WRAPS, on a card wide enough for all four and with the
     * other deck parked somewhere the walk passes. deck 2 sits on 4,5 throughout, so deck 1's
     * walk is 2,3 -> 6,7 -> 0,1 -> (4,5 is skipped) -> 2,3. */
    {
        struct settings_state w;
        const char *want[3] = { "deck1=6,7;deck2=4,5", "deck1=0,1;deck2=4,5",
                                "deck1=2,3;deck2=4,5" };
        int step;

        memset(&w, 0, sizeof w);
        w.audio_channels = 8;
        for (step = 0; step < 3; step++) {
            const char *word = settings_next_value(ST_ROW_DECK1, &w);

            CHECK(word && strcmp(word, want[step]) == 0,
                  "deck 1's cycle step %d asks for '%s', not '%s'", step,
                  word ? word : "(null)", want[step]);
            /* What the file would now say -- the panel writes it, and the next tap must see
             * the world it just made rather than the one it started from. */
            sscanf(word, "deck1=%d,%d", &w.deck_a[0], &w.deck_b[0]);
        }
        CHECK(w.deck_a[0] == 2 && w.deck_b[0] == 3,
              "three taps left deck 1 at %d,%d, so the cycle did not come back",
              w.deck_a[0], w.deck_b[0]);
    }

    /* THE WIDTH FILTER: the cycle never offers a pair the card cannot carry. A 4-channel card
     * is channels 0..3, so it carries 0,1 and 2,3 and nothing else; a 6-channel card adds 4,5
     * and still never reaches 6,7. */
    {
        struct settings_state w;

        memset(&w, 0, sizeof w);
        w.audio_channels = 4;
        CHECK(strcmp(settings_next_value(ST_ROW_DECK1, &w), "deck1=0,1;deck2=4,5") == 0,
              "a 4-channel card offers '%s' after 2,3 -- 4,5 is not on it",
              settings_next_value(ST_ROW_DECK1, &w));

        /* 6 channels: 4,5 now fits, but deck 2 is sitting on it, so the walk goes on to 0,1. */
        memset(&w, 0, sizeof w);
        w.audio_channels = 6;
        CHECK(strcmp(settings_next_value(ST_ROW_DECK1, &w), "deck1=0,1;deck2=4,5") == 0,
              "a 6-channel card offers '%s' after 2,3 -- deck 2 holds 4,5 and 6,7 is not on it",
              settings_next_value(ST_ROW_DECK1, &w));

        /* ...and with deck 2 off it, 4,5 is the next one. */
        w.deck_a[1] = 6; w.deck_b[1] = 7;
        CHECK(strcmp(settings_next_value(ST_ROW_DECK1, &w), "deck1=4,5;deck2=6,7") == 0,
              "a 6-channel card offers '%s' after 2,3 with deck 2 elsewhere",
              settings_next_value(ST_ROW_DECK1, &w));
    }

    /* A PAIR THE CANDIDATE LIST DOES NOT HOLD -- set by hand in the conf -- still answers a
     * sensible word rather than none or a crash: the cycle starts at the first candidate that
     * fits and is free, which here is 0,1. */
    {
        struct settings_state w;

        memset(&w, 0, sizeof w);
        w.audio_channels = 8;
        w.deck_a[0] = 1; w.deck_b[0] = 2;      /* not an adjacent pair in the list */
        CHECK(settings_next_value(ST_ROW_DECK1, &w)
              && strcmp(settings_next_value(ST_ROW_DECK1, &w), "deck1=0,1;deck2=4,5") == 0,
              "a deck on a pair the list does not hold asks for '%s'",
              settings_next_value(ST_ROW_DECK1, &w));
    }
}

/* THE DJM ROWS HIDDEN (RB_SETTINGS_DJM unset): six rows, in number order, the box shrunk to
 * them and still centred, a hidden row with no rect and no answer, and the controller's
 * selection walking only the rows shown. Then every row again, as the other tests had it. */
static void test_layout(void)
{
    static const int want[] = { ST_ROW_USB1, ST_ROW_USB2, ST_ROW_MIRROR, ST_ROW_SHARE,
                                ST_ROW_ENCODE, ST_ROW_POINTER };
    int i, x0, y0, x1, y1, act;

    settings_show_rows(0, 1);
    CHECK(settings_shown() == 6, "%d rows shown without the DJM ones", settings_shown());
    for (i = 0; i < 6; i++) {
        CHECK(settings_slot_row(i + 1) == want[i], "position %d holds row %d", i + 1,
              settings_slot_row(i + 1));
        CHECK(settings_row_slot(want[i]) == i + 1, "row %d is not at position %d",
              want[i], i + 1);
    }
    CHECK(settings_slot_row(0) == 0 && settings_slot_row(7) == 0, "a position past the rows");
    CHECK(SB_H == SB_BOX_H(6) && SB_H == 429 && SB_Y0 == (MZ_LOGICAL_H - 429) / 2,
          "the six-row box is %d tall at %d", SB_H, SB_Y0);
    settings_box_rect(&x0, &y0, &x1, &y1);
    CHECK(y0 == SB_Y0 && y1 == SB_Y1 && y1 - y0 + 1 == 429, "the box rect is not six rows");
    settings_row_rect(ST_ROW_USB1, &x0, &y0, &x1, &y1);
    CHECK(y0 == SB_ROW_TOP(0), "USB1 STOP is not the first row");
    settings_row_rect(ST_ROW_POINTER, &x0, &y0, &x1, &y1);
    CHECK(y1 == SB_Y1 - SB_BORDER - SB_PAD, "the last row does not end at the box's margin");
    {
        static const int hidden[] = { ST_ROW_MIXER, ST_ROW_DECK1, ST_ROW_DECK2 };
        for (i = 0; i < 3; i++) {
            settings_row_rect(hidden[i], &x0, &y0, &x1, &y1);
            CHECK(x1 < x0 && settings_row_slot(hidden[i]) == 0,
                  "hidden row %d still has a rect", hidden[i]);
        }
    }
    begin(1000);
    act = ST_ACT_NONE;
    tap(val_x(), SB_ROW_TOP(2) + SB_ROW_H / 2, &act);
    CHECK(act == ST_ROW_MIRROR, "a tap on the third row answered %d, not HDMI AUDIO", act);
    begin(1000);
    settings_focus(99, 1000);
    CHECK(settings_focused() == 6 && settings_pressed() == ST_ROW_POINTER,
          "the selection runs past the rows shown (%d, row %d)", settings_focused(),
          settings_pressed());
    settings_focus(1, 1000);
    CHECK(settings_pressed() == ST_ROW_USB1, "the first selection is row %d", settings_pressed());
    settings_close();

    /* ...and the VNC rows hidden too, which is the ODROID's own panel: three rows. */
    settings_show_rows(0, 0);
    CHECK(settings_shown() == 3 && settings_slot_row(1) == ST_ROW_USB1 &&
          settings_slot_row(2) == ST_ROW_USB2 && settings_slot_row(3) == ST_ROW_MIRROR &&
          SB_H == SB_BOX_H(3), "without the DJM and VNC rows: %d rows", settings_shown());
    settings_show_rows(1, 0);
    CHECK(settings_shown() == 6 && settings_slot_row(1) == ST_ROW_MIXER &&
          settings_slot_row(5) == ST_ROW_DECK1 && settings_row_slot(ST_ROW_SHARE) == 0,
          "the DJM rows without the VNC ones are not in number order");

    settings_show_rows(1, 1);
    CHECK(settings_shown() == SB_ROWS && SB_H == 615, "the DJM rows did not come back");
    for (i = 1; i <= SB_ROWS; i++)
        CHECK(settings_slot_row(i) == i, "with every row shown, position %d holds row %d",
              i, settings_slot_row(i));
}

int main(void)
{
    test_geometry();
    test_labels_and_values();
    test_next_values();
    test_deck_pairs();
    test_gesture_value_only();
    test_hold_rows();
    test_dim_rows();
    test_ownership();
    test_clock();
    test_paint_size();
    test_paint_image();
    test_values_are_drawn();

    /* THE CONTROLLER'S SELECTION (settings_remote.h): none until focused, clamped to the
     * rows, the highlight while no finger is on a value, and moving it restarts the clock */
    {
        int act = ST_ACT_NONE;
        begin(1000);
        CHECK(settings_focused() == 0 && settings_pressed() == 0,
              "a raised panel has no selection until something focuses");
        settings_focus(0, 1000);
        CHECK(settings_focused() == 1 && settings_pressed() == 1, "focus clamps up to row 1");
        settings_focus(SB_ROWS + 5, 1000);
        CHECK(settings_focused() == SB_ROWS, "...and down to the last row");
        settings_focus(ST_ROW_MIXER, 9000);
        CHECK(settings_deadline() == 9000 + SB_TIMEOUT_MS, "moving the focus restarts the clock");
        feed(1, val_x(), row_ymid(ST_ROW_USB1), &act);
        CHECK(settings_pressed() == ST_ROW_USB1 || settings_hold_row() == ST_ROW_USB1,
              "a finger's press outranks the selection");
        settings_close();
        CHECK(settings_focused() == 0, "closing clears the selection");
        settings_focus(2, 1000);
        CHECK(settings_focused() == 0, "a shut panel takes no focus");
    }

    test_layout();

    printf("ok: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
