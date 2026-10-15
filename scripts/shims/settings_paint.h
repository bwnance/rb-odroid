/*
 * settings_paint.h -- the SETTINGS panel's image.
 *
 * A drawing module of the family menu_paint.c and side_paint.c belong to, and
 * written to the same rule: pure, a function of its arguments and of nothing else, in
 * the same palette (menu_pixel(), which is public for exactly this reason). "Painting it
 * twice paints it once" is load-bearing -- the panel is presented on a plane whose buffer
 * is single-buffered, so a paint that is not idempotent is a paint that tears. Every
 * pixel of the box is written on every call.
 *
 * THE VIEW'S PICTURE RECT IS THE BOX, and the picture pointer is PRE-OFFSET to the box's
 * own top-left pixel (bx/by stay 0): the caller sets
 * `dw`/`dh` to the box's own size in framebuffer pixels (SB_W/SB_H logical px at the
 * page's scale) and `bx`/`by` to where that lands. Logical box coordinates are then
 * scaled by dw/SB_W and dh/SB_H, so the same code draws the panel on its own plane and
 * on the page, and the plane is created at exactly the size the picture says.
 */
#ifndef RBPI4B_SETTINGS_PAINT_H
#define RBPI4B_SETTINGS_PAINT_H

#include "menu_paint.h"        /* struct menu_view, menu_pixel() */
#include "settings_zone.h"     /* SB_* geometry, struct settings_state, SB_ROWS */

/* THE ROWS HAVE NO MARKS, and that is the operator's own call (2026-10-14): *"do we even
 * need icons for the settings menu?"* -- asked after seeing them on the glass, and the
 * answer is no. The first version drew a cycle arrow on the four rows that change in place
 * and a chevron on the one that raises the USB STOP chooser; both went, for three reasons
 * that outlive the decision:
 *
 *   * the VALUE already carries the state ("INTERNAL", "ON"), so a mark restates it;
 *   * one rule for the whole panel is easier to learn than two -- every row's value is
 *     tappable and that is all there is to know -- where two marks are a legend that has to
 *     be read before the panel can be used;
 *   * and they crowded the value against the frame (the operator: *"there should be some
 *     padding next to the icons on the right side"*), so removing them hands that space
 *     back to the values.
 *
 * A row that OPENS a box is therefore signalled by its label and not by a glyph, and the
 * box it raises repeats the same two words in its own title ("USB STOP"). If the drill-down
 * ever wants marking again, the cheap version is one small chevron on that row alone. */

/* The label's inset from its row's left edge, and the value's from its right edge -- the
 * same number, so a row reads as one line of two columns ("MIXER ROUTE   INTERNAL") rather
 * than as text pushed into a corner. SP_VALUE_RIGHT is also what the size refusal keeps the
 * widest value clear of the frame by, so the padding is a property of the panel and not of
 * whichever string happens to be in force. */
#define SP_LABEL_X     12
#define SP_VALUE_RIGHT 12

/* Is this view something settings_paint() can draw into? The box has a title, a rule and
 * SB_ROWS rows of SB_ROW_H logical px, each of which has to host the font's line box
 * (menu_font.h's MENU_FONT_LINE) after scaling, or the labels collide with the frames
 * around them. And every row has to hold its OWN LABEL whole and its WIDEST VALUE whole
 * -- measured against settings_row_widest_value() and not against the value in force,
 * because the caller asks this once and paints only if it answers yes, so a value that
 * outgrew its column would take the whole panel off the glass rather than be clipped
 * (the rule every box here keeps).
 *
 * menu_paint.h's menu_view_ok() answers the same kind of question for the band and
 * side_paint.h's side_paint_ok() for the drawer. */
int settings_paint_ok(const struct menu_view *v);

/* The box's size in framebuffer pixels for a page of dw x dh -- the size the plane has to
 * be set up at. The caller asks this rather than computing it twice. */
int settings_paint_w(int page_dw);
int settings_paint_h(int page_dh);

/* Draw the panel into `v`.
 *
 *   S            rbp's and the files' answers (settings_zone.h): each row's value comes
 *                from settings_row_text(), and a row that cannot be operated is drawn in
 *                the palette's two OFF classes -- the chooser's idiom for a dead button,
 *                which is what makes the mixer row's refusal visible rather than silent.
 *   pressed_row  the value under the finger, 1..SB_ROWS, or 0 -- settings_pressed(),
 *                which is menu_pressed()'s question asked of this module. A pressed row is
 *                never an off row: a finger on a dim value still has to see that it landed
 *                somewhere, or the panel reads as dead rather than as refusing.
 *   flash_row    the lit half of a running HOLD's blink, or 0 -- settings_hold_row() when
 *                settings_hold_flash() is 1. It wears the SAME pressed pair, which is the
 *                whole of the flash: a USB row alternates between that pair and its normal
 *                face every ST_HOLD_FLASH_MS until the hold fires, so the operator can see
 *                the three seconds running -- the same blink the chooser's buttons wore,
 *                because this is the same gesture. The two arguments are never the
 *                same row -- settings_pressed() answers 0 while a hold runs -- and if they
 *                ever were, pressed_row gives way.
 *
 * Every pixel of the view is written. Does nothing when settings_paint_ok() is 0. */
void settings_paint(const struct menu_view *v, const struct settings_state *S,
                    int pressed_row, int flash_row);

#endif /* RBPI4B_SETTINGS_PAINT_H */
