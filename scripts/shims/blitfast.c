/*
 * blitfast.c -- DirectFB's colour-keyed RGB16 -> RGB32 blit, in one pass
 * (blitfast.h).
 *
 * DirectFB's software renderer (gfx/generic) runs a blit as a pipeline of stage
 * functions over each span, all handed the same GenefxState. In this build
 * (libdirectfb-1.4.so.0.0.0, md5 8f1b71b3..., the RX3's) its layout, read off the
 * two stages' code:
 *   +0x000  funcs[32]  the pipeline, NULL-terminated
 *   +0x080  length     the span, in pixels
 *   +0x0d8  Aop        the destination
 *   +0x0f4  Astep      its step, 1 forwards
 *   +0x11c  Skey       the source colour key (low 16 bits for RGB16)
 *   +0x154  Dacc       the accumulator the first stage writes...
 *   +0x158  Sacc       ...and the second reads
 *   +0x15c  -> Sop     a pointer to the source pointer
 *   +0x164  Ostep      the source step, 1 forwards
 * Sop_rgb16_Kto_Dacc widens r5/g6/b5 by bit replication ((r << 3) | (r >> 2), ...)
 * with alpha 0xff, and flags a keyed pixel with alpha 0xf000; Sacc_to_Aop_rgb32
 * writes 0xff000000 | r << 16 | g << 8 | b for each unflagged one (alpha > 0xfff
 * is skipped). The fused span below is exactly that.
 */
#define _GNU_SOURCE
#include "blitfast.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#if defined(__ARM_NEON__) || defined(__ARM_NEON)
#include <arm_neon.h>
#endif

#include "envutil.h"
#include "pointsrc.h"       /* pointsrc_log() */
#include "rbp_hook.h"
#include "rbp_key.h"        /* is_rbp_process() */

#define DFB_SOP_OFF      0x00078d44ul    /* Sop_rgb16_Kto_Dacc */
#define DFB_SACC_OFF     0x00080818ul    /* Sacc_to_Aop_rgb32 */
/* ldr ip,[r0,#128] ; ldr r3,[r0,#348]  -- nothing pc-relative */
static const uint8_t sop_guard[8]  = { 0x80, 0xc0, 0x90, 0xe5, 0x5c, 0x31, 0x90, 0xe5 };
/* ldr ip,[r0,#128] ; push {r4,r5,r6,lr} -- nothing pc-relative */
static const uint8_t sacc_guard[8] = { 0x80, 0xc0, 0x90, 0xe5, 0x70, 0x40, 0x2d, 0xe9 };

#define G_FUNCS  0x000
#define G_LENGTH 0x080
#define G_AOP    0x0d8
#define G_ASTEP  0x0f4
#define G_SKEY   0x11c
#define G_DACC   0x154
#define G_SACC   0x158
#define G_SOPP   0x15c
#define G_OSTEP  0x164

#define AT(g, off, type) (*(type *)((char *)(g) + (off)))

static inline uint32_t widen(uint16_t p)
{
     uint32_t r = p >> 11, g = (p >> 5) & 0x3f, b = p & 0x1f;
     return 0xff000000u | ((r << 3 | r >> 2) << 16) | ((g << 2 | g >> 4) << 8) | (b << 3 | b >> 2);
}

void blitfast_span(const uint16_t *s, uint32_t *d, int n, uint16_t key)
{
     int i = 0;
#if defined(__ARM_NEON__) || defined(__ARM_NEON)
     const uint16x8_t k = vdupq_n_u16(key);
     const uint32x4_t a = vdupq_n_u32(0xff000000u);
     for (; i + 8 <= n; i += 8) {
          uint16x8_t p = vld1q_u16(s + i);
          uint16x8_t keyed = vceqq_u16(p, k);
          uint16x8_t r = vshrq_n_u16(p, 11);
          uint16x8_t g = vandq_u16(vshrq_n_u16(p, 5), vdupq_n_u16(0x3f));
          uint16x8_t b = vandq_u16(p, vdupq_n_u16(0x1f));
          r = vorrq_u16(vshlq_n_u16(r, 3), vshrq_n_u16(r, 2));
          g = vorrq_u16(vshlq_n_u16(g, 2), vshrq_n_u16(g, 4));
          b = vorrq_u16(vshlq_n_u16(b, 3), vshrq_n_u16(b, 2));
          /* 0x00rr / 0x00gg / 0x00bb in u16 lanes -> 0xffrrggbb in u32 lanes */
          uint32x4_t lo = vorrq_u32(a, vorrq_u32(vshlq_n_u32(vmovl_u16(vget_low_u16(r)), 16),
                          vorrq_u32(vshlq_n_u32(vmovl_u16(vget_low_u16(g)), 8),
                                    vmovl_u16(vget_low_u16(b)))));
          uint32x4_t hi = vorrq_u32(a, vorrq_u32(vshlq_n_u32(vmovl_u16(vget_high_u16(r)), 16),
                          vorrq_u32(vshlq_n_u32(vmovl_u16(vget_high_u16(g)), 8),
                                    vmovl_u16(vget_high_u16(b)))));
          /* keyed lanes keep what the destination has */
          uint32x4_t klo = vreinterpretq_u32_s32(vmovl_s16(vreinterpret_s16_u16(vget_low_u16(keyed))));
          uint32x4_t khi = vreinterpretq_u32_s32(vmovl_s16(vreinterpret_s16_u16(vget_high_u16(keyed))));
          vst1q_u32(d + i, vbslq_u32(klo, vld1q_u32(d + i), lo));
          vst1q_u32(d + i + 4, vbslq_u32(khi, vld1q_u32(d + i + 4), hi));
     }
#endif
     for (; i < n; i++)
          if (s[i] != key)
               d[i] = widen(s[i]);
}

/* ---- the detours ------------------------------------------------------------ */

typedef void (*stage_fn)(void *gfxs);
static stage_fn orig_sop, orig_sacc;
static unsigned long sop_addr, sacc_addr;
static void *fused_gfxs;            /* the span the next Sacc_to_Aop_rgb32 skips */
static int enabled;
static unsigned int checked;

static int pipeline_is_ours(void *gfxs)
{
     void **funcs = &AT(gfxs, G_FUNCS, void *);

     return (unsigned long)funcs[0] == sop_addr && (unsigned long)funcs[1] == sacc_addr &&
            funcs[2] == NULL && AT(gfxs, G_OSTEP, int) == 1 && AT(gfxs, G_ASTEP, int) == 1 &&
            AT(gfxs, G_DACC, void *) == AT(gfxs, G_SACC, void *);
}

/* DirectFB's own two stages for this span, into `out` instead of the layer */
static void reference_span(void *gfxs, uint32_t *out)
{
     void *aop = AT(gfxs, G_AOP, void *);

     AT(gfxs, G_AOP, void *) = out;
     orig_sop(gfxs);
     orig_sacc(gfxs);
     AT(gfxs, G_AOP, void *) = aop;
}

static void hooked_sop(void *gfxs)
{
     if (enabled && pipeline_is_ours(gfxs)) {
          int n = AT(gfxs, G_LENGTH, int);
          const uint16_t *s = *AT(gfxs, G_SOPP, const uint16_t **);
          uint32_t *d = AT(gfxs, G_AOP, uint32_t *);
          uint16_t key = (uint16_t)AT(gfxs, G_SKEY, uint32_t);

          if (n <= 0)
               return;
          if (checked < BLITFAST_CHECK_SPANS && n <= 4096) {
               static uint32_t ref[4096];
               memcpy(ref, d, (size_t)n * 4);
               reference_span(gfxs, ref);
               blitfast_span(s, d, n, key);
               checked++;
               if (memcmp(ref, d, (size_t)n * 4)) {
                    for (int i = 0; i < n; i++)
                         if (ref[i] != d[i]) {
                              pointsrc_log("blitfast: span of %d differs at %d: 0x%08x, "
                                           "DirectFB 0x%08x (src 0x%04x key 0x%04x) -- off",
                                           n, i, d[i], ref[i], s[i], key);
                              break;
                         }
                    memcpy(d, ref, (size_t)n * 4);   /* DirectFB's, as it would be */
                    enabled = 0;
               } else if (checked == BLITFAST_CHECK_SPANS) {
                    pointsrc_log("blitfast: %u spans matched DirectFB exactly -- trusted",
                                 checked);
               }
               fused_gfxs = gfxs;
               return;
          }
          blitfast_span(s, d, n, key);
          fused_gfxs = gfxs;
          return;
     }
     orig_sop(gfxs);
}

static void hooked_sacc(void *gfxs)
{
     if (fused_gfxs && fused_gfxs == gfxs) {
          fused_gfxs = NULL;                       /* done in the first stage */
          return;
     }
     orig_sacc(gfxs);
}

/* libdirectfb's load address: its mapping at file offset 0. Not dlsym/dladdr --
 * rbp is not position-independent, so DirectFBCreate resolves to rbp's own PLT
 * entry for it, and dladdr names rbp (measured on the unit 2026-10-26). */
static unsigned long dfb_base(void)
{
     FILE *f = fopen("/proc/self/maps", "r");
     char line[512];
     unsigned long lo, off, base = 0;

     if (!f)
          return 0;
     while (fgets(line, sizeof line, f))
          if (strstr(line, "/libdirectfb-1.4.so") &&
              sscanf(line, "%lx-%*[0-9a-f] %*s %lx", &lo, &off) == 2 && off == 0) {
               base = lo;
               break;
          }
     fclose(f);
     return base;
}

__attribute__((constructor)) static void blitfast_init(void)
{
     unsigned long base;

     if (!is_rbp_process() || !atoi(env_str("BLIT_FAST", "1")))
          return;
     /* a switch without the config: the file there at start, and the blit stays
      * DirectFB's (an A/B for the waveform jumps, 2026-10-28) */
     if (access("/run/rbp/blitfast.off", F_OK) == 0) {
          pointsrc_log("blitfast: /run/rbp/blitfast.off -- off");
          return;
     }
     base = dfb_base();
     if (!base) {
          pointsrc_log("blitfast: no DirectFB in this process -- off");
          return;
     }
     sop_addr = base + DFB_SOP_OFF;
     sacc_addr = base + DFB_SACC_OFF;
     /* the guards are this build's: any other DirectFB fails them, and is left be */
     orig_sacc = (stage_fn)rbp_hook(sacc_addr, sacc_guard, (void *)hooked_sacc);
     if (!orig_sacc) {
          pointsrc_log("blitfast: Sacc_to_Aop_rgb32 is not the one measured -- off");
          return;
     }
     orig_sop = (stage_fn)rbp_hook(sop_addr, sop_guard, (void *)hooked_sop);
     if (!orig_sop) {
          pointsrc_log("blitfast: Sop_rgb16_Kto_Dacc is not the one measured -- off");
          return;                       /* the Sacc detour alone passes everything on */
     }
     enabled = 1;
     pointsrc_log("blitfast: DirectFB's RGB16 colour-keyed -> RGB32 blit fused (base 0x%lx)",
                  base);
}
