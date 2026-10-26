/*
 * g2d_sw.c -- the i.MX G2D calls rbp makes, in software (g2d_sw.h has why).
 */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "g2d_sw.h"

#define EXPORT __attribute__((visibility("default")))

/* NXP's enum g2d_format: the 32-bit ones a blit can copy byte for byte when
 * both sides agree. rbp only ever asks for RGBA8888 -> RGBA8888. */
static int is32(int f)
{
     return (f >= 1 && f <= 4) || (f >= 6 && f <= 9);
}

/* One axis: for output i, the source span [i*s/d, (i+1)*s/d) in 1/256 px. */
static void resample_1d(const uint32_t *in, int in_step, int n_in,
                        uint32_t *out, int out_step, int n_out)
{
     for (int i = 0; i < n_out; i++) {
          long a = (long)i * n_in * 256 / n_out;
          long b = (long)(i + 1) * n_in * 256 / n_out;
          unsigned long acc[4] = { 0, 0, 0, 0 }, tot = 0;
          uint32_t px = 0;

          if (b <= a)
               b = a + 1;
          for (long j = a >> 8; j <= (b - 1) >> 8 && j < n_in; j++) {
               long lo = j * 256 > a ? j * 256 : a;
               long hi = (j + 1) * 256 < b ? (j + 1) * 256 : b;
               unsigned long w = (unsigned long)(hi - lo);
               uint32_t v = in[j * in_step];
               for (int c = 0; c < 4; c++)
                    acc[c] += ((v >> (8 * c)) & 0xff) * w;
               tot += w;
          }
          for (int c = 0; c < 4 && tot; c++)
               px |= (uint32_t)((acc[c] + tot / 2) / tot) << (8 * c);
          out[i * out_step] = px;
     }
}

__attribute__((visibility("hidden")))
int g2d_sw_scale32(const uint32_t *src, int sstride, int sx, int sy, int sw, int sh,
                   uint32_t *dst, int dstride, int dx, int dy, int dw, int dh)
{
     uint32_t *tmp;

     if (!src || !dst || sw <= 0 || sh <= 0 || dw <= 0 || dh <= 0)
          return -1;
     src += (long)sy * sstride + sx;
     dst += (long)dy * dstride + dx;
     if (sw == dw && sh == dh) {
          for (int y = 0; y < sh; y++)
               memcpy(dst + (long)y * dstride, src + (long)y * sstride,
                      (size_t)sw * 4);
          return 0;
     }
     tmp = malloc((size_t)dw * (size_t)sh * 4);       /* rows done, columns next */
     if (!tmp)
          return -1;
     for (int y = 0; y < sh; y++)
          resample_1d(src + (long)y * sstride, 1, sw, tmp + (long)y * dw, 1, dw);
     for (int x = 0; x < dw; x++)
          resample_1d(tmp + x, dw, sh, dst + x, dstride, dh);
     free(tmp);
     return 0;
}

EXPORT int g2d_open(void **handle)
{
     static int h;

     if (!handle)
          return -1;
     *handle = &h;
     return 0;
}

EXPORT int g2d_close(void *handle)
{
     (void)handle;
     return 0;
}

EXPORT int g2d_make_current(void *handle, int type)
{
     (void)handle;
     (void)type;
     return 0;
}

EXPORT int g2d_finish(void *handle)
{
     (void)handle;
     return 0;                         /* every blit is done when it returns */
}

EXPORT struct g2d_buf *g2d_alloc(int size, int cacheable)
{
     struct g2d_buf *b;
     void *p;

     (void)cacheable;
     if (size <= 0 || !(b = calloc(1, sizeof *b)))
          return NULL;
     if (!(p = calloc(1, (size_t)size))) {
          free(b);
          return NULL;
     }
     b->buf_handle = p;
     b->buf_vaddr = p;
     b->buf_paddr = (int)(intptr_t)p;
     b->buf_size = size;
     return b;
}

EXPORT int g2d_free(struct g2d_buf *b)
{
     if (!b)
          return -1;
     free(b->buf_handle);
     free(b);
     return 0;
}

EXPORT int g2d_blit(void *handle, struct g2d_surface *s, struct g2d_surface *d)
{
     (void)handle;
     if (!s || !d || !s->planes[0] || !d->planes[0] || s->rot || d->rot ||
         !is32(s->format) || s->format != d->format)
          return -1;
     return g2d_sw_scale32((const uint32_t *)(intptr_t)s->planes[0], s->stride,
                           s->left, s->top, s->right - s->left, s->bottom - s->top,
                           (uint32_t *)(intptr_t)d->planes[0], d->stride,
                           d->left, d->top, d->right - d->left, d->bottom - d->top);
}
