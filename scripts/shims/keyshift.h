/* SPDX-License-Identifier: MPL-2.0
 *
 * keyshift.h -- each deck's key shifted by whole semitones, in rbp's audio path.
 *
 * The XDJ-RX3 has no key shift. This is rx3-toolkit's (github.com/Tratosca/
 * rx3-toolkit, MPL-2.0, at cc17a1f): a hook on dsp::TimeStretchManager::operate,
 * the per-deck block after speed and Master Tempo, which shifts the block in
 * place -- downwards through rbp's own Pitch beat effect, upwards through the
 * toolkit's shifter (rx3_pitch_shift.h), each where it is measured to be clean.
 * The FLX10's KEY SYNC (map_flx10.c) decides the shift; this only applies it.
 *
 * Every call is safe from any thread: the request is one atomic word per deck,
 * and the audio thread applies it at the next block.
 */
#ifndef RBPI4B_KEYSHIFT_H
#define RBPI4B_KEYSHIFT_H

/* Hook rbp's time stretcher, in rbp only, if its first instructions are the ones
 * measured. 0 = installed. Safe before rbp's own start-up: nothing of rbp's is
 * called until a shift is first asked for. */
int keyshift_install(void);

/* Deck 0 or 1 to `semitones` (-12..+12, clamped). Builds the engines on first
 * use. Answers 0 when the hook is in and the request was taken. */
int keyshift_set(int deck, int semitones);

/* The shift deck `deck` is asked to play at now. */
int keyshift_get(int deck);

/* A new track on `deck`: back to 0, and the shifter's history is dropped at the
 * next block. */
void keyshift_reload(int deck);

/* `deck`'s output silenced from the next block while `on` (after its key shift,
 * before rbp's mixer), whether or not a shift is in play. For INST.DOUBLES of a
 * paused deck (map_flx10.c): the copy rbp starts playing is not heard. */
void keyshift_mute(int deck, int on);

#endif /* RBPI4B_KEYSHIFT_H */
