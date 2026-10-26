/*
 * doublelock.h -- two decks playing the same track at the same spot play ONE
 * stretched copy of it.
 *
 * rbp time-stretches each deck on its own (MASTER TEMPO): dsp::TimeStretchManager
 * with a background stretcher and a smoothed speed table, whose splice points
 * follow the deck's own speed history. Two decks fed the same audio at the same
 * position and speed therefore splice a few ms apart and drift -- measured on the
 * unit 2026-10-26 with both decks' stretcher output recorded side by side:
 * correlation 0.04 at lag 0, the best lag wandering +-185 samples from one 0.25 s
 * to the next, and still drifting after both were restarted from one cue. Summed,
 * that is a comb filter: the hollow swoosh of instant doubles, or of the same
 * track loaded twice (operator: "only with the same track on both decks"), gone
 * with MASTER TEMPO off.
 *
 * So while both decks read the same track -- the same length to the sample, which
 * the same file on two USB sticks also is -- within DOUBLELOCK_TOL_FRAMES of each
 * other, the deck processed second in rbp's audio callback plays the first deck's
 * stretched block instead of its own. The two then sum like MASTER TEMPO off.
 * Further apart than that is an echo or a flam, which may be meant (an offset
 * double), and is left alone. Each change of state is a short crossfade, never a
 * cut. Everything after the stretcher -- EQ, FX, faders -- is still per deck.
 *
 * RB_DOUBLES_LOCK=0 leaves rbp's stretchers as they are.
 */
#ifndef RBPI4B_DOUBLELOCK_H
#define RBPI4B_DOUBLELOCK_H

#define DOUBLELOCK_TOL_FRAMES 882u    /* 20 ms: closer is a comb, further an echo */
#define DOUBLELOCK_FADE_FRAMES 512u   /* each change of state crossfades this long */
#define DOUBLELOCK_MAX_FRAMES 8192u
#define DOUBLELOCK_SAME_CALLBACK_US 500u    /* rbp's callback is 64 frames, 1.45 ms apart */

/* One block of `deck`'s stretched output -- `frames` interleaved stereo floats at
 * `out`, read from source position `pos` -- on rbp's audio thread, straight after
 * TimeStretchManager::operate. May replace `out` with the other deck's block.
 * `now_us` is a monotonic clock. */
void doublelock_block(unsigned int deck, unsigned long pos, float *out,
                      unsigned int frames, unsigned long long now_us);

/* The track `deck` has loaded, as its length in samples; 0 = none. WEAK: rbp's
 * Player::getTotalLength in the shim, the test's own in the test. */
long doublelock_track_len(unsigned int deck);

/* 1 while `deck` is playing the other deck's block (or fading to it). */
int doublelock_locked(unsigned int deck);

void doublelock_enable(int on);

#endif /* RBPI4B_DOUBLELOCK_H */
