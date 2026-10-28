/*
 * touchscroll.c -- BROWSE's list follows a dragging finger row for row (docs/21
 * item 26: "touch scrolling is jerky"): the finger's samples are not held back
 * (adapt_hooked, below), and no row of a drag is lost.
 *
 * How rbp 1.20 scrolls a list under a finger (read 2026-10-28): each touch report is
 * a key input that BrowseUiIf::TouchListScrollProc @0xd0120 turns into a row, the
 * finger's y over the row height; when the row changes it calls
 * UiKey_BrowseTouchListScroll @0x121490 with the difference, which STORES it as the
 * scroll still to make (0x032756ec). Ui_EventTask's TouchScroll @0x1217e8 makes that
 * scroll (CursorMoveFlickBrowse) and clears it on its own round. So two row changes
 * between two of its rounds lose the first: the list stops behind the finger, then
 * jumps on -- the jerk.
 *
 * The function is replaced whole (it is short), with rbp's own checks in rbp's order,
 * and the difference ADDED to what is still to be made.
 */
#include <stdint.h>
#include <string.h>

#include "rbp_hook.h"
#include "rbp_key.h"        /* is_rbp_process() */
#include "shimutil.h"       /* klog() */

#define PENDING 0x032756ecu

static int (*pc_certified)(void) = (void *)0x000ff2d8u;        /* UiGetPcCertified */
static int (*pc_control_shown)(void) = (void *)0x00113020u;    /* IsPcControlDispOn */
static int (*list_jump_mode)(void) = (void *)0x00112aa4u;      /* getListJumpMode */
static int (*cursor_area)(void) = (void *)0x00114e30u;         /* getCursorArea */
static void (*change_area)(int) = (void *)0x00101a90u;         /* changeActiveArea */
static int (*menu_on)(void) = (void *)0x00112e10u;             /* IsMenuOn */

/* Pure, for the test: the scroll still to make after a finger's `delta` rows. */
int touchscroll_add(int pending, int delta)
{
    return pending + delta;
}

static int list_scroll(int delta)
{
    if (pc_certified() && pc_control_shown())
        return 0;
    if (list_jump_mode())
        return 0;
    if (!cursor_area())
        change_area(1);
    if (menu_on() || !delta || menu_on())
        return 0;
    int *pending = (int *)(unsigned long)PENDING, was = __atomic_load_n(pending, __ATOMIC_SEQ_CST);
    /* TouchScroll reads it, then writes 0: added only if it has not been taken between */
    while (!__atomic_compare_exchange_n(pending, &was, touchscroll_add(was, delta), 0,
                                        __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST))
        ;
    return 1;
}

/* THE FINGER, NOT HELD BACK. Every touch sample passes rbp's
 * TouchAdValueHysteresis::procAdaptValue @0x363a74 first, one state per axis: a move
 * inside a small band is held, one inside a larger band creeps the held value one unit
 * every six samples, and one past it is taken only after four samples in a row beyond
 * it -- a resistive panel's filter. With a capacitive panel's samples (tscfake.c) it
 * holds a dragging finger still, then jumps it ~100 px every ~120 ms (measured on the
 * unit 2026-10-28: TouchListScrollProc's y changed every 120 ms though samples came
 * every 8), so the list jumped two or three rows at a time. Once a touch is under
 * way (state 2) the sample now goes through as it is; the first sample after a gap is
 * still rbp's to drop (state 0 -> 1), which tscfake.c's press burst relies on. */
static void (*original_adapt)(uint8_t *, uint8_t *);
static const unsigned adapt_counters[] = { 0x10, 0x14, 0x18, 0x1c, 0x2c, 0x30, 0x34, 0x38 };

static void adapt_hooked(uint8_t *filter, uint8_t *status)
{
    if (!status[0] || filter[0] != 2) {
        original_adapt(filter, status);
        return;
    }
    memcpy(filter + 0xc, status + 4, 4);        /* x as held: the finger's */
    memcpy(filter + 0x28, status + 8, 4);       /* y */
    for (unsigned i = 0; i < sizeof(adapt_counters) / sizeof(*adapt_counters); i++)
        memset(filter + adapt_counters[i], 0, 4);
}

__attribute__((constructor)) static void touchscroll_init(void)
{
    if (!is_rbp_process())
        return;
    /* push {r3,r4,r5,lr} ; mov r4,r0 */
    static const uint8_t guard[8] = { 0x38, 0x40, 0x2d, 0xe9, 0x00, 0x40, 0xa0, 0xe1 };
    /* each function called, by its first two words (read from 1.20, 2026-10-28) */
    static const struct { unsigned address, a, b; } called[] = {
        { 0x0ff2d8, 0xe92d4008, 0xe59f301c }, { 0x113020, 0xe59f3024, 0xe5932000 },
        { 0x112aa4, 0xe30f38b8, 0xe3403326 }, { 0x114e30, 0xe59f3040, 0xe5932004 },
        { 0x101a90, 0xe309306c, 0xe3403326 }, { 0x112e10, 0xe30f38b8, 0xe3403326 },
    };
    int same = !memcmp((const void *)0x121490ul, guard, 8) &&
               *(const uint32_t *)0x121518ul == PENDING;
    for (unsigned i = 0; same && i < sizeof(called) / sizeof(*called); i++)
        same = !memcmp((const void *)(unsigned long)called[i].address, &called[i].a, 8);
    if (!same) {
        klog("knobshim2: touch scroll: rbp's list scroll is not the one measured -- left\n");
        return;
    }
    /* ldrb r3,[r1] ; str r4,[sp,#-4]! */
    static const uint8_t adapt_guard[8] = { 0x00, 0x30, 0xd1, 0xe5, 0x04, 0x40, 0x2d, 0xe5 };
    original_adapt = rbp_hook(0x363a74, adapt_guard, (void *)adapt_hooked);
    if (!original_adapt)
        klog("knobshim2: touch scroll: rbp's touch filter is not the one measured -- left\n");
    /* replaced whole: its trampoline is never called */
    if (!rbp_hook(0x121490, guard, (void *)list_scroll))
        klog("knobshim2: touch scroll: hook failed\n");
}
