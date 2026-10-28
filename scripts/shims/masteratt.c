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
#include <fcntl.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

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

/* ---- HEADROOM (operator, 2026-10-28: rekordbox's setting of that name) -------------
 * The decks' level before the mixer, apart from the master's attenuator: at -12 dB a
 * channel's meter reads 12 dB lower, and the master with it. rbp's
 * mixerengine::MixerChannel::update(frames) @0x9e890 takes a block from its deck
 * (getPlayerDataPointer), puts it through its three gain stages (+0x40, +0xe8, +0x120,
 * the first two ramping) and writes it to its own buffer at +0x1c, which EQ, fader and
 * meters take from: the block is scaled there, after update, so the ramps are left be
 * and the deck's own buffer (the external mixer's taps) is not touched. Kept in
 * /root/settings/headroom (dB, 0 / -3 / -6 / -9 / -12), set from UTILITY (fbshim's
 * utility_rows.c through headroom_set). */
#define CHANNEL_UPDATE 0x0009e890ul
/* ldr r3,[r0,#0xd0] ; push {r4,r5,r6,lr} */
static const uint8_t update_guard[8] = { 0xd0, 0x30, 0x90, 0xe5, 0x70, 0x40, 0x2d, 0xe9 };
#define CHANNEL_OUT 0x1c
#define HEADROOM_FILE "/root/settings/headroom"
typedef void (*update_fn)(void *self, int frames);
static update_fn original_update;
static volatile float headroom_gain = 1.0f;
static volatile int headroom_db;

/* dB as the gain applied. Pure, for the test. */
float headroom_gain_of(int db)
{
     return db >= 0 ? 1.0f : db < -24 ? 0.063095734f : powf(10.0f, (float)db / 20.0f);
}

/* Set it now and keep it. */
__attribute__((visibility("default"))) void headroom_set(int db)
{
     char buf[16];
     int fd, n;

     headroom_db = db;
     headroom_gain = headroom_gain_of(db);
     fd = open(HEADROOM_FILE, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
     if (fd >= 0) {
          n = snprintf(buf, sizeof buf, "%d\n", db);
          if (write(fd, buf, (size_t)n) != n)
               klog("knobshim2: HEADROOM: %s not written\n", HEADROOM_FILE);
          close(fd);
     }
     klog("knobshim2: HEADROOM %d dB (x%.3f on each deck)\n", db, (double)headroom_gain);
}

__attribute__((visibility("default"))) int headroom_get(void)
{
     return headroom_db;
}

static void hooked_update(void *self, int frames)
{
     float g = headroom_gain, *out;

     original_update(self, frames);
     if (g == 1.0f || frames <= 0 || frames > 4096)
          return;
     out = *(float **)((char *)self + CHANNEL_OUT);
     if (!out)
          return;
     for (int i = 0; i < frames * 2; i++)
          out[i] *= g;
}

static void headroom_load(void)
{
     char buf[16];
     int fd = open(HEADROOM_FILE, O_RDONLY | O_CLOEXEC), n;

     if (fd < 0)
          return;
     n = (int)read(fd, buf, sizeof buf - 1);
     close(fd);
     if (n <= 0)
          return;
     buf[n] = '\0';
     headroom_db = atoi(buf);
     headroom_gain = headroom_gain_of(headroom_db);
}

__attribute__((constructor)) static void masteratt_init(void)
{
     if (!is_rbp_process())
          return;
     original_setatt = (setatt_fn)rbp_hook(MASTEROUT_SETATT, setatt_guard, (void *)hooked_setatt);
     if (!original_setatt)
          klog("knobshim2: MASTER ATT.: MasterOut::setATT is not the one measured -- "
               "the setting stays a no-op\n");
     headroom_load();
     original_update = (update_fn)rbp_hook(CHANNEL_UPDATE, update_guard, (void *)hooked_update);
     if (!original_update)
          klog("knobshim2: HEADROOM: MixerChannel::update is not the one measured -- off\n");
     else
          klog("knobshim2: HEADROOM %d dB\n", headroom_db);
     original_booth = (setatt_fn)rbp_hook(BOOTH_SETATT, booth_guard, (void *)hooked_booth);
     if (!original_booth)
          klog("knobshim2: BOOTH MONITOR ATT.: BoothMonitor::setATT is not the one "
               "measured\n");
}
