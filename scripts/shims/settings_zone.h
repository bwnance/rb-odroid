/*
 * settings_zone.h -- the SETTINGS panel: its box, its rows, and which row a finger
 * acted on.
 *
 * WHY IT EXISTS. The operator's ask of 2026-10-14 -- *"i want to be able to switch
 * from the internal mixer to the external mixer from the glass"*, and then, on the
 * shape of it, *"maybe a settings screen with stuff like USB stop and mixer mode"*
 * and *"and then it is just a gear icon"*. Until now the only way to change the
 * mixer route was `confscreen.py` on a laptop.
 *
 * THE SWITCH ITSELF IS NOT THIS FILE'S BUSINESS, and that is the whole of why this
 * is small. `/run/rbp/mixer.mode` already exists, `audioshim.c`'s
 * mixer_route_tick() already re-reads it while the unit plays, and the config page
 * already writes it -- so a row here does exactly what the page does: it moves a
 * word in a file. NOTHING in this module opens a file, reaches rbp, or reads a clock;
 * the row values arrive in a `struct settings_state` the caller fills, and the word a
 * tap asks for comes back as a string (settings_next_value()). The caller writes it.
 * That is the family's contract, stated in side_zone.h: a module says WHICH control
 * and leaves the message to the caller.
 *
 * PURE, like menu_zone.c, fx_zone.c and util_zone.c, and for the same reason --
 * test_settings.c pins the geometry, the gesture, the value-only target and every
 * readout on a host with no Pi, no panel and no player.
 *
 * ---------------------------------------------------------------------------
 * THE BOX. A centred, titled, ruled panel of framed ROWS -- the drawing family
 * menu_paint.c and side_paint.c belong to, with a list where they have one row:
 *
 *     +---------------------------------------------------+  SB_Y0
 *     |  SB_BORDER                                        |
 *     |                SETTINGS            (SB_TITLE_H)   |
 *     |  ________________________________________________|  SB_RULE_Y
 *     |  SB_PAD                                           |
 *     |  +-----------------------------------------------+|
 *     |  | MIXER ROUTE            INTERNAL           (0) ||  SB_ROW_H
 *     |  +-----------------------------------------------+|
 *     |  SB_ROW_GAP                                       |
 *     |  +-----------------------------------------------+|
 *     |  | USB1 STOP              RBOX USB               ||
 *     |  +-----------------------------------------------+|
 *     |  ...                                    8 rows    |
 *     |  SB_PAD                                           |
 *     +---------------------------------------------------+  SB_Y1
 *
 * SB_H = 2*3 + 40 + 1 + 2*8 + 8*56 + 7*6 = 553, exactly -- the same arithmetic
 * the USB STOP chooser's own box was, before it was removed on 2026-10-14, and
 * test_settings.c asserts the sum rather than the number, so a change to a row's height
 * moves what is under it instead of running off the bed. The bed is MZ_LOGICAL_H (800),
 * so EIGHT is the most rows that fit with a margin: nine would need 615 px and still fit,
 * twelve would not.
 * ---------------------------------------------------------------------------
 * THE TARGET IS THE VALUE, AND ONLY THE VALUE. The rows are a list to be READ, and the
 * operator asked for the label half to be inert -- so the framed rect is what tells the
 * painter what a row is, and SB_VALUE_X0..X1 is the only thing a press can act on. That
 * costs one deliberate departure from the usual box rule, and it is the one to keep in mind
 * when reading the gesture: A PRESS ON A ROW'S LABEL HALF DOES NOT DISMISS THE PANEL. The
 * rule a box normally keeps -- "a release that began off every button dismisses" -- would
 * mean, for a list of rows, that a finger landing on a label threw the panel away, which is
 * exactly what the operator asked not to happen. So the dismissal is "began outside every
 * ROW", and the title band, the rule, the padding, the margin and the glass outside the box
 * are all still ways out.
 * ---------------------------------------------------------------------------
 */
#ifndef RBPI4B_SETTINGS_ZONE_H
#define RBPI4B_SETTINGS_ZONE_H

#include "menu_zone.h"      /* MZ_LOGICAL_W/H (the space) and the MZ_FEED_* codes */

/* ------------------------------------------------------------------ geometry */

/* EVERY ROW THE PANEL KNOWS, and so the most it can show: nine is 615 px, which fits the
 * 800 px bed. Which of them it DOES show is the layout (settings_show_djm() below), and the
 * box is as tall as the rows shown -- so SB_H and SB_Y0 are asked of the layout, not fixed. */
#define SB_ROWS       9

/* The rows, 1-based and in the order they are drawn. THE NUMBER IS ALSO THE ANSWER: a
 * tap reports the row it acted on, so there is no second enum to drift out of step with
 * this one (settings_row_ejects() is what tells the caller which kind of row it was handed).
 *
 * THE TWO USB ROWS REPLACED THE ONE `USB STOP` ROW on 2026-10-14, at the operator's ask:
 * *"add a line for each usb stick the settings menu, it should be called USB1 and USB2 and
 * the setting should be the USB label name, if you hold the USB1/USB2 button it should
 * behave like the USB stop menu buttons"*. Each stick has its own row, reading its own name,
 * and a three-second hold on either ejects THAT device -- the chooser's own gesture, one row
 * per device. The band's USB STOP column is unchanged and still raises the chooser; what
 * went is the panel's row that raised it, because two hold-to-eject doors onto the same two
 * devices inside one panel was the redundancy, not the feature.
 *
 * THEY READ "USB1 STOP"/"USB2 STOP", the operator's own words (2026-10-14): *"you can call it
 * USB1 STOP and USB2 STOP vs 'USB 1', 'USB 2'"*. The word STOP is what makes the rows
 * legible as the eject they are -- a tap on them does nothing, and the hold is the whole
 * gesture -- and on a panel of settings it is the only label that says so. It is also why
 * they are not spelled with the space the rest of the tree uses ("USB 1"): the operator's
 * spelling is the row's, and the label is read as one phrase. */
#define ST_ROW_MIXER    1
#define ST_ROW_USB1     2
#define ST_ROW_USB2     3
/* HDMI AUDIO, added 2026-10-26 at the operator's ask -- *"put that setting for hdmi audio into
 * the settings menu"*: the mirror of the master on the board's own HDMI card (audioshim.c),
 * switched live through /run/rbp/mirror.mode. ON/OFF, always operable; the shim decides
 * whether there is a card to mirror to, as it does at start. */
#define ST_ROW_MIRROR   4
#define ST_ROW_SHARE    5
#define ST_ROW_ENCODE   6
#define ST_ROW_POINTER  7
/* THE TWO DECK PAIRS, added 2026-10-15 with the live pair map. The operator asked *"what about
 * configuring which channel pairs get audio?"*, and in EXTERNAL mixer mode the deck pairs ARE
 * the answer to it: rbp's mix is not sent at all, so master/headphones/booth are not in the path
 * and `deck1`/`deck2` are the only pairs that decide where audio goes. That is why these two are
 * here and the other four streams are not -- together with the arithmetic below, which has room
 * for two more rows and not for six.
 *
 * THEY CYCLE, like the mixer route's row: a tap moves the deck to the next pair the shim can
 * carry (2,3 -> 4,5 -> 6,7 -> 2,3). They are NOT dim in internal mode, and that is deliberate:
 * unlike the mixer row, whose readout would claim a route the unit is not taking, this row reads
 * a SETTING -- where deck 1 goes when external is in force -- and a setting can be made before
 * it is used. */
#define ST_ROW_DECK1    8
#define ST_ROW_DECK2    9

#define SB_TITLE      "SETTINGS"

/* 640 rather than the chooser's 560: the longest label is "POINTER INJECT" (129 px at the
 * shipped atlas, measured off menu_font.h on the host) and the widest value is
 * "USB 1 & USB 2" (115 px), and the two have to sit in one row with room between them.
 * 640 leaves 458 px of row for the label and 160 px for the value, against 129 and 107 --
 * test_settings.c pins both numbers rather than trusting this comment. */
#define SB_W        640
/* The box for `n` rows: 2*3 + 40 + 1 + 2*8 + n*56 + (n-1)*6 -- 553 for the eight it had
 * until 2026-10-26, 615 for all nine, 429 for the six a unit without a DJM shows. */
#define SB_BOX_H(n) (2 * SB_BORDER + SB_TITLE_H + SB_RULE + 2 * SB_PAD \
                     + (n) * SB_ROW_H + ((n) - 1) * SB_ROW_GAP)
#define SB_H        SB_BOX_H(settings_shown())
#define SB_X0       ((MZ_LOGICAL_W - SB_W) / 2)          /* 320 */
#define SB_Y0       ((MZ_LOGICAL_H - SB_H) / 2)          /* centred, whatever the rows */
#define SB_X1       (SB_X0 + SB_W - 1)                   /* 959 */
#define SB_Y1       (SB_Y0 + SB_H - 1)

/* The box's margin, inside it on all four sides -- PR_BORDER's number and its job, and
 * the box's own edge is the black bed -- the restyle of 2026-10-06 that took the frame off
 * the band and the chooser and put the white round each control instead. */
#define SB_BORDER    3
#define SB_PAD       8          /* between the margin and what it holds */
#define SB_TITLE_H  40
#define SB_RULE      1
#define SB_ROW_H    56
#define SB_ROW_GAP   6

#define SB_ROW_X0   (SB_X0 + SB_BORDER + SB_PAD)          /* 331 */
#define SB_ROW_X1   (SB_X1 - SB_BORDER - SB_PAD)          /* 948 */

#define SB_TITLE_Y0 (SB_Y0 + SB_BORDER)
#define SB_TITLE_Y1 (SB_TITLE_Y0 + SB_TITLE_H - 1)
#define SB_RULE_Y   (SB_TITLE_Y1 + 1)

/* Row i (0-based, the POSITION on the panel and not the row's number) top -- the arithmetic in one place, so the painter, the hit test and
 * the test cannot disagree about where a row is. */
#define SB_ROW_TOP(i) (SB_RULE_Y + 1 + SB_PAD + (i) * (SB_ROW_H + SB_ROW_GAP))

/* THE VALUE'S OWN RECT, at the right-hand end of the row -- the only part of a row a
 * press acts on. 160 px holds the widest value ("USB 1 & USB 2", 115 px measured) with the
 * value's own padding to spare, and it is measured from the row's right edge so that
 * padding -- and the value's end -- sit INSIDE the target that acts: the operator taps
 * where the value is drawn, not beside it. */
#define SB_VALUE_W  160
#define SB_VALUE_X0 (SB_ROW_X1 - SB_VALUE_W + 1)
#define SB_VALUE_X1 (SB_ROW_X1)

/* The box goes away on its own, at the chooser's number and for its reason: this panel
 * covers the middle of the performance screen and nothing it holds is urgent: ten seconds is
 * long enough to read six rows and decide, and long enough that a three-second hold started
 * in the last third of it still completes. Suspended while a finger is down, by the same rule
 * -- a press the panel
 * swallowed has been withheld from rbp for its whole life and rbp is going to be given its
 * release ([[declined-press-must-still-see-release]]). */
#define SB_TIMEOUT_MS 10000

/* THE HOLD, for the two USB rows only: a press on a live row starts it, the row draws
 * PRESSED for the first half period and its normal face for the second, and three seconds
 * later the eject goes out on THAT device's own channel. A release before that sends nothing,
 * because the alternative -- one row per device with a tap-to-eject -- is a control the
 * operator's media would not survive a stray finger on.
 *
 * 3000 IS THE OPERATOR'S OWN THREE SECONDS, first asked for on 2026-10-05 and carried here
 * unchanged: *"if still held after 3 seconds have it ejected. if they release it before three
 * seconds don't eject it"*. 250 ms is the HALF period of the blink a running hold draws, and
 * it is rbp's own house blink rather than a number chosen here -- a State-2 LedStat entry
 * blinks at its `+28`, and the deck PLAY lights are 500 ms a full cycle -- so six full blinks
 * fit inside one hold. Both numbers were the USB STOP chooser's before it was removed on
 * 2026-10-14, and they are kept to the millisecond because the operator's finger has already
 * learned them. */
#define ST_HOLD_MS        3000
#define ST_HOLD_FLASH_MS  250

/* The ink's margin inside a row on each side, in font pixels -- PR_LABEL_MARGIN's number
 * and its reason: the Decker bake inks up to a pixel past a glyph's advance, so a value
 * laid out to the advance exactly touches the frame. */
#define SB_LABEL_MARGIN 2

/* ------------------------------------------------------------- the answers */

/* What a TAP or a COMPLETED HOLD asks for, and the value is the ROW it acted on
 * (1..SB_ROWS), or ST_ACT_NONE. There is no separate enum because every row has exactly one
 * act: the caller asks settings_row_ejects() to learn whether that act is an eject -- the two
 * USB rows, whose hold completed -- or a write, and settings_next_value() for the word.
 *
 * A DIM ROW ANSWERS NOTHING -- settings_row_live() is asked here, at the fire, and a
 * refusal is ST_ACT_NONE rather than an act the caller has to remember to check. That is the
 * operator's own choice for the mixer row ("refuse the tap", 2026-10-14) and it is the
 * chooser's rule for a dim button, which refuses to start a hold at all rather than letting a
 * press through to be dropped at the far end. */
#define ST_ACT_NONE 0

/* ------------------------------------------------------------- the state */

/* What the caller knows, filled by it and never by this module.
 *
 * THE USB ROWS REUSE THE CHOOSER'S OWN ANSWERS rather than copies: `usb_live[]` is
 * the walk of rbp's two UsbStorageManager objects, and the same NUMBER the chooser's buttons
 * objects (pointsrc_usb_state()), and `usb_name[]` is the same string the chooser puts on a
 * button -- MINUS its "HOLD " prefix, which is that box's instruction and not a name. Both
 * come out of usb_label.h's sanitizer, so the panel cannot show a stick a name rbp's own
 * SOURCE screen would refuse.
 *
 * A row with media but no name of its own reads "--": the STICK is what the row is about,
 * and its label is what the operator asked it to show, so an unnamed stick says so rather
 * than borrowing the row's own "USB 1" back.
 *
 * THE THREE VIEWER BITS ARE THE ONE PLACE THIS PANEL KNOWS SOMETHING IT CANNOT VERIFY.
 * vnc.live, vnc.mode and vnc.input are read by vncserve ON THE HOST, whose start value
 * comes from rb.conf and whose file wins only once one exists. So a caller that reads a
 * file shows what the file says, and a caller that finds none shows the documented
 * default -- which is right on the shipped config and wrong on a unit that overrides
 * them in rb.conf. The panel is authoritative the moment it writes one; docs/07-touch.md
 * says so where the rows are described. */
/* "HOLD " (5) + usb_label.h's USB_LABEL_MAX (30) is what the chooser composes; a name here
 * is the bare one, so the caller's buffer is the same size less nothing -- it is the same
 * string minus a prefix, and being generous costs a few bytes. */
#define ST_NAME_MAX 35

struct settings_state {
    int usb_live[2];        /* device i+1 has media present, i.e. [mgr+0x88] == 2 */
    char usb_name[2][ST_NAME_MAX + 1];   /* ...and its own volume label, drawn only if live */
    int mixer_present;      /* a recognised digital mixer is on the USB bus */
    int mixer_external;     /* the switch file says `external` */
    int share_on;           /* vnc.live  */
    int encode_hw;          /* vnc.mode, where nonzero is `hwjpeg` */
    int pointer_on;         /* vnc.input */
    /* THE DECK PAIRS AND THE CARD'S WIDTH, for the two deck rows (ST_ROW_DECK1/2).
     *
     * READ FROM `/run/rbp/audio.state` -- the file audioshim publishes -- and NOT from the
     * live map file, because the row must show what the unit is DOING: audio.state carries the
     * resolved map, so a pair the shim refused is not claimed here. This is the mixer row's own
     * rule ("the route IN EFFECT and not the file's word") applied to a pair.
     *
     * `deck_a[i]`/`deck_b[i]` are deck i+1's channels; `audio_channels` is the width the card
     * was opened with. A zeroed state -- the caller that has not read the file yet -- is legal
     * and is the shipped picture, which here means both decks on the shim's own defaults (2,3
     * and 4,5) against an unknown width. */
    int audio_channels;     /* 0 = not known, and then every candidate pair is offered */
    int deck_a[2];
    int deck_b[2];
    int mirror_off;         /* mirror.mode says `off`; zero -- no file -- is the mirror ON */
};

/* ---------------------------------------------------------------- the layout */

/* WHICH ROWS THE PANEL SHOWS, in the order it shows them. Two groups can be hidden: the three
 * DJM rows -- MIXER ROUTE and the two DECK PAIRs, which only mean something with an external
 * digital mixer on the bus (operator, 2026-10-26: *"i don't use a djm and i won't, but i don't
 * want to disable that completely"*) -- and the three VNC rows, SHARE, ENCODE and POINTER
 * INJECT, which only mean something on a unit running the viewer (operator, the same day:
 * *"also conditionally set the vnc settings rows"*). Hidden rows keep their numbers, so
 * nothing that names a row changes, and the box shrinks to the rows left. Every row is the
 * default, so a caller that never asks gets them all. pointsrc.c asks once, at load, from
 * RB_SETTINGS_DJM and RB_SETTINGS_VNC -- before anything sizes a buffer from SB_H. */
void settings_show_rows(int djm, int vnc);
int settings_shown(void);           /* how many rows the panel shows */
int settings_slot_row(int slot);    /* the row at position 1..settings_shown(), or 0 */
int settings_row_slot(int row);     /* the position row `row` is drawn at, or 0 if hidden */

/* ---------------------------------------------------------------- the state */

/* Raise the panel, or re-raise it if it is already up (which restarts the clock).
 * `now_ms` is the caller's monotonic milliseconds. A press in flight is NOT inherited --
 * the tap that raised this fired on a release, so it is already over. */
void settings_open(unsigned long long now_ms);

/* Put it away. Safe on a shut panel, and safe from the tick as well as from the touch
 * thread. */
void settings_close(void);

int settings_is_open(void);

/* THE CONTROLLER'S SELECTION -- the panel worked from a browse knob, with no finger
 * (settings_remote.h): a POSITION, clamped to 1..settings_shown(), so the knob walks the
 * rows shown and never a hidden one. The row there (settings_slot_row()) is the highlight
 * whenever no press is in flight (settings_pressed()), and moving it restarts the
 * panel's clock as a touch would. 0 until something focuses; settings_open() and
 * settings_close() clear it, so a panel a finger raised shows no selection. */
void settings_focus(int slot, unsigned long long now_ms);
int settings_focused(void);

/* The wall-clock milliseconds at which the panel closes itself. Only meaningful while
 * settings_is_open(). */
unsigned long long settings_deadline(void);

/* WHAT THE CLOCK HAS DONE, asked on every slice of the caller's wait while the panel is up.
 * Two things can happen: a hold can complete, and the panel can time out.
 *
 * IT TAKES THE STATE NOW, which it did not before the USB rows existed: a hold fires only if
 * the device is STILL live at the moment it completes, so the tick has to be able to ask --
 * the same rule the chooser keeps, and the reason a stick pulled out from under a running
 * hold cannot eject. */
enum {
    ST_TICK_NONE = 0,       /* the panel is not up; nothing was done */
    ST_TICK_UP,             /* still up, and nothing changed */
    ST_TICK_FLASH,          /* nothing but the blink moved; the panel is still up */
    ST_TICK_EJECT,          /* a hold completed: `*act` names the row, and the caller sends
                             * the eject ONCE. THE PANEL IS STILL UP -- the finger that made
                             * the hold is by definition still down, so the release is still
                             * owed and the panel has to be there to take it (settings_tick()'s
                             * fire branch says why in full) */
    ST_TICK_TIMEOUT         /* it dismissed itself */
};
int settings_tick(const struct settings_state *S, unsigned long long now_ms, int *act);

/* Drop everything, including a press in flight. pointsrc.c calls this where it calls
 * menu_reset() and fxlist_reset(), for the reason stated there: a box left up by a
 * finger that went away cannot be dismissed at all, because every press is swallowed
 * while it is up. */
void settings_reset(void);

/* ---------------------------------------------------------------- the hold */

/* The row whose HOLD IS RUNNING -- the finger is down on its value, the row is live, and the
 * finger has not left it -- or 0 for none. This is the device the eject will be for. */
int settings_hold_row(void);

/* The flash, while a hold is running: 1 while the row draws PRESSED and 0 while it draws its
 * normal face. Meaningless with no hold, and then 0. It advances only in settings_tick() --
 * the module has no clock, so the phase is carried in the state the same way the hold is. */
int settings_hold_flash(void);

/* How long the running hold has been held, in milliseconds, or 0 with no hold. It is what
 * the caller logs: "usb 1 held 3020 ms (hold is 3000 ms)" is the line that tells a drill the
 * threshold was crossed by a real finger and not by the tick's own slice. */
unsigned long long settings_hold_ms(unsigned long long now_ms);

/* Which device a completed hold on `row` is for: the USB rows answer 1 and 2, which is the
 * device's OWN channel -- the number rbp's UsbStorageManager::onKey tests against its own,
 * exactly as the ejected device's own channel. Any other row answers 0, and a
 * caller that sends on 0 sends nothing. */
int settings_act_channel(int row);

/* ---------------------------------------------------------------- the input */

/* The ROW (1..SB_ROWS) whose framed rect contains a logical point, or 0. This is what
 * "began outside every row" is asked of -- the dismissal -- and NOT what fires. */
int settings_row_at(int x, int y);

/* The value rect the finger is over right now, or 0 -- the highlight, and the only rect
 * a press can act on. Separate from settings_row_at() because the two answer different
 * questions, and because the painter needs both. */
int settings_value_at(int x, int y);

/* The row the finger is highlighting, or 0 -- settings_value_at()'s answer, published
 * for the painter (menu_pressed()'s question asked of this module). With no finger on
 * a value it is the controller's focused row (settings_focus()), so the selection is
 * drawn with the same pressed pair. */
int settings_pressed(void);

/* The row's and the value's rects, logical, inclusive at both ends -- the same rects the
 * two hit tests above use, so the ink and the hit target are the same pixels. Out of
 * range answers the empty rect (x1 < x0), as the other zones' do. */
void settings_row_rect(int row, int *x0, int *y0, int *x1, int *y1);
void settings_value_rect(int row, int *x0, int *y0, int *x1, int *y1);

/* The panel's own rect, logical, inclusive at both ends. */
void settings_box_rect(int *x0, int *y0, int *x1, int *y1);

/* ---------------------------------------------------------------- the words */

const char *settings_title(void);

/* The row's label, or NULL out of range. A character the atlas cannot draw would ship as
 * a gap rather than as a failure, which is why test_settings.c walks every character of
 * every label against menu_font.h. */
const char *settings_row_label(int row);

/* What row `row`'s value column READS, given rbp's and the files' answer. A NULL state
 * is legal and is the shipped picture -- every row at its default -- so a caller that has
 * no snapshot draws a real panel rather than nothing.
 *
 * THE MIXER ROW READS THE ROUTE IN EFFECT AND NOT THE FILE'S WORD, which is the one
 * readout here that is a decision rather than a copy. With no recognised mixer on the bus
 * audioshim STAYS INTERNAL no matter what the file says (audioshim.c's own fallback, and
 * the reason it logs the refusal rather than doing nothing quietly), so a row that read
 * `EXTERNAL` at that moment would be claiming a route the unit is not taking. It reads
 * INTERNAL, dim, and cannot be pressed. */
const char *settings_row_text(int row, const struct settings_state *S);

/* THE WIDEST VALUE `row` can ever read, in the atlas's own advances -- the number
 * settings_paint.c's size refusal measures the value column against. It is asked of the
 * row's whole table rather than of the value in force, because a refusal that depended on
 * the state would take the panel off the glass the moment a long value arrived, and the
 * caller asks the refusal ONCE and paints only if it answers yes, so a state-dependent one
 * would take the whole panel off the glass. Out of range answers 0. */
const char *settings_row_widest_value(int row);

/* THE WORD A TAP ON `row` ASKS FOR, or NULL for a row that does not write one -- which is
 * now the two USB rows, whose act is an eject (settings_row_ejects()). The caller writes the
 * word to the row's own switch file, verbatim and with a newline, so the cycle lives in ONE
 * place and a caller cannot derive the next value differently from the one the panel is
 * showing. */
const char *settings_next_value(int row, const struct settings_state *S);

/* Has this row anything to do? The mixer row is live only while a recognised mixer is on the
 * bus (the operator's "refuse the tap"); each USB row only while rbp reports media in ITS
 * slot, which is also what stops a hold from starting on an empty one; the three viewer rows
 * always. Out of range answers 0, so a caller cannot read past the tables. */
int settings_row_live(int row, const struct settings_state *S);

/* `text` cut to at most `max_px` of the atlas's own advances, into `out` (at most `max`
 * bytes, NUL included). Answers the character count, 0 for a NULL text, a max_px that cannot
 * hold one character, or a max of 0.
 *
 * IT EXISTS FOR THE USB ROWS, whose value is the STICK'S OWN NAME -- the operator's to write,
 * and longer than any column. It cuts FROM THE END, so what is lost is the tail of the name
 * and never its head, and each character is charged the advance menu_font_adv() gives it, the
 * same walk the painter makes, so the string it centres is the string measured here.
 * settings_paint.c cuts with it and centres what is left. */
int settings_text_clip(const char *text, int max_px, char *out, int max);

/* IS THIS ROW AN EJECT? True for the two USB rows, whose act is a completed hold rather than
 * a tap -- the caller sends `settings_act_channel(row)` on that device's own channel instead
 * of writing a word, and the painter uses it for one thing: the hold's blink. (It used to
 * mean "raises the USB STOP chooser", and the chevron that went with it is gone: the panel
 * has no row that opens a box any more.) */
int settings_row_ejects(int row);

/* Feed one pointer report, exactly as pointsrc.c's funnel hands them out. `now_ms` is the
 * same monotonic clock settings_open() took, and it is here because the two USB rows hold: a
 * press has to be STAMPED where it lands, exactly as it is for the chooser.
 *
 * WHILE THE PANEL IS UP IT OWNS EVERY REPORT, wherever it lands -- menu_zone.c's rule for
 * an open band and every other box's: a press that dismisses must not also
 * press whatever is underneath it. The return is MZ_FEED_TAKEN for everything while open
 * and MZ_FEED_NONE the moment it is shut, so rbp's stream is untouched everywhere else by
 * construction and not by a rectangle test. A release with no press behind it is taken
 * too but does not close the panel.
 *
 * WHAT A RELEASE DOES DEPENDS ON WHERE THE PRESS BEGAN:
 *
 *   ON A LIVE ROW'S VALUE, and it ended on the same one
 *                                     `*act` names the row and the panel STAYS UP, so
 *                                     several settings can be flipped in one visit and the
 *                                     value that just changed is visible. (This used to
 *                                     close the panel for the USB row, which raised the
 *                                     chooser; that row is gone.) A release on a USB row
 *                                     that did not hold long enough answers NOTHING -- the
 *                                     eject needs its three seconds.
 *   ON A DIM ROW'S VALUE            nothing at all: the row cannot be operated, and its
 *                                     release dismisses nothing.
 *   ON A ROW, OFF ITS VALUE         NOTHING, AND THE PANEL STAYS UP. This is the
 *                                     operator's "tap the value only": a finger that
 *                                     lands on a label is a finger that was reading, not
 *                                     one that wants the panel gone.
 *   OUTSIDE EVERY ROW -- the title, the rule, the padding, the margin, the glass
 *                                     dismisses the panel and answers ST_ACT_NONE. That is
 *                                     the whole of "tap outside to dismiss".
 *
 * THE PRESS IS ANCHORED, as every feeder in this shim is: the row and the rect that
 * matter are the ones the press STARTED on, and a finger that slides off -- even back on
 * again -- fires nothing. The release's own coordinates are what fire, though, for
 * every other feeder's reason: the panel reports the up wherever the finger was, and a fast
 * flick that lifted at a moved position must not act on the value it has already left. */
int settings_feed(const struct settings_state *S, int down, int x, int y,
                  unsigned long long now_ms, int *act);

#endif /* RBPI4B_SETTINGS_ZONE_H */
