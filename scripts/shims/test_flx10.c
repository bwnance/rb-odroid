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
static int cue[4], mcue, deleted_deck = -1, deleted_pad = -1;
int me_get_cue(int idx) { return cue[idx]; }
void me_set_cue(int idx, int on) { cue[idx] = on; }
int me_get_master_cue(void) { return mcue; }
void me_set_master_cue(int on) { mcue = on; }
int hotcue_delete(int deck, int pad) { deleted_deck = deck; deleted_pad = pad; return 0; }
static int browse_mode = 1, usb1 = 1;
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
     build(NULL);

     /* transport, both decks */
     reset(); ON(1, 11); OFF(1, 11);
     CHECK(sent_n == 2 && is(0, K_PLAY, OP_PRESS, 1) && is(1, K_PLAY, OP_RELEASE, 1), "deck 1 PLAY");
     reset(); ON(2, 12); OFF(2, 12);
     CHECK(sent_n == 2 && is(0, K_CUE, OP_PRESS, 2), "deck 2 CUE on player 2");

     /* KEY SYNC toggles MASTER TEMPO; its long press does nothing */
     reset(); ON(1, 101); OFF(1, 101);
     CHECK(sent_n == 2 && is(0, K_MT, OP_PRESS, 1) && is(1, K_MT, OP_RELEASE, 1), "KEY SYNC -> MT");
     reset(); ON(1, 26); OFF(1, 26); OFF(1, 101);
     CHECK(sent_n == 0, "KEY SYNC held sends nothing (%d sent)", sent_n);

     /* SHIFT + BEAT SYNC = MASTER; TEMPO RESET nothing, SHIFT = TEMPO RANGE */
     reset(); ON(1, 92);
     CHECK(sent_n == 2 && is(0, K_MASTER, OP_PRESS, 1), "SHIFT+SYNC -> MASTER");
     reset(); ON(1, 65); OFF(1, 65); ON(1, 96);
     CHECK(sent_n == 1 && is(0, K_TEMPO_RANGE, OP_PRESS, 1), "TEMPO RESET nothing, SHIFT -> RANGE");
     OFF(1, 96);

     /* beat jump / track search, slip reverse, slip / vinyl */
     reset(); ON(2, 94); ON(2, 98);
     CHECK(is(0, K_SRREV, OP_PRESS, 2) && is(1, K_TRFWD, OP_PRESS, 2), "BEAT JUMP < / SHIFT > ");
     OFF(2, 94); OFF(2, 98);
     reset(); ON(1, 23);
     CHECK(is(0, K_VINYL, OP_PRESS, 1), "SHIFT+SLIP -> VINYL"); OFF(1, 23);

     /* the loop section, not looping */
     looping[0] = 0;
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

     /* browse: both knobs scroll; pushes load their own deck; hold = TAG TRACK */
     reset(); CC(7, 64, 1); CC(7, 64, 127);
     CHECK(sent_n == 2 && sent[0].key == K_SELECTOR && sent[0].param == 1 && sent[1].param == -1, "browse");
     reset(); CC(7, 100, 127);
     CHECK(sent_n == 8 && sent[7].param == -1, "SHIFT+browse = 8 steps");
     reset(); ON(7, 71); map_flx10.tick(); OFF(7, 71);
     CHECK(sent_n == 4 && is(0, K_LOAD, OP_PRESS, 2) && is(1, K_LOAD, OP_RELEASE, 2) &&
           is(2, K_SELECTOR, OP_PRESS, 1) && is(3, K_SELECTOR, OP_RELEASE, 1),
           "push 71: LOAD deck 2, then SELECT");
     /* on SOURCE (browse mode 12) with a stick in slot 1, a push opens USB 1 */
     browse_mode = 12;
     reset(); ON(7, 70); OFF(7, 70);
     CHECK(sent_n == 2 && is(0, K_USB1, OP_PRESS, 1) && is(1, K_USB1, OP_RELEASE, 1),
           "push on SOURCE -> USB1");
     usb1 = 0;
     reset(); ON(7, 71); OFF(7, 71);
     CHECK(sent_n == 4 && is(0, K_LOAD, OP_PRESS, 2) && is(2, K_SELECTOR, OP_PRESS, 1),
           "push on SOURCE, slot 1 empty -> LOAD + SELECT as elsewhere");
     usb1 = 1; browse_mode = 1;
     reset(); ON(7, 70); ms(650); map_flx10.tick(); OFF(7, 70);
     CHECK(sent_n == 2 && is(0, 0x420e, OP_PRESS, 1) && is(1, 0x420e, OP_RELEASE, 1),
           "held push -> TAG TRACK 0x420e, and no LOAD (%d)", sent_n);
     /* SAMPLER VOL -> SCFX PARAMETER 0x50a7 on both mixer channels */
     reset(); CC(7, 3, 127); CC(7, 35, 9);
     CHECK(sent_n == 2 && sent[0].key == 0x50a7 && sent[0].ch == 1 && sent[1].ch == 2 &&
           sent[0].param == 1023, "SAMPLER VOL -> PARAMETER on ch 1 and 2 (%d)", sent_n);
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
          CHECK(sent_n == 1 && sent[0].key == K_JOG_ROT && sent[0].f > 0.8f && sent[0].f < 3.0f,
                "burst -> about 1.7 rev/s, not the clamp (got %.3f)", sent_n ? sent[0].f : -1.0f);
          float fast = sent_n ? sent[0].f : 0.0f;
          ms(200); map_flx10.tick();
          CHECK(sent_n >= 2 && sent[sent_n - 1].f == 0.0f, "idle edge sends speed 0");
          reset();
          CC(1, 34, 64 + 1);
          for (int i = 0; i < 4; i++) { ms(10); CC(1, 34, 64 + 1); }
          CHECK(sent_n >= 3 && sent[sent_n - 1].f > 0.0f && sent[sent_n - 1].f < 0.05f,
                "slow turn -> slow (got %.4f)", sent_n ? sent[sent_n - 1].f : -1.0f);
          CHECK(fast > 20.0f * (sent_n ? sent[sent_n - 1].f : 1.0f), "fast is much faster than slow");
          ms(200); map_flx10.tick();
          /* one full revolution of counts moves rbp's position by 720 */
          reset();
          long before = -1;
          CC(2, 35, 64 + 1); ms(5); CC(2, 35, 64 + 1);
          before = sent_n ? sent[sent_n - 1].l : -1;
          for (int n = 0; n < 12960; n += 21) { CC(2, 35, 64 + 21); }
          ms(5); CC(2, 35, 64 + 1); ms(30); map_flx10.tick();
          long after = sent[sent_n - 1].l;
          long moved = (after - before) & 0xFFFF;
          CHECK(sent[0].ch == 2 && moved > 700 && moved < 740, "one rev moves rbp's position ~720 (%ld)", moved);
          ms(200); map_flx10.tick();
     }

     /* ---- LEDs (flx10-leds.json) ---- */
     {
          extern int flx10_led_pad_base(int deck, int rbp_mode);
          const struct led_notes *n = map_flx10.leds;
          CHECK(n != NULL, "map_flx10 has an LED table");
          if (n) {
               CHECK(n->deck_ch == 0 && n->n_sync == 88 && n->n_cue == 12 &&
                     n->n_play == 11 && n->n_keylock == 101 && n->n_slip == 64,
                     "deck LED notes");
               CHECK(n->n_loopin == 16 && n->n_loopout == 17 && n->n_autoloop == 20,
                     "loop LED notes");
               CHECK(n->n_vinyl == -1 && n->master_ch_first < 0 && n->fx_ch < 0 &&
                     n->meter_master_ch < 0, "unanswered LEDs stay dark");
               CHECK(n->n_quantize == 53, "QUANTIZE LED on 53");
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
          /* the pad page follows the unit's own mode buttons */
          reset(); ON(1, 30); OFF(1, 30);
          CHECK(flx10_led_pad_base(0, 0) == 16, "PAD FX1 -> page 16 (%d)",
                flx10_led_pad_base(0, 0));
          reset(); ON(1, 109); OFF(1, 109);
          CHECK(flx10_led_pad_base(0, 0) == 96, "SHIFT+BEAT JUMP -> page 96");
          reset(); ON(1, 34); OFF(1, 34);
          CHECK(flx10_led_pad_base(0, 0) == 48, "SAMPLER -> page 48");
          reset(); ON(2, 111); OFF(2, 111);
          CHECK(flx10_led_pad_base(1, 0) == 112 && flx10_led_pad_base(0, 0) == 48,
                "deck 2 KEY SHIFT -> 112, deck 1 untouched");
          reset(); ON(1, 27); OFF(1, 27);
          CHECK(flx10_led_pad_base(0, 5) == 0, "HOT CUE -> page 0");
     }

     printf("%s: %d checks, %d failures\n", failures ? "FAIL" : "ok", checks, failures);
     return failures ? 1 : 0;
}
