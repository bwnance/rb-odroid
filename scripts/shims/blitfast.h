/*
 * blitfast.h -- DirectFB's colour-keyed RGB16 -> RGB32 blit, in one pass.
 *
 * rbp's images are RGB565 with a colour key; this port's layer is RGB32 (rb.conf's
 * RB_FB_LIE_BPP: the deck strip's frame needs it). DirectFB 1.4 has no hardware
 * driver here (directfbrc: no-hardware, and no gfxdrivers at all) and no direct
 * routine for that pair, so every such blit runs its generic two-stage pipeline:
 * Sop_rgb16_Kto_Dacc expands each source pixel into an accumulator, flagging the
 * keyed ones, and Sacc_to_Aop_rgb32 packs the unflagged ones into the layer.
 * Sampled on the unit 2026-10-26: those two were 302 of 500 samples of rbp's
 * gui_task, the thread at ~90% of a core.
 *
 * This detours the first stage: when the pipeline is exactly those two stages,
 * forwards, it writes the 32-bit pixels straight to the destination (NEON, eight
 * at a time) and the second stage is skipped for that span. The result is the
 * same pixels -- each channel widened by bit replication, alpha 0xFF, keyed
 * pixels left as they were. Any other pipeline goes through DirectFB untouched.
 * The first BLITFAST_CHECK_SPANS fused spans are also run through DirectFB's own
 * stages into a scratch row and compared; one difference and it switches itself
 * off for the run, and says so.
 *
 * RB_BLIT_FAST=0 leaves DirectFB as it ships.
 */
#ifndef RBPI4B_BLITFAST_H
#define RBPI4B_BLITFAST_H

#include <stdint.h>

#define BLITFAST_CHECK_SPANS 2000

/* `n` RGB565 pixels at `s` to RGB32 at `d`, those equal to `key` left as `d` has
 * them. Pure, for the test. */
void blitfast_span(const uint16_t *s, uint32_t *d, int n, uint16_t key);

#endif /* RBPI4B_BLITFAST_H */
