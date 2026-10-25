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
#include <sound/asequencer.h>

#include "shimutil.h"
#include "rbp_abi.h"
#include "rbp_bridge.h"
#include "rbp_vu.h"
#include "ctrl_map.h"
#include "pitch_state.h"
#include "flx10_hid.h"

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
static int dk_note_v[2][128];                 /* last note value on deck ch 1/2 */
int midi_cc(int ch, int cc, int v) { if (ch == 15 && cc >= 0 && cc < 128) jd_cc_v[cc] = v; return 1; }
int midi_note(int ch, int n, int v)
{
     if (ch == 15 && n >= 0 && n < 128) { jd_note_v[n] = v; if (n == 0x70) note70_n++; }
     if ((ch == 0 || ch == 1) && n >= 0 && n < 128) dk_note_v[ch][n] = v;
     return 1;
}
int midi_sysex_out(const unsigned char *b, unsigned n) { (void)b; (void)n; return 1; }
int midi_surface_generation(void) { return 1; }
int midi_out_ready(void) { return 1; }
volatile int led_kick;
static int cue[4], mcue, deleted_deck = -1, deleted_pad = -1;
int me_get_cue(int idx) { return cue[idx]; }
void me_set_cue(int idx, int on) { cue[idx] = on; }
int me_get_master_cue(void) { return mcue; }
void me_set_master_cue(int on) { mcue = on; }
int hotcue_delete(int deck, int pad) { deleted_deck = deck; deleted_pad = pad; return 0; }
static int browse_mode = 1, usb1 = 1, cursor_track = 1;
int flx10_cursor_is_track(void) { return cursor_track; }
/* the engine, as SLIP REVERSE / REVERSE see it */
static int slipmode[2], reversing[2], slip_sets, rev_sets;
int flx10_slip_mode(int d) { return slipmode[d]; }
int flx10_reversing(int d) { return reversing[d]; }
void flx10_set_slip(int d, int on) { slipmode[d] = on; slip_sets++; }
void flx10_set_reverse(int d, int on) { reversing[d] = on; rev_sets++; }
static int bj_n, bj_deck[16], bj_t[16];
static int goes_back[2];
int flx10_trrev_goes_back(int d) { return goes_back[d]; }
int flx10_cue_ms(int d, int total_ms) { (void)d; (void)total_ms; return 1000; }
static int loop_primes[2];
void flx10_loop_prime(int d) { loop_primes[d]++; }
static int xf_assign[2] = { -1, -1 };
void flx10_set_xf_assign(int d, int a) { xf_assign[d] = a; }
static int second_press[2];
static long start_pos[2] = { 22050, 22050 }; static int start_ms[2] = { 500, 500 };
static int seek_n, seek_deck; static long seek_pos;
long flx10_start_pos(int d, int *ms) { *ms = start_ms[d]; return start_pos[d]; }
void flx10_seek(int d, long pos) { seek_n++; seek_deck = d; seek_pos = pos; }
void flx10_trrev_as_second_press(int d) { second_press[d]++; }
static int bpm_auto_n;
void flx10_bfx_bpm_auto(void) { bpm_auto_n++; }
void flx10_beat_jump(int d, int type) { if (bj_n < 16) { bj_deck[bj_n] = d; bj_t[bj_n] = type; } bj_n++; }
static int pj_n, pj_deck[16]; static float pj_b[16];
int flx10_plain_jump(int d, float b) { if (pj_n < 16) { pj_deck[pj_n] = d; pj_b[pj_n] = b; } pj_n++; return 1; }
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

int main(void)
{
     setenv("FLX10_HID", "0", 1);          /* no sender thread in the tests */
     memset(jd_note_v, 0xff, sizeof jd_note_v);
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
     CHECK(sent_n == 1 && is(0, K_TEMPO_RANGE, OP_PRESS, 1), "SHIFT + TEMPO RESET -> RANGE");
     OFF(1, 96);

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
     reset(); OFF(1, 21);
     CHECK(sent_n == 1 && is(0, K_REV, OP_RELEASE, 1) && slipmode[0] == 1,
           "release: REV release, slip still on for the return");
     ms(250); map_flx10.tick();
     CHECK(slipmode[0] == 1, "slip mode kept while the deck is watched");
     /* quantize: the reverse turns up after the release -> ended by the map */
     reversing[0] = 1; rev_sets = 0; ms(20); map_flx10.tick();
     CHECK(rev_sets == 1 && reversing[0] == 0, "a late reverse after the release is ended");
     for (int i = 0; i < 30 && slipmode[0]; i++) { ms(100); map_flx10.tick(); }
     CHECK(slipmode[0] == 0, "slip mode put back off once the deck plays forward");
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
     ON(11, 3);
     CHECK(deleted_deck == 1 && deleted_pad == 4, "SHIFT+pad 4 deck 2 deletes its hot cue");

     /* channel CUE latches; master CUE */
     ON(1, 84); OFF(1, 84);
     CHECK(cue[0] == 1, "ch1 CUE on");
     ON(7, 99);
     CHECK(mcue == 1, "master CUE on");

     /* browse: both knobs scroll; pushes load their own deck; hold = fast scroll */
     reset(); CC(7, 64, 1); CC(7, 64, 127);
     CHECK(sent_n == 2 && sent[0].key == K_SELECTOR && sent[0].param == 1 && sent[1].param == -1, "browse");
     reset(); CC(7, 100, 127);
     CHECK(sent_n == 8 && sent[7].param == -1, "SHIFT+browse = 8 steps");
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
     reset(); ON(7, 71); OFF(7, 71);
     CHECK(sent_n == 1 && is(0, K_LOAD, OP_PRESS, 2),
           "push on SOURCE, slot 1 empty -> as elsewhere (%d)", sent_n);
     ms(60); map_flx10.tick();
     usb1 = 1; browse_mode = 1;
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
     browse_mode = 1;
     reset(); ON(7, 122); ON(7, 104); ON(7, 93);
     CHECK(is(0, K_SOURCE, OP_PRESS, 1) && is(1, K_SHORTCUT, OP_PRESS, 1) &&
           is(2, K_TRACKFILTER, OP_PRESS, 1) && is(3, K_TRACKFILTER, OP_REPEAT, 1),
           "VIEW -> SOURCE, SHIFT+VIEW -> SHORTCUT, SHIFT+push -> TRACK FILTER");
     OFF(7, 122); OFF(7, 104); OFF(7, 93);

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
               { 32, -1, "LOW CUT ECHO" }, { 33, 1, "ECHO" }, { 34, 0, "MT DELAY" },
               { 35, 3, "SPIRAL" }, { 36, 5, "REVERB" }, { 37, 9, "TRANS" },
               { 38, -1, "ENIGMA JET" }, { 39, 6, "FLANGER" }, { 40, 7, "PHASER" },
               { 41, -1, "STRETCH" }, { 42, 11, "SLIP ROLL" }, { 43, 10, "ROLL" },
               { 44, -1, "MOBIUS saw" }, { 45, -1, "MOBIUS tri" },
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
          /* x the jog gain (12960/5484, one turn = 4 beats as rekordbox): ~4 */
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
          /* one full revolution of counts moves rbp's position by 720 x the
           * gain (12960/5484 = 2.363): ~1702 */
          reset();
          long before = -1;
          CC(2, 35, 64 + 1); ms(5); CC(2, 35, 64 + 1);
          before = sent_n ? sent[sent_n - 1].l : -1;
          for (int n = 0; n < 12960; n += 21) { CC(2, 35, 64 + 21); }
          ms(5); CC(2, 35, 64 + 1); ms(30); map_flx10.tick();
          long after = sent[sent_n - 1].l;
          long moved = (after - before) & 0xFFFF;
          CHECK(sent[0].ch == 2 && moved > 1680 && moved < 1725, "one rev moves rbp's position ~1702 (%ld)", moved);
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
               CHECK(n->n_vinyl == -1 && n->master_ch_first < 0 && n->fx_ch < 0 &&
                     n->meter_master_ch < 0, "unanswered LEDs stay dark");
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
          hs.bpm_x100 = 13905; hs.key_code = 7; hs.cue_ms = 358;
          flx10_hid_state_pkt(p, 1, &hs);
          same = 1;
          for (int i = 0; i < 64; i++)
               if (i != 45 && p[i] != cap_loaded[i]) {
                    same = 0;
                    printf("  byte %d: %02x, capture %02x\n", i, p[i], cap_loaded[i]);
               }
          CHECK(same, "loaded deck 2 = rekordbox's packet (byte 45 aside)");

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
          hs.playing = 0; hs.cue_standby = 0; hs.bpm_x100 = 38056; flx10_hid_state_pkt(p, 0, &hs);
          CHECK(p[2] == 0x02 && p[21] == 0x7c && p[22] == 0x51 && p[49] == 0x60,
                "paused off the cue -> 02; 380.56 BPM -> 7c 51 / 60");
          CHECK(flx10_key_shift(-912) == -2 && flx10_key_shifted(7, -2) == 3, "-9.12%% -> -2 semitones; 5B -2 = 3B");
          hs.key_code = 0; flx10_hid_state_pkt(p, 0, &hs);
          CHECK(p[60] == 0x80 && p[61] == 0x0d, "no key -> 80 0d");
          hs.cue_ms = -1; flx10_hid_state_pkt(p, 0, &hs);
          CHECK(p[31] == 0 && p[55] == 0, "no cue -> zeros");
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
          CHECK(flx10_loop_code(1459, 16456 * 11 / 10, 1000) == 0x0e, "track-time loop at +10%% still 4 beats");
          hs.master = 1; hs.sync = 1; hs.jog_touch = 1; flx10_hid_state_pkt(p, 0, &hs);
          CHECK(p[5] == 0x91 && p[3] == 0x18, "MASTER + SYNC -> 91, jog touched -> 18 (capture 2)");
          hs.jog_touch = 0; hs.time_remain = 1; flx10_hid_state_pkt(p, 0, &hs);
          CHECK(p[3] == 0x0a, "rbp's time mode remaining -> [3] 0a");
          hs.master = 0; hs.sync = 0; hs.jog_touch = 0; hs.time_remain = 0;
          hs.loop_in_ms = hs.loop_out_ms = -1; flx10_hid_state_pkt(p, 0, &hs);
          CHECK(p[25] == 0 && p[27] == 0x80 && p[28] == 0, "no loop -> 00 00 80");
          /* position between rbp's readings */
          CHECK(flx10_hid_extrapolate(1000, 50, 1, 0, 0) == 1050, "50 ms at 0%% -> +50");
          CHECK(flx10_hid_extrapolate(1000, 50, 1, 1000, 0) == 1055, "50 ms at +10%% -> +55");
          CHECK(flx10_hid_extrapolate(1000, 50, 0, 0, 0) == 1000, "paused -> no movement");
          CHECK(flx10_hid_extrapolate(1000, 5000, 1, 0, 0) == 1200, "capped 200 ms ahead");
          CHECK(flx10_hid_extrapolate(9990, 50, 1, 0, 10000) == 10000, "capped at the track's end");
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
          CHECK(note70_n == 1, "9F 70 not sent twice");
          CHECK(dk_note_v[0][101] == 0 && dk_note_v[1][101] == 0, "KEY SYNC LED sent dark on connect");
     }

     printf("%s: %d checks, %d failures\n", failures ? "FAIL" : "ok", checks, failures);
     return failures ? 1 : 0;
}
