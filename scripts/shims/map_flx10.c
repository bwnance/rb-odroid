/*
 * map_flx10.c -- the Pioneer DDJ-FLX10, driving the RX3 player (rbp).
 *
 * EVERY NUMBER IN THIS FILE IS MEASURED ON THE UNIT, not taken from a vendor
 * list: no MIDI message list for the FLX10 was at hand, so each control was
 * recorded off the hardware with scripts/odroid/midi-learn.py (2026-10-24, on the
 * ODROID-N2+ -- docs/21, "DDJ-FLX10"). Where a row is a guess it says so.
 *
 * WHAT DRIVES WHAT is the operator's own table, agreed control by control and
 * recorded in docs/21 ("FLX10 map"). The shape follows map_flx4.c, whose surface
 * the FLX10 resembles closely -- PLAY 11, CUE 12, SHIFT 63, loop IN/OUT 16/17,
 * BEAT SYNC 88, channel CUE 84 and the pad-mode buttons 27/30/32/34 are the same
 * numbers -- so the parts that are the same are written the same way, and the
 * comments here are about where the FLX10 differs.
 *
 * The RX3 is a 2-deck player: FLX10 decks 1 and 2 (MIDI ch 1/2) and mixer strips
 * 1 and 2 drive it. DECK SELECT moves a side's controls to ch 3/4, and nothing on
 * ch 3/4 is bound, so decks 3 and 4 and strips 3 and 4 do nothing by design.
 *
 * THE RX3 KEYCODES NOT YET FOUND are not guessed either. They are read from the
 * environment (FLX10_KEYS, see flx10_keys_parse()), so each one is a line in
 * rb.local.conf and a restart -- no rebuild -- while they are being identified:
 *
 *   RB_FLX10_KEYS="callprev=0x4118,callnext=0x4119"
 *
 *   callprev, callnext  rbp's CUE/LOOP CALL < / >: IN/OUT inside a loop send
 *                       these, which is how the RX3 halves and doubles a loop
 *   gatecue, releasefx, beatjump2
 *                       the RX3's SHIFT pad modes, from SHIFT + HOT CUE /
 *                       PAD FX1 / SAMPLER; their pads are bound only once the
 *                       mode key is, so an unset mode cannot leave the pads
 *                       acting in the previous mode
 *   param               the Sound Color FX PARAMETER knob (from SAMPLER VOL)
 *   tagtrack            TAG TRACK (from holding a browse knob down)
 *
 * An unset name logs one line per press ("not yet mapped") and does nothing.
 */
#define _GNU_SOURCE
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>         /* access() */
#include <sound/asequencer.h>

#include "shimutil.h"
#include "shmstate.h"       /* g_cue_mix / g_cue_gain / g_mirror_gain */
#include "rbp_abi.h"
#include "rbp_bridge.h"
#include "rbp_vu.h"         /* g_fader / g_fader_seen */
#include "pitch_state.h"
#include "ctrl_map.h"

/* ---- receive channels, 0-based (MIDI channel minus one) ------------------ */
#define CH_DECK1   0    /* deck 1: buttons, jog, tempo, mixer strip 1 */
#define CH_DECK2   1    /* deck 2, the same set */
#define CH_FX      4    /* MIDI ch 5: the Beat FX section */
#define CH_MIX     6    /* MIDI ch 7: mixer, browse, LOAD, SCFX buttons */
#define CH_PADS1   7    /* MIDI ch 8: deck 1 pads */
#define CH_PADS1S  8    /* MIDI ch 9: deck 1 pads + SHIFT */
#define CH_PADS2   9    /* MIDI ch 10: deck 2 pads */
#define CH_PADS2S  10   /* MIDI ch 11: deck 2 pads + SHIFT */

#define IS_DECK(ch) ((ch) == CH_DECK1 || (ch) == CH_DECK2)

/* ---- deck notes (ch 1/2), measured ---- */
#define N_PLAY          11
#define N_CUE           12
#define N_SHIFT         63
#define N_SYNC          88    /* sent when the finger LEAVES (as on the FLX4) */
#define N_SYNC_SHIFT    92    /* SHIFT + BEAT SYNC = MASTER */
#define N_SYNC_LONG    120    /* long press (SYNC RATE): nothing on the RX3 */
#define N_KEYSYNC      101    /* KEY SYNC, short press */
#define N_KEYSYNC_LONG  26    /* KEY SYNC held: nothing for now (operator) */
#define N_TEMPO_RESET   65
#define N_TEMPO_RANGE   96    /* SHIFT + TEMPO RESET */
#define N_IN            16    /* IN / 1/2X */
#define N_OUT           17    /* OUT / 2X */
#define N_4BEAT         20    /* 4 BEAT / EXIT */
#define N_IN_SHIFT      76    /* SHIFT + IN (IN ADJUST) */
#define N_OUT_SHIFT     77    /* SHIFT + OUT (OUT ADJUST / RELOOP) */
#define N_4BEAT_SHIFT   80    /* SHIFT + 4 BEAT (ACTIVE): nothing */
#define N_BJ_PREV       94    /* 4 BEAT JUMP <  -> SEARCH < */
#define N_BJ_NEXT       95
#define N_BJ_PREV_SHIFT 97    /* SHIFT -> TRACK SEARCH < */
#define N_BJ_NEXT_SHIFT 98
#define N_SLIP_REVERSE  21
#define N_REVERSE       56    /* SHIFT + SLIP REVERSE */
#define N_QUANTIZE      53
#define N_SLIP          64
#define N_VINYL         23    /* SHIFT + SLIP */
#define N_JOG_TOUCH     54    /* platter pressed down (mechanical) */
#define N_JOG_TOUCH_SH 103    /* the same, with SHIFT held */
#define N_CH_CUE        84
#define N_MODE_HOT      27
#define N_MODE_PADFX1   30
#define N_MODE_BEATJUMP 32
#define N_MODE_SAMPLER  34
#define N_MODE_KEYBOARD 105   /* SHIFT + HOT CUE */
#define N_MODE_PADFX2   107   /* SHIFT + PAD FX1 */
#define N_MODE_BEATLOOP 109   /* SHIFT + BEAT JUMP */
#define N_MODE_KEYSHIFT 111   /* SHIFT + SAMPLER */

/* ---- deck CCs ---- */
#define CC_TEMPO_MSB     0
#define CC_TEMPO_LSB    32
#define CC_JOG_SIDE     33
#define CC_JOG_VINYL    34    /* platter top, vinyl mode on */
#define CC_JOG_NOVINYL  35    /* platter top, vinyl mode off */
#define CC_JOG_SHIFT    31    /* platter top + SHIFT */
#define CC_JOG_SIDE_SH  38    /* side ring + SHIFT */
#define CC_TRIM          4
#define CC_EQH           7
#define CC_EQM          11
#define CC_EQL          15
#define CC_FADER        19

/* ---- mixer channel (ch 7) ---- */
#define N_LOAD1         70    /* deck 1's browse knob, pushed */
#define N_LOAD2         71
#define N_PUSH1_SHIFT   93    /* SHIFT + deck 1's browse push -> TRACK FILTER */
#define N_BACK         101
#define N_VIEW         122
#define N_BACK_SHIFT   102
#define N_VIEW_SHIFT   104
#define N_MASTER_CUE    99
#define N_SAMPLER_CUE  105
#define N_SCFX_FIRST     0    /* SPACE D.ECHO CRUSH PITCH NOISE FILTER = 0..5 */
#define CC_BROWSE       64    /* both knobs send this: relative 0x01/0x7F */
#define CC_BROWSE_SHIFT 100
#define CC_CFX1         23
#define CC_CFX2         24
#define CC_XFADER       31
#define CC_MASTER_MSB    8
#define CC_MASTER_LSB   40
#define CC_BOOTH_MSB     9    /* the unit's own booth gain: not rbp's business */
#define CC_HP_MIX       12
#define CC_HP_LEVEL     13
#define CC_SAMPLER_VOL   3    /* -> SCFX PARAMETER, once that key is known */

/* ---- Beat FX (ch 5) ---- */
#define N_FX_BEATPREV   74
#define N_FX_BEATNEXT   75
#define N_FX_AUTO      102    /* SHIFT + BEAT <: nothing */
#define N_FX_TAP       107    /* SHIFT + BEAT > */
#define N_FX_ONOFF      71
#define N_FX_RELEASE    67    /* SHIFT + ON/OFF: nothing */
#define N_FX_SEL_FIRST  32    /* effect selector, 14 positions: 32..45 */
#define N_FX_SEL_LAST   45
#define N_FX_CH_FIRST   16    /* channel selector, 7 positions: 16..22 */
#define N_FX_CH_LAST    22
#define CC_FX_LEVEL      2

#define MIRROR_GAIN_MID_DEFAULT 0.6

/* ---- calibration and the keys still to be identified ---- */
static int knob_scale = 1;
static int browse_shift_steps = 8;   /* SHIFT + browse: steps per detent */
static int jog_scale = 1;
static int jog_ppr = 720;            /* the FLX4's measured value; the FLX10's
                                      * platter is unmeasured (JOG_PPR) */
static int jog_rev = 0;
static int jog_idle_ms = 120;
static int jog_verbose = 0;
static int tempo_verbose = 0;
static int tempo_rev = 0;
static int fourbeat_ms = 1100;       /* how long the simulated IN hold lasts */
static int browse_hold_ms = 600;     /* a browse push held this long = TAG TRACK */
static double mastervol_mid = MIRROR_GAIN_MID_DEFAULT;

static int k_callprev, k_callnext, k_gatecue, k_releasefx, k_beatjump2,
           k_param, k_tagtrack;

/* FLX10_KEYS: "name=0xNNNN,name=0xNNNN". Unknown names are logged and skipped,
 * so a typo is visible rather than silently ignored. */
static void flx10_keys_parse(void)
{
     const char *s = env_text("FLX10_KEYS", "");
     char buf[512];
     char *tok, *save = NULL;

     if (!s[0])
          return;
     snprintf(buf, sizeof buf, "%s", s);
     for (tok = strtok_r(buf, ", ;", &save); tok; tok = strtok_r(NULL, ", ;", &save)) {
          char *eq = strchr(tok, '=');
          int *slot = NULL;
          long v;

          if (!eq) {
               klog("knobshim2: FLX10_KEYS: '%s' is not name=code; skipped\n", tok);
               continue;
          }
          *eq = '\0';
          v = strtol(eq + 1, NULL, 0);
          if      (!strcmp(tok, "callprev"))  slot = &k_callprev;
          else if (!strcmp(tok, "callnext"))  slot = &k_callnext;
          else if (!strcmp(tok, "gatecue"))   slot = &k_gatecue;
          else if (!strcmp(tok, "releasefx")) slot = &k_releasefx;
          else if (!strcmp(tok, "beatjump2")) slot = &k_beatjump2;
          else if (!strcmp(tok, "param"))     slot = &k_param;
          else if (!strcmp(tok, "tagtrack"))  slot = &k_tagtrack;
          if (!slot) {
               klog("knobshim2: FLX10_KEYS: unknown name '%s'; skipped\n", tok);
               continue;
          }
          *slot = (int)v;
          klog("knobshim2: FLX10_KEYS: %s = 0x%04x\n", tok, (unsigned)v);
     }
}

static void not_yet(const char *what, int deck)
{
     klog("knobshim2: FLX10 %s (deck %d): RX3 keycode not yet mapped "
          "(FLX10_KEYS)\n", what, deck);
}

static void pulse(int key, int sch)
{
     send_rx_key(key, OP_PRESS, sch, 0);
     send_rx_key(key, OP_RELEASE, sch, 0);
}

/* ---- SHIFT ---------------------------------------------------------------
 * The FLX10 reports SHIFT itself (note 63 on the deck channel) and also sends
 * distinct notes for most SHIFT combinations, so only the controls whose SHIFT
 * layer is NOT a separate message need this: the Beat FX LEVEL knob (SHIFT turns
 * it into TIME). Either deck's SHIFT counts. */
static int shift_down[2];

static int any_shift(void) { return shift_down[0] || shift_down[1]; }

/* ---- the jog, the pitch fader, MASTER LEVEL ------------------------------
 * The same contracts as map_flx4.c's -- see that file for why each is shaped
 * the way it is. Copied rather than shared because each map owns its dispatch. */
struct jog_ctrl {
     int sch, started, moving;
     unsigned long long last_ms;
     unsigned int vpos;
};
static struct jog_ctrl jog_state[2] = { { 1, 0, 0, 0, 0 }, { 2, 0, 0, 0, 0 } };

static void flx10_jog(int deck, int val)
{
     struct jog_ctrl *s = &jog_state[deck];
     int d = val - 0x40;
     unsigned long long t;
     float dt, speed;
     int dp;

     if (d == 0)
          return;
     t = shim_now_ms();
     if (!s->started) {
          s->started = 1;
          s->last_ms = t;
          return;
     }
     if (jog_rev)
          d = -d;
     dp = d * jog_scale;
     if (dp > 4096) dp = 4096;
     if (dp < -4096) dp = -4096;
     dt = (float)(long long)(t - s->last_ms) / 1000.0f;
     s->last_ms = t;
     if (dt < 0.0005f)
          dt = 0.0005f;
     s->vpos = (s->vpos + (unsigned int)dp) & 0xFFFFu;
     speed = (float)dp / (float)(jog_ppr * jog_scale) / dt;
     if (speed > 8.0f) speed = 8.0f;
     if (speed < -8.0f) speed = -8.0f;
     s->moving = 1;
     send_rx_key_fl(K_JOG_ROT, OP_ROTATE, s->sch, 0, speed, (long)s->vpos);
     if (jog_verbose)
          klog("knobshim2: flx10 jog deck%d delta=%d speed=%.2f\n",
               deck + 1, d, (double)speed);
}

static void flx10_jog_idle(unsigned long long t)
{
     for (int i = 0; i < 2; i++) {
          struct jog_ctrl *s = &jog_state[i];
          if (!s->started || !s->moving ||
              t - s->last_ms < (unsigned long long)jog_idle_ms)
               continue;
          s->moving = 0;
          send_rx_key_fl(K_JOG_ROT, OP_ROTATE, s->sch, 0, 0.0f, (long)s->vpos);
     }
}

struct pitch_ctrl { int have_hi, pos; };
static struct pitch_ctrl pitch_state[2] = { { 0, 0x2000 }, { 0, 0x2000 } };

static void flx10_pitch(int deck, int cc, int val)
{
     struct pitch_ctrl *s = &pitch_state[deck];
     float norm;
     int pos, v10;

     if (cc == CC_TEMPO_MSB) {
          s->have_hi = 1;
          s->pos = (s->pos & 0x7f) | (val << 7);
          return;
     }
     s->pos = (s->pos & 0x3f80) | (val & 0x7f);
     if (!s->have_hi)
          return;
     pos = s->pos;
     norm = ((float)pos - 0x2000) / 8192.0f;
     if (norm > 1.0f) norm = 1.0f;
     if (norm < -1.0f) norm = -1.0f;
     if (tempo_rev)
          norm = -norm;
     v10 = (int)((norm + 1.0f) * 511.5f);
     if (v10 < 0) v10 = 0;
     if (v10 > 1023) v10 = 1023;
     pitch_state_set(deck, norm);
     send_rx_key_fl(K_TEMPO_SLIDER, OP_VALUE, deck + 1, (long)v10, norm, (long)pos);
     if (tempo_verbose)
          klog("knobshim2: flx10 pitch deck%d pos=0x%04x norm=%.3f\n",
               deck + 1, pos, (double)norm);
}

/* MASTER LEVEL sets the unit's own output volume after the USB audio, so it is
 * never sent to rbp (that would attenuate the master twice -- map_flx4.c's
 * argument, unchanged). As on the FLX4 it feeds the HDMI mirror's level, which
 * is off by default on this board. BOOTH LEVEL is the same story with no mirror
 * behind it: the unit applies it to its own booth output, and it is not read. */
static int mastervol_have_hi, mastervol_pos;

static void flx10_mastervol(int cc, int val)
{
     double g, mid = mastervol_mid;

     if (cc == CC_MASTER_MSB) {
          mastervol_have_hi = 1;
          mastervol_pos = (mastervol_pos & 0x7f) | (val << 7);
          return;
     }
     mastervol_pos = (mastervol_pos & 0x3f80) | (val & 0x7f);
     if (!mastervol_have_hi)
          return;
     if (!(mid > 0.0) || mid > 1.0)
          mid = MIRROR_GAIN_MID_DEFAULT;
     g = ((double)mastervol_pos / 16383.0) / mid;
     if (g > 1.0)
          g = 1.0;
     g_mirror_gain = (float)g;
}

/* ---- the loop section ----------------------------------------------------
 * The operator's table (docs/21):
 *
 *   control         not in a loop            in a loop
 *   IN / 1/2X       loop IN                  halve  (rbp's CALL <)
 *   OUT / 2X        loop OUT                 double (rbp's CALL >)
 *   4 BEAT / EXIT   4-beat loop, one press   exit   (RELOOP/EXIT)
 *   SHIFT + IN      loop IN                  IN ADJUST  (IN held)
 *   SHIFT + OUT     RELOOP (last loop)       OUT ADJUST (OUT held)
 *
 * "In a loop" is rbp's own answer at the moment of the PRESS
 * (aloop_is_looping(), PlayEngine::isLooping), and the release goes to whatever
 * key the press went to -- so a loop that ends while the button is held cannot
 * leave an rbp key half-pressed.
 *
 * The 4-beat loop is the RX3's own gesture, "hold IN for about a second", made
 * by pressing rbp's IN and releasing it fourbeat_ms later from tick(). */
static int loop_key[2][3];                   /* [deck][IN, OUT, SHIFT-OUT/IN] */
static unsigned long long fourbeat_until[2]; /* 0 = no simulated hold running */

static void loop_press(int deck, int slot, int key, int sch)
{
     loop_key[deck][slot] = key;
     if (key)
          send_rx_key(key, OP_PRESS, sch, 0);
}

static void loop_release(int deck, int slot, int sch)
{
     int key = loop_key[deck][slot];
     loop_key[deck][slot] = 0;
     if (key)
          send_rx_key(key, OP_RELEASE, sch, 0);
}

static void flx10_loop(int deck, int note, int on)
{
     int sch = deck + 1;
     int looping;

     if (on)
          looping = aloop_is_looping(deck) != 0;
     else
          looping = 0;                       /* unused on a release */

     switch (note) {
     case N_IN:
          if (!on) { loop_release(deck, 0, sch); return; }
          if (looping && !k_callprev) { not_yet("loop halve (CALL <)", sch); return; }
          loop_press(deck, 0, looping ? k_callprev : K_LOOPIN, sch);
          return;
     case N_OUT:
          if (!on) { loop_release(deck, 1, sch); return; }
          if (looping && !k_callnext) { not_yet("loop double (CALL >)", sch); return; }
          loop_press(deck, 1, looping ? k_callnext : K_LOOPOUT, sch);
          return;
     case N_IN_SHIFT:
          if (!on) { loop_release(deck, 2, sch); return; }
          loop_press(deck, 2, K_LOOPIN, sch);
          return;
     case N_OUT_SHIFT:
          if (!on) { loop_release(deck, 2, sch); return; }
          loop_press(deck, 2, looping ? K_LOOPOUT : K_RELOOP, sch);
          return;
     case N_4BEAT:
          if (!on)
               return;                       /* the press is the whole gesture */
          if (looping) {
               pulse(K_RELOOP, sch);         /* exit */
          } else if (!fourbeat_until[deck]) {
               send_rx_key(K_LOOPIN, OP_PRESS, sch, 0);
               fourbeat_until[deck] = shim_now_ms() + (unsigned)fourbeat_ms;
          }
          return;
     default:
          return;
     }
}

static void flx10_fourbeat_tick(unsigned long long t)
{
     for (int d = 0; d < 2; d++) {
          if (fourbeat_until[d] && t >= fourbeat_until[d]) {
               fourbeat_until[d] = 0;
               send_rx_key(K_LOOPIN, OP_RELEASE, d + 1, 0);
               if (verbose)
                    klog("knobshim2: flx10 4 BEAT deck%d: IN held %d ms\n",
                         d + 1, fourbeat_ms);
          }
     }
}

/* ---- which screen rbp is on, and whether slot 1 has a stick --------------
 * rbp's SOURCE menu is browse mode 12, and on that screen rbp deliberately
 * ignores the browse encoder and its push: a real RX3 opens a stick there with
 * its USB1 key (0x0209). map_jp21.c measured both and remaps its push the same
 * way. The mode is the first word of rbp's uiBrowse singleton (rbp_abi.h), one
 * read at a fixed address, which pointsrc.c already does from its own thread.
 *
 * Both are WEAK so the host test can answer for rbp without dereferencing its
 * addresses. */
#define BROWSE_MODE_SOURCE 12

__attribute__((weak)) int flx10_browse_mode(void)
{
     return *(volatile int *)(UIBROWSE_GLOBAL + UIBROWSE_OFF_MODE);
}

__attribute__((weak)) int flx10_usb1_present(void)
{
     return access("/media/usb1/sda1/PIONEER/rekordbox/export.pdb", F_OK) == 0;
}

/* ---- the browse knobs' pushes --------------------------------------------
 * Both knobs TURN with the same message (ch 7 CC 64), so they scroll one list;
 * only the pushes say which deck: 70 = deck 1, 71 = deck 2. A push is LOAD on
 * release, unless it was held browse_hold_ms, which is TAG TRACK -- decided in
 * tick() so the hold fires while the knob is still down. A double push is two
 * LOADs, which the operator chose over an instant-doubles gesture.
 *
 * ON THE SOURCE SCREEN a push opens USB 1 instead (operator, 2026-10-24), when
 * slot 1 has a rekordbox stick: LOAD means nothing there, and rbp ignores the
 * knob on that screen, so without this the stick could only be opened from the
 * glass. A slot-2-only stick is still opened from the glass, as in map_jp21.c:
 * no USB2 keycode has been measured. */
static unsigned long long push_at[2];
static int push_fired[2];

static void flx10_push(int deck, int on)
{
     if (on) {
          push_at[deck] = shim_now_ms();
          push_fired[deck] = 0;
          return;
     }
     if (!push_at[deck])
          return;
     push_at[deck] = 0;
     if (push_fired[deck])
          return;
     if (flx10_browse_mode() == BROWSE_MODE_SOURCE && flx10_usb1_present()) {
          pulse(K_USB1, CH_GLOBAL);
          klog("knobshim2: flx10 browse push on SOURCE -> open USB1 (0x%04x)\n", K_USB1);
          return;
     }
     pulse(K_LOAD, deck + 1);
}

static void flx10_push_tick(unsigned long long t)
{
     for (int d = 0; d < 2; d++) {
          if (!push_at[d] || push_fired[d] ||
              t - push_at[d] < (unsigned long long)browse_hold_ms)
               continue;
          push_fired[d] = 1;
          if (k_tagtrack)
               pulse(k_tagtrack, CH_GLOBAL);
          else
               not_yet("TAG TRACK (browse held)", d + 1);
     }
}

static void flx10_browse(int val, int steps)
{
     int delta = (val >= 64) ? (val - 128) : val;
     int n;

     if (delta == 0)
          return;
     n = abs(delta) * steps * knob_scale;
     if (n > 64)
          n = 64;
     for (int i = 0; i < n; i++)
          send_rx_key(K_SELECTOR, OP_ROTATE, CH_GLOBAL, delta < 0 ? -1 : 1);
}

/* ---- Sound Color FX ------------------------------------------------------
 * Six buttons that behave as a radio group on the unit: selecting one sends the
 * previous one's note-off and then the new one's note-on, all within a
 * millisecond. rbp's six SCFX buttons are toggles that apply to both channels.
 * So a note-on of a different button selects it (on both players, the way
 * map_flx4.c's startup selects FILTER), and a note-off of the CURRENT button
 * that is not followed by a note-on within 50 ms is the unit deselecting it --
 * which is rbp's button pressed again. FLX10 PITCH drives rbp's SWEEP, the one
 * name the two panels do not share. */
static const int scfx_key[6] = {
     K_SPACE, K_DUBECHO, K_CRUSH, K_SWEEP, K_NOISE, K_FILTER
};
static int scfx_cur = -1;
static unsigned long long scfx_off_at;

static void scfx_send(int idx)
{
     pulse(scfx_key[idx], 1);
     pulse(scfx_key[idx], 2);
}

static void flx10_scfx(int idx, int on)
{
     if (on) {
          scfx_off_at = 0;
          if (idx != scfx_cur) {
               scfx_cur = idx;
               scfx_send(idx);
               if (verbose)
                    klog("knobshim2: flx10 SCFX -> %d\n", idx);
          }
          return;
     }
     if (idx == scfx_cur)
          scfx_off_at = shim_now_ms() + 50;
}

static void flx10_scfx_tick(unsigned long long t)
{
     if (scfx_off_at && t >= scfx_off_at) {
          scfx_off_at = 0;
          if (scfx_cur >= 0) {
               scfx_send(scfx_cur);          /* rbp's toggle: off again */
               if (verbose)
                    klog("knobshim2: flx10 SCFX %d off\n", scfx_cur);
               scfx_cur = -1;
          }
     }
}

/* ---- Beat FX -------------------------------------------------------------
 * The effect selector is a 14-position switch sending one note per position
 * (32..45). It is matched to rbp's 14-position switch BY NAME (operator,
 * docs/21); FLX10 effects the RX3 does not have select nothing and log.
 *
 * Note 32 is the fully counter-clockwise end: confirmed on the unit
 * 2026-10-24 (the operator's REVERB / FLANGER / PHASER / ROLL reports matched
 * the FLX10 side of this table exactly).
 *
 *   FLX10 (32 -> 45, CCW -> CW, as printed): LOW CUT ECHO, ECHO, MT DELAY,
 *   SPIRAL, REVERB, TRANS, ENIGMA JET | FLANGER, PHASER, STRETCH, SLIP ROLL,
 *   ROLL, MOBIUS (saw), MOBIUS (triangle)
 *   rbp's switch positions, MEASURED on the Pi (docs/13 S5.9, and fx_zone.c's
 *   names): 0 DELAY 1 ECHO 2 PING PONG 3 SPIRAL 4 HELIX 5 REVERB 6 FLANGER
 *   7 PHASER 8 FILTER 9 TRANS 10 ROLL 11 SLIP ROLL 12 PITCH 13 VINYL BRAKE.
 *   NOT the order printed round the RX3's knob -- the first version of this
 *   table assumed that and sent REVERB as HELIX. */
static const signed char fxsel_to_rbp[14] = {
     -1,  1,  0,  3,  5,  9, -1,     /* LOW CUT ECHO .. ENIGMA JET */
      6,  7, -1, 11, 10, -1, -1,     /* FLANGER .. MOBIUS */
};
static const char *const fxsel_name[14] = {
     "LOW CUT ECHO", "ECHO", "MT DELAY", "SPIRAL", "REVERB", "TRANS",
     "ENIGMA JET", "FLANGER", "PHASER", "STRETCH", "SLIP ROLL", "ROLL",
     "MOBIUS (saw)", "MOBIUS (triangle)",
};

static void flx10_fxsel(int note)
{
     int i = note - N_FX_SEL_FIRST;
     int pos = fxsel_to_rbp[i];

     if (pos < 0) {
          klog("knobshim2: flx10 Beat FX %s: no RX3 equivalent, unchanged\n",
               fxsel_name[i]);
          return;
     }
     send_rx_key(K_BFXTYPE, OP_VALUE, CH_GLOBAL, pos);
     klog("knobshim2: flx10 Beat FX %s -> RX3 position %d\n", fxsel_name[i], pos);
}

/* LEVEL/DEPTH (ch 5 CC 2, MSB of a 14-bit pair): DEPTH, or with SHIFT held the
 * RX3's TIME, which the FLX10 has no knob for. TIME is a relative control in rbp
 * (map_jp21.c's handle_fx_time()), so each MSB step while SHIFT is down is one
 * step of TIME. DEPTH is not sent meanwhile, so letting go of SHIFT and moving
 * the knob again takes DEPTH from wherever the knob now is. */
static int fxlevel_last = -1;

static void flx10_fxlevel(int val)
{
     int prev = fxlevel_last;

     fxlevel_last = val;
     if (any_shift()) {
          if (prev >= 0 && val != prev) {
               long d = (val > prev) ? 1 : -1;
               send_rx_key_fl(K_TIME, OP_ROTATE, CH_GLOBAL, d, 0.0f, d);
          }
          return;
     }
     {
          int v = cc_to_10bit(val);
          send_rx_key_f(K_DEPTH, OP_VALUE, CH_GLOBAL, v, (float)v / 1023.0f);
     }
}

/* ---- notes ---------------------------------------------------------------- */

static void flx10_note(int ch, int note, int on)
{
     int deck = IS_DECK(ch) ? ch : -1;

     if (deck >= 0) {
          int sch = deck + 1;

          switch (note) {
          case N_SHIFT:
               shift_down[deck] = on;
               return;
          case N_CH_CUE:                     /* latching PFL, as on the FLX4 */
               if (on)
                    me_set_cue(ch, me_get_cue(ch) == 0 ? 1 : 0);
               return;
          case N_SYNC: {                     /* the FLX4's release-edge handling */
               static int sync_down[2];
               if (on) {
                    if (sync_down[deck])
                         return;
                    sync_down[deck] = 1;
               } else if (sync_down[deck]) {
                    sync_down[deck] = 0;
                    return;
               }
               pulse(K_SYNC, sch);
               return;
          }
          case N_SYNC_SHIFT:
               if (on)
                    pulse(K_MASTER, sch);
               return;
          case N_KEYSYNC:                    /* KEY SYNC toggles MASTER TEMPO */
               if (on)
                    pulse(K_MT, sch);
               return;
          case N_IN: case N_OUT: case N_4BEAT:
          case N_IN_SHIFT: case N_OUT_SHIFT:
               flx10_loop(deck, note, on);
               return;
          case N_MODE_KEYBOARD:
               if (on) {
                    if (k_gatecue) pulse(k_gatecue, sch);
                    else not_yet("GATE CUE mode", sch);
               }
               return;
          case N_MODE_PADFX2:
               if (on) {
                    if (k_releasefx) pulse(k_releasefx, sch);
                    else not_yet("RELEASE FX mode", sch);
               }
               return;
          case N_MODE_KEYSHIFT:
               if (on) {
                    if (k_beatjump2) pulse(k_beatjump2, sch);
                    else not_yet("BEAT JUMP 2 mode", sch);
               }
               return;
          default:
               break;
          }
     }

     if (ch == CH_MIX) {
          if (note == N_MASTER_CUE) {
               if (on)
                    me_set_master_cue(me_get_master_cue() == 0 ? 1 : 0);
               return;
          }
          if (note == N_LOAD1 || note == N_LOAD2) {
               flx10_push(note - N_LOAD1, on);
               return;
          }
          if (note >= N_SCFX_FIRST && note < N_SCFX_FIRST + 6) {
               flx10_scfx(note - N_SCFX_FIRST, on);
               return;
          }
     }

     if (ch == CH_FX) {
          if (note >= N_FX_SEL_FIRST && note <= N_FX_SEL_LAST) {
               if (on)
                    flx10_fxsel(note);
               return;
          }
          if (note >= N_FX_CH_FIRST && note <= N_FX_CH_LAST) {
               /* Which note is which label (3/1/2/MIC/SP/MST) is still to be read
                * off the panel, so the position is logged and nothing is sent. */
               if (on)
                    klog("knobshim2: flx10 Beat FX channel selector note %d "
                         "(label not yet mapped)\n", note);
               return;
          }
          if (note == N_FX_BEATPREV || note == N_FX_BEATNEXT) {
               if (on) {
                    long d = (note == N_FX_BEATPREV) ? -1 : 1;
                    send_rx_key_fl(d < 0 ? K_BEATPREV : K_BEATNEXT, OP_PRESS,
                                   CH_GLOBAL, d, 0.0f, d);
               }
               return;
          }
     }

     /* SHIFT + a HOT CUE pad: delete that cue (map_flx4.c's gesture). */
     if ((ch == CH_PADS1S || ch == CH_PADS2S) && note >= 0 && note < 8) {
          if (on)
               hotcue_delete(ch == CH_PADS1S ? 0 : 1, note + 1);
          return;
     }

     /* Everything else is a table row. */
     for (int i = 0; i < note_map_n; i++) {
          int key, *p;

          if (note_map[i].rch != ch || note_map[i].note != note)
               continue;
          key = note_map[i].key;
          if (key == 0) {
               if (verbose)
                    klog("knobshim2: flx10 ch%d note%d (log-only)\n", ch + 1, note);
               return;
          }
          p = &note_map[i].pressed;
          if (on && !*p) {
               *p = 1;
               send_rx_key(key, OP_PRESS, note_map[i].sch, 0);
               if (note_map[i].repeat)
                    send_rx_key(key, OP_REPEAT, note_map[i].sch, 0);
          } else if (!on && *p) {
               *p = 0;
               send_rx_key(key, OP_RELEASE, note_map[i].sch, 0);
          }
          return;
     }
     if (verbose)
          klog("knobshim2: flx10 unmapped ch%d note%d on=%d\n", ch + 1, note, on);
}

/* ---- CCs ------------------------------------------------------------------ */

/* The knobs and faders are 14-bit pairs (CC n + CC n+32); like map_flx4.c, the
 * MSB alone drives rbp's 10-bit value and the LSB is dropped here. */
static int is_lsb_only(int ch, int cc)
{
     if (IS_DECK(ch))
          return cc == CC_TRIM + 32 || cc == CC_EQH + 32 || cc == CC_EQM + 32 ||
                 cc == CC_EQL + 32 || cc == CC_FADER + 32;
     if (ch == CH_MIX)
          return cc == CC_CFX1 + 32 || cc == CC_CFX2 + 32 || cc == CC_XFADER + 32 ||
                 cc == CC_HP_MIX + 32 || cc == CC_HP_LEVEL + 32 ||
                 cc == CC_BOOTH_MSB + 32 || cc == CC_SAMPLER_VOL + 32;
     if (ch == CH_FX)
          return cc == CC_FX_LEVEL + 32;
     return 0;
}

static void flx10_cc_abs(int ch, int cc, int val)
{
     for (int i = 0; i < abs_map_n; i++) {
          int v, key, sch;
          float fval;

          if (abs_map[i].rch != ch || abs_map[i].cc != cc)
               continue;
          v = cc_to_10bit(val);
          if (v == abs_map[i].last)
               return;
          abs_map[i].last = v;
          key = abs_map[i].key;
          sch = abs_map[i].sch;
          fval = (float)v / 1023.0f;
          if (key == K_FADER && sch >= 1 && sch <= 2) {
               g_fader[sch] = v;
               g_fader_seen[sch] = 1;
          } else if (key == K_HPMIX) {
               g_cue_mix = fval;
          } else if (key == K_HPLEVEL) {
               g_cue_gain = fval;
          }
          send_rx_key_f(key, OP_VALUE, sch, v, fval);
          return;
     }
     if (verbose)
          klog("knobshim2: flx10 unmapped ch%d cc%d val=%d\n", ch + 1, cc, val);
}

static void flx10_cc(int ch, int cc, int val)
{
     if (is_lsb_only(ch, cc))
          return;
     if (IS_DECK(ch)) {
          if (cc == CC_TEMPO_MSB || cc == CC_TEMPO_LSB) {
               flx10_pitch(ch, cc, val);
               return;
          }
          if (cc == CC_JOG_SIDE || cc == CC_JOG_VINYL || cc == CC_JOG_NOVINYL ||
              cc == CC_JOG_SHIFT || cc == CC_JOG_SIDE_SH) {
               flx10_jog(ch, val);
               return;
          }
     } else if (ch == CH_MIX) {
          if (cc == CC_BROWSE) {
               flx10_browse(val, 1);
               return;
          }
          if (cc == CC_BROWSE_SHIFT) {
               flx10_browse(val, browse_shift_steps);
               return;
          }
          if (cc == CC_MASTER_MSB || cc == CC_MASTER_LSB) {
               flx10_mastervol(cc, val);
               return;
          }
          if (cc == CC_BOOTH_MSB)
               return;                       /* the unit's own booth gain */
          if (cc == CC_SAMPLER_VOL) {
               if (k_param) {
                    int v = cc_to_10bit(val);
                    send_rx_key_f(k_param, OP_VALUE, CH_GLOBAL, v, (float)v / 1023.0f);
               } else if (verbose) {
                    not_yet("SCFX PARAMETER (SAMPLER VOL)", 0);
               }
               return;
          }
     } else if (ch == CH_FX && cc == CC_FX_LEVEL) {
          flx10_fxlevel(val);
          return;
     }
     flx10_cc_abs(ch, cc, val);
}

/* ---- the tables ----------------------------------------------------------- */

static void flx10_build(void)
{
     /* Additive, like every map: see ctrl_bindings_reset() in ctrl_map.h. */
     knob_scale = env_num("KNOB_SCALE", 1);
     if (knob_scale < 1) knob_scale = 1;
     jog_scale = env_num("JOG_SCALE", 1);
     if (jog_scale < 1) jog_scale = 1;
     jog_ppr = env_num("JOG_PPR", 720);
     if (jog_ppr < 1) jog_ppr = 1;
     jog_rev = env_on("JOG_REV", 0);
     jog_idle_ms = env_num("JOG_IDLE_MS", 120);
     if (jog_idle_ms < 10) jog_idle_ms = 10;
     jog_verbose = env_on("JOG_VERBOSE", 0);
     tempo_verbose = env_on("TEMPO_VERBOSE", 0);
     tempo_rev = env_on("TEMPO_REV", 0);
     mastervol_mid = env_dnum("MIRROR_GAIN_MID", MIRROR_GAIN_MID_DEFAULT);
     flx10_keys_parse();

     for (int d = 0; d < 2; d++) {
          int rch = CH_DECK1 + d;
          int sch = 1 + d;

          add_note(rch, N_PLAY, K_PLAY, sch);
          add_note(rch, N_CUE, K_CUE, sch);
          add_note(rch, N_JOG_TOUCH, K_JOG_TOUCH, sch);
          add_note(rch, N_JOG_TOUCH_SH, K_JOG_TOUCH, sch);   /* SHIFT-held press */
          add_note(rch, N_TEMPO_RANGE, K_TEMPO_RANGE, sch);
          add_note(rch, N_BJ_PREV, K_SRREV, sch);
          add_note(rch, N_BJ_NEXT, K_SRFWD, sch);
          add_note(rch, N_BJ_PREV_SHIFT, K_TRREV, sch);
          add_note(rch, N_BJ_NEXT_SHIFT, K_TRFWD, sch);
          add_note(rch, N_SLIP_REVERSE, K_REV, sch);
          add_note(rch, N_REVERSE, K_REV, sch);
          add_note(rch, N_QUANTIZE, K_QUANTIZE, sch);
          add_note(rch, N_SLIP, K_SLIP, sch);
          add_note(rch, N_VINYL, K_VINYL, sch);
          /* Pad modes, as close to the FLX10's labels as the RX3's modes come
           * (operator, docs/21). The SHIFT modes (KEYBOARD -> GATE CUE, PAD FX2
           * -> RELEASE FX, KEY SHIFT -> BEAT JUMP 2) are named cases in
           * flx10_note() because their keycodes come from FLX10_KEYS. */
          add_note(rch, N_MODE_HOT, K_HOTCUE, sch);
          add_note(rch, N_MODE_PADFX1, K_SLIPLOOP, sch);
          add_note(rch, N_MODE_BEATJUMP, K_BEATJUMP, sch);
          add_note(rch, N_MODE_BEATLOOP, K_ALOOP, sch);
          /* Known and deliberately unbound (key 0): logged, nothing sent. */
          add_note(rch, N_MODE_SAMPLER, 0, sch);
          add_note(rch, N_TEMPO_RESET, 0, sch);
          add_note(rch, N_KEYSYNC_LONG, 0, sch);
          add_note(rch, N_SYNC_LONG, 0, sch);
          add_note(rch, N_4BEAT_SHIFT, 0, sch);

          add_abs(rch, CC_TRIM, K_TRIM, sch);
          add_abs(rch, CC_EQH, K_EQH, sch);
          add_abs(rch, CC_EQM, K_EQM, sch);
          add_abs(rch, CC_EQL, K_EQL, sch);
          add_abs(rch, CC_FADER, K_FADER, sch);
     }

     /* Pads: note = mode base + pad (0..7), the base moving with the unit's own
      * pad mode: 0 HOT CUE, 16 PAD FX1, 32 BEAT JUMP, 48 SAMPLER, 64 KEYBOARD,
      * 80 PAD FX2, 96 BEAT LOOP, 112 KEY SHIFT (0/16/32/48/64/96 measured; 80 and
      * 112 follow the stride). rbp's K_PAD1..8 act in rbp's current mode, so a
      * base is bound only where the map puts rbp in a mode for it -- SAMPLER
      * never, and the three SHIFT modes only once their key is configured. */
     for (int d = 0; d < 2; d++) {
          int rch = d ? CH_PADS2 : CH_PADS1;
          int sch = 1 + d;
          for (int p = 0; p < 8; p++) {
               add_note(rch,  0 + p, K_PAD1 + p, sch);  /* HOT CUE   -> HOT CUE   */
               add_note(rch, 16 + p, K_PAD1 + p, sch);  /* PAD FX1   -> SLIP LOOP */
               add_note(rch, 32 + p, K_PAD1 + p, sch);  /* BEAT JUMP -> BEAT JUMP */
               add_note(rch, 96 + p, K_PAD1 + p, sch);  /* BEAT LOOP -> BEAT LOOP */
               if (k_gatecue)
                    add_note(rch, 64 + p, K_PAD1 + p, sch);   /* -> GATE CUE    */
               if (k_releasefx)
                    add_note(rch, 80 + p, K_PAD1 + p, sch);   /* -> RELEASE FX  */
               if (k_beatjump2)
                    add_note(rch, 112 + p, K_PAD1 + p, sch);  /* -> BEAT JUMP 2 */
          }
     }

     /* Mixer channel. */
     add_abs(CH_MIX, CC_CFX1, K_COLOR, 1);
     add_abs(CH_MIX, CC_CFX2, K_COLOR, 2);
     add_abs(CH_MIX, CC_XFADER, K_XFADER, CH_GLOBAL);
     add_abs(CH_MIX, CC_HP_MIX, K_HPMIX, CH_GLOBAL);
     add_abs(CH_MIX, CC_HP_LEVEL, K_HPLEVEL, CH_GLOBAL);
     add_note(CH_MIX, N_BACK, K_BACK, CH_GLOBAL);
     add_note(CH_MIX, N_BACK_SHIFT, K_TAGLIST, CH_GLOBAL);
     /* VIEW opens SOURCE (operator, 2026-10-24): the FLX10 has no SOURCE button,
      * and without one the USB stick is unreachable from the surface. SHIFT +
      * VIEW is SHORTCUT. */
     add_note(CH_MIX, N_VIEW, K_SOURCE, CH_GLOBAL);
     add_note(CH_MIX, N_VIEW_SHIFT, K_SHORTCUT, CH_GLOBAL);
     /* SHIFT + browse push: TRACK FILTER. It needs the second edge --
      * map_flx4.c's add_note_repeat() note and rbp_abi.h's OP_REPEAT explain
      * why. 93 is deck 1's knob (measured); deck 2's SHIFT push is unrecorded
      * and logs as unmapped until it is. */
     add_note_repeat(CH_MIX, N_PUSH1_SHIFT, K_TRACKFILTER, CH_GLOBAL);
     add_note(CH_MIX, N_SAMPLER_CUE, 0, CH_GLOBAL);

     /* Beat FX. */
     add_note(CH_FX, N_FX_ONOFF, K_BFX, CH_GLOBAL);
     add_note(CH_FX, N_FX_TAP, K_TAP, CH_GLOBAL);
     add_note(CH_FX, N_FX_AUTO, 0, CH_GLOBAL);
     add_note(CH_FX, N_FX_RELEASE, 0, CH_GLOBAL);
}

static void flx10_startup(void)
{
     /* Mixer channel 1 -> deck 1, channel 2 -> deck 2, and rbp's master level at
      * unity: both for the reasons map_flx4.c's startup gives. Sound Color FX is
      * NOT forced to a type here (the FLX4 forces FILTER because it has no SCFX
      * buttons); the FLX10's buttons select one. */
     *(volatile uint32_t *)ADDR_MIXER_ROUTE_PLAYER0 = MIXER_ROUTE_INPUT0;
     *(volatile uint32_t *)ADDR_MIXER_ROUTE_PLAYER1 = MIXER_ROUTE_INPUT1;
     send_rx_key_f(K_MASTERLVL, OP_VALUE, CH_GLOBAL, 1023, 1.0f);
     klog("knobshim2: DDJ-FLX10: routed Ch1->Deck1, Ch2->Deck2; master level unity\n");
}

static void flx10_tick(void)
{
     unsigned long long t = shim_now_ms();

     flx10_jog_idle(t);
     flx10_fourbeat_tick(t);
     flx10_push_tick(t);
     flx10_scfx_tick(t);
}

static void flx10_event(const struct snd_seq_event *ev)
{
     switch (ev->type) {
     case SNDRV_SEQ_EVENT_CONTROLLER:
          flx10_cc(ev->data.control.channel, ev->data.control.param,
                   ev->data.control.value);
          break;
     case SNDRV_SEQ_EVENT_NOTEON:
          flx10_note(ev->data.note.channel, ev->data.note.note,
                     ev->data.note.velocity > 0);
          break;
     case SNDRV_SEQ_EVENT_NOTEOFF:
          flx10_note(ev->data.note.channel, ev->data.note.note, 0);
          break;
     default:
          break;
     }
}

/* No LED table yet: NULL sends nothing (ctrl_map.h). The FLX4's LEDs turned out
 * to use each control's own input note, which is the first thing to try here. */
const struct ctrl_map map_flx10 = {
     "flx10",
     flx10_build,
     flx10_startup,
     flx10_event,
     flx10_tick,
     NULL,
     NULL,
     NULL,
};
