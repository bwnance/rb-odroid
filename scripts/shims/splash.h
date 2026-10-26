/*
 * splash.h -- a picture over everything for the first seconds of rbp.
 *
 * rbp's first seconds on the glass are half-made screens, and the controller's
 * pads light rbp's start-up hot cues blue for a few seconds; the operator's
 * answer (2026-10-26) is to cover it, "frontend-dev style": black, the rekordbox
 * logo (or RB_SPLASH_IMAGE, made by rbassets.py), over every rbp frame for
 * SPLASH_MS after the first one. While it is up the controls shim (knobshim.so)
 * sends the controller no LEDs and takes no input from it -- shim_splash_active()
 * is exported for that, and knobshim declares it weak.
 */
#ifndef RBPI4B_SPLASH_H
#define RBPI4B_SPLASH_H

#include <stdint.h>

/* Once per rbp frame, on rbp's render thread: paint the splash on the REAL
 * framebuffer's page (`page`, the top-left of the w x h picture, `pitch` its
 * stride in pixels, `bpp` 16 or 32; NULL paints nothing but keeps the clock) while
 * it is up, and when it ends give rbp its framebuffer back (splash.c's header).
 * Answers 1 if it painted. The first call is rbp's first frame and starts the
 * clock. Nothing is painted unless rbp's mapping was diverted. */
int splash_frame(void *page, int pitch, int w, int h, int bpp, unsigned long long now_ms);

/* 1 from load until the splash is over -- SPLASH_MS after rbp's first frame, or
 * SPLASH_NO_FRAME_MS after load if rbp never draws (a headless unit). */
int shim_splash_active(void);

/* The picture into `out`, centred on black: what splash_frame() draws. Pure, for
 * the test: `img` is iw x ih RGB565 (NULL draws black only). */
void splash_paint(void *out, int pitch, int w, int h, int bpp,
                  const uint16_t *img, int iw, int ih);

#endif /* RBPI4B_SPLASH_H */
