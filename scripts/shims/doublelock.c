/*
 * doublelock.c -- two decks on the same track at the same spot play one stretched
 * copy of it (doublelock.h).
 *
 * rbp's audio callback (the JuceALSA thread) runs every deck's
 * TimeStretchManager::operate one after the other, 64 frames at a time. Each
 * block, as played, is kept here as the LEADER; the next deck's block in the same
 * callback -- within DOUBLELOCK_SAME_CALLBACK_US of it -- is a follower, and if it
 * reads the same track (the same length in samples) within DOUBLELOCK_TOL_FRAMES
 * of the leader, it is replaced by the leader's. The swap is a per-sample
 * crossfade, both ways, so neither locking nor letting go is a cut.
 */
#include "doublelock.h"

#include <string.h>

#include "rbp_abi.h"        /* PLAYENGINE_GLOBAL, PLAYER_TOTAL_LENGTH */

static float leader[DOUBLELOCK_MAX_FRAMES * 2];
static int leader_deck = -1;
static unsigned long leader_pos;
static unsigned int leader_frames;
static long leader_len;
static unsigned long long leader_us;
static float mix[2];             /* 0 = the deck's own block, 1 = the leader's */
static int enabled = 1;

__attribute__((weak)) long doublelock_track_len(unsigned int deck)
{
     void *pe = *(void **)PLAYENGINE_GLOBAL;
     void **b, **e;
     long len;

     if (!pe)
          return 0;
     b = *(void ***)((char *)pe + 12);        /* PlayEngine's vector of Players */
     e = *(void ***)((char *)pe + 16);
     if (!b || deck >= (unsigned int)(e - b) || !b[deck])
          return 0;
     len = ((long (*)(void *))PLAYER_TOTAL_LENGTH)(b[deck]);
     return len > 0 && len < 0x7fffffffL ? len : 0;   /* INT_MAX: no reader, no track */
}

void doublelock_block(unsigned int deck, unsigned long pos, float *out,
                      unsigned int frames, unsigned long long now_us)
{
     const float step = 1.0f / (float)DOUBLELOCK_FADE_FRAMES;
     unsigned long dist;
     long len;
     int have, near;

     if (!enabled || deck > 1u || !out || !frames || frames > DOUBLELOCK_MAX_FRAMES) {
          leader_deck = -1;
          return;
     }
     len = doublelock_track_len(deck);
     have = leader_deck == (int)(1u - deck) && leader_frames == frames &&
            now_us - leader_us < DOUBLELOCK_SAME_CALLBACK_US;
     dist = pos > leader_pos ? pos - leader_pos : leader_pos - pos;
     near = have && len > 0 && leader_len == len && dist <= DOUBLELOCK_TOL_FRAMES;
     if (have && (near || mix[deck] > 0.0f)) {
          float target = near ? 1.0f : 0.0f;
          for (unsigned int i = 0; i < frames; i++) {
               float m = mix[deck];
               if (m < target)
                    m = m + step > 1.0f ? 1.0f : m + step;
               else if (m > target)
                    m = m - step < 0.0f ? 0.0f : m - step;
               mix[deck] = m;
               if (m >= 1.0f) {
                    out[2 * i] = leader[2 * i];
                    out[2 * i + 1] = leader[2 * i + 1];
               } else if (m > 0.0f) {
                    out[2 * i] = out[2 * i] * (1.0f - m) + leader[2 * i] * m;
                    out[2 * i + 1] = out[2 * i + 1] * (1.0f - m) + leader[2 * i + 1] * m;
               }
          }
     } else {
          mix[deck] = 0.0f;            /* no other block this callback to fade from */
     }
     /* this block, as played, leads the next deck's */
     memcpy(leader, out, (size_t)frames * 2 * sizeof(float));
     leader_deck = (int)deck;
     leader_pos = pos;
     leader_frames = frames;
     leader_len = len;
     leader_us = now_us;
}

int doublelock_locked(unsigned int deck)
{
     return deck < 2u && mix[deck] > 0.0f;
}

void doublelock_enable(int on)
{
     enabled = on;
     if (!on)
          mix[0] = mix[1] = 0.0f;
}
