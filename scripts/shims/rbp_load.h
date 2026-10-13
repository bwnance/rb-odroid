/*
 * rbp_load.h -- load a track onto a deck by its DATABASE ID.
 *
 * One symbol: the watcher thread ctrlshim.c spawns from its constructor, beside
 * the LED bridge and the MIDI reader.
 *
 * WHY THIS EXISTS.  Everything else in this shim drives rbp the way a finger or
 * a controller would: a touch becomes a pointer record, a button becomes a
 * keycode, and both then have to be *navigated* -- the browse screen has to be
 * raised, a list scrolled or a search typed, a row selected.  That is a lot of
 * moving parts to say "play this track", and every one of them is a way to be
 * wrong.
 *
 * rbp has a door that skips all of it.  See rbp_load.c for the account: it is a
 * mailbox message carrying the track's own id, and it is the same path the
 * player uses when a track is dragged onto a deck.
 *
 * THE WATCHER IS INERT UNTIL ASKED.  It only ever reads a file, and only acts
 * when that file exists and its contents have CHANGED.  With no file there is
 * nothing to do and nothing is logged: a shim that quietly did nothing is
 * exactly what a shim that is not deployed yet looks like, and that is the
 * right default for something that reaches this far into rbp.
 */
#ifndef RBPI4B_RBP_LOAD_H
#define RBPI4B_RBP_LOAD_H

/* Polls /tmp/rb_load for one command line -- "<deck> <trackid> [hi]" -- and
 * calls rbp's own load-by-music-id entry point when the line changes. Never
 * returns; detached by the caller. */
void *rbp_load_thread(void *arg);

#endif /* RBPI4B_RBP_LOAD_H */
