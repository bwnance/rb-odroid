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
 * THE RX3 KEYCODES THIS MAP NEEDED BEYOND rbp_abi.h were found in the player
 * binary (objdump; derivations below and in docs/21) and are built in. Each can
 * still be overridden from the environment (FLX10_KEYS, flx10_keys_parse()), so
 * a correction is a line in rb.local.conf and a restart, not a rebuild:
 *
 *   RB_FLX10_KEYS="param=0x50a7"
 *
 *   callprev, callnext  rbp's CUE/LOOP CALL < / >: IN/OUT inside a loop send
 *                       these, which is how the RX3 halves and doubles a loop.
 *                       FOUND, and built in (0x4323 / 0x4322): ui::Player::
 *                       asEventCode @0x2f16d4 turns 0x4322 into event 32 and
 *                       0x4323 into event 33, which PlayerSkeleton::onKeyEvent
 *                       dispatches (PRESS only) to PlayerInnards vtable +0xf0
 *                       onEv_CallNext and +0xf4 onEv_CallPrev; both call
 *                       DjEngineIF::setLoopCutterDouble when looping. Confirmed
 *                       on the unit 2026-10-24: halve and double both work.
 *                       The names still override them.
 *   param               the Sound Color FX PARAMETER knob (from SAMPLER VOL):
 *                       0x50a7. ui::Mixer::asEventCode @0x2cf898 maps it to
 *                       event 0x100e, which MixerSkeleton::onKeyEvent sends to
 *                       MixerChInnards vtable +0x40, onEv_ColorFxParameter
 *                       (float) -- the slot after +0x3c onEv_ColorFxVolume,
 *                       which is where COLOR (0x509d, event 0x100b) lands.
 *                       Per mixer channel, so it is sent to channels 1 and 2.
 *
 * GATE CUE, RELEASE FX and BEAT JUMP 2 NEED NO KEYCODE: on the RX3 they are a
 * second press of the same pad-mode button (see flx10_set_mode()).
 */
#define _GNU_SOURCE
#include <fcntl.h>          /* open(), for rbp's mount table */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>         /* access() */
#include <time.h>           /* clock_gettime() */
#include <sound/asequencer.h>

#include "shimutil.h"
#include "shmstate.h"       /* g_cue_mix / g_cue_gain / g_mirror_gain */
#include "rbp_abi.h"
#include "rbp_bridge.h"
#include "rbp_vu.h"         /* g_fader / g_fader_seen */
#include "pitch_state.h"
#include "ctrl_map.h"
#include "rbp_led.h"        /* led_loop_flash */
#include "midi_io.h"        /* midi_cc / midi_note, for the jog displays */
#include "flx10_hid.h"      /* the jog screens' per-deck HID state */
#include "settings_remote.h" /* the SETTINGS panel, from SHIFT + VIEW held */

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
#define N_CUE_SHIFT     72    /* SHIFT + CUE: back to the track's start */
#define N_MIXPOINT_PREV 89    /* MIX POINT SELECT <: previous track */
#define N_MIXPOINT_NEXT 90    /* MIX POINT SELECT >: next track */
#define N_XF_A          22    /* crossfader assign switch at A (0x7F) */
#define N_XF_B          24    /*   ... at B */
#define N_XF_THRU       29    /*   ... at THRU (Pioneer's MIDI list, M11) */
#define N_SHIFT         63
#define N_SYNC          88    /* sent when the finger LEAVES (as on the FLX4) */
#define N_SYNC_SHIFT    92    /* SHIFT + BEAT SYNC = MASTER */
#define N_SYNC_LONG    120    /* long press (SYNC RATE): nothing on the RX3 */
#define N_KEYSYNC      101    /* KEY SYNC, short press */
#define N_KEYSYNC_LONG  26    /* KEY SYNC held: MASTER TEMPO */
#define N_TEMPO_RESET   65
#define N_TEMPO_RANGE   96    /* SHIFT + TEMPO RESET */
#define N_IN            16    /* IN / 1/2X */
#define N_OUT           17    /* OUT / 2X */
#define N_4BEAT         20    /* 4 BEAT / EXIT */
#define N_IN_SHIFT      76    /* SHIFT + IN (IN ADJUST) */
#define N_OUT_SHIFT     77    /* SHIFT + OUT (OUT ADJUST / RELOOP) */
#define N_4BEAT_SHIFT   80    /* SHIFT + 4 BEAT (ACTIVE): elapsed / remaining */
#define N_BJ_PREV       94    /* 4 BEAT JUMP <: 4 beats back */
#define N_BJ_NEXT       95
#define N_BJ_PREV_SHIFT 97    /* SHIFT: 16 beats back */
#define N_BJ_NEXT_SHIFT 98
#define N_SLIP_REVERSE  21
#define N_REVERSE       56    /* SHIFT + SLIP REVERSE */
#define N_QUANTIZE      53
#define N_SLIP          64
#define N_VINYL         23    /* SHIFT + SLIP: VINYL, disabled */
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
#define N_FX_AUTO      102    /* SHIFT + BEAT <: BPM back to AUTO */
#define N_FX_TAP       107    /* SHIFT + BEAT > */
/* 70, not the FLX4's 71 the map first copied: the operator's rekordbox capture of
 * 2026-10-26 has the button as 94 46 and rekordbox lighting it with 94 46 7f/00, and
 * with 71 rbp's Beat FX never came on (operator: "i can't hear FX"). */
#define N_FX_ONOFF      70
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
/* MEASURED on the unit 2026-10-24: five hand-counted turns of deck 1's platter
 * top gave 65416 net counts in 7103 messages, 13083 per revolution -- about 18x
 * the FLX4's 720. Five turns by hand are good to roughly a tenth of a turn
 * (+-2%, so 12820..13350), and no design value is published. The default is
 * 12960: 360 x 36 (36 counts per degree) and exactly 18 x the FLX4's measured
 * 720, the one value in that range with that structure (0.95% below the
 * measurement, i.e. about 5.05 turns counted as 5). An inference, not a finding
 * (operator's choice, 2026-10-24). Since 2026-10-25 the count no longer
 * enters the scaling (rekordbox's ms per count does, below); kept as the
 * record, the capture agreeing: 1.8 s a turn at ~12950 counts. */
#define FLX10_JOG_PPR 12960
#define JOG_WINDOW_US 4000           /* one jog update per window at most */
#define JOG_POS_PPR   720            /* the resolution rbp's position is fed at */
/* How far a jog count moves the track: one platter turn = one turn of the
 * jog screen's arm = 1.8 s of track (a record at 33 1/3), for both controls.
 * Counts per turn measured on the unit with JOG_CAL over ten taped turns
 * (2026-10-25): the top 129410 / 10 = 12941 (0.1391 ms a count -- rekordbox's
 * own, from the USB capture, is 0.1390), the side ring 125437 / 10 = 12544
 * (0.1435; rekordbox moves the side ring 0.1650 a count, ~2.07 s a turn, which
 * runs ahead of the arm).
 * rbp takes a position in 1/720 turns of ITS jog, and one of its turns moves
 * the track RBP_TURN_MS: 1000 ms, measured with JOG_CAL on the unit
 * (2026-10-25: 1000.3 / 1000.0 / 1002.3 / 999.1 over four three-turn
 * gestures, top and side, both ways). So a count is ms_per_count * 720 / RBP_TURN_MS rbp units,
 * applied to the position and the speed alike. JOG_RBP_TURN_MS sets rbp's
 * turn, JOG_GAIN multiplies both controls (default 1). */
#define FLX10_MS_PER_COUNT_TOP  0.1391
#define FLX10_MS_PER_COUNT_SIDE 0.1435
#define FLX10_RBP_TURN_MS       1000.0
#define FLX10_JOG_GAIN          1.0
static double jog_rbp_turn_ms = FLX10_RBP_TURN_MS;
static double jog_gain = FLX10_JOG_GAIN;
static int jog_rev = 0;
static int jog_idle_ms = 120;
static int jog_verbose = 0;
static int tempo_verbose = 0;
static int tempo_rev = 0;
static int browse_hold_ms = 500;     /* a browse push held this long = rbp's
                                      * own long press (fast scroll) */
static double mastervol_mid = MIRROR_GAIN_MID_DEFAULT;

static int k_callprev = 0x4323, k_callnext = 0x4322;   /* see the header */
static int k_param = 0x50a7;                            /* see the header */

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
          else if (!strcmp(tok, "param"))     slot = &k_param;
          if (!slot && (!strcmp(tok, "gatecue") || !strcmp(tok, "releasefx") ||
                        !strcmp(tok, "beatjump2"))) {
               klog("knobshim2: FLX10_KEYS: '%s' is no longer a keycode (the SHIFT "
                    "pad modes are reached through the mode keys); skipped\n", tok);
               continue;
          }
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

/* ---- INST.DOUBLES ----------------------------------------------------------
 * The other deck's track on `deck`, from the same point (the RX3 manual p. 74:
 * BEAT SYNC/INST.DOUBLES held, or LOAD pressed twice). Done as rbp's own BEAT
 * SYNC held past its long press (1000 ms, ui::KeyInput::getLongPressTime): down
 * now, up from flx10_sync_tick. Watched on the unit 2026-10-26: SYNC held 1.6 s
 * put deck 1 on deck 2's track at its position. Two LOAD keys in a row did
 * nothing there, so both FLX10 gestures come here. */
#define SYNC_LONG_HOLD_MS 1100
/* rbp starts the copy playing whatever the other deck is doing; a copy of a
 * paused deck is paused again, once, if it starts within this of the hold's
 * end (operator, 2026-10-26: the copy should not play when the original is
 * paused) */
#define DOUBLES_PAUSE_MS  3000
static unsigned long long sync_up_at[2], doubles_pause_until[2];
static int doubles_src_paused[2], doubles_place[2];
/* the FLX10 still sends its BEAT SYNC tap (88) when a held one (120) is let
 * go -- 0.4 s after the hold, in the key log on the unit 2026-10-26 -- which
 * toggled SYNC right after every copy: that one is dropped */
#define SYNC_LONG_TAP_MS  10000
static unsigned long long sync_long_at[2];
static int deck_playing[2], deck_loaded[2];   /* (deck_display's, below) */
int flx10_doubles_place(int deck);        /* (below, with the other seeks) */

static void flx10_inst_doubles(int deck, const char *why)
{
     if (sync_up_at[deck])
          return;                         /* one already held */
     send_rx_key(K_SYNC, OP_PRESS, deck + 1, 0);
     sync_up_at[deck] = shim_now_ms() + SYNC_LONG_HOLD_MS;
     doubles_src_paused[deck] = deck_loaded[1 - deck] && !deck_playing[1 - deck];
     klog("knobshim2: flx10 deck%d %s: INST.DOUBLES (deck%d %s)\n", deck + 1, why,
          2 - deck, doubles_src_paused[deck] ? "paused" : "playing");
}

static void flx10_sync_tick(unsigned long long t)
{
     for (int d = 0; d < 2; d++) {
          if (sync_up_at[d] && t >= sync_up_at[d]) {
               sync_up_at[d] = 0;
               send_rx_key(K_SYNC, OP_RELEASE, d + 1, 0);
               doubles_pause_until[d] = doubles_src_paused[d] ? t + DOUBLES_PAUSE_MS : 0;
          }
          if (doubles_pause_until[d]) {
               if (t >= doubles_pause_until[d]) {
                    doubles_pause_until[d] = 0;
                    doubles_place[d] = 0;
               } else if (!doubles_place[d] && deck_playing[d]) {
                    pulse(K_PLAY, d + 1);
                    doubles_place[d] = 1;
                    klog("knobshim2: flx10 deck%d: a copy of a paused deck, paused\n", d + 1);
               } else if (doubles_place[d] && !deck_playing[d]) {
                    /* rbp loads a paused deck's copy from the very start, not
                     * its position (operator, 2026-10-26: "the play position is
                     * not copied", nor auto cue): put where the original is */
                    doubles_pause_until[d] = 0;
                    doubles_place[d] = 0;
                    flx10_doubles_place(d);
               }
          }
     }
}

/* ---- LINK: the USB stick's eject ------------------------------------------
 * Each deck's LINK button (note 74, otherwise unused here) stands for that
 * slot's stick, deck 1 for USB1 and deck 2 for USB2 (operator, 2026-10-26):
 *   light    on while rbp has the stick mounted and ready
 *   press    a slow blink for LINK_SLOW_MS (its dark half first, so the press
 *            shows), then a fast one for the rest of LINK_HOLD_MS -- a count-in
 *   held     LINK_HOLD_MS: rbp's USB STOP for that slot -- the settings
 *            panel's eject, the same three keys; still blinking fast, held or not
 *   off      once the stick is gone from rbp's mount table: safe to pull
 * Let go before LINK_HOLD_MS and nothing is ejected: the light is steady again.
 * A press with no stick in the slot does nothing. An eject that has not
 * unmounted after LINK_EJECT_MAX_MS stops blinking and says so. (The unmount
 * itself takes ~250 ms on the unit, which is why the blink starts on the press:
 * started at the eject it was over before its first dark half.) The rbp reads
 * are WEAK for the host test. */
#define N_LINK            74
#define LINK_HOLD_MS      2000
#define LINK_SLOW_MS      1000      /* the first part of the hold blinks slowly... */
#define LINK_BLINK_MS     250       /* ...in halves this long */
#define LINK_FAST_MS      125       /* the rest, and the eject, blink twice as fast */
#define LINK_EJECT_MAX_MS 20000
static int link_held[2], link_fired[2], link_ejecting[2], link_armed[2];
static int link_led_v[2] = { -1, -1 };
static unsigned long long link_at[2], link_eject_at[2];

/* rbp's own answer for slot 0/1: its UsbStorageManager says media present and
 * ready -- the walk pointsrc.c's usb_slots_live() makes for the settings panel,
 * every pointer guarded (rbp allocates them while it starts). */
__attribute__((weak)) int flx10_usb_ready(int slot)
{
     unsigned long objmgr, array, mgr;
     int count;

     objmgr = *(volatile unsigned long *)(uintptr_t)UI_OBJ_MGR_HOLDER;
     if (!objmgr)
          return 0;
     count = *(volatile int *)(uintptr_t)(objmgr + UIOBJMGR_OFF_MGRNUM);
     array = *(volatile unsigned long *)(uintptr_t)(objmgr + UIOBJMGR_OFF_MGRARR);
     if (!array || count < slot + 1 || count > 2)
          return 0;
     mgr = ((volatile unsigned long *)(uintptr_t)array)[slot];
     if (!mgr || *(volatile int *)(uintptr_t)(mgr + USB_MGR_OFF_CHANNEL) != slot + 1)
          return 0;
     return *(volatile int *)(uintptr_t)(mgr + USB_MGR_OFF_MEDIA) == USB_MGR_MEDIA_READY;
}

/* Something of slot 0/1 is still mounted in rbp's view (/media/usbN/...). */
__attribute__((weak)) int flx10_usb_mounted(int slot)
{
     char buf[8192], want[24];
     int fd = open("/proc/self/mounts", O_RDONLY), n, tot = 0;

     if (fd < 0)
          return 0;
     while (tot < (int)sizeof buf - 1 &&
            (n = (int)read(fd, buf + tot, sizeof buf - 1 - tot)) > 0)
          tot += n;
     close(fd);
     buf[tot] = 0;
     snprintf(want, sizeof want, " /media/usb%d/", slot + 1);
     return strstr(buf, want) != NULL;
}

/* rbp's USB STOP for slot 0/1, on the slot's own channel: press, REPEAT (the edge
 * that reaches the eject) and release -- rbp_abi.h's K_USBSTOP. */
__attribute__((weak)) void flx10_usb_eject(int slot)
{
     send_rx_key(K_USBSTOP, OP_PRESS, slot + 1, 0);
     send_rx_key(K_USBSTOP, OP_REPEAT, slot + 1, 0);
     send_rx_key(K_USBSTOP, OP_RELEASE, slot + 1, 0);
}

static void flx10_link_tick(unsigned long long t)
{
     for (int d = 0; d < 2; d++) {
          int ready = flx10_usb_ready(d), v;

          /* a press on a ready slot arms (blinking); let go early and it disarms */
          if (link_held[d] && !link_fired[d] && !link_armed[d] && !link_ejecting[d]) {
               link_fired[d] = 1;               /* one decision per press */
               if (ready)
                    link_armed[d] = 1;
               else
                    klog("knobshim2: flx10 deck%d LINK: no stick ready in USB%d\n",
                         d + 1, d + 1);
          }
          if (link_armed[d] && !link_held[d]) {
               link_armed[d] = 0;
               klog("knobshim2: flx10 deck%d LINK let go after %llu ms: no eject\n",
                    d + 1, t - link_at[d]);
          }
          if (link_armed[d] && t - link_at[d] >= LINK_HOLD_MS) {
               link_armed[d] = 0;
               flx10_usb_eject(d);
               link_ejecting[d] = 1;
               link_eject_at[d] = t;
               klog("knobshim2: flx10 deck%d LINK held: ejecting USB%d\n", d + 1, d + 1);
          }
          if (link_ejecting[d]) {
               if (!ready && !flx10_usb_mounted(d)) {
                    link_ejecting[d] = 0;
                    klog("knobshim2: flx10 USB%d ejected (%llu ms)\n", d + 1,
                         t - link_eject_at[d]);
               } else if (t - link_eject_at[d] > LINK_EJECT_MAX_MS) {
                    link_ejecting[d] = 0;
                    klog("knobshim2: flx10 USB%d still %s %d ms after the eject\n",
                         d + 1, ready ? "ready" : "mounted", LINK_EJECT_MAX_MS);
               }
          }
          if (link_armed[d] || link_ejecting[d]) {
               unsigned long long e = t - link_at[d];
               v = (e < LINK_SLOW_MS ? (e / LINK_BLINK_MS) & 1
                                     : ((e - LINK_SLOW_MS) / LINK_FAST_MS) & 1) ? 0x7F : 0x00;
          } else {
               v = ready ? 0x7F : 0x00;
          }
          if (v != link_led_v[d] && midi_note(d, N_LINK, v))
               link_led_v[d] = v;
     }
}

/* ---- SHIFT + VIEW held: rbp's UTILITY ------------------------------------
 * rbp's own UTILITY screen, which now carries the shim's HDMI AUDIO row
 * (utility_rows.c), and replaces the shim's drawn SETTINGS panel on this unit
 * (operator, 2026-10-26: "get to UTILITY via shift and long press of view"):
 *   SHIFT + VIEW, held PANEL_HOLD_MS   MENU held UTILITY_MENU_MS -- rbp's own
 *                                      long press of MENU, which opens UTILITY
 *                                      (docs/08: 400 ms and up). A shorter press
 *                                      is rbp's SHORTCUT, sent on the release.
 * In UTILITY the browse knobs, their push and BACK are rbp's own.
 *
 * The shim's panel (settings_remote.h) is still worked from here while it is up
 * -- the touch gear on a unit that has one raises it:
 *   while it is up:
 *     either browse knob              select a row
 *     either browse knob's push       press the row's value -- held, for the USB
 *                                     rows' three-second eject
 *     BACK                            put it away
 * A push or a BACK that went to the panel takes its release there too. The
 * panel lives in fbshim.so; these are WEAK so a build without it has none. */
#define PANEL_HOLD_MS 1000
#define UTILITY_MENU_MS 500                /* the touch band's MZ_HOLD_KEY_MS */
static unsigned long long menu_up_at;      /* the held MENU's release, 0 = none */
__attribute__((weak, visibility("default"))) void shim_settings_remote(int cmd);
__attribute__((weak, visibility("default"))) int shim_settings_remote_open(void);
static int view_held, view_long;
static unsigned long long view_at;
static int panel_owns[128];               /* a mixer-channel note's press went to the panel */

static int panel_open(void)
{
     return shim_settings_remote_open && shim_settings_remote_open();
}

static void panel_cmd(int cmd)
{
     if (shim_settings_remote)
          shim_settings_remote(cmd);
}

/* A mixer-channel note for the panel: SHIFT + VIEW always (its length decides),
 * the browse pushes and BACK while the panel is up. 1 = taken. */
static int flx10_panel_note(int note, int on)
{
     if (note == N_VIEW_SHIFT) {
          if (on) {
               view_held = 1;
               view_long = 0;
               view_at = shim_now_ms();
          } else if (view_held) {
               view_held = 0;
               if (!view_long)
                    pulse(K_SHORTCUT, CH_GLOBAL);
          }
          return 1;
     }
     if (note < 0 || note > 127)
          return 0;
     if (!on && panel_owns[note]) {
          panel_owns[note] = 0;
          if (note == N_LOAD1 || note == N_LOAD2)
               panel_cmd(SR_RELEASE);
          return 1;
     }
     if (!on || !panel_open())
          return 0;
     if (note == N_LOAD1 || note == N_LOAD2) {
          panel_owns[note] = 1;
          panel_cmd(SR_PRESS);
          return 1;
     }
     if (note == N_BACK) {
          panel_owns[note] = 1;
          panel_cmd(SR_CLOSE);
          return 1;
     }
     return 0;
}

static void flx10_panel_tick(unsigned long long t)
{
     if (view_held && !view_long && t - view_at >= PANEL_HOLD_MS) {
          view_long = 1;
          if (panel_open())
               panel_cmd(SR_CLOSE);      /* one screen at a time */
          send_rx_key(K_MENU, OP_PRESS, CH_GLOBAL, 0);
          menu_up_at = t + UTILITY_MENU_MS;
          klog("knobshim2: flx10 SHIFT + VIEW held: MENU held %d ms -> UTILITY\n",
               UTILITY_MENU_MS);
     }
     if (menu_up_at && t >= menu_up_at) {
          menu_up_at = 0;
          send_rx_key(K_MENU, OP_RELEASE, CH_GLOBAL, 0);
     }
}

/* ---- SHIFT ---------------------------------------------------------------
 * The FLX10 reports SHIFT itself (note 63 on the deck channel) and also sends
 * distinct notes for most SHIFT combinations, so only the controls whose SHIFT
 * layer is NOT a separate message need this: the Beat FX LEVEL knob (SHIFT turns
 * it into TIME). Either deck's SHIFT counts. */
static int shift_down[2];

static int any_shift(void) { return shift_down[0] || shift_down[1]; }

/* ---- the jog, the pitch fader, MASTER LEVEL ------------------------------
 * The pitch fader and MASTER LEVEL follow map_flx4.c. The jog does not, and the
 * difference is measured: the FLX10's platter reports at 13083 counts/rev and in
 * BURSTS -- several messages land in the same millisecond on the sequencer -- so
 * map_flx4.c's per-message speed (counts / ms since the last message) saw dt = 0
 * inside every burst, fell back to its 0.5 ms floor, and pinned nearly every
 * update at the 8 rev/s clamp: the jog moved at one speed whatever the hand did
 * (operator, 2026-10-24).
 *
 * So counts are accumulated and sent at most once per JOG_WINDOW_US, the speed
 * being the window's counts over its real length on a microsecond clock. The
 * position rbp is given is scaled to JOG_POS_PPR counts/rev -- the FLX4's 720,
 * the resolution that contract was measured against -- so a turn of this
 * platter moves rbp's position as far as a turn of the FLX4's did; the speed
 * carries the true revolutions/second either way. tick() flushes a window that
 * ends after the last message, and the idle edge (JOG_IDLE_MS) still sends 0. */
struct jog_ctrl {
     int sch, moving;
     double pending;                 /* rbp units (1/720 turn) since the last update */
     unsigned long long last_ms;     /* last MESSAGE, for the idle edge */
     unsigned long long win_us;      /* start of the current window */
     double fpos;                    /* rbp position, JOG_POS_PPR per rev */
};
static struct jog_ctrl jog_state[2] = { { .sch = 1 }, { .sch = 2 } };
/* the platter pressed (N_JOG_TOUCH / N_JOG_TOUCH_SH), for the jog screen */
static int flx10_jog_touched[2];

static unsigned long long now_us(void)
{
     struct timespec ts;
     clock_gettime(CLOCK_MONOTONIC, &ts);
     return (unsigned long long)ts.tv_sec * 1000000ULL +
            (unsigned long long)ts.tv_nsec / 1000ULL;
}

/* rbp's 16-bit position from fpos, rounded without libm. */
static long jog_pos16(double f)
{
     long r = (long)(f >= 0.0 ? f + 0.5 : f - 0.5);
     return (long)((unsigned long)r & 0xFFFFu);
}

static void jog_flush(struct jog_ctrl *s, unsigned long long t_us)
{
     double dt = (double)(t_us - s->win_us) / 1e6;
     float speed;

     if (s->pending == 0.0)
          return;
     if (dt < JOG_WINDOW_US / 1e6)
          dt = JOG_WINDOW_US / 1e6;
     speed = (float)(s->pending / JOG_POS_PPR / dt);
     if (speed > 8.0f) speed = 8.0f;
     if (speed < -8.0f) speed = -8.0f;
     s->fpos += s->pending * jog_scale;
     s->pending = 0;
     s->win_us = t_us;
     send_rx_key_fl(K_JOG_ROT, OP_ROTATE, s->sch, 0, speed,
                    jog_pos16(s->fpos));
     if (jog_verbose)
          klog("knobshim2: flx10 jog deck%d speed=%.3f rev/s\n", s->sch, (double)speed);
}

/* JOG_CAL=1: one log line per jog gesture (ended by a JOG_CAL_GAP_MS pause):
 * the platter's net counts and how far rbp's playhead moved, so the gain can
 * be set from rbp's ms per count against rekordbox's (tools/flx10-pcap-decode.py
 * --jog). Meaningful on a paused deck; a playing deck adds its play time. */
#define JOG_CAL_GAP_MS 400
static int jog_cal;
struct jog_cal { int active, counts, msgs, pos0; double units; unsigned long long t0, last; };
static struct jog_cal jcal[2];
int flx10_pos_now(int deck, int *pos_ms, int *moving);

static void jog_cal_count(int deck, int d, double units)
{
     struct jog_cal *c = &jcal[deck];
     int mov;

     if (!c->active) {
          if (!flx10_pos_now(deck, &c->pos0, &mov))
               return;
          c->active = 1;
          c->counts = c->msgs = 0;
          c->units = 0.0;
          c->t0 = shim_now_ms();
     }
     c->counts += d;
     c->units += units;
     c->msgs++;
     c->last = shim_now_ms();
}

static void jog_cal_tick(void)
{
     unsigned long long now = shim_now_ms();

     for (int i = 0; i < 2; i++) {
          struct jog_cal *c = &jcal[i];
          int pos, mov, dms;
          if (!c->active || now - c->last < JOG_CAL_GAP_MS)
               continue;
          c->active = 0;
          if (!flx10_pos_now(i, &pos, &mov) || !c->counts)
               continue;
          dms = pos - c->pos0;
          /* rbp_turn_ms: what one rbp jog turn (720 units) moved the track --
           * the number JOG_RBP_TURN_MS wants */
          klog("knobshim2: jogcal deck%d counts=%d msgs=%d secs=%.2f track_ms=%d "
               "ms_per_count=%.4f rbp_units=%.0f rbp_turn_ms=%.1f (set %.1f, gain %.3f)%s\n",
               i + 1, c->counts, c->msgs, (double)(c->last - c->t0) / 1000.0, dms,
               (double)dms / c->counts, c->units,
               c->units != 0.0 ? dms * JOG_POS_PPR / c->units : 0.0,
               jog_rbp_turn_ms, jog_gain, mov ? " PLAYING: includes play time" : "");
     }
}

static void flx10_jog(int deck, int cc, int val)
{
     struct jog_ctrl *s = &jog_state[deck];
     int d = val - 0x40;
     unsigned long long t = now_us();
     int side = cc == CC_JOG_SIDE || cc == CC_JOG_SIDE_SH;
     double units;

     if (d == 0)
          return;
     if (jog_rev)
          d = -d;
     if (!s->moving) {               /* first count after a rest: open a window */
          s->moving = 1;
          s->pending = 0.0;
          s->win_us = t;
     }
     units = d * (side ? FLX10_MS_PER_COUNT_SIDE : FLX10_MS_PER_COUNT_TOP) *
             jog_gain * JOG_POS_PPR / jog_rbp_turn_ms;
     s->pending += units;
     s->last_ms = t / 1000ULL;
     if (jog_cal)
          jog_cal_count(deck, d, units);
     flx10_hid_jog_activity(deck, d > 0 ? 1 : -1);
     if (t - s->win_us >= JOG_WINDOW_US)
          jog_flush(s, t);
}

static void flx10_jog_tick(void)
{
     if (jog_cal)
          jog_cal_tick();
     unsigned long long t = now_us();

     for (int i = 0; i < 2; i++) {
          struct jog_ctrl *s = &jog_state[i];
          if (!s->moving)
               continue;
          if (s->pending != 0.0 && t - s->win_us >= JOG_WINDOW_US)
               jog_flush(s, t);
          if (t / 1000ULL - s->last_ms >= (unsigned long long)jog_idle_ms) {
               jog_flush(s, t);
               s->moving = 0;
               send_rx_key_fl(K_JOG_ROT, OP_ROTATE, s->sch, 0, 0.0f,
                              jog_pos16(s->fpos));
          }
     }
}

struct pitch_ctrl {
     int have_hi, pos;
     int valid, v10;          /* the fader's last full reading, for TEMPO RESET */
     float norm;
};
static struct pitch_ctrl pitch_state[2] = { { 0, 0x2000, 0, 512, 0.0f },
                                            { 0, 0x2000, 0, 512, 0.0f } };

/* TEMPO RESET (operator, 2026-10-25), as rekordbox does it: a toggle per deck.
 * On, the deck's tempo is held at 0.0% -- rbp is sent the fader's centre and
 * fader moves are kept but not sent; off, rbp gets wherever the fader is now.
 * Its LED (deck note 65) is lit while on. rbp has no tempo reset of its own
 * (IPlayerInnards::onEv_TempoReset exists only as the empty default), so this
 * is the map's. */
static int tempo_lock[2];
#define TEMPO_CENTRE_POS 0x2000

static void tempo_send(int deck, int v10, float norm, int pos)
{
     pitch_state_set(deck, norm);
     send_rx_key_fl(K_TEMPO_SLIDER, OP_VALUE, deck + 1, (long)v10, norm, (long)pos);
}

static void tempo_reset_led(int deck)
{
     midi_note(deck, N_TEMPO_RESET, tempo_lock[deck] ? 0x7F : 0x00);
}

static void flx10_tempo_reset(int deck)
{
     struct pitch_ctrl *s = &pitch_state[deck];

     tempo_lock[deck] = !tempo_lock[deck];
     if (tempo_lock[deck])
          tempo_send(deck, 511, 0.0f, TEMPO_CENTRE_POS);
     else if (s->valid)
          tempo_send(deck, s->v10, s->norm, s->pos);
     tempo_reset_led(deck);
     klog("knobshim2: flx10 deck%d TEMPO RESET %s\n", deck + 1,
          tempo_lock[deck] ? "on (tempo held at 0.0%)" : "off (back to the fader)");
}

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
     norm = pitch_norm_from_pos14(pos);
     if (norm > 1.0f) norm = 1.0f;
     if (norm < -1.0f) norm = -1.0f;
     if (tempo_rev)
          norm = -norm;
     v10 = (int)((norm + 1.0f) * 511.5f);
     if (v10 < 0) v10 = 0;
     if (v10 > 1023) v10 = 1023;
     s->valid = 1;
     s->v10 = v10;
     s->norm = norm;
     if (!tempo_lock[deck])
          tempo_send(deck, v10, norm, pos);
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

/* ---- SLIP REVERSE / REVERSE ----------------------------------------------
 * As labelled (operator, 2026-10-25):
 *   SLIP REVERSE (note 21): momentary and ALWAYS a slip reverse -- reverse
 *     while held, and on release the playhead returns to where it would have
 *     been, whether or not slip mode is on.
 *   REVERSE (SHIFT, note 56): latching plain reverse, NEVER slipping, even
 *     with slip mode on.
 *   LED (note 21): lit while the button is held and while the deck reverses.
 *
 * rbp's own REV key (ui::PlayerInnards::onKey_Reverse @0x302ff8) is one or the
 * other depending on slip mode: with slip mode OFF a press toggles reverse
 * (latching); with it ON a press starts reverse and the release ends it, the
 * engine slipping meanwhile. So:
 *   SLIP REVERSE turns slip mode on for the gesture if it was off (setSlipMode,
 *     put back once the reverse has ended and the slip return has landed --
 *     see QUANTIZE ON below), and sends REV press / release.
 *   REVERSE turns slip mode off while latched if it was on (put back when the
 *     latch ends), and toggles with a REV press + release, which is a latch
 *     with slip off. A latch rbp ends by itself (a new track) is noticed by the
 *     tick and undone the same way.
 * The engine reads are WEAK so the host test can answer for rbp. */

__attribute__((weak)) int flx10_slip_mode(int deck)
{
     void *pe = *(void **)PLAYENGINE_GLOBAL;
     return pe ? ((int (*)(void *, int))PE_ISSLIPMODEON)(pe, deck) != 0 : 0;
}

static void *pe_player(int deck);

/* Asked for OR still in effect (rbp_abi.h, PLAYER_REVERSE_ON_OFF). getReverse
 * alone is only the asked-for half: with quantize on it reads clear the moment
 * SLIP REVERSE is released while the deck goes on reversing to the next beat,
 * and putting slip off then (setSlipMode(false) mid-slip is
 * endSlippingToStayFg, which leaves the reverse alone) left the deck reversing
 * for good (operator, 2026-10-25). */
__attribute__((weak)) int flx10_reversing(int deck)
{
     void *pe = *(void **)PLAYENGINE_GLOBAL, *player;
     if (!pe)
          return 0;
     if (((int (*)(void *, int))PE_GETREVERSE)(pe, deck) & 0xff)
          return 1;
     player = pe_player(deck);
     return player && *((unsigned char *)player + PLAYER_REVERSE_ON_OFF) != 0;
}

/* Slip mode straight into the engine (DjEngineIF::setSlipMode), at once:
 * the REV key sent right after reads isSlipModeOn to choose between a slip
 * reverse and a latch, so slip has to be on BEFORE it arrives. (Sending rbp's
 * SLIP key instead, 2026-10-25, let the REV press see slip still off: with
 * quantize on the reverse then latched and was never undone.) */
__attribute__((weak)) void flx10_set_slip(int deck, int on)
{
     ((void (*)(void *, int, int))DJE_SET_SLIPMODE)(NULL, deck, on != 0);
}

__attribute__((weak)) void flx10_set_reverse(int deck, int on)
{
     ((void (*)(void *, int, int))DJE_SET_REVERSE)(NULL, deck, on != 0);
}

/* QUANTIZE ON: a reverse that starts late. playengine::Player::setReverse
 * @0x6a5ec has an early return at 0x6a698 taken when quantize is on (the
 * Player+0xec8 flag, on a branch gated by Player+0xef0's +84 -- believed to be
 * slip/loop engaged, not confirmed) that leaves the flag unset; the reverse
 * then shows up later (on the beat, by the operator's report).
 * A short press is then released before the deck is reversing, onKey_Reverse's
 * release sees no reverse to end, and the deck stays reversed -- what the
 * operator saw (2026-10-25). So after a release the tick watches the deck and
 * ends any reverse that turns up (setReverse(false), itself quantized), and
 * slip mode is put back as soon as the reverse is over: SLIP REVERSE is
 * "hold: slip on and reverse; release: reverse stops, slip off" (operator,
 * 2026-10-25 -- the old fixed 2 s watch left slip on for 2 s).
 *   reverse seen while held, or after the release: slip off once the deck
 *     has read not reversing -- asked for nor in effect, flx10_reversing --
 *     for SLIPREV_CLEAR_TICKS ticks. With quantize on the reverse goes on to
 *     the next beat after the release and the slip return lands there
 *     (endSlippingFromCallback clears both flags); slip put off before that
 *     ends the slip and leaves the deck reversing. SLIPREV_MAX_MS caps the
 *     wait (rbp's own slip reverse limit is 8 s, calcSlipReverseOffPoint);
 *   never seen (a tap released before a quantized reverse began): the watch
 *     lasts SLIPREV_PENDING_MS for the late start, then slip off. */
#define SLIPREV_PENDING_MS 1000
#define SLIPREV_CLEAR_TICKS 2
#define SLIPREV_MAX_MS 10000
static int rev_seen[2], rev_clear_n[2];
static int rev_held[2], rev_forced_slip[2], rev_latched[2], rev_slip_restore[2];
static unsigned long long rev_slip_off_at[2], rev_latched_at[2];
static unsigned long long rev_watch_until[2], rev_stop_sent_at[2], rev_released_at[2];
/* slip put back off, then checked: rbp sometimes kept slip on after a SLIP
 * REVERSE (operator, 2026-10-25) -- a setSlipMode(false) landing while the
 * deck is still slipping back seems not to take -- so it is re-checked
 * every SLIPREV_CHECK_MS and sent again, up to SLIPREV_CHECK_TRIES times */
#define SLIPREV_CHECK_MS 300
#define SLIPREV_CHECK_TRIES 5
#define SLIPREV_CHECK_FOR_MS 3000    /* ...for this long: it can come back late */
static unsigned long long rev_slip_check_at[2], rev_slip_check_until[2];
static int rev_slip_tries[2];
static int rev_led_v[2] = { -1, -1 };

static void flx10_slip_reverse(int deck, int on)
{
     int sch = deck + 1;

     if (on) {
          if (rev_latched[deck] || rev_held[deck])
               return;                       /* REVERSE is latched: leave it */
          rev_slip_check_at[deck] = 0;
          klog("knobshim2: flx10 deck%d SLIP REVERSE press: slip=%d forced=%d "
               "watching=%d restore_pending=%d\n", deck + 1, flx10_slip_mode(deck),
               rev_forced_slip[deck], rev_watch_until[deck] != 0, rev_slip_off_at[deck] != 0);
          if (rev_slip_off_at[deck] || rev_watch_until[deck]) {
               rev_slip_off_at[deck] = 0;     /* pressed again before the restore */
               rev_watch_until[deck] = 0;
          } else if (!flx10_slip_mode(deck)) {
               flx10_set_slip(deck, 1);
               rev_forced_slip[deck] = 1;
          }
          rev_held[deck] = 1;
          rev_seen[deck] = 0;
          send_rx_key(K_REV, OP_PRESS, sch, 0);
          return;
     }
     if (!rev_held[deck])
          return;
     rev_held[deck] = 0;
     send_rx_key(K_REV, OP_RELEASE, sch, 0);
     klog("knobshim2: flx10 deck%d SLIP REVERSE release: slip=%d forced=%d reversing=%d\n",
          deck + 1, flx10_slip_mode(deck), rev_forced_slip[deck], flx10_reversing(deck));
     if (flx10_reversing(deck))
          rev_seen[deck] = 1;
     rev_released_at[deck] = shim_now_ms();
     rev_watch_until[deck] = rev_released_at[deck] + (rev_seen[deck] ? 1 : SLIPREV_PENDING_MS);
     rev_clear_n[deck] = 0;
     rev_stop_sent_at[deck] = 0;
}

static void rev_unlatch(int deck)
{
     rev_latched[deck] = 0;
     if (rev_slip_restore[deck]) {
          rev_slip_restore[deck] = 0;
          flx10_set_slip(deck, 1);
     }
}

static void flx10_reverse_latch(int deck)
{
     int sch = deck + 1;

     if (rev_held[deck])
          return;
     if (!rev_latched[deck]) {
          if (flx10_slip_mode(deck)) {
               flx10_set_slip(deck, 0);
               rev_slip_restore[deck] = 1;
          }
          pulse(K_REV, sch);                 /* slip off: a latching toggle */
          rev_latched[deck] = 1;
          rev_latched_at[deck] = shim_now_ms();
     } else {
          if (flx10_slip_mode(deck))          /* slip turned on meanwhile */
               flx10_set_reverse(deck, 0);
          else
               pulse(K_REV, sch);
          rev_unlatch(deck);
     }
}

static void flx10_rev_tick(unsigned long long t)
{
     for (int d = 0; d < 2; d++) {
          int v;
          if (rev_held[d] && flx10_reversing(d))
               rev_seen[d] = 1;
          if (rev_watch_until[d] && !rev_held[d] && !rev_latched[d]) {
               int r = flx10_reversing(d);
               if (r && t - rev_released_at[d] >= SLIPREV_MAX_MS) {
                    klog("knobshim2: flx10 deck%d SLIP REVERSE: still reversing %d ms "
                         "after the release; giving up the wait\n", d + 1, SLIPREV_MAX_MS);
                    r = 0;
                    rev_seen[d] = 1;
                    rev_clear_n[d] = SLIPREV_CLEAR_TICKS;
               }
               if (r) {
                    rev_seen[d] = 1;
                    rev_clear_n[d] = 0;
                    if (t - rev_stop_sent_at[d] >= 100) {
                         if (!rev_stop_sent_at[d])
                              klog("knobshim2: flx10 deck%d SLIP REVERSE: still reversing "
                                   "after the release (quantize); ending it, slip "
                                   "on until it is over\n", d + 1);
                         flx10_set_reverse(d, 0);
                         rev_stop_sent_at[d] = t;
                    }
               } else if ((rev_seen[d] && ++rev_clear_n[d] >= SLIPREV_CLEAR_TICKS) ||
                          (!rev_seen[d] && t >= rev_watch_until[d])) {
                    rev_watch_until[d] = 0;
                    klog("knobshim2: flx10 deck%d SLIP REVERSE: reverse over (%s), slip=%d "
                         "forced=%d\n", d + 1, rev_seen[d] ? "ended" : "never started",
                         flx10_slip_mode(d), rev_forced_slip[d]);
                    if (rev_forced_slip[d])
                         rev_slip_off_at[d] = t;     /* now, below */
               }
          }
          if (rev_slip_off_at[d] && t >= rev_slip_off_at[d]) {
               rev_slip_off_at[d] = 0;
               if (rev_forced_slip[d]) {
                    rev_forced_slip[d] = 0;
                    flx10_set_slip(d, 0);
                    klog("knobshim2: flx10 deck%d SLIP REVERSE: slip put back off\n", d + 1);
                    rev_slip_check_at[d] = t + SLIPREV_CHECK_MS;
                    rev_slip_check_until[d] = t + SLIPREV_CHECK_FOR_MS;
                    rev_slip_tries[d] = 0;
               }
          }
          if (rev_slip_check_at[d] && t >= rev_slip_check_at[d]) {
               rev_slip_check_at[d] = 0;
               if (!rev_held[d] && !rev_latched[d] && flx10_slip_mode(d) &&
                   rev_slip_tries[d] < SLIPREV_CHECK_TRIES) {
                    rev_slip_tries[d]++;
                    klog("knobshim2: flx10 deck%d SLIP REVERSE: slip on again %llu ms "
                         "after the restore; off again (try %d)\n", d + 1,
                         t + SLIPREV_CHECK_FOR_MS - rev_slip_check_until[d],
                         rev_slip_tries[d]);
                    flx10_set_slip(d, 0);
               }
               if (t + SLIPREV_CHECK_MS <= rev_slip_check_until[d] && !rev_held[d])
                    rev_slip_check_at[d] = t + SLIPREV_CHECK_MS;
          }
          if (rev_latched[d] && t - rev_latched_at[d] > 500 && !flx10_reversing(d))
               rev_unlatch(d);               /* rbp ended it (a load, ...) */
          v = (rev_held[d] || flx10_reversing(d)) ? 0x7F : 0x00;
          if (v != rev_led_v[d] && midi_note(d, N_SLIP_REVERSE, v))
               rev_led_v[d] = v;
     }
}

/* ---- 4 BEAT JUMP < > ------------------------------------------------------
 * Real beat jumps (operator, 2026-10-25): 4 beats, and with SHIFT 16 beats.
 * These used to be rbp's SEARCH keys, which jump only while playing -- on a
 * paused deck onKey_SearchRev @0x304908 does a one-step msSearch, which is the
 * "tiny amount" the operator saw.
 *
 * Then they called DjEngineIF::playBeatJump, what a BEAT JUMP pad calls. With
 * quantize on that goes through the deck's quantizing loop object, and fast
 * repeated presses were sometimes late and sometimes lost (operator,
 * 2026-10-25). So the jump is now the plain loop object's
 * Loop::playBeatJump (rbp_abi.h, LOOP_PLAY_BEATJUMP), which moves the playhead
 * at once by exactly that many beats, every press -- an integer number of
 * beats keeps the phase. Inside a loop rbp's own call is kept (a jump there is
 * a loop move). Both are WEAK so the host test can answer for rbp. */
__attribute__((weak)) void flx10_beat_jump(int deck, int type)
{
     ((void (*)(void *, int, int))DJE_PLAY_BEATJUMP)(NULL, deck, type);
}

/* playengine::Player for a deck, NULL if rbp has none (yet) */
static void *pe_player(int deck)
{
     void *pe = *(void **)PLAYENGINE_GLOBAL;
     void **b, **e;

     if (!pe)
          return NULL;
     b = *(void ***)((char *)pe + PE_PLAYERS_BEGIN_OFF);
     e = *(void ***)((char *)pe + PE_PLAYERS_END_OFF);
     if (!b || deck < 0 || deck >= (int)(e - b))
          return NULL;
     return b[deck];
}

__attribute__((weak)) int flx10_plain_jump(int deck, float beats)
{
     void *player = pe_player(deck), *loop;

     if (!player)
          return 0;
     loop = *(void **)((char *)player + PLAYER_PLAIN_LOOP_OFF);
     if (!loop)
          return 0;
     return ((int (*)(void *, const float *))LOOP_PLAY_BEATJUMP)(loop, &beats) & 0xff;
}

static void flx10_jump(int deck, int beats)
{
     static const int type_m[17] = { [4] = BEATJUMP_M4, [16] = BEATJUMP_M16 };
     static const int type_p[17] = { [4] = BEATJUMP_P4, [16] = BEATJUMP_P16 };
     int n = beats < 0 ? -beats : beats;

     if (aloop_is_looping(deck))
          flx10_beat_jump(deck, beats < 0 ? type_m[n] : type_p[n]);
     else
          flx10_plain_jump(deck, (float)beats);
}

/* ---- SHIFT + CUE, MIX POINT SELECT < > ---------------------------------------
 * (operator, 2026-10-25.)
 *   SHIFT + CUE   back to the start: straight to the auto cue point (the main
 *                 cue when auto cue set it), else to the top; play/pause kept.
 *   MIX POINT <   the same, or -- already at the start -- the previous track.
 *   MIX POINT >   next track.
 *
 * The start is a SEEK the map does itself (flx10_seek_start), not rbp's TRACK
 * SEARCH <: that visibly went to the top and then to the auto cue point, and
 * held down it repeats through tracks. "At the start" = within START_ZONE_MS
 * after that point (or rbp's own JdgTrackBack rule says so).
 *
 * Previous / next track are TRACK SEARCH, held TRACK_HOLD_MS from the tick
 * whatever the finger does (a held key repeats after 500 ms -- the operator
 * does not want that). rbp does not act on the key event: TotalMainIfUi::onKey
 * -> TotalMainIf::InputKey_Track @0xd50d8 only records it as down/up and
 * TotalCnt_SentinelTASK polls that, so a press must stay down a while to be
 * seen. For "previous", the time JdgTrackBack @0xeab60 compares against
 * (rbp_abi.h, TTS_WK_LASTPRESS) is stamped first, so rbp reads the press as a
 * second press and loads the previous track instead of going to the top. */
#define START_ZONE_MS 1500
#define TRACK_HOLD_MS 100
static int deck_loaded[2], deck_elapsed_ms[2], deck_total_ms[2];
/* for BEAT SYNC's range fit: what deck_display last showed */
static int deck_bpm[2], deck_tempo[2], deck_range[2], deck_sync[2], deck_master[2];
static int deck_mt[2];                     /* MASTER TEMPO on (struct deck_info's mt) */
static int deck_speed[2];                  /* the speed it plays at, % x100 (flx10_shown_tempo) */
static char deck_key[2][16];               /* rbp's key text for the deck, "" = none */
static int deck_playing[2], deck_on_air[2];  /* struct deck_info's playing / on_air */
static int deck_orig[2];                   /* the track's own BPM x100 (bpm_orig), 0 = unknown */

/* BEAT SYNC's tempo range. The NEED is the master's playing BPM over this
 * track's own (its playing BPM at its tempo %), as the smallest range that holds
 * it -- a 100 BPM track synced to a 250 BPM master needs WIDE. Ranges
 * +-6/10/16/WIDE (100%) are getTempoRange 0..3, stepped by rbp's TEMPO RANGE key,
 * which goes round them in that order (so WIDE -> +-10 is two steps).
 *
 * SYNC pressed on: the range widened first if the need is wider (operator,
 * 2026-10-25), never narrowed. Then, while synced and following, the range
 * follows the need both ways -- a master sped way up widens it, slowed back down
 * narrows it again -- but never below the FLOOR: the range the deck had when
 * SYNC came on, or the one the operator set by hand since (operator,
 * 2026-10-26: a deck that started on +-10 never ends on +-6 unless set there).
 * A range change the shim did not just ask for is the operator's. */
#define FIT_SETTLE_MS 500
static int range_floor[2] = { -1, -1 };    /* -1 = not synced */
static int range_expect[2] = { -1, -1 };   /* the range our steps are heading to */
static int range_seen[2] = { -1, -1 };     /* the range when last looked at */
static int range_by_hand[2];               /* range_expect is the operator's press */
static int range_synced[2];                /* deck_sync when last looked at */
#define RANGE_PRESS_MS 2000                /* SYNC pressed: its floor kept this long for rbp */
static unsigned long long range_press_at[2];
static unsigned long long range_sent_at[2];

/* the smallest range that holds the sync's tempo, -1 = not known */
static int flx10_sync_need(int deck)
{
     static const int limit[4] = { 600, 1000, 1600, 10000 };
     int m = 1 - deck;
     long long orig, need;

     if (!deck_loaded[deck] || !deck_loaded[m] || !deck_master[m] ||
         deck_bpm[deck] <= 0 || deck_bpm[m] <= 0 || deck_tempo[deck] <= -10000)
          return -1;
     /* the track's own BPM: rbp's, when it says. Worked back from the playing
      * BPM and tempo % it is wrong while synced -- the % is the FADER's, not
      * the speed sync plays at (deck 2 synced at 94.08 read -0.68% against its
      * own 100.62: the need looked like -0.6%, the range narrowed, then
      * widened, round and round -- on the unit 2026-10-26) */
     orig = deck_orig[deck] > 0 ? deck_orig[deck]
                                : (long long)deck_bpm[deck] * 10000 / (10000 + deck_tempo[deck]);
     if (orig <= 0)
          return -1;
     need = (long long)deck_bpm[m] * 10000 / orig - 10000;
     if (need < 0)
          need = -need;
     for (int i = 0; i < 4; i++)
          if (need <= limit[i])
               return i;
     return 3;
}

static void flx10_range_steer(int deck, int want, const char *why)
{
     int cur = deck_range[deck] & 3, n = (want - cur + 4) % 4;

     if (!n)
          return;
     klog("knobshim2: flx10 deck%d BEAT SYNC %s: range %d -> %d\n", deck + 1, why, cur, want);
     while (n--)
          pulse(K_TEMPO_RANGE, deck + 1);
     range_expect[deck] = want;
     range_by_hand[deck] = 0;
     range_seen[deck] = cur;
     range_sent_at[deck] = shim_now_ms();
}

/* SYNC about to go on: the floor is the range now, widened first if needed */
static void flx10_sync_fit_range(int deck)
{
     int need = flx10_sync_need(deck), cur = deck_range[deck] & 3;

     range_floor[deck] = cur;
     range_seen[deck] = cur;
     range_press_at[deck] = shim_now_ms();
     if (need > cur)
          flx10_range_steer(deck, need, "on");
}

/* TEMPO RANGE pressed: rbp's next range round the ring -- but on a deck synced
 * to a master, never one too narrow for the sync: the press goes on round to the
 * next that holds it, or does nothing when only the one it is on does (operator,
 * 2026-10-26: WIDE -> +-6 and straight back was a flash, not a choice). The
 * range it lands on is the operator's, so it is the floor (flx10_range_tick). */
static void flx10_tempo_range_press(int deck)
{
     int cur = deck_range[deck] & 3, next = (cur + 1) & 3, need = -1;

     if (deck_sync[deck] && !deck_master[deck] && deck_master[1 - deck])
          need = flx10_sync_need(deck);
     while (need >= 0 && next < need && next != cur)
          next = (next + 1) & 3;
     if (next == cur) {
          klog("knobshim2: flx10 deck%d TEMPO RANGE: the sync needs range %d, kept\n",
               deck + 1, cur);
          return;
     }
     for (int n = (next - cur) & 3; n > 0; n--)
          pulse(K_TEMPO_RANGE, deck + 1);
     range_expect[deck] = next;              /* the tick waits for it, then floors */
     range_by_hand[deck] = 1;
     range_seen[deck] = cur;
     range_sent_at[deck] = shim_now_ms();
}

static void flx10_range_tick(unsigned long long t)
{
     for (int d = 0; d < 2; d++) {
          int cur = deck_range[d] & 3, need, want;

          if (!deck_sync[d]) {
               /* not synced YET, just after SYNC was pressed: the floor taken
                * then is kept for rbp to catch up -- dropped here, the deck came
                * on synced at the widened range and that became the floor, so it
                * never narrowed (operator, 2026-10-26: +-6, SYNC, WIDE for good) */
               if (!range_synced[d] && range_press_at[d] &&
                   t - range_press_at[d] < RANGE_PRESS_MS)
                    continue;
               range_floor[d] = range_expect[d] = -1;
               range_by_hand[d] = 0;
               range_synced[d] = 0;
               continue;
          }
          range_synced[d] = 1;
          range_press_at[d] = 0;
          if (range_floor[d] < 0)                 /* synced some other way */
               range_floor[d] = range_seen[d] = cur;
          if (range_expect[d] >= 0) {
               if (cur == range_expect[d] || t - range_sent_at[d] >= FIT_SETTLE_MS) {
                    if (range_by_hand[d] && cur == range_expect[d])
                         range_floor[d] = cur;      /* the operator's press arrived */
                    range_expect[d] = -1;           /* (or never will) */
                    range_by_hand[d] = 0;
               } else {
                    continue;
               }
          } else if (cur != range_seen[d]) {
               range_floor[d] = cur;                /* the operator's */
          }
          range_seen[d] = cur;
          if (deck_master[d] || !deck_master[1 - d])
               continue;                            /* only a deck following a master */
          need = flx10_sync_need(d);
          if (need < 0)
               continue;
          want = need > range_floor[d] ? need : range_floor[d];
          if (want != cur)
               flx10_range_steer(d, want, want > cur ? "wider" : "narrower");
     }
}

static int tr_key[2];
static unsigned long long tr_up_at[2];

__attribute__((weak)) int flx10_trrev_goes_back(int deck)
{
     int t = ((int (*)(int))UI_GET_PLAYTIME)(deck);
     int st = ((int (*)(int))CMN_GET_PLAYSTATUS)(deck);

     if (t < 0)
          return 1;
     if (st == 4 && t == 0)
          return 1;
     if (st == 6 && ((int (*)(int))UI_GET_CUE_BY_AUTOCUE)(deck) == 1)
          return 1;
     return 0;
}

__attribute__((weak)) void flx10_trrev_as_second_press(int deck)
{
     unsigned long *last = (unsigned long *)(TTS_WK_GLOBAL +
          (unsigned long)deck * TTS_WK_STRIDE + TTS_WK_LASTPRESS);
     *last = (unsigned long)((int (*)(void))OSTIM_GET_TIME)();
}

/* Where "the start" is, in samples (0 = the top), and in ms through *ms.
 * -1 when rbp has no player or track. */
__attribute__((weak)) long flx10_start_pos(int deck, int *ms)
{
     void *pe = *(void **)PLAYENGINE_GLOBAL;
     void *player = pe_player(deck), *reader, *cue;
     long pos = 0, len;

     *ms = 0;
     if (!pe || !player)
          return -1;
     reader = *(void **)((char *)player + PLAYER_READER_OFF);
     if (!reader)
          return -1;
     if ((((int (*)(void *, int))PE_ISCUE_BY_AUTOCUE)(pe, deck) & 0xff) != 0) {
          void **vt = *(void ***)reader;
          cue = ((void *(*)(void *, int))vt[READER_VT_GETCUE])(reader, 0);
          if (cue && *(long *)((char *)cue + CUE_POS_OFF) != -1)
               pos = *(long *)((char *)cue + CUE_POS_OFF);
     }
     len = ((long (*)(void *))PLAYER_TOTAL_LENGTH)(player);
     if (pos > 0 && len > 0 && deck_total_ms[deck] > 0)
          *ms = (int)((long long)pos * deck_total_ms[deck] / len);
     return pos;
}

__attribute__((weak)) void flx10_seek(int deck, long pos)
{
     void *player = pe_player(deck);

     if (!player)
          return;
     ((void (*)(void *, long, int))PLAYER_SET_PLAYPOS)(player, pos, 1);
     ((void (*)(void *))PLAYER_SET_BEATPOS_SEARCH)(player);
}

/* INST.DOUBLES of a paused deck: `deck`'s copy to where the original stands.
 * The original's position in ms (PE_GETPLAYINGTIME), over the track's length
 * in ms and in samples -- it is the same track on both. Answers 1 if it moved
 * the copy. WEAK so the host test can answer for rbp. */
__attribute__((weak)) int flx10_doubles_place(int deck)
{
     void *pe = *(void **)PLAYENGINE_GLOBAL, *player = pe_player(deck);
     long len, pos;
     int ms, total = deck_total_ms[1 - deck];

     if (!pe || !player || total <= 0 ||
         !((int (*)(void *, int))PE_ISLOADED)(pe, 1 - deck))
          return 0;
     ms = ((int (*)(void *, int))PE_GETPLAYINGTIME)(pe, 1 - deck);
     len = ((long (*)(void *))PLAYER_TOTAL_LENGTH)(player);
     if (ms < 0 || len <= 0)
          return 0;
     pos = (long)((long long)ms * len / total);
     flx10_seek(deck, pos);
     klog("knobshim2: flx10 deck%d: the copy put at deck%d's %d ms\n", deck + 1, 2 - deck, ms);
     return 1;
}

/* The jog screens' playhead, read by the HID sender 100 times a second
 * (flx10_hid.h): rbp_led's reading is 20 Hz, which made the arm lag a jog
 * and step back on a pause. The same PlayEngine getters rbp_led calls from
 * its own thread. WEAK so the host test can answer for rbp. */
__attribute__((weak)) int flx10_pos_now(int deck, int *pos_ms, int *moving)
{
     void *pe = *(void **)PLAYENGINE_GLOBAL;

     if (!pe || !((int (*)(void *, int))PE_ISLOADED)(pe, deck))
          return 0;
     *pos_ms = ((int (*)(void *, int))PE_GETPLAYINGTIME)(pe, deck);
     /* a playing deck under a held platter (vinyl mode) is held by the hand,
      * not running: counted as stopped, or the arm ran ahead at the tempo
      * and snapped back -- the "hold the jog and the arm shakes" bug */
     *moving = ((int (*)(void *, int))PE_ISPLAYING)(pe, deck) != 0 &&
               ((int (*)(void *, int))PE_ISINCUESTANDBY)(pe, deck) == 0 &&
               ((int (*)(void *, int))PE_ISJOGTOUCHING)(pe, deck) == 0;
     /* -1: playing in reverse, so the sender runs the arm backward */
     if (*moving && (((int (*)(void *, int))PE_GETREVERSE)(pe, deck) & 0xff))
          *moving = -1;
     return 1;
}

/* The audio file a deck has loaded, for the jog screens' artwork and
 * waveforms (flx10_hid.h). rbp keeps the string in the deck's reader, which a
 * new load replaces: copied at once, bounded. WEAK for the host test. */
__attribute__((weak)) int flx10_track_path(int deck, char *buf, size_t n)
{
     void *pe = *(void **)PLAYENGINE_GLOBAL;
     const char *p;

     buf[0] = 0;
     if (!pe || !((int (*)(void *, int))PE_ISLOADED)(pe, deck))
          return 0;
     p = ((const char *(*)(void *, int))PE_GETFILEPATH)(pe, deck);
     if (!p || !p[0])
          return 0;
     snprintf(buf, n, "%s", p);
     return 1;
}

/* A deck's 8 hot cue slots as rbp holds them now, for the jog screens' cue
 * marks (flx10_hid.h): in_ms -1 = empty, loop 1 = a loop cue, colour = rbp's
 * colour info, raw. Cues set or deleted on the unit show here at once, where
 * the export's files keep the list the track was loaded with. WEAK for the
 * host test. */
__attribute__((weak)) int flx10_hot_cues(int deck, int in_ms[8], int loop[8], unsigned colour[8])
{
     void *pe = *(void **)PLAYENGINE_GLOBAL;

     if (!pe || !((int (*)(void *, int))PE_ISLOADED)(pe, deck))
          return 0;
     for (unsigned k = 0; k < 8; k++) {
          in_ms[k] = ((int (*)(int, unsigned))UI_GET_HOTCUE_INTIME)(deck, k);
          loop[k] = ((int (*)(int, unsigned))UI_GET_HOTCUE_LOOPFLG)(deck, k) != 0;
          colour[k] = ((unsigned (*)(int, unsigned))UI_GET_HOTCUE_COLORINF)(deck, k);
     }
     return 1;
}

/* rbp's time mode for the deck, flipped (elapsed <-> remaining): what the
 * RX3's TIME MODE shortcut sets, there being no touch screen or mouse here
 * (operator, 2026-10-25). The jog screen follows rbp's mode. WEAK for the
 * host test. */
__attribute__((weak)) void flx10_toggle_time_mode(int deck)
{
     int remain = ((int (*)(int))UI_GET_KEYTIMEMODE)(deck) == 1;

     ((void (*)(int, int))UI_SET_KEYTIMEMODE)(deck, !remain);
     klog("knobshim2: flx10 deck%d time mode -> %s\n", deck + 1,
          remain ? "elapsed" : "remaining");
}

/* rbp's track-end warning phase for the deck (rbp_abi.h
 * UI_REMAIN_CAUTION_REVERSE): 1 while the warning flash is in its "on" half.
 * Read by the HID sender at 100 Hz for the jog's light ring. WEAK for the
 * host test. */
__attribute__((weak)) int flx10_end_caution(int deck)
{
     return deck >= 0 && deck < 2 &&
            ((volatile int *)UI_REMAIN_CAUTION_REVERSE)[deck] != 0;
}

/* The main cue point in ms (the jog's cue arm), -1 when there is none: the
 * reader's cue object for LAST_CUE, as flx10_start_pos reads it, scaled from
 * samples by the track's length. WEAK so the host test can answer for rbp. */
__attribute__((weak)) int flx10_cue_ms(int deck, int total_ms)
{
     void *player = pe_player(deck), *reader, *cue;
     long pos, len;

     if (!player || total_ms <= 0)
          return -1;
     reader = *(void **)((char *)player + PLAYER_READER_OFF);
     if (!reader)
          return -1;
     cue = ((void *(*)(void *, int))(*(void ***)reader)[READER_VT_GETCUE])(reader, 0);
     if (!cue)
          return -1;
     pos = *(long *)((char *)cue + CUE_POS_OFF);
     len = ((long (*)(void *))PLAYER_TOTAL_LENGTH)(player);
     if (pos < 0 || len <= 0)
          return -1;
     return (int)((long long)pos * total_ms / len);
}

static void track_tap(int deck, int key)
{
     if (tr_key[deck])
          send_rx_key(tr_key[deck], OP_RELEASE, deck + 1, 0);
     send_rx_key(key, OP_PRESS, deck + 1, 0);
     tr_key[deck] = key;
     tr_up_at[deck] = shim_now_ms() + TRACK_HOLD_MS;
}

static void flx10_track_tick(unsigned long long t)
{
     for (int d = 0; d < 2; d++)
          if (tr_key[d] && t >= tr_up_at[d]) {
               send_rx_key(tr_key[d], OP_RELEASE, d + 1, 0);
               tr_key[d] = 0;
          }
}

static void flx10_track_note(int deck, int note, int on)
{
     int ms;
     long pos;

     if (!on)
          return;                            /* nothing follows the finger */
     if (note == N_MIXPOINT_NEXT) {
          klog("knobshim2: flx10 deck%d MIX POINT > -> next track\n", deck + 1);
          track_tap(deck, K_TRFWD);
          return;
     }
     if (!deck_loaded[deck]) {
          if (note == N_MIXPOINT_PREV) {
               flx10_trrev_as_second_press(deck);
               track_tap(deck, K_TRREV);
          }
          return;
     }
     pos = flx10_start_pos(deck, &ms);
     if (note == N_MIXPOINT_PREV &&
         (pos < 0 || deck_elapsed_ms[deck] <= ms + START_ZONE_MS ||
          flx10_trrev_goes_back(deck))) {
          klog("knobshim2: flx10 deck%d MIX POINT <: at the start (%d ms, start %d ms) "
               "-> previous track\n", deck + 1, deck_elapsed_ms[deck], ms);
          flx10_trrev_as_second_press(deck);
          track_tap(deck, K_TRREV);
          return;
     }
     if (pos < 0)
          return;
     klog("knobshim2: flx10 deck%d %s -> start of the track (%ld samples, %d ms)\n",
          deck + 1, note == N_CUE_SHIFT ? "SHIFT+CUE" : "MIX POINT <", pos, ms);
     flx10_seek(deck, pos);
}

/* ---- crossfader assign ------------------------------------------------------
 * Each channel's A / THRU / B switch (operator, 2026-10-25: the crossfader
 * faded nothing). Per Pioneer's MIDI list (M11) the unit sends, on the deck
 * channel, note 22 = A, 24 = B, 29 = THRU, 0x7F for the position the switch
 * moved to (the others go 0x00 alongside). rbp's assign was never set at all
 * (rbp_abi.h, DJE_SET_XF_ASSIGN), so it stayed THRU. The unit also reports the
 * switch in its answer to the position query sent at connect. WEAK so the host
 * test can answer for rbp. */
__attribute__((weak)) void flx10_set_xf_assign(int deck, int assign)
{
     ((void (*)(void *, int, int))DJE_SET_XF_ASSIGN)(NULL, deck, assign);
}

static void flx10_xf_assign(int deck, int note)
{
     int a = note == N_XF_A ? XF_ASSIGN_A : note == N_XF_B ? XF_ASSIGN_B : XF_ASSIGN_THRU;
     flx10_set_xf_assign(deck, a);
     klog("knobshim2: flx10 channel %d crossfader assign -> %s\n", deck + 1,
          a == XF_ASSIGN_A ? "A" : a == XF_ASSIGN_B ? "B" : "THRU");
}

/* ---- Beat FX AUTO (SHIFT + BEAT <) -------------------------------------------
 * Puts the Beat FX BPM back to AUTO after TAP (operator, 2026-10-25), what the
 * RX3 does for TAP with SHIFT held (rbp_abi.h, BPMMGR_SET_DETECTMODE). Called
 * directly: the RX3 gets there through a SHIFT held on its own panel, which
 * this map cannot hold for it. */
__attribute__((weak)) void flx10_bfx_bpm_auto(void)
{
     void *mgr = ((void *(*)(void))BPMMGR_GET_INSTANCE)();
     if (mgr)
          ((int (*)(void *, int))BPMMGR_SET_DETECTMODE)(mgr, BFX_BPM_AUTO);
}

/* ---- loop objects primed before a loop key ----------------------------------
 * On a freshly loaded track a loop made with IN / OUT showed no length, and
 * doubling it stopped short; after an auto beat loop (4 BEAT) and an exit it
 * behaved (operator, 2026-10-25). The loop objects cache the track's length,
 * BPM and beat grid, and only some paths refresh that cache (the auto beat
 * loop, LoopQuantize's beat jump); IN / OUT / CALL then work from the previous
 * track's numbers. So before each loop key the map runs the same three
 * refreshes on both of the deck's loop objects (rbp_abi.h,
 * LOOP_INIT_TOTALLENGTH). They only read the Player and fill the cache. */
__attribute__((weak)) void flx10_loop_prime(int deck)
{
     static const unsigned offs[2] = { PLAYER_PLAIN_LOOP_OFF, PLAYER_QUANT_LOOP_OFF };
     void *player = pe_player(deck);

     if (!player)
          return;
     for (int i = 0; i < 2; i++) {
          void *loop = *(void **)((char *)player + offs[i]);
          if (!loop)
               continue;
          ((int (*)(void *))LOOP_INIT_TOTALLENGTH)(loop);
          ((int (*)(void *))LOOP_INIT_BPM)(loop);
          ((int (*)(void *))LOOP_INIT_BEATPOS)(loop);
     }
}

/* ---- the loop section ----------------------------------------------------
 * The operator's table (docs/21):
 *
 *   control         not in a loop            in a loop
 *   IN / 1/2X       loop IN                  halve  (rbp's CALL <)
 *   OUT / 2X        loop OUT                 double (rbp's CALL >)
 *   4 BEAT / EXIT   4-beat loop, at once     exit   (RELOOP/EXIT)
 *   SHIFT + IN      loop IN                  IN ADJUST  (IN held)
 *   SHIFT + OUT     RELOOP (last loop)       OUT ADJUST (OUT held)
 *
 * "In a loop" is rbp's own answer at the moment of the PRESS
 * (aloop_is_looping(), PlayEngine::isLooping), and the release goes to whatever
 * key the press went to -- so a loop that ends while the button is held cannot
 * leave an rbp key half-pressed.
 *
 * The 4-beat loop is the RX3's own "hold IN" gesture with the wait taken out.
 * In ui::PlayerInnards::onKey_LoopIn @0x303b18 the PRESS sets the loop-in point,
 * and the REPEAT edge (op 1, which rbp's key manager produces only after about a
 * second of holding) is the branch at 0x303c98 that calls
 * DjEngineIF::setAutoBeatLoop(ch, {4, 1}, 3, ...) when the deck is not already
 * looping or in cue standby. Sending press, repeat and release together is that
 * same sequence, so the loop starts on the press instead of a beat later. The
 * first version held IN for 1.1 s and waited for rbp's own repeat. */
static int loop_key[2][3];                   /* [deck][IN, OUT, SHIFT-OUT/IN] */

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

     if (on) {
          looping = aloop_is_looping(deck) != 0;
          flx10_loop_prime(deck);
     } else {
          looping = 0;                       /* unused on a release */
     }

     switch (note) {
     case N_IN:
          if (!on) { loop_release(deck, 0, sch); return; }
          if (looping && !k_callprev) { not_yet("loop halve (CALL <)", sch); return; }
          if (!looping)
               led_loop_flash[deck] = 1;      /* loop-in set: IN flashes 4x */
          loop_press(deck, 0, looping ? k_callprev : K_LOOPIN, sch);
          return;
     case N_OUT:
          if (!on) { loop_release(deck, 1, sch); return; }
          if (looping && !k_callnext) { not_yet("loop double (CALL >)", sch); return; }
          loop_press(deck, 1, looping ? k_callnext : K_LOOPOUT, sch);
          return;
     case N_IN_SHIFT:
          if (!on) { loop_release(deck, 2, sch); return; }
          if (!looping)
               led_loop_flash[deck] = 1;
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
          } else {
               send_rx_key(K_LOOPIN, OP_PRESS, sch, 0);
               send_rx_key(K_LOOPIN, OP_REPEAT, sch, 0);
               send_rx_key(K_LOOPIN, OP_RELEASE, sch, 0);
          }
          return;
     default:
          return;
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

/* Is the highlighted browse row a track? rbp's own answer (rbp_abi.h,
 * UiBrowseComm_IsTrackList). WEAK so the host test can answer for rbp. */
/* For the push's log line: what the track test read. WEAK with the test. */
__attribute__((weak)) void flx10_cursor_debug(int *mode, int *cur, int *lf, int *rf)
{
     const volatile unsigned char *b = (const volatile unsigned char *)UIBROWSE_GLOBAL;
     *mode = *(const volatile int *)(b + UIBROWSE_OFF_MODE);
     *cur = *(const volatile int *)(b + UIBROWSE_OFF_CURSOR);
     *lf = b[UIBROWSE_OFF_LISTFLAGS];
     *rf = (*cur >= 0 && *cur <= 11)
           ? b[(*cur + 1) * UIBROWSE_ROW_STRIDE + UIBROWSE_OFF_ROWFLAGS] : -1;
}

__attribute__((weak)) int flx10_cursor_is_track(void)
{
     const volatile unsigned char *b = (const volatile unsigned char *)UIBROWSE_GLOBAL;
     int mode = *(const volatile int *)(b + UIBROWSE_OFF_MODE);

     if (mode == 3 || mode == 13 || mode == 14) {
          int cur = *(const volatile int *)(b + UIBROWSE_OFF_CURSOR);
          if (b[UIBROWSE_OFF_LISTFLAGS] & UIBROWSE_LISTFLAG_TRACKS)
               return 1;
          if (cur < 0 || cur > 11)
               return 0;
          return (b[(cur + 1) * UIBROWSE_ROW_STRIDE + UIBROWSE_OFF_ROWFLAGS] &
                  UIBROWSE_ROWFLAG_TRACK) != 0;
     }
     if (mode == 4)
          return *(const volatile unsigned short *)(b + UIBROWSE_OFF_TAGDEPTH) == 1;
     return 0;
}

/* ---- the browse knobs' pushes --------------------------------------------
 * Both knobs TURN with the same message (ch 7 CC 64), so they scroll one list;
 * only the pushes say which deck: 70 = deck 1, 71 = deck 2. A push is LOAD on
 * release. A second push within LOAD_DOUBLE_MS of a LOAD is INST.DOUBLES,
 * as printed on the FLX10 (operator, 2026-10-26; it used to be a second LOAD):
 * the first push's LOAD has gone, and the other deck's track replaces it.
 *
 * HELD browse_hold_ms, it is the RX3's own long press of the rotary selector,
 * which is rbp's FAST SCROLL (the fast-forward icon). rbp's long press for
 * 0x420c is 500 ms (KeyInput::getLongPressTime @0x37de18) and arrives as the
 * selector with op 1 (KeyInput::isLongEnter @0x37e1cc tests keycode | 1 << 24,
 * byte 11 being the op); while scrolling rbp asks whether the selector is
 * still down (isRotarySelectorPressed @0x37e028). So at 500 ms tick() sends
 * the selector PRESS then op 1 and keeps it held, and the release goes out
 * when the knob comes up -- nothing else on that release. The hold used to be
 * TAG TRACK (0x420e); the operator unmapped it (2026-10-25).
 *
 * ON THE SOURCE SCREEN a push opens USB 1 instead (operator, 2026-10-24), when
 * slot 1 has a rekordbox stick: LOAD means nothing there, and rbp ignores the
 * knob on that screen, so without this the stick could only be opened from the
 * glass. A slot-2-only stick is still opened from the glass, as in map_jp21.c:
 * no USB2 keycode has been measured. */
static unsigned long long push_at[2];
static int push_fired[2];            /* the long press went out; selector held */
/* LOAD is held ~50 ms like a real tap: rbp did nothing with a press and a
 * release sent in the same instant (operator, 2026-10-25). */
#define LOAD_HOLD_MS 50
static unsigned long long load_up_at[2];
#define LOAD_DOUBLE_MS 400                 /* a second LOAD push within this: INST.DOUBLES */
static unsigned long long load_at[2];       /* the last LOAD push, 0 = none to pair */

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
     if (push_fired[deck]) {
          push_fired[deck] = 0;
          send_rx_key(K_SELECTOR, OP_RELEASE, CH_GLOBAL, 0);   /* fast scroll off */
          return;
     }
     /* the second push of a pair, whatever the screen: the first's LOAD has
      * taken rbp back to the player, so the cursor is on no track now */
     if (load_at[deck] && shim_now_ms() - load_at[deck] < LOAD_DOUBLE_MS) {
          load_at[deck] = 0;
          flx10_inst_doubles(deck, "LOAD twice");
          return;
     }
     if (flx10_browse_mode() == BROWSE_MODE_SOURCE && flx10_usb1_present()) {
          pulse(K_USB1, CH_GLOBAL);
          klog("knobshim2: flx10 browse push on SOURCE -> open USB1 (0x%04x)\n", K_USB1);
          return;
     }
     /* A track row LOADs to this knob's deck; anything else (folder,
      * playlist, category) is SELECTed, which opens it. It used to be LOAD
      * then SELECT for every row, and the SELECT on a track opened rbp's track
      * menu (operator, 2026-10-25). */
     {
          int mode, cur, lf, rf, track = flx10_cursor_is_track();
          flx10_cursor_debug(&mode, &cur, &lf, &rf);
          klog("knobshim2: flx10 push deck%d: browse mode %d cursor %d listflags "
               "0x%02x rowflags 0x%02x -> %s\n", deck + 1, mode, cur, lf, rf & 0xff,
               track ? "LOAD (held)" : "SELECT");
          if (track) {
               load_at[deck] = shim_now_ms();
               if (load_up_at[deck])
                    send_rx_key(K_LOAD, OP_RELEASE, deck + 1, 0);
               send_rx_key(K_LOAD, OP_PRESS, deck + 1, 0);
               load_up_at[deck] = shim_now_ms() + LOAD_HOLD_MS;
          } else {
               pulse(K_SELECTOR, CH_GLOBAL);
          }
     }
}

static void flx10_push_tick(unsigned long long t)
{
     for (int d = 0; d < 2; d++) {
          if (load_up_at[d] && t >= load_up_at[d]) {
               load_up_at[d] = 0;
               send_rx_key(K_LOAD, OP_RELEASE, d + 1, 0);
          }
          if (!push_at[d] || push_fired[d] ||
              t - push_at[d] < (unsigned long long)browse_hold_ms)
               continue;
          push_fired[d] = 1;
          /* rbp's long press of the selector: fast scroll until the release */
          send_rx_key(K_SELECTOR, OP_PRESS, CH_GLOBAL, 0);
          send_rx_key(K_SELECTOR, OP_REPEAT, CH_GLOBAL, 0);
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
 *   RX3 (CCW -> CW, as printed, from the operator's photo 2026-10-26): DELAY,
 *   ECHO, PING PONG, SPIRAL, REVERB, TRANS, FILTER | FLANGER, PHASER, PITCH,
 *   SLIP ROLL, ROLL, VINYL BRAKE, HELIX
 *   rbp's switch positions, MEASURED on the Pi (docs/13 S5.9, and fx_zone.c's
 *   names): 0 DELAY 1 ECHO 2 PING PONG 3 SPIRAL 4 HELIX 5 REVERB 6 FLANGER
 *   7 PHASER 8 FILTER 9 TRANS 10 ROLL 11 SLIP ROLL 12 PITCH 13 VINYL BRAKE.
 *   NOT the order printed round the RX3's knob -- the first version of this
 *   table assumed that and sent REVERB as HELIX. */
/* MATCHED BY NAME where the FLX10's label has an RX3 effect (MT DELAY is
 * DELAY), and the other five by the closest detent of the RX3's own dial
 * (operator, 2026-10-26: "match by name ... the rest by closest position on the
 * dial"). The two dials then agree position for position except at the ends:
 * LOW CUT ECHO -> PING PONG (RX3 detent 2), ENIGMA JET -> FILTER (6), STRETCH
 * -> PITCH (9), MOBIUS saw -> VINYL BRAKE (12), MOBIUS triangle -> HELIX (13).
 * The FX screen shows the RX3's name (flx10_fxscreen_tick), so the five whose
 * label differs say what they are. */
static const signed char fxsel_to_rbp[14] = {
      2,  1,  0,  3,  5,  9,  8,     /* LOW CUT ECHO .. ENIGMA JET */
      6,  7, 12, 11, 10, 13,  4,     /* FLANGER .. MOBIUS */
};
static const char *const fxsel_name[14] = {
     "LOW CUT ECHO", "ECHO", "MT DELAY", "SPIRAL", "REVERB", "TRANS",
     "ENIGMA JET", "FLANGER", "PHASER", "STRETCH", "SLIP ROLL", "ROLL",
     "MOBIUS (saw)", "MOBIUS (triangle)",
};

static int fxsel_pos = -1;          /* the RX3 position last selected, -1 = none yet */

static void flx10_fxsel(int note)
{
     int i = note - N_FX_SEL_FIRST;
     int pos = fxsel_to_rbp[i];

     if (pos < 0) {
          klog("knobshim2: flx10 Beat FX %s: no RX3 equivalent, unchanged\n",
               fxsel_name[i]);
          return;
     }
     fxsel_pos = pos;
     send_rx_key(K_BFXTYPE, OP_VALUE, CH_GLOBAL, pos);
     klog("knobshim2: flx10 Beat FX %s -> RX3 position %d\n", fxsel_name[i], pos);
}

/* The channel selector: one note per position, MEASURED on the unit
 * 2026-10-24 by stepping it counter-clockwise to clockwise -- 22 21 18 16 17
 * 19 20 under the printed SP MIC 3 1 2 4 MST, i.e. 16..19 = channels 1..4,
 * 20 = MST, 21 = MIC, 22 = SP. rbp's EnBeatEffectSelectChannel (rbp_abi.h):
 * 0 PLAYER_0, 1 PLAYER_1, 2 MIC, 3 ASSIGN_A, 4 ASSIGN_B, 5 MASTER, 6 AUX.
 * 3, 4 and SP select nothing and leave rbp's target where it was (operator,
 * 2026-10-24: crossfader A/B on 3/4 would contradict the deck's own channel
 * lights). The unit sends a position only when the knob moves, so after a start
 * rbp keeps its own default until it is turned. */
static const signed char fxch_target[7] = {
     0, 1, -1, -1, BFX_CH_MASTER, 2, -1,   /* notes 16..22: 1 2 3 4 MST MIC SP */
};
static const char *const fxch_name[7] = { "1", "2", "3", "4", "MST", "MIC", "SP" };

static void flx10_fxch(int note)
{
     int i = note - N_FX_CH_FIRST;

     if (fxch_target[i] < 0) {
          klog("knobshim2: flx10 Beat FX channel %s: no RX3 target, unchanged\n",
               fxch_name[i]);
          return;
     }
     send_rx_key(K_BFXCH, OP_VALUE, CH_GLOBAL, fxch_target[i]);
     klog("knobshim2: flx10 Beat FX channel %s -> RX3 target %d\n",
          fxch_name[i], fxch_target[i]);
}

/* ---- the FX screen ---------------------------------------------------------
 * In rekordbox's host mode -- which the connect sequence (flx10_display_init)
 * puts the unit in -- the FLX10 draws its FX screen only from three SysEx the
 * host sends, and stays on whatever came last (operator, 2026-10-26: "the FX
 * display on the flx10 does not update"). Decoded from the operator's
 * capture_fx of the same day, every effect, beat and channel stepped through:
 *
 *   F0 00 40 05 00 00 04 01 00 00 0A  00 28 00 26 00 <37 bytes>  F7   the name
 *   F0 00 40 05 00 00 04 01 00 00 0B  <6 bytes>                  F7   the beat
 *   F0 00 40 05 00 00 04 01 00 00 0C  00 00 n n n n  tap 00      F7   the BPM
 *
 * The name and the beat are ASCII, 8 bits a character LSB first, packed into
 * the 7-bit bytes LSB first -- the name from bit 1 of its field, the beat from
 * bit 0 ("ECHO" = 0a 0d 42 74 09, "1/2" = 31 5e 48 01). The BPM is x100 in hex
 * digits, one a byte (135.90 = 00 00 03 05 01 06), then 1 in TAP mode.
 * rekordbox's beat texts are "1/16".."64", "3/4", and "50%" for a percent.
 *
 * So the screen shows the RX3's effect, its beat and its BPM, read from rbp:
 *   - the effect: rbp's current one (BeatEffectManager+0x08) while it is on,
 *     the selector's last position while it is off -- the manager's type reads
 *     0 then (rbp_abi.h's conjunction);
 *   - the beat: that effect object's rung (+0x44) through rbp's own table
 *     (ui::MasterInnards::getBeatFxNumDen: 1/16 1/8 1/4 1/2 3/4 1 2 4 8 16 32
 *     64), or for REVERB and PITCH, whose arrows step a percent, the percent
 *     itself (+0x30, what BeatEffectReverb::calcFromPercent and
 *     BeatEffectPitch::changePercentValuePitch read; REVERB 1 10 25 50 75 90
 *     100, PITCH -50 .. 100 on rbp's screen). +0x24 does not move with the
 *     arrows on those two (operator, 2026-10-26). An effect that is off is found
 *     among the manager's per-effect objects (+0x0c..+0x48) by its class --
 *     the slots are not in type order on this build (read 2026-10-26);
 *   - the BPM: dsp::BpmManager's +0x1c, x100, 12000 while it holds the
 *     invalid marker (getBfxBpmNoCheck), and its +0x10 detect mode (1 TAP).
 * Plain reads, no call into rbp but BpmManager::getInstance(), which
 * flx10_bfx_bpm_auto() already makes from this thread. */
#define FXS_POLL_MS 100

/* The names the FX screen shows. TRANS reads TRANNY, the operator's own flavour
 * (2026-10-26); rbp's own screen keeps its name. */
static const char *const rbp_fx_name[14] = {
     "DELAY", "ECHO", "PING PONG", "SPIRAL", "HELIX", "REVERB", "FLANGER",
     "PHASER", "FILTER", "TRANNY", "ROLL", "SLIP ROLL", "PITCH", "VINYL BRAKE",
};
/* each effect's class's vtable (nm: "vtable for mixerengine::BeatEffect..."),
 * by rbp switch position; an object's first word is this + 8 */
static const unsigned int rbp_fx_vtable[14] = {
     0x00425340, 0x004253f0, 0x0041fe38, 0x0041f568, 0x0041f890, 0x0041dd78,
     0x00425498, 0x0041fd78, 0x0041f978, 0x0041f730, 0x0041f488, 0x0041f7d8,
     0x0041dc30, 0x0041fc70,
};
#define RBP_FX_REVERB        5      /* rbp switch positions of the two percent effects */
#define RBP_FX_PITCH         12
#define BE_OFF_PERCENT       0x30   /* BeatEffect* -> long percent (REVERB, PITCH) */
#define BPMMGR_OFF_BFX_BPM   0x1c   /* BPM x100, or the invalid marker */
#define BPMMGR_OFF_BFX_MODE  0x10   /* EnBfxBpmDetectMode: 0 AUTO, 1 TAP */
#define BPMMGR_INVALID_BPM   0x00417334   /* the marker getBfxBpmNoCheck compares */

/* `n` bytes of 7-bit SysEx data holding `text` packed from bit `bit0`. Pure. */
static void fxs_pack(const char *text, int bit0, unsigned char *out, int n)
{
     memset(out, 0, (size_t)n);
     for (int c = 0; text[c]; c++)
          for (int i = 0; i < 8; i++) {
               int b = bit0 + c * 8 + i;
               if (b >= n * 7)
                    return;
               if (((unsigned char)text[c] >> i) & 1)
                    out[b / 7] |= (unsigned char)(1 << (b % 7));
          }
}

static const unsigned char fxs_head[10] = {
     0xF0, 0x00, 0x40, 0x05, 0x00, 0x00, 0x04, 0x01, 0x00, 0x00 };

/* The three messages. Each fills `out` and answers its length. Pure, for the test. */
int flx10_fx_name_sysex(const char *name, unsigned char *out)
{
     static const unsigned char pre[5] = { 0x00, 0x28, 0x00, 0x26, 0x00 };
     memcpy(out, fxs_head, 10);
     out[10] = 0x0A;
     memcpy(out + 11, pre, 5);
     fxs_pack(name, 1, out + 16, 37);
     out[53] = 0xF7;
     return 54;
}

int flx10_fx_beat_sysex(const char *text, unsigned char *out)
{
     memcpy(out, fxs_head, 10);
     out[10] = 0x0B;
     fxs_pack(text, 0, out + 11, 6);
     out[17] = 0xF7;
     return 18;
}

int flx10_fx_bpm_sysex(int bpm_x100, int tap, unsigned char *out)
{
     memcpy(out, fxs_head, 10);
     out[10] = 0x0C;
     if (bpm_x100 < 0) bpm_x100 = 0;
     if (bpm_x100 > 0xFFFFFF) bpm_x100 = 0xFFFFFF;
     for (int k = 0; k < 6; k++)
          out[11 + k] = (unsigned char)((bpm_x100 >> (4 * (5 - k))) & 0x0F);
     out[17] = tap ? 1 : 0;
     out[18] = 0x00;
     out[19] = 0xF7;
     return 20;
}

/* The beat text rbp's screen shows: the rung's fraction, or for a percent effect
 * the percent. Pure, for the test. */
void flx10_fx_beat_text(int rung, int pct, int is_pct, char *out, int max)
{
     static const char *const rungs[12] = {
          "1/16", "1/8", "1/4", "1/2", "3/4", "1", "2", "4", "8", "16", "32", "64" };
     if (is_pct)
          snprintf(out, (size_t)max, "%d%%", pct);
     else if (rung >= 0 && rung < 12)
          snprintf(out, (size_t)max, "%s", rungs[rung]);
     else
          snprintf(out, (size_t)max, "%s", "");
}

struct fxs_view {
     int pos;              /* rbp switch position, -1 = not known */
     char beat[8];
     int bpm_x100, tap;
};

/* What rbp holds now. WEAK so the host test can answer for rbp. 0 = not up. */
__attribute__((weak)) int flx10_fx_read(int sel_pos, struct fxs_view *v)
{
     unsigned int mixer, bem, be = 0, type, vt;
     void *bm;
     int pos = -1;

     mixer = *(volatile unsigned int *)(uintptr_t)ME_SINGLETON;
     if (!mixer)
          return 0;
     bem = *(volatile unsigned int *)(uintptr_t)(mixer + BEM_IN_MIXER);
     if (!bem)
          return 0;
     type = *(volatile unsigned int *)(uintptr_t)(bem + BEM_OFF_TYPE);
     if (type != BFX_TYPE_OFF) {
          be = *(volatile unsigned int *)(uintptr_t)(bem + BEM_OFF_EFFECT);
          vt = be ? *(volatile unsigned int *)(uintptr_t)be : 0;
          for (int k = 0; k < 14; k++)
               if (vt == rbp_fx_vtable[k] + 8)
                    pos = k;
          if (pos < 0)
               be = 0;
     }
     if (!be && sel_pos >= 0 && sel_pos < 14) {
          for (unsigned off = 0x0c; off <= 0x48 && !be; off += 4) {
               unsigned int o = *(volatile unsigned int *)(uintptr_t)(bem + off);
               if (o && *(volatile unsigned int *)(uintptr_t)o == rbp_fx_vtable[sel_pos] + 8)
                    be = o;
          }
          pos = be ? sel_pos : -1;
     }
     v->pos = pos;
     if (be)
          flx10_fx_beat_text(*(volatile int *)(uintptr_t)(be + BE_OFF_BEAT),
                             *(volatile int *)(uintptr_t)(be + BE_OFF_PERCENT),
                             pos == RBP_FX_REVERB || pos == RBP_FX_PITCH,
                             v->beat, (int)sizeof v->beat);
     else
          v->beat[0] = '\0';
     v->bpm_x100 = 12000;
     v->tap = 0;
     bm = ((void *(*)(void))BPMMGR_GET_INSTANCE)();
     if (bm) {
          unsigned int b = *(volatile unsigned int *)((char *)bm + BPMMGR_OFF_BFX_BPM);
          if (b != *(volatile unsigned int *)(uintptr_t)BPMMGR_INVALID_BPM)
               v->bpm_x100 = (int)b;
          v->tap = *(volatile int *)((char *)bm + BPMMGR_OFF_BFX_MODE) == 1;
     }
     return 1;
}

/* Is rbp's Beat FX on: 1, 0, or -1 when rbp is not up. WEAK for the host test. */
__attribute__((weak)) int flx10_fx_on(void)
{
     unsigned int mixer, bem;

     mixer = *(volatile unsigned int *)(uintptr_t)ME_SINGLETON;
     if (!mixer)
          return -1;
     bem = *(volatile unsigned int *)(uintptr_t)(mixer + BEM_IN_MIXER);
     if (!bem)
          return -1;
     return *(volatile unsigned int *)(uintptr_t)(bem + BEM_OFF_TYPE) != BFX_TYPE_OFF;
}

static struct fxs_view fxs_sent;
static int fxs_valid;                 /* fxs_sent is what the unit shows */
static int fxs_ready;                 /* the connect sequence has gone out */
static unsigned long long fxs_at;

static void flx10_fxscreen_tick(unsigned long long t)
{
     struct fxs_view v;
     unsigned char sx[54];

     if (!fxs_ready || t < fxs_at)
          return;
     fxs_at = t + FXS_POLL_MS;
     if (!midi_out_ready() || !flx10_fx_read(fxsel_pos, &v))
          return;
     if (v.pos >= 0 && (!fxs_valid || v.pos != fxs_sent.pos))
          midi_sysex_out(sx, (unsigned)flx10_fx_name_sysex(rbp_fx_name[v.pos], sx));
     if (v.pos >= 0 && (!fxs_valid || strcmp(v.beat, fxs_sent.beat)))
          midi_sysex_out(sx, (unsigned)flx10_fx_beat_sysex(v.beat, sx));
     if (!fxs_valid || v.bpm_x100 != fxs_sent.bpm_x100 || v.tap != fxs_sent.tap)
          midi_sysex_out(sx, (unsigned)flx10_fx_bpm_sysex(v.bpm_x100, v.tap, sx));
     if (v.pos < 0 && fxs_valid)
          v.pos = fxs_sent.pos;         /* nothing known: keep what is shown */
     fxs_sent = v;
     fxs_valid = 1;
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

/* ---- pad modes ------------------------------------------------------------
 * rbp's pad modes are four keys and a second bank behind each. Pressing a mode's
 * key while rbp is already in that mode flips the bank instead of re-entering:
 * PlayerInnards::onKey_HotCue @0x3030ec flips +0x79, onKey_AutoBeatLoop +0x7a,
 * onKey_SlipBeatLoop +0x7b, onKey_BeatJumpLoopMove +0x7c; entering a mode from
 * another clears its bank. PlayerInnards::getStat publishes the result as the
 * pad-mode byte rbp_led.c already reads (UiGetPadMode's walk, rbp_abi.h):
 *
 *   0 HOT CUE   1 BEAT LOOP   2 SLIP LOOP    3 BEAT JUMP
 *   4 GATE CUE  5 BEAT LOOP 2 6 RELEASE FX   7 BEAT JUMP 2
 *
 * The four main buttons behave as the RX3's own do: one press of the mode's key,
 * so pressing the button of the mode already showing clicks through to its
 * second bank and back (operator's preference, 2026-10-24 -- an earlier version
 * made it do nothing). The SHIFT buttons ask for a second bank directly, and
 * the map sends however many presses get rbp there from where it is: none, one
 * (flip), or two (enter, then flip).
 *
 * flx10_pad_mode() is WEAK so the host test can answer for rbp; -1 = not up. */
__attribute__((weak)) int flx10_pad_mode(int deck)
{
     void *holder, *sw, *pd;

     holder = *(void **)UI_PADMODE_HOLDER_GLOBAL;
     if (!holder)
          return -1;
     sw = *(void **)((char *)holder + PADMODE_STATWATCHER_OFF);
     if (!sw)
          return -1;
     pd = *(void **)((char *)sw + (deck ? PADMODE_DECK2_OFF : PADMODE_DECK1_OFF));
     if (!pd)
          return -1;
     return (int)*(unsigned char *)((char *)pd + PADMODE_BYTE_OFF);
}

static const int mode_key[4] = { K_HOTCUE, K_ALOOP, K_SLIPLOOP, K_BEATJUMP };

/* The UNIT'S OWN pad page, per deck, as the note base its pads are sending on.
 * The FLX10 moves its pads (and the only pad LEDs it will show) to a new base
 * whenever one of its eight mode buttons is pressed, whatever rbp does with the
 * press -- so the LED bridge has to light the page the unit is on, not the one
 * rbp's mode would suggest. -1 = no mode button seen yet (the bridge then goes by
 * rbp's mode, below). */
static int flx10_page[2] = { -1, -1 };
static unsigned long long flx10_page_t[2];   /* when the page last changed */

static void flx10_set_page(int deck, int pg)
{
     if (flx10_page[deck] != pg) {
          flx10_page[deck] = pg;
          flx10_page_t[deck] = shim_now_ms();
          led_kick = 3;                          /* repaint the new page soon */
     }
}

/* Which pads are under a finger, and when each was last pressed -- the LED
 * bridge's pad_touch hook (LED_PAD_DIM_BRIGHT). */
static unsigned pad_held[2];
static unsigned long long pad_press_ms[2][8];

int flx10_pad_touch(int deck, int pad, unsigned long long *press_ms)
{
     if (deck < 0 || deck > 1 || pad < 0 || pad > 7) {
          *press_ms = 0;
          return 0;
     }
     *press_ms = pad_press_ms[deck][pad];
     return (pad_held[deck] >> pad) & 1;
}

/* The page after a mode button press, or -1 for "not a mode button". A main
 * button toggles between its page and its SHIFT page, as the unit does on a
 * second press (operator, 2026-10-24: BEAT JUMP twice put the unit's pads on
 * the BEAT LOOP page, 96); a SHIFT button goes straight to its page. */
static int flx10_mode_page(int note, int cur)
{
     int main_pg;

     switch (note) {
     case N_MODE_HOT:      main_pg = 0;  break;
     case N_MODE_PADFX1:   main_pg = 16; break;
     case N_MODE_BEATJUMP: main_pg = 32; break;
     case N_MODE_SAMPLER:  main_pg = 48; break;
     case N_MODE_KEYBOARD: return 64;
     case N_MODE_PADFX2:   return 80;
     case N_MODE_BEATLOOP: return 96;
     case N_MODE_KEYSHIFT: return 112;
     default:              return -1;
     }
     return cur == main_pg ? main_pg + 64 : main_pg;
}

/* struct led_notes' pad_base hook. Before any mode button has been pressed the
 * unit is on whatever page it powered up on, which this side cannot know; rbp's
 * mode, put through the operator's table (docs/21), is the best guess. */
static const int flx10_page_for_rbp[8] = { 0, 96, 16, 32, 64, 48, 80, 112 };

int flx10_led_pad_base(int deck, int rbp_mode)
{
     int pg;

     if (deck < 0 || deck > 1)
          return -1;
     pg = flx10_page[deck];
     if (pg >= 0) {
          /* Wait (up to 300 ms) for rbp to be in this page's mode before the
           * bridge paints it: painting first shows the old mode's lights on
           * the new page for a moment (operator, 2026-10-25). */
          static const int page_mode[8] = { 0, 2, 3, 5, 4, 6, 1, 7 };
          if (rbp_mode != page_mode[(pg >> 4) & 7] &&
              shim_now_ms() - flx10_page_t[deck] < 300)
               return -2;
          return pg;
     }
     return (rbp_mode >= 0 && rbp_mode < 8) ? flx10_page_for_rbp[rbp_mode] : -1;
}

static void flx10_set_mode(int deck, int want)
{
     int cur = flx10_pad_mode(deck);
     int base = want & 3, key = mode_key[want & 3], sch = deck + 1;

     if (cur == want)
          return;
     if (cur >= 0 && cur <= 7 && (cur & 3) == base) {
          pulse(key, sch);                   /* same mode, other bank: flip */
     } else {
          pulse(key, sch);                   /* enter (bank cleared) ... */
          if (want >= 4)
               pulse(key, sch);              /* ... then flip to the second bank */
     }
     if (verbose)
          klog("knobshim2: flx10 deck%d pad mode %d -> %d\n", sch, cur, want);
}

/* A main mode button (operator, 2026-10-24): the first press goes to its mode,
 * a second press does what SHIFT + that button does, a third comes back -- the
 * FLX10's own double-press behaviour under rekordbox. `a` is the button's mode,
 * `b` its SHIFT mode, both as rbp's mode byte. */
static void flx10_main_mode(int deck, int a, int b)
{
     flx10_set_mode(deck, flx10_pad_mode(deck) == a ? b : a);
}

/* ---- notes ---------------------------------------------------------------- */

/* SHIFT + BACK's way back from the playlist view (see flx10_note) */
#define SB_AGAIN_MS 200
static int sb_opened;
static unsigned long long sb_again_at;

static void flx10_keysync_press(int deck);

static void flx10_note(int ch, int note, int on)
{
     int deck = IS_DECK(ch) ? ch : -1;

     if (ch == CH_MIX && flx10_panel_note(note, on))
          return;

     /* A pad hit says exactly which page the unit is on: the backstop for
      * any mode change the toggle above gets wrong. */
     if ((ch == CH_PADS1 || ch == CH_PADS1S || ch == CH_PADS2 ||
          ch == CH_PADS2S) && (note & 15) < 8) {
          int d = (ch == CH_PADS1 || ch == CH_PADS1S) ? 0 : 1, p = note & 7;
          if (on) {
               flx10_set_page(d, note & 0x70);
               led_kick = led_kick > 1 ? led_kick : 1;   /* held / flash: show it now */
               pad_held[d] |= 1u << p;
               pad_press_ms[d][p] = shim_now_ms();
          } else {
               pad_held[d] &= ~(1u << p);
               led_kick = led_kick > 1 ? led_kick : 1;
          }
     }

     if (deck >= 0) {
          int sch = deck + 1;

          if (on) {
               int pg = flx10_mode_page(note, flx10_page[deck]);
               if (pg != -1)
                    flx10_set_page(deck, pg);
          }

          if (note == N_JOG_TOUCH || note == N_JOG_TOUCH_SH)
               flx10_jog_touched[deck] = on;     /* and on to its table row */
          if (note == N_SLIP && on)
               rev_slip_check_at[deck] = 0;      /* the operator's own SLIP: hands off */
          if (note == N_LINK && deck < 2) {      /* the USB stick's eject (LINK above) */
               link_held[deck] = on;
               if (on) {
                    link_at[deck] = shim_now_ms();
                    link_fired[deck] = 0;
               }
               return;
          }
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
               /* the hold's own release -- a lone 88 00 after a 120, in the
                * key log on the unit 2026-10-26 */
               if (sync_long_at[deck] &&
                   shim_now_ms() - sync_long_at[deck] < SYNC_LONG_TAP_MS) {
                    sync_long_at[deck] = 0;
                    return;
               }
               if (!deck_sync[deck])
                    flx10_sync_fit_range(deck);
               pulse(K_SYNC, sch);
               return;
          }
          case N_SYNC_LONG:
               /* BEAT SYNC held: the FLX10 holds the button itself and says so
                * with note 120, no 88 while held */
               if (on) {
                    sync_long_at[deck] = shim_now_ms();
                    flx10_inst_doubles(deck, "SYNC held");
               }
               return;
          case N_SYNC_SHIFT:
               if (on)
                    pulse(K_MASTER, sch);
               return;
          case N_TEMPO_RANGE:
               if (on)
                    flx10_tempo_range_press(deck);
               return;
          case N_TEMPO_RESET:
               if (on)
                    flx10_tempo_reset(deck);
               return;
          case N_XF_A: case N_XF_B: case N_XF_THRU:
               if (on)
                    flx10_xf_assign(deck, note);
               return;
          case N_CUE_SHIFT: case N_MIXPOINT_PREV: case N_MIXPOINT_NEXT:
               flx10_track_note(deck, note, on);
               return;
          case N_BJ_PREV:       if (on) flx10_jump(deck, -4);  return;
          case N_BJ_NEXT:       if (on) flx10_jump(deck, 4);   return;
          case N_BJ_PREV_SHIFT: if (on) flx10_jump(deck, -16); return;
          case N_BJ_NEXT_SHIFT: if (on) flx10_jump(deck, 16);  return;
          case N_SLIP_REVERSE:
               flx10_slip_reverse(deck, on);
               return;
          case N_REVERSE:
               if (on)
                    flx10_reverse_latch(deck);
               return;
          /* KEY SYNC (operator, 2026-10-25). The unit decides short from long
           * itself: a short press sends 101 on/off, a hold sends 26 on at the
           * threshold and 26 off + 101 off on release (no 101 on) -- measured
           * with midi-learn.py. The hold is MASTER TEMPO. The short press is
           * KEY SYNC, which rbp does not have, so the shim does it (keyshift.c,
           * flx10_keysync below; operator, 2026-10-26: latched, like a CDJ). */
          case N_KEYSYNC:
               if (on)
                    flx10_keysync_press(deck);
               return;
          case N_4BEAT_SHIFT:
               if (on)
                    flx10_toggle_time_mode(deck);
               return;
          case N_KEYSYNC_LONG:
               if (on)
                    pulse(K_MT, sch);
               return;
          case N_IN: case N_OUT: case N_4BEAT:
          case N_IN_SHIFT: case N_OUT_SHIFT:
               flx10_loop(deck, note, on);
               return;
          /* Pad modes (operator's table, docs/21), by rbp's mode byte:
           *   button      press           SHIFT / second press
           *   HOT CUE     0 HOT CUE       4 GATE CUE
           *   PAD FX1     2 SLIP LOOP     6 RELEASE FX
           *   BEAT JUMP   3 BEAT JUMP     1 BEAT LOOP
           *   SAMPLER     5 BEAT LOOP 2   7 BEAT JUMP 2 */
          case N_MODE_HOT:      if (on) flx10_main_mode(deck, 0, 4); return;
          case N_MODE_PADFX1:   if (on) flx10_main_mode(deck, 2, 6); return;
          case N_MODE_BEATJUMP: if (on) flx10_main_mode(deck, 3, 1); return;
          case N_MODE_SAMPLER:  if (on) flx10_main_mode(deck, 5, 7); return;
          case N_MODE_KEYBOARD: if (on) flx10_set_mode(deck, 4); return;
          case N_MODE_PADFX2:   if (on) flx10_set_mode(deck, 6); return;
          case N_MODE_BEATLOOP: if (on) flx10_set_mode(deck, 1); return;
          case N_MODE_KEYSHIFT: if (on) flx10_set_mode(deck, 7); return;
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
          /* VIEW (operator, 2026-10-25): from the player it opens SOURCE;
           * on the SOURCE menu it is SOURCE again, which closes it (BROWSE
           * there opened the source's browser, and BACK did nothing); from
           * anywhere else (a browse list) it is
           * the RX3's BROWSE key, which toggles back to the player. Decided on
           * the press, and the release goes to the same key. */
          /* SHIFT + BACK (operator, 2026-10-25): from the player it is the
           * RX3's PLAYLIST key, and pressed again from the list that opened it
           * goes back to the player (BROWSE, as VIEW does); from any other
           * browse screen it stays TAG LIST. Decided on the press; the release
           * goes to the same key. */
          if (note == N_BACK_SHIFT) {
               static int sb_key;
               if (on) {
                    int bm = flx10_browse_mode();
                    if (bm == BROWSE_MODE_PLAY) {
                         sb_key = K_PLAYLIST;
                         sb_opened = 1;
                    } else if (sb_opened) {
                         /* BROWSE from the playlist view opens the browse
                          * screen (operator, 2026-10-25); a second BROWSE from
                          * there is the player, so the tick sends it if the
                          * first did not get there */
                         sb_key = K_BROWSE;
                         sb_opened = 0;
                         sb_again_at = shim_now_ms() + SB_AGAIN_MS;
                    } else {
                         sb_key = K_TAGLIST;
                    }
                    send_rx_key(sb_key, OP_PRESS, CH_GLOBAL, 0);
               } else if (sb_key) {
                    send_rx_key(sb_key, OP_RELEASE, CH_GLOBAL, 0);
                    sb_key = 0;
               }
               return;
          }
          if (note == N_VIEW) {
               static int view_key;
               if (on) {
                    int bm = flx10_browse_mode();
                    view_key = (bm == BROWSE_MODE_PLAY || bm == BROWSE_MODE_SOURCE)
                               ? K_SOURCE : K_BROWSE;
                    send_rx_key(view_key, OP_PRESS, CH_GLOBAL, 0);
               } else if (view_key) {
                    send_rx_key(view_key, OP_RELEASE, CH_GLOBAL, 0);
                    view_key = 0;
               }
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
          /* ON/OFF: the FLX10 latches this button itself -- each push sends ONE
           * message, 7f when its light goes to flashing and 00 when it stops,
           * never a press and a release (MIDI dump on the unit, 2026-10-26:
           * 00, 7f, 00, 7f, ... one per push). So the velocity is the state
           * wanted, and rbp's BFX key -- whose down TOGGLES:
           * MasterInnards::onEv_BeatEffectEnable gets 2 from
           * uif::MixerSkeleton::onKeyEvent @0x2c9f0c -- is tapped only when rbp
           * is not already there. Button and rbp then agree after every push,
           * whatever happened before; and from its reports, at start. */
          if (note == N_FX_ONOFF) {
               unsigned long long now = shim_now_ms();
               int is_on = flx10_fx_on();
               /* The unit's answer to the controls query -- every 2 s for
                * rbp_vu.c's first 30 s -- REPORTS the latch the same way (94 46
                * 7f while its light flashes), so rbp takes it from there at
                * start: the button is the truth, and it survives rbp's restarts. */
               int report = midi_query_at_ms() && now - midi_query_at_ms() < 300;
               if (is_on < 0 || is_on == on)
                    return;
               pulse(K_BFX, CH_GLOBAL);
               klog("knobshim2: flx10 Beat FX ON/OFF -> %s%s\n", on ? "on" : "off",
                    report ? " (the unit's report)" : "");
               return;
          }
          if (note >= N_FX_SEL_FIRST && note <= N_FX_SEL_LAST) {
               if (on)
                    flx10_fxsel(note);
               return;
          }
          if (note >= N_FX_CH_FIRST && note <= N_FX_CH_LAST) {
               if (on)
                    flx10_fxch(note);
               return;
          }
          if (note == N_FX_AUTO) {
               if (on) {
                    flx10_bfx_bpm_auto();
                    klog("knobshim2: flx10 Beat FX BPM -> AUTO\n");
               }
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

     /* SHIFT + a HOT CUE or GATE CUE pad (page 0 / 64): delete that cue, as
      * rbp's own gesture -- its SHIFT key held round the pad key (rbp_abi.h,
      * K_SHIFT). hotcue_delete()'s direct calls (rbp_bridge.c) took only on a
      * playing deck and left the cue drawn on rbp's screen and the jog until
      * the next input (operator, 2026-10-26). */
     if ((ch == CH_PADS1S || ch == CH_PADS2S) && (note & ~64) >= 0 && (note & ~64) < 8) {
          int sch = ch == CH_PADS1S ? 1 : 2;
          if (on) {
               send_rx_key(K_SHIFT, OP_PRESS, sch, 0);
               send_rx_key(K_PAD1 + (note & 7), OP_PRESS, sch, 0);
               send_rx_key(K_PAD1 + (note & 7), OP_RELEASE, sch, 0);
               send_rx_key(K_SHIFT, OP_RELEASE, sch, 0);
               if (verbose)
                    klog("knobshim2: flx10 deck%d SHIFT + pad %d: hot cue delete\n",
                         sch, (note & 7) + 1);
          }
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
               flx10_jog(ch, cc, val);
               return;
          }
     } else if (ch == CH_MIX) {
          if ((cc == CC_BROWSE || cc == CC_BROWSE_SHIFT) && panel_open()) {
               panel_cmd(val < 64 ? SR_NEXT : SR_PREV);   /* 0x01 down, 0x7F up */
               return;
          }
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
                    /* per mixer channel, like COLOR: both channels follow the
                     * one knob, as the RX3's single PARAMETER knob does */
                    int v = cc_to_10bit(val);
                    send_rx_key_f(k_param, OP_VALUE, 1, v, (float)v / 1023.0f);
                    send_rx_key_f(k_param, OP_VALUE, 2, v, (float)v / 1023.0f);
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
     {
          const char *g = env_text("JOG_GAIN", NULL);
          double v = (g && *g) ? strtod(g, NULL) : 0.0;
          jog_gain = (v > 0.0) ? v : FLX10_JOG_GAIN;
          g = env_text("JOG_RBP_TURN_MS", NULL);
          v = (g && *g) ? strtod(g, NULL) : 0.0;
          jog_rbp_turn_ms = (v > 0.0) ? v : FLX10_RBP_TURN_MS;
     }
     jog_rev = env_on("JOG_REV", 0);
     jog_idle_ms = env_num("JOG_IDLE_MS", 120);
     if (jog_idle_ms < 10) jog_idle_ms = 10;
     jog_verbose = env_on("JOG_VERBOSE", 0);
     jog_cal = env_on("JOG_CAL", 0);
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
          add_note(rch, N_QUANTIZE, K_QUANTIZE, sch);
          add_note(rch, N_SLIP, K_SLIP, sch);
          /* SHIFT + SLIP (VINYL) is off (operator, 2026-10-25): the button has
           * no light, so there is no telling whether a deck is in vinyl or CDJ
           * mode. Logged, nothing sent; rbp keeps whatever mode it has. */
          add_note(rch, N_VINYL, 0, sch);
          /* The pad-mode buttons are named cases in flx10_note()
           * (flx10_set_mode()). Known and deliberately unbound (key 0): logged,
           * nothing sent. */
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
      * base is bound only where the map puts rbp in a mode for it -- all eight. */
     for (int d = 0; d < 2; d++) {
          int rch = d ? CH_PADS2 : CH_PADS1;
          int sch = 1 + d;
          for (int p = 0; p < 8; p++) {
               add_note(rch,  0 + p, K_PAD1 + p, sch);  /* HOT CUE   -> HOT CUE   */
               add_note(rch, 16 + p, K_PAD1 + p, sch);  /* PAD FX1   -> SLIP LOOP */
               add_note(rch, 32 + p, K_PAD1 + p, sch);  /* BEAT JUMP -> BEAT JUMP */
               add_note(rch, 96 + p, K_PAD1 + p, sch);  /* BEAT LOOP -> BEAT LOOP */
               add_note(rch, 48 + p, K_PAD1 + p, sch);  /* SAMPLER   -> BEAT LOOP 2 */
               add_note(rch, 64 + p, K_PAD1 + p, sch);   /* KEYBOARD  -> GATE CUE    */
               add_note(rch, 80 + p, K_PAD1 + p, sch);   /* PAD FX2   -> RELEASE FX  */
               add_note(rch, 112 + p, K_PAD1 + p, sch);  /* KEY SHIFT -> BEAT JUMP 2 */
          }
     }

     /* Mixer channel. */
     add_abs(CH_MIX, CC_CFX1, K_COLOR, 1);
     add_abs(CH_MIX, CC_CFX2, K_COLOR, 2);
     add_abs(CH_MIX, CC_XFADER, K_XFADER, CH_GLOBAL);
     add_abs(CH_MIX, CC_HP_MIX, K_HPMIX, CH_GLOBAL);
     add_abs(CH_MIX, CC_HP_LEVEL, K_HPLEVEL, CH_GLOBAL);
     add_note(CH_MIX, N_BACK, K_BACK, CH_GLOBAL);
     /* VIEW is a named case in flx10_note() (SOURCE from the player, BROWSE
      * from anywhere else). SHIFT + VIEW is SHORTCUT. */
     /* SHIFT + VIEW: rbp's SHORTCUT on a short press, the shim's SETTINGS panel on
      * a long one -- flx10_panel_note(), ahead of this table. */
     /* SHIFT + browse push: TRACK FILTER. It needs the second edge --
      * map_flx4.c's add_note_repeat() note and rbp_abi.h's OP_REPEAT explain
      * why. 93 is deck 1's knob (measured); deck 2's SHIFT push is unrecorded
      * and logs as unmapped until it is. */
     add_note_repeat(CH_MIX, N_PUSH1_SHIFT, K_TRACKFILTER, CH_GLOBAL);
     add_note(CH_MIX, N_SAMPLER_CUE, 0, CH_GLOBAL);

     /* Beat FX. */
     add_note(CH_FX, N_FX_TAP, K_TAP, CH_GLOBAL);
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
     /* The HDMI mirror's level (flx10_mastervol) starts as the MASTER knob all
      * the way down, not shmstate.c's full: the unit does not report the knob
      * until it moves, and a mirror at full while the knob sits at zero was the
      * loud start (operator, 2026-10-26). */
     g_mirror_gain = 0.0f;
     klog("knobshim2: DDJ-FLX10: routed Ch1->Deck1, Ch2->Deck2; master level unity\n");
}

static void flx10_sb_tick(unsigned long long t)
{
     if (!sb_again_at || t < sb_again_at)
          return;
     sb_again_at = 0;
     if (flx10_browse_mode() != BROWSE_MODE_PLAY)
          pulse(K_BROWSE, CH_GLOBAL);
}

static void flx10_splash_tick(void);
static void flx10_keysync_tick(void);

/* MASTER follows the faders (operator, 2026-10-26: the master stayed on a deck
 * mixed out with its fader). rbp has an on-air hand-over of its own --
 * SyncMaster::updateMaster @0x84634 -- but only for a deck whose entry at
 * [SyncMaster+0x38][deck*8+4] carries 0x10, and neither deck's does on this
 * unit (read live 2026-10-26: 0x1 and 0x0, rbp's mixer otherwise seeing the
 * faders right) -- watched, rbp moved deck 1's master to deck 2 at once and
 * never deck 2's to deck 1. So the other way round is done here, the same way:
 * the moment the master deck goes off air -- its channel fader or the
 * crossfader, as rbp's ON AIR shows it -- while the other deck plays on air,
 * MASTER is pressed on the other deck. And either move, rbp's or this one, is
 * undone when the deck it left is back on air within MASTER_CUT_MS: a quick
 * fader cut leaves the master where it was (operator, 2026-10-26: a wait before
 * moving lagged, none switched on every cut). */
#define MASTER_CUT_MS    500
#define MASTER_RESEND_MS 300     /* one press, then this for rbp to take it */
static void flx10_master_tick(unsigned long long t)
{
     static int last_m = -1, cut_from = -1, ours = -1;
     static unsigned long long cut_at, sent_at;
     int m = deck_master[0] ? 0 : deck_master[1] ? 1 : -1, o = 1 - m;

     if (ours >= 0 && t - sent_at > 1000)
          ours = -1;                     /* rbp never took it */
     /* rbp moved it -- not to where this pressed -- from a deck off air:
      * maybe only a cut */
     if (m >= 0 && last_m >= 0 && m != last_m) {
          if (m == ours)
               ours = -1;
          else if (!deck_on_air[last_m]) {
               cut_from = last_m;
               cut_at = t;
          }
     }
     last_m = m;
     if (sent_at && t - sent_at < MASTER_RESEND_MS)
          return;
     if (cut_from >= 0) {
          if (m < 0 || t - cut_at >= MASTER_CUT_MS ||
              (m == cut_from && deck_on_air[cut_from])) {
               cut_from = -1;
          } else if (deck_on_air[cut_from]) {
               if (m != cut_from) {
                    sent_at = t ? t : 1;
                    ours = cut_from;
                    pulse(K_MASTER, cut_from + 1);
                    klog("knobshim2: flx10 deck%d back on air: a cut, MASTER back to it\n",
                         cut_from + 1);
               }
               cut_from = -1;
               return;
          } else {
               return;
          }
     }
     if (m < 0 || deck_on_air[m] || !deck_loaded[o] || !deck_playing[o] ||
         !deck_on_air[o] || deck_master[o])
          return;
     sent_at = t ? t : 1;
     ours = o;
     cut_from = m;
     cut_at = sent_at;
     pulse(K_MASTER, o + 1);
     klog("knobshim2: flx10 deck%d off air: MASTER to deck%d\n", m + 1, o + 1);
}

static void flx10_tick(void)
{
     unsigned long long t = shim_now_ms();

     flx10_jog_tick();
     flx10_rev_tick(t);
     flx10_sb_tick(t);
     flx10_track_tick(t);
     flx10_push_tick(t);
     flx10_scfx_tick(t);
     flx10_panel_tick(t);
     flx10_link_tick(t);
     flx10_fxscreen_tick(t);
     flx10_splash_tick();
     flx10_keysync_tick();
     flx10_master_tick(t);
     flx10_sync_tick(t);
     flx10_range_tick(t);
}

static void flx10_unit_setting(int ch, int is_cc, int num, int val);

/* ---- KEY SYNC ---------------------------------------------------------------
 * rbp has no key sync; keyshift.c (rx3-toolkit's key shift) shifts a deck's key
 * by whole semitones in rbp's audio path, and this decides by how much -- the
 * CDJ-3000's rule as its documentation describes it: the deck goes to whichever
 * key HARMONICALLY COMPATIBLE with the MASTER's needs the least change, where
 * compatible is the same key, the dominant, the subdominant, the relative, and
 * the relatives of the dominant and subdominant -- on the Camelot wheel, the
 * master's number or one either side, either letter (operator, 2026-10-26: "10B
 * and 11B should count as synced"). So no shift is ever more than 2 semitones.
 *
 *   the LED is lit while the deck is compatible -- by its own key, or by a shift
 *     of ours -- and latched; a track loaded already compatible lights it with
 *     no shift (operator: "when we load a track with a key that's already
 *     matched: light key sync up")
 *   a press on an unlit deck shifts it and latches; a press on a lit one undoes a
 *     shift of ours, and does nothing when there is none to undo (operator: "i can
 *     turn key sync off in this state which i don't think should be possible")
 *   a latched deck follows the master's key; a new track lets the latch go
 *
 * A NEW TRACK is a key or length that differs from the last one seen and has held
 * for KS_SETTLE_MS: rbp fills a load in in stages, and acting on each stage lit,
 * cleared and relit the LED (operator, 2026-10-26).
 *
 * WHAT A DECK PLAYS IN is its key's root (flx10_key_code's pairs: a major key and
 * its relative minor share one, and one Camelot number), plus any shift of ours,
 * plus -- with MASTER TEMPO off -- the semitones its speed moves the pitch. WEAK,
 * so a build or a test without keyshift.o has no key sync. */
__attribute__((weak)) int keyshift_set(int deck, int semitones);
__attribute__((weak)) int keyshift_get(int deck);
__attribute__((weak)) void keyshift_reload(int deck);

#define KS_SETTLE_MS 400

static int ks_latched[2];
static int ks_led[2] = { -1, -1 };
static char ks_seen_key[2][16];            /* the track on the deck: its key... */
static int ks_seen_total[2];               /* ...and its length */
static char ks_pend_key[2][16];            /* a change not yet held KS_SETTLE_MS */
static int ks_pend_total[2];
static unsigned long long ks_pend_at[2];

int flx10_key_code(const char *k);

/* The root, 0..11, of rbp's key text (major keys chromatically from C, a minor
 * key on its relative major's), or -1. */
int flx10_key_root(const char *k)
{
     int c = flx10_key_code(k);
     return c ? (c - 1) / 2 : -1;
}

/* Semitones a tempo of `tempo_x100` % moves the pitch by, with MASTER TEMPO off --
 * the jog's own rounding (flx10_key_shift), so the key the jog shows and the key
 * KEY SYNC matches are one key. */
int flx10_tempo_semis(int tempo_x100, int mt)
{
     return mt ? 0 : flx10_key_shift(tempo_x100);
}

/* A root `d` semitones from the master's: 0 the same Camelot number (the key or
 * its relative), 7 and 5 one number up and down (dominant, subdominant). */
static int ks_compatible(int d)
{
     d = ((d % 12) + 12) % 12;
     return d == 0 || d == 5 || d == 7;
}

/* The least shift that makes a deck whose root is `root` (its speed moving it `t`)
 * compatible with a master playing in `target`: 0 when it already is; at each
 * distance the same or relative key before a neighbour, + before -. Pure. */
int flx10_keysync_shift(int root, int t, int target)
{
     for (int dist = 0; dist <= 6; dist++) {
          int fallback = 99;
          for (int sign = 1; sign >= -1; sign -= 2) {
               int n = sign * dist, d = root + t + n - target;
               if (((d % 12) + 12) % 12 == 0)
                    return n;
               if (fallback == 99 && ks_compatible(d))
                    fallback = n;
               if (!dist)
                    break;
          }
          if (fallback != 99)
               return fallback;
     }
     return 0;
}

static int ks_master_of(int deck)
{
     int m = !deck;
     return deck_master[m] && !deck_master[deck] && deck_loaded[m] ? m : -1;
}

/* The shift deck `deck` needs from its own key, or 99 when there is nothing to
 * match: no master, or a key not known. */
static int ks_wanted(int deck)
{
     int m = ks_master_of(deck), root = flx10_key_root(deck_key[deck]), mroot;

     if (m < 0 || root < 0 || !deck_loaded[deck])
          return 99;
     mroot = flx10_key_root(deck_key[m]);
     if (mroot < 0)
          return 99;
     mroot += (keyshift_get ? keyshift_get(m) : 0) + flx10_tempo_semis(deck_speed[m], deck_mt[m]);
     return flx10_keysync_shift(root, flx10_tempo_semis(deck_speed[deck], deck_mt[deck]),
                                ((mroot % 12) + 12) % 12);
}

static void ks_release(int deck, const char *why)
{
     ks_latched[deck] = 0;
     if (keyshift_set)
          keyshift_set(deck, 0);
     klog("knobshim2: flx10 deck%d KEY SYNC off (%s)\n", deck + 1, why);
}

static void flx10_keysync_press(int deck)
{
     int n;

     if (deck > 1)
          return;
     if (ks_latched[deck] || (ks_wanted(deck) == 0 && !(keyshift_get && keyshift_get(deck)))) {
          if (keyshift_get && keyshift_get(deck)) {
               ks_release(deck, "pressed again");
          } else if (!ks_latched[deck] && keyshift_set) {
               ks_latched[deck] = 1;          /* compatible already: lit, now latched */
               klog("knobshim2: flx10 deck%d KEY SYNC: already compatible\n", deck + 1);
          }
          return;
     }
     if (!keyshift_set) {
          klog("knobshim2: flx10 deck%d KEY SYNC: no key shift in this build\n", deck + 1);
          return;
     }
     n = ks_wanted(deck);
     if (n == 99) {
          klog("knobshim2: flx10 deck%d KEY SYNC: nothing to match (key '%s', master %s)\n",
               deck + 1, deck_key[deck], ks_master_of(deck) < 0 ? "none" : "the other deck");
          return;
     }
     if (keyshift_set(deck, n) != 0) {
          klog("knobshim2: flx10 deck%d KEY SYNC: the key shift is not hooked in\n", deck + 1);
          return;
     }
     ks_latched[deck] = 1;
     /* the track it latched on, so the tick does not take it for a new one */
     memcpy(ks_seen_key[deck], deck_key[deck], sizeof ks_seen_key[deck]);
     ks_seen_total[deck] = deck_total_ms[deck];
     klog("knobshim2: flx10 deck%d KEY SYNC on: %s %+d -> deck%d's %s\n", deck + 1,
          deck_key[deck], n, ks_master_of(deck) + 1, deck_key[ks_master_of(deck)]);
}

/* Has deck `d` got a new track, held KS_SETTLE_MS? */
static int ks_new_track(int d, unsigned long long t)
{
     if (!deck_loaded[d] || !deck_key[d][0] ||
         (!strcmp(ks_seen_key[d], deck_key[d]) && ks_seen_total[d] == deck_total_ms[d])) {
          ks_pend_at[d] = 0;
          return 0;
     }
     if (!ks_pend_at[d] || strcmp(ks_pend_key[d], deck_key[d]) ||
         ks_pend_total[d] != deck_total_ms[d]) {
          memcpy(ks_pend_key[d], deck_key[d], sizeof ks_pend_key[d]);
          ks_pend_total[d] = deck_total_ms[d];
          ks_pend_at[d] = t ? t : 1;
          return 0;
     }
     if (t - ks_pend_at[d] < KS_SETTLE_MS)
          return 0;
     memcpy(ks_seen_key[d], deck_key[d], sizeof ks_seen_key[d]);
     ks_seen_total[d] = deck_total_ms[d];
     ks_pend_at[d] = 0;
     return 1;
}

static void flx10_keysync_tick(void)
{
     unsigned long long t = shim_now_ms();

     for (int d = 0; d < 2; d++) {
          int v, want;

          if (ks_new_track(d, t)) {
               if (ks_latched[d]) {
                    ks_release(d, "a new track");
                    if (keyshift_reload)
                         keyshift_reload(d);
               }
               if (keyshift_set && ks_wanted(d) == 0) {
                    ks_latched[d] = 1;
                    klog("knobshim2: flx10 deck%d KEY SYNC on: %s already compatible with "
                         "deck%d's %s\n", d + 1, deck_key[d], ks_master_of(d) + 1,
                         deck_key[ks_master_of(d)]);
               }
          } else if (ks_latched[d] && !deck_loaded[d]) {
               ks_release(d, "the deck is empty");
          } else if (ks_latched[d]) {
               int n = ks_wanted(d);
               if (n != 99 && keyshift_get && n != keyshift_get(d)) {
                    keyshift_set(d, n);
                    klog("knobshim2: flx10 deck%d KEY SYNC follows the master: %+d\n",
                         d + 1, n);
               }
          }
          want = ks_wanted(d);
          v = ks_latched[d] || (want == 0 && !(keyshift_get && keyshift_get(d))) ? 0x7F : 0x00;
          if (fxs_ready && v != ks_led[d] && midi_note(d, N_KEYSYNC, v))
               ks_led[d] = v;          /* only once the connect sequence is out */
     }
}

/* ---- the start-up splash ---------------------------------------------------
 * While fbshim's splash covers rbp's first seconds (splash.h) the controller is
 * covered too (operator, 2026-10-26): its input goes nowhere -- the unit's own
 * display settings are still recorded, the connect sequence needs them -- and the
 * LED bridge sends nothing (rbp_led.c). When it ends the unit is asked again for
 * every absolute control, so a fader or switch moved meanwhile, or reported in
 * the dump the splash swallowed, is where the hardware says. WEAK: no fbshim.so,
 * no splash. */
__attribute__((weak, visibility("default"))) int shim_splash_active(void);

static int splash_on(void)
{
     return shim_splash_active && shim_splash_active();
}

static void flx10_splash_tick(void)
{
     static int was = -1;
     int on = splash_on();

     if (was == 1 && !on) {
          klog("knobshim2: FLX10: splash over -- controller live, asking for its "
               "controls again\n");
          led_query_absolute();
          led_kick = 2;
     }
     was = on;
}

static void flx10_event(const struct snd_seq_event *ev)
{
     if (ev->type == SNDRV_SEQ_EVENT_CONTROLLER)
          flx10_unit_setting(ev->data.control.channel, 1,
                             (int)ev->data.control.param, ev->data.control.value);
     else if (ev->type == SNDRV_SEQ_EVENT_NOTEON || ev->type == SNDRV_SEQ_EVENT_NOTEOFF)
          flx10_unit_setting(ev->data.note.channel, 0, ev->data.note.note,
                             ev->type == SNDRV_SEQ_EVENT_NOTEON ? ev->data.note.velocity : 0);
     if (splash_on())
          return;
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

/* ---- the jog displays ------------------------------------------------------
 *
 * Pioneer's own MIDI list (DDJ-FLX10_MIDI_Message_List_E1, "Illumination
 * Control") gives a display channel, MIDI ch 16, whose fields the unit renders
 * itself: no HID, no session. d = deck - 1.
 *   BPM            CC 0x14+d / 0x34+d   BPM x 10, 14-bit (0.0 .. 999.9)
 *   playing speed  CC 0x18+d / 0x38+d   (percent + 100) x 10, 14-bit
 *   time           CC 0x42+2d minutes, 0x43+2d seconds, 0..99 each
 *   time mode      Note 0x14+d          0x7F = remaining (the unit draws the -)
 *   marker         CC 0x10+d / 0x30+d   0..359 degrees (the clock arm; shown or
 *                                       hidden per deck on the unit itself, with
 *                                       a double tap of SHIFT)
 *   (cue marker CC 0x1C+d / 0x3C+d is in the list too; not sent yet)
 *   MASTER / SYNC  Note 0x18+d / 0x1C+d
 *   master tempo   Note 0x20+d
 *   show info      Note 0x5D+d          0x00 show, 0x7F hide
 * After rekordbox's connect sequence the screens also need a per-deck HID state
 * packet or they say "Not Loaded" (flx10_hid.c). The tempo range box, the
 * waveform and the artwork are HID too and not driven yet. Values go out on change, and all of them once a second. */
#define JD_CH 15
struct jd_cache { int v[24]; unsigned long long refresh; };
static struct jd_cache jd[2];

static void jd_cc(int deck, int slot, int cc, int val, int force)
{
     if (!force && jd[deck].v[slot] == val)
          return;
     if (midi_cc(JD_CH, cc, val))
          jd[deck].v[slot] = val;
}

static void jd_note(int deck, int slot, int note, int val, int force)
{
     if (!force && jd[deck].v[slot] == val)
          return;
     if (midi_note(JD_CH, note, val))
          jd[deck].v[slot] = val;
}

static void jd_cc14(int deck, int slot, int msb, int lsb, int val, int force)
{
     if (val < 0) val = 0;
     if (val > 16383) val = 16383;
     if (!force && jd[deck].v[slot] == val)
          return;
     if (midi_cc(JD_CH, msb, (val >> 7) & 0x7f) &&
         midi_cc(JD_CH, lsb, val & 0x7f))
          jd[deck].v[slot] = val;
}

/* rbp's key text ("11B", "F#m", "Ebm", ...) as the FLX10's key code (CC 0x49
 * on the deck channel), 0 = none. The code table, read off the unit by sending
 * each code (2026-10-25): the major keys in chromatic order from C, each
 * followed by its relative minor -- 1 8B (C), 2 8A (Am), 3 3B (Db), 4 3A,
 * 5 10B, 6 10A, 7 5B, 8 5A, 9 12B, 10 12A, 11 7B, 12 7A, 13 2B, 14 2A,
 * 15 9B, 16 9A, 17 4B, 18 4A, 19 11B, 20 11A, 21 6B, 22 6A, 23 1B, 24 1A. */
int flx10_key_code(const char *k)
{
     /* Camelot number of the major key on each code pair, in code order */
     static const int camelot_major[12] = { 8, 3, 10, 5, 12, 7, 2, 9, 4, 11, 6, 1 };
     /* semitone (C = 0) -> Camelot number, major / minor */
     static const int maj[12] = { 8, 3, 10, 5, 12, 7, 2, 9, 4, 11, 6, 1 };
     static const int min[12] = { 5, 12, 7, 2, 9, 4, 11, 6, 1, 8, 3, 10 };
     static const int semis[7] = { 9, 11, 0, 2, 4, 5, 7 };   /* A..G */
     int num = 0, minor = 0, i;

     if (!k || !k[0])
          return 0;
     if (k[0] >= '0' && k[0] <= '9') {                 /* Camelot: 1..12 A/B */
          num = k[0] - '0';
          i = 1;
          if (k[1] >= '0' && k[1] <= '9')
               num = num * 10 + (k[i++] - '0');
          if (k[i] == 'A' || k[i] == 'a')
               minor = 1;
          else if (k[i] != 'B' && k[i] != 'b')
               return 0;
     } else {                                          /* classic: C, F#m, Ebm */
          int s;
          char c = k[0];
          if (c >= 'a' && c <= 'g')
               c = (char)(c - 'a' + 'A');
          if (c < 'A' || c > 'G')
               return 0;
          s = semis[c - 'A'];
          i = 1;
          if (k[i] == '#') { s++; i++; }
          else if (k[i] == 'b') { s += 11; i++; }
          s %= 12;
          if (k[i] == 'm' || k[i] == 'M')
               minor = 1;
          num = minor ? min[s] : maj[s];
     }
     if (num < 1 || num > 12)
          return 0;
     for (i = 0; i < 12; i++)
          if (camelot_major[i] == num)
               break;
     /* a minor key's code is the pair whose MAJOR shares its Camelot number */
     return 2 * i + (minor ? 2 : 1);
}

/* The jog's BPM as rekordbox sends it (captured 2026-10-25): SysEx
 *   F0 00 40 05 00 00 04 01 00 <0x11 + deck> n5 n4 n3 n2 n1 n0 F7
 * where n5..n0 are the hex digits of BPM x 100, high first (82.00 =
 * 00 00 02 00 00 08), and 0F x6 blanks it. This is what carries the second
 * decimal -- the CC field (BPM x 10) cannot. Ignored outside rekordbox's HID
 * session, so unused until the HID phase. bpm_x100 <= 0 = blank. Fills 17
 * bytes; pure, for the test. */
void flx10_bpm_sysex(int deck, int bpm_x100, unsigned char *out)
{
     static const unsigned char head[9] = {
          0xF0, 0x00, 0x40, 0x05, 0x00, 0x00, 0x04, 0x01, 0x00 };
     memcpy(out, head, sizeof head);
     out[9] = (unsigned char)(0x11 + deck);
     for (int k = 0; k < 6; k++)
          out[10 + k] = bpm_x100 > 0
               ? (unsigned char)((bpm_x100 >> (4 * (5 - k))) & 0x0F) : 0x0F;
     out[16] = 0xF7;
}

/* The display values, worked out of rbp's raw readings. Pure, for the test. */
void flx10_jog_values(const struct deck_info *di, int *bpm10, int *speed10,
                      int *min, int *sec, int *deg)
{
     int rem = di->total_ms - di->elapsed_ms;
     if (rem < 0)
          rem = 0;
     /* x100 -> x10, TRUNCATED as rbp's own screen does: 100.6 at +16% is
      * 116.69, which the RX3 shows as 116.6 (rounding gave 116.7) */
     *bpm10 = di->bpm_raw / 10;
     if (*bpm10 > 9999)
          *bpm10 = 9999;
     *speed10 = di->tempo_raw / 10 + 1000;       /* % x100 -> (% + 100) x 10 */
     *min = rem / 60000;
     if (*min > 99)
          *min = 99;
     *sec = (rem / 1000) % 60;
     /* a record at 33 1/3 rpm: one turn per 1.8 s of play */
     *deg = di->elapsed_ms > 0 ? (int)((di->elapsed_ms % 1800) * 360 / 1800) : 0;
}

/* rekordbox's connect sequence for the display (tools/captures/
 * flx10-rekordbox-startup.txt, the first ~830 ms, one copy of each, LED clears
 * left out): without it, after a power cycle, the jog's clock arm is gone and
 * the marker CCs move nothing; with the unit switched over from rekordbox it
 * was there (operator, 2026-10-25). Which of these turns the arm on is not
 * narrowed down -- all of it is what rekordbox sends, so all of it is sent,
 * each time the unit is (re)connected -- midi_surface_generation() moving --
 * FLX10_INIT_DELAY1_MS and again FLX10_INIT_DELAY2_MS after it is seen: the
 * first gives udev's flx10-unlock.py run (99-flx10-unlock.rules) time to land,
 * the second covers a unit still booting its displays. Not on a timer: a replay
 * while connected blanks the BPM fields for a tick for nothing. The 11..14 0F x6
 * SysEx blank the per-deck BPM fields as rekordbox does.
 *
 * 9F 70 00 goes first and the rest FLX10_INIT_REST_MS later, as rekordbox does:
 * it asks the unit for its own display settings (in the capture the unit
 * answers 1.3 ms later with 9F 3C..4E / BF 41..4A -- the clock arm on/off,
 * toggled on the unit with a double tap of SHIFT, is one of them). Those are
 * recorded (flx10_unit_setting) and sent back in place of the captured values,
 * so a reconnect does not reset what the operator set on the unit. */
static const unsigned char flx10_init_sx_0a[] = {
     0xF0, 0x00, 0x40, 0x05, 0x00, 0x00, 0x04, 0x01, 0x00, 0x00, 0x0A, 0x00, 0x28,
     0x00, 0x26, 0x00, 0x1A, 0x3D, 0x12, 0x14, 0x29, 0x55, 0x29, 0x20, 0x28, 0x49,
     0x4A, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
     0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
     0x00, 0xF7 };
static const unsigned char flx10_init_sx_0b[] = {
     0xF0, 0x00, 0x40, 0x05, 0x00, 0x00, 0x04, 0x01, 0x00, 0x00, 0x0B, 0x2B, 0x68,
     0x00, 0x00, 0x00, 0x00, 0xF7 };
static const unsigned char flx10_init_sx_0c[] = {
     0xF0, 0x00, 0x40, 0x05, 0x00, 0x00, 0x04, 0x01, 0x00, 0x00, 0x0C, 0x00, 0x00,
     0x02, 0x0E, 0x0E, 0x00, 0x00, 0x00, 0xF7 };
/* ch 16 notes (0x9F) and CCs (0xBF), in rekordbox's order */
static const unsigned char flx10_init_short[][3] = {
     { 0x9F, 0x70, 0x00 },
     { 0x9F, 0x40, 0x7F }, { 0x9F, 0x41, 0x00 }, { 0x9F, 0x42, 0x7F },
     { 0xBF, 0x46, 0x30 }, { 0xBF, 0x3E, 0x20 }, { 0x9F, 0x3C, 0x00 },
     { 0x9F, 0x3D, 0x00 }, { 0x9F, 0x3F, 0x7F }, { 0xBF, 0x0D, 0x00 },
     { 0xBF, 0x41, 0x0A }, { 0xBF, 0x42, 0x00 }, { 0x9F, 0x4D, 0x7F },
     { 0x9F, 0x47, 0x00 }, { 0xBF, 0x43, 0x00 }, { 0x9F, 0x48, 0x7F },
     { 0x9F, 0x49, 0x7F }, { 0x9F, 0x4A, 0x7F }, { 0x9F, 0x4E, 0x7F },
     { 0xBF, 0x44, 0x30 }, { 0x9F, 0x45, 0x7F }, { 0x9F, 0x4C, 0x7F },
     { 0xBF, 0x47, 0x20 }, { 0xBF, 0x49, 0x20 }, { 0xBF, 0x48, 0x40 },
     { 0xBF, 0x4A, 0x20 },
};
#define FLX10_INIT_DELAY1_MS 1000
#define FLX10_INIT_DELAY2_MS 4000
#define FLX10_INIT_REST_MS   300

/* The unit's own display settings as it last reported them, ch 16 numbers
 * 0x3C..0x4E, notes [0] and CCs [1]; -1 = never reported. */
#define UNIT_SET_LO 0x3C
#define UNIT_SET_HI 0x4E
static int unit_set[2][UNIT_SET_HI - UNIT_SET_LO + 1] = {
     [0 ... 1] = { [0 ... UNIT_SET_HI - UNIT_SET_LO] = -1 } };

static void flx10_unit_setting(int ch, int is_cc, int num, int val)
{
     if (ch != JD_CH || num < UNIT_SET_LO || num > UNIT_SET_HI)
          return;
     if (unit_set[is_cc][num - UNIT_SET_LO] >= 0 &&     /* a change, not the first report */
         unit_set[is_cc][num - UNIT_SET_LO] != val)
          klog("knobshim2: FLX10: unit setting %s 0x%02X = 0x%02X\n",
               is_cc ? "CC" : "note", num, val);
     unit_set[is_cc][num - UNIT_SET_LO] = val;
}

/* What to send for one of rekordbox's setup messages: the unit's own value
 * when it has reported one, the captured value otherwise. */
static int flx10_setup_value(const unsigned char *m)
{
     int is_cc = (m[0] & 0xF0) == 0xB0;
     if (m[1] >= UNIT_SET_LO && m[1] <= UNIT_SET_HI &&
         unit_set[is_cc][m[1] - UNIT_SET_LO] >= 0)
          return unit_set[is_cc][m[1] - UNIT_SET_LO];
     return m[2];
}

static void flx10_display_init(unsigned long long now)
{
     static int seen_gen;                    /* 0: never connected */
     static unsigned long long at1, at2;     /* 0: nothing pending */
     static unsigned long long rest_at;      /* the rest, after 9F 70 00 */
     int gen = midi_surface_generation();
     unsigned char sx[17];

     if (gen != seen_gen) {
          seen_gen = gen;
          at1 = now + FLX10_INIT_DELAY1_MS;
          at2 = now + FLX10_INIT_DELAY2_MS;
          rest_at = 0;
          klog("knobshim2: FLX10: connection %d; display connect sequence in "
               "%d and %d ms\n", gen, FLX10_INIT_DELAY1_MS, FLX10_INIT_DELAY2_MS);
     }
     if (rest_at && now >= rest_at) {
          rest_at = 0;
     } else {
          if (at1 && now >= at1)
               at1 = 0;
          else if (at2 && !at1 && now >= at2)
               at2 = 0;
          else
               return;
          if (!midi_out_ready()) {            /* route not up yet: try next tick */
               if (!at2) at2 = now + 100;
               return;
          }
          midi_note(JD_CH, 0x70, 0x00);       /* "report your settings" */
          rest_at = now + FLX10_INIT_REST_MS;
          return;
     }
     midi_sysex_out(flx10_init_sx_0a, sizeof flx10_init_sx_0a);
     midi_sysex_out(flx10_init_sx_0b, sizeof flx10_init_sx_0b);
     midi_sysex_out(flx10_init_sx_0c, sizeof flx10_init_sx_0c);
     fxs_ready = 1;                         /* the FX screen is ours from here */
     fxs_valid = 0;
     for (unsigned k = 0; k < sizeof flx10_init_short / sizeof flx10_init_short[0]; k++) {
          const unsigned char *m = flx10_init_short[k];
          if (m[0] == 0x9F && m[1] == 0x70)
               continue;                       /* went out first */
          if ((m[0] & 0xF0) == 0x90)
               midi_note(m[0] & 0x0F, m[1], flx10_setup_value(m));
          else
               midi_cc(m[0] & 0x0F, m[1], flx10_setup_value(m));
     }
     for (int d = 0; d < 4; d++) {
          flx10_bpm_sysex(d, 0, sx);             /* 0F x6: blank, as rekordbox */
          midi_sysex_out(sx, sizeof sx);
     }
     /* the map's own LEDs, which a power-cycled unit has forgotten */
     tempo_reset_led(0);
     tempo_reset_led(1);
     midi_note(0, N_KEYSYNC, 0x00);        /* KEY SYNC: dark, whatever MT is */
     midi_note(1, N_KEYSYNC, 0x00);
     ks_led[0] = ks_led[1] = -1;           /* ...until the latch says (flx10_keysync_tick) */
     rev_led_v[0] = rev_led_v[1] = -1;     /* the tick re-sends them */
     link_led_v[0] = link_led_v[1] = -1;
     /* Every pad page dark, as rekordbox does (all 128 notes on both pad
      * channels in the capture). The unit keeps each page's lights itself and
      * the bridge paints only the page shown, so a page not visited since the
      * last session showed that session's lights on its first visit until the
      * bridge caught up (operator, 2026-10-25). The page the bridge is on is
      * left alone: its cache would not send it again. */
     for (int d = 0; d < 2; d++) {
          int m = flx10_pad_mode(d);
          int cur = flx10_page[d] >= 0 ? flx10_page[d]
                  : (m >= 0 && m < 8) ? flx10_page_for_rbp[m] : -1;
          for (int note = 0; note < 128; note++)
               if (cur < 0 || note < cur || note >= cur + 8)
                    midi_note(d ? CH_PADS2 : CH_PADS1, note, 0x00);
     }
     /* everything the display shows goes out again right after */
     jd[0].refresh = jd[1].refresh = 0;
}

/* The tempo % the jog screen shows (and the key shift with it): the speed
 * the deck really plays at, the playing BPM over the track's own (StatWatcher
 * +0x14 / +0x18). rbp's getTempoX100 is the deck's fader: on a synced deck
 * that is not the master it follows the fader and the jog's bend while the
 * deck plays at the master's BPM (operator, 2026-10-25), and after SYNC is
 * turned off the deck keeps the synced speed until the fader is moved while
 * getTempoX100 says where the fader is -- the unit scales a stacked waveform
 * by this number, so a deck at +36% sent as -2% scrolled at the wrong speed
 * beside a BPM-matched deck (operator, 2026-10-26). Exposed for the test. */
int flx10_shown_tempo(const struct deck_info *di)
{
     long long t;

     if (!di->loaded || di->bpm_raw <= 0 || di->bpm_orig <= 0 || di->bpm_orig >= 0xffff)
          return di->tempo_raw;
     t = (long long)di->bpm_raw * 10000 / di->bpm_orig - 10000;
     if (t < -10000 || t > 10000)
          return di->tempo_raw;          /* not a sane pair: rbp's own */
     return (int)t;
}

static void flx10_deck_display(int deck, const struct deck_info *di)
{
     unsigned long long now = shim_now_ms();
     int force, bpm10, speed10, min, sec, deg, d = deck;

     deck_loaded[deck] = di->loaded;
     deck_elapsed_ms[deck] = di->elapsed_ms;
     deck_total_ms[deck] = di->total_ms;
     deck_bpm[deck] = di->bpm_raw;
     deck_tempo[deck] = di->tempo_raw;
     deck_range[deck] = di->tempo_range;
     deck_sync[deck] = di->sync;
     deck_master[deck] = di->master;
     deck_mt[deck] = di->mt;
     deck_speed[deck] = flx10_shown_tempo(di);
     snprintf(deck_key[deck], sizeof deck_key[deck], "%s", di->key);
     deck_orig[deck] = di->bpm_orig;
     deck_playing[deck] = di->playing;
     deck_on_air[deck] = di->on_air;
     if (deck == 0) {
          flx10_display_init(now);
          flx10_hid_set_pos_reader(flx10_pos_now);
          flx10_hid_set_caution_reader(flx10_end_caution);
          flx10_hid_set_path_reader(flx10_track_path);
          flx10_hid_set_cue_reader(flx10_hot_cues);
     }
     /* the jog screens draw nothing without this (flx10_hid.h); the MIDI
      * values below still go out too */
     {
          struct flx10_hid_deck hs = {
               .loaded = di->loaded, .playing = di->playing,
               .cue_standby = di->cue_standby,
               .elapsed_ms = di->elapsed_ms, .total_ms = di->total_ms,
               .bpm_x100 = di->bpm_raw,
               .tempo_x100 = flx10_shown_tempo(di), .range = di->tempo_range,
               .mt = di->mt, .sync = di->sync,
               .key_code = flx10_key_code(di->key),
               .key_semis = keyshift_get ? keyshift_get(deck) : 0,
               .cue_ms = di->loaded ? flx10_cue_ms(deck, di->total_ms) : -1,
               .looping = di->looping, .master = di->master,
               .jog_touch = flx10_jog_touched[deck],
               .time_remain = di->time_remain,
               .loop_in_ms = di->loop_in_ms, .loop_out_ms = di->loop_out_ms,
               .loop_num = di->loop_num, .loop_den = di->loop_den,
               .on_air = di->on_air,
               .slip = flx10_slip_mode(deck),
          };
          flx10_hid_update(deck, &hs);
     }
     force = now - jd[deck].refresh >= 1000;

     if (force)
          jd[deck].refresh = now;
     jd_note(deck, 0, 0x5D + d, di->loaded ? 0x00 : 0x7F, force);
     jd_note(deck, 1, 0x20 + d, di->mt ? 0x7F : 0x00, force);
     jd_note(deck, 2, 0x18 + d, di->master ? 0x7F : 0x00, force);
     jd_note(deck, 3, 0x1C + d, di->sync ? 0x7F : 0x00, force);
     /* Key display format (Note 0x5B on the DECK's channel -- the list prints
      * status 9F but channel 1/2/3/4, and only the deck channel works,
      * measured 2026-10-25: 0x00 classic, 0x7F Camelot),
      * following the notation rbp's own panel shows: a key text starting with
      * a digit ("3B") is Camelot. Before any key is known, Camelot -- the
      * operator's RX3 shows Camelot. */
     {
          int camelot = di->key[0] ? (di->key[0] >= '0' && di->key[0] <= '9') : 1;
          int v = camelot ? 0x7F : 0x00;
          if (force || jd[deck].v[11] != v) {
               if (midi_note(deck, 0x5B, v))
                    jd[deck].v[11] = v;
          }
     }
     /* The two-decimal BPM SysEx (flx10_bpm_sysex) is NOT sent: the unit
      * ignores it outside rekordbox's HID session -- tried alone and after
      * rekordbox's 00 0A / 0B / 0C setup messages, 2026-10-25. Kept for the
      * HID phase. */
     if (!di->loaded)
          return;
     /* key (CC 0x49) and key change amount (CC 0x4A, 0x0D = 0) on the deck's
      * own channel; slots 12 and 13 of the cache */
     {
          int code = flx10_key_code(di->key);
          if (force || jd[deck].v[12] != code) {
               if (midi_cc(deck, 0x49, code))
                    jd[deck].v[12] = code;
          }
          if (force || jd[deck].v[13] != 0x0D) {
               if (midi_cc(deck, 0x4A, 0x0D))
                    jd[deck].v[13] = 0x0D;
          }
     }
     flx10_jog_values(di, &bpm10, &speed10, &min, &sec, &deg);
     jd_note(deck, 4, 0x14 + d, di->time_remain ? 0x7F : 0x00, force);   /* rbp's time mode */
     jd_cc(deck, 5, 0x42 + 2 * d, min, force);
     jd_cc(deck, 6, 0x43 + 2 * d, sec, force);
     /* The CC BPM (x10, tenths only). Kept: on its own the BPM SysEx is
      * ignored in this mode (sent by hand 2026-10-25, nothing changed), so
      * dropping the CC left the jog frozen on its last value. */
     jd_cc14(deck, 7, 0x14 + d, 0x34 + d, bpm10, force);
     jd_cc14(deck, 8, 0x18 + d, 0x38 + d, speed10, force);
     /* The clock arm (CC 0x10+d / 0x30+d, 0..359 deg), turning once per 1.8 s
      * of play like a record. A first hand test on deck 1 moved nothing, but on
      * deck 2 the arm appeared after a load and followed sent values (2026-10-25)
      * -- so it does work; what hides it on a deck is still open. The cue marker
      * (CC 0x1C+d) stays unsent until there is a cue position to give it. */
     jd_cc14(deck, 9, 0x10 + d, 0x30 + d, deg, force);
}

/* ---- LEDs -----------------------------------------------------------------
 *
 * Measured 2026-10-24 with scripts/odroid/led-probe.py (flx10-leds.json): every
 * LED answers on its own button's input note and channel, ON = 0x7F. The second
 * bank buttons (SHIFT + mode) light their PARENT button flashing -- 105 flashes
 * HOT CUE, 109 BEAT JUMP, 107 PAD FX1, 111 SAMPLER -- which is exactly how the
 * unit shows the shifted mode. BEAT LOOP 2 lives on SAMPLER (34, solid). Pads take the palette (LED_ENC_FLX10_PALETTE) and
 * show only the page the unit is on, hence the pad_base hook. The meters are the
 * FLX4's: CC 2 on the deck's channel, same scale.
 *
 * TEMPO RESET and SLIP REVERSE are lit by the map itself (tempo_reset_led,
 * flx10_rev_tick). Left dark on purpose: PART/stems, PAGE, LINK, the
 * sampler CUE and channels 3/4 -- nothing in rbp says anything for them. */
static const struct led_notes flx10_leds = {
     .deck_ch = 0,                 /* decks 1/2 on channels 0/1 */
     .n_sync = N_SYNC,
     .n_cue = N_CUE,
     .n_play = N_PLAY,
     .n_keylock = -1,              /* KEY SYNC's LED stays dark (operator) */
     .n_vinyl = -1,                /* no VINYL LED */
     .n_slip = N_SLIP,
     .n_loopin = N_IN,
     .n_loopout = N_OUT,
     .n_autoloop = N_4BEAT,
     /* by rbp's mode byte: 0 HOT CUE, 1 BEAT LOOP, 2 SLIP LOOP, 3 BEAT JUMP */
     .n_mode = { N_MODE_HOT, N_MODE_BEATLOOP, N_MODE_PADFX1, N_MODE_BEATJUMP },
     .mode_banks = 1,
     /* 4 GATE CUE, 5 BEAT LOOP 2, 6 RELEASE FX, 7 BEAT JUMP 2 */
     .n_mode_bank = { N_MODE_KEYBOARD, N_MODE_SAMPLER, N_MODE_PADFX2,
                      N_MODE_KEYSHIFT },
     .pad_ch = CH_PADS1,
     .pad_ch2 = CH_PADS2,
     .pad_shift_mirror = 1,
     .n_pad_base = { 0, 96, 16, 32 },   /* unused while pad_base is set */
     .pad_enc = LED_ENC_FLX10_PALETTE,
     .pad_base = flx10_led_pad_base,
     .strip_ch_first = 0,
     .strip_count = 2,
     .n_strip_cue = N_CH_CUE,
     .master_ch_first = -1,        /* master CUE: no LED answered */
     .master_ch_count = 0,
     .n_master_cue = -1,
     /* NO Beat FX ON/OFF LED, though rekordbox lights it on note 70: the FLX10
      * hands every 94 46 sent to it straight back as a press -- the bridge's
      * resend every 2 s and the active-effect blink arrived as presses and
      * toggled the effect on and off (MIDI dump on the unit, 2026-10-26). */
     .fx_ch = -1,
     .n_fx = { -1, -1, -1, -1, -1 },
     .meter_ch_first = 0,
     .n_meter_cc = 2,
     .meter_enc = METER_ENC_FLX4_LEVEL,
     .meter_pre_fader = 1,
     .meter_master_ch = -1,        /* CC 2/3 on ch 7: nothing moved */
     .n_meter_master_l = -1,
     .n_meter_master_r = -1,
     .n_quantize = N_QUANTIZE,
     .loop_style = LED_LOOP_REKORDBOX,
     .pad_style = LED_PAD_DIM_BRIGHT,
     .pad_touch = flx10_pad_touch,
     .deck_display = flx10_deck_display,
};

const struct ctrl_map map_flx10 = {
     "flx10",
     flx10_build,
     flx10_startup,
     flx10_event,
     flx10_tick,
     NULL,
     NULL,
     &flx10_leds,
};
