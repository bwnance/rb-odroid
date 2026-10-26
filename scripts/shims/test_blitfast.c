/*
 * test_blitfast.c -- host checks for blitfast.c: the fused span is DirectFB's two
 * stages, pixel for pixel. The two stages are modelled here as their code in
 * libdirectfb-1.4.so.0.0.0 reads (Sop_rgb16_Kto_Dacc @0x78d44, Sacc_to_Aop_rgb32
 * @0x80818), accumulator and all.
 *
 *   make test_blitfast && ./test_blitfast
 */
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "blitfast.h"

/* what blitfast.c's start-up reaches for; the test never installs it */
void pointsrc_log(const char *fmt, ...) { (void)fmt; }
void *rbp_hook(unsigned long t, const uint8_t g[8], void *r) { (void)t; (void)g; (void)r; return NULL; }
int is_rbp_process(void) { return 0; }

static int checks, failures;
#define CHECK(c, ...) do { checks++; if (!(c)) { failures++; printf("FAIL %s:%d: ", __FILE__, __LINE__); \
     printf(__VA_ARGS__); printf("\n"); } } while (0)

typedef struct { uint16_t b, g, r, a; } acc_t;

static void directfb(const uint16_t *s, uint32_t *d, int n, uint32_t skey)
{
     acc_t *acc = malloc((size_t)n * sizeof *acc);
     uint16_t key = (uint16_t)skey;
     for (int i = 0; i < n; i++) {          /* Sop_rgb16_Kto_Dacc */
          uint32_t p = s[i];
          if (p == key) {
               acc[i].a = 0xf000;
               continue;
          }
          acc[i].r = (uint16_t)((p >> 13) | ((p >> 11) << 3));
          acc[i].g = (uint16_t)(((p >> 3) & 0xfc) | ((p << 21) >> 30));
          acc[i].b = (uint16_t)(((p << 3) & 0xff) | ((uint32_t)(p << 27) >> 29));
          acc[i].a = 0xff;
     }
     for (int i = 0; i < n; i++) {          /* Sacc_to_Aop_rgb32 */
          uint32_t px;
          if (acc[i].a > 0xfff)
               continue;
          px = acc[i].r > 0xff ? 0xffff0000u : (0xff000000u | (uint32_t)acc[i].r << 16);
          px |= acc[i].g > 0xff ? 0xff00u : (uint32_t)acc[i].g << 8;
          px |= acc[i].b > 0xff ? 0xffu : acc[i].b;
          d[i] = px;
     }
     free(acc);
}

int main(void)
{
     static uint16_t src[5000];
     static uint32_t a[5000], b[5000];
     srand(1);
     /* every RGB565 value once, key 0xf81f (rbp's) */
     for (int base = 0; base < 65536; base += 4096) {
          for (int i = 0; i < 4096; i++) {
               src[i] = (uint16_t)(base + i);
               a[i] = b[i] = 0x12345678u + (uint32_t)i;
          }
          directfb(src, a, 4096, 0xf81f);
          blitfast_span(src, b, 4096, 0xf81f);
          CHECK(!memcmp(a, b, 4096 * 4), "every pixel value from 0x%04x: as DirectFB", base);
     }
     /* spans of every length 0..40 at every alignment, keys sprinkled in */
     for (int len = 0; len <= 40; len++)
          for (int off = 0; off < 4; off++) {
               for (int i = 0; i < 64; i++) {
                    src[i] = (rand() & 3) ? (uint16_t)rand() : 0xf81f;
                    a[i] = b[i] = (uint32_t)rand();
               }
               directfb(src + off, a + off, len, 0xf81f);
               blitfast_span(src + off, b + off, len, 0xf81f);
               CHECK(!memcmp(a, b, sizeof(uint32_t) * 64), "length %d at offset %d", len, off);
          }
     /* the key's own bits never written, whatever key */
     for (int i = 0; i < 16; i++) { src[i] = 0x0000; a[i] = b[i] = 0xdeadbeef; }
     blitfast_span(src, b, 16, 0x0000);
     CHECK(!memcmp(a, b, 16 * 4), "key 0x0000: black keyed, the destination kept");
     /* a key above 16 bits compares on its low half, as DirectFB's does */
     for (int i = 0; i < 16; i++) { src[i] = 0xf81f; a[i] = b[i] = 1; }
     directfb(src, a, 16, 0x1f81f);
     blitfast_span(src, b, 16, (uint16_t)0x1f81f);
     CHECK(!memcmp(a, b, 16 * 4), "Skey 0x1f81f: the low 16 bits");

     printf("%s: %d checks, %d failures\n", failures ? "FAIL" : "ok", checks, failures);
     return failures ? 1 : 0;
}
