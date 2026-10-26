/*
 * test_splash.c -- splash.c: the picture centred on black, at 16 and 32 bpp, and
 * the clock that ends it.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "splash.h"

static int checks, failures;
#define CHECK(cond, ...) do { checks++; if (!(cond)) { failures++;           \
        printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__);      \
        printf("\n"); } } while (0)

void pointsrc_log(const char *fmt, ...) { (void)fmt; }

int main(void)
{
    static const uint16_t img[2 * 3] = { 0xffff, 0xf800, 0x07e0, 0x001f, 0x0000, 0xffff };
    uint16_t p16[8 * 6];
    uint32_t p32[10 * 6];
    int ok = 1;

    memset(p16, 0x55, sizeof p16);
    splash_paint(p16, 8, 8, 6, 16, img, 2, 3);     /* centred: x 3..4, y 1..3 */
    for (int y = 0; y < 6; y++)
        for (int x = 0; x < 8; x++) {
            int in = x >= 3 && x < 5 && y >= 1 && y < 4;
            uint16_t want = in ? img[(y - 1) * 2 + (x - 3)] : 0;
            ok &= p16[y * 8 + x] == want;
        }
    CHECK(ok, "16 bpp: black, the picture centred and copied as is");

    memset(p32, 0x55, sizeof p32);
    splash_paint(p32, 10, 8, 6, 32, img, 2, 3);    /* pitch wider than the picture */
    CHECK(p32[1 * 10 + 3] == 0xffffffffu && p32[1 * 10 + 4] == 0xffff0000u &&
          p32[2 * 10 + 3] == 0xff00ff00u && p32[2 * 10 + 4] == 0xff0000ffu &&
          p32[0] == 0 && p32[5 * 10 + 7] == 0,
          "32 bpp: RGB565 widened to XRGB, black around (%08x %08x)", p32[13], p32[14]);
    CHECK(p32[8] == 0x55555555u && p32[9] == 0x55555555u,
          "32 bpp: nothing written past the picture's width in the pitch");

    memset(p16, 0x55, sizeof p16);
    splash_paint(p16, 8, 8, 6, 16, NULL, 0, 0);
    ok = 1;
    for (int i = 0; i < 8 * 6; i++)
        ok &= p16[i] == 0;
    CHECK(ok, "no picture: black");

    setenv("SPLASH_FILE", "/nonexistent", 1);
    CHECK(shim_splash_active(), "up from load");
    CHECK(splash_frame(p16, 8, 8, 6, 16, 100000) == 0 && shim_splash_active(),
          "first frame: nothing painted with rbp's framebuffer not diverted, but up");
    CHECK(splash_frame(NULL, 0, 0, 0, 0, 104999) == 0 && shim_splash_active(),
          "still up at 4.999 s, a frame with no page keeping the clock");
    CHECK(splash_frame(p16, 8, 8, 6, 16, 105000) == 0 && !shim_splash_active(),
          "over at 5 s, and the controller is let go");
    CHECK(splash_frame(p16, 8, 8, 6, 16, 105001) == 0, "and stays over");

    printf("%s: %d checks, %d failures\n", failures ? "FAIL" : "ok", checks, failures);
    return failures ? 1 : 0;
}
