/*
 * test_g2d.c -- the software G2D (g2d_sw.c): the rescale, and the calls as
 * rbp's DecodeScalingJpeg makes them.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "g2d_sw.h"

int g2d_open(void **handle);
int g2d_finish(void *handle);
struct g2d_buf *g2d_alloc(int size, int cacheable);
int g2d_free(struct g2d_buf *b);
int g2d_blit(void *handle, struct g2d_surface *s, struct g2d_surface *d);

static int checks, failures;
#define CHECK(cond, ...) do { checks++; if (!(cond)) { failures++;           \
        printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__);      \
        printf("\n"); } } while (0)

static uint32_t px(int r, int g, int b, int a)
{
     return (uint32_t)r | (uint32_t)g << 8 | (uint32_t)b << 16 | (uint32_t)a << 24;
}

int main(void)
{
     /* 2x2 -> 1x1: each channel averaged on its own */
     {
          uint32_t s[4] = { px(0, 0, 0, 255), px(100, 0, 0, 255),
                            px(0, 200, 0, 255), px(100, 200, 40, 255) }, d = 0;
          CHECK(g2d_sw_scale32(s, 2, 0, 0, 2, 2, &d, 1, 0, 0, 1, 1) == 0 &&
                d == px(50, 100, 10, 255), "2x2 -> 1x1 averages (%08x)", d);
     }
     /* 3 -> 2: fractional coverage */
     {
          uint32_t s[3] = { px(0, 0, 0, 0), px(90, 0, 0, 0), px(180, 0, 0, 0) }, d[2];
          g2d_sw_scale32(s, 3, 0, 0, 3, 1, d, 2, 0, 0, 2, 1);
          CHECK((d[0] & 0xff) == 30 && (d[1] & 0xff) == 150,
                "3 -> 2: 1.5 px each (%u %u)", d[0] & 0xff, d[1] & 0xff);
     }
     /* same size: copied; rectangles and strides honoured */
     {
          uint32_t s[4 * 3], d[5 * 4];
          for (int i = 0; i < 12; i++)
               s[i] = (uint32_t)i;
          memset(d, 0, sizeof d);
          g2d_sw_scale32(s, 4, 1, 1, 2, 2, d, 5, 2, 1, 2, 2);
          CHECK(d[1 * 5 + 2] == 5 && d[1 * 5 + 3] == 6 && d[2 * 5 + 2] == 9 &&
                d[2 * 5 + 3] == 10 && d[0] == 0 && d[1 * 5 + 1] == 0,
                "a 2x2 rectangle copied into a rectangle, nothing else touched");
     }
     /* upscale of a flat colour stays flat */
     {
          uint32_t s = px(12, 34, 56, 255), d[9];
          int flat = 1;
          g2d_sw_scale32(&s, 1, 0, 0, 1, 1, d, 3, 0, 0, 3, 3);
          for (int i = 0; i < 9; i++)
               flat &= d[i] == s;
          CHECK(flat, "1x1 -> 3x3 of one colour");
     }
     CHECK(g2d_sw_scale32(NULL, 1, 0, 0, 1, 1, NULL, 1, 0, 0, 1, 1) == -1 &&
           g2d_sw_scale32((uint32_t *)&checks, 1, 0, 0, 0, 1, (uint32_t *)&checks, 1,
                          0, 0, 1, 1) == -1, "empty or missing: refused");

     /* as DecodeScalingJpeg calls it: open, two buffers, a 4x4 -> 240x240 blit */
     {
          void *h = NULL;
          struct g2d_buf *sb, *db;
          struct g2d_surface s, d;
          uint32_t *sp;
          int ok = 1;

          CHECK(g2d_open(&h) == 0 && h, "g2d_open succeeds");
          sb = g2d_alloc(4 * 4 * 4, 0);
          db = g2d_alloc(240 * 240 * 4, 0);
          CHECK(sb && db && sb->buf_paddr == (int)(intptr_t)sb->buf_vaddr &&
                db->buf_size == 230400, "g2d_alloc: memory, paddr = the pointer");
          sp = sb->buf_vaddr;
          for (int i = 0; i < 16; i++)
               sp[i] = px(200, 100, 50, 255);
          memset(&s, 0, sizeof s);
          memset(&d, 0, sizeof d);
          s.format = d.format = 1;
          s.planes[0] = sb->buf_paddr;
          s.right = s.bottom = s.stride = s.width = s.height = 4;
          d.planes[0] = db->buf_paddr;
          d.right = d.bottom = d.stride = d.width = d.height = 240;
          CHECK(g2d_blit(h, &s, &d) == 0 && g2d_finish(h) == 0, "blit and finish");
          for (int i = 0; i < 240 * 240; i++)
               ok &= ((uint32_t *)db->buf_vaddr)[i] == px(200, 100, 50, 255);
          CHECK(ok, "the 240x240 result is the picture, scaled");
          d.format = 0;
          CHECK(g2d_blit(h, &s, &d) == -1, "mixed formats refused");
          CHECK(g2d_free(sb) == 0 && g2d_free(db) == 0, "g2d_free");
     }

     printf("%s: %d checks, %d failures\n", failures ? "FAIL" : "ok", checks, failures);
     return failures ? 1 : 0;
}
