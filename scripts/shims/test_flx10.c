/*
 * test_flx10.c -- pins map_flx10.c's dispatch against the operator's table
 * (docs/21, "FLX10 map"), with rbp stubbed out. Every input below is a message
 * the unit was measured to send (scripts/odroid/midi-learn.py).
 *
 * Pure host C: it links the production map, ctrl_map.c, shimutil.c, shmstate.c
 * and pitch_state.c, and nothing that carries an rbp address. The timed
 * gestures (the SCFX deselect window, the browse-push hold) are run against
 * the real clock with short sleeps.
 *
 *   cc -std=gnu11 -Wall -o test_flx10 test_flx10.c map_flx10.c ctrl_map.c \
 *      shimutil.c shmstate.c pitch_state.c -lpthread -lm && ./test_flx10
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sound/asequencer.h>

#include "shimutil.h"
#include "rbp_abi.h"
#include "rbp_bridge.h"
#include "rbp_vu.h"
#include "ctrl_map.h"
#include "pitch_state.h"
#include "flx10_hid.h"
#include "flx10_media.h"
#include "settings_remote.h"

static int checks, failures;
#define CHECK(cond, ...) do { checks++; if (!(cond)) { failures++;           \
        printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__);      \
        printf("\n"); } } while (0)

/* ---- stubs for what the map reaches in rbp ---- */
int verbose = 0;
int g_fader[3];
int g_fader_seen[3];
static int looping[2];
int aloop_is_looping(int deck) { return looping[deck]; }
int led_loop_flash[2];
/* the last value sent on ch 16 per note / CC number, -1 = never */
static int jd_note_v[128], jd_cc_v[128], note70_n;
static int pad_note_v[2][128];              /* pad channels 7 / 9 */
static int dk_note_v[2][128];                 /* last note value on deck ch 1/2 */
int midi_cc(int ch, int cc, int v) { if (ch == 15 && cc >= 0 && cc < 128) jd_cc_v[cc] = v; return 1; }
int midi_note(int ch, int n, int v)
{
     if (ch == 15 && n >= 0 && n < 128) { jd_note_v[n] = v; if (n == 0x70) note70_n++; }
     if ((ch == 0 || ch == 1) && n >= 0 && n < 128) dk_note_v[ch][n] = v;
     if ((ch == 7 || ch == 9) && n >= 0 && n < 128) pad_note_v[ch == 9][n] = v;
     return 1;
}
int midi_sysex_out(const unsigned char *b, unsigned n) { (void)b; (void)n; return 1; }
int midi_surface_generation(void) { return 1; }
int midi_out_ready(void) { return 1; }
volatile int led_kick;
static int query_n, splash_up;
static unsigned long long query_at;
int led_query_absolute(void) { query_n++; return 1; }
unsigned long long midi_query_at_ms(void) { return query_at; }
int shim_splash_active(void) { return splash_up; }
static int ks_shift[2], ks_reloads;
int keyshift_set(int d, int n) { ks_shift[d] = n; return 0; }
int keyshift_get(int d) { return ks_shift[d]; }
void keyshift_reload(int d) { ks_reloads++; ks_shift[d] = 0; }
static int ks_muted[2], ks_mute_calls;
void keyshift_mute(int d, int on) { ks_muted[d] = on; ks_mute_calls++; }
static int cue[4], mcue, deleted_deck = -1, deleted_pad = -1;
int me_get_cue(int idx) { return cue[idx]; }
void me_set_cue(int idx, int on) { cue[idx] = on; }
static int preview_cue;
void me_set_preview_cue(int on) { preview_cue = on; }
int me_get_preview_cue(void) { return preview_cue; }
int me_get_master_cue(void) { return mcue; }
void me_set_master_cue(int on) { mcue = on; }
int hotcue_delete(int deck, int pad) { deleted_deck = deck; deleted_pad = pad; return 0; }
static int browse_mode = 1, usb1 = 1, cursor_track = 1;
int flx10_cursor_is_track(void) { return cursor_track; }
static int menu_open, on_categories;
int flx10_on_categories(void) { return on_categories; }
int flx10_menu_open(void) { return menu_open; }
/* the engine, as SLIP REVERSE / REVERSE see it */
static int slipmode[2], reversing[2], rev_effect[2], slip_sets, rev_sets;
int flx10_slip_mode(int d) { return slipmode[d]; }
/* reversing: asked for (getReverse); rev_effect: still in effect after it was
 * asked off -- a quantized release, until the beat */
int flx10_reversing(int d) { return reversing[d] || rev_effect[d]; }
void flx10_set_slip(int d, int on) { slipmode[d] = on; slip_sets++; }
void flx10_set_reverse(int d, int on) { reversing[d] = on; rev_sets++; }
static int bj_n, bj_deck[16], bj_t[16];
static int goes_back[2];
int flx10_trrev_goes_back(int d) { return goes_back[d]; }
int flx10_cue_ms(int d, int total_ms) { (void)d; (void)total_ms; return 1000; }
static int timemode_n[2];
void flx10_toggle_time_mode(int d) { timemode_n[d]++; }
int flx10_pos_now(int d, int *pos, int *mov) { (void)d; *pos = 0; *mov = 0; return 0; }
int flx10_track_path(int d, char *b, size_t n) { (void)d; if (n) b[0] = 0; return 0; }
int flx10_hot_cues(int d, int *i, int *l, unsigned *c) { (void)d; (void)i; (void)l; (void)c; return 0; }
/* the USB slots, as LINK's eject sees them */
static int usb_ready[2], usb_mounted[2], usb_ejects[2];
int flx10_usb_ready(int s) { return usb_ready[s]; }
int flx10_usb_mounted(int s) { return usb_mounted[s]; }
void flx10_usb_eject(int s) { usb_ejects[s]++; }
/* the SETTINGS panel in fbshim.so (settings_remote.h) */
static int panel_up, panel_cmds[16], panel_cmds_n;
void shim_settings_remote(int cmd) { if (panel_cmds_n < 16) panel_cmds[panel_cmds_n++] = cmd; }
int shim_settings_remote_open(void) { return panel_up; }
int flx10_end_caution(int d) { (void)d; return 0; }
static int loop_primes[2];
void flx10_loop_prime(int d) { loop_primes[d]++; }
static int xf_assign[2] = { -1, -1 };
void flx10_set_xf_assign(int d, int a) { xf_assign[d] = a; }
static int second_press[2];
static long start_pos[2] = { 22050, 22050 }; static int start_ms[2] = { 500, 500 };
static int seek_n, seek_deck; static long seek_pos;
long flx10_start_pos(int d, int *ms) { *ms = start_ms[d]; return start_pos[d]; }
void flx10_seek(int d, long pos) { seek_n++; seek_deck = d; seek_pos = pos; }
static int place_n, place_deck = -1;
int flx10_doubles_place(int d) { place_n++; place_deck = d; return 1; }
void flx10_trrev_as_second_press(int d) { second_press[d]++; }
static int bpm_auto_n;
void flx10_bfx_bpm_auto(void) { bpm_auto_n++; }
/* the FX screen reads nothing of rbp's here: its messages are tested as bytes */
int flx10_fx_read(int sel, void *v) { (void)sel; (void)v; return 0; }
static int fx_on;
int flx10_fx_on(void) { return fx_on; }
static int bfxq_panel = -1;                /* rbp's UI not up, unless a test says */
int flx10_bfxq_panel(void) { return bfxq_panel; }
void flx10_beat_jump(int d, int type) { if (bj_n < 16) { bj_deck[bj_n] = d; bj_t[bj_n] = type; } bj_n++; }
static int pj_n, pj_deck[16]; static float pj_b[16];
int flx10_plain_jump(int d, float b) { if (pj_n < 16) { pj_deck[pj_n] = d; pj_b[pj_n] = b; } pj_n++; return 1; }
static int route_ours[2] = { -1, -1 };
void flx10_route_deck(int d, int ours) { if (d >= 0 && d < 2) route_ours[d] = ours; }
static int rbp_setting[3] = { -1, -1, -1 };
int flx10_rbp_setting(int which) { return which >= 0 && which < 3 ? rbp_setting[which] : -1; }
int flx10_unit_want(int is_cc, int num, int rbp_value);
/* the rbp mode each FLX10 pad page belongs to */
static int pmode(int pg) { static const int m[8] = { 0, 2, 3, 5, 4, 6, 1, 7 }; return pg < 0 ? 0 : m[(pg >> 4) & 7]; }
void flx10_cursor_debug(int *m, int *c, int *l, int *r) { *m = 3; *c = 0; *l = 0; *r = 2; }
static int padmode[2];                     /* what rbp's pad-mode byte would read */
int flx10_pad_mode(int deck) { return padmode[deck]; }
int flx10_browse_mode(void) { return browse_mode; }
int flx10_usb1_present(void) { return usb1; }
int cc_to_10bit(int v) { if (v < 0) v = 0; if (v > 127) v = 127; return (v * 1023 + 63) / 127; }

struct sent { int key, op, ch; long param; float f; long l; };
static struct sent sent[512];
static int sent_n;
static void rec(int key, int op, int ch, long param, float f, long l)
{
     if (sent_n < 512) { sent[sent_n].key = key; sent[sent_n].op = op;
          sent[sent_n].ch = ch; sent[sent_n].param = param;
          sent[sent_n].f = f; sent[sent_n].l = l; sent_n++; }
}
void send_rx_key(int k, int op, int ch, long p) { rec(k, op, ch, p, 0.0f, 0); }
void send_rx_key_f(int k, int op, int ch, long p, float f) { rec(k, op, ch, p, f, 0); }
void send_rx_key_fl(int k, int op, int ch, long p, float f, long l) { rec(k, op, ch, p, f, l); }

/* ---- driving the map ---- */
static void ev(int type, int ch, int a, int b)
{
     struct snd_seq_event e;
     memset(&e, 0, sizeof e);
     e.type = type;
     if (type == SNDRV_SEQ_EVENT_CONTROLLER) {
          e.data.control.channel = ch; e.data.control.param = a; e.data.control.value = b;
     } else {
          e.data.note.channel = ch; e.data.note.note = a; e.data.note.velocity = b;
     }
     map_flx10.event(&e);
}
#define ON(ch, n)  ev(SNDRV_SEQ_EVENT_NOTEON, (ch) - 1, (n), 127)
#define OFF(ch, n) ev(SNDRV_SEQ_EVENT_NOTEON, (ch) - 1, (n), 0)
#define CC(ch, c, v) ev(SNDRV_SEQ_EVENT_CONTROLLER, (ch) - 1, (c), (v))

static void ms(int n) { struct timespec t = { n / 1000, (n % 1000) * 1000000L }; nanosleep(&t, NULL); }
static void reset(void) { sent_n = 0; }
static int is(int i, int key, int op, int ch)
{
     return i < sent_n && sent[i].key == key && sent[i].op == op && sent[i].ch == ch;
}
static void build(const char *keys)
{
     if (keys) setenv("FLX10_KEYS", keys, 1); else unsetenv("FLX10_KEYS");
     ctrl_bindings_reset();
     map_flx10.build();
}


/* ---- artwork and waveforms (flx10_media.c) ----
 * The byte-for-byte check against rekordbox's own packets was made with the
 * operator's capture and export stick (flx10_media.h); these pin the parsing
 * and the packets on hand-made files, the export's real artwork having no
 * place in the repo. */
static void be32w(unsigned char *b, unsigned v) { b[0] = v >> 24; b[1] = v >> 16; b[2] = v >> 8; b[3] = v; }
static void le16w(unsigned char *b, unsigned v) { b[0] = v; b[1] = v >> 8; }
static void le32w(unsigned char *b, unsigned v) { le16w(b, v); le16w(b + 2, v >> 16); }

/* An ANLZ file: PMAI header, then sections {tag, header length, body}. */
static size_t anlz_sec(unsigned char *b, size_t o, const char *tag, size_t hl,
                       const unsigned char *hdr_tail, size_t tail_n,
                       const unsigned char *body, size_t bl)
{
     memcpy(b + o, tag, 4);
     be32w(b + o + 4, (unsigned)hl);
     be32w(b + o + 8, (unsigned)(hl + bl));
     memset(b + o + 12, 0, hl - 12);
     if (tail_n)
          memcpy(b + o + 12, hdr_tail, tail_n);
     memcpy(b + o + hl, body, bl);
     return o + hl + bl;
}

static size_t anlz_start(unsigned char *b)
{
     memcpy(b, "PMAI", 4);
     be32w(b + 4, 28);
     memset(b + 8, 0, 20);
     return 28;
}

/* A short-ASCII DeviceSQL string; returns its length. */
static size_t dstr_ascii(unsigned char *b, const char *s)
{
     size_t n = strlen(s);
     b[0] = (unsigned char)(((n + 1) << 1) | 1);
     memcpy(b + 1, s, n);
     return n + 1;
}

static void media_tests(void)
{
     static unsigned char ex2[4096], dat[1024], pdb[3 * 4096];
     unsigned char p[128];
     size_t n2, nd;

     /* packets: [0] deck, [1] type, index and count LE16, 122 bytes each */
     {
          unsigned char data[300];
          for (int i = 0; i < 300; i++)
               data[i] = (unsigned char)i;
          CHECK(flx10_bulk_count(300) == 3 && flx10_bulk_count(122) == 1 &&
                flx10_bulk_count(123) == 2, "packet counts");
          flx10_bulk_pkt(p, 1, 0x38, data, 300, 3);
          CHECK(p[0] == 0x20 && p[1] == 0x38 && p[2] == 3 && p[3] == 0 && p[4] == 3 &&
                p[5] == 0 && p[6] == (unsigned char)244 && p[6 + 55] == (unsigned char)299 &&
                p[6 + 56] == 0 && p[127] == 0, "last packet: header, the tail, zero padding");
     }

     /* ANLZ sections and the payloads built from them */
     {
          unsigned char w6[6] = { 1, 2, 3, 4, 5, 6 }, w7[9] = { 7, 8, 9, 10, 11, 12, 13, 14, 15 };
          unsigned char t7[12] = { 0, 0, 0, 3, 0, 0, 0, 3, 0, 0x96, 0, 0 };
          unsigned char q[16] = { 0, 1, 0x40, 0, 0, 0, 0x01, 0x6b,  0, 2, 0x40, 0, 0, 0, 0x03, 0x42 };
          unsigned char jpg[5] = { 0xff, 0xd8, 0x01, 0xff, 0xd9 }, jpgm[7] = { 0xff, 0xd8, 2, 3, 4, 0xff, 0xd9 };
          struct flx10_media_src src;
          struct flx10_media m;
          size_t ho, hl, bl;
          long bo;

          n2 = anlz_start(ex2);
          n2 = anlz_sec(ex2, n2, "PPTH", 16, NULL, 0, (const unsigned char *)"xx", 2);
          n2 = anlz_sec(ex2, n2, "PWV7", 24, t7, 12, w7, 9);
          n2 = anlz_sec(ex2, n2, "PWV6", 20, NULL, 0, w6, 6);
          be32w(ex2 + 8, (unsigned)n2);
          nd = anlz_start(dat);
          nd = anlz_sec(dat, nd, "PQTZ", 24, NULL, 0, q, 16);
          bo = flx10_anlz_section(ex2, n2, "PWV6", &ho, &hl, &bl);
          CHECK(bo > 0 && hl == 20 && bl == 6 && !memcmp(ex2 + bo, w6, 6), "ANLZ: PWV6 found");
          CHECK(flx10_anlz_section(ex2, n2, "PSSI", &ho, &hl, &bl) < 0, "ANLZ: absent tag");
          CHECK(flx10_anlz_section(ex2, n2 - 3, "PWV6", &ho, &hl, &bl) < 0,
                "ANLZ: a torn file is refused, not overrun");

          memset(&src, 0, sizeof src);
          src.ex2 = ex2; src.ex2_len = n2; src.dat = dat; src.dat_len = nd;
          src.art = jpg; src.art_len = 5; src.artm = jpgm; src.artm_len = 7;
          src.total_ms = 186489;
          CHECK(flx10_media_from_src(&src, &m) == 0 && m.n == 12 && m.have_art && m.have_wave,
                "twelve transfers");
          CHECK(m.it[0].type == 0x30 && m.it[0].len == 4 && !m.it[0].data[0] &&
                m.it[3].type == 0x39 && m.it[3].len == 366 && m.it[3].data[1] == 'H' &&
                m.it[4].type == 0x3b && m.it[4].len == 610,
                "a load starts with an empty 0x30; 0x39 and 0x3b as rekordbox");
          CHECK(m.it[2].type == 0x2d && m.it[2].len == 56 && m.it[2].data[4] == 10 &&
                m.it[2].data[6] == 0x9e && m.it[2].data[55] == 0 && m.it[5].type == 0x3e &&
                m.it[5].len == 0 && m.it[11].type == 0x3e, "0x2d (no cues) and 0x3e, as rekordbox");
          CHECK(m.it[1].type == 0x30 && m.it[1].len == 52 && m.it[1].data[0] == 3 &&
                m.it[1].data[1] == 6 && m.it[1].data[2] == 0xe9 && m.it[1].data[3] == 1,
                "0x30: 3:06.489 as 03 06 e9 01 (the capture's), then the cue slots");
          CHECK(m.it[6].type == 0x2b && m.it[6].len == 7 && m.it[6].data[0] == 5 &&
                m.it[6].data[1] == 0 && !memcmp(m.it[6].data + 2, jpg, 5), "0x2b: LE16 + JPEG");
          CHECK(m.it[7].type == 0x33 && m.it[7].len == 10 && m.it[7].data[0] == 7 &&
                m.it[7].data[2] == 0 && !memcmp(m.it[7].data + 3, jpgm, 7), "0x33: LE24 + JPEG");
          CHECK(m.it[8].type == 0x37 && m.it[8].len == 7 && m.it[8].data[0] == 0x80 &&
                !memcmp(m.it[8].data + 1, w6, 6), "0x37: 0x80 + PWV6");
          CHECK(m.it[9].type == 0x2f && m.it[9].len == 10 && m.it[9].data[0] == 2 &&
                m.it[9].data[2] == 1 && m.it[9].data[3] == 0x6b && m.it[9].data[4] == 1 &&
                m.it[9].data[6] == 2 && m.it[9].data[7] == 0x42 && m.it[9].data[8] == 3,
                "0x2f: count, then beat + LE24 ms (the capture's 01 6b 01 00)");
          CHECK(m.it[10].type == 0x38 && m.it[10].len == 13 && m.it[10].data[0] == 3 &&
                m.it[10].data[3] == 0 && !memcmp(m.it[10].data + 4, w7, 9), "0x38: LE32 count + PWV7");
          flx10_media_free(&m);

          memset(&src, 0, sizeof src);
          src.total_ms = 1000;
          CHECK(flx10_media_from_src(&src, &m) == 0 && m.n == 12 && !m.have_art && !m.have_wave &&
                m.it[6].len == 2 && m.it[7].len == 3 && m.it[8].len == 3601 &&
                m.it[9].len == 2 && m.it[10].len == 4,
                "nothing found: empty artwork, a flat overview, no detail");
          flx10_media_free(&m);
     }

     /* the hot cue slots in 0x30, from the EXT's PCO2 or the DAT's PCOB */
     {
          static unsigned char ext[1024], dat2[1024];
          unsigned char sec[400], out[52];
          struct flx10_media_src src;
          size_t ne, nd2, k = 0;
          /* PCO2 (type 1 = hot cues): cue A at 27 ms, default colour; loop C at
           * 70.616 s, palette code 0x2a */
          memset(sec, 0, sizeof sec);
          be32w(sec, 1);                               /* list type: hot cues */
          for (int e = 0; e < 2; e++) {
               unsigned char *q = sec + 8 + 88 * e;
               memcpy(q, "PCP2", 4); be32w(q + 4, 16); be32w(q + 8, 88);
               be32w(q + 12, e ? 3 : 1);
               q[16] = (unsigned char)(e ? 2 : 1);
               be32w(q + 20, e ? 70616 : 27);
               be32w(q + 40, 0);                       /* no comment */
               q[44] = (unsigned char)(e ? 0x2a : 0);  /* palette code */
          }
          ne = anlz_start(ext);
          ne = anlz_sec(ext, ne, "PCO2", 20, sec, 8, sec + 8, 176);
          /* the DAT's PCOB: cue B at 42.380 s (used only without a PCO2) */
          memset(sec, 0, sizeof sec);
          be32w(sec, 1);
          {
               unsigned char *q = sec + 12;
               memcpy(q, "PCPT", 4); be32w(q + 4, 28); be32w(q + 8, 56);
               be32w(q + 12, 2); q[28] = 1; be32w(q + 32, 42380);
          }
          nd2 = anlz_start(dat2);
          nd2 = anlz_sec(dat2, nd2, "PCOB", 24, sec, 12, sec + 12, 56);

          memset(&src, 0, sizeof src);
          src.ext = ext; src.ext_len = ne; src.dat = dat2; src.dat_len = nd2;
          src.total_ms = 225960;
          flx10_cue_payload(&src, out);
          {
               static const unsigned char want[52] = {
                    0x03, 0x2d, 0xc0, 0x03,
                    0x01, 0x16, 0x00, 0x00, 0x1b, 0x00,        /* A: cue, default green */
                    0, 0, 0, 0, 0, 0,                          /* B: empty */
                    0x02, 0x2a, 0x01, 0x0a, 0x68, 0x02 };      /* C: loop, its own colour */
               for (k = 0; k < 52 && out[k] == want[k]; k++)
                    ;
          }
          CHECK(k == 52, "0x30: length, then slots by hot cue number (differs at %d)", (int)k);
          src.ext = NULL; src.ext_len = 0;
          flx10_cue_payload(&src, out);
          CHECK(out[4] == 0 && out[10] == 1 && out[11] == 0x16 && out[12] == 0 &&
                out[13] == 0x2a && out[14] == 0x7c && out[15] == 1,
                "no EXT: the DAT's PCOB (B at 0:42.380)");
     }
     /* a cue set on the unit: one slot, then the update transfers */
     {
          unsigned char e[6], pay[52] = { 0 };
          struct flx10_media m;
          flx10_cue_slot(e, 2, 0, 196934);
          CHECK(e[0] == 2 && e[1] == 0x24 && e[2] == 3 && e[3] == 16 && e[4] == 0xa6 && e[5] == 3,
                "a new loop cue: default orange, 3:16.934");
          CHECK(flx10_media_cue_update(pay, &m) == 0 && m.n == 2 && m.it[0].type == 0x30 &&
                m.it[0].len == 52 && m.it[1].type == 0x3e, "cue update: 0x30 then 0x3e");
          flx10_media_free(&m);
     }

     /* export.pdb: a tracks page and an artwork page */
     {
          unsigned char *t, *r;
          char art[256], anlz[256];
          size_t so;
          /* header: page length, 2 tables */
          memset(pdb, 0, sizeof pdb);
          le32w(pdb + 4, 4096);
          le32w(pdb + 8, 2);
          le32w(pdb + 0x1c, 0);  le32w(pdb + 0x1c + 8, 1);  le32w(pdb + 0x1c + 12, 1);
          le32w(pdb + 0x2c, 13); le32w(pdb + 0x2c + 8, 2);  le32w(pdb + 0x2c + 12, 2);
          /* page 1: one track row */
          t = pdb + 4096;
          le32w(t + 8, 0);
          t[0x18] = 1;
          t[0x1b] = 0x34;
          le16w(t + 4096 - 4, 1);          /* row 0 present */
          le16w(t + 4096 - 6, 0);          /* at the heap's start */
          r = t + 0x28;
          le32w(r + 0x1c, 7);              /* artwork 7 */
          le32w(r + 0x48, 1390);
          so = 0x5e + 42;
          le16w(r + 0x5e + 2 * 14, (unsigned)so);
          so += dstr_ascii(r + so, "/PIONEER/USBANLZ/P030/0002FB38/ANLZ0000.DAT");
          le16w(r + 0x5e + 2 * 20, (unsigned)so);
          {                                /* UTF-16: "/Contents/é.mp3" */
               const char *a = "/Contents/";
               size_t k = 0, nch = strlen(a) + 1 + 4;
               r[so] = 0x90;
               le16w(r + so + 1, (unsigned)(4 + 2 * nch));
               for (size_t i = 0; a[i]; i++, k++)
                    le16w(r + so + 4 + 2 * k, (unsigned char)a[i]);
               le16w(r + so + 4 + 2 * k++, 0xe9);
               le16w(r + so + 4 + 2 * k++, '.');
               le16w(r + so + 4 + 2 * k++, 'm');
               le16w(r + so + 4 + 2 * k++, 'p');
               le16w(r + so + 4 + 2 * k++, '3');
          }
          /* page 2: artwork 7 */
          t = pdb + 2 * 4096;
          le32w(t + 8, 13);
          t[0x18] = 1;
          t[0x1b] = 0x34;
          le16w(t + 4096 - 4, 1);
          r = t + 0x28;
          le32w(r, 7);
          dstr_ascii(r + 4, "/PIONEER/Artwork/00001/a7.jpg");

          CHECK(flx10_pdb_find(pdb, sizeof pdb, "/Contents/\xc3\xa9.mp3", art, sizeof art,
                               anlz, sizeof anlz) == 0 &&
                !strcmp(anlz, "/PIONEER/USBANLZ/P030/0002FB38/ANLZ0000.DAT") &&
                !strcmp(art, "/PIONEER/Artwork/00001/a7.jpg"),
                "pdb: track by its UTF-16 path -> analysis and artwork paths (%s | %s)",
                anlz, art);
          CHECK(flx10_pdb_find(pdb, sizeof pdb, "/Contents/e.mp3", art, sizeof art,
                               anlz, sizeof anlz) == -1, "pdb: unknown track");
          CHECK(flx10_pdb_find(pdb, 4096 + 100, "/Contents/\xc3\xa9.mp3", art, sizeof art,
                               anlz, sizeof anlz) == -1, "pdb: torn file, no overrun");
     }

     /* the whole load, from a little export on disk */
     {
          char root[64], path[512];
          struct flx10_media m;
          FILE *f;
          snprintf(root, sizeof root, "/tmp/flx10-media-test-%d", (int)getpid());
          snprintf(path, sizeof path, "mkdir -p %s/PIONEER/rekordbox %s/PIONEER/USBANLZ/P030/0002FB38 "
                   "%s/PIONEER/Artwork/00001 %s/Contents", root, root, root, root);
          CHECK(system(path) == 0, "test export made");
#define PUT(rel, buf, len) do { snprintf(path, sizeof path, "%s%s", root, rel); \
          f = fopen(path, "wb"); if (f) { fwrite(buf, 1, len, f); fclose(f); } } while (0)
          PUT("/PIONEER/rekordbox/export.pdb", pdb, sizeof pdb);
          PUT("/PIONEER/USBANLZ/P030/0002FB38/ANLZ0000.DAT", dat, nd);
          PUT("/PIONEER/USBANLZ/P030/0002FB38/ANLZ0000.2EX", ex2, n2);
          PUT("/PIONEER/Artwork/00001/a7.jpg", "\xff\xd8\xff\xd9", 4);
          PUT("/PIONEER/Artwork/00001/a7_m.jpg", "\xff\xd8\x00\xff\xd9", 5);
#undef PUT
          snprintf(path, sizeof path, "%s/Contents/\xc3\xa9.mp3", root);
          CHECK(flx10_media_load(path, 186489, &m) == 0 && m.n == 12 && m.have_art &&
                m.have_wave && m.it[6].len == 6 && m.it[7].len == 8 && m.it[10].len == 13,
                "load: export found above the file, every piece read");
          flx10_media_free(&m);
          snprintf(path, sizeof path, "%s/Contents/other.mp3", root);
          CHECK(flx10_media_load(path, 1000, &m) == -1 && m.n == 0, "load: not in the export");
          CHECK(flx10_media_load("/nonexistent/a.mp3", 1000, &m) == -1, "load: no export");
          snprintf(path, sizeof path, "rm -rf %s", root);
          if (system(path)) {}
     }
}

int main(void)
{
     setenv("FLX10_HID", "0", 1);          /* no sender thread in the tests */
     memset(jd_note_v, 0xff, sizeof jd_note_v);
     memset(pad_note_v, 0xff, sizeof pad_note_v);
     memset(jd_cc_v, 0xff, sizeof jd_cc_v);
     build(NULL);

     /* transport, both decks */
     reset(); ON(1, 11); OFF(1, 11);
     CHECK(sent_n == 2 && is(0, K_PLAY, OP_PRESS, 1) && is(1, K_PLAY, OP_RELEASE, 1), "deck 1 PLAY");
     reset(); ON(2, 12); OFF(2, 12);
     CHECK(sent_n == 2 && is(0, K_CUE, OP_PRESS, 2), "deck 2 CUE on player 2");

     /* KEY SYNC: short press nothing (rbp has no key sync), hold = MASTER TEMPO */
     reset(); ON(1, 101); OFF(1, 101);
     CHECK(sent_n == 0, "KEY SYNC short press sends nothing (%d sent)", sent_n);
     reset(); ON(1, 26); OFF(1, 26); OFF(1, 101);
     CHECK(sent_n == 2 && is(0, K_MT, OP_PRESS, 1) && is(1, K_MT, OP_RELEASE, 1),
           "KEY SYNC held -> MT, once (%d sent)", sent_n);
     reset(); ON(2, 26); OFF(2, 26); OFF(2, 101);
     CHECK(sent_n == 2 && is(0, K_MT, OP_PRESS, 2), "deck 2 KEY SYNC held -> MT on deck 2");

     /* SHIFT + BEAT SYNC = MASTER; TEMPO RESET nothing, SHIFT = TEMPO RANGE */
     reset(); ON(1, 92);
     CHECK(sent_n == 2 && is(0, K_MASTER, OP_PRESS, 1), "SHIFT+SYNC -> MASTER");
     reset(); ON(1, 96);
     CHECK(sent_n == 2 && is(0, K_TEMPO_RANGE, OP_PRESS, 1) && is(1, K_TEMPO_RANGE, OP_RELEASE, 1),
           "SHIFT + TEMPO RESET -> RANGE, one step on the press");
     OFF(1, 96);
     CHECK(sent_n == 2, "...nothing on the release");

     /* TEMPO RESET: a toggle; on holds rbp at the centre and ignores the fader,
      * off sends the fader's current position; LED note 65 follows */
     {
          float n0;
          CC(1, 0, 0x50); CC(1, 32, 0x00);             /* fader off centre */
          n0 = sent_n ? sent[sent_n - 1].f : 0.0f;
          reset(); ON(1, 65); OFF(1, 65);
          CHECK(sent_n == 1 && is(0, K_TEMPO_SLIDER, OP_VALUE, 1) && sent[0].param == 511 &&
                sent[0].f == 0.0f && sent[0].l == 0x2000, "TEMPO RESET on -> centre");
          CHECK(dk_note_v[0][65] == 0x7F, "TEMPO RESET LED on");
          reset(); CC(1, 0, 0x30); CC(1, 32, 0x00);
          CHECK(sent_n == 0, "fader ignored while TEMPO RESET is on (%d)", sent_n);
          reset(); ON(1, 65); OFF(1, 65);
          CHECK(sent_n == 1 && is(0, K_TEMPO_SLIDER, OP_VALUE, 1) && sent[0].l == (0x30 << 7) &&
                sent[0].f != 0.0f && sent[0].f != n0, "TEMPO RESET off -> the fader's new place");
          CHECK(dk_note_v[0][65] == 0x00, "TEMPO RESET LED off");
          reset(); ON(2, 65); OFF(2, 65);
          CHECK(sent_n == 1 && is(0, K_TEMPO_SLIDER, OP_VALUE, 2), "deck 2 TEMPO RESET is its own");
          reset(); ON(2, 65); OFF(2, 65);
     }

     /* SLIP REVERSE: momentary, slip forced on for the gesture and put back
      * after the release; LED while held */
     slipmode[0] = 0; reversing[0] = 0; slip_sets = 0;
     reset(); ON(1, 21);
     CHECK(slipmode[0] == 1 && sent_n == 1 && is(0, K_REV, OP_PRESS, 1),
           "SLIP REVERSE, slip off: slip on, REV press");
     map_flx10.tick();
     CHECK(dk_note_v[0][21] == 0x7F, "SLIP REVERSE LED lit while held");
     reversing[0] = 1; ms(20); map_flx10.tick();       /* rbp reversing while held */
     reset(); OFF(1, 21);
     CHECK(sent_n == 1 && is(0, K_REV, OP_RELEASE, 1) && slipmode[0] == 1,
           "release: REV release, slip still on for the return");
     reversing[0] = 0;                                 /* rbp ended it on the release */
     ms(20); map_flx10.tick(); ms(20); map_flx10.tick(); ms(20); map_flx10.tick();
     CHECK(slipmode[0] == 0, "reverse over: slip off at once (within ~60 ms)");
     slipmode[0] = 1;                       /* rbp kept slip on after all */
     for (int i = 0; i < 5 && slipmode[0]; i++) { ms(100); map_flx10.tick(); }
     CHECK(slipmode[0] == 0, "slip found still on after the restore: sent off again");
     for (int i = 0; i < 40; i++) { ms(100); map_flx10.tick(); }
     /* quantize on: the release clears getReverse at once and the deck goes
      * on reversing to the beat. Slip put off then left it reversing for good
      * (operator, 2026-10-25): slip stays on until it is really over. */
     reset(); ON(1, 21);
     reversing[0] = 1; ms(20); map_flx10.tick();
     reset(); OFF(1, 21);
     reversing[0] = 0; rev_effect[0] = 1;
     for (int i = 0; i < 5; i++) { ms(100); map_flx10.tick(); }
     CHECK(slipmode[0] == 1, "quantize: slip kept on while the reverse runs to the beat");
     CHECK(dk_note_v[0][21] == 0x7F, "quantize: LED lit while it still reverses");
     rev_effect[0] = 0;                     /* the slip return landed on the beat */
     ms(20); map_flx10.tick(); ms(20); map_flx10.tick(); ms(20); map_flx10.tick();
     CHECK(slipmode[0] == 0, "quantize: slip off once the reverse is over");
     for (int i = 0; i < 40; i++) { ms(100); map_flx10.tick(); }
     /* ...and a reverse that never ends is not waited on for ever */
     reset(); ON(1, 21);
     reversing[0] = 1; ms(20); map_flx10.tick();
     reset(); OFF(1, 21);
     reversing[0] = 0; rev_effect[0] = 1;
     for (int i = 0; i < 95; i++) { ms(100); map_flx10.tick(); }
     CHECK(slipmode[0] == 1, "stuck reverse: still waiting inside 10 s");
     for (int i = 0; i < 10; i++) { ms(100); map_flx10.tick(); }
     CHECK(slipmode[0] == 0, "stuck reverse: slip off after 10 s");
     rev_effect[0] = 0;
     for (int i = 0; i < 40; i++) { ms(100); map_flx10.tick(); }
     /* a tap released before a quantized reverse began: the late reverse is
      * ended when it turns up, then slip goes off */
     reset(); ON(1, 21); OFF(1, 21);
     ms(250); map_flx10.tick();
     CHECK(slipmode[0] == 1, "tap: slip kept while a late reverse may still come");
     reversing[0] = 1; rev_sets = 0; ms(20); map_flx10.tick();
     CHECK(rev_sets == 1 && reversing[0] == 0, "a late reverse after the release is ended");
     ms(20); map_flx10.tick(); ms(20); map_flx10.tick(); ms(20); map_flx10.tick();
     CHECK(slipmode[0] == 0, "...and slip off right after");
     for (int i = 0; i < 40; i++) { ms(100); map_flx10.tick(); }
     /* a tap whose reverse never comes: slip off after the pending window */
     reset(); ON(1, 21); OFF(1, 21);
     for (int i = 0; i < 9; i++) { ms(100); map_flx10.tick(); }
     CHECK(slipmode[0] == 1, "tap, no reverse yet: slip kept inside the window");
     for (int i = 0; i < 3; i++) { ms(100); map_flx10.tick(); }
     CHECK(slipmode[0] == 0, "tap, no reverse ever: slip off after ~1 s");
     for (int i = 0; i < 40; i++) { ms(100); map_flx10.tick(); }
     map_flx10.tick();
     CHECK(dk_note_v[0][21] == 0x00, "SLIP REVERSE LED off");
     /* with slip mode already on, it is left alone */
     slipmode[0] = 1; slip_sets = 0;
     reset(); ON(1, 21); OFF(1, 21);
     for (int i = 0; i < 25; i++) { ms(100); map_flx10.tick(); }
     CHECK(slip_sets == 0 && slipmode[0] == 1 && sent_n == 2, "slip already on: untouched");
     /* REVERSE (SHIFT): latch, slip off while latched, put back after */
     slipmode[0] = 1; reversing[0] = 0;
     reset(); ON(1, 56); OFF(1, 56);
     CHECK(slipmode[0] == 0 && sent_n == 2 && is(0, K_REV, OP_PRESS, 1) && is(1, K_REV, OP_RELEASE, 1),
           "REVERSE with slip on: slip off, REV toggle");
     reversing[0] = 1; map_flx10.tick();
     CHECK(dk_note_v[0][21] == 0x7F, "LED lit while reversing");
     reset(); ON(1, 21); OFF(1, 21);
     CHECK(sent_n == 0, "SLIP REVERSE ignored while REVERSE is latched");
     reset(); ON(1, 56); OFF(1, 56);
     CHECK(sent_n == 2 && is(0, K_REV, OP_PRESS, 1) && slipmode[0] == 1,
           "REVERSE again: REV toggle off, slip mode back on");
     reversing[0] = 0; map_flx10.tick();
     CHECK(dk_note_v[0][21] == 0x00, "LED off");
     slipmode[0] = 0;

     /* beat jump / track search, slip / vinyl */
     /* 4 BEAT JUMP: the plain loop's jump, every press at once; SHIFT = 16 beats;
      * inside a loop rbp's own beat jump (a loop move) */
     looping[0] = looping[1] = 0;
     pj_n = 0; bj_n = 0; reset(); ON(2, 94); OFF(2, 94); ON(1, 95); OFF(1, 95);
     CHECK(pj_n == 2 && pj_deck[0] == 1 && pj_b[0] == -4.0f && pj_deck[1] == 0 && pj_b[1] == 4.0f &&
           bj_n == 0 && sent_n == 0, "BEAT JUMP < deck 2 -4, > deck 1 +4, plain jumps, no keys");
     pj_n = 0; for (int i = 0; i < 3; i++) { ON(1, 98); OFF(1, 98); }
     CHECK(pj_n == 3 && pj_b[0] == 16.0f && pj_b[2] == 16.0f, "SHIFT + BEAT JUMP > x3: three +16 jumps at once");
     /* held: one jump, then from 400 ms the same every 150 ms, until it comes up */
     pj_n = 0; ON(1, 95); ms(300); map_flx10.tick();
     CHECK(pj_n == 1, "BEAT JUMP > held 300 ms: the one jump (%d)", pj_n);
     ms(100); map_flx10.tick();
     CHECK(pj_n == 2 && pj_b[1] == 4.0f, "...at 400 ms it repeats (%d)", pj_n);
     ms(100); map_flx10.tick(); ms(100); map_flx10.tick();
     CHECK(pj_n == 4, "...every 100 ms (%d)", pj_n);
     OFF(1, 95); ms(300); map_flx10.tick();
     CHECK(pj_n == 4, "...and stops when it comes up (%d)", pj_n);
     ON(1, 98); ms(400); map_flx10.tick(); OFF(1, 95); ms(100); map_flx10.tick();
     CHECK(pj_n == 6 && pj_b[5] == 16.0f, "SHIFT + > held repeats 16s; the plain > coming up ends it too (%d)", pj_n);
     OFF(1, 98);
     /* SAMPLER CUE: the preview player's headphone cue (the RX3's LINK CUE), toggled */
     ON(7, 105); OFF(7, 105);
     CHECK(preview_cue == 1, "SAMPLER CUE: LINK CUE on");
     ON(7, 105); OFF(7, 105);
     CHECK(preview_cue == 0, "...pressed again: off");
     /* the input selectors: deck 1's channel off USB B is muted, back on B restored */
     map_flx10.tick();
     CHECK(route_ours[0] == 1 && route_ours[1] == 1, "selectors not reported: both decks routed");
     ON(1, 86); map_flx10.tick();
     CHECK(route_ours[0] == 0 && route_ours[1] == 1, "channel 1 to LINE: deck 1 muted, deck 2 not");
     OFF(1, 86); ON(1, 85); map_flx10.tick();
     CHECK(route_ours[0] == 0, "...to USB A: still muted");
     OFF(1, 85); ON(1, 87); map_flx10.tick();
     CHECK(route_ours[0] == 1, "...back to USB B: routed again");
     ON(2, 86); map_flx10.tick();
     CHECK(route_ours[1] == 0, "channel 2 to LINE: deck 2 muted");
     ON(2, 87); map_flx10.tick();
     CHECK(route_ours[1] == 1, "...back: routed");
     /* near an end, a jump goes as far as whole beats allow (120 BPM: 500 ms a beat) */
     {
          extern int flx10_jump_clamp(int beats, int pos_ms, int total_ms, int bpm_x100);
          CHECK(flx10_jump_clamp(-4, 1500, 60000, 12000) == -3, "3 beats from the start: -4 -> -3");
          CHECK(flx10_jump_clamp(-16, 1500, 60000, 12000) == -3, "...and -16 -> -3");
          CHECK(flx10_jump_clamp(-4, 300, 60000, 12000) == 0, "under a beat: no jump");
          CHECK(flx10_jump_clamp(-4, 2000, 60000, 12000) == -4, "exactly 4 beats: -4");
          CHECK(flx10_jump_clamp(16, 58000, 60000, 12000) == 4, "4 beats from the end: +16 -> +4");
          CHECK(flx10_jump_clamp(4, 30000, 60000, 12000) == 4, "mid-track: as asked");
          CHECK(flx10_jump_clamp(-4, 1500, 60000, 0) == -4, "no BPM known: as asked");
     }
     /* UTILITY's settings as the unit's own (capture_settings) */
     CHECK(flx10_unit_want(1, 0x42, 0) == 0x00 && flx10_unit_want(1, 0x42, 1) == 0x10 &&
           flx10_unit_want(1, 0x43, 2) == 0x20, "MASTER / BOOTH ATT. 0 / -6 / -12 dB -> BF 42/43 00 10 20");
     CHECK(flx10_unit_want(1, 0x42, 3) == -1 && flx10_unit_want(1, 0x42, -1) == -1, "...nothing for an unknown one");
     CHECK(flx10_unit_want(0, 0x45, 0) == 0x00 && flx10_unit_want(0, 0x45, 1) == 0x7F,
           "POWER MANAGEMENT NEVER / 20min -> 9F 45 00 / 7F");
     /* the four rows kept on this machine: rekordbox's defaults until set */
     {
          extern int flx10_setting_get(int num);
          extern int flx10_setting_set(int num, int val);
          CHECK(flx10_setting_get(0x47) == 0x7F && flx10_setting_get(0x49) == 0x7F &&
                flx10_setting_get(0x45) == -1, "kept settings: stereo / on by default, only the four");
          CHECK(flx10_setting_set(0x47, 0x00) == 1 && flx10_setting_get(0x47) == 0x00 &&
                flx10_setting_set(0x47, 0x10) == 0, "master mono kept; a value not 00 / 7F refused");
          flx10_setting_set(0x47, 0x7F);
          CHECK(flx10_setting_get(0x44) == 0x00 && flx10_setting_set(0x44, 0x40) == 1 &&
                flx10_setting_get(0x44) == 0x40 && flx10_setting_set(0x44, 0x7F) == 0 &&
                flx10_setting_set(0x44, 0x15) == 0, "demo / screen saver: disabled by default, 00..40 by 10s");
          flx10_setting_set(0x44, 0x00);
     }
     /* the panel's own long press: 113 / 112, on once it is held, off at the release */
     pj_n = 0; ON(1, 113);
     CHECK(pj_n == 1 && pj_b[0] == 4.0f, "long > (113): a jump at once (%d)", pj_n);
     ms(100); map_flx10.tick(); ms(100); map_flx10.tick();
     CHECK(pj_n == 3, "...then every 100 ms (%d)", pj_n);
     OFF(1, 113); ms(300); map_flx10.tick();
     CHECK(pj_n == 3, "...until it comes up (%d)", pj_n);
     pj_n = 0; ON(2, 112); ms(100); map_flx10.tick(); OFF(2, 112);
     CHECK(pj_n == 2 && pj_deck[0] == 1 && pj_b[1] == -4.0f, "long < (112) on deck 2: -4, repeated (%d)", pj_n);
     looping[1] = 1; pj_n = 0; bj_n = 0; ON(2, 97); OFF(2, 97);
     CHECK(pj_n == 0 && bj_n == 1 && bj_t[0] == 11 && bj_deck[0] == 1, "in a loop: rbp's own -16 (loop move)");
     looping[1] = 0;
     bj_n = 1;
     reset(); ON(1, 23); OFF(1, 23);
     CHECK(sent_n == 0, "SHIFT+SLIP (VINYL) disabled: nothing sent (%d)", sent_n);

     /* the loop section, not looping; each press first primes the loop cache */
     looping[0] = 0;
     loop_primes[0] = 0; ON(1, 16); OFF(1, 16); ON(1, 17); OFF(1, 17);
     CHECK(loop_primes[0] == 2 && loop_primes[1] == 0, "IN / OUT presses prime deck 1's loops (%d)", loop_primes[0]);
     reset(); ON(1, 16); OFF(1, 16);
     CHECK(sent_n == 2 && is(0, K_LOOPIN, OP_PRESS, 1) && is(1, K_LOOPIN, OP_RELEASE, 1), "IN, no loop");
     reset(); ON(1, 17); OFF(1, 17);
     CHECK(sent_n == 2 && is(0, K_LOOPOUT, OP_PRESS, 1), "OUT, no loop");
     reset(); ON(1, 77); OFF(1, 77);
     CHECK(sent_n == 2 && is(0, K_RELOOP, OP_PRESS, 1) && is(1, K_RELOOP, OP_RELEASE, 1), "SHIFT+OUT, no loop -> RELOOP");

     /* 4 BEAT, not looping: IN press + REPEAT + release at once, nothing later */
     reset(); ON(1, 20); OFF(1, 20);
     CHECK(sent_n == 3 && is(0, K_LOOPIN, OP_PRESS, 1) && is(1, K_LOOPIN, OP_REPEAT, 1) &&
           is(2, K_LOOPIN, OP_RELEASE, 1), "4 BEAT -> IN press/repeat/release immediately");
     ms(50); map_flx10.tick();
     CHECK(sent_n == 3, "nothing follows from the tick");

     /* in a loop */
     looping[0] = 1;
     reset(); ON(1, 20); OFF(1, 20);
     CHECK(sent_n == 2 && is(0, K_RELOOP, OP_PRESS, 1), "4 BEAT in a loop -> exit");
     reset(); ON(1, 77); OFF(1, 77);
     CHECK(sent_n == 2 && is(0, K_LOOPOUT, OP_PRESS, 1), "SHIFT+OUT in a loop -> OUT ADJUST");
     reset(); ON(1, 76); OFF(1, 76);
     CHECK(sent_n == 2 && is(0, K_LOOPIN, OP_PRESS, 1), "SHIFT+IN -> IN (adjust)");
     reset(); ON(1, 16); OFF(1, 16);
     CHECK(sent_n == 2 && is(0, 0x4323, OP_PRESS, 1),
           "IN in a loop -> built-in CALL < (0x4323) halves (%d)", sent_n);
     reset(); ON(1, 17); OFF(1, 17);
     CHECK(sent_n == 2 && is(0, 0x4322, OP_PRESS, 1), "OUT in a loop -> built-in CALL > (0x4322)");

     /* with the CALL codes configured: halve/double, release follows the press */
     build("callprev=0x4118,callnext=0x4119,gatecue=0x4130");   /* gatecue: ignored now */
     looping[0] = 1;
     reset(); ON(1, 16); looping[0] = 0; OFF(1, 16);
     CHECK(sent_n == 2 && is(0, 0x4118, OP_PRESS, 1) && is(1, 0x4118, OP_RELEASE, 1),
           "IN in a loop -> CALL <, released to the same key after the loop ended");
     looping[0] = 1;
     reset(); ON(1, 17); OFF(1, 17);
     CHECK(sent_n == 2 && is(0, 0x4119, OP_PRESS, 1), "OUT in a loop -> CALL >");
     looping[0] = 0;

     /* pad modes: the button names a mode; the map sends the presses that get
      * rbp there from what its pad-mode byte reads (0..3 modes, 4..7 banks) */
     {
          static const struct { int note, from, presses, key; const char *what; } pm[] = {
               { 27, 0, 1, K_HOTCUE,   "HOT CUE while in HOT CUE: to GATE CUE (as SHIFT+HOT CUE)" },
               { 27, 4, 1, K_HOTCUE,   "HOT CUE from GATE CUE: one press (back)" },
               { 27, 3, 1, K_HOTCUE,   "HOT CUE from BEAT JUMP: enter" },
               { 105, 0, 1, K_HOTCUE,  "SHIFT+HOT CUE from HOT CUE: flip to GATE CUE" },
               { 105, 2, 2, K_HOTCUE,  "SHIFT+HOT CUE from SLIP LOOP: enter + flip" },
               { 105, 4, 0, K_HOTCUE,  "SHIFT+HOT CUE in GATE CUE: nothing" },
               { 30, 6, 1, K_SLIPLOOP, "PAD FX1 from RELEASE FX: flip back to SLIP LOOP" },
               { 107, 1, 2, K_SLIPLOOP, "PAD FX2 from BEAT LOOP: enter SLIP LOOP + flip" },
               { 32, 7, 1, K_BEATJUMP, "BEAT JUMP from BEAT JUMP 2: flip back" },
               { 32, 3, 1, K_ALOOP,    "BEAT JUMP while in BEAT JUMP: to BEAT LOOP (as SHIFT+BEAT JUMP)" },
               { 32, 1, 1, K_BEATJUMP, "BEAT JUMP while in BEAT LOOP: back to BEAT JUMP" },
               { 30, 2, 1, K_SLIPLOOP, "PAD FX1 while in SLIP LOOP: to RELEASE FX" },
               { 111, 5, 2, K_BEATJUMP, "KEY SHIFT from BEAT LOOP 2: enter BEAT JUMP + flip" },
               { 111, 3, 1, K_BEATJUMP, "KEY SHIFT from BEAT JUMP: flip to BEAT JUMP 2" },
               { 109, 5, 1, K_ALOOP,   "BEAT LOOP from BEAT LOOP 2: one press" },
               { 109, 1, 0, K_ALOOP,   "SHIFT+BEAT JUMP while in BEAT LOOP: nothing" },
               { 109, 0, 1, K_ALOOP,   "BEAT LOOP from HOT CUE: enter" },
               { 34, 0, 2, K_ALOOP,    "SAMPLER from HOT CUE: BEAT LOOP 2 (enter + flip)" },
               { 34, 1, 1, K_ALOOP,    "SAMPLER from BEAT LOOP: flip to BEAT LOOP 2" },
               { 34, 5, 2, K_BEATJUMP, "SAMPLER while in BEAT LOOP 2: to BEAT JUMP 2" },
               { 34, 7, 2, K_ALOOP,    "SAMPLER while in BEAT JUMP 2: back to BEAT LOOP 2" },
               { 27, -1, 1, K_HOTCUE,  "HOT CUE with rbp not up (-1): enter" },
          };
          for (unsigned i = 0; i < sizeof pm / sizeof pm[0]; i++) {
               padmode[0] = pm[i].from;
               reset(); ON(1, pm[i].note); OFF(1, pm[i].note);
               int ok = sent_n == 2 * pm[i].presses;
               for (int k = 0; ok && k < pm[i].presses; k++)
                    ok = is(2 * k, pm[i].key, OP_PRESS, 1) && is(2 * k + 1, pm[i].key, OP_RELEASE, 1);
               CHECK(ok, "%s (%d sent)", pm[i].what, sent_n);
          }
          padmode[1] = 0;
          reset(); ON(2, 105);
          CHECK(sent_n == 2 && is(0, K_HOTCUE, OP_PRESS, 2), "deck 2 SHIFT+HOT CUE on player 2");
          OFF(2, 105);
          padmode[0] = 0;
     }
     /* the SHIFT-mode banks' pads are always bound now */
     reset(); ON(8, 64); OFF(8, 64); ON(8, 80 + 2); OFF(8, 80 + 2); ON(10, 112 + 7); OFF(10, 112 + 7);
     CHECK(sent_n == 6 && is(0, K_PAD1, OP_PRESS, 1) && is(2, K_PAD1 + 2, OP_PRESS, 1) &&
           is(4, K_PAD1 + 7, OP_PRESS, 2), "KEYBOARD / PAD FX2 / KEY SHIFT pads bound");

     /* pads */
     reset(); ON(10, 7 + 32); OFF(10, 7 + 32);
     CHECK(is(0, K_PAD1 + 7, OP_PRESS, 2), "deck 2 pad 8 in BEAT JUMP");
     reset(); ON(10, 50); OFF(10, 50);
     CHECK(sent_n == 2 && is(0, K_PAD1 + 2, OP_PRESS, 2), "SAMPLER-base pad 3 bound (BEAT LOOP 2)");
     reset(); ON(11, 3);
     CHECK(sent_n == 4 && is(0, K_SHIFT, OP_PRESS, 2) && is(1, K_PAD1 + 3, OP_PRESS, 2) &&
           is(2, K_PAD1 + 3, OP_RELEASE, 2) && is(3, K_SHIFT, OP_RELEASE, 2),
           "SHIFT+pad 4 deck 2: rbp's SHIFT held round the pad key (its delete)");
     reset(); ON(9, 64 + 6);
     CHECK(sent_n == 4 && is(1, K_PAD1 + 6, OP_PRESS, 1),
           "SHIFT+pad 7 on deck 1's GATE CUE page: the same");
     reset(); ON(9, 32 + 6);
     CHECK(!(sent_n == 4 && is(0, K_SHIFT, OP_PRESS, 1)), "SHIFT+pad on BEAT JUMP is not a delete");

     /* channel CUE latches; master CUE */
     ON(1, 84); OFF(1, 84);
     CHECK(cue[0] == 1, "ch1 CUE on");
     /* master CUE and the headphone knobs are the unit's own mix: none reaches rbp */
     reset(); mcue = 0; ON(7, 99); OFF(7, 99);
     CHECK(mcue == 0 && sent_n == 0, "master CUE: not sent to rbp");
     reset(); CC(7, 12, 100); CC(7, 13, 100);
     CHECK(sent_n == 0, "HP MIX / HP LEVEL: not sent to rbp (%d sent)", sent_n);

     /* browse: both knobs scroll; pushes load their own deck; hold = fast scroll */
     reset(); CC(7, 64, 1); CC(7, 64, 127);
     CHECK(sent_n == 2 && sent[0].key == K_SELECTOR && sent[0].param == 1 && sent[1].param == -1, "browse");
     reset(); CC(7, 100, 127);
     CHECK(sent_n == 8 && sent[7].param == -1, "SHIFT+browse = 8 steps");
     browse_mode = 3;                           /* a track list */
     reset(); ON(7, 71); map_flx10.tick(); OFF(7, 71);
     CHECK(sent_n == 1 && is(0, K_LOAD, OP_PRESS, 2), "push 71 on a track: LOAD down (%d)", sent_n);
     ms(30); map_flx10.tick();
     CHECK(sent_n == 1, "LOAD still held at 30 ms");
     ms(30); map_flx10.tick();
     CHECK(sent_n == 2 && is(1, K_LOAD, OP_RELEASE, 2), "LOAD up after ~50 ms (%d)", sent_n);
     cursor_track = 0;
     reset(); ON(7, 70); OFF(7, 70);
     CHECK(sent_n == 2 && is(0, K_SELECTOR, OP_PRESS, 1) && is(1, K_SELECTOR, OP_RELEASE, 1),
           "push on a folder/playlist: SELECT only (%d)", sent_n);
     cursor_track = 1;
     /* on SOURCE (browse mode 12) with a stick in slot 1, a push opens USB 1 */
     browse_mode = 12;
     reset(); ON(7, 70); OFF(7, 70);
     CHECK(sent_n == 2 && is(0, K_USB1, OP_PRESS, 1) && is(1, K_USB1, OP_RELEASE, 1),
           "push on SOURCE -> USB1");
     usb1 = 0;
     ms(500); reset(); ON(7, 71); OFF(7, 71);
     CHECK(sent_n == 1 && is(0, K_LOAD, OP_PRESS, 2),
           "push on SOURCE, slot 1 empty -> as elsewhere (%d)", sent_n);
     ms(60); map_flx10.tick();
     usb1 = 1; browse_mode = 3;
     reset(); ON(7, 70); ms(450); map_flx10.tick();
     CHECK(sent_n == 0, "push held 0.45 s: nothing yet (%d)", sent_n);
     ms(100); map_flx10.tick();
     CHECK(sent_n == 2 && is(0, K_SELECTOR, OP_PRESS, 1) && is(1, K_SELECTOR, OP_REPEAT, 1),
           "push held 0.5 s: rbp's long press (selector PRESS + op 1), held (%d)", sent_n);
     CC(7, 64, 1);
     CHECK(sent_n == 3 && sent[2].key == K_SELECTOR && sent[2].op == OP_ROTATE,
           "turning while held scrolls (rbp makes it fast)");
     OFF(7, 70);
     CHECK(sent_n == 4 && is(3, K_SELECTOR, OP_RELEASE, 1),
           "release: selector up, no LOAD (%d)", sent_n);
     /* in BROWSE, a push quickly after a LOAD is no double: the LOAD took rbp
      * back to the player, and that push is the player's own */
     ms(500); reset(); ON(7, 71); OFF(7, 71); ms(60); map_flx10.tick();
     CHECK(sent_n == 2 && is(0, K_LOAD, OP_PRESS, 2), "BROWSE: a push is a LOAD");
     browse_mode = 1;                           /* the LOAD took rbp to the player */
     ms(150); ON(7, 71); OFF(7, 71);
     CHECK(sent_n == 2, "a push on the player: held back (%d)", sent_n);
     ms(450); map_flx10.tick();
     CHECK(sent_n == 4 && is(2, K_SELECTOR, OP_PRESS, 1),
           "...and no second: what a push on the player sends, SELECT (%d)", sent_n);
     /* on the player, two pushes: INST.DOUBLES (rbp's SYNC held past 1 s) */
     ms(500); reset(); ON(7, 71); OFF(7, 71); ms(150); ON(7, 71); OFF(7, 71);
     CHECK(sent_n == 1 && is(0, K_SYNC, OP_PRESS, 2),
           "two pushes on the player: INST.DOUBLES, SYNC down on deck 2, no SELECT (%d)", sent_n);
     ms(600); map_flx10.tick();
     CHECK(sent_n == 1, "...held");
     ms(600); map_flx10.tick();
     CHECK(sent_n == 2 && is(1, K_SYNC, OP_RELEASE, 2), "...up after 1.1 s (%d)", sent_n);
     ms(500); map_flx10.tick();
     CHECK(sent_n == 2, "...and nothing after it");
     browse_mode = 3;
     /* SAMPLER VOL -> SCFX PARAMETER 0x50a7 on both mixer channels */
     reset(); CC(7, 3, 127); CC(7, 35, 9);
     CHECK(sent_n == 2 && sent[0].key == 0x50a7 && sent[0].ch == 1 && sent[1].ch == 2 &&
           sent[0].param == 1023, "SAMPLER VOL -> PARAMETER on ch 1 and 2 (%d)", sent_n);
     browse_mode = 4;
     reset(); ON(7, 122); OFF(7, 122);
     CHECK(sent_n == 2 && is(0, K_BROWSE, OP_PRESS, 1) && is(1, K_BROWSE, OP_RELEASE, 1),
           "VIEW while browsing -> BROWSE (back to the player)");
     browse_mode = 12;
     reset(); ON(7, 122); OFF(7, 122);
     CHECK(sent_n == 2 && is(0, K_SOURCE, OP_PRESS, 1), "VIEW on SOURCE -> SOURCE (closes it)");
     {
          int had = usb1;
          usb1 = 0; browse_mode = 1; map_flx10.tick();      /* the stick gone: nothing browsed */
          usb1 = 1; map_flx10.tick();
          reset(); ON(7, 122); OFF(7, 122);
          CHECK(sent_n == 2 && is(0, K_SOURCE, OP_PRESS, 1),
                "VIEW on the player, nothing browsed yet -> SOURCE");
          browse_mode = 3; map_flx10.tick(); browse_mode = 1;
          reset(); ON(7, 122); OFF(7, 122);
          CHECK(sent_n == 2 && is(0, K_BROWSE, OP_PRESS, 1),
                "...a list browsed: VIEW on the player -> BROWSE, back to it");
          usb1 = had; browse_mode = 12;
     }
     /* VIEW held: the FLX10 sends 103 on+off, then a lone 122 off -- MENU
      * tapped on a list or SOURCE (LIST MENU / MY SETTINGS), nothing on the player */
     for (int i = 0; i < 3; i++) {
          browse_mode = i == 0 ? 3 : i == 1 ? 12 : 1;
          reset(); ON(7, 103); OFF(7, 103);
          if (browse_mode == 1) {
               CHECK(sent_n == 0, "VIEW held on the player: nothing (%d)", sent_n);
          } else {
               CHECK(sent_n == 1 && is(0, K_MENU, OP_PRESS, 1),
                     "VIEW held on mode %d -> MENU down (%d)", browse_mode, sent_n);
               ms(60); map_flx10.tick();
               CHECK(sent_n == 2 && is(1, K_MENU, OP_RELEASE, 1),
                     "...tapped, not held to UTILITY (%d)", sent_n);
          }
          OFF(7, 122); ms(60); map_flx10.tick();
          CHECK(sent_n == (browse_mode == 1 ? 0 : 2), "...and VIEW's lone release sends nothing");
     }
     /* a push while a popup menu is up is its SELECT, never a LOAD, held ~50 ms:
      * rbp's popups ignore an instant press and release */
     browse_mode = 3; cursor_track = 1; menu_open = 1;
     reset(); ON(7, 70); OFF(7, 70);
     CHECK(sent_n == 1 && is(0, K_SELECTOR, OP_PRESS, 1), "push in a menu: SELECT down (%d)", sent_n);
     ms(30); map_flx10.tick();
     CHECK(sent_n == 1, "...still held at 30 ms");
     ms(30); map_flx10.tick();
     CHECK(sent_n == 2 && is(1, K_SELECTOR, OP_RELEASE, 1), "...up after ~50 ms, no LOAD (%d)", sent_n);
     browse_mode = 12;
     reset(); ON(7, 70); OFF(7, 70); ms(60); map_flx10.tick();
     CHECK(sent_n == 2 && is(0, K_SELECTOR, OP_PRESS, 1),
           "MY SETTINGS on SOURCE: the push selects, it does not open USB1 (%d)", sent_n);
     browse_mode = 1;
     reset(); ON(7, 71); OFF(7, 71);
     CHECK(sent_n == 1 && is(0, K_SELECTOR, OP_PRESS, 1),
           "a menu on the player: SELECT at once, not held back for a double (%d)", sent_n);
     ms(450); map_flx10.tick();
     CHECK(sent_n == 2 && is(1, K_SELECTOR, OP_RELEASE, 1), "...and only its release after it");
     menu_open = 0; browse_mode = 3;
     reset(); CC(7, 64, 1); ON(7, 70); OFF(7, 70);
     CHECK(sent_n == 2 && is(1, K_LOAD, OP_PRESS, 1), "menu closed: a push on a track LOADs again");
     ms(60); map_flx10.tick();
     browse_mode = 1;
     reset(); ON(7, 122); ON(7, 93);
     CHECK(is(0, K_BROWSE, OP_PRESS, 1) && is(1, K_TRACKFILTER, OP_PRESS, 1) &&
           is(2, K_TRACKFILTER, OP_REPEAT, 1),
           "VIEW on the player, a list browsed -> BROWSE (the last place), SHIFT+push -> "
           "TRACK FILTER");
     OFF(7, 122); OFF(7, 93);

     /* the start-up splash: the controller is ignored while it is up, and asked
      * for its controls again the moment it ends */
     splash_up = 1; map_flx10.tick(); reset(); query_n = 0;
     ON(7, 122); OFF(7, 122);
     CHECK(sent_n == 0, "splash up: VIEW goes nowhere (%d sent)", sent_n);
     map_flx10.tick();
     CHECK(query_n == 0, "...and nothing is asked while it is up");
     splash_up = 0; map_flx10.tick();
     CHECK(query_n == 1, "splash over: the unit is asked for its controls once");
     map_flx10.tick();
     CHECK(query_n == 1, "...only once");
     ON(7, 122); OFF(7, 122);
     CHECK(sent_n == 2, "...and the controller is live again");

     /* SHIFT + VIEW: SHORTCUT on a short press (on the release), rbp's UTILITY
      * (MENU held 500 ms) on a long one; while the shim's panel is up the browse
      * knob, its push and BACK work it */
     reset(); panel_cmds_n = 0; ON(7, 104);
     CHECK(sent_n == 0, "SHIFT+VIEW press: nothing yet (the length decides)");
     ms(300); map_flx10.tick(); OFF(7, 104);
     CHECK(sent_n == 2 && is(0, K_SHORTCUT, OP_PRESS, 1) && is(1, K_SHORTCUT, OP_RELEASE, 1) &&
           panel_cmds_n == 0, "short SHIFT+VIEW -> SHORTCUT");
     reset(); ON(7, 104); ms(1100); map_flx10.tick(); OFF(7, 104);
     CHECK(sent_n == 1 && is(0, K_MENU, OP_PRESS, 1) && panel_cmds_n == 0,
           "SHIFT+VIEW held 1 s -> MENU down, no SHORTCUT, no panel");
     ms(400); map_flx10.tick();
     CHECK(sent_n == 1, "...MENU still held at 400 ms");
     ms(150); map_flx10.tick();
     CHECK(sent_n == 2 && is(1, K_MENU, OP_RELEASE, 1), "...and let go after 500 ms: UTILITY");
     panel_up = 1; reset(); panel_cmds_n = 0; ON(7, 104); ms(1100); map_flx10.tick();
     OFF(7, 104); ms(600); map_flx10.tick();
     CHECK(panel_cmds_n == 1 && panel_cmds[0] == SR_CLOSE && sent_n == 2,
           "with the shim's panel up, the long press closes it and opens UTILITY");
     panel_up = 1; reset(); panel_cmds_n = 0;
     CC(7, 64, 1); CC(7, 64, 127); ON(7, 70); OFF(7, 70); ON(7, 101); OFF(7, 101);
     CHECK(sent_n == 0 && panel_cmds_n == 5 && panel_cmds[0] == SR_NEXT &&
           panel_cmds[1] == SR_PREV && panel_cmds[2] == SR_PRESS &&
           panel_cmds[3] == SR_RELEASE && panel_cmds[4] == SR_CLOSE,
           "panel up: knob selects, push presses / releases, BACK closes -- rbp gets none");
     reset(); panel_cmds_n = 0; ON(7, 71); panel_up = 0; OFF(7, 71);
     CHECK(sent_n == 0 && panel_cmds_n == 2 && panel_cmds[1] == SR_RELEASE,
           "a push the panel took keeps its release, the panel gone or not");
     panel_up = 0;

     /* BACK: rbp's BACK, but SOURCE on the category column and on the player, where
      * rbp's does nothing */
     browse_mode = 1;
     reset(); ON(7, 101); OFF(7, 101);
     CHECK(sent_n == 2 && is(0, K_SOURCE, OP_PRESS, 1) && is(1, K_SOURCE, OP_RELEASE, 1),
           "BACK on the player -> SOURCE");
     browse_mode = 3;
     reset(); ON(7, 101); OFF(7, 101);
     CHECK(sent_n == 2 && is(0, K_BACK, OP_PRESS, 1) && is(1, K_BACK, OP_RELEASE, 1),
           "BACK in a list -> BACK");
     on_categories = 1;
     reset(); ON(7, 101); on_categories = 0; OFF(7, 101);
     CHECK(sent_n == 2 && is(0, K_SOURCE, OP_PRESS, 1) && is(1, K_SOURCE, OP_RELEASE, 1),
           "BACK on the category column -> SOURCE, its release with it");

     /* LINK: the USB stick's eject. Light on with a stick ready; held 2 s it
      * ejects and blinks; off once the stick has left the mount table. */
     usb_ready[0] = usb_mounted[0] = 1; usb_ejects[0] = 0;
     ms(20); map_flx10.tick();
     CHECK(dk_note_v[0][74] == 0x7F && dk_note_v[1][74] == 0x00,
           "LINK lit for the stick in USB1, dark for an empty USB2");
     reset(); ON(1, 74); ms(10); map_flx10.tick();
     CHECK(dk_note_v[0][74] == 0x00, "LINK pressed: the blink starts at once, dark half first");
     ms(300); map_flx10.tick();
     CHECK(dk_note_v[0][74] == 0x7F, "...and blinks");
     ms(300); map_flx10.tick(); OFF(1, 74); ms(20); map_flx10.tick();
     CHECK(usb_ejects[0] == 0 && sent_n == 0 && dk_note_v[0][74] == 0x7F,
           "let go early: no eject, steady again");
     ON(1, 74); ms(10); map_flx10.tick();
     {
          int flips = 0, last = dk_note_v[0][74];
          for (int i = 0; i < 9; i++) {              /* 0.01 .. 0.91 s: slow halves */
               ms(100); map_flx10.tick();
               flips += dk_note_v[0][74] != last;
               last = dk_note_v[0][74];
          }
          CHECK(flips >= 2 && flips <= 4, "the first second blinks slowly (%d flips)", flips);
          flips = 0;
          for (int i = 0; i < 15; i++) {             /* 1.0 .. 1.9 s: fast halves */
               ms(60); map_flx10.tick();
               flips += dk_note_v[0][74] != last;
               last = dk_note_v[0][74];
          }
          CHECK(flips >= 6 && flips <= 8, "the second blinks twice as fast (%d flips)", flips);
     }
     CHECK(usb_ejects[0] == 0, "not yet at 1.8 s");
     ms(250); map_flx10.tick();                      /* 2.06 s */
     CHECK(usb_ejects[0] == 1, "held 2 s: USB1 ejected");
     OFF(1, 74);
     {
          int seen_on = 0, seen_off = 0;
          for (int i = 0; i < 8; i++) {
               ms(130); map_flx10.tick();
               seen_on |= dk_note_v[0][74] == 0x7F;
               seen_off |= dk_note_v[0][74] == 0x00;
          }
          CHECK(seen_on && seen_off, "still blinking after the release, while it ejects");
     }
     usb_ready[0] = 0; ms(20); map_flx10.tick();
     usb_mounted[0] = 0; ms(300); map_flx10.tick(); ms(300); map_flx10.tick();
     CHECK(dk_note_v[0][74] == 0x00 && usb_ejects[0] == 1,
           "off once rbp lets go and the stick is unmounted; one eject only");
     ON(2, 74); ms(10); map_flx10.tick(); ms(2100); map_flx10.tick(); OFF(2, 74);
     CHECK(usb_ejects[1] == 0, "a hold with no stick in USB2 ejects nothing");

     /* Sound Color FX radio group */
     reset(); ON(7, 0);
     CHECK(sent_n == 4 && is(0, 0x50a1, OP_PRESS, 1) && is(2, 0x50a1, OP_PRESS, 2),
           "SPACE -> 0x50a1 on both players (measured, docs/21)");
     reset(); OFF(7, 0); ON(7, 2);
     CHECK(sent_n == 4 && is(0, 0x50a5, OP_PRESS, 1), "CRUSH -> 0x50a5");
     reset(); OFF(7, 2); ON(7, 0);
     reset(); OFF(7, 0); ON(7, 3); ms(80); map_flx10.tick();
     CHECK(sent_n == 4 && sent[0].key == K_SWEEP, "PITCH -> SWEEP, and the old one's off is not a toggle");
     reset(); OFF(7, 3); ms(80); map_flx10.tick();
     CHECK(sent_n == 4 && sent[0].key == K_SWEEP, "deselect -> SWEEP pressed again");

     /* Beat FX */
     /* every effect position against rbp's MEASURED switch order (docs/13 S5.9):
      * 0 DELAY 1 ECHO 2 PING PONG 3 SPIRAL 4 HELIX 5 REVERB 6 FLANGER 7 PHASER
      * 8 FILTER 9 TRANS 10 ROLL 11 SLIP ROLL 12 PITCH 13 VINYL BRAKE */
     {
          static const struct { int note; int pos; const char *name; } fx[] = {
               /* by name, and the rest by the RX3 dial's closest detent */
               { 32, 2, "LOW CUT ECHO -> PING PONG" }, { 33, 1, "ECHO" },
               { 34, 0, "MT DELAY -> DELAY" }, { 35, 3, "SPIRAL" }, { 36, 5, "REVERB" },
               { 37, 9, "TRANS" }, { 38, 8, "ENIGMA JET -> FILTER" }, { 39, 6, "FLANGER" },
               { 40, 7, "PHASER" }, { 41, 12, "STRETCH -> PITCH" }, { 42, 11, "SLIP ROLL" },
               { 43, 10, "ROLL" }, { 44, 13, "MOBIUS saw -> VINYL BRAKE" },
               { 45, 4, "MOBIUS tri -> HELIX" },
          };
          for (unsigned i = 0; i < sizeof fx / sizeof fx[0]; i++) {
               reset(); ON(5, fx[i].note); OFF(5, fx[i].note);
               if (fx[i].pos < 0)
                    CHECK(sent_n == 0, "%s: no RX3 effect (%d sent)", fx[i].name, sent_n);
               else
                    CHECK(sent_n == 1 && sent[0].key == K_BFXTYPE && sent[0].param == fx[i].pos,
                          "%s -> RX3 position %d (got %ld)", fx[i].name, fx[i].pos,
                          sent_n ? sent[0].param : -99L);
          }
     }
     /* the FX screen's three SysEx, byte for byte against rekordbox's (capture_fx) */
     {
          extern int flx10_fx_name_sysex(const char *, unsigned char *);
          extern int flx10_fx_beat_sysex(const char *, unsigned char *);
          extern int flx10_fx_bpm_sysex(int, int, unsigned char *);
          extern void flx10_fx_beat_text(int, int, int, char *, int);
          static const unsigned char head[10] = {
               0xF0, 0x00, 0x40, 0x05, 0x00, 0x00, 0x04, 0x01, 0x00, 0x00 };
          static const unsigned char echo[] = { 0x00, 0x28, 0x00, 0x26, 0x00,
               0x0a, 0x0d, 0x42, 0x74, 0x09 };
          static const unsigned char low_cut[] = { 0x00, 0x28, 0x00, 0x26, 0x00,
               0x18, 0x3d, 0x3a, 0x05, 0x64, 0x50, 0x2a, 0x54, 0x40, 0x14, 0x1a, 0x04,
               0x69, 0x13 };
          unsigned char sx[64];
          char t[8];
          int n, zero = 1;

          n = flx10_fx_name_sysex("ECHO", sx);
          for (int k = 16 + 5; k < 53; k++)
               zero &= sx[k] == 0;
          CHECK(n == 54 && !memcmp(sx, head, 10) && sx[10] == 0x0A &&
                !memcmp(sx + 11, echo, sizeof echo) && zero && sx[53] == 0xF7,
                "FX name ECHO packs as rekordbox's");
          flx10_fx_name_sysex("LOW CUT ECHO", sx);
          CHECK(!memcmp(sx + 11, low_cut, sizeof low_cut), "FX name LOW CUT ECHO");
          n = flx10_fx_beat_sysex("1/2", sx);
          CHECK(n == 18 && sx[10] == 0x0B && sx[11] == 0x31 && sx[12] == 0x5e &&
                sx[13] == 0x48 && sx[14] == 0x01 && sx[15] == 0 && sx[16] == 0 &&
                sx[17] == 0xF7, "FX beat 1/2");
          flx10_fx_beat_sysex("-1/16", sx);
          CHECK(sx[11] == 0x2d && sx[12] == 0x62 && sx[13] == 0x3c && sx[14] == 0x09 &&
                sx[15] == 0x63 && sx[16] == 0x06, "FX beat -1/16 fills all six bytes");
          n = flx10_fx_bpm_sysex(13590, 0, sx);
          CHECK(n == 20 && sx[10] == 0x0C && !memcmp(sx + 11,
                "\x00\x00\x03\x05\x01\x06\x00\x00", 8) && sx[19] == 0xF7,
                "FX BPM 135.90");
          flx10_fx_bpm_sysex(12000, 1, sx);
          CHECK(!memcmp(sx + 11, "\x00\x00\x02\x0e\x0e\x00\x01\x00", 8),
                "FX BPM 120.00, TAP");
          flx10_fx_beat_text(5, 0, 0, t, sizeof t);
          CHECK(!strcmp(t, "1"), "rung 5 reads 1 (got %s)", t);
          flx10_fx_beat_text(4, 0, 0, t, sizeof t);
          CHECK(!strcmp(t, "3/4"), "rung 4 reads 3/4 (got %s)", t);
          flx10_fx_beat_text(0, 0, 0, t, sizeof t);
          CHECK(!strcmp(t, "1/16"), "rung 0 reads 1/16 (got %s)", t);
          flx10_fx_beat_text(11, 0, 0, t, sizeof t);
          CHECK(!strcmp(t, "64"), "rung 11 reads 64 (got %s)", t);
          flx10_fx_beat_text(1, 10, 1, t, sizeof t);
          CHECK(!strcmp(t, "10%"), "a percent effect reads its percent, not its rung (got %s)", t);
          flx10_fx_beat_text(5, -50, 1, t, sizeof t);
          CHECK(!strcmp(t, "-50%"), "PITCH reads a signed percent (got %s)", t);
     }
     reset(); fx_on = 0; ON(5, 70);
     CHECK(sent_n == 2 && is(0, K_BFX, OP_PRESS, 1) && is(1, K_BFX, OP_RELEASE, 1),
           "Beat FX ON/OFF is note 70: 7f with rbp's FX off is a tap of rbp's BFX key");
     fx_on = 1; OFF(5, 70);
     CHECK(sent_n == 4 && is(2, K_BFX, OP_PRESS, 1), "00 with rbp's FX on: another tap");
     fx_on = 0; OFF(5, 70);
     CHECK(sent_n == 4, "00 with rbp's FX already off: nothing");
     fx_on = 1; ON(5, 70);
     CHECK(sent_n == 4, "7f with rbp's FX already on: nothing");
     fx_on = -1; ON(5, 70); OFF(5, 70);
     CHECK(sent_n == 4, "rbp not up: nothing");
     fx_on = 0; ms(400); query_at = shim_now_ms(); ms(100); ON(5, 70);
     CHECK(sent_n == 6 && is(4, K_BFX, OP_PRESS, 1),
           "the controls query's answer reports the latch on, rbp's FX off: rbp follows");
     fx_on = 1; ms(2000); query_at = shim_now_ms(); ms(100); ON(5, 70);
     CHECK(sent_n == 6, "...and the next answer, rbp already on: nothing");
     query_at = 0; fx_on = 0;
     reset(); ON(5, 71); OFF(5, 71);
     CHECK(sent_n == 0, "note 71 on the FX channel: nothing");
     reset(); CC(5, 2, 64);
     CHECK(sent_n == 1 && sent[0].key == K_DEPTH, "LEVEL -> DEPTH");
     reset(); ON(1, 63); CC(5, 2, 65); CC(5, 2, 63); OFF(1, 63);
     CHECK(sent_n == 2 && sent[0].key == K_TIME && sent[0].param == 1 && sent[1].param == -1,
           "SHIFT + LEVEL -> TIME steps");
     /* the channel selector: 1 2 MST MIC send rbp's target; 3 4 SP send nothing */
     {
          static const struct { int note, target; } ch[] = {
               { 16, 0 }, { 17, 1 }, { 18, -1 }, { 19, -1 }, { 20, 5 }, { 21, 2 }, { 22, -1 },
          };
          for (unsigned i = 0; i < 7; i++) {
               reset(); ON(5, ch[i].note); OFF(5, ch[i].note);
               if (ch[i].target < 0)
                    CHECK(sent_n == 0, "channel selector note %d: nothing", ch[i].note);
               else
                    CHECK(sent_n == 1 && sent[0].key == K_BFXCH && sent[0].param == ch[i].target,
                          "channel selector note %d -> target %d", ch[i].note, ch[i].target);
          }
     }
     /* SHIFT + BACK: PLAYLIST from the player, TAG LIST elsewhere */
     browse_mode = 1;
     reset(); ON(7, 102); OFF(7, 102);
     CHECK(sent_n == 2 && is(0, K_PLAYLIST, OP_PRESS, 1) && is(1, K_PLAYLIST, OP_RELEASE, 1),
           "SHIFT+BACK on the player -> PLAYLIST");
     browse_mode = 3;                           /* the playlist view it opened */
     reset(); ON(7, 102); OFF(7, 102);
     CHECK(sent_n == 2 && is(0, K_BROWSE, OP_PRESS, 1) && is(1, K_BROWSE, OP_RELEASE, 1),
           "SHIFT+BACK again from that list -> BROWSE");
     ms(250); map_flx10.tick();
     CHECK(sent_n == 4 && is(2, K_BROWSE, OP_PRESS, 1), "...and BROWSE again while not on the player");
     browse_mode = 1;
     reset(); ON(7, 102); OFF(7, 102);         /* open again */
     browse_mode = 3; ON(7, 102); OFF(7, 102);
     browse_mode = 1; ms(250); map_flx10.tick();
     CHECK(sent_n == 4, "no second BROWSE once the player is up (%d)", sent_n);
     browse_mode = 3;
     reset(); ON(7, 102); OFF(7, 102);
     CHECK(sent_n == 2 && is(0, K_TAGLIST, OP_PRESS, 1), "SHIFT+BACK in a list it did not open -> TAG LIST");
     browse_mode = 1;

     reset(); ON(2, 80); OFF(2, 80);
     CHECK(timemode_n[1] == 1 && timemode_n[0] == 0 && sent_n == 0,
           "SHIFT + 4 BEAT on deck 2 -> its time mode flipped, once");
     reset(); bpm_auto_n = 0; ON(5, 102); OFF(5, 102);
     CHECK(bpm_auto_n == 1 && sent_n == 0, "SHIFT + BEAT < -> Beat FX BPM AUTO, once");
     reset(); ON(5, 107); ON(5, 74);
     CHECK(is(0, K_TAP, OP_PRESS, 1) && sent[1].key == K_BEATPREV, "TAP, BEAT <");

     /* faders: MSB drives, LSB dropped; strip 3 ignored; booth ignored */
     reset(); CC(1, 19, 127); CC(1, 51, 5); CC(3, 19, 64); CC(7, 9, 64); CC(7, 41, 1);
     CHECK(sent_n == 1 && sent[0].key == K_FADER && sent[0].ch == 1, "fader 1 only (%d)", sent_n);

     /* the jog: a BURST inside one millisecond must not read as the 8 rev/s clamp
      * (the bug the operator felt), a slow turn must read slow, and the idle edge
      * must still send 0. 12960 counts/rev (measured ~13083; 18 x 720). */
     {
          ms(200); map_flx10.tick();          /* let any earlier motion go idle */
          reset();
          for (int i = 0; i < 10; i++) CC(1, 34, 64 + 10);   /* 100 counts at once */
          CHECK(sent_n == 0, "a burst inside the window sends nothing yet (%d)", sent_n);
          ms(5); CC(1, 34, 64 + 10);                          /* 110 counts, ~5 ms */
          /* rekordbox's 0.139 ms a count over rbp's 1000 ms turn: ~3 rev/s */
          CHECK(sent_n == 1 && sent[0].key == K_JOG_ROT && sent[0].f > 1.0f && sent[0].f < 7.0f,
                "burst -> about 4 rev/s with the gain, not the clamp (got %.3f)", sent_n ? sent[0].f : -1.0f);
          float fast = sent_n ? sent[0].f : 0.0f;
          ms(200); map_flx10.tick();
          CHECK(sent_n >= 2 && sent[sent_n - 1].f == 0.0f, "idle edge sends speed 0");
          reset();
          CC(1, 34, 64 + 1);
          for (int i = 0; i < 4; i++) { ms(10); CC(1, 34, 64 + 1); }
          CHECK(sent_n >= 3 && sent[sent_n - 1].f > 0.0f && sent[sent_n - 1].f < 0.12f,
                "slow turn -> slow (got %.4f)", sent_n ? sent[sent_n - 1].f : -1.0f);
          CHECK(fast > 20.0f * (sent_n ? sent[sent_n - 1].f : 1.0f), "fast is much faster than slow");
          ms(200); map_flx10.tick();
          /* one turn of the top (12960 counts) moves the track 1.8 s, as
           * the arm: 12960 x 0.1391 ms x 720 / rbp's 1000 ms turn = ~1298 */
          reset();
          long before = -1;
          CC(2, 35, 64 + 1); ms(5); CC(2, 35, 64 + 1);
          before = sent_n ? sent[sent_n - 1].l : -1;
          for (int n = 0; n < 12960; n += 21) { CC(2, 35, 64 + 21); }
          ms(5); CC(2, 35, 64 + 1); ms(30); map_flx10.tick();
          long after = sent[sent_n - 1].l;
          long moved = (after - before) & 0xFFFF;
          CHECK(sent[0].ch == 2 && moved > 1285 && moved < 1310, "one top turn moves rbp's position ~1297 (%ld)", moved);
          ms(200); map_flx10.tick();
          /* the side ring: one turn is ~12544 counts and also 1.8 s */
          reset();
          CC(2, 33, 64 + 1); ms(5); CC(2, 33, 64 + 1);
          before = sent_n ? sent[sent_n - 1].l : -1;
          for (int n = 0; n < 12544; n += 20) { CC(2, 33, 64 + 20); }
          ms(5); CC(2, 33, 64 + 1); ms(30); map_flx10.tick();
          moved = (sent[sent_n - 1].l - before) & 0xFFFF;
          CHECK(moved > 1285 && moved < 1310, "one side-ring turn moves it the same (%ld)", moved);
          ms(200); map_flx10.tick();
     }

     /* ---- LEDs (flx10-leds.json) ---- */
     {
          extern int flx10_led_pad_base(int deck, int rbp_mode);
          const struct led_notes *n = map_flx10.leds;
          CHECK(n != NULL, "map_flx10 has an LED table");
          if (n) {
               CHECK(n->deck_ch == 0 && n->n_sync == 88 && n->n_cue == 12 &&
                     n->n_play == 11 && n->n_keylock == -1 && n->n_slip == 64,
                     "deck LED notes");
               CHECK(n->n_loopin == 16 && n->n_loopout == 17 && n->n_autoloop == 20,
                     "loop LED notes");
               CHECK(n->n_vinyl == -1 && n->master_ch_first < 0 &&
                     n->meter_master_ch < 0, "unanswered LEDs stay dark");
               CHECK(n->fx_ch < 0 && n->n_fx[LED_FX_BFX_ONOFF] == -1,
                     "no Beat FX ON/OFF LED: the FLX10 echoes it back as a press");
               CHECK(n->n_quantize == 53, "QUANTIZE LED on 53");
               CHECK(n->loop_style == LED_LOOP_REKORDBOX, "rekordbox loop LEDs");
               CHECK(n->mode_banks == 1 && n->n_mode[0] == 27 && n->n_mode[1] == 109 &&
                     n->n_mode[2] == 30 && n->n_mode[3] == 32 &&
                     n->n_mode_bank[0] == 105 && n->n_mode_bank[1] == 34 &&
                     n->n_mode_bank[2] == 107 && n->n_mode_bank[3] == 111,
                     "mode LEDs by rbp mode 0..7");
               CHECK(n->pad_ch == 7 && n->pad_ch2 == 9 &&
                     n->pad_enc == LED_ENC_FLX10_PALETTE && n->pad_base != NULL,
                     "pad LEDs: ch 8/10, palette, unit page");
               CHECK(n->n_meter_cc == 2 && n->meter_ch_first == 0 &&
                     n->meter_enc == METER_ENC_FLX4_LEVEL, "meters on CC 2");
               CHECK(n->strip_ch_first == 0 && n->strip_count == 2 && n->n_strip_cue == 84,
                     "channel CUE LEDs");
          }
          /* setting a loop-in point asks for the 4-flash; halving does not */
          led_loop_flash[0] = led_loop_flash[1] = 0;
          looping[0] = 0; reset(); ON(1, 16); OFF(1, 16);
          CHECK(led_loop_flash[0] == 1 && led_loop_flash[1] == 0, "IN outside a loop -> flash deck 1");
          led_loop_flash[0] = 0; looping[0] = 1; reset(); ON(1, 16); OFF(1, 16);
          CHECK(led_loop_flash[0] == 0, "IN in a loop (halve) -> no flash");
          looping[0] = 0;
          /* BPM SysEx, as rekordbox sends it */
          {
               extern void flx10_bpm_sysex(int, int, unsigned char *);
               static const unsigned char want[17] = { 0xF0, 0x00, 0x40, 0x05, 0x00, 0x00,
                    0x04, 0x01, 0x00, 0x12, 0x00, 0x00, 0x02, 0x0D, 0x09, 0x05, 0xF7 };
               unsigned char sx[17];
               flx10_bpm_sysex(1, 11669, sx);       /* deck 2, 116.69 = 0x2D95 */
               CHECK(memcmp(sx, want, 17) == 0,

                     "116.69 on deck 2 -> ... 12 00 00 02 0D 09 05 F7");
               flx10_bpm_sysex(0, 8200, sx);
               CHECK(sx[9] == 0x11 && sx[12] == 0x02 && sx[15] == 0x08, "82.00 deck 1 = 11 00 00 02 00 00 08");
               flx10_bpm_sysex(0, 0, sx);
               CHECK(sx[10] == 0x0F && sx[15] == 0x0F, "empty deck -> 0F x6 (blank)");
          }
          /* key text -> the FLX10's key code (table read off the unit) */
          {
               extern int flx10_key_code(const char *);
               static const struct { const char *k; int code; } kt[] = {
                    { "", 0 }, { "8B", 1 }, { "8A", 2 }, { "3B", 3 }, { "11B", 19 },
                    { "11A", 20 }, { "1A", 24 }, { "12A", 10 }, { "C", 1 }, { "Am", 2 },
                    { "Db", 3 }, { "C#", 3 }, { "Bbm", 4 }, { "A#m", 4 }, { "F#m", 20 },
                    { "Gbm", 20 }, { "Ebm", 14 }, { "B", 23 }, { "G#m", 24 }, { "Abm", 24 },
                    { "x", 0 }, { "13A", 0 },
               };
               for (unsigned q = 0; q < sizeof kt / sizeof kt[0]; q++)
                    CHECK(flx10_key_code(kt[q].k) == kt[q].code, "key '%s' -> code %d (got %d)",
                          kt[q].k, kt[q].code, flx10_key_code(kt[q].k));
          }
          /* tempo fader ends: both reach +-1 exactly */
          CHECK(pitch_norm_from_pos14(0) == -1.0f && pitch_norm_from_pos14(16383) == 1.0f &&
                pitch_norm_from_pos14(16378) == 1.0f && pitch_norm_from_pos14(8192) == 0.0f &&
                pitch_norm_from_pos14(5) == -1.0f,
                "tempo fader: 0/16383/16378/8192/5 -> -1/+1/+1/0/-1");
          CHECK(pitch_norm_from_pos14(8192 + 4096) > 0.49f && pitch_norm_from_pos14(8192 + 4096) < 0.51f,
                "tempo fader half way up is ~+0.5");
          /* jog display values */
          {
               extern void flx10_jog_values(const struct deck_info *, int *, int *,
                                            int *, int *, int *);
               struct deck_info di = { 0 };
               int b, s, mi, se, dg;
               di.loaded = 1; di.total_ms = 300000; di.elapsed_ms = 155000;
               di.bpm_raw = 8199; di.tempo_raw = -250;
               flx10_jog_values(&di, &b, &s, &mi, &se, &dg);
               CHECK(b == 819, "BPM 81.99 -> 819 (x10, truncated as the RX3 shows) (%d)", b);
               { struct deck_info d2 = di; int b2, s2, m2, e2, g2;
                 d2.bpm_raw = 11669; flx10_jog_values(&d2, &b2, &s2, &m2, &e2, &g2);
                 CHECK(b2 == 1166, "116.69 -> 116.6 like the RX3 screen (%d)", b2); }
               CHECK(s == 975, "tempo -2.50%% -> (pct+100)*10 = 975 (%d)", s);
               CHECK(mi == 2 && se == 25, "remaining 145 s -> 2:25 (%d:%d)", mi, se);
               CHECK(dg == 40, "155000 ms -> 200 ms into a 1.8 s turn = 40 deg (%d)", dg);
               CHECK(n && n->deck_display != NULL, "the FLX10 takes deck display values");
          }
          /* the tempo % sent is the speed the deck plays at */
          {
               extern int flx10_shown_tempo(const struct deck_info *);
               struct deck_info di = { 0 };
               di.loaded = 1; di.bpm_raw = 13600; di.bpm_orig = 10000; di.tempo_raw = -200;
               CHECK(flx10_shown_tempo(&di) == 3600,
                     "sync off, fader untouched: +36%% played, not the fader's -2%% (%d)",
                     flx10_shown_tempo(&di));
               di.sync = 1;
               CHECK(flx10_shown_tempo(&di) == 3600, "synced: the same (%d)", flx10_shown_tempo(&di));
               di.bpm_orig = 0;
               CHECK(flx10_shown_tempo(&di) == -200, "no track BPM: rbp's own value");
          }
          /* the pad page follows the unit's own mode buttons */
          reset(); ON(1, 30); OFF(1, 30);
          CHECK(flx10_led_pad_base(0, pmode(16)) == 16, "PAD FX1 -> page 16");
          CHECK(flx10_led_pad_base(0, 0) == -2, "page 16 while rbp is still in HOT CUE -> wait (-2)");
          reset(); ON(1, 109); OFF(1, 109);
          CHECK(flx10_led_pad_base(0, pmode(96)) == 96, "SHIFT+BEAT JUMP -> page 96");
          reset(); ON(1, 34); OFF(1, 34);
          CHECK(flx10_led_pad_base(0, pmode(48)) == 48, "SAMPLER -> page 48");
          reset(); ON(2, 111); OFF(2, 111);
          CHECK(flx10_led_pad_base(1, pmode(112)) == 112 && flx10_led_pad_base(0, pmode(48)) == 48,
                "deck 2 KEY SHIFT -> 112, deck 1 untouched");
          reset(); ON(1, 27); OFF(1, 27);
          CHECK(flx10_led_pad_base(0, pmode(0)) == 0, "HOT CUE -> page 0");
          /* a second press of a main button moves the unit to its SHIFT page */
          reset(); ON(1, 27); OFF(1, 27);
          CHECK(flx10_led_pad_base(0, pmode(64)) == 64, "HOT CUE twice -> page 64");
          CHECK(led_kick > 0, "a page change asks for a repaint");
          reset(); ON(1, 27); OFF(1, 27);
          CHECK(flx10_led_pad_base(0, pmode(0)) == 0, "HOT CUE third press -> page 0");
          reset(); ON(1, 32); OFF(1, 32); ON(1, 32); OFF(1, 32);
          CHECK(flx10_led_pad_base(0, pmode(96)) == 96, "BEAT JUMP twice -> page 96");
          reset(); ON(1, 30); OFF(1, 30); ON(1, 30); OFF(1, 30);
          CHECK(flx10_led_pad_base(0, pmode(80)) == 80, "PAD FX1 twice -> page 80");
          reset(); ON(1, 34); OFF(1, 34); ON(1, 34); OFF(1, 34);
          CHECK(flx10_led_pad_base(0, pmode(112)) == 112, "SAMPLER twice -> page 112");
          reset(); ON(1, 107); OFF(1, 107); ON(1, 107); OFF(1, 107);
          CHECK(flx10_led_pad_base(0, pmode(80)) == 80, "PAD FX2 twice stays 80");
          /* held pads, for the dim -> bright rule */
          {
               extern int flx10_pad_touch(int, int, unsigned long long *);
               unsigned long long t = 0;
               CHECK(n && n->pad_style == LED_PAD_DIM_BRIGHT && n->pad_touch, "dim/bright pads");
               CHECK(n && n->pad_shift_mirror, "cue pages lit on the SHIFT layer too");
               reset(); ON(8, 32 + 2);
               CHECK(flx10_pad_touch(0, 2, &t) == 1 && t != 0, "pad 3 held, press time kept");
               CHECK(flx10_pad_touch(1, 2, &t) == 0, "deck 2 pad 3 not held");
               OFF(8, 32 + 2);
               CHECK(flx10_pad_touch(0, 2, &t) == 0 && t != 0, "released, press time stays");
          }
          /* a pad hit corrects the page */
          reset(); ON(8, 96 + 3); OFF(8, 96 + 3);
          CHECK(flx10_led_pad_base(0, pmode(96)) == 96, "pad on 99 -> page 96");
          reset(); ON(10, 16 + 7); OFF(10, 16 + 7);
          CHECK(flx10_led_pad_base(1, pmode(16)) == 16 && flx10_led_pad_base(0, pmode(96)) == 96,
                "deck 2 pad on 23 -> deck 2 page 16 only");
     }

     /* SHIFT + CUE: a seek straight to the start (the auto cue point); no keys,
      * nothing on the release, nothing repeats while held */
     {
          const struct led_notes *n = map_flx10.leds;
          struct deck_info di;
          memset(&di, 0, sizeof di);
          di.loaded = 1; di.elapsed_ms = 60000; di.total_ms = 200000;
          n->deck_display(1, &di);
          seek_n = 0; reset(); ON(2, 72);
          for (int i = 0; i < 8; i++) { ms(100); map_flx10.tick(); }
          OFF(2, 72);
          CHECK(seek_n == 1 && seek_deck == 1 && seek_pos == 22050 && sent_n == 0,
                "SHIFT+CUE -> one seek to the auto cue point, no keys, held 0.8 s (%d, %d)", seek_n, sent_n);
          /* at the start already: SHIFT+CUE seeks again (harmless) */
          di.elapsed_ms = 900; n->deck_display(1, &di);
          seek_n = 0; reset(); ON(2, 72); OFF(2, 72);
          CHECK(seek_n == 1 && sent_n == 0, "SHIFT+CUE at the start: just the seek, never a track");

          /* MIX POINT <: mid-track = the same seek; at the start = previous track */
          di.elapsed_ms = 60000; n->deck_display(1, &di);
          seek_n = 0; second_press[1] = 0; reset(); ON(2, 89); OFF(2, 89);
          CHECK(seek_n == 1 && sent_n == 0 && second_press[1] == 0, "MIX POINT < mid-track -> seek to the start");
          di.elapsed_ms = 1800; n->deck_display(1, &di);          /* 500 + 1500 zone */
          seek_n = 0; reset(); ON(2, 89);
          for (int i = 0; i < 8; i++) { ms(100); map_flx10.tick(); }
          OFF(2, 89);
          CHECK(seek_n == 0 && second_press[1] == 1 && sent_n == 2 && is(0, K_TRREV, OP_PRESS, 2) &&
                is(1, K_TRREV, OP_RELEASE, 2), "MIX POINT < at the start -> previous track, one 100 ms tap (%d)", sent_n);
          di.elapsed_ms = 60000; goes_back[1] = 1; n->deck_display(1, &di);
          seek_n = 0; reset(); ON(2, 89); ms(120); map_flx10.tick(); OFF(2, 89);
          CHECK(seek_n == 0 && sent_n == 2, "MIX POINT < where rbp says at the start -> previous track");
          goes_back[1] = 0;
     }

     /* MIX POINT >: next track, one 100 ms tap however long it is held */
     reset(); ON(1, 90);
     CHECK(sent_n == 1 && is(0, K_TRFWD, OP_PRESS, 1), "MIX POINT > -> TRACK SEARCH > pressed");
     for (int i = 0; i < 8; i++) { ms(100); map_flx10.tick(); }
     OFF(1, 90);
     CHECK(sent_n == 2 && is(1, K_TRFWD, OP_RELEASE, 1), "...released after 100 ms, held 0.8 s: no repeat (%d)", sent_n);

     /* crossfader assign switches: 22 A, 24 B, 29 THRU (0x7F); 0x00s ignored */
     reset(); ON(1, 22); OFF(1, 24); OFF(1, 29);
     CHECK(xf_assign[0] == 1 && sent_n == 0, "channel 1 assign A");
     OFF(2, 22); ON(2, 24); OFF(2, 29);
     CHECK(xf_assign[1] == 2, "channel 2 assign B");
     OFF(1, 22); OFF(1, 24); ON(1, 29);
     CHECK(xf_assign[0] == 0 && xf_assign[1] == 2, "channel 1 THRU, channel 2 untouched");

     /* the jog screens' HID packets (flx10_hid.c), against the rekordbox capture
      * (tools/captures/flx10-rekordbox-hid-notes.txt) */
     {
          unsigned char p[FLX10_HID_PKT];
          struct flx10_hid_deck hs;
          /* deck 2 empty, WIDE + MT, -9.12% (capture 18:26:35.987) */
          static const unsigned char cap_empty[64] = {
               0x20,0x21,0x00,0x08,0x44,0x01,0x02,0x01,0x80,0x10,0,0,0,0,0,0,
               0,0,0,0,0,0,0,0x70,0xfc,0,0,0x80,0,0,0,0,
               0,0,0,0,0,0,0,0,0,0,0,0xff,0xff,0,0,0,
               0,0,0,0,0,0,0,0,0,0,0,0,0x80,0x0d,0,0 };
          /* deck 2 loaded, cue standby at 0:00.358, 2:32.869 long, 139.05 BPM,
           * -9.12% WIDE MT, key 5B, cue 358 ms (capture 18:31:30.000) */
          static const unsigned char cap_loaded[64] = {
               0x20,0x21,0x00,0x08,0x44,0x01,0x02,0x01,0x80,0xb4,0x00,0x00,0x00,0x66,0x01,0x02,
               0x20,0x65,0x03,0x00,0x00,0x8b,0x00,0x70,0xfc,0x00,0x00,0x80,0x00,0x00,0x00,0x66,
               0x01,0,0,0,0,0,0,0,0,0,0,0xff,0xff,0x00,0,0,
               0,0x50,0,0,0,0,0,0x66,0x01,0x00,0x01,0x00,0x87,0x0d,0x00,0x00 };
          int same = 1, rest = 1;

          memset(&hs, 0, sizeof hs);
          hs.range = 3; hs.mt = 1; hs.tempo_x100 = -912;
          flx10_hid_state_pkt(p, 1, &hs);
          for (int i = 0; i < 64; i++) same &= p[i] == cap_empty[i];
          for (int i = 64; i < FLX10_HID_PKT; i++) rest &= p[i] == 0;
          CHECK(same && rest, "empty deck 2 = rekordbox's empty packet, byte for byte");

          hs.loaded = 1; hs.cue_standby = 1; hs.elapsed_ms = 358; hs.total_ms = 152869;
          hs.time_remain = 1;                /* rekordbox's default: remaining */
          hs.bpm_x100 = 13905; hs.key_code = 7; hs.cue_ms = 358;
          flx10_hid_state_pkt(p, 1, &hs);
          same = 1;
          for (int i = 0; i < 64; i++)
               if (i != 45 && p[i] != cap_loaded[i]) {
                    same = 0;
                    printf("  byte %d: %02x, capture %02x\n", i, p[i], cap_loaded[i]);
               }
          CHECK(same, "loaded deck 2 = rekordbox's packet (byte 45 aside)");
          hs.slip = 1; flx10_hid_state_pkt(p, 1, &hs);
          CHECK(p[2] == 0x01, "slip mode -> [2] |= 01 (SLIP on the screen)");
          hs.slip = 0;
          {
               struct flx10_hid_deck e;
               memset(&e, 0, sizeof e);
               e.slip = 1; flx10_hid_state_pkt(p, 1, &e);
               CHECK(p[2] == 0x01 && p[9] == 0x10, "SLIP on an empty deck too");
          }
          hs.end_caution = 1; flx10_hid_state_pkt(p, 1, &hs);
          CHECK(p[8] == 0x00, "track-end warning flash: the jog ring off ([8] 00)");
          hs.end_caution = 0;

          /* deck 1 at WIDE +33.78%, MT off: 220.15 BPM, key 8B shown 7B "+5" */
          memset(&hs, 0, sizeof hs);
          hs.loaded = 1; hs.playing = 1; hs.range = 3; hs.tempo_x100 = 3378;
          hs.bpm_x100 = 22015; hs.key_code = 1; hs.elapsed_ms = 75123; hs.total_ms = 186306;
          hs.cue_ms = 11554; hs.sync = 1;
          flx10_hid_state_pkt(p, 0, &hs);
          CHECK(p[0] == 0x10 && p[7] == 0x02 && p[2] == 0x00, "deck 1 playing: 10 .. 00 .. 02");
          CHECK(p[21] == 0xdc && p[22] == 0x10 && p[49] == 0x50, "220.15 BPM -> dc 10 / 50");
          CHECK(p[23] == 0x32 && p[24] == 0x0d, "tempo +33.78%% -> 32 0d");
          CHECK(p[60] == 0x8b && p[61] == 0x12, "8B at +33.78%%, MT off -> 7B, +5 (%02x %02x)", p[60], p[61]);
          CHECK(p[11] == 1 && p[12] == 15 && p[13] == 123 && p[14] == 0, "1:15.123 -> 01 0f 7b 00");
          CHECK(p[15] == 3 && p[16] == 6 && p[17] == 0x32 && p[18] == 0x01, "3:06.306 -> 03 06 32 01");
          CHECK(p[31] == 0x22 && p[32] == 0x2d && p[55] == 0x22 && p[56] == 0x2d, "cue 11554 ms -> 22 2d (twice)");
          CHECK(p[5] == 0x11, "SYNC on -> 11");
          hs.mt = 1; flx10_hid_state_pkt(p, 0, &hs);
          CHECK(p[60] == 0x81 && p[61] == 0x0d && p[4] == 0x44, "MT on -> the track's own 8B, no shift");
          hs.mt = 0; hs.tempo_x100 = 338; hs.range = 1; flx10_hid_state_pkt(p, 0, &hs);
          CHECK(p[60] == 0x83 && p[61] == 0x0e && p[4] == 0x20, "+3.38%% -> 3B, +1 (capture)");
          hs.tempo_x100 = 205; flx10_hid_state_pkt(p, 0, &hs);
          CHECK(p[60] == 0x81 && p[61] == 0x0d, "+2.05%% -> 8B, no shift (capture)");
          hs.key_semis = -2; flx10_hid_state_pkt(p, 0, &hs);
          CHECK(p[60] == 0x95 && p[61] == 0x0b, "KEY SYNC -2 on 8B -> 6B, -2 (%02x %02x)", p[60], p[61]);
          hs.mt = 0; hs.tempo_x100 = 338; hs.key_semis = 2; flx10_hid_state_pkt(p, 0, &hs);
          CHECK(p[61] == 0x0d + 3, "tempo +1 and KEY SYNC +2 -> +3 beside the key (%02x)", p[61]);
          hs.key_semis = 0; hs.tempo_x100 = 205;
          hs.playing = 0; hs.cue_standby = 0; hs.bpm_x100 = 38056; flx10_hid_state_pkt(p, 0, &hs);
          CHECK(p[2] == 0x02 && p[21] == 0x7c && p[22] == 0x51 && p[49] == 0x60,
                "paused off the cue -> 02; 380.56 BPM -> 7c 51 / 60");
          CHECK(flx10_key_shift(-912) == -2 && flx10_key_shifted(7, -2) == 3, "-9.12%% -> -2 semitones; 5B -2 = 3B");
          hs.key_code = 0; flx10_hid_state_pkt(p, 0, &hs);
          CHECK(p[60] == 0x80 && p[61] == 0x0d, "no key -> 80 0d");
          hs.cue_ms = -1; flx10_hid_state_pkt(p, 0, &hs);
          CHECK(p[31] == 0 && p[55] == 0 && p[9] == 0xb0, "no cue -> zeros, cue marker off ([9] b0)");
          hs.cue_ms = 358;
          /* the loop: in / out ms at [25..30] as captured (12648 -> 14107),
           * looping -> [9] bc, [10] 0e; none -> [27] 80 */
          hs.loop_in_ms = 12648; hs.loop_out_ms = 14107; hs.looping = 1;
          hs.bpm_x100 = 16456; hs.tempo_x100 = 0;      /* capture 1's 164.56 BPM */
          flx10_hid_state_pkt(p, 0, &hs);
          CHECK(p[25] == 0x68 && p[26] == 0x31 && p[27] == 0x00 && p[28] == 0x1b && p[29] == 0x37 &&
                p[30] == 0x00 && p[9] == 0xbc && p[10] == 0x0e, "4-beat loop 12648..14107 -> captured bytes");
          hs.looping = 0; flx10_hid_state_pkt(p, 0, &hs);
          CHECK(p[25] == 0x68 && p[27] == 0x80 && p[9] == 0xbc && p[10] == 0,
                "loop exited: kept, [27] 80, [10] 00, [9] stays bc (capture 2)");
          /* loop sizes: capture 2's doubling / halving */
          CHECK(flx10_loop_code(1459, 16456, 0) == 0x0e && flx10_loop_code(2917, 16456, 0) == 0x0f &&
                flx10_loop_code(1459 * 128, 16456, 0) == 0x15 && flx10_loop_code(365, 16456, 0) == 0x0c &&
                flx10_loop_code(182, 16456, 0) == 0x0a && flx10_loop_code(91, 16456, 0) == 0x08 &&
                flx10_loop_code(46, 16456, 0) == 0x07 && flx10_loop_code(6, 16456, 0) == 0x04,
                "loop size codes 512 .. 1/64");
          CHECK(flx10_loop_code(1000, 16456, 0) == 0x01, "2.74 beats -> 01 (the screen's --)");
          CHECK(flx10_loop_code_beats(512, 1) == 0x15 && flx10_loop_code_beats(1, 128) == 0x03 &&
                flx10_loop_code_beats(1, 64) == 0x04 && flx10_loop_code_beats(4, 1) == 0x0e &&
                flx10_loop_code_beats(1, 2) == 0x0a && flx10_loop_code_beats(2, 4) == 0x0a,
                "rbp's beat counts -> codes");
          CHECK(flx10_loop_code_beats(3, 4) == 0x01 && flx10_loop_code_beats(0, 0) == 0x01 &&
                flx10_loop_code_beats(1024, 1) == 0x01, "not a power of two / unknown -> 01");
          {
               struct flx10_hid_deck c = hs;
               c.loaded = 1; c.bpm_x100 = 16456; c.tempo_x100 = 0; c.looping = 1;
               c.loop_in_ms = 100000; c.loop_out_ms = 101000;    /* clipped at the end */
               c.loop_num = 512; c.loop_den = 1;
               flx10_hid_state_pkt(p, 0, &c);
               CHECK(p[10] == 0x15, "512 clipped at the track's end -> 15, not -- (%02x)", p[10]);
               c.loop_in_ms = 100000; c.loop_out_ms = 101459; c.loop_num = 8;
               flx10_hid_state_pkt(p, 0, &c);
               CHECK(p[10] == 0x0e, "the length wins when it resolves (%02x)", p[10]);
               c.on_air = 1; c.time_remain = 1; flx10_hid_state_pkt(p, 0, &c);
               CHECK(p[3] == 0x0a, "ON AIR -> [3] 02, the white edges");
               flx10_hid_set_onair(3, 0x04); flx10_hid_state_pkt(p, 0, &c);
               CHECK(p[3] == 0x0c, "FLX10_ONAIR=3:04 -> [3] |= 04 on air");
               c.on_air = 0; flx10_hid_state_pkt(p, 0, &c);
               CHECK(p[3] == 0x08, "off air -> not set");
               flx10_hid_set_onair(-1, 0);
          }
          CHECK(flx10_loop_code(1459, 16456 * 11 / 10, 1000) == 0x0e, "track-time loop at +10%% still 4 beats");
          hs.master = 1; hs.sync = 1; hs.jog_touch = 1; hs.time_remain = 1; flx10_hid_state_pkt(p, 0, &hs);
          CHECK(p[5] == 0x91 && p[3] == 0x18, "MASTER + SYNC -> 91, jog touched -> 18 (capture 2)");
          hs.jog_touch = 0; hs.time_remain = 1; flx10_hid_state_pkt(p, 0, &hs);
          CHECK(p[3] == 0x08, "rbp's time mode remaining -> [3] 08");
          hs.time_remain = 0; flx10_hid_state_pkt(p, 0, &hs);
          CHECK(p[3] == 0x00, "rbp's time mode elapsed -> [3] 00");
          hs.master = 0; hs.sync = 0; hs.jog_touch = 0; hs.time_remain = 0;
          hs.loop_in_ms = hs.loop_out_ms = -1; flx10_hid_state_pkt(p, 0, &hs);
          CHECK(p[25] == 0 && p[27] == 0x80 && p[28] == 0, "no loop -> 00 00 80");
          /* position between rbp's readings */
          CHECK(flx10_hid_extrapolate(1000, 50, 1, 0, 0) == 1050, "50 ms at 0%% -> +50");
          CHECK(flx10_hid_extrapolate(1000, 50, 1, 1000, 0) == 1055, "50 ms at +10%% -> +55");
          CHECK(flx10_hid_extrapolate(1000, 50, 0, 0, 0) == 1000, "paused -> no movement");
          CHECK(flx10_hid_extrapolate(1000, 5000, 1, 0, 0) == 1200, "capped 200 ms ahead");
          CHECK(flx10_hid_extrapolate(9990, 50, 1, 0, 10000) == 10000, "capped at the track's end");
          /* the arm, 100 Hz on fresh readings */
          {
               struct flx10_hid_track k;
               unsigned long long t = 1000;
               flx10_hid_track_init(&k);
               k.fresh = 1;
               CHECK(flx10_hid_merge(&k, 5000, 1, 0, 0, 0, t) == 5000, "first reading shown");
               CHECK(flx10_hid_merge(&k, 5000, 1, 0, 0, 0, t += 10) == 5010, "same block: runs on at the tempo");
               CHECK(flx10_hid_merge(&k, 5000, 1, 0, 0, 0, t += 10) == 5020, "still running on");
               CHECK(flx10_hid_merge(&k, 5023, 1, 0, 0, 0, t += 10) == 5030, "next block behind: no step back");
               CHECK(flx10_hid_merge(&k, 5023, 0, 0, 0, 0, t += 10) == 5030, "paused: holds the last shown, no jump back");
               CHECK(flx10_hid_merge(&k, 5023, 0, 0, 0, 0, t += 10) == 5030, "and keeps it while rbp stays put");
               CHECK(flx10_hid_merge(&k, 4800, 0, FLX10_JOG_HELD, 0, 0, t += 10) == 4800, "jogged back: the reading");
               {
                    int a1 = flx10_hid_merge(&k, 4820, 0, FLX10_JOG_HELD, 0, 0, t += 10);
                    int a2 = flx10_hid_merge(&k, 4785, 0, FLX10_JOG_HELD, 0, 0, t += 10);
                    CHECK(a1 >= 4800 && a1 <= 4803 && a2 >= 4798 && a2 <= 4803,
                          "held still: the reading's wobble barely moves the arm (%d %d)", a1, a2);
               }
               CHECK(flx10_hid_merge(&k, 4700, 0, FLX10_JOG_HELD, 0, 0, t += 10) == 4700, "held, moved: follows");
               flx10_hid_merge(&k, 4690, 1, FLX10_JOG_TURNING_BACK, 0, 0, t += 10);
               {
                    int v = flx10_hid_merge(&k, 4680, 1, FLX10_JOG_TURNING_BACK, 0, 0, t += 10);
                    CHECK(v < 4680 && v >= 4680 - 30, "turning back: drawn ahead of the reading, the way it moves (%d)", v);
                    v = flx10_hid_merge(&k, 4680, 1, FLX10_JOG_TURNING_BACK, 0, 0, t += 10);
                    CHECK(v < 4680, "turning: carried on between rbp's blocks (%d)", v);
                    /* a slight move: rbp's reading wobbles against the hand */
                    int w = flx10_hid_merge(&k, 4690, 1, FLX10_JOG_TURNING_BACK, 0, 0, t += 10);
                    CHECK(w <= v, "a wobble against the hand does not move the arm back (%d after %d)", w, v);
                    flx10_hid_merge(&k, 4690, 1, FLX10_JOG_HELD, 0, 0, t += 100);   /* speed settles */
                    flx10_hid_merge(&k, 4690, 1, FLX10_JOG_HELD, 0, 0, t += 100);
                    int x = flx10_hid_merge(&k, 4692, 1, FLX10_JOG_TURNING, 0, 0, t += 30);
                    int y = flx10_hid_merge(&k, 4692, 1, FLX10_JOG_TURNING, 0, 0, t += 10);
                    CHECK(x == y && y >= 4690 && y <= 4692, "a slow nudge: no lead, no twitch (%d %d)", x, y);
               }
               /* a fast turn, then the hand stops: no lead once counts stop */
               {
                    int r = 6000, v = 0;
                    flx10_hid_merge(&k, r, 0, FLX10_JOG_HELD, 0, 0, t += 10);
                    for (int i = 0; i < 6; i++)
                         v = flx10_hid_merge(&k, r += 20, 1, FLX10_JOG_TURNING, 0, 0, t += 10);
                    CHECK(v > r, "fast turn: drawn ahead (%d > %d)", v, r);
                    k.fresh = 0;
                    v = flx10_hid_merge(&k, r, 1, FLX10_JOG_TURNING, 0, 0, t += 10);
                    CHECK(v - r <= 30, "hand stopped: lead dropped, at most a small hold (%d vs %d)", v, r);
                    k.fresh = 1;
               }
               /* playing, the jog turned back (a bend down): the track still
                * runs forward, slower, and the arm follows it smoothly */
               {
                    int r = 7000, v, prev = 0, back = 0, jump = 0;
                    k.fresh = 1;
                    for (int i = 0; i < 10; i++)          /* playing at 1x first */
                         prev = flx10_hid_merge(&k, r += 10, 1, 0, 0, 0, t += 10);
                    for (int i = 0; i < 40; i++) {
                         r += 6;                         /* 0.6x speed */
                         v = flx10_hid_merge(&k, r, 1, FLX10_JOG_TURNING_BACK, 0, 0, t += 10);
                         back += v < prev;
                         jump += v - prev > 25;
                         prev = v;
                    }
                    CHECK(!back && !jump && prev >= r - 10 && prev <= r + 20,
                          "bend down while playing: arm runs forward smoothly (back %d jumps %d, %d vs %d)",
                          back, jump, prev, r);
               }
               /* playing, then in reverse (mov -1): the arm turns round and
                * runs back smoothly, the reading moving in blocks */
               {
                    struct flx10_hid_track k;          /* a deck of its own */
                    int r = 8000, v = 0, prev, fwd = 0, jump = 0;
                    flx10_hid_track_init(&k);
                    k.fresh = 1;
                    for (int i = 0; i < 12; i++) {
                         if (i % 3 == 0) r += 30;
                         prev = flx10_hid_merge(&k, r, 1, 0, 0, 0, t += 10);
                    }
                    for (int i = 0; i < 45; i++) {
                         if (i % 3 == 0) r -= 30;
                         v = flx10_hid_merge(&k, r, -1, 0, 0, 0, t += 10);
                         if (i >= 6) {               /* past the turn-round */
                              fwd += v > prev;
                              jump += prev - v > 25;
                         }
                         prev = v;
                    }
                    CHECK(!fwd && !jump && prev <= r + 5 && prev >= r - 40,
                          "reverse: arm runs back without jerks (fwd %d jumps %d, %d vs %d)",
                          fwd, jump, prev, r);
                    v = flx10_hid_merge(&k, r, 0, 0, 0, 0, t += 10);
                    CHECK(v <= r + 1 && v >= r - 40, "reverse then pause: no step forward (%d vs %d)", v, r);
               }
               /* the reverse flag set while the reading still runs forward
                * (quantized reverse): the arm keeps going forward smoothly */
               {
                    struct flx10_hid_track k;
                    int r = 9000, v, prev = 0, back = 0;
                    flx10_hid_track_init(&k);
                    k.fresh = 1;
                    for (int i = 0; i < 12; i++) {
                         if (i % 3 == 0) r += 30;
                         prev = flx10_hid_merge(&k, r, 1, 0, 0, 0, t += 10);
                    }
                    for (int i = 0; i < 30; i++) {
                         if (i % 3 == 0) r += 30;
                         v = flx10_hid_merge(&k, r, -1, 0, 0, 0, t += 10);
                         back += v < prev;
                         prev = v;
                    }
                    CHECK(!back, "reverse flag ahead of the reading: no backward jitter (%d)", back);
               }
               CHECK(flx10_hid_merge(&k, 9000, 0, 0, 0, 0, t += 10) == 9000, "cue/seek while paused: the reading");
               flx10_hid_merge(&k, 9000, 1, 0, 0, 0, t += 10);
               flx10_hid_merge(&k, 9000, 1, 0, 0, 0, t += 10);
               CHECK(flx10_hid_merge(&k, 2000, 0, 0, 0, 0, t += 10) == 2000, "CUE back while playing: the cue point");
          }
          {
               struct flx10_hid_deck c = hs;
               c.loaded = 1; c.total_ms = 200000; c.elapsed_ms = 200040;
               flx10_hid_state_pkt(p, 0, &c);
               CHECK(p[11] == 3 && p[12] == 19 && (p[13] | p[14] << 8) == 999,
                     "past the end: shown 1 ms short of it (arms stay)");
          }
          /* sync + jog: tempo and key held */
          {
               struct flx10_hid_tempo_hold th;
               struct flx10_hid_deck c;
               memset(&th, 0, sizeof th); memset(&c, 0, sizeof c);
               c.loaded = 1; c.sync = 1; c.tempo_x100 = 250; c.bpm_x100 = 12500;
               flx10_hid_tempo_hold(&th, &c, 0, 1000);
               CHECK(c.tempo_x100 == 250, "not jogging: rbp's tempo");
               c.tempo_x100 = 410; c.bpm_x100 = 12700; flx10_hid_tempo_hold(&th, &c, 1, 1010);
               CHECK(c.tempo_x100 == 250 && c.bpm_x100 == 12500, "jog on a synced deck: tempo/BPM held");
               c.tempo_x100 = 300; flx10_hid_tempo_hold(&th, &c, 0, 1500);
               CHECK(c.tempo_x100 == 250, "after the jog, still catching up: held");
               c.tempo_x100 = 252; flx10_hid_tempo_hold(&th, &c, 0, 1600);
               CHECK(c.tempo_x100 == 252 && !th.on, "back within 0.05%%: released");
               c.tempo_x100 = 410; flx10_hid_tempo_hold(&th, &c, 1, 1700);
               c.tempo_x100 = 300; flx10_hid_tempo_hold(&th, &c, 0, 4800);
               CHECK(c.tempo_x100 == 300, "given up 3 s after the last turn");
               c.sync = 0; c.tempo_x100 = 410; flx10_hid_tempo_hold(&th, &c, 1, 5000);
               CHECK(c.tempo_x100 == 410, "not synced: nothing held");
          }
          flx10_hid_heartbeat_pkt(p, 3);
          CHECK(p[0] == 0 && p[1] == 0x3D && p[2] == 3 && p[4] == 5, "heartbeat 00 3D n 00 05");
     }

     /* the connect sequence: 9F 70 00 first, the rest 300 ms later, with the
      * unit's own reported settings in place of the captured values */
     {
          const struct led_notes *n = map_flx10.leds;
          struct deck_info di;

          memset(&di, 0, sizeof di);
          ev(SNDRV_SEQ_EVENT_NOTEON, 15, 0x3C, 0x7F);     /* unit: 9F 3C 7F (capture: 00) */
          ev(SNDRV_SEQ_EVENT_CONTROLLER, 15, 0x41, 0x05); /* unit: BF 41 05 (capture: 0A) */
          n->deck_display(0, &di);
          CHECK(note70_n == 0, "nothing in the first second after a connection");
          ms(1050); n->deck_display(0, &di);
          CHECK(note70_n == 1 && jd_note_v[0x40] < 0, "9F 70 00 alone first");
          ms(350); n->deck_display(0, &di);
          CHECK(jd_note_v[0x3C] == 0x7F, "9F 3C: the unit's 7F, not the capture's 00 (%d)", jd_note_v[0x3C]);
          CHECK(jd_cc_v[0x41] == 0x05, "BF 41: the unit's 05 (%d)", jd_cc_v[0x41]);
          CHECK(jd_note_v[0x3D] == 0x00 && jd_note_v[0x40] == 0x7F && jd_cc_v[0x46] == 0x30,
                "unreported ones: the captured values");
          CHECK(jd_note_v[0x47] < 0 && jd_note_v[0x49] < 0 && jd_cc_v[0x42] < 0 && jd_note_v[0x45] < 0,
                "the unit's audio settings, unreported: not sent (%d %d %d %d)", jd_note_v[0x47],
                jd_note_v[0x49], jd_cc_v[0x42], jd_note_v[0x45]);
          CHECK(note70_n == 1, "9F 70 not sent twice");
          CHECK(dk_note_v[0][101] == 0 && dk_note_v[1][101] == 0, "KEY SYNC LED sent dark on connect");
     }
     /* ...and every pad page dark but the one the bridge is on */
     for (int d = 0; d < 2; d++) {
          int dark = 0, first_left = -1, left = 0;
          for (int k = 0; k < 128; k++) {
               if (pad_note_v[d][k] == 0)
                    dark++;
               else if (pad_note_v[d][k] < 0) {
                    if (first_left < 0)
                         first_left = k;
                    left++;
               }
          }
          CHECK(dark == 120 && left == 8 && first_left % 16 == 0,
                "deck %d: 120 pad notes sent dark, the current page's 8 left (%d / %d at %d)",
                d + 1, dark, left, first_left);
     }

     /* BEAT SYNC fits the tempo range first (after the connect test: deck_display
      * starts the connect timing) */
     {
          const struct led_notes *n = map_flx10.leds;
          struct deck_info d0, d1;
          int ranges = 0;
          memset(&d0, 0, sizeof d0); memset(&d1, 0, sizeof d1);
          d0.loaded = 1; d0.master = 1; d0.bpm_raw = 25000;
          d1.loaded = 1; d1.bpm_raw = 10500; d1.tempo_raw = 500; d1.tempo_range = 1;
          n->deck_display(0, &d0); n->deck_display(1, &d1);
          reset(); ON(2, 88); OFF(2, 88);
          for (int i = 0; i < sent_n; i++)
               ranges += sent[i].key == K_TEMPO_RANGE && sent[i].op == OP_PRESS;
          CHECK(ranges == 2 && is(sent_n - 2, K_SYNC, OP_PRESS, 2),
                "BEAT SYNC 100 -> 250 BPM: range +-10 -> WIDE (2 steps), then SYNC (%d)", ranges);
          d1.bpm_raw = 24500; d1.tempo_raw = 0; d1.tempo_range = 3;    /* 2.04%% away */
          n->deck_display(1, &d1);
          reset(); ON(2, 88); OFF(2, 88);
          CHECK(sent_n == 2 && is(0, K_SYNC, OP_PRESS, 2),
                "2%% away on WIDE: kept (only ever widens), just SYNC");
          d1.tempo_range = 0; d1.bpm_raw = 22000;                       /* 13.6%% away */
          n->deck_display(1, &d1);
          reset(); ON(2, 88); OFF(2, 88);
          CHECK(sent_n == 6 && is(0, K_TEMPO_RANGE, OP_PRESS, 2) && is(2, K_TEMPO_RANGE, OP_PRESS, 2) &&
                is(4, K_SYNC, OP_PRESS, 2), "13.6%% on +-6: two steps to +-16, then SYNC");
          d1.sync = 1; n->deck_display(1, &d1);
          reset(); ON(2, 88); OFF(2, 88);
          CHECK(sent_n == 2 && is(0, K_SYNC, OP_PRESS, 2), "turning sync off: no range change");
          d1.sync = 0; d0.master = 0; n->deck_display(0, &d0); n->deck_display(1, &d1);
          reset(); ON(2, 88); OFF(2, 88);
          CHECK(sent_n == 2, "no master: no range change");
          d0.playing = 1; n->deck_display(0, &d0);     /* a copy of a playing deck */
          reset(); ON(2, 120);
          CHECK(sent_n == 1 && is(0, K_SYNC, OP_PRESS, 2),
                "SYNC held (the FLX10's note 120): rbp's SYNC down");
          ms(500); map_flx10.tick(); OFF(2, 120); ON(2, 120);
          CHECK(sent_n == 1, "...held, whatever else 120 says meanwhile");
          ms(700); map_flx10.tick();
          CHECK(sent_n == 2 && is(1, K_SYNC, OP_RELEASE, 2),
                "...and up after 1.1 s, past rbp's 1000 ms long press: INST.DOUBLES");
          ms(400); OFF(2, 88);
          CHECK(sent_n == 2, "the FLX10's lone SYNC release on letting go of the hold: dropped (%d)", sent_n);
          ms(500); ON(2, 88); OFF(2, 88);
          CHECK(sent_n == 4 && is(2, K_SYNC, OP_PRESS, 2), "the next tap: SYNC (%d)", sent_n);
          memset(&d0, 0, sizeof d0); memset(&d1, 0, sizeof d1);
          n->deck_display(0, &d0); n->deck_display(1, &d1);
     }
     media_tests();
     /* KEY SYNC: the CDJ-3000's rule -- the least shift to a key compatible with the
      * master's (its Camelot number or one either side, either letter); lit while
      * compatible; a press undoes only a shift of ours; new tracks counted once settled */
     {
          extern int flx10_key_root(const char *);
          extern int flx10_tempo_semis(int, int);
          extern int flx10_keysync_shift(int, int, int);
          const struct led_notes *n = map_flx10.leds;
          struct deck_info a, b;

          CHECK(flx10_key_root("8A") == 0 && flx10_key_root("8B") == 0 &&
                flx10_key_root("Am") == 0 && flx10_key_root("10A") == 2 &&
                flx10_key_root("") == -1, "roots: 8A, 8B, Am on C; 10A on D");
          CHECK(flx10_tempo_semis(5946, 0) == 8 && flx10_tempo_semis(5946, 1) == 0 &&
                flx10_tempo_semis(300, 0) == 1 && flx10_tempo_semis(-200, 0) == 0,
                "tempo semitones: +59.46%% is 8 without MASTER TEMPO, none with it");
          CHECK(flx10_keysync_shift(2, 0, 0) == -2, "10A onto 8A: -2 (not compatible: two numbers)");
          CHECK(flx10_keysync_shift(flx10_key_root("11B"), 0, flx10_key_root("10B")) == 0 &&
                flx10_keysync_shift(flx10_key_root("9A"), 0, flx10_key_root("10A")) == 0 &&
                flx10_keysync_shift(flx10_key_root("10A"), 0, flx10_key_root("10B")) == 0,
                "one number apart, or relative: already compatible, no shift");
          CHECK(flx10_keysync_shift(flx10_key_root("4B"), 0, flx10_key_root("9A")) == -1,
                "4B onto 9A: -1 to the same key");
          {
               int worst = 0;
               for (int r = 0; r < 12; r++)
                    for (int m = 0; m < 12; m++) {
                         int k = flx10_keysync_shift(r, 0, m);
                         if (k < 0) k = -k;
                         if (k > worst) worst = k;
                    }
               CHECK(worst == 2, "no key ever needs more than 2 semitones (%d)", worst);
          }

          memset(&a, 0, sizeof a);
          a.loaded = 1; a.master = 1; a.mt = 1; a.total_ms = 200000;
          strcpy(a.key, "8A");
          b = a;
          b.master = 0; b.total_ms = 180000;
          strcpy(b.key, "10A");
          n->deck_display(0, &a);
          n->deck_display(1, &b);
          ks_shift[0] = ks_shift[1] = 0; ks_reloads = 0;
          map_flx10.tick(); ms(450); map_flx10.tick();
          CHECK(dk_note_v[1][101] == 0, "10A against 8A: not compatible, dark");
          reset(); ON(2, 101); OFF(2, 101); map_flx10.tick();
          CHECK(ks_shift[1] == -2 && dk_note_v[1][101] == 0x7F,
                "KEY SYNC on deck 2: 10A -2 onto deck 1's 8A, LED lit (%d)", ks_shift[1]);
          strcpy(a.key, "12A");                               /* E's relative: root 4 */
          n->deck_display(0, &a); map_flx10.tick();
          CHECK(ks_shift[1] == 2, "the master moves to 12A: deck 2 follows, +2 onto 12A itself (%d)",
                ks_shift[1]);
          ON(2, 101); OFF(2, 101); map_flx10.tick();
          CHECK(ks_shift[1] == 0 && dk_note_v[1][101] == 0, "a press undoes our shift: dark");
          ON(1, 101); OFF(1, 101); map_flx10.tick();
          CHECK(ks_shift[0] == 0, "KEY SYNC on the master deck itself: nothing");

          /* a load already compatible lights it once, after it settles, with no shift */
          strcpy(b.key, "11A"); b.total_ms = 150000;          /* one from 12A */
          n->deck_display(1, &b); map_flx10.tick();
          CHECK(dk_note_v[1][101] == 0x7F, "11A against 12A: compatible, lit");
          strcpy(b.key, ""); n->deck_display(1, &b); map_flx10.tick();
          strcpy(b.key, "11A"); n->deck_display(1, &b); map_flx10.tick();
          ms(450); map_flx10.tick();
          CHECK(dk_note_v[1][101] == 0x7F && ks_shift[1] == 0, "...and stays lit through the load");
          ON(2, 101); OFF(2, 101); map_flx10.tick();
          CHECK(dk_note_v[1][101] == 0x7F && ks_shift[1] == 0,
                "a press with nothing of ours to undo: still lit, nothing shifted");
          strcpy(a.key, "2A");                                /* far from 11A */
          n->deck_display(0, &a); map_flx10.tick();
          CHECK(dk_note_v[1][101] == 0x7F && ks_shift[1] != 0,
                "latched, it follows the master (%d)", ks_shift[1]);
          strcpy(b.key, "5B"); b.total_ms = 160000;
          n->deck_display(1, &b); map_flx10.tick(); ms(450); map_flx10.tick();
          CHECK(ks_shift[1] == 0 && ks_reloads >= 1, "a new track lets it go and resets the shift");
          a.master = 0;
          n->deck_display(0, &a); map_flx10.tick();
          ON(2, 101); OFF(2, 101); map_flx10.tick();
          CHECK(dk_note_v[1][101] == 0, "no master: nothing to match, dark");
     }

     /* MASTER follows the faders */
     {
          struct deck_info a, b;
          void (*dd)(int, const struct deck_info *) = map_flx10.leds->deck_display;

          memset(&a, 0, sizeof a);
          a.loaded = a.playing = a.on_air = 1; a.total_ms = 200000;
          b = a; b.master = 1; b.total_ms = 180000;
          dd(0, &a); dd(1, &b); reset();
          map_flx10.tick(); ms(1500); map_flx10.tick();
          CHECK(sent_n == 0, "both on air: the master stays");
          a.playing = 0; b.on_air = 0; dd(0, &a); dd(1, &b);
          map_flx10.tick(); ms(1500); map_flx10.tick();
          CHECK(sent_n == 0, "the other deck not playing: the master stays");
          b.on_air = 1; dd(1, &b); map_flx10.tick(); ms(1000);
          a.playing = 1; b.on_air = 0; dd(0, &a); dd(1, &b); map_flx10.tick();
          CHECK(sent_n == 2 && is(0, K_MASTER, OP_PRESS, 1) && is(1, K_MASTER, OP_RELEASE, 1),
                "deck 2 off air, deck 1 playing on air: MASTER on deck 1 at once (%d)", sent_n);
          ms(100); map_flx10.tick();
          CHECK(sent_n == 2, "...once, while rbp takes it");
          a.master = 1; b.master = 0; dd(0, &a); dd(1, &b); map_flx10.tick();
          ms(250); b.on_air = 1; dd(1, &b); map_flx10.tick();
          CHECK(sent_n == 4 && is(2, K_MASTER, OP_PRESS, 2),
                "deck 2 back on air 350 ms later: a cut, MASTER back on deck 2 (%d)", sent_n);
          a.master = 0; b.master = 1; dd(0, &a); dd(1, &b); ms(1000); map_flx10.tick();
          CHECK(sent_n == 4, "deck 2 master again: nothing more");
          b.on_air = 0; dd(1, &b); map_flx10.tick();
          CHECK(sent_n == 6 && is(4, K_MASTER, OP_PRESS, 1), "off air again: MASTER on deck 1");
          a.master = 1; b.master = 0; dd(0, &a); dd(1, &b); ms(100); map_flx10.tick();
          ms(600); map_flx10.tick(); b.on_air = 1; dd(1, &b); map_flx10.tick();
          CHECK(sent_n == 6, "...and back on air 700 ms later is a mix: deck 1 keeps it");

          /* rbp's own move, deck 1 to deck 2 at once: undone after a cut */
          ms(1000); map_flx10.tick();
          a.on_air = 0; a.master = 0; b.master = 1; dd(0, &a); dd(1, &b); map_flx10.tick();
          ms(400); a.on_air = 1; dd(0, &a); map_flx10.tick();
          CHECK(sent_n == 8 && is(6, K_MASTER, OP_PRESS, 1),
                "rbp moved it on a 400 ms cut: MASTER back on deck 1 (%d)", sent_n);
          a.master = 1; b.master = 0; dd(0, &a); dd(1, &b); ms(1000); map_flx10.tick();
          a.on_air = 0; a.master = 0; b.master = 1; dd(0, &a); dd(1, &b); map_flx10.tick();
          ms(600); map_flx10.tick(); a.on_air = 1; dd(0, &a); map_flx10.tick();
          CHECK(sent_n == 8, "...but off air 600 ms is a mix: deck 2 keeps it");
     }

     /* a copy of a paused deck: paused again when rbp starts it */
     {
          struct deck_info p0, p1;
          memset(&p0, 0, sizeof p0); memset(&p1, 0, sizeof p1);
          p0.loaded = p1.loaded = 1;
          map_flx10.leds->deck_display(0, &p0); map_flx10.leds->deck_display(1, &p1);
          browse_mode = 1;                      /* the player */
          reset(); ON(7, 71); OFF(7, 71); ms(60); map_flx10.tick();
          ms(200); ON(7, 71); OFF(7, 71); ms(500); map_flx10.tick();
          CHECK(!ks_muted[1], "INST.DOUBLES of a paused deck: deck 2 heard until rbp's long press nears");
          ms(500); map_flx10.tick();
          CHECK(ks_muted[1], "...then muted, before rbp loads and starts the copy");
          ms(200); map_flx10.tick();
          CHECK(sent_n == 2 && is(1, K_SYNC, OP_RELEASE, 2), "doubles of a paused deck 1 (%d)", sent_n);
          ms(300); p1.playing = 1; map_flx10.leds->deck_display(1, &p1); map_flx10.tick();
          CHECK(sent_n == 4 && is(2, K_PLAY, OP_PRESS, 2), "the copy started: PLAY, paused (%d)", sent_n);
          place_n = 0; ms(100); map_flx10.tick();
          CHECK(sent_n == 4 && place_n == 0, "...once, and not placed while still playing");
          p1.playing = 0; map_flx10.leds->deck_display(1, &p1); map_flx10.tick();
          CHECK(place_n == 1 && place_deck == 1, "stopped: put where deck 1 stands (%d)", place_n);
          CHECK(!ks_muted[1], "...and heard again, paused and placed");
          ms(500); map_flx10.tick();
          CHECK(sent_n == 4 && place_n == 1, "...once");
          p0.playing = 1; p1.playing = 0;
          map_flx10.leds->deck_display(0, &p0); map_flx10.leds->deck_display(1, &p1);
          ms(500); reset(); ON(7, 71); OFF(7, 71); ms(60); map_flx10.tick();
          ms(200); ON(7, 71); OFF(7, 71); ms(1200); map_flx10.tick();
          ms(300); p1.playing = 1; map_flx10.leds->deck_display(1, &p1); map_flx10.tick();
          CHECK(sent_n == 2 && place_n == 1, "doubles of a playing deck: the copy plays (%d)", sent_n);
          CHECK(!ks_muted[1], "...and is never muted");
          memset(&p0, 0, sizeof p0); memset(&p1, 0, sizeof p1);
          map_flx10.leds->deck_display(0, &p0); map_flx10.leds->deck_display(1, &p1);
          ms(500);
     }

     /* a synced deck's tempo range follows the master, wider and narrower, never
      * below where it started or where the operator set it */
     {
          struct deck_info m, f;
          memset(&m, 0, sizeof m); memset(&f, 0, sizeof f);
          m.loaded = f.loaded = 1; m.master = 1;
          m.bpm_raw = 10500; f.bpm_raw = 10500; f.bpm_orig = 10000; f.tempo_range = 1; f.sync = 1;
          f.tempo_raw = -68;                 /* synced: rbp's tempo %% is the fader's, not the speed */
          map_flx10.leds->deck_display(0, &m); map_flx10.leds->deck_display(1, &f);
          reset(); ms(600); map_flx10.tick();
          CHECK(sent_n == 0, "the master within deck 2's +-10%%: nothing");
          m.bpm_raw = 12000; f.bpm_raw = 12000;                       /* +20%% on a 100 BPM track */
          map_flx10.leds->deck_display(0, &m); map_flx10.leds->deck_display(1, &f);
          map_flx10.tick();
          CHECK(sent_n == 4 && is(0, K_TEMPO_RANGE, OP_PRESS, 2) && is(2, K_TEMPO_RANGE, OP_PRESS, 2),
                "master at 120 on a 100 BPM track: +-10 -> WIDE, two steps (%d)", sent_n);
          ms(100); map_flx10.tick();
          CHECK(sent_n == 4, "...once, while rbp's range comes back");
          f.tempo_range = 3; map_flx10.leds->deck_display(1, &f); ms(100); map_flx10.tick();
          CHECK(sent_n == 4, "WIDE arrived: nothing");
          m.bpm_raw = 10300; f.bpm_raw = 10300;                       /* back to +3%% */
          map_flx10.leds->deck_display(0, &m); map_flx10.leds->deck_display(1, &f);
          map_flx10.tick();
          CHECK(sent_n == 8 && is(4, K_TEMPO_RANGE, OP_PRESS, 2) && is(6, K_TEMPO_RANGE, OP_PRESS, 2),
                "the master back down to +3%%: WIDE -> +-10, the start, not +-6 (two steps round, %d)", sent_n);
          f.tempo_range = 1; map_flx10.leds->deck_display(1, &f); ms(600); map_flx10.tick();
          CHECK(sent_n == 8, "at +-10: nothing");
          f.tempo_range = 0; map_flx10.leds->deck_display(1, &f); map_flx10.tick();  /* by hand */
          ms(600); map_flx10.tick();
          CHECK(sent_n == 8, "set to +-6 by hand: kept, it is the operator's now");
          m.bpm_raw = 11200; f.bpm_raw = 11200;                       /* +12%% */
          map_flx10.leds->deck_display(0, &m); map_flx10.leds->deck_display(1, &f);
          map_flx10.tick();
          CHECK(sent_n == 12, "+12%%: +-6 -> +-16, two steps (%d)", sent_n);
          f.tempo_range = 2; map_flx10.leds->deck_display(1, &f); ms(100); map_flx10.tick();
          m.bpm_raw = 10300; f.bpm_raw = 10300;
          map_flx10.leds->deck_display(0, &m); map_flx10.leds->deck_display(1, &f);
          map_flx10.tick();
          CHECK(sent_n == 16, "back to +3%%: +-16 -> +-6, the hand-set floor (two steps round, %d)", sent_n);
          f.tempo_range = 0; map_flx10.leds->deck_display(1, &f); ms(600); map_flx10.tick();
          m.bpm_raw = 9408; f.bpm_raw = 9408; f.bpm_orig = 10062;     /* the unit's: -6.5%% */
          map_flx10.leds->deck_display(0, &m); map_flx10.leds->deck_display(1, &f);
          map_flx10.tick();
          CHECK(sent_n == 18 && is(16, K_TEMPO_RANGE, OP_PRESS, 2), "94.08 against 100.62: +-6 -> +-10 (%d)", sent_n);
          f.tempo_range = 1; map_flx10.leds->deck_display(1, &f);
          for (int i = 0; i < 6; i++) { ms(200); map_flx10.tick(); }
          CHECK(sent_n == 18, "...and stays there, the fader's -0.68%% notwithstanding (%d)", sent_n);
          /* TEMPO RANGE pressed on the synced deck: never to a range too narrow */
          m.bpm_raw = 2000; f.bpm_raw = 2000;                         /* -80%%: WIDE */
          map_flx10.leds->deck_display(0, &m); map_flx10.leds->deck_display(1, &f);
          map_flx10.tick(); f.tempo_range = 3; map_flx10.leds->deck_display(1, &f);
          ms(600); map_flx10.tick();
          reset(); ON(2, 96); OFF(2, 96);
          CHECK(sent_n == 0, "WIDE needed: TEMPO RANGE does nothing, no flash to +-6 (%d)", sent_n);
          m.bpm_raw = 9408; f.bpm_raw = 9408;                         /* -6.5%%: +-10 */
          map_flx10.leds->deck_display(0, &m); map_flx10.leds->deck_display(1, &f);
          reset(); ON(2, 96); OFF(2, 96);       /* before the tick narrows it */
          CHECK(sent_n == 4 && is(0, K_TEMPO_RANGE, OP_PRESS, 2) && is(2, K_TEMPO_RANGE, OP_PRESS, 2),
                "+-10 needed: WIDE goes round past +-6 to +-10, two steps (%d)", sent_n);
          ms(100); map_flx10.tick();
          CHECK(sent_n == 4, "...nothing more while rbp's range comes back (%d)", sent_n);
          f.tempo_range = 1; map_flx10.leds->deck_display(1, &f); ms(100); map_flx10.tick();
          ms(600); map_flx10.tick();
          CHECK(sent_n == 4, "...and +-10 is kept");
          m.bpm_raw = 9900; f.bpm_raw = 9900;                         /* -1.6%%: +-6 would do */
          map_flx10.leds->deck_display(0, &m); map_flx10.leds->deck_display(1, &f);
          ms(600); map_flx10.tick();
          CHECK(sent_n == 4, "...as the floor: the master at -1.6%% leaves it at +-10 (%d)", sent_n);
          f.sync = 0; f.tempo_range = 3; map_flx10.leds->deck_display(1, &f); ms(600); map_flx10.tick();
          reset(); ON(2, 96); OFF(2, 96);
          CHECK(sent_n == 2 && is(0, K_TEMPO_RANGE, OP_PRESS, 2), "not synced: one step, WIDE -> +-6 as rbp does");
          CHECK(sent_n == 2, "SYNC off: the range is the operator's");
          memset(&m, 0, sizeof m); memset(&f, 0, sizeof f);
          map_flx10.leds->deck_display(0, &m); map_flx10.leds->deck_display(1, &f);
     }

     /* SYNC pressed at +-6 on a deck that needs WIDE: WIDE, and back to +-6 later --
      * the floor taken at the press survives the moment before rbp says synced */
     {
          struct deck_info m, f;
          memset(&m, 0, sizeof m); memset(&f, 0, sizeof f);
          m.loaded = f.loaded = 1; m.master = 1;
          m.bpm_raw = 2000; f.bpm_raw = 10000; f.bpm_orig = 10000; f.tempo_range = 0;
          map_flx10.leds->deck_display(0, &m); map_flx10.leds->deck_display(1, &f);
          ms(600); reset(); ON(2, 88); OFF(2, 88);
          CHECK(sent_n == 8 && is(6, K_SYNC, OP_PRESS, 2), "SYNC at +-6, -80%% needed: WIDE first (%d)", sent_n);
          ms(100); map_flx10.tick();                         /* rbp not synced yet */
          f.tempo_range = 3; map_flx10.leds->deck_display(1, &f); ms(100); map_flx10.tick();
          f.sync = 1; f.bpm_raw = 2000; map_flx10.leds->deck_display(1, &f); ms(100); map_flx10.tick();
          CHECK(sent_n == 8, "synced at WIDE: nothing more (%d)", sent_n);
          m.bpm_raw = 10300; f.bpm_raw = 10300;
          map_flx10.leds->deck_display(0, &m); map_flx10.leds->deck_display(1, &f);
          ms(600); map_flx10.tick();
          CHECK(sent_n == 10, "the master back to +3%%: WIDE -> +-6, the range SYNC was pressed on (%d)", sent_n);
          memset(&m, 0, sizeof m); memset(&f, 0, sizeof f);
          map_flx10.leds->deck_display(0, &m); map_flx10.leds->deck_display(1, &f);
     }

     /* Beat FX quantize: the touch panel's own state (the stick's, from rbp) sent again
      * 1.5 s after a load, as it is; nothing without a load */
     {
          struct deck_info di;
          memset(&di, 0, sizeof di);
          bfxq_panel = -1; ms(1600); map_flx10.tick();      /* anything pending, unsent */
          reset(); bfxq_panel = 0;
          di.loaded = 1; di.total_ms = 200000; map_flx10.leds->deck_display(0, &di);
          ms(1000); map_flx10.tick();
          CHECK(sent_n == 0, "BFX quantize: nothing within 1.5 s of a load (%d)", sent_n);
          ms(600); map_flx10.tick(); map_flx10.tick();
          CHECK(sent_n == 1 && is(0, K_EFFECTQUANT, OP_PRESS, CH_GLOBAL) && sent[0].param == 0,
                "BFX quantize: the panel's OFF goes as OFF, the first time too (%d)", sent_n);
          di.total_ms = 180000; map_flx10.leds->deck_display(0, &di);   /* another track */
          ms(1600); map_flx10.tick();
          CHECK(sent_n == 2 && is(1, K_EFFECTQUANT, OP_PRESS, CH_GLOBAL) && sent[1].param == 0,
                "BFX quantize: turned off on the panel, a load sends OFF (%d)", sent_n);
          bfxq_panel = 1; map_flx10.leds->deck_display(0, &di);         /* same track */
          ms(1600); map_flx10.tick();
          CHECK(sent_n == 2, "BFX quantize: no load, nothing sent (%d)", sent_n);
          memset(&di, 0, sizeof di); map_flx10.leds->deck_display(0, &di);
          bfxq_panel = -1;
     }

     printf("%s: %d checks, %d failures\n", failures ? "FAIL" : "ok", checks, failures);
     return failures ? 1 : 0;
}
