/*
 * settings_remote.h -- the SETTINGS panel worked from a controller.
 *
 * The panel (settings_zone.c) is fbshim.so's and is otherwise worked by a finger
 * or a mouse through pointsrc.c. A unit with neither -- the ODROID with only the
 * FLX10 -- could not reach it at all (operator, 2026-10-26), so the controls shim
 * (knobshim.so, the MIDI maps) sends it commands instead. fbshim.so exports these
 * two functions and is ahead of knobshim.so in RB_LD_PRELOAD, so knobshim can call
 * them; it declares them WEAK and checks them, so a build or a test without
 * fbshim.so simply has no panel.
 *
 * A command is queued and carried out on fbshim's own thread, which also runs the
 * panel's clock (its timeout and the USB rows' three-second hold) while no
 * pointing device is attached -- with one attached, pointsrc.c's read loop does.
 */
#ifndef RBPI4B_SETTINGS_REMOTE_H
#define RBPI4B_SETTINGS_REMOTE_H

enum {
     SR_TOGGLE = 1,      /* raise the panel with the first row selected, or put it away */
     SR_NEXT,            /* select the next row down */
     SR_PREV,            /* ...the next row up */
     SR_PRESS,           /* press the selected row's value, as a finger would */
     SR_RELEASE,         /* ...and let go: a cycle row changes; a USB row ejects only
                          * if the press was held three seconds */
     SR_CLOSE            /* put it away */
};

void shim_settings_remote(int cmd);
int shim_settings_remote_open(void);   /* the panel is up (by any route) */

#endif /* RBPI4B_SETTINGS_REMOTE_H */
