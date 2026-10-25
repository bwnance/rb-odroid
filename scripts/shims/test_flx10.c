/*
 * test_flx10.c -- pins map_flx10.c's dispatch against the operator's table
 * (docs/21, "FLX10 map"), with rbp stubbed out. Every input below is a message
 * the unit was measured to send (scripts/odroid/midi-learn.py).
 *
 * Pure host C: it links the production map, ctrl_map.c, shimutil.c, shmstate.c
 * and pitch_state.c, and nothing that carries an rbp address. The two timed
 * gestures (4 BEAT's simulated hold, the SCFX deselect window) are run against
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
int flx10_browse_mode(void) { return browse_mode; }
int flx10_usb1_present(void) { return usb1; }
int cc_to_10bit(int v) { if (v < 0) v = 0; if (v > 127) v = 127; return (v * 1023 + 63) / 127; }

struct sent { int key, op, ch; long param; };
static struct sent sent[512];
static int sent_n;
static void rec(int key, int op, int ch, long param)
{
     if (sent_n < 512) { sent[sent_n].key = key; sent[sent_n].op = op;
          sent[sent_n].ch = ch; sent[sent_n].param = param; sent_n++; }
}
void send_rx_key(int k, int op, int ch, long p) { rec(k, op, ch, p); }
void send_rx_key_f(int k, int op, int ch, long p, float f) { (void)f; rec(k, op, ch, p); }
void send_rx_key_fl(int k, int op, int ch, long p, float f, long l) { (void)f; (void)l; rec(k, op, ch, p); }

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

     /* 4 BEAT, not looping: IN pressed now, released ~1.1 s later from tick */
     reset(); ON(1, 20); OFF(1, 20);
     CHECK(sent_n == 1 && is(0, K_LOOPIN, OP_PRESS, 1), "4 BEAT presses IN");
     ON(1, 20); OFF(1, 20);
     CHECK(sent_n == 1, "a second 4 BEAT during the hold is ignored");
     ms(300); map_flx10.tick();
     CHECK(sent_n == 1, "IN still held at 300 ms");
     ms(900); map_flx10.tick();
     CHECK(sent_n == 2 && is(1, K_LOOPIN, OP_RELEASE, 1), "IN released after the hold");

     /* in a loop */
     looping[0] = 1;
     reset(); ON(1, 20); OFF(1, 20);
     CHECK(sent_n == 2 && is(0, K_RELOOP, OP_PRESS, 1), "4 BEAT in a loop -> exit");
     reset(); ON(1, 77); OFF(1, 77);
     CHECK(sent_n == 2 && is(0, K_LOOPOUT, OP_PRESS, 1), "SHIFT+OUT in a loop -> OUT ADJUST");
     reset(); ON(1, 76); OFF(1, 76);
     CHECK(sent_n == 2 && is(0, K_LOOPIN, OP_PRESS, 1), "SHIFT+IN -> IN (adjust)");
     reset(); ON(1, 16); OFF(1, 16);
     CHECK(sent_n == 0, "IN in a loop, CALL code unset: nothing (%d)", sent_n);

     /* with the CALL codes configured: halve/double, release follows the press */
     build("callprev=0x4118,callnext=0x4119,gatecue=0x4130");
     looping[0] = 1;
     reset(); ON(1, 16); looping[0] = 0; OFF(1, 16);
     CHECK(sent_n == 2 && is(0, 0x4118, OP_PRESS, 1) && is(1, 0x4118, OP_RELEASE, 1),
           "IN in a loop -> CALL <, released to the same key after the loop ended");
     looping[0] = 1;
     reset(); ON(1, 17); OFF(1, 17);
     CHECK(sent_n == 2 && is(0, 0x4119, OP_PRESS, 1), "OUT in a loop -> CALL >");
     looping[0] = 0;

     /* SHIFT pad modes: the configured one sends, its pads are bound; others not */
     reset(); ON(1, 105);
     CHECK(sent_n == 2 && is(0, 0x4130, OP_PRESS, 1), "SHIFT+HOT CUE -> gatecue key");
     reset(); ON(8, 64); OFF(8, 64);
     CHECK(sent_n == 2 && is(0, K_PAD1, OP_PRESS, 1), "KEYBOARD-base pad bound once gatecue is set");
     reset(); ON(8, 80); OFF(8, 80); ON(1, 107);
     CHECK(sent_n == 0, "PAD FX2 base unbound while releasefx is unset (%d)", sent_n);

     /* pad modes and pads */
     reset(); ON(1, 30); ON(1, 32); ON(1, 109); ON(1, 34);
     CHECK(sent_n == 3 && is(0, K_SLIPLOOP, OP_PRESS, 1) && is(1, K_BEATJUMP, OP_PRESS, 1) &&
           is(2, K_ALOOP, OP_PRESS, 1), "PAD FX1/BEAT JUMP/BEAT LOOP; SAMPLER nothing");
     OFF(1, 30); OFF(1, 32); OFF(1, 109); OFF(1, 34);
     reset(); ON(10, 7 + 32); OFF(10, 7 + 32);
     CHECK(is(0, K_PAD1 + 7, OP_PRESS, 2), "deck 2 pad 8 in BEAT JUMP");
     reset(); ON(10, 50);
     CHECK(sent_n == 0, "SAMPLER-base pad unbound"); OFF(10, 50);
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
     CHECK(sent_n == 2 && is(0, K_LOAD, OP_PRESS, 2), "push 71 loads deck 2");
     /* on SOURCE (browse mode 12) with a stick in slot 1, a push opens USB 1 */
     browse_mode = 12;
     reset(); ON(7, 70); OFF(7, 70);
     CHECK(sent_n == 2 && is(0, K_USB1, OP_PRESS, 1) && is(1, K_USB1, OP_RELEASE, 1),
           "push on SOURCE -> USB1");
     usb1 = 0;
     reset(); ON(7, 71); OFF(7, 71);
     CHECK(sent_n == 2 && is(0, K_LOAD, OP_PRESS, 2), "push on SOURCE, slot 1 empty -> LOAD as before");
     usb1 = 1; browse_mode = 1;
     reset(); ON(7, 70); ms(650); map_flx10.tick(); OFF(7, 70);
     CHECK(sent_n == 0, "held push: TAG TRACK unset -> nothing, and no LOAD (%d)", sent_n);
     reset(); ON(7, 122); ON(7, 104); ON(7, 93);
     CHECK(is(0, K_SOURCE, OP_PRESS, 1) && is(1, K_SHORTCUT, OP_PRESS, 1) &&
           is(2, K_TRACKFILTER, OP_PRESS, 1) && is(3, K_TRACKFILTER, OP_REPEAT, 1),
           "VIEW -> SOURCE, SHIFT+VIEW -> SHORTCUT, SHIFT+push -> TRACK FILTER");
     OFF(7, 122); OFF(7, 104); OFF(7, 93);

     /* Sound Color FX radio group */
     reset(); ON(7, 0);
     CHECK(sent_n == 4 && is(0, K_SPACE, OP_PRESS, 1) && is(2, K_SPACE, OP_PRESS, 2), "SPACE on both players");
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
     reset(); ON(5, 107); ON(5, 74);
     CHECK(is(0, K_TAP, OP_PRESS, 1) && sent[1].key == K_BEATPREV, "TAP, BEAT <");

     /* faders: MSB drives, LSB dropped; strip 3 ignored; booth ignored */
     reset(); CC(1, 19, 127); CC(1, 51, 5); CC(3, 19, 64); CC(7, 9, 64); CC(7, 41, 1);
     CHECK(sent_n == 1 && sent[0].key == K_FADER && sent[0].ch == 1, "fader 1 only (%d)", sent_n);

     printf("%s: %d checks, %d failures\n", failures ? "FAIL" : "ok", checks, failures);
     return failures ? 1 : 0;
}
