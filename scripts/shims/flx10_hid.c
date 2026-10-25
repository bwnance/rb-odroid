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

/* rekordbox, deck 1 loaded and paused, bytes 0..63 (the rest are zero) */
static const unsigned char rb_loaded[64] = {
     0x10, 0x21, 0x00, 0x00, 0x2c, 0x01, 0x00, 0x02,
     0x80, 0xb4, 0x00, 0x00, 0x00, 0x02, 0x00, 0x02,
     0x26, 0x32, 0x03, 0x00, 0x00, 0x7c, 0x00, 0x03,
     0x00, 0x00, 0x00, 0x80, 0x00, 0x00, 0x00, 0x02,
     0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
     0x00, 0x00, 0x00, 0xff, 0xff, 0x00, 0x00, 0x00,
     0x00, 0x30, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02,
     0x00, 0x00, 0x01, 0x00, 0x85, 0x0d, 0x00, 0x00,
};

/* The loaded packet is rekordbox's captured one with these fields filled
 * (decoded 2026-10-25, scripts/odroid/hid-decode.py rounds 1-2):
 *   [2]      00 playing, 02 paused
 *   [4]      flags: 0x04 master tempo; 0x70 the tempo range box, 0x10 = +-6,
 *            0x20 = +-10, 0x30 = +-16, 0x40 = WIDE (0x00 / 0x60 = none);
 *            0x08 kept as captured (0x80 moved the blue tone arm)
 *   [5]      0x81 active
 *   [9]      0xb4 as captured; 00 reads as Not Loaded
 *   [11..12] elapsed seconds, big-endian; [13] the fraction, /256 (the arm
 *            moves with it)
 *   [16]     the key; encoding not found, 00 = blank, so blank
 *   [18]     BPM hundredths, 0..99 (captured 03: every BPM showed .03 until
 *            this was filled, 2026-10-25)
 *   [21..22] BPM, whole number, little-endian
 *   [23..24] tempo % x100, signed little-endian
 */
void flx10_hid_state_pkt(unsigned char p[FLX10_HID_PKT], int deck,
                         const struct flx10_hid_deck *s)
{
     static const unsigned char range_bits[4] = { 0x10, 0x20, 0x30, 0x40 };
     unsigned char db = (unsigned char)(0x10 * (deck + 1));
     int sec, bpm, tempo;

     memset(p, 0, FLX10_HID_PKT);
     if (!s || !s->loaded) {          /* 10 21 00 00 20 01 00 00 80 10 */
          p[0] = db; p[1] = 0x21; p[4] = 0x20; p[5] = 0x01; p[8] = 0x80; p[9] = 0x10;
          return;
     }
     memcpy(p, rb_loaded, sizeof rb_loaded);
     p[0] = db;
     p[2] = s->playing ? 0x00 : 0x02;
     p[4] = (unsigned char)((p[4] & ~0x74) | (s->mt ? 0x04 : 0) |
                            range_bits[s->range & 3]);
     p[5] = 0x81;
     sec = s->elapsed_ms > 0 ? s->elapsed_ms / 1000 : 0;
     if (sec > 0xFFFF) sec = 0xFFFF;
     p[11] = (unsigned char)(sec >> 8);
     p[12] = (unsigned char)sec;
     p[13] = s->elapsed_ms > 0 ? (unsigned char)((s->elapsed_ms % 1000) * 256 / 1000) : 0;
     p[16] = 0x00;
     bpm = s->bpm_x100 > 0 ? s->bpm_x100 / 100 : 0;
     if (bpm > 0xFFFF) bpm = 0xFFFF;
     p[18] = (unsigned char)(s->bpm_x100 > 0 ? s->bpm_x100 % 100 : 0);
     p[21] = (unsigned char)bpm;
     p[22] = (unsigned char)(bpm >> 8);
     tempo = s->tempo_x100;
     if (tempo > 32767) tempo = 32767;
     if (tempo < -32768) tempo = -32768;
     p[23] = (unsigned char)(tempo & 0xFF);
     p[24] = (unsigned char)((tempo >> 8) & 0xFF);
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
