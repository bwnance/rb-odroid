/*
 * side_paint.c -- the edge drawers' image: title, SYNC, the nudge pair, the fader and
 * its readout, then CUE and PLAY at the bottom -- on a black bed with no frame around
 * the panel (2026-10-06; every control still frames itself).
 *
 * THE ~10 DUPLICATED LINES, again and deliberately. menu_paint.c's pixel accessors
 * are static there and this file duplicates them for the same reason
 * (menu_paint.c's header records the decision): the alternative is exporting an
 * accessor that must know three module-internal notions to save ten lines. What is
 * NOT duplicated is the palette -- menu_pixel() is public, so the drawer and the band
 * are the same colours by construction.
 *
 * EVERY PIXEL IS WRITTEN ON EVERY CALL. The drawer's plane is single-buffered
 * (side_paint.h), so the panel's rectangle is filled first and every mark goes on
 * top; nothing here reads the buffer, so a second paint of the same state leaves the
 * same image and the plane cannot tear between states.
 */
#include "side_paint.h"
#include "menu_font.h"

#include <stddef.h>          /* size_t, NULL */
#include <stdio.h>           /* snprintf, for the fader readout */

/* --- pixels, in the view's own (framebuffer) coordinates -------------------- */

static void sp_set(const struct menu_view *v, int fx, int fy, unsigned int pv)
{
    unsigned char *row;

    if (fx < 0 || fy < 0 || fx >= v->dw || fy >= v->dh)
        return;
    row = (unsigned char *)v->pix + (size_t)fy * v->pitch * (v->bpp / 8);
    if (v->bpp == 32)
        *(unsigned int *)(row + (size_t)fx * 4) = pv;
    else
        *(unsigned short *)(row + (size_t)fx * 2) = (unsigned short)pv;
}

static void sp_fill(const struct menu_view *v, int x0, int y0, int x1, int y1,
                    unsigned int pv)
{
    int x, y;

    for (y = y0; y < y1; y++)
        for (x = x0; x < x1; x++)
            sp_set(v, x, y, pv);
}

/* --- rounded boxes, for the buttons and the cap ----------------------------- */

/* Is DEVICE pixel (fx, fy) inside the rectangle [fx0,fx1) x [fy0,fy1) with its corners
 * rounded by `r`? Only a pixel inside one of the four r x r corner boxes can be outside,
 * so the distance is measured from that corner's own centre and everywhere else both
 * offsets come out zero and the test passes.
 *
 * INTEGER AND EXACT on purpose. No anti-aliasing and no float: this painter's contract is
 * that a paint is a pure function of its arguments, and the plane it draws into is
 * single-buffered, so a corner that came out one pixel differently between two paints
 * would be a tear. */
static int sp_round_in(int fx, int fy, int fx0, int fy0, int fx1, int fy1, int r)
{
    int dx = 0, dy = 0;

    if (fx < fx0 || fx >= fx1 || fy < fy0 || fy >= fy1)
        return 0;
    if (r < 1)
        return 1;
    if (fx < fx0 + r)
        dx = (fx0 + r) - fx;
    else if (fx >= fx1 - r)
        dx = fx - (fx1 - 1 - r);
    if (fy < fy0 + r)
        dy = (fy0 + r) - fy;
    else if (fy >= fy1 - r)
        dy = fy - (fy1 - 1 - r);
    return dx * dx + dy * dy <= r * r;
}

static void sp_fill_round(const struct menu_view *v, int x0, int y0, int x1, int y1,
                          unsigned int pv, int r)
{
    int x, y;

    for (y = y0; y < y1; y++)
        for (x = x0; x < x1; x++)
            if (sp_round_in(x, y, x0, y0, x1, y1, r))
                sp_set(v, x, y, pv);
}

/* A ring of thickness `w` that FOLLOWS the rounded outline: inside the box but not inside
 * the same box inset by w. Spelled as two tests rather than as four straight runs and
 * four arcs, because a thick rounded frame drawn that way leaves a seam at every corner
 * -- which is exactly the artefact this shape exists to avoid. */
static void sp_frame_round(const struct menu_view *v, int x0, int y0, int x1, int y1,
                           unsigned int pv, int w, int r)
{
    int x, y, ri;

    if (w < 1)
        w = 1;
    ri = r - w;
    if (ri < 0)
        ri = 0;
    for (y = y0; y < y1; y++)
        for (x = x0; x < x1; x++) {
            if (!sp_round_in(x, y, x0, y0, x1, y1, r))
                continue;
            if (sp_round_in(x, y, x0 + w, y0 + w, x1 - w, y1 - w, ri))
                continue;
            sp_set(v, x, y, pv);
        }
}

/* --- logical (panel-local) to framebuffer, with the mirror ----------------- */

/* One local logical x to a framebuffer column. The mirror is here and nowhere else:
 * the view's buffer is placed at the panel's SCREEN rectangle, so the right drawer's
 * local 0 sits at the buffer's RIGHT edge. */
static int sp_x(const struct menu_view *v, int side, int lx)
{
    if (side == SZ_RIGHT)
        lx = SZ_W - 1 - lx;
    return (lx * v->dw) / SZ_W;
}

static int sp_y(const struct menu_view *v, int y)
{
    return (y * v->dh) / SZ_H;
}

/* An inclusive local rect to a half-open framebuffer rect. The two x ends are sorted
 * because the mirror makes the first the larger one on the right drawer. */
static void sp_rect(const struct menu_view *v, int side, int lx0, int ly0,
                    int lx1, int ly1, int *fx0, int *fy0, int *fx1, int *fy1)
{
    int a = sp_x(v, side, lx0), b = sp_x(v, side, lx1);

    *fx0 = a < b ? a : b;
    *fx1 = (a < b ? b : a) + 1;
    *fy0 = sp_y(v, ly0);
    *fy1 = sp_y(v, ly1) + 1;
}

/* --- text, from the band's own atlas --------------------------------------- */

/* menu_paint.c's menu_blend() is static there, so this is it again -- the same
 * deliberate duplication as sp_set(), for the same reason. */
static unsigned int sp_blend(int bpp, unsigned int base, unsigned int ink, int cov)
{
    int br, bg, bb, ir, ig, ib;

    if (cov <= 0)
        return base;
    if (cov >= 255)
        return ink;
    if (bpp == 32) {
        br = (int)((base >> 16) & 0xffu); bg = (int)((base >> 8) & 0xffu);
        bb = (int)(base & 0xffu);
        ir = (int)((ink >> 16) & 0xffu);  ig = (int)((ink >> 8) & 0xffu);
        ib = (int)(ink & 0xffu);
        return (unsigned int)(((br * (255 - cov) + ir * cov + 127) / 255) << 16 |
                              ((bg * (255 - cov) + ig * cov + 127) / 255) << 8 |
                              ((bb * (255 - cov) + ib * cov + 127) / 255));
    }
    br = (int)((base >> 11) & 0x1fu) * 255 / 31;
    bg = (int)((base >> 5) & 0x3fu) * 255 / 63;
    bb = (int)(base & 0x1fu) * 255 / 31;
    ir = (int)((ink >> 11) & 0x1fu) * 255 / 31;
    ig = (int)((ink >> 5) & 0x3fu) * 255 / 63;
    ib = (int)(ink & 0x1fu) * 255 / 31;
    return (unsigned int)(((((br * (255 - cov) + ir * cov + 127) / 255) & 0xf8) << 8) |
                          ((((bg * (255 - cov) + ig * cov + 127) / 255) & 0xfc) << 3) |
                          ((((bb * (255 - cov) + ib * cov + 127) / 255) & 0xf8) >> 3));
}

/* One glyph, pen at (pen_fx) and the top of the LINE BOX at line_fy, clipped to
 * [cx0, cx1) so text can never run off the panel it is drawn into. */
static void sp_glyph(const struct menu_view *v, int pen_fx, int line_fy,
                     unsigned char c, unsigned int base, unsigned int ink,
                     int cx0, int cx1)
{
    const struct menu_glyph *g = menu_font_glyph(c);
    int col, row;

    for (row = 0; row < (int)g->h; row++)
        for (col = 0; col < (int)g->w; col++) {
            int fx = pen_fx + g->left + col;
            int cov = menu_font_cov(g, col, row);

            if (cov == 0 || fx < cx0 || fx >= cx1)
                continue;
            /* 0 and 255 are exact -- the base and the ink themselves -- so only the
             * glyph's edges blend and the paint stays idempotent. */
            sp_set(v, fx, line_fy + g->top + row,
                   cov >= 255 ? ink : sp_blend(v->bpp, base, ink, cov));
        }
}

static void sp_text(const struct menu_view *v, int pen_fx, int line_fy,
                    const char *s, unsigned int base, unsigned int ink,
                    int cx0, int cx1)
{
    if (!s)
        return;
    for (; *s; s++) {
        sp_glyph(v, pen_fx, line_fy, (unsigned char)*s, base, ink, cx0, cx1);
        pen_fx += menu_font_adv((unsigned char)*s, (unsigned char)s[1]);
    }
}

/* A string centred in a LOCAL rect, by the LINE BOX -- the band's rule, so a
 * descender cannot nudge a cap off centre. The rect is scaled and mirrored first,
 * which is why the centring happens on the framebuffer rect and not on the logical
 * one: on the right drawer the logical ends run backwards. The line box is
 * MENU_FONT_LINE DEVICE pixels and not the scaled one, because the atlas is drawn
 * at 1:1 -- only the rectangles scale (menu_paint.c:294-299 does the same). */
static void sp_text_center(const struct menu_view *v, int side, int lx0, int ly0,
                           int lx1, int ly1, const char *s,
                           unsigned int base, unsigned int ink)
{
    int fx0, fy0, fx1, fy1, w, line_fy;

    sp_rect(v, side, lx0, ly0, lx1, ly1, &fx0, &fy0, &fx1, &fy1);
    w = menu_text_width(s);
    line_fy = fy0 + (fy1 - fy0 - MENU_FONT_LINE) / 2;
    sp_text(v, (fx0 + fx1 - w) / 2, line_fy, s, base, ink, fx0, fx1);
}

/* --- the pieces ------------------------------------------------------------ */

/* A button box: face, frame, centred label, over an explicit LOCAL x range. The
 * range is a parameter rather than SZ_BTN_X0/X1 because the nudge pair is two boxes
 * in one row and they share this code -- a second near-identical function for a
 * half-width button would be the same 8 lines with two constants changed. The pressed
 * pair is the band's own (MENU_BTN_PRESSED / MENU_LABEL_PRESSED), so a press reads the
 * same here as it does on the strip. */
/* A button that rbp has LIT. The face stays the dark MENU_BTN and the accent goes
 * into the FRAME AND THE LABEL -- a backlit control, which is what the three it is
 * used for are: rbp lights a PLAY, a CUE and a SYNC and the hardware beside this
 * panel lights the same three. The alternative, filling the face with the accent,
 * was rejected because it is the same picture as "a finger is on me"
 * (MENU_BTN_PRESSED) and would have made the two indistinguishable -- and because
 * PLAY is lit for as long as a deck is playing, which is most of the time.
 *
 * A BLINK is not a third appearance: the caller resolves it and passes 1 or 0
 * (side_paint.h), so a flashing button is this button alternating with the one
 * below, at rbp's own period. Nothing here keeps a clock, and that is what keeps
 * every paint a pure function of its arguments.
 *
 * THE ACCENT IS PER-CONTROL, and it is a palette CLASS and not a pixel so the caller
 * cannot name a colour this painter has not agreed to (menu_paint.h). The three lit
 * controls are three colours: SYNC keeps the panel's own blue accent, CUE takes the
 * orange and PLAY the green -- the operator's ask of 2026-10-09, and the colours the
 * hardware uses for the same three. An accent is only ever asked for by a button that
 * CAN light (the nudge cells pass MENU_BTN_PRESSED and are never lit); a class that
 * happens to be unused changes nothing about the picture.
 *
 * A LIT FRAME IS ONE PIXEL THICKER -- *"the cue button highlight frame 1pixel thicker
 * ... the sync frame should also be 1pixel thicker"* -- so the highlight reads as a
 * ring rather than as a hairline at the panel's scale. It is deliberately NOT thicker
 * while a finger is on the button: the pressed picture must stay independent of
 * whether rbp has the control lit (test_side.c pins exactly that, because the finger's
 * own feedback is the one thing a state must never change). */
static void sp_button_x(const struct menu_view *v, int side, int lx0, int lx1,
                        int ly0, int ly1, const char *label, int pressed, int lit,
                        int accent_cls)
{
    int fx0, fy0, fx1, fy1;
    unsigned int face = menu_pixel(v->bpp, pressed ? MENU_BTN_PRESSED : MENU_BTN);
    unsigned int ink  = menu_pixel(v->bpp, pressed ? MENU_LABEL_PRESSED
                                      : (lit ? accent_cls : MENU_LABEL));
    int thick = (lit && !pressed) ? SP_FRAME_LIT : 1;

    sp_rect(v, side, lx0, ly0, lx1, ly1, &fx0, &fy0, &fx1, &fy1);
    sp_fill_round(v, fx0, fy0, fx1, fy1, face, SP_ROUND_PX);
    sp_frame_round(v, fx0, fy0, fx1, fy1, ink, thick, SP_ROUND_PX);
    sp_text_center(v, side, lx0, ly0, lx1, ly1, label, face, ink);
}

static void sp_button(const struct menu_view *v, int side, int ly0, int ly1,
                      const char *label, int pressed, int lit, int accent_cls)
{
    sp_button_x(v, side, SZ_BTN_X0, SZ_BTN_X1, ly0, ly1, label, pressed, lit,
                accent_cls);
}

/* A ROUND BUTTON -- the tempo-RANGE and MASTER TEMPO discs. There is no second shape
 * here: the circle is sp_fill_round() with a radius of half the width, which is the same
 * rounding every box on this panel already uses, taken to its limit. `lit` works exactly
 * as sp_button_x()'s does -- the accent goes into the rim and the label, and the pressed
 * picture stays independent of it.
 *
 * THE RADIUS COMES OFF THE DEVICE RECT, not the logical one: a panel's rectangles scale
 * and its marks do not (side_paint.h's rule), and a corner is a mark. On the panel this
 * port ships the two are the same number. */
static void sp_disc(const struct menu_view *v, int side, int lx0, int lx1,
                    int ly0, int ly1, const char *label, int pressed, int lit,
                    int accent_cls)
{
    int fx0, fy0, fx1, fy1, r;
    unsigned int face = menu_pixel(v->bpp, pressed ? MENU_BTN_PRESSED : MENU_BTN);
    unsigned int ink  = menu_pixel(v->bpp, pressed ? MENU_LABEL_PRESSED
                                      : (lit ? accent_cls : MENU_LABEL));

    sp_rect(v, side, lx0, ly0, lx1, ly1, &fx0, &fy0, &fx1, &fy1);
    r = (fx1 - fx0) / 2;
    sp_fill_round(v, fx0, fy0, fx1, fy1, face, r);
    sp_frame_round(v, fx0, fy0, fx1, fy1, ink, (lit && !pressed) ? SP_FRAME_LIT : 1, r);
    sp_text_center(v, side, lx0, ly0, lx1, ly1, label, face, ink);
}

/* THE NUDGE MARK, DRAWN RATHER THAN SET -- and it has to be, because the atlas has no
 * '+'. menu_font.h carries space, 0-9, A-Z, a-z and `.` `/` `-` `_` `:` `?` `=` `&` `@`
 * `#`; '-' is there and '+' is not, and a pair where the minus came from the font and
 * the plus from two rectangles would be visibly two different marks at the same weight.
 * So BOTH are bars: a horizontal one for '-', and the same bar with a vertical one
 * through its middle for '+'. `arm` is the bar's half-length, `thick` its weight --
 * the same 1:1 DEVICE pixels as the glyphs beside them, not the scaled rectangle, so
 * the mark matches the labels on any panel size.
 *
 * Drawn as an even-armed cross so the two cells read as a pair at a glance, which is
 * the whole job of the mark. */
static void sp_sign(const struct menu_view *v, int side, int lx0, int lx1,
                    int ly0, int ly1, int plus, int pressed)
{
    int fx0, fy0, fx1, fy1, cx, cy;
    int arm = SP_SIGN_ARM, thick = SP_SIGN_THICK;
    unsigned int ink = menu_pixel(v->bpp, pressed ? MENU_LABEL_PRESSED : MENU_LABEL);

    sp_rect(v, side, lx0, ly0, lx1, ly1, &fx0, &fy0, &fx1, &fy1);
    cx = (fx0 + fx1) / 2;
    cy = (fy0 + fy1) / 2;
    sp_fill(v, cx - arm, cy - thick / 2, cx + arm + 1, cy - thick / 2 + thick, ink);
    if (plus)
        sp_fill(v, cx - thick / 2, cy - arm, cx - thick / 2 + thick, cy + arm + 1, ink);
}

/* THE PITCH SCALE'S THREE LABELS -- '-', '0' and '+' -- and they are DRAWN, and SMALLER
 * than the atlas can go.
 *
 * WHY DRAWN. The atlas is a single 19 px bake, and menu_font.h's rule is that nothing in
 * this interface scales a glyph: "a scaled bitmap is what this file exists to stop
 * doing". The operator asked for marks smaller than that -- 2026-10-12, *"font is too
 * big, for the + 0"* -- so the honest way to a smaller mark is to draw one rather than to
 * shrink the font. It is the same decision sp_sign() already makes for the nudge pair,
 * which is drawn because the atlas has no '+' at all.
 *
 * ONE SHAPE FAMILY, so the three read as a set: '-' is one bar, '+' is that bar crossed,
 * and '0' is a small ring. The bars are SP_ARM half-long and SP_ARM_T thick; the ring is
 * SP_RING_W x SP_RING_H with a SP_RING_T wall.
 *
 * THE WEIGHT IS THE HASH MARKS' OWN, one pixel, because these are part of the same scale
 * -- the operator's correction (2026-10-12, *"make the font not bold"*). A label drawn
 * heavier than the marks it labels reads as a different kind of thing. */
#define SP_ARM      6    /* a bar's half-length: 13 px across */
#define SP_ARM_T    1    /* ...and its weight, the hash marks' own */
#define SP_RING_W   9
#define SP_RING_H  13
#define SP_RING_T   1

/* HOW FAR OUTSIDE THE MARKS THE LABELS SIT: from the marks' outer ends to the near edge
 * of a label's widest mark, which is a bar at SP_ARM half-long. One number, so the whole
 * column moves together and the three cannot drift out of line with each other. */
#define SP_LBL_GAP  3

#define SP_LBL_MINUS 0
#define SP_LBL_ZERO  1
#define SP_LBL_PLUS  2

/* One label, centred on the LOCAL point (lx, ly) -- which for all three is the row of the
 * mark they belong to, so the scale reads as one line: the top mark carries a '-', the
 * detent a '0', the bottom mark a '+'. */
static void sp_scale_label(const struct menu_view *v, int side, int lx, int ly,
                           int kind)
{
    int fx0, fy0, fx1, fy1;
    unsigned int ink = menu_pixel(v->bpp, MENU_LABEL);

    if (kind == SP_LBL_ZERO) {
        sp_rect(v, side, lx - SP_RING_W / 2, ly - SP_RING_H / 2,
                lx + SP_RING_W / 2, ly + SP_RING_H / 2, &fx0, &fy0, &fx1, &fy1);
        sp_frame_round(v, fx0, fy0, fx1, fy1, ink, SP_RING_T, 4);
        return;
    }
    sp_rect(v, side, lx - SP_ARM, ly - SP_ARM_T / 2, lx + SP_ARM,
            ly - SP_ARM_T / 2 + SP_ARM_T - 1, &fx0, &fy0, &fx1, &fy1);
    sp_fill(v, fx0, fy0, fx1, fy1, ink);
    if (kind == SP_LBL_PLUS) {
        sp_rect(v, side, lx - SP_ARM_T / 2, ly - SP_ARM,
                lx - SP_ARM_T / 2 + SP_ARM_T - 1, ly + SP_ARM,
                &fx0, &fy0, &fx1, &fy1);
        sp_fill(v, fx0, fy0, fx1, fy1, ink);
    }
}

/* The fader: a thin rail, the marks either side of it, then the cap riding on it. The
 * shape is the cdj3k-emu mixer's own fader -- side_paint.h carries the reference, the
 * mapping onto this palette and the one departure (square corners).
 *
 * THE MODE DECIDES THE CAP'S WIDTH and nothing else: the volume cap is the wider of the
 * two and the pitch cap the narrower, which is the difference the operator's *"make them
 * visually different"* is carried by. Both are wider than the rail, so both read as a
 * knob; neither can write outside the view, because sp_set clips every write and sp_rect
 * sorts the two scaled x ends -- no guard is needed for a cap wider than the groove. Both
 * stay inside SZ_FADER_GX0..GX1 (the grab lane), so the knob you can see is the knob you
 * can grab.
 *
 * THE MARKS AND THE RAIL GO DOWN FIRST, so the cap rides over them rather than being
 * striped by them. The rail is the one groove the marks are read against, and the marks
 * are placed from SP_MID_X outward, alternating long and short. */
static void sp_fader(const struct menu_view *v, int side, int fader_v, int mode,
                     int grabbed)
{
    int fx0, fy0, fx1, fy1, cy, hy0, hy1, i;
    unsigned int rail = menu_pixel(v->bpp, MENU_DIV);
    unsigned int mark = menu_pixel(v->bpp, MENU_BORDER);
    unsigned int face = menu_pixel(v->bpp, grabbed ? MENU_BTN_PRESSED : MENU_BTN);
    unsigned int edge = menu_pixel(v->bpp, MENU_BORDER);
    unsigned int grip = menu_pixel(v->bpp, MENU_LABEL);
    int cap_w = SP_CAP_W;
    int half  = cap_w / 2;

    /* The rail, placed with SP_L/SP_R so it is its own mirror -- see their note. */
    sp_rect(v, side, SP_L(SP_RAIL_W / 2), SZ_FADER_Y0,
            SP_R(SP_RAIL_W / 2), SZ_FADER_Y1, &fx0, &fy0, &fx1, &fy1);
    sp_fill(v, fx0, fy0, fx1, fy1, rail);

    /* The marks: SP_MARK_N of them, evenly over the SCALE -- which ends where the cap's
     * centre can reach, not where the travel does (SP_SCALE_Y0/Y1). EVERY ONE THE SAME
     * LENGTH, the operator's own correction (*"make all the lines the same width vs
     * alternating"*), their inner and outer ends the same two distances out for all of
     * them, so the scale reads as one column of equal marks.
     *
     * THE RIGHT-HAND RUN IS THE MIRROR OF THE LEFT ONE and not SP_MID_X plus the same two
     * distances: the two would otherwise sit a pixel apart, and the two DECKS would too. */
    for (i = 0; i < SP_MARK_N; i++) {
        int y = SP_SCALE_Y0 +
                (i * (SP_SCALE_Y1 - SP_SCALE_Y0)) / (SP_MARK_N - 1);

        sp_rect(v, side, SP_L(SP_MARK_RAIL), y, SP_L(SP_MARK_IN), y,
                &fx0, &fy0, &fx1, &fy1);
        sp_fill(v, fx0, fy0, fx1, fy1, mark);
        sp_rect(v, side, SP_R(SP_MARK_IN), y, SP_R(SP_MARK_RAIL), y,
                &fx0, &fy0, &fx1, &fy1);
        sp_fill(v, fx0, fy0, fx1, fy1, mark);
    }

    /* THE PITCH SCALE'S LABELS, AND ONLY IN PITCH MODE: '-' on the TOP mark, '0' on the
     * detent, '+' on the BOTTOM one. One column, on the left side of the hash marks where
     * a printed scale keeps its numbers, and each VERTICALLY CENTRED ON THE MARK IT
     * LABELS -- the operator's own correction, *"it should be vertically centered with the
     * top hash and bottom hash"* -- so the marks and the labels read as one line rather
     * than as a scale with things floating off its ends.
     *
     * THE ENDS ARE THE WHOLE OF THE ORIENTATION, and they are why the operator asked for
     * them: the drawer's pitch fader runs TOP IS SLOW (pitch_state.h carries the
     * correction), so '-' belongs at the top and '+' at the bottom. Without them, which
     * way the control runs is something the operator has to remember.
     *
     * AND THEY ARE SCALE MARKS, SO THEY GO DOWN WITH THE MARKS -- before the cap, which
     * then rides over them. The knob sits on the centre row whenever the tempo is
     * untouched, which is exactly where the zero is, so a zero drawn last would be painted
     * across the face of the knob.
     *
     * ON THE SCREEN'S LEFT, ON BOTH PANELS, and that is the operator's own correction
     * (2026-10-12): *"the +/-/0 indicators on the right panel should also be on the left
     * side not right"*. It is the one mark here that is NOT mirrored, and deliberately:
     * these are the scale's NUMBER-like marks, and a scale whose numbers swap sides when
     * you look at the other deck is one the operator has to re-read every time. So the
     * column is on the OUTER side of the left panel and the INNER side of the right one,
     * which is "the left of the fader" on the glass in both cases.
     *
     * Everything else about them is panel-local and mirrors as usual -- the marks
     * themselves, the rail, the cap. */
    if (mode == SZ_MODE_PITCH) {
        int col = side_screen_x(side, SP_L(SP_MARK_RAIL) - SP_LBL_GAP - SP_ARM);

        sp_scale_label(v, side, col, SP_SCALE_Y0, SP_LBL_MINUS);
        sp_scale_label(v, side, col, SP_SCALE_Y0 + (SP_SCALE_Y1 - SP_SCALE_Y0) / 2,
                       SP_LBL_ZERO);
        sp_scale_label(v, side, col, SP_SCALE_Y1, SP_LBL_PLUS);
    }

    /* The cap's centre, clamped to the SCALE's ends -- which is what puts the line across
     * its middle exactly on the top mark at 100% and on the bottom mark at 0%, and what
     * keeps the cap's own body inside the rail at either extreme. */
    cy = side_fader_y(fader_v);
    if (cy < SP_SCALE_Y0)
        cy = SP_SCALE_Y0;
    if (cy > SP_SCALE_Y1)
        cy = SP_SCALE_Y1;
    hy0 = cy - SP_HANDLE_H / 2;
    hy1 = hy0 + SP_HANDLE_H - 1;

    sp_rect(v, side, SP_L(half), hy0, SP_R(half), hy1, &fx0, &fy0, &fx1, &fy1);
    sp_fill_round(v, fx0, fy0, fx1, fy1, face, SP_ROUND_PX);
    sp_frame_round(v, fx0, fy0, fx1, fy1, edge, 1, SP_ROUND_PX);

    /* THE GRIP, and it is why the cap reads as something to grab: a bar across the face,
     * inset so it never touches the cap's own outline. MENU_LABEL, which is the
     * reference's white. Drawn LAST, so it is the topmost thing on the cap. */
    sp_rect(v, side, SP_L(half - SP_GRIP_INSET), cy - SP_GRIP_H / 2,
            SP_R(half - SP_GRIP_INSET), cy - SP_GRIP_H / 2 + SP_GRIP_H - 1,
            &fx0, &fy0, &fx1, &fy1);
    sp_fill(v, fx0, fy0, fx1, fy1, grip);
}

/* The value, ABOVE the fader (it sits between the nudge pair and the fader now that
 * PLAY has moved to the bottom). 0..100. Digits only: the atlas carries no '%'
 * (menu_font.h's punctuation run), and a number under a fader needs no unit.
 *
 * VOLUME ONLY -- side_paint() does not call this in pitch mode. The shim is never told
 * rbp's tempo range (pitch_state.h), so a number here would be a tempo it cannot know;
 * and "%d" of the fader's own travel is not a tempo either. The knob and the detent mark
 * carry the pitch position instead. */
static void sp_readout(const struct menu_view *v, int side, int fader_v)
{
    char buf[8];

    snprintf(buf, sizeof buf, "%d", side_fader_pct(fader_v));
    sp_text_center(v, side, SZ_READ_X0, SZ_READ_Y0, SZ_READ_X1, SZ_READ_Y1, buf,
                   menu_pixel(v->bpp, MENU_FILL), menu_pixel(v->bpp, MENU_LABEL));
}

int side_paint_ok(const struct menu_view *v)
{
    int min;

    if (!v || !v->pix)
        return 0;
    if (v->pitch <= 0 || v->fb_w <= 0 || v->fb_h <= 0)
        return 0;
    if (v->bpp != 16 && v->bpp != 32)
        return 0;
    if (v->dw <= 0 || v->dh <= 0)
        return 0;
    if (v->bx < 0 || v->by < 0)
        return 0;
    if (v->bx + v->dw > v->fb_w || v->by + v->dh > v->fb_h)
        return 0;
    /* Room for the frame and something inside it. The plane IS the panel here, so
     * both extents are the view's own. */
    if (v->dw < 2 * SP_BORDER + 2 || v->dh < 2 * SP_BORDER + 2)
        return 0;
    /* AND ROOM FOR THE LABELS, which is menu_view_ok()'s rule one level up: the
     * button boxes have to host the font's line box and its widest label. Both are
     * drawn at 1:1 DEVICE pixels -- the atlas is not scaled, only the rectangles are
     * -- so it is the SCALED box that has to be big enough, exactly as the band asks
     * only that its own band host the line box and not that the page be 1280 wide. */
    min = ((SZ_CUE_Y1 - SZ_CUE_Y0 + 1) * v->dh) / SZ_H;
    if (min < MENU_FONT_LINE)
        return 0;
    min = (SZ_BTN_X1 - SZ_BTN_X0 + 1) * v->dw / SZ_W;
    if (min < menu_text_width("PLAY"))
        return 0;
    /* ...and the NARROWEST box in the panel, which is a nudge cell, has to host the
     * DRAWN cross: 2*SP_SIGN_ARM+1 device px wide and as many tall. It is the tightest
     * constraint here and the only one that would silently clip a mark rather than a
     * word, because a bar drawn past the edge is simply cut with no trace. */
    if ((SZ_NUDGE_M_X1 - SZ_NUDGE_M_X0 + 1) * v->dw / SZ_W < 2 * SP_SIGN_ARM + 1)
        return 0;
    min = ((SZ_NUDGE_Y1 - SZ_NUDGE_Y0 + 1) * v->dh) / SZ_H;
    if (min < 2 * SP_SIGN_ARM + 1)
        return 0;
    return 1;
}

void side_paint(const struct menu_view *v, int side, int pressed_hit, int fader_v,
                int mode, int sync, int cue, int play, int mt, int master)
{
    int fx0, fy0, fx1, fy1;
    unsigned int fill  = menu_pixel(v->bpp, MENU_FILL);
    unsigned int ink   = menu_pixel(v->bpp, MENU_LABEL);
    char title[16];

    if (!side_paint_ok(v))
        return;
    if (side != SZ_LEFT && side != SZ_RIGHT)
        return;
    if (fader_v < 0)
        fader_v = 0;
    if (fader_v > 1023)
        fader_v = 1023;
    /* One bit each: the caller's values are booleans, and a value that is merely
     * TRUTHY must not change what is drawn (sp_button_x tests it with a ?:). */
    mode = (mode == SZ_MODE_PITCH) ? SZ_MODE_PITCH : SZ_MODE_VOL;
    sync = sync ? 1 : 0;
    cue  = cue  ? 1 : 0;
    play = play ? 1 : 0;
    mt   = mt   ? 1 : 0;
    master = master ? 1 : 0;

    /* THE BED, AND NOTHING AROUND IT. *"you don't need a white border on the side swipe
     * menus"* (2026-10-06) -- so the panel's own edge is the black MENU_FILL and the
     * only outlines on the glass are the ones each control draws for itself
     * (sp_button/sp_button_x's sp_frame, the fader cap's, the well's). That is the same
     * shape the band and the USB STOP chooser were restyled to the day before, and it
     * is deliberately all four edges: a frame that survived on one side would be the
     * mirror showing through, which is the one thing this panel's drawing can get
     * wrong without any hit test noticing. */
    sp_rect(v, side, 0, 0, SZ_W - 1, SZ_H - 1, &fx0, &fy0, &fx1, &fy1);
    sp_fill(v, fx0, fy0, fx1, fy1, fill);

    /* THE TITLE NAMES THE MODE, because the title IS the toggle -- *"some type of label
     * toggle (fader/pitch)"*. It is the panel's one piece of idle glass and it already
     * names the strip, so the label states which control the fader is at the moment it
     * is read rather than needing a second cell to say so. The channel number stays: it
     * is what tells the two panels apart at a glance. */
    snprintf(title, sizeof title, "CH %d %s", side_channel(side),
             mode == SZ_MODE_PITCH ? "PITCH" : "FADER");
    sp_text_center(v, side, SZ_TITLE_X0, SZ_TITLE_Y0, SZ_TITLE_X1, SZ_TITLE_Y1,
                   title, fill, ink);

    /* THE BEAT-MATCH PAIR, side by side where SYNC used to be alone -- and in SCREEN ORDER
     * on both decks, which is why every x here goes through side_screen_x() (side_zone.h's
     * note). Only SYNC has rbp state behind it (its sync-locked LED); MASTER is a
     * designation with no LED we have identified, so it is a plain momentary button that
     * lights only under a finger. */
    sp_button_x(v, side, side_screen_x(side, SZ_SYNC_X0), side_screen_x(side, SZ_SYNC_X1),
                SZ_SYNC_Y0, SZ_SYNC_Y1, "SYNC", pressed_hit == SZ_HIT_SYNC, sync,
                MENU_BTN_PRESSED);
    sp_button_x(v, side, side_screen_x(side, SZ_MASTER_X0),
                side_screen_x(side, SZ_MASTER_X1), SZ_SYNC_Y0, SZ_SYNC_Y1, "MASTER",
                pressed_hit == SZ_HIT_MASTER, master, MENU_BTN_PRESSED);

    /* THE TWO DISCS, above the fader, AND ONLY ON THE PITCH CONTROL -- *"TR and MT should
     * be on the pitch control"*: a tempo range and a key lock are properties of playing a
     * track at a speed, so they belong to the pitch fader and go with it. They keep their
     * SCREEN order too -- *"same with the TR/MT buttons"* -- so they go through
     * side_screen_x() as the pair above does. MASTER TEMPO lights from rbp's own key-lock
     * predicate -- the same reading the controller's key-lock LED is sent -- and the tempo
     * RANGE disc does not light at all: the shim is never told which range rbp is set to
     * (pitch_state.h), so there is nothing honest to draw on it. */
    if (mode == SZ_MODE_PITCH) {
        sp_disc(v, side, side_screen_x(side, SZ_RNG_X0), side_screen_x(side, SZ_RNG_X1),
                SZ_RND_Y0, SZ_RND_Y1, "TR", pressed_hit == SZ_HIT_RANGE, 0,
                MENU_BTN_PRESSED);
        sp_disc(v, side, side_screen_x(side, SZ_MT_X0), side_screen_x(side, SZ_MT_X1),
                SZ_RND_Y0, SZ_RND_Y1, "MT", pressed_hit == SZ_HIT_MT, mt,
                MENU_BTN_PRESSED);
    }

    /* The nudge pair: two boxes sharing one row, each with its DRAWN mark rather than
     * a label. The '+' cell is the right one -- the operator's own order, left to
     * right, and the caller is what decides which way each one moves the tempo
     * (side_zone.h's SZ_HIT_NUDGE_*). Neither has a state to show: rbp has no LED for
     * a nudge, and a nudge is a thing you do and not a thing you are. */
    sp_button_x(v, side, side_screen_x(side, SZ_NUDGE_M_X0),
                side_screen_x(side, SZ_NUDGE_M_X1), SZ_NUDGE_Y0, SZ_NUDGE_Y1,
                "", pressed_hit == SZ_HIT_NUDGE_M, 0, MENU_BTN_PRESSED);
    sp_sign(v, side, side_screen_x(side, SZ_NUDGE_M_X0),
            side_screen_x(side, SZ_NUDGE_M_X1), SZ_NUDGE_Y0, SZ_NUDGE_Y1, 0,
            pressed_hit == SZ_HIT_NUDGE_M);
    sp_button_x(v, side, side_screen_x(side, SZ_NUDGE_P_X0),
                side_screen_x(side, SZ_NUDGE_P_X1), SZ_NUDGE_Y0, SZ_NUDGE_Y1,
                "", pressed_hit == SZ_HIT_NUDGE_P, 0, MENU_BTN_PRESSED);
    sp_sign(v, side, side_screen_x(side, SZ_NUDGE_P_X0),
            side_screen_x(side, SZ_NUDGE_P_X1), SZ_NUDGE_Y0, SZ_NUDGE_Y1, 1,
            pressed_hit == SZ_HIT_NUDGE_P);

    if (mode != SZ_MODE_PITCH)
        sp_readout(v, side, fader_v);
    sp_fader(v, side, fader_v, mode, pressed_hit == SZ_HIT_FADER);

    sp_button(v, side, SZ_CUE_Y0, SZ_CUE_Y1, "CUE", pressed_hit == SZ_HIT_CUE, cue,
              MENU_CUE);
    sp_button(v, side, SZ_PLAY_Y0, SZ_PLAY_Y1, "PLAY", pressed_hit == SZ_HIT_PLAY,
              play, MENU_PLAY);
}
