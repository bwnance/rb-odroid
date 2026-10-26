/*
 * test_doublelock.c -- host checks for doublelock.c: which deck plays whose
 * stretched block, and how it gets there. rbp's two decks are played by hand:
 * deck 1 is all +1, deck 2 all -1, so a sample says whose it is.
 *
 *   make test_doublelock && ./test_doublelock
 */
#include <stdio.h>

#include "doublelock.h"

static int checks, failures;
#define CHECK(c, ...) do { checks++; if (!(c)) { failures++; printf("FAIL %s:%d: ", __FILE__, __LINE__); \
     printf(__VA_ARGS__); printf("\n"); } } while (0)

static long len[2] = { 1000000, 1000000 };
long doublelock_track_len(unsigned int deck) { return len[deck]; }

#define F 64
static float a[F * 2], b[F * 2];
static unsigned long long t = 1000000;

/* one rbp callback: deck 1's block, then deck 2's, 100 us apart */
static void callback(unsigned long pos1, unsigned long pos2)
{
     for (int i = 0; i < F * 2; i++) {
          a[i] = 1.0f;
          b[i] = -1.0f;
     }
     doublelock_block(0, pos1, a, F, t);
     doublelock_block(1, pos2, b, F, t + 100);
     t += 1451;
}

static int all(const float *x, float v)
{
     for (int i = 0; i < F * 2; i++)
          if (x[i] != v)
               return 0;
     return 1;
}

int main(void)
{
     unsigned long p = 500000;
     int n;

     callback(p, p + 2000);
     CHECK(all(a, 1.0f) && all(b, -1.0f), "45 ms apart: an echo, each deck its own");
     callback(p, p);
     CHECK(all(a, 1.0f), "the leader is never touched");
     CHECK(b[0] > -1.0f && b[F * 2 - 1] > b[0] && b[F * 2 - 1] < 1.0f,
           "the same spot: deck 2 starts fading to deck 1's block (%f .. %f)", b[0], b[F * 2 - 1]);
     CHECK(doublelock_locked(1) && !doublelock_locked(0), "deck 2 is the one locked");
     for (n = 0; n < 8; n++)
          callback(p, p);
     CHECK(all(b, 1.0f), "after 512 frames: deck 2 plays deck 1's block exactly");
     callback(p + 300, p);
     CHECK(all(b, 1.0f), "7 ms apart (a synced copy): still one copy");
     callback(p + 3000, p);
     CHECK(b[0] < 1.0f && b[F * 2 - 1] < b[0], "68 ms apart: deck 2 fades back to its own");
     for (n = 0; n < 8; n++)
          callback(p + 3000, p);
     CHECK(all(b, -1.0f) && !doublelock_locked(1), "...and is its own again");

     len[1] = 1000001;
     for (n = 0; n < 9; n++)
          callback(p, p);
     CHECK(all(b, -1.0f), "one sample longer: a different track, never locked");
     len[1] = 0;
     callback(p, p);
     CHECK(all(b, -1.0f), "no track: nothing");
     len[1] = 1000000;

     /* deck 2's block alone, with deck 1's from the last callback 1.35 ms before */
     for (n = 0; n < 9; n++)
          callback(p, p);
     for (int i = 0; i < F * 2; i++)
          b[i] = -1.0f;
     t += 3000;
     doublelock_block(1, p, b, F, t);
     CHECK(all(b, -1.0f), "no leader in this callback: its own block, at once");
     t += 1451;                             /* the next callback */

     /* deck 1 first only after a gap: a stale deck 2 never leads deck 1 */
     callback(p, p);
     CHECK(all(a, 1.0f), "deck 1 after deck 2's block of the last callback: untouched");

     doublelock_enable(0);
     for (n = 0; n < 9; n++)
          callback(p, p);
     CHECK(all(b, -1.0f), "RB_DOUBLES_LOCK=0: rbp's own stretchers");
     doublelock_enable(1);

     printf("%s: %d checks, %d failures\n", failures ? "FAIL" : "ok", checks, failures);
     return failures ? 1 : 0;
}
