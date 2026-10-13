/*
 * vidmask.h -- a rectangle of the panel that shows raw video and MASKS rbp.
 *
 * One symbol: vidmask_tick(), called from menu_draw.c's menu_frame_tick(), which
 * rbp drives itself through FBIO_WAITFORVSYNC -- so this runs at rbp's own frame
 * cadence and needs no thread of its own.
 *
 * INERT UNTIL ASKED, like rbp_load.c: with RB_VIDEO unset (the default) the tick
 * is one comparison and nothing else. See vidmask.c for the whole account.
 */
#ifndef RBPI4B_VIDMASK_H
#define RBPI4B_VIDMASK_H

/* Set the mask up on first call if RB_VIDEO names a file, then advance one frame
 * per 1/RB_VIDEO_FPS. Cheap and safe to call every frame; does nothing at all when
 * no video was asked for, or when the plane could not be had. */
void vidmask_tick(void);

#endif /* RBPI4B_VIDMASK_H */
