/* SPDX-License-Identifier: MPL-2.0
 *
 * keyshift.c -- each deck's key shifted by whole semitones (keyshift.h).
 *
 * Adapted from rx3-toolkit (github.com/Tratosca/rx3-toolkit at cc17a1f,
 * MPL-2.0): mod/modules/keyshift/rx3_keyshift.h and _decl.h (the engines and the
 * audio hook); the detour is rbp_hook.c, from the toolkit's rx3_hooks.c. This file
 * stays under the MPL; the rest of this tree does not have to. Every address and
 * guard below was checked against the XDJ-RX3 1.20 rbp this port runs (sha1
 * 3513d34b..., which the toolkit lists as supported).
 *
 * BUILT FOR ARMv7 WITH VFP/NEON, softfp (Makefile): the shims are soft-float
 * armv5t because rbp is, and softfp keeps that calling convention while doing
 * the floating point in hardware -- the shifter is per-sample float work on
 * rbp's audio thread and would not keep up emulated.
 */
#include "keyshift.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#include "doublelock.h"     /* the same track on both decks: one stretched copy */
#include "envutil.h"
#include "rbp_hook.h"       /* rbp_hook(): the detour */
#include "rbp_key.h"        /* is_rbp_process() */
#include "shimutil.h"       /* klog() */

typedef struct { float left, right; } Float2;

#include "rx3_pitch_shift.h"

/* ---- rbp 1.20 --------------------------------------------------------------- */
#define TIMESTRETCH_MANAGER  0x000a0e54ul   /* dsp::TimeStretchManager::operate */
static const uint8_t timestretch_guard[8] = { 0xf0, 0x41, 0x2d, 0xe9, 0x00, 0x40, 0xa0, 0xe1 };
#define PITCH_CTOR           0x0008b700ul   /* mixerengine::BeatEffectPitch::BeatEffectPitch() */
#define PITCH_INITIALIZE     0x0008b2b8ul   /* ::initialize() */
#define PITCH_EXECUTE        0x0008c648ul   /* ::execute(const Float2*, Float2*, int) */
#define PITCH_ADJUST         0x000b5b38ul   /* mixerengine::BeatEffect::adjustParameter */
#define PITCH_PARAM_LEVEL_DEPTH 2
#define PITCH_PARAM_PERCENT     4
#define PITCH_UNITY_LEVEL_DEPTH 0.5f       /* the percent curve's unity point */
#define PITCH_MAX_FRAMES     8192u
#define SAMPLE_RATE          44100u         /* rbp's, which audioshim keeps */

typedef long (*operate_fn)(void *, uint32_t, Float2 *, unsigned int, unsigned int);
typedef void *(*pitch_ctor_fn)(void *);
typedef void (*pitch_initialize_fn)(void *);
typedef void (*pitch_adjust_fn)(void *, int, float);
typedef void (*pitch_execute_fn)(void *, const Float2 *, Float2 *, int);

/* At level/depth 0.5 the effect's shift is exactly percent/100, so -50..+100 is
 * -12..+12 semitones; these are the nearest integer percentages (the toolkit's
 * measurement; the worst is -9, 13 cents flat). */
static const signed char semitone_percent[25] = {
    -50, -47, -44, -41, -37, -33, -29, -25, -21, -16, -11, -6, 0,
    6, 12, 19, 26, 33, 41, 50, 59, 68, 78, 89, 100
};

struct deck {
    volatile unsigned int request;       /* low byte semitones+12, above it a counter */
    unsigned int applied;
    volatile unsigned int reset_gen;
    unsigned int seen_reset;
    unsigned int tail_blocks;
    void *volatile pitch;                /* rbp's Pitch effect, published when built */
    Float2 *pitch_out;
    struct rx3_shifter shifter;
    float *volatile history;             /* the shifter's ring, published when built */
    uint32_t last_pos;
    unsigned int last_frames;
    int last_dir, pos_valid, was_silent;
};

static struct deck decks[2] = { { .request = 12u }, { .request = 12u } };
static operate_fn original_operate;
static volatile int engines_built;

/* ---- the engines ------------------------------------------------------------ */

static int semis_of(unsigned int request)
{
    return (int)(request & 0xffu) - 12;
}

static float percent_request(int semitones)
{
    int p = semitone_percent[semitones + 12];
    return (float)p + (p >= 0 ? 0.5f : -0.5f);      /* adjustParameter truncates */
}

static void publish_percent(void *pitch, int semitones)
{
    ((pitch_adjust_fn)PITCH_ADJUST)(pitch, PITCH_PARAM_PERCENT, percent_request(semitones));
}

/* Both engines for one deck, built on the caller's thread and then published:
 * the audio thread touches neither until its pointer is set. */
static void build_deck(struct deck *d)
{
    void *obj = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    Float2 *out = mmap(NULL, PITCH_MAX_FRAMES * sizeof(Float2), PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    float *hist = mmap(NULL, RX3_SHIFT_HISTORY * 2u * sizeof(float), PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

    if (hist != MAP_FAILED) {
        rx3_shifter_init(&d->shifter, hist);
        rx3_shifter_set_semitones(&d->shifter, semis_of(d->request));
        __atomic_store_n(&d->history, hist, __ATOMIC_RELEASE);
    }
    if (obj == MAP_FAILED || out == MAP_FAILED)
        return;
    ((pitch_ctor_fn)PITCH_CTOR)(obj);
    *(uint32_t *)((uint8_t *)obj + 4) = SAMPLE_RATE;
    *(uint32_t *)((uint8_t *)obj + 8) = SAMPLE_RATE;
    *(float *)((uint8_t *)obj + 0xc) = 1.0f / (float)SAMPLE_RATE;
    *(uint32_t *)((uint8_t *)obj + 0x10) = PITCH_MAX_FRAMES;   /* sizes its buffers */
    ((pitch_initialize_fn)PITCH_INITIALIZE)(obj);
    ((pitch_adjust_fn)PITCH_ADJUST)(obj, PITCH_PARAM_LEVEL_DEPTH, PITCH_UNITY_LEVEL_DEPTH);
    publish_percent(obj, semis_of(d->request));
    d->pitch_out = out;
    __atomic_store_n(&d->pitch, obj, __ATOMIC_RELEASE);
}

static int block_is_silent(const Float2 *b, unsigned int frames)
{
    for (unsigned int i = 0; i < frames; i++)
        if (b[i].left > 1e-6f || b[i].left < -1e-6f || b[i].right > 1e-6f || b[i].right < -1e-6f)
            return 0;
    return 1;
}

/* A jump, a change of direction, sound after silence or a new track: the
 * shifter's history no longer leads into this block. */
static int transport_reset(struct deck *d, uint32_t pos, unsigned int frames, int silent)
{
    unsigned int gen = __atomic_load_n(&d->reset_gen, __ATOMIC_SEQ_CST);
    int reset = gen != d->seen_reset || (!silent && d->was_silent);

    d->seen_reset = gen;
    d->was_silent = silent;
    if (d->pos_valid) {
        int delta = (int)(pos - d->last_pos);
        int span = (int)(frames > d->last_frames ? frames : d->last_frames) * 4;
        int dir = delta > 0 ? 1 : delta < 0 ? -1 : 0;
        if (delta < -span || delta > span)
            reset = 1;
        if (dir) {
            if (d->last_dir && dir != d->last_dir)
                reset = 1;
            d->last_dir = dir;
        }
    }
    d->pos_valid = 1;
    d->last_pos = pos;
    d->last_frames = frames;
    return reset;
}

static void apply(struct deck *d, Float2 *out, unsigned int frames)
{
    unsigned int req = __atomic_load_n(&d->request, __ATOMIC_SEQ_CST);
    int semis = semis_of(req);
    void *pitch = __atomic_load_n(&d->pitch, __ATOMIC_ACQUIRE);
    float *hist = __atomic_load_n(&d->history, __ATOMIC_ACQUIRE);

    if (req != d->applied) {
        if (pitch)
            publish_percent(pitch, semis);
        if (hist)
            rx3_shifter_set_semitones(&d->shifter, semis);
        d->applied = req;
        d->tail_blocks = semis ? 64u : 0u;           /* let the effect's tail finish */
    }
    if (semis < 0 || (semis == 0 && d->tail_blocks)) {
        /* down: rbp's own effect; the shifter keeps absorbing so a change of
         * direction starts from real audio */
        if (hist)
            rx3_shifter_absorb(&d->shifter, (const float *)(void *)out, frames);
        if (!pitch || !d->pitch_out)
            return;
        ((pitch_execute_fn)PITCH_EXECUTE)(pitch, out, d->pitch_out, (int)frames);
        memcpy(out, d->pitch_out, frames * sizeof(Float2));
        if (semis == 0 && d->tail_blocks)
            d->tail_blocks--;
    } else if (hist) {
        if (rx3_shifter_is_active(&d->shifter))
            rx3_shifter_process(&d->shifter, (float *)(void *)out, frames);
        else
            rx3_shifter_absorb(&d->shifter, (const float *)(void *)out, frames);
    }
}

/* a deck's block shifted to its key */
static void shift_block(void *mgr, uint32_t pos, Float2 *out, unsigned int frames)
{
    unsigned int deck;
    struct deck *d;

    if (!out || !frames || frames > PITCH_MAX_FRAMES || !engines_built)
        return;
    deck = *(const uint32_t *)mgr;                   /* the manager's first word */
    if (deck >= 2u)
        return;
    d = &decks[deck];
    if (transport_reset(d, pos, frames, block_is_silent(out, frames))) {
        int semis = semis_of(d->request);
        float *hist = __atomic_load_n(&d->history, __ATOMIC_ACQUIRE);
        void *pitch = __atomic_load_n(&d->pitch, __ATOMIC_ACQUIRE);
        if (hist) {
            rx3_shifter_seed(&d->shifter, hist, (const float *)(const void *)out, frames);
            rx3_shifter_set_semitones(&d->shifter, semis);
            d->shifter.ratio = d->shifter.target;
        }
        if (pitch)
            publish_percent(pitch, semis);
    }
    apply(d, out, frames);
}

static volatile int deck_muted[2];           /* keyshift_mute() */

static long hooked_operate(void *mgr, uint32_t pos, Float2 *out, unsigned int frames,
                           unsigned int mode)
{
    long r = original_operate(mgr, pos, out, frames, mode);
    unsigned int deck = *(const uint32_t *)mgr;

    /* first, each deck's stretched block as rbp made it: the same track at the
     * same spot on both decks plays one copy (doublelock.h); a key shift then
     * applies on top, per deck. The raw syscall: glibc 2.13 keeps clock_gettime
     * in librt. */
    if (out && frames && deck < 2u) {
        struct timespec ts;
        if (syscall(SYS_clock_gettime, CLOCK_MONOTONIC, &ts) == 0)
            doublelock_block(deck, pos, (float *)(void *)out, frames,
                             (unsigned long long)ts.tv_sec * 1000000ull +
                             (unsigned long long)ts.tv_nsec / 1000ull);
    }
    shift_block(mgr, pos, out, frames);
    if (out && frames && deck < 2u && deck_muted[deck])
        memset(out, 0, (size_t)frames * sizeof(Float2));
    return r;
}

/* ---- the API ---------------------------------------------------------------- */

int keyshift_install(void)
{
    if (original_operate)
        return 0;
    if (!is_rbp_process())
        return -1;
    original_operate = (operate_fn)rbp_hook(TIMESTRETCH_MANAGER, timestretch_guard,
                                        (void *)hooked_operate);
    if (!original_operate) {
        klog("knobshim2: key shift: rbp's TimeStretchManager::operate is not the one "
             "measured -- no key shift\n");
        return -1;
    }
    klog("knobshim2: key shift: hooked TimeStretchManager::operate\n");
    return 0;
}

int keyshift_set(int deck, int semitones)
{
    struct deck *d;
    unsigned int req, next;

    if (deck < 0 || deck > 1 || !original_operate)
        return -1;
    if (!engines_built) {
        build_deck(&decks[0]);
        build_deck(&decks[1]);
        __atomic_store_n(&engines_built, 1, __ATOMIC_RELEASE);
        klog("knobshim2: key shift: engines built (%u Hz)\n", SAMPLE_RATE);
    }
    if (semitones < -12) semitones = -12;
    if (semitones > 12) semitones = 12;
    d = &decks[deck];
    req = __atomic_load_n(&d->request, __ATOMIC_SEQ_CST);
    for (;;) {
        if (semis_of(req) == semitones)
            return 0;
        next = ((req & ~0xffu) + 0x100u) | (unsigned int)(semitones + 12);
        if (__atomic_compare_exchange_n(&d->request, &req, next, 0, __ATOMIC_SEQ_CST,
                                        __ATOMIC_SEQ_CST))
            return 0;
    }
}

void keyshift_mute(int deck, int on)
{
    if (deck >= 0 && deck < 2)
        deck_muted[deck] = on;
}

int keyshift_get(int deck)
{
    return deck < 0 || deck > 1 ? 0 : semis_of(__atomic_load_n(&decks[deck].request,
                                                               __ATOMIC_SEQ_CST));
}

void keyshift_reload(int deck)
{
    if (deck < 0 || deck > 1 || !engines_built)
        return;
    keyshift_set(deck, 0);
    __sync_add_and_fetch(&decks[deck].reset_gen, 1u);
}

__attribute__((constructor)) static void keyshift_init(void)
{
    doublelock_enable(atoi(env_str("DOUBLES_LOCK", "1")) != 0);
    keyshift_install();
}
