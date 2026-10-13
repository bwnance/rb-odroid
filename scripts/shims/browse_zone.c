/*
 * browse_zone.c -- see browse_zone.h for the bug, the proof and the shape of the fix.
 */
#define _GNU_SOURCE
#include <stdio.h>

#include "browse_zone.h"
#include "rbp_abi.h"        /* UIBROWSE_GLOBAL, the offsets, BROWSE_MODE_* */
#include "pointsrc.h"       /* pointsrc_log */

/* THE BROWSER'S OWN GEOMETRY, and it is NOT util_zone.c's. The browse list's first
 * row spans y 96..147 -- centre 122 -- with the documented 52 px pitch, so
 * `row = (y - 96) / 52` over twelve rows, y 96..719. UTILITY's list starts at y 50;
 * the browse list sits 46 px lower because it has a column header ("FOLDER"/"TRACK")
 * above it, and reusing util_zone's constant would put every tap on the wrong row.
 *
 * Two measured points pin it, both taken with the panel and read off the glass:
 * a tap at y 122 selected row 0 (VIBE) and one at y 226 selected row 2 (Sundrop). */
#define BZ_ROW0_TOP   96
#define BZ_ROW_PITCH  52
#define BZ_ROWS       12

/* The right pane, in logical x. The browse screen's two panes meet at x 731: the left
 * is 105..730 and the right 735..1279. The left edge is left deliberately loose -- a
 * tap on the boundary is a tap on a boundary either way -- but the RIGHT edge is not:
 * the outer column belongs to the edge drawers' entry zone, which already owns it, and
 * stealing from it would arm a swipe instead of moving a cursor. */
#define BZ_PANE_X0   731
#define BZ_PANE_X1   1230

/* Write one int into rbp's own browse singleton. rbp is non-PIE and this address is
 * fixed, and the value is bounded below before it is written: the cursor is clamped to
 * 0..11 by rbp's own setCursor(), so anything in that range is a state rbp itself can
 * be in, and a row that is drawn is a row the cursor may hold. */
static void bz_write_cursor(int row)
{
    volatile int *cursor = (volatile int *)(UIBROWSE_GLOBAL + 4 + UIBROWSE_OFF_CURSOR);
    volatile int *itemnum = (volatile int *)(UIBROWSE_GLOBAL + UIBROWSE_LIST_STRIDE
                                             + UIBROWSE_OFF_ITEMNUM);
    int n = *itemnum;

    if (n > 0 && row > n - 1)
        row = n - 1;
    if (row > BZ_ROWS - 1)
        row = BZ_ROWS - 1;
    if (row < 0)
        row = 0;

    if (*cursor != row) {
        *cursor = row;
        pointsrc_log("browse: right pane cursor -> row %d (the row tapped)", row);
    }
}

void browse_zone_note(int down, int x, int y)
{
    volatile int *mode = (volatile int *)(UIBROWSE_GLOBAL + UIBROWSE_OFF_MODE);
    int row;

    /* A PRESS ONLY. rbp acts on the row when the finger goes down; writing on the
     * release too would move the cursor a second time after the pane had already
     * followed it somewhere else. */
    if (!down)
        return;
    if (*mode != BROWSE_MODE_BROWSE)
        return;
    if (x < BZ_PANE_X0 || x > BZ_PANE_X1)
        return;
    if (y < BZ_ROW0_TOP || y >= BZ_ROW0_TOP + BZ_ROWS * BZ_ROW_PITCH)
        return;

    row = (y - BZ_ROW0_TOP) / BZ_ROW_PITCH;
    bz_write_cursor(row);
}
