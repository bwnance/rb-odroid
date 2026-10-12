/*
 * deckmark_paint.h -- put the bar on a page, and take it off again.
 *
 * EVERYTHING HERE IS PURE: a page pointer and a rectangle in, pixels written, no rbp, no ioctl, no
 * clock. That is what lets test_deckmark.c check the arithmetic against a fake page on a host --
 * and the arithmetic is the part that goes wrong: a rect one row too tall, or a stride read as
 * pixels instead of bytes, produces a bar in the wrong place rather than no bar at all.
 *
 * THIS PAINTS INTO rbp's PAGE, WHICH IS NOT OURS. So the bar has to be REVERSIBLE: deckmark_save()
 * copies what is under it into a caller-owned buffer and deckmark_restore() puts it back. Nothing
 * here decides when that happens -- see deckmark.c.
 */
#ifndef RBPI4B_DECKMARK_PAINT_H
#define RBPI4B_DECKMARK_PAINT_H

/* The bytes one row of `x0..x1` occupies at this depth, which is the size both the save and the
 * restore move per row. Zero for a depth neither of them can draw in. */
int deckmark_row_bytes(int bpp, int x0, int x1);

/* Bytes to hold the whole rectangle, for a caller sizing its buffer. Zero if the rect or depth is
 * one this module will not draw in. */
int deckmark_rect_bytes(int bpp, int pitch, int x0, int y0, int x1, int y1);

/* 0xRRGGBB (the project's palette form) to a pixel of this page's depth: RGB565 at 16, XRGB8888 at
 * 32. An unknown depth yields 0, and the callers below then refuse to draw rather than writing a
 * value that means nothing. */
unsigned int deckmark_pixel(int bpp, unsigned int rgb);

/* Fill the rectangle with `pv`. Returns 0 when the depth is one this module does not draw in. */
int deckmark_fill(unsigned char *page, int pitch, int bpp,
                  int x0, int y0, int x1, int y1, unsigned int pv);

/* Copy the rectangle out into `into` (which must be deckmark_rect_bytes() big). Returns 0 if the
 * depth is unsupported or `into` is NULL. */
int deckmark_save(const unsigned char *page, int pitch, int bpp,
                  int x0, int y0, int x1, int y1, unsigned char *into);

/* Copy `from` back over the rectangle. The inverse of deckmark_save() for the same rect. */
int deckmark_restore(unsigned char *page, int pitch, int bpp,
                     int x0, int y0, int x1, int y1, const unsigned char *from);

#endif /* RBPI4B_DECKMARK_PAINT_H */
