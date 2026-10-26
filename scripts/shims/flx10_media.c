/*
 * flx10_media.c -- the FLX10 jog screens' artwork and waveforms, built from
 * the rekordbox export the loaded track is on (flx10_media.h has the
 * layouts and where they were checked).
 *
 * THE EXPORT IS ONLY READ. Every file here is opened O_RDONLY, read and
 * closed: nothing is written, created, locked or renamed, and rbp's own
 * handles on export.pdb are not touched. rbp writes export.pdb while it runs
 * (play history), so a read can catch it mid-write: every offset taken from
 * the file is bounds-checked, and a torn read costs only that track's
 * artwork, never a crash and never a write. rbp's own database calls
 * (DsqlContent_GetImageID, DsqlImg_GetPath) were the alternative and were
 * not used: they run rbp's database engine, which also writes the export,
 * from a thread it does not expect.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "flx10_media.h"
#include "syscalls.h"

#define PAYLOAD_PER_PKT 122
#define MAX_FILE (32u << 20)            /* nothing on an export comes near */

/* ---- packets ---------------------------------------------------------- */

int flx10_bulk_count(size_t len)
{
     return len ? (int)((len + PAYLOAD_PER_PKT - 1) / PAYLOAD_PER_PKT) : 1;
}

void flx10_bulk_pkt(unsigned char p[128], int deck, int type,
                    const unsigned char *data, size_t len, int idx)
{
     int count = flx10_bulk_count(len);
     size_t off = (size_t)(idx - 1) * PAYLOAD_PER_PKT, take = 0;

     memset(p, 0, 128);
     p[0] = (unsigned char)(0x10 * (deck + 1));
     p[1] = (unsigned char)type;
     p[2] = (unsigned char)(idx & 0xff);
     p[3] = (unsigned char)(idx >> 8);
     p[4] = (unsigned char)(count & 0xff);
     p[5] = (unsigned char)(count >> 8);
     if (off < len)
          take = len - off < PAYLOAD_PER_PKT ? len - off : PAYLOAD_PER_PKT;
     if (take)
          memcpy(p + 6, data + off, take);
}

/* ---- ANLZ files --------------------------------------------------------- */

static unsigned be32(const unsigned char *b)
{
     return (unsigned)b[0] << 24 | (unsigned)b[1] << 16 | (unsigned)b[2] << 8 | b[3];
}

static unsigned le16(const unsigned char *b)
{
     return (unsigned)b[0] | (unsigned)b[1] << 8;
}

static unsigned le32(const unsigned char *b)
{
     return le16(b) | le16(b + 2) << 16;
}

long flx10_anlz_section(const unsigned char *d, size_t n, const char tag[4],
                        size_t *hdr_off, size_t *hdr_len, size_t *body_len)
{
     size_t o, hl, tl;

     if (!d || n < 12 || memcmp(d, "PMAI", 4))
          return -1;
     o = be32(d + 4);
     while (o + 12 <= n) {
          hl = be32(d + o + 4);
          tl = be32(d + o + 8);
          if (tl < 12 || hl < 12 || hl > tl || tl > n - o)
               return -1;                      /* torn or not an ANLZ file */
          if (!memcmp(d + o, tag, 4)) {
               *hdr_off = o;
               *hdr_len = hl;
               *body_len = tl - hl;
               return (long)(o + hl);
          }
          o += tl;
     }
     return -1;
}

/* ---- export.pdb ----------------------------------------------------------- */

#define PDB_TRACKS  0
#define PDB_ARTWORK 13

/* A DeviceSQL string at d[o] as UTF-8 into out (always terminated). */
static int pdb_string(const unsigned char *d, size_t n, size_t o, char *out, size_t on)
{
     size_t len, i, w = 0;
     unsigned kind;

     if (!on)
          return -1;
     out[0] = 0;
     if (o >= n)
          return -1;
     kind = d[o];
     if (kind & 1) {                           /* short ASCII */
          len = (kind >> 1) ? (kind >> 1) - 1 : 0;
          if (o + 1 + len > n || len >= on)
               return -1;
          memcpy(out, d + o + 1, len);
          out[len] = 0;
          return 0;
     }
     if (o + 4 > n)
          return -1;
     len = le16(d + o + 1);
     if (len < 4 || o + len > n)
          return -1;
     len -= 4;
     if (kind == 0x40) {                       /* long ASCII */
          if (len >= on)
               return -1;
          memcpy(out, d + o + 4, len);
          out[len] = 0;
          return 0;
     }
     if (kind != 0x90)
          return -1;
     for (i = 0; i + 1 < len; i += 2) {         /* UTF-16LE */
          unsigned c = le16(d + o + 4 + i);
          if (c >= 0xd800 && c < 0xdc00 && i + 3 < len) {
               unsigned c2 = le16(d + o + 4 + i + 2);
               if (c2 >= 0xdc00 && c2 < 0xe000) {
                    c = 0x10000 + ((c - 0xd800) << 10) + (c2 - 0xdc00);
                    i += 2;
               }
          }
          if (c < 0x80) {
               if (w + 1 >= on) return -1;
               out[w++] = (char)c;
          } else if (c < 0x800) {
               if (w + 2 >= on) return -1;
               out[w++] = (char)(0xc0 | c >> 6);
               out[w++] = (char)(0x80 | (c & 0x3f));
          } else if (c < 0x10000) {
               if (w + 3 >= on) return -1;
               out[w++] = (char)(0xe0 | c >> 12);
               out[w++] = (char)(0x80 | (c >> 6 & 0x3f));
               out[w++] = (char)(0x80 | (c & 0x3f));
          } else {
               if (w + 4 >= on) return -1;
               out[w++] = (char)(0xf0 | c >> 18);
               out[w++] = (char)(0x80 | (c >> 12 & 0x3f));
               out[w++] = (char)(0x80 | (c >> 6 & 0x3f));
               out[w++] = (char)(0x80 | (c & 0x3f));
          }
     }
     out[w] = 0;
     return 0;
}

/* Calls fn(row offset) for every present row of the table of type `type`,
 * until fn returns non-zero (which is returned). */
static int pdb_rows(const unsigned char *d, size_t n, unsigned type,
                    int (*fn)(const unsigned char *, size_t, size_t, void *), void *arg)
{
     unsigned plen, ntab, t;

     if (n < 0x1c)
          return 0;
     plen = le32(d + 4);
     ntab = le32(d + 8);
     if (plen < 0x100 || plen > 0x10000 || ntab > 64 || 0x1c + 16 * ntab > n)
          return 0;
     for (t = 0; t < ntab; t++) {
          const unsigned char *e = d + 0x1c + 16 * t;
          unsigned page, last, guard = 0;

          if (le32(e) != type)
               continue;
          page = le32(e + 8);
          last = le32(e + 12);
          while (guard++ < 100000) {
               size_t b = (size_t)page * plen;
               unsigned nsmall, nlarge, rows, flags, g;

               if (b + plen > n)
                    break;
               nsmall = d[b + 0x18];
               flags = d[b + 0x1b];
               nlarge = le16(d + b + 0x22);
               rows = (nlarge > nsmall && nlarge != 0x1fff) ? nlarge : nsmall;
               if (le32(d + b + 8) == type && !(flags & 0x40) && rows) {
                    for (g = 0; g <= (rows - 1) / 16; g++) {
                         size_t base = b + plen - (size_t)g * 0x24;
                         unsigned present, j;
                         if (base < b + 0x28 + 0x24)
                              break;
                         present = le16(d + base - 4);
                         for (j = 0; j < 16 && g * 16 + j < rows; j++) {
                              size_t ro;
                              int r;
                              if (!(present >> j & 1))
                                   continue;
                              ro = b + 0x28 + le16(d + base - 6 - 2 * j);
                              if (ro >= b + plen)
                                   continue;
                              if ((r = fn(d, n, ro, arg)) != 0)
                                   return r;
                         }
                    }
               }
               if (page == last)
                    break;
               page = le32(d + b + 12);
          }
     }
     return 0;
}

struct track_q {
     const char *rel;
     unsigned art_id;
     char *anlz;
     size_t anlz_n;
     char buf[1024];
};

static int track_row(const unsigned char *d, size_t n, size_t r, void *arg)
{
     struct track_q *q = arg;

     if (r + 0x5e + 42 > n)
          return 0;
     if (pdb_string(d, n, r + le16(d + r + 0x5e + 2 * 20), q->buf, sizeof q->buf) ||
         strcmp(q->buf, q->rel))
          return 0;
     q->art_id = le32(d + r + 0x1c);
     if (pdb_string(d, n, r + le16(d + r + 0x5e + 2 * 14), q->anlz, q->anlz_n))
          q->anlz[0] = 0;
     return 1;
}

struct art_q {
     unsigned id;
     char *out;
     size_t on;
};

static int art_row(const unsigned char *d, size_t n, size_t r, void *arg)
{
     struct art_q *q = arg;

     if (r + 5 > n || le32(d + r) != q->id)
          return 0;
     if (pdb_string(d, n, r + 4, q->out, q->on))
          q->out[0] = 0;
     return 1;
}

int flx10_pdb_find(const unsigned char *pdb, size_t n, const char *rel,
                   char *art, size_t art_n, char *anlz, size_t anlz_n)
{
     struct track_q tq;
     struct art_q aq;

     if (!pdb || !rel || !art_n || !anlz_n)
          return -1;
     art[0] = anlz[0] = 0;
     memset(&tq, 0, sizeof tq);
     tq.rel = rel;
     tq.anlz = anlz;
     tq.anlz_n = anlz_n;
     if (!pdb_rows(pdb, n, PDB_TRACKS, track_row, &tq))
          return -1;
     if (tq.art_id) {
          aq.id = tq.art_id;
          aq.out = art;
          aq.on = art_n;
          pdb_rows(pdb, n, PDB_ARTWORK, art_row, &aq);
     }
     return 0;
}

/* ---- payloads ------------------------------------------------------------- */

static int add(struct flx10_media *m, int type, const unsigned char *hdr, size_t hl,
               const unsigned char *body, size_t bl)
{
     unsigned char *p;

     if (m->n >= FLX10_MEDIA_MAX || !(p = malloc(hl + bl ? hl + bl : 1)))
          return -1;
     if (hl)
          memcpy(p, hdr, hl);
     if (bl)
          memcpy(p + hl, body, bl);
     m->it[m->n].type = (unsigned char)type;
     m->it[m->n].data = p;
     m->it[m->n].len = hl + bl;
     m->n++;
     return 0;
}

void flx10_media_free(struct flx10_media *m)
{
     for (int i = 0; i < m->n; i++)
          free(m->it[i].data);
     memset(m, 0, sizeof *m);
}

int flx10_media_from_src(const struct flx10_media_src *s, struct flx10_media *m)
{
     unsigned char h[4];
     size_t ho, hl, bl;
     long bo;
     int t = s->total_ms > 0 ? s->total_ms : 0;

     memset(m, 0, sizeof *m);
     /* 0x30 empty first: rekordbox sends it as a load starts */
     memset(h, 0, 4);
     if (add(m, 0x30, h, 4, NULL, 0))
          goto oom;
     /* 0x30: the length, as the state packet's [15..18] */
     h[0] = (unsigned char)(t / 60000 > 255 ? 255 : t / 60000);
     h[1] = (unsigned char)(t / 1000 % 60);
     h[2] = (unsigned char)(t % 1000 & 0xff);
     h[3] = (unsigned char)(t % 1000 >> 8);
     if (add(m, 0x30, h, 4, NULL, 0))
          goto oom;

     /* 0x2d: the length, then ten empty entries, as rekordbox sent them */
     {
          static const unsigned char empty[5] = { 0x9e, 0x2f, 0x27, 0x01, 0x00 };
          unsigned char c[4 + 2 + 50];
          memcpy(c, h, 4);
          c[4] = 10;
          c[5] = 0;
          for (int i = 0; i < 10; i++)
               memcpy(c + 6 + 5 * i, empty, 5);
          if (add(m, 0x2d, c, sizeof c, NULL, 0))
               goto oom;
     }
     /* 0x39: 00 "HOT CUE" (3 packets), 0x3b: zeros (5 packets) -- both as
      * rekordbox sent them at a load, meaning not known */
     {
          static const unsigned char lab[8] = { 0, 'H', 'O', 'T', ' ', 'C', 'U', 'E' };
          unsigned char *z = calloc(1, 610);
          if (!z)
               goto oom;
          if (add(m, 0x39, lab, sizeof lab, z, 366 - sizeof lab) ||
              add(m, 0x3b, z, 610, NULL, 0)) {
               free(z);
               goto oom;
          }
          free(z);
     }
     /* 0x3e: nothing played yet (one zero packet) */
     if (add(m, 0x3e, NULL, 0, NULL, 0))
          goto oom;

     /* 0x2b / 0x33: the two JPEGs behind their lengths (0 = none, or one
      * too big for its length field) */
     {
          size_t al = s->art && s->art_len <= 0xffff ? s->art_len : 0;
          size_t ml = s->artm && s->artm_len <= 0xffffff ? s->artm_len : 0;
          h[0] = (unsigned char)al;
          h[1] = (unsigned char)(al >> 8);
          if (add(m, 0x2b, h, 2, s->art, al))
               goto oom;
          h[0] = (unsigned char)ml;
          h[1] = (unsigned char)(ml >> 8);
          h[2] = (unsigned char)(ml >> 16);
          if (add(m, 0x33, h, 3, s->artm, ml))
               goto oom;
          m->have_art = al && ml;
     }

     /* 0x37: 0x80 + PWV6; without one a flat 1200-column overview */
     h[0] = 0x80;
     bo = flx10_anlz_section(s->ex2, s->ex2_len, "PWV6", &ho, &hl, &bl);
     if (bo >= 0) {
          if (add(m, 0x37, h, 1, s->ex2 + bo, bl))
               goto oom;
     } else {
          unsigned char *z = calloc(1, 3600);
          if (!z || add(m, 0x37, h, 1, z, 3600)) {
               free(z);
               goto oom;
          }
          free(z);
     }

     /* 0x2f: the beat grid, from PQTZ's 8-byte entries (beat, tempo, ms) */
     bo = flx10_anlz_section(s->dat, s->dat_len, "PQTZ", &ho, &hl, &bl);
     {
          size_t nb = bo >= 0 ? bl / 8 : 0;
          unsigned char *g = malloc(2 + 4 * nb);
          if (!g)
               goto oom;
          if (nb > 0xffff)
               nb = 0xffff;
          g[0] = (unsigned char)nb;
          g[1] = (unsigned char)(nb >> 8);
          for (size_t i = 0; i < nb; i++) {
               const unsigned char *e = s->dat + bo + 8 * i;
               unsigned ms = be32(e + 4);
               g[2 + 4 * i] = e[1];             /* beat number, 1..4 */
               g[3 + 4 * i] = (unsigned char)ms;
               g[4 + 4 * i] = (unsigned char)(ms >> 8);
               g[5 + 4 * i] = (unsigned char)(ms >> 16);
          }
          if (add(m, 0x2f, g, 2 + 4 * nb, NULL, 0)) {
               free(g);
               goto oom;
          }
          free(g);
     }

     /* 0x38: LE32 entry count + PWV7 (header [16..19], BE) */
     bo = flx10_anlz_section(s->ex2, s->ex2_len, "PWV7", &ho, &hl, &bl);
     if (bo >= 0 && hl >= 20) {
          unsigned cnt = be32(s->ex2 + ho + 16);
          if ((size_t)cnt * 3 > bl)
               cnt = (unsigned)(bl / 3);
          h[0] = (unsigned char)cnt;
          h[1] = (unsigned char)(cnt >> 8);
          h[2] = (unsigned char)(cnt >> 16);
          h[3] = (unsigned char)(cnt >> 24);
          if (add(m, 0x38, h, 4, s->ex2 + bo, (size_t)cnt * 3))
               goto oom;
          m->have_wave = 1;
     } else {
          memset(h, 0, 4);
          if (add(m, 0x38, h, 4, NULL, 0))
               goto oom;
     }
     /* 0x3e again, now the waveforms are in */
     if (add(m, 0x3e, NULL, 0, NULL, 0))
          goto oom;
     return 0;
oom:
     flx10_media_free(m);
     return -1;
}

/* ---- reading the export ---------------------------------------------------- */

/* The whole file, read only; NULL when it cannot be read. */
static unsigned char *slurp(const char *path, size_t *len)
{
     int fd = real_open(path, O_RDONLY | O_CLOEXEC, 0);
     unsigned char *b = NULL, *nb;
     size_t cap = 0, n = 0;
     ssize_t r;

     *len = 0;
     if (fd < 0)
          return NULL;
     for (;;) {
          if (n == cap) {
               if (cap >= MAX_FILE || !(nb = realloc(b, cap ? cap * 2 : 65536)))
                    goto fail;
               b = nb;
               cap = cap ? cap * 2 : 65536;
          }
          r = real_read(fd, b + n, cap - n);
          if (r < 0) {
               if (errno == EINTR)
                    continue;
               goto fail;
          }
          if (r == 0)
               break;
          n += (size_t)r;
     }
     real_close(fd);
     *len = n;
     return b;
fail:
     real_close(fd);
     free(b);
     return NULL;
}

int flx10_media_load(const char *track_path, int total_ms, struct flx10_media *m)
{
     char root[1024], p[1300], art[512], anlz[512];
     unsigned char *pdb = NULL;
     size_t pdb_len = 0, k;
     struct flx10_media_src src;
     unsigned char *ex2 = NULL, *dat = NULL, *a = NULL, *am = NULL;
     int rc;

     memset(m, 0, sizeof *m);
     if (!track_path || track_path[0] != '/' || strlen(track_path) >= sizeof root)
          return -1;
     /* the export's root: the nearest directory above the file holding
      * PIONEER/rekordbox/export.pdb */
     strcpy(root, track_path);
     for (;;) {
          char *s = strrchr(root, '/');
          if (!s || s == root)
               return -1;
          *s = 0;
          snprintf(p, sizeof p, "%s/PIONEER/rekordbox/export.pdb", root);
          if ((pdb = slurp(p, &pdb_len)) != NULL)
               break;
     }
     k = strlen(root);
     rc = flx10_pdb_find(pdb, pdb_len, track_path + k, art, sizeof art, anlz, sizeof anlz);
     free(pdb);
     if (rc)
          return -1;

     memset(&src, 0, sizeof src);
     src.total_ms = total_ms;
     if (anlz[0]) {
          char *dot;
          snprintf(p, sizeof p, "%s%s", root, anlz);
          dat = slurp(p, &src.dat_len);
          if ((dot = strrchr(p, '.')) != NULL && strlen(dot) == 4) {
               strcpy(dot, ".2EX");
               ex2 = slurp(p, &src.ex2_len);
          }
     }
     if (art[0]) {
          char *dot;
          snprintf(p, sizeof p, "%s%s", root, art);
          a = slurp(p, &src.art_len);
          if ((dot = strrchr(p, '.')) != NULL && strlen(p) + 2 < sizeof p) {
               memmove(dot + 2, dot, strlen(dot) + 1);
               dot[0] = '_';
               dot[1] = 'm';
               am = slurp(p, &src.artm_len);
          }
     }
     src.ex2 = ex2;
     src.dat = dat;
     src.art = a;
     src.artm = am;
     rc = flx10_media_from_src(&src, m);
     free(ex2);
     free(dat);
     free(a);
     free(am);
     return rc;
}
