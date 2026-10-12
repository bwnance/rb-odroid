/*
 * side_paint.h -- the edge drawers' image.
 *
 * The second drawing module after menu_paint.c, and written to the same
 * rule: pure, a function of its arguments and of nothing else, over the caller's
 * `struct menu_view`, in the same palette the band uses (menu_pixel()). "Painting
 * the drawer twice paints it once" is load-bearing here and not a nicety -- the
 * drawer is presented on a SINGLE-BUFFERED plane, so a paint that is not idempotent
 * is a paint that tears. Every pixel of the panel rectangle is written on every
 * call, which is what makes that true by construction.
 *
 * COORDINATES ARE PANEL-LOCAL AND THE MIRROR IS APPLIED HERE. side_zone.h works in
 * local x (0 = the outer edge); the view's buffer is in SCREEN order, because the
 * plane is placed at the panel's screen rectangle. `side` is what turns one into the
 * other, and it is the only reason this function takes it.
 */
#ifndef RBPI4B_SIDE_PAINT_H
#define RBPI4B_SIDE_PAINT_H

#include "menu_paint.h"      /* struct menu_view, menu_pixel() */
#include "side_zone.h"       /* SZ_* geometry, side_local_x() */

/* NO FRAME IS DRAWN AROUND THE PANEL -- *"you don't need a white border on the side
 * swipe menus"* (2026-10-06) -- so this is no longer a border width and is kept only
 * as the margin side_paint_ok() asks the view to have. It is the number the band's own
 * border was (menu_paint.c's MENU_BORDER_PX, not exported), and the band's frame is
 * gone for the same reason, so the two still read as one family. Do not delete it
 * without moving the guard: the guard is what refuses a panel too small to draw. */
#define SP_BORDER    2

/* The fader's cap, in logical rows. It is wider than the rail and taller than it is
 * wide -- the shape a fader cap reads as -- and it is CLAMPED inside the travel's own
 * ends so it never hangs out of the scale at either extreme. Its width is SP_CAP_W, and
 * it is the same in both modes. */
#define SP_HANDLE_H   30

/* ---------------------------------------------------------------------------
 * THE FADER'S STYLE IS THE cdj3k-emu MIXER'S OWN FADER, brought over on the operator's
 * ask of 2026-10-12 -- *"the style should look more like the faders we created in the
 * cdj3k-emu repo and look like fader_knob.png"*. That fader (`crates/cdj3k-emu-ui/src/
 * app/mixer.rs`, `channel_fader()`) is three things, and all three are here:
 *
 *   1. a THIN RAIL, not a wide well -- the groove is a few pixels of MENU_DIV down the
 *      middle, and it is the marks either side of it that make the scale;
 *   2. ELEVEN MARKS, ALTERNATING LONG AND SHORT, their outer ends on a common rail so
 *      the long ones read as the scale's divisions. The reference also prints 0..10
 *      beside the long ones; there is no room for digits on a 180 px panel and the
 *      operator's own reference image carries none either, so they are omitted;
 *   3. a CAP WIDER THAN THE RAIL with a GRIP LINE across its face -- the one mark that
 *      says "put your finger here" rather than "this is the level".
 *
 * MAPPED ONTO THIS PANEL'S PALETTE, and the numbers are close: the reference's rail core
 * is (70,73,78) against MENU_DIV's (70,74,80); its cap face (61,64,69) against MENU_BTN's
 * (40,44,52); its cap outline (155,158,164) against MENU_BORDER's (200,204,210); and its
 * grip is WHITE against MENU_LABEL's (240,242,246). Its marks (116,119,124) fall between
 * MENU_DIV and MENU_BORDER, and MENU_BORDER is taken because the drawer's bed is BLACK
 * where the reference's is a grey panel -- a mark has to be lighter here to read at all.
 *
 * CORNERS STAY SQUARE, which is a deliberate departure: the reference rounds its cap by
 * 2 px, and every other control on this panel -- and the band's, and the chooser's -- is a
 * square box. Rounding one element of a family would read as a mistake, not as a detail.
 * ------------------------------------------------------------------------- */
#define SP_RAIL_W        8   /* the groove, centred on SP_MID_X */
#define SP_MARK_RAIL    67   /* the marks' OUTER ends, either side of the centre */
#define SP_MARK_N       21   /* marks over the travel -- twenty divisions */
#define SP_MARK_IN      18   /* ... and their INNER ends, the same distance in for every
                              * one of them: the scale is uniform, so a mark is a mark.
                              * (It alternated long/short to begin with, after the
                              * reference; the operator's own correction, 2026-10-12:
                              * *"make all the lines the same width vs alternating"*,
                              * and then *"get each line a bit closer to the middle grey
                              * strip"* -- so the inner ends hug the groove.) */

/* THE CAP, AND THERE IS ONE WIDTH FOR BOTH MODES. It began as two -- 80 for the volume
 * fader and 58 for the pitch -- and the operator's own reversal made them one (2026-10-12,
 * *"the knob should look the same as the fader knob"*). The two modes are still told
 * apart: by the title, by the readout being drawn in one and not the other, and by the
 * scale's `-` / `0` / `+` labels. What they no longer differ in is the KNOB, which is the
 * one thing on this panel a hand aims at rather than reads.
 *
 * It is sized against the GRAB LANE (SZ_FADER_GX0..GX1, +-45 from the centre), so the knob
 * that is drawn is entirely a knob that can be grabbed. */
#define SP_CAP_W  80

/* The grip: a bar across the cap's face, inset from each end so the face's own outline
 * is never touched -- the reference draws 4 px in, and 6 here because this cap is wider. */
#define SP_GRIP_INSET    6
#define SP_GRIP_H        3

/* HOW ROUND A BOX'S CORNERS ARE, in DEVICE pixels -- the operator's ask of 2026-10-12,
 * *"make the knobs and buttons with a 5px rounded corner"*. It is applied to every box
 * this painter draws: the four buttons (SYNC, the two nudge cells, CUE, PLAY) and the
 * fader's cap.
 *
 * DEVICE PIXELS AND NOT LOGICAL, like the glyphs and the nudge signs beside them
 * (SP_SIGN_*, MENU_FONT_*): a panel's RECTANGLES scale with its size, but the marks on
 * them do not, and a corner is a mark. At the panel this port ships -- 180x800, drawn
 * 1:1 -- the two are the same number.
 *
 * IT DEPARTS FROM THE cdj3k-emu REFERENCE, which rounds its fader cap by 2 px and leaves
 * its buttons square. 5 is the operator's own number, for both. */
#define SP_ROUND_PX     5

/* THE SCALE'S OWN ENDS, AND THEY ARE NOT THE TRAVEL'S. The cap is a physical knob: the
 * line across its middle is what marks a value, and the cap's body extends SP_HANDLE_H/2
 * either side of that line. The clamp in sp_fader() has always stopped the centre there --
 * that is what keeps the knob from hanging out of the travel at either extreme -- so the
 * highest a value can be marked is SP_HANDLE_H/2 inside the travel, and that is where the
 * scale's top mark belongs.
 *
 * Drawing the top mark at the travel's own end instead left the knob's centre line 15 px
 * SHORT of it at 100%, and the operator's own correction (2026-10-12): *"the middle marker
 * of the knob 100% should match up with the top hash mark and the bottom hash mark for
 * 0%"*. So the marks run between these two rows, and the centre line lands on one of them
 * exactly at either end.
 *
 * THE RAIL STILL RUNS THE FULL TRAVEL, because the cap's BODY covers that whole range --
 * a groove shorter than the thing sliding in it would show the knob's ends past its own
 * ends at both extremes. */
#define SP_SCALE_Y0  (SZ_FADER_Y0 + SP_HANDLE_H / 2)
#define SP_SCALE_Y1  (SZ_FADER_Y1 - SP_HANDLE_H / 2)

/* The fader's centre column, in PANEL-LOCAL x. Everything on the fader -- the rail, both
 * runs of marks, and both caps -- is placed from this one number. */
#define SP_MID_X  ((SZ_FADER_X0 + SZ_FADER_X1) / 2)   /* 89 */

/* WHERE THINGS SIT RELATIVE TO THAT CENTRE LINE.
 *
 * THE PANEL IS 180 px WIDE, SO ITS CENTRE IS BETWEEN PIXELS 89 AND 90, and SP_MID_X is the
 * 89 of that pair. A span written as (SP_MID_X - d .. SP_MID_X + d) is therefore ONE PIXEL
 * WIDER to the right of the centre than to the left, and its own mirror lands a pixel away
 * from it -- which made the two decks' faders differ by a pixel. The operator, looking at
 * ch1 beside ch2: *"the pitch control alignment on the ch1 vs ch2 look a little different
 * it should be the same and positioned the same for both pitch and fader"*.
 *
 * These two are the whole of the fix. SP_L(d) is d pixels out from the centre line, and
 * SP_R(d) is its EXACT mirror -- the same (SZ_W - 1 - x) reflection side_zone.h's
 * side_screen_x() uses -- so a span between them is its own reflection and both decks draw
 * identically. Every centred thing on the fader is placed with them: the rail, the cap, the
 * cap's grip, and both runs of marks. */
#define SP_L(d)  (SP_MID_X - (d) + 1)
#define SP_R(d)  (SZ_W - 1 - SP_L(d))

/* HOW THICK A LIT BUTTON'S HIGHLIGHT RING IS, in device px. One more than the
 * unlit/pressed frame (sp_frame's 1), which is the operator's *"the cue button
 * highlight frame 1pixel thicker ... the sync frame should also be 1pixel thicker"*.
 * A pressed button keeps the 1 px ring whatever rbp has lit, so the finger's own
 * feedback stays a function of the press alone (test_side.c pins that). */
#define SP_FRAME_LIT 2

/* THE NUDGE MARKS: half-length and weight, in 1:1 DEVICE pixels like the glyphs
 * beside them. They are drawn as bars and not set in the font because the atlas has
 * no '+' at all (side_paint.c), so the two cells have to match by construction -- and
 * they are the tightest thing the panel has to host, which is why side_paint_ok()
 * measures a nudge cell against them.
 *
 * THE ARM CAME DOWN FROM 12 WHEN THE CELL WENT TO HALF HEIGHT (SZ_NUDGE_H). The mark
 * is square -- both bars are `arm` long -- so a 25 px cross does not fit the 36-row
 * cell a 1280x800 panel gives, let alone a short panel's: side_paint_ok() would have
 * refused the drawer outright below ~556 device rows, where before it drew. 9 makes a
 * 19 px cross, which fits the cell at every size this port targets and still leaves
 * the two marks unmistakable -- and it is the mark, not the guard, that gives way: a
 * control with a smaller sign is a control, and a drawer that will not draw is not. */
#define SP_SIGN_ARM    9
#define SP_SIGN_THICK  4

/* Is this view something side_paint() can draw into? menu_paint.h's menu_view_ok()
 * answers the same question for the BAND and refuses a panel whose band cannot host
 * the font's line box; this is that check for the drawer's own rectangles, so a
 * 480x320 panel is refused here rather than drawn as a smear. */
int side_paint_ok(const struct menu_view *v);

/* Draw the drawer `side` (SZ_LEFT/SZ_RIGHT) into `v`, whose buffer is the panel's
 * screen rectangle.
 *
 *   pressed_hit  the control under the finger, SZ_HIT_* -- from side_pressed(side);
 *                SZ_HIT_NONE when nothing is down.
 *   fader_v      the fader's position, 0..1023, top = full.
 *   mode         SZ_MODE_VOL or SZ_MODE_PITCH -- WHICH CONTROL THE FADER IS. It is an
 *                argument and not a call to side_mode() because this painter's whole
 *                contract is that it is a function of its arguments, and a reader of
 *                this file is owed the answer to "why is this handle here" without
 *                following a call into another module. It selects three things: the
 *                knob's width, the centre mark's weight, and whether the readout is
 *                drawn at all.
 *
 * `fader_v` IS A POSITION IN WHICHEVER SPACE THE MODE IMPLIES, and the caller supplies
 * it because the drawer has no position of its own when it is not being dragged: in
 * VOLUME it is the same channel-fader value rbp's mixer is being sent, so what is drawn
 * is what the channel is doing; in PITCH it is the tempo slider's position, which is the
 * shared g_pitch_norm[] in the same 0..1023 space. Both are the value the operator would
 * get by touching the track, so the handle is always where a finger would land.
 *
 * THE READOUT IS VOLUME-ONLY. In pitch mode the rows above the track are left empty --
 * the operator's own choice -- because the shim is never told rbp's tempo RANGE
 * (pitch_state.h), so any number printed there would be a tempo the shim cannot know.
 * The knob and the centre mark carry the position instead.
 *
 *   sync/cue/play/mt  whether rbp has that LED lit ON THIS DECK right now -- 1 lit, 0
 *                dark, and a blink arithmetically already resolved to one of the
 *                two. They come from rbp_transport.h's rbp_transport_get(), which is the
 *                same reading the controller's own LEDs are sent, so the drawer and
 *                the hardware say the same thing at the same instant and not merely
 *                at the same rate. Pass 0 for all of them when rbp's state is not known
 *                yet: "I do not know" and "dark" are the same picture on a panel with
 *                two levels, which is why the caller does not have to distinguish them
 *                here.
 *
 *                `mt` is MASTER TEMPO and reaches only the round disc that toggles it.
 *                `master` is the MASTER half of the beat-match pair -- lit on the deck rbp
 *                has designated master, which is ONE deck at a time (RBP_TRANSPORT_MASTER).
 *                SYNC is the SYNC half of that pair, and the tempo-RANGE disc has no rbp
 *                state behind it at all -- see the note at it in side_zone.h.
 *
 * Every pixel of the view is written, and written the same way for the same inputs --
 * the destination is single-buffered on the page route, so a paint that is not
 * idempotent is a paint that tears. Does nothing when side_paint_ok() is 0. */
void side_paint(const struct menu_view *v, int side, int pressed_hit, int fader_v,
                int mode, int sync, int cue, int play, int mt, int master);

#endif /* RBPI4B_SIDE_PAINT_H */
