/*
 * mixlevels.c -- a running log of the mix's levels, to see what the mixer did
 * while the operator played (operator, 2026-10-28: "tracks get drowned out too
 * easily"; docs/21 item 41).
 *
 * Once a second, while anything is heard, one line goes to /tmp/mixlevels.log:
 * for each deck the peak and RMS after its fader (what the sum gets) and the peak
 * before its EQ (after trim and OUTPUT HEADROOM), with the trim, fader and
 * crossfader gains and the three EQ knobs as rbp held them; then the master's peak,
 * RMS and the samples over full scale. All dBFS, rbp's float scale (1.0 = full).
 *
 * Where the numbers come from (rbp 1.20, read 2026-10-28):
 * - each deck's mixerengine::MixerChannel: the block before its EQ at +0x1c, read at
 *   updateFilter's entry (masteratt.c's hook calls mixlevels_channel() on either side
 *   of rbp's), and its output at +0x20 once updateFilter has run -- the buffer
 *   MasterOutChannel sums (initSourcePointers takes each input's +0x20). Trim's gain
 *   now at +0x40, ChannelFader's at +0x70, CrossFader's at +0xa0, the active EQ at
 *   +0x1ac with its three knobs (0..1) at +0x18;
 * - the master: MasterOutChannel::update @0x59490 leaves the sum, after Beat FX, in
 *   the buffer at +0x20 that MasterOut::update scales by the master level (pinned at
 *   0 dB, map_flx10.c) and sends to the card.
 *
 * The audio thread only adds up; a thread of its own writes the file. The log is
 * kept under 4 MB (the older half moved to /tmp/mixlevels.log.1).
 */
#include <fcntl.h>
#include <math.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "rbp_hook.h"
#include "rbp_key.h"        /* is_rbp_process() */
#include "shimutil.h"       /* klog(), shim_now_ms() */

#define LOG_FILE      "/tmp/mixlevels.log"
#define LOG_MAX       (4L << 20)
#define WINDOW_MS     1000
#define DECKS         2
#define QUIET_DB      (-70.0f)

#define CH_INPUT      0x14
#define CH_PRE        0x1c
#define CH_OUT        0x20
#define CH_TRIM       0x40
#define CH_FADER      0x70
#define CH_XFADER     0xa0
#define CH_EQ         0x1ac
#define EQ_KNOBS      0x18
#define MASTER_OUT    0x20

struct deck_acc {
     float pre_peak, peak;
     double sum;                     /* of squares, both sides */
     unsigned long n;                /* samples summed */
     float trim, fader, xfader, eq[3];
     int seen;
};

struct window {
     struct deck_acc deck[DECKS];
     float peak;
     double sum;
     unsigned long n, clips;
     time_t at;
};

static struct window acc, ready[2];
static unsigned seq;                 /* windows finished; ready[(seq - 1) & 1] is the last */
static unsigned long long window_start;

static float block_peak(const float *s, int n)
{
     float p = 0.0f;
     for (int i = 0; i < n; i++) {
          float a = fabsf(s[i]);
          if (a > p)
               p = a;
     }
     return p;
}

static double block_squares(const float *s, int n)
{
     double t = 0.0;
     for (int i = 0; i < n; i++)
          t += (double)s[i] * s[i];
     return t;
}

/* From masteratt.c's updateFilter hook: `after` 0 before rbp's EQ runs, 1 after. */
__attribute__((visibility("default"))) void mixlevels_channel(void *channel, int frames, int after)
{
     uint8_t *c = channel;
     unsigned input = *(unsigned *)(c + CH_INPUT);
     const float *s;
     struct deck_acc *d;

     if (input >= DECKS || frames <= 0 || frames > 4096)
          return;
     d = &acc.deck[input];
     if (!after) {
          s = *(float **)(c + CH_PRE);
          if (s) {
               float p = block_peak(s, frames * 2);
               if (p > d->pre_peak)
                    d->pre_peak = p;
          }
          return;
     }
     s = *(float **)(c + CH_OUT);
     if (!s)
          return;
     float p = block_peak(s, frames * 2);
     if (p > d->peak)
          d->peak = p;
     d->sum += block_squares(s, frames * 2);
     d->n += (unsigned long)frames * 2;
     d->trim = *(float *)(c + CH_TRIM);
     d->fader = *(float *)(c + CH_FADER);
     d->xfader = *(float *)(c + CH_XFADER);
     uint8_t *eq = *(uint8_t **)(c + CH_EQ);
     if (eq)
          memcpy(d->eq, eq + EQ_KNOBS, sizeof d->eq);
     d->seen = 1;
}

typedef void (*update_fn)(void *self, int frames);
static update_fn original_master;

static void master_update(void *self, int frames)
{
     original_master(self, frames);
     const float *s = *(float **)((uint8_t *)self + MASTER_OUT);
     if (s && frames > 0 && frames <= 4096) {
          for (int i = 0; i < frames * 2; i++) {
               float a = fabsf(s[i]);
               if (a > acc.peak)
                    acc.peak = a;
               if (a > 1.0f)
                    acc.clips++;
               acc.sum += (double)s[i] * s[i];
          }
          acc.n += (unsigned long)frames * 2;
     }
     unsigned long long now = shim_now_ms();
     if (!window_start)
          window_start = now;
     if (now - window_start < WINDOW_MS)
          return;
     window_start = now;
     acc.at = time(NULL);
     ready[seq & 1] = acc;
     __atomic_add_fetch(&seq, 1, __ATOMIC_RELEASE);
     memset(&acc, 0, sizeof acc);
}

/* ---- the file ------------------------------------------------------------------ */
static float db(float g)
{
     return g > 1e-7f ? 20.0f * log10f(g) : -140.0f;
}

static float rms_db(double sum, unsigned long n)
{
     return n ? db((float)sqrt(sum / (double)n)) : -140.0f;
}

/* One window as a line; 0 when it is all quiet. Pure, for the test. */
int mixlevels_line(const struct window *w, char *out, size_t size)
{
     float loudest = db(w->peak);
     for (int i = 0; i < DECKS; i++)
          if (db(w->deck[i].peak) > loudest)
               loudest = db(w->deck[i].peak);
     if (loudest < QUIET_DB)
          return 0;
     struct tm tm;
     localtime_r(&w->at, &tm);
     int n = (int)strftime(out, size, "%F %T", &tm);
     for (int i = 0; i < DECKS && n < (int)size; i++) {
          const struct deck_acc *d = &w->deck[i];
          if (!d->seen) {
               n += snprintf(out + n, size - (size_t)n, "  D%d -", i + 1);
               continue;
          }
          n += snprintf(out + n, size - (size_t)n,
                        "  D%d pk %6.1f rms %6.1f pre %6.1f trim %+5.1f fdr %6.1f xf %6.1f eq %.2f/%.2f/%.2f |",
                        i + 1, db(d->peak), rms_db(d->sum, d->n), db(d->pre_peak), db(d->trim),
                        db(d->fader), db(d->xfader), d->eq[0], d->eq[1], d->eq[2]);
     }
     if (n < (int)size)
          n += snprintf(out + n, size - (size_t)n, "  MST pk %6.1f rms %6.1f over %lu\n",
                        db(w->peak), rms_db(w->sum, w->n), w->clips);
     return n < (int)size ? n : (int)size - 1;
}

static void put(const char *line, int len)
{
     int fd = open(LOG_FILE, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
     if (fd < 0)
          return;
     if (lseek(fd, 0, SEEK_END) > LOG_MAX) {
          close(fd);
          rename(LOG_FILE, LOG_FILE ".1");
          fd = open(LOG_FILE, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
          if (fd < 0)
               return;
     }
     ssize_t w = write(fd, line, (size_t)len);
     (void)w;
     close(fd);
}

static void *writer(void *arg)
{
     unsigned done = 0;
     char line[512];
     (void)arg;
     for (;;) {
          usleep(250000);
          unsigned now = __atomic_load_n(&seq, __ATOMIC_ACQUIRE);
          if (now == done)
               continue;
          struct window w = ready[(now - 1) & 1];
          done = now;
          int len = mixlevels_line(&w, line, sizeof line);
          if (len > 0)
               put(line, len);
     }
     return NULL;
}

__attribute__((constructor)) static void mixlevels_init(void)
{
     if (!is_rbp_process())
          return;
     /* push {r3,r4,r5,r6,r7,lr} ; mov r4,r0 */
     static const uint8_t guard[8] = { 0xf8, 0x40, 0x2d, 0xe9, 0x00, 0x40, 0xa0, 0xe1 };
     original_master = (update_fn)rbp_hook(0x59490, guard, (void *)master_update);
     if (!original_master) {
          klog("knobshim2: mix levels: MasterOutChannel::update is not the one measured -- off\n");
          return;
     }
     pthread_t t;
     if (pthread_create(&t, NULL, writer, NULL) == 0)
          pthread_detach(t);
     klog("knobshim2: mix levels: logging to %s\n", LOG_FILE);
}
