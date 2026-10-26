/*
 * pointsrc.h — where pointer input comes from.
 *
 * On the SC Live 4 this was a hardcoded /dev/input/event0 carrying an ILI2117
 * touchscreen. A Raspberry Pi driving an HDMI monitor has no touchscreen at all
 * unless one is plugged in, and what the operator will actually attach is a
 * mouse or a trackpad. So this file owns two device kinds behind one interface:
 *
 *   absolute — a touchscreen or tablet: the device reports *where*.
 *   relative — a mouse or trackpad: the device reports *how far*, and the shim
 *              accumulates that into a pointer position.
 *
 * Both end at tscfake_emit(), which is the only thing rbp sees. Discovery is
 * re-run whenever the current device disappears, so unplugging and replugging
 * works without restarting the player.
 */
#ifndef RBPI4B_POINTSRC_H
#define RBPI4B_POINTSRC_H

#include "settings_zone.h"  /* struct settings_state, for the snapshot below */

/* Start (or re-start) the discovery/reader thread. Idempotent and safe to call
 * from any thread; returns 0 if the thread is running. */
int pointsrc_start(void);

/* Read-only snapshot of what the pointer is currently doing, for the launcher's
 * status output and for debugging without a log file. */
void pointsrc_status(char *buf, unsigned long buflen);

/* The live pointer, for the fb cursor compositor (fb_cursor.c). Returns 1 when a
 * *relative* device is driving the pointer and fills in its logical position and
 * button state; 0 when nothing is attached, or when the device is an absolute
 * one — which reports where it is touched and needs no arrow drawn for it.
 *
 * Unlocked, like the status above: these are four `volatile int`s the reader
 * thread updates, and the consumer redraws at 30 Hz anyway, so the worst a torn
 * read can do is place the arrow one frame behind. */
int pointsrc_cursor(int *logical_x, int *logical_y, int *down);

/* Append one line to /tmp/pointsrc.log when POINT_DEBUG is set. Shared between
 * the reader thread and the compositor so both halves of the pointer path land
 * in one file, in the order things happened. */
void pointsrc_log(const char *fmt, ...);

/* Everything the SETTINGS panel reads: rbp's two media slots -- BOTH the answer about whether
 * each has media and each device's own NAME, off `/tmp/udev_usbN.label`, which is what the
 * USB1 STOP/USB2 STOP rows print -- the USB bus (through usb_devices.c's mixer_present(), the same single walk audioshim asks),
 * and the four live switch files. Public for the reason the snapshot above is: the panel's
 * picture depends on all of it, so menu_draw.c's builder reads it on every build and the
 * answer it was read at is part of the published image.
 *
 * A NULL S is legal and does nothing, so a caller cannot fault here by accident. */
void pointsrc_settings_state(struct settings_state *S);

/* Write `word` to settings row `row`'s switch file (the panel's own path for that row, and
 * the same write), for rbp's UTILITY rows (utility_rows.c). 0 if the row has a file. */
int pointsrc_settings_write(int row, const char *word);


/* Is rbp on the PERFORMANCE screen -- the one it draws the deck badges and the pads on?
 *
 * EXPORTED, and it is the touch thread's own answer: menu_draw.c's paint thread gates the deck
 * badges' play ring on it, and a second read of rbp's browse mode up there would be a second
 * spelling of one question. 0 when rbp is not the reader, or the mode cannot be asked -- every
 * failure answers "not the screen", so nothing is drawn on a guess. */
int rbp_perf_screen(void);

#endif /* RBPI4B_POINTSRC_H */
