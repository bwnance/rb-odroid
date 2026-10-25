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
 *   [3]      0x08 (+0x10 while the jog is touched -- not sent)
 *   [4]      tempo range box 0x10 +-6, 0x20 +-10, 0x30 +-16, 0x40 WIDE;
 *            0x04 master tempo
 *   [5]      0x01, +0x10 SYNC on
 *   [6]      0x02          [7] 0x02 decks 1/3, 0x01 decks 2/4
 *   [8]      0x80          [9] 0xb4 loaded (0x10 empty; 0xbc while looping)
 *   [11..14] position: minutes, seconds, milliseconds (LE16)
 *   [15..18] track length, the same way
 *   [21]     BPM, whole number (low 8 bits)
 *   [22]     tenths << 4 | whole number >> 8
 *   [23..24] tempo % x100, signed LE16
 *   [25..30] loop in / out, ms LE24 each; [27] 0x80 = no loop  (not sent yet)
 *   [31..33] the cue point, ms LE24 (the cue arm); again at [55..57]
 *   [43..44] 0xff 0xff     [45] 0x02     [58] 0x01
 *   [49]     hundredths << 4
 *   [60]     0x80 | key code (flx10_key_code's table), the key as shown
 *   [61]     0x0d + the key shift in semitones (MT off; "+5" beside it)
 * The empty packet is the same header with [9] 0x10, no position/BPM, [27]
 * 0x80, [43..44] 0xff, [60] 0x80, [61] 0x0d, and the range box and tempo %. */
static const unsigned char range_bits[4] = { 0x10, 0x20, 0x30, 0x40 };

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
     if (s->sync)
          p[5] |= 0x10;
     p[9] = 0xb4;
     put_time(p + 11, s->elapsed_ms);
     put_time(p + 15, s->total_ms);
     bpm = s->bpm_x100 > 0 ? s->bpm_x100 : 0;
     whole = bpm / 100;
     if (whole > 0xfff) whole = 0xfff;
     p[21] = (unsigned char)(whole & 0xff);
     p[22] = (unsigned char)((((bpm / 10) % 10) << 4) | ((whole >> 8) & 0x0f));
     p[49] = (unsigned char)((bpm % 10) << 4);
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
}

void flx10_hid_heartbeat_pkt(unsigned char p[FLX10_HID_PKT], int n)
{
     memset(p, 0, FLX10_HID_PKT);
     p[1] = 0x3D;
     p[2] = (unsigned char)n;
     p[4] = 0x05;
}

/* ---- the sender ---- */

static struct flx10_hid_deck hd[4];
static pthread_mutex_t hd_lock = PTHREAD_MUTEX_INITIALIZER;

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
          usleep(20000);
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
               klog("knobshim2: FLX10 HID: sending deck state to %s every 20 ms\n",
                    path);
          }
          n = n % 5 + 1;
          flx10_hid_heartbeat_pkt(p, n);
          int ok = send_pkt(fd, p);
          for (int d = 0; ok && d < 4; d++) {
               struct flx10_hid_deck s;
               pthread_mutex_lock(&hd_lock);
               s = hd[d];
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
     hd[deck] = *s;
     pthread_mutex_unlock(&hd_lock);
     pthread_once(&once, hid_start);
}
