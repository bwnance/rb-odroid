/*
 * g2d_sw.h -- the NXP i.MX G2D calls rbp makes, done in software.
 *
 * rbp scales every piece of artwork it shows through the RX3's GPU: its
 * DecodeScalingJpeg @0x176208 decodes the JPEG with libjpeg, copies it into a
 * g2d_alloc buffer and has g2d_blit scale it to 240x240 (resize @0x176160 does
 * the same for other sizes). libg2d.so.0.8 drives the i.MX GPU, which no board
 * this runs on has, so g2d_open failed, DecodeScalingJpeg returned -2 and the
 * browser drew its default cover for every track (operator, 2026-10-26).
 * fbshim.so exports the seven calls rbp imports and, being preloaded, is what
 * rbp binds them to: buffers are plain memory whose "physical address" is the
 * pointer (rbp is a 32-bit process), and a blit is a CPU rescale.
 *
 * The layouts are NXP's g2d.h as rbp fills them (resize builds two 60-byte
 * surfaces on its stack): format, planes[3], left, top, right, bottom,
 * stride, width, height, blendfunc, global_alpha, clrcolor, rot.
 */
#ifndef RBPI4B_G2D_SW_H
#define RBPI4B_G2D_SW_H

#include <stdint.h>

struct g2d_buf {
     void *buf_handle;
     void *buf_vaddr;
     int buf_paddr;
     int buf_size;
};

struct g2d_surface {
     int format;            /* enum g2d_format: 1 RGBA8888, the one rbp uses */
     int planes[3];         /* "physical" addresses: here, the pointers */
     int left, top, right, bottom;
     int stride;            /* in pixels */
     int width, height;
     int blendfunc, global_alpha, clrcolor, rot;
};

/* The rescale a blit does, exposed for the test: the 32-bit pixels of the
 * source rectangle (sx, sy, sw x sh, row stride sstride pixels) into the
 * destination one, each of the four bytes on its own. Every output pixel is
 * the average of the source area it covers, fractions included -- a clean
 * downscale, and a blend at the edges of an upscale. 0, or -1 for an empty
 * rectangle or no memory. */
int g2d_sw_scale32(const uint32_t *src, int sstride, int sx, int sy, int sw, int sh,
                   uint32_t *dst, int dstride, int dx, int dy, int dw, int dh);

#endif /* RBPI4B_G2D_SW_H */
