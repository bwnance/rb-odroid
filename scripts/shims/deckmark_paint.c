/*
 * deckmark_paint.c -- see deckmark_paint.h for the contract. Nothing here knows what a deck is.
 */
#include "deckmark_paint.h"

#include <stddef.h>
#include <string.h>

int deckmark_row_bytes(int bpp, int x0, int x1)
{
    int px;

    if (bpp != 16 && bpp != 32)
        return 0;
    if (x1 < x0)
        return 0;
    px = x1 - x0 + 1;
    return px * (bpp / 8);
}

int deckmark_rect_bytes(int bpp, int pitch, int x0, int y0, int x1, int y1)
{
    int row = deckmark_row_bytes(bpp, x0, x1);

    if (row == 0 || pitch <= 0 || y1 < y0)
        return 0;
    return row * (y1 - y0 + 1);
}

unsigned int deckmark_pixel(int bpp, unsigned int rgb)
{
    unsigned int r = (rgb >> 16) & 0xffu, g = (rgb >> 8) & 0xffu, b = rgb & 0xffu;

    if (bpp == 16)
        return (unsigned int)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
    if (bpp == 32)
        return rgb;                     /* the page is 16 bpp in practice; 32 is the cursor's
                                         * form and is carried through unswizzled */
    return 0;
}

static unsigned char *cell(unsigned char *page, int pitch, int bpp, int x, int y)
{
    return page + (size_t)y * (size_t)pitch + (size_t)x * (size_t)(bpp / 8);
}

int deckmark_fill(unsigned char *page, int pitch, int bpp,
                  int x0, int y0, int x1, int y1, unsigned int pv)
{
    int row = deckmark_row_bytes(bpp, x0, x1);
    int y;
    int i, n;

    if (row == 0 || page == NULL || pitch <= 0 || y1 < y0)
        return 0;
    n = (x1 - x0 + 1);
    for (y = y0; y <= y1; y++) {
        unsigned char *p = cell(page, pitch, bpp, x0, y);

        if (bpp == 16) {
            unsigned short v = (unsigned short)pv;

            for (i = 0; i < n; i++)
                ((unsigned short *)p)[i] = v;
        } else {
            for (i = 0; i < n; i++)
                ((unsigned int *)p)[i] = pv;
        }
    }
    return 1;
}

int deckmark_save(const unsigned char *page, int pitch, int bpp,
                  int x0, int y0, int x1, int y1, unsigned char *into)
{
    int row = deckmark_row_bytes(bpp, x0, x1);
    int y;

    if (row == 0 || page == NULL || into == NULL || pitch <= 0 || y1 < y0)
        return 0;
    for (y = y0; y <= y1; y++)
        memcpy(into + (size_t)(y - y0) * (size_t)row,
               (const unsigned char *)cell((unsigned char *)page, pitch, bpp, x0, y), (size_t)row);
    return 1;
}

int deckmark_restore(unsigned char *page, int pitch, int bpp,
                     int x0, int y0, int x1, int y1, const unsigned char *from)
{
    int row = deckmark_row_bytes(bpp, x0, x1);
    int y;

    if (row == 0 || page == NULL || from == NULL || pitch <= 0 || y1 < y0)
        return 0;
    for (y = y0; y <= y1; y++)
        memcpy(cell(page, pitch, bpp, x0, y),
               from + (size_t)(y - y0) * (size_t)row, (size_t)row);
    return 1;
}
