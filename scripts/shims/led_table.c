/*
 * led_table.c -- see led_table.h for what this is for and why it is a separate
 * module. No address of rbp's appears here; the reads are rbp_led.c's.
 */
#include <string.h>

#include "led_table.h"

/* Little-endian u32 out of a byte buffer. memcpy rather than a cast because the
 * contract here is "some bytes rbp wrote", with no alignment promise, and a
 * table this module is handed in a test is a local array. */
static unsigned int le32(const unsigned char *p)
{
     unsigned int v;
     memcpy(&v, p, sizeof v);
     return v;
}

const unsigned char *led_find(const unsigned char *tbl, unsigned int count,
                              unsigned int id, unsigned int ch)
{
     unsigned int i;

     if (!tbl)
          return NULL;
     if (count > LED_DUMP_MAX)
          count = LED_DUMP_MAX;
     for (i = 0; i < count; i++) {
          const unsigned char *e = tbl + (size_t)i * LED_ENTRY_SIZE;
          if (le32(e + 0) == id && le32(e + 4) == ch)
               return e;
     }
     return NULL;
}

unsigned int led_merge_settled(const unsigned char *a, const unsigned char *b,
                               unsigned int count,
                               const unsigned char *prev, unsigned int prev_count,
                               unsigned char *out)
{
     unsigned int n = 0, i;

     if (!a || !b || !out)
          return 0;
     if (count > LED_DUMP_MAX)
          count = LED_DUMP_MAX;
     for (i = 0; i < count; i++) {
          const unsigned char *ea = a + (size_t)i * LED_ENTRY_SIZE;
          const unsigned char *eb = b + (size_t)i * LED_ENTRY_SIZE;
          const unsigned char *keep;

          if (memcmp(ea, eb, LED_ENTRY_SIZE) == 0)
               keep = ea;                      /* agreed: rbp held still */
          else
               keep = led_find(prev, prev_count, le32(ea + 0), le32(ea + 4));
          if (!keep)
               continue;                       /* in flux, and no history */
          memcpy(out + (size_t)n * LED_ENTRY_SIZE, keep, LED_ENTRY_SIZE);
          n++;
     }
     return n;
}

int led_blink_on(unsigned long long now_ms, unsigned int period_ms,
                 unsigned int fallback_ms)
{
     unsigned long long p = period_ms ? period_ms : fallback_ms;

     if (p < 2)
          return 1;
     return (now_ms % p) < (p / 2);
}

/* The DDJ-FLX10's pad palette (flx10-leds.json, 2026-10-24): velocity 4k for
 * k = 1..15 is one fixed colour, 64 is white, and +64 is the dim version of the
 * same colour (127 = dim white). Velocity 0 is NOT off on this unit -- it is the
 * unit's own idle colour, a dim purple -- so an unlit pad looks like the unit
 * with nothing driving it, which is what it would look like anyway.
 *
 * The hues are the operator's names for what each velocity showed, put on a
 * colour wheel: rbp's RGB goes to the nearest one, and anything without much
 * colour in it goes to white. Here and not in rbp_led.c
 * because it is pure, so test_led_table pins it. */
static const struct { int vel; int hue; } flx10_pal[] = {
     {  4, 240 },   /* blue */
     {  8, 200 },   /* light blue */
     { 12, 175 },   /* teal */
     { 16, 155 },   /* greener teal */
     { 20, 120 },   /* green */
     { 24,  80 },   /* yellow-green */
     { 28,  58 },   /* yellow */
     { 32,  35 },   /* orange */
     { 36,  22 },   /* darker orange */
     { 40,   8 },   /* even darker orange -- the closest it has to red */
     { 44, 340 },   /* pink */
     { 48, 322 },   /* light pink */
     { 52, 305 },   /* purpler light pink */
     { 56, 288 },   /* darker purple pink */
     { 60, 268 },   /* darker purple */
};

unsigned char led_flx10_palette(int r, int g, int b)
{
     int mx = r > g ? (r > b ? r : b) : (g > b ? g : b);
     int mn = r < g ? (r < b ? r : b) : (g < b ? g : b);
     int d = mx - mn, hue, best = 64, bestd = 1000;

     if (mx == 0)
          return 0;
     /* saturation below ~0.3: white */
     if (d * 10 < mx * 3)
          return 64;
     if (mx == r)
          hue = 60 * (g - b) / d;
     else if (mx == g)
          hue = 120 + 60 * (b - r) / d;
     else
          hue = 240 + 60 * (r - g) / d;
     if (hue < 0)
          hue += 360;
     for (unsigned k = 0; k < sizeof(flx10_pal) / sizeof(flx10_pal[0]); k++) {
          int dh = hue - flx10_pal[k].hue;
          if (dh < 0)
               dh = -dh;
          if (dh > 180)
               dh = 360 - dh;
          if (dh < bestd) {
               bestd = dh;
               best = flx10_pal[k].vel;
          }
     }
     return (unsigned char)best;
}
