/*
 * deckbadge_paint.c -- see deckbadge_paint.h for the ring, and for why nothing here has to undo
 * itself.
 *
 * The transform is NOT side_paint.c's: that one maps a drawer's own 1280x... panel-local space
 * onto its buffer, and this maps the screen's logical space onto the page. Two lines, and the
 * numbers are the same ones menu_draw.c's own page route uses (v->dw/dh is the picture, v->bx/by
 * is where the picture sits on the screen).
 */
#include "deckbadge_paint.h"

#include <stddef.h>        /* size_t, for the pixel arithmetic */

#include "deckbadge_zone.h"

/* The drawer's lit frame, in device px (side_paint.h's SP_FRAME_LIT). Spelled again rather than
 * included so that a change to the drawer's thickness is a decision someone makes here too,
 * rather than something that happens to this surface without being looked at. */
#define DB_RING_W 2

/* One device pixel. THE SCALING AND THE OFFSET ARE side_paint.c's sp_set(), which is the only
 * other writer of this buffer: `pitch` is in PIXELS and not bytes, so the row is scaled by the
 * pixel size; and v->pix is the START OF THE PICTURE, with v->bx/by saying where the picture sits
 * on the panel -- they are not an offset into it. Getting either wrong does not produce a missing
 * ring, it produces a ring somewhere else on rbp's screen. */
static void db_px(const struct menu_view *v, int x, int y, unsigned int pv)
{
    unsigned char *row;

    if (x < 0 || y < 0 || x >= (int)v->dw || y >= (int)v->dh)
        return;
    row = (unsigned char *)v->pix + (size_t)y * (size_t)v->pitch * (size_t)(v->bpp / 8);
    if (v->bpp == 32)
        *(unsigned int *)(row + (size_t)x * 4) = pv;
    else
        *(unsigned short *)(row + (size_t)x * 2) = (unsigned short)pv;
}

void deckbadge_ring(const struct menu_view *v, int x0, int y0, int x1, int y1, unsigned int pv)
{
    int i, x, y;

    if (!v || !v->pix || v->bpp != 16 || x1 <= x0 || y1 <= y0)
        return;
    for (i = 0; i < DB_RING_W; i++) {
        int yt = y0 + i, yb = y1 - i;

        if (yt >= yb)
            break;                            /* a box too small to carry the ring */
        for (x = x0 + i; x <= x1 - i; x++) {
            db_px(v, x, yt, pv);
            db_px(v, x, yb, pv);
        }
        for (y = yt; y <= yb; y++) {
            db_px(v, x0 + i, y, pv);
            db_px(v, x1 - i, y, pv);
        }
    }
}
