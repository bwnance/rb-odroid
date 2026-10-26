/*
 * flx10_hid.c -- the DDJ-FLX10's jog screens over HID. See flx10_hid.h.
 *
 * The node is found by scanning /sys/class/hidraw/<n>/device/uevent for the
 * unit's 2B73:0041 (the chroot binds the host's /dev and /sys, so a replugged
 * unit's new node shows up), retried once a second while there is none, and
 * dropped on a write error so a replug is picked up the same way.
 *
 * hidraw takes byte 0 as the report number; the FLX10 has none, so every write
 * is a 0 followed by the 128-byte packet.
 */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "flx10_hid.h"
#include "shimutil.h"
#include "syscalls.h"

/* ---- the per-deck state packet ("xx 21") -------------------------------------
 * Decoded from a USB capture of rekordbox driving the unit (tools/captures/
 * flx10-rekordbox-hid-notes.txt, tools/flx10-pcap-decode.py; 2026-10-25),
 * every field checked against the operator's notes. Only bytes 0..63 are ever
 * non-zero.
 *   [0]      deck 0x10/0x20/0x30/0x40          [1] 0x21
 *   [2]      0x02 paused away from the cue point; 0x00 playing, or paused on
 *            it (cue standby) -- the capture shows 0x00 after a load and a cue
 *   [3]      0x08, +0x10 while the jog is touched, +0x02 the white edges
 *            (sent for rbp's ON AIR; rekordbox sent 0x08)
 *   [4]      tempo range box 0x10 +-6, 0x20 +-10, 0x30 +-16, 0x40 WIDE;
 *            0x04 master tempo
 *   [5]      0x01, +0x10 SYNC on, +0x80 tempo MASTER (second capture)
 *   [6]      0x02          [7] 0x02 decks 1/3, 0x01 decks 2/4
 *   [8]      0x80          [9] 0xb4 loaded (0x10 empty; 0xbc while looping)
 *   [11..14] position: minutes, seconds, milliseconds (LE16)
 *   [15..18] track length, the same way
 *   [21]     BPM, whole number (low 8 bits)
 *   [22]     tenths << 4 | whole number >> 8
 *   [23..24] tempo % x100, signed LE16
 *   [25..30] the last loop's in / out, ms LE24 each; [27] (in's top byte)
 *            0x80 while no loop is playing
 *   [10]     the playing loop's size code (flx10_loop_code), 0 when none
 *   [9]      0xbc once the track has a loop
 *   [31..33] the cue point, ms LE24 (the cue arm); again at [55..57]
 *   [43..44] 0xff 0xff     [45] 0x02     [58] 0x01
 *   [49]     hundredths << 4
 *   [60]     0x80 | key code (flx10_key_code's table), the key as shown
 *   [61]     0x0d + the key shift in semitones (MT off; "+5" beside it)
 * The empty packet is the same header with [9] 0x10, no position/BPM, [27]
 * 0x80, [43..44] 0xff, [60] 0x80, [61] 0x0d, and the range box and tempo %. */
static const unsigned char range_bits[4] = { 0x10, 0x20, 0x30, 0x40 };
static int onair_byte = 3, onair_mask = 0x02;   /* the white edges */

void flx10_hid_set_onair(int byte, int mask)
{
     onair_byte = (byte >= 2 && byte < FLX10_HID_PKT && mask > 0) ? byte : -1;
     onair_mask = mask & 0xff;
}

int flx10_key_shift(int tempo_x100)
{
     const double semi = 1.0594630943592953;      /* 2^(1/12) */
     const double half = 1.0293022366434920;      /* 2^(1/24) */
     double r = 1.0 + tempo_x100 / 10000.0, up = half, down = 1.0 / half;
     int k = 0;

     if (r <= 0.0)
          return -12;
     while (r >= up && k < 24) { k++; up *= semi; }
     while (r < down && k > -24) { k--; down /= semi; }
     return k;
}

int flx10_key_shifted(int code, int k)
{
     int idx;

     if (code < 1 || code > 24)
          return 0;
     idx = (((code - 1) / 2 + k) % 12 + 12) % 12;   /* one step = one semitone */
     return idx * 2 + 1 + ((code - 1) & 1);
}

int flx10_loop_code(int len_ms, int bpm_x100, int tempo_x100)
{
     /* beats, as a power of two: exponent -6..9 and the code for it */
     static const signed char code[16] = {
          0x04, 0x05, 0x06, 0x07, 0x08, 0x0a,               /* 1/64 .. 1/2 */
          0x0c, 0x0d, 0x0e, 0x0f, 0x10, 0x11, 0x12, 0x13, 0x14, 0x15 };
     double beat_ms, want;

     if (len_ms <= 0 || bpm_x100 <= 0 || tempo_x100 <= -10000)
          return 0x01;
     /* one beat of the track, in track ms; the loop's ends are whole ms, so a
      * 1/64-beat loop (~6 ms) is off by up to a ms: 3%, or 1.5 ms */
     beat_ms = 60000.0 * (1.0 + tempo_x100 / 10000.0) / (bpm_x100 / 100.0);
     want = beat_ms / 64.0;
     for (int i = 0; i < 16; i++, want *= 2.0) {
          double tol = want * 0.03 > 1.5 ? want * 0.03 : 1.5;
          if (len_ms > want - tol && len_ms < want + tol)
               return code[i];
     }
     return 0x01;
}

int flx10_loop_code_beats(int num, int den)
{
     static const signed char code[17] = {
          0x03,                                             /* 1/128 (guess) */
          0x04, 0x05, 0x06, 0x07, 0x08, 0x0a,               /* 1/64 .. 1/2 */
          0x0c, 0x0d, 0x0e, 0x0f, 0x10, 0x11, 0x12, 0x13, 0x14, 0x15 };
     int e = 0;

     if (num <= 0 || den <= 0)
          return 0x01;
     while (num > 1 && (num & 1) == 0) { num >>= 1; e++; }
     while (den > 1 && (den & 1) == 0) { den >>= 1; e--; }
     if (num != 1 || den != 1 || e < -7 || e > 9)
          return 0x01;
     return code[e + 7];
}

static void put_time(unsigned char *p, int ms)
{
     int min;

     if (ms < 0) ms = 0;
     min = ms / 60000;
     p[0] = (unsigned char)(min > 255 ? 255 : min);
     p[1] = (unsigned char)((ms / 1000) % 60);
     p[2] = (unsigned char)((ms % 1000) & 0xff);
     p[3] = (unsigned char)((ms % 1000) >> 8);
}

static void put_le24(unsigned char *p, int v)
{
     p[0] = (unsigned char)v;
     p[1] = (unsigned char)(v >> 8);
     p[2] = (unsigned char)(v >> 16);
}

void flx10_hid_state_pkt(unsigned char p[FLX10_HID_PKT], int deck,
                         const struct flx10_hid_deck *s)
{
     int tempo, bpm, whole, k, code;

     memset(p, 0, FLX10_HID_PKT);
     p[0] = (unsigned char)(0x10 * (deck + 1));
     p[1] = 0x21;
     p[3] = 0x08;
     p[5] = 0x01;
     p[6] = 0x02;
     p[7] = (deck & 1) ? 0x01 : 0x02;
     p[8] = 0x80;
     p[9] = 0x10;
     p[27] = 0x80;
     p[43] = p[44] = 0xff;
     p[60] = 0x80;
     p[61] = 0x0d;
     if (!s) {
          p[4] = 0x20;
          return;
     }
     p[4] = (unsigned char)((s->mt ? 0x04 : 0) | range_bits[s->range & 3]);
     tempo = s->tempo_x100;
     if (tempo > 32767) tempo = 32767;
     if (tempo < -32768) tempo = -32768;
     p[23] = (unsigned char)(tempo & 0xff);
     p[24] = (unsigned char)((tempo >> 8) & 0xff);
     if (!s->loaded)
          return;

     p[2] = (!s->playing && !s->cue_standby) ? 0x02 : 0x00;
     if (s->jog_touch)
          p[3] |= 0x10;
     /* (rbp's time mode is not sent: [3] 0x02, once read as "remaining",
      * is the white edges -- operator, 2026-10-25 -- see on_air below) */
     if (s->sync)
          p[5] |= 0x10;
     if (s->master)
          p[5] |= 0x80;
     p[9] = 0xb4;
     /* at the track's end rbp's position reaches (or passes) the length, and
      * the screen drops the arms for a position not inside the track: kept a
      * millisecond short of the end */
     put_time(p + 11, (s->total_ms > 0 && s->elapsed_ms >= s->total_ms) ?
                      s->total_ms - 1 : s->elapsed_ms);
     put_time(p + 15, s->total_ms);
     bpm = s->bpm_x100 > 0 ? s->bpm_x100 : 0;
     whole = bpm / 100;
     if (whole > 0xfff) whole = 0xfff;
     p[21] = (unsigned char)(whole & 0xff);
     p[22] = (unsigned char)((((bpm / 10) % 10) << 4) | ((whole >> 8) & 0x0f));
     p[49] = (unsigned char)((bpm % 10) << 4);
     if (s->loop_in_ms >= 0 && s->loop_out_ms > s->loop_in_ms) {
          put_le24(p + 25, s->loop_in_ms);
          put_le24(p + 28, s->loop_out_ms);
          p[9] = 0xbc;
          if (s->looping) {
               code = flx10_loop_code(s->loop_out_ms - s->loop_in_ms,
                                      s->bpm_x100, s->tempo_x100);
               if (code == 0x01)
                    code = flx10_loop_code_beats(s->loop_num, s->loop_den);
               p[10] = (unsigned char)code;
          }
          else
               p[27] = 0x80;
     }
     if (s->cue_ms >= 0) {
          put_le24(p + 31, s->cue_ms);
          put_le24(p + 55, s->cue_ms);
     }
     p[45] = 0x02;
     p[58] = 0x01;
     k = s->mt ? 0 : flx10_key_shift(tempo);
     if (k > 12) k = 12;
     if (k < -12) k = -12;
     code = flx10_key_shifted(s->key_code, k);
     if (code) {
          p[60] = (unsigned char)(0x80 | code);
          p[61] = (unsigned char)(0x0d + k);
     }
     if (s->on_air && onair_byte >= 0)
          p[onair_byte] |= (unsigned char)onair_mask;
}

void flx10_hid_heartbeat_pkt(unsigned char p[FLX10_HID_PKT], int n)
{
     memset(p, 0, FLX10_HID_PKT);
     p[1] = 0x3D;
     p[2] = (unsigned char)n;
     p[4] = 0x05;
}

/* ---- the sender ---- */

#define HID_PERIOD_US 10000          /* 100 Hz, about rekordbox's rate */
#define EXTRAP_MAX_MS 200
#define NO_BACKSTEP_MS 100
#define JOG_ACTIVE_MS 60             /* the jog counts as turning this long after a count */
#define JOG_LEAD_MS 12               /* how far ahead of rbp's reading a turning jog is drawn */
#define JOG_LEAD_MAX_MS 30
#define JOG_LEAD_FRESH_MS 8          /* ...and only while counts are still arriving */
#define JOG_LEAD_MIN_VEL 0.3f        /* lead only above this speed (track ms per ms) */
#define JOG_STILL_MS 40              /* a held jog's reading wobbles this much */
#define TEMPO_HOLD_MS 3000           /* sync + jog: tempo held this long after the jog */

struct hid_slot {
     struct flx10_hid_deck s;
     struct flx10_hid_track k;
     struct flx10_hid_tempo_hold th;
     unsigned long long jog_at;       /* last jog turn (flx10_hid_jog_activity) */
     int jog_dir;                     /* its direction, +1 / -1 */
};
static struct hid_slot hd[4];
static pthread_mutex_t hd_lock = PTHREAD_MUTEX_INITIALIZER;
static int (*pos_reader)(int deck, int *pos_ms, int *moving);

static int moving(const struct flx10_hid_deck *s)
{
     return s->loaded && s->playing && !s->cue_standby;
}

int flx10_hid_extrapolate(int base_ms, long dt_ms, int mov, int tempo_x100,
                          int total_ms)
{
     long v;

     if (!mov || dt_ms <= 0)
          return base_ms;
     if (dt_ms > EXTRAP_MAX_MS)
          dt_ms = EXTRAP_MAX_MS;
     v = base_ms + dt_ms * (10000L + tempo_x100) / 10000L;
     if (total_ms > 0 && v > total_ms)
          v = total_ms;
     return (int)v;
}

void flx10_hid_track_init(struct flx10_hid_track *k)
{
     memset(k, 0, sizeof *k);
     k->hold = -1;
}

int flx10_hid_merge(struct flx10_hid_track *k, int reading, int mov, int jog,
                    int tempo_x100, int total_ms, unsigned long long now)
{
     /* how fast rbp's reading moves, from its changes (it moves in audio
      * blocks), for the jog's lead */
     if (reading != k->last_reading) {
          long dt = (long)(now - k->changed_at);
          float v = (dt > 0 && dt < 100) ?
                    (float)(reading - k->last_reading) / (float)dt : 0.0f;
          k->vel = 0.5f * k->vel + 0.5f * v;
          k->last_reading = reading;
          k->changed_at = now;
     }
     if (jog == FLX10_JOG_TURNING || jog == FLX10_JOG_TURNING_BACK) {
          /* the hand moves the playhead: rbp's reading, carried on at its
           * own speed to now and JOG_LEAD_MS beyond (the reading is a block
           * old by the time the screen draws it) -- but only at a real
           * speed in the hand's direction: a slight move under a held hand
           * gives a noisy speed, and the lead turned it into the arm
           * twitching back and forth. And the arm only moves the way the
           * hand turns: rbp's reading wobbles (as when held still), and a
           * wobble against the hand is not shown unless it is a real jump
           * (more than NO_BACKSTEP_MS). */
          int dir = jog == FLX10_JOG_TURNING ? 1 : -1;
          /* on a playing deck the jog bends the speed and the track still
           * runs forward (a jog turned back slows it): the arm goes the way
           * the TRACK moves, not the hand -- holding it against the hand
           * made it stutter forward in NO_BACKSTEP_MS jumps */
          if (mov)
               dir = k->vel < 0.0f ? -1 : 1;
          long v = reading;
          if (k->fresh && k->vel * (float)dir >= JOG_LEAD_MIN_VEL) {
               long ahead = (long)(now - k->changed_at) + JOG_LEAD_MS;
               if (ahead > JOG_LEAD_MAX_MS)
                    ahead = JOG_LEAD_MAX_MS;
               v += (long)(k->vel * (float)ahead);
          }
          if ((v - k->shown) * dir < 0 && (k->shown - v) * dir < NO_BACKSTEP_MS)
               v = k->shown;                 /* against the hand: hold */
          if (v < 0) v = 0;
          if (total_ms > 0 && v > total_ms) v = total_ms;
          k->base_ms = reading;
          k->shown = (int)v;
          k->hold = -1;
     } else if (jog == FLX10_JOG_HELD) {
          /* held still: rbp's reading wobbles around the spot, so it moves
           * the arm only past JOG_STILL_MS */
          int diff = reading - k->shown;
          k->vel = 0.0f;
          if (diff > JOG_STILL_MS || diff < -JOG_STILL_MS)
               k->shown = reading;
          else if (diff >= 8 || diff <= -8)
               k->shown += diff / 8;         /* eases onto the wobble's middle */
          k->base_ms = reading;
          k->hold = -1;
     } else if (mov) {
          int pred = flx10_hid_extrapolate(k->base_ms, (long)(now - k->at),
                                           k->was_moving, tempo_x100, total_ms);
          /* rbp's reading moves in audio blocks, so it trails the estimate a
           * little: a reading just behind is not a jump back (a seek is
           * further than NO_BACKSTEP_MS) */
          if (k->was_moving && reading < pred && pred - reading < NO_BACKSTEP_MS)
               k->base_ms = pred;
          else
               k->base_ms = reading;
          k->shown = k->base_ms;
          k->hold = -1;
     } else {
          /* just stopped: the estimate ran a block ahead of where rbp
           * stopped; keep showing it while rbp's position stays put, so the
           * arm does not step back on a pause */
          if (k->was_moving && k->shown > reading &&
              k->shown - reading < NO_BACKSTEP_MS) {
               k->hold = k->shown;
               k->hold_src = reading;
          }
          if (k->hold >= 0 && reading >= k->hold_src - 1 && reading <= k->hold_src + 1)
               k->shown = k->hold;
          else {
               k->hold = -1;
               k->shown = reading;
          }
          k->base_ms = reading;
     }
     k->at = now;
     k->was_moving = mov && !jog;
     return k->shown;
}

void flx10_hid_tempo_hold(struct flx10_hid_tempo_hold *h, struct flx10_hid_deck *s,
                          int turning, unsigned long long now)
{
     if (!s->loaded || !s->sync) {
          h->on = 0;
          return;
     }
     if (turning) {
          if (!h->on) {               /* the value from before the jog */
               h->on = 1;
               h->tempo_x100 = h->last_tempo;
               h->bpm_x100 = h->last_bpm;
          }
          h->until = now + TEMPO_HOLD_MS;
     } else if (h->on) {
          int dt = s->tempo_x100 - h->tempo_x100;
          if (now >= h->until || (dt > -5 && dt < 5))
               h->on = 0;             /* back where it was, or given up */
     }
     if (!h->on) {
          h->last_tempo = s->tempo_x100;
          h->last_bpm = s->bpm_x100;
          return;
     }
     s->tempo_x100 = h->tempo_x100;
     s->bpm_x100 = h->bpm_x100;
}

void flx10_hid_set_pos_reader(int (*fn)(int deck, int *pos_ms, int *moving))
{
     pos_reader = fn;
}

void flx10_hid_jog_activity(int deck, int dir)
{
     if (deck >= 0 && deck < 4) {
          hd[deck].jog_at = shim_now_ms();
          hd[deck].jog_dir = dir;
     }
}

static int find_node(char *path, size_t n)
{
     DIR *d = opendir("/sys/class/hidraw");
     struct dirent *e;
     int found = 0;

     if (!d)
          return 0;
     while (!found && (e = readdir(d)) != NULL) {
          char p[300], buf[512];
          int fd;
          ssize_t r;

          if (e->d_name[0] == '.')
               continue;
          snprintf(p, sizeof p, "/sys/class/hidraw/%s/device/uevent", e->d_name);
          fd = real_open(p, O_RDONLY, 0);
          if (fd < 0)
               continue;
          r = real_read(fd, buf, sizeof buf - 1);
          real_close(fd);
          if (r <= 0)
               continue;
          buf[r] = 0;
          /* HID_ID=0003:00002B73:00000041 */
          if (strstr(buf, "00002B73:00000041")) {
               snprintf(path, n, "/dev/%s", e->d_name);
               found = 1;
          }
     }
     closedir(d);
     return found;
}

static int send_pkt(int fd, const unsigned char *p)
{
     unsigned char buf[FLX10_HID_PKT + 1];

     buf[0] = 0;
     memcpy(buf + 1, p, FLX10_HID_PKT);
     return real_write(fd, buf, sizeof buf) == (ssize_t)sizeof buf;
}

static void *hid_thread(void *arg)
{
     int fd = -1, n = 0;
     unsigned long long next_look = 0;
     unsigned char p[FLX10_HID_PKT];
     char path[300];
     (void)arg;

     for (;;) {
          usleep(HID_PERIOD_US);
          if (fd < 0) {
               unsigned long long now = shim_now_ms();
               if (now < next_look)
                    continue;
               next_look = now + 1000;
               if (!find_node(path, sizeof path))
                    continue;
               fd = real_open(path, O_WRONLY | O_CLOEXEC, 0);
               if (fd < 0) {
                    klog("knobshim2: FLX10 HID: cannot open %s: %s\n", path,
                         strerror(errno));
                    continue;
               }
               klog("knobshim2: FLX10 HID: sending deck state to %s every %d ms\n",
                    path, HID_PERIOD_US / 1000);
          }
          n = n % 5 + 1;
          flx10_hid_heartbeat_pkt(p, n);
          int ok = send_pkt(fd, p);
          for (int d = 0; ok && d < 4; d++) {
               struct flx10_hid_deck s;
               unsigned long long t;
               int pos = 0, mov = 0, live;

               /* rbp's own playhead, read now (decks 1-2): rbp_led's copy is
                * 20 Hz, too coarse for the arm, and late to see a pause */
               live = pos_reader && d < 2 && pos_reader(d, &pos, &mov);
               t = shim_now_ms();
               pthread_mutex_lock(&hd_lock);
               s = hd[d].s;
               if (!live) {
                    pos = s.elapsed_ms;
                    mov = moving(&s);
               }
               if (s.loaded) {
                    /* touched on a playing deck does nothing (no vinyl mode) */
                    int jog = t - hd[d].jog_at < JOG_ACTIVE_MS ?
                              (hd[d].jog_dir < 0 ? FLX10_JOG_TURNING_BACK : FLX10_JOG_TURNING) :
                              (s.jog_touch && !mov) ? FLX10_JOG_HELD : 0;
                    flx10_hid_tempo_hold(&hd[d].th, &s, t - hd[d].jog_at < JOG_ACTIVE_MS,
                                         t);
                    hd[d].k.fresh = t - hd[d].jog_at < JOG_LEAD_FRESH_MS;
                    s.elapsed_ms = flx10_hid_merge(&hd[d].k, pos, mov, jog,
                                                   s.tempo_x100, s.total_ms, t);
                    if (live) {
                         s.playing = mov;      /* a pause shows at once */
                         if (mov)
                              s.cue_standby = 0;
                    }
               }
               pthread_mutex_unlock(&hd_lock);
               flx10_hid_state_pkt(p, d, &s);
               ok = send_pkt(fd, p);
          }
          if (!ok) {
               klog("knobshim2: FLX10 HID: write to %s failed (%s); looking for "
                    "the unit again\n", path, strerror(errno));
               real_close(fd);
               fd = -1;
               next_look = shim_now_ms() + 1000;
          }
     }
     return NULL;
}

static void hid_start(void)
{
     pthread_t t;

     if (!env_on("FLX10_HID", 1)) {
          klog("knobshim2: FLX10 HID: off (FLX10_HID=0); the jogs will say "
               "Not Loaded\n");
          return;
     }
     {
          const char *oa = env_text("FLX10_ONAIR", NULL);
          unsigned b, m;
          if (oa && *oa && sscanf(oa, "%x:%x", &b, &m) == 2) {
               flx10_hid_set_onair((int)b, (int)m);
               klog("knobshim2: FLX10 HID: ON AIR -> byte %u |= 0x%02x\n", b, m);
          }
     }
     if (pthread_create(&t, NULL, hid_thread, NULL) == 0)
          pthread_detach(t);
     else
          klog("knobshim2: FLX10 HID: cannot start its thread\n");
}

void flx10_hid_update(int deck, const struct flx10_hid_deck *s)
{
     static pthread_once_t once = PTHREAD_ONCE_INIT;

     if (deck < 0 || deck > 3 || !s)
          return;
     pthread_mutex_lock(&hd_lock);
     if (!hd[deck].s.loaded && s->loaded)
          flx10_hid_track_init(&hd[deck].k);      /* a new track: no hold */
     hd[deck].s = *s;
     pthread_mutex_unlock(&hd_lock);
     pthread_once(&once, hid_start);
}
