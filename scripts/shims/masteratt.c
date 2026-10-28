/*
 * masteratt.c -- UTILITY's MASTER ATT. (-12 / -6 / 0 dB), applied to the samples.
 *
 * On the RX3 the setting never touches the audio: mixerengine::MasterOut::setATT
 * @0x5b270 sets two GPIO lines (GpioManager::write on the objects at +0xb0 and
 * +0x48) that switch an attenuator in the unit's analogue output -- 0 = (0,1)...
 * read off the code 2026-10-27: setting 0 writes (0,0), 1 writes (0,1), 2 writes
 * (1,0). There is no such stage here, so the setting did nothing (operator,
 * 2026-10-27). The value reaching setATT is 0 = 0 dB, 1 = -6 dB, 2 = -12 dB --
 * the operator's ears on the unit; rbp's UTILITY row (stUtilityObjects row 18,
 * its option list @0x513d50: "-12dB", "-6dB", "0dB") lists the labels the other
 * way round, and 0 leaving both lines low fits "no attenuation".
 *
 * This detours setATT -- rbp's original still runs -- and publishes the gain as
 * g_master_att (shmstate.h), which audioshim.c applies to the master output after
 * the meters, as the RX3's analogue attenuator sits after its converter. rbp
 * calls setATT with the saved setting at start, so the level is right from the
 * first block.
 *
 * WITH THE DDJ-FLX10 the attenuation is the unit's own (operator, 2026-10-28):
 * rekordbox sets it over MIDI (ch 16 BF 42 for the master, BF 43 for the booth,
 * 00 / 10 / 20 = 0 / -6 / -12 dB -- capture_settings), and the map sends the
 * setting that way (map_flx10.c, flx10_unit_tick) while the samples are left
 * alone (masteratt_hardware). BoothMonitor::setATT @0x5a44c is detoured too, for
 * the booth's: the same enum (its gain table runs down 6 dB a step from 0).
 */
#include <stdint.h>

#include "rbp_hook.h"
#include "rbp_key.h"        /* is_rbp_process() */
#include "shimutil.h"       /* klog() */
#include "shmstate.h"       /* g_master_att */

#define MASTEROUT_SETATT 0x0005b270ul    /* mixerengine::MasterOut::setATT(EnMasterATT) */
/* push {r3,r4,r5,lr} ; subs r4,r1,#0 -- nothing pc-relative */
static const uint8_t setatt_guard[8] = { 0x38, 0x40, 0x2d, 0xe9, 0x00, 0x40, 0x51, 0xe2 };

#define BOOTH_SETATT 0x0005a44cul       /* mixerengine::BoothMonitor::setATT(EnBoothMonitorATT) */
/* ldr r3,[r0,#0x44] ; cmp r3,r1 */
static const uint8_t booth_guard[8] = { 0x44, 0x30, 0x90, 0xe5, 0x01, 0x00, 0x53, 0xe1 };

typedef void (*setatt_fn)(void *self, int att);
static setatt_fn original_setatt, original_booth;

/* The settings rbp last applied, 0 0 dB / 1 -6 dB / 2 -12 dB; -1 not yet. */
volatile int masteratt_master = -1, masteratt_booth = -1;
static volatile int hardware;

/* rbp's EnMasterATT as a linear gain. Pure, for the test. */
float masteratt_gain(int att)
{
     switch (att) {
     case 1:  return 0.50118723f;       /* -6 dB */
     case 2:  return 0.25118864f;       /* -12 dB */
     default: return 1.0f;              /* 0 dB, and anything rbp did not label */
     }
}

static void apply(void)
{
     g_master_att = hardware ? 1.0f : masteratt_gain(masteratt_master);
}

/* 1: the controller attenuates its outputs itself, the samples are not scaled. */
void masteratt_hardware(int on)
{
     hardware = on;
     apply();
}

static void hooked_setatt(void *self, int att)
{
     masteratt_master = att;
     apply();
     klog("knobshim2: MASTER ATT. %d -> %s\n", att,
          hardware ? "the controller's own attenuator" : "scaled on the master output");
     original_setatt(self, att);
}

static void hooked_booth(void *self, int att)
{
     masteratt_booth = att;
     klog("knobshim2: BOOTH MONITOR ATT. %d\n", att);
     original_booth(self, att);
}

__attribute__((constructor)) static void masteratt_init(void)
{
     if (!is_rbp_process())
          return;
     original_setatt = (setatt_fn)rbp_hook(MASTEROUT_SETATT, setatt_guard, (void *)hooked_setatt);
     if (!original_setatt)
          klog("knobshim2: MASTER ATT.: MasterOut::setATT is not the one measured -- "
               "the setting stays a no-op\n");
     original_booth = (setatt_fn)rbp_hook(BOOTH_SETATT, booth_guard, (void *)hooked_booth);
     if (!original_booth)
          klog("knobshim2: BOOTH MONITOR ATT.: BoothMonitor::setATT is not the one "
               "measured\n");
}
