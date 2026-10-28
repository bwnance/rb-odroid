/* SPDX-License-Identifier: MPL-2.0
 *
 * browsesort.c -- tap a BROWSE heading to sort the track list by it, as on a
 * CDJ-3000; tap the heading it is sorted by to turn the sort round. The triangles that
 * say which are browserows.c's (heading_mark); which heading a tap is on is
 * browserows_heading_sort().
 *
 * The operator's ask (2026-10-28, docs/21 item 31), with a CDJ-3000 photo: a triangle
 * by every heading, tapped to sort.
 *
 * HOW A TAP BECOMES A SORT. The sort has to be asked for on rbp's database client task,
 * the way LIST MENU > SORT asks for it: SendSortedBrwsList @0x256030 inside
 * SelectRightMenu @0x2596cc. So a tap posts a UI event of its own into rbp's
 * BrowseComm mailbox (SendMessage_ToBrowseComm's pool 0x03268be0 and mailbox
 * 0x03267230, read from 1.20): UiBrowseComm_IsUIMsgValid_ListReq @0x104edc sees it on
 * that task and asks Ui_BrowseComm_ReqMenuSelect @0x110c50 for item 0x7e00|sort,
 * which comes back to SelectRightMenu (browsemenu.c hands those items here), where the
 * sort is sent.
 *
 * DIRECTION. rbp sorts each field one way only. A sort sent with 0x4000 set carries
 * its own direction (0x8000 = down): DBSMain_OnListClientCmd @0x153530 narrows the
 * sort to a byte, so the request's full word is kept per task around it, and
 * ListStructSortKeySet @0x171780 sets the comparator itself (LBGrp_SetSortType
 * @0x17bd28, bit 7 = down) for such a sort.
 *
 * Adapted from rx3-toolkit (github.com/Tratosca/rx3-toolkit at cc17a1f, MPL-2.0):
 * mod/modules/core/services/rx3_browse.c (enqueue_sort, sort_validate, sort_select,
 * sort_dispatch, sort_keys, rx3_browse_touch). Every address checked against our rbp
 * 1.20, 2026-10-28. This file stays under the MPL.
 */
#include "browserows.h"

#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "rbp_hook.h"
#include "shimutil.h"       /* klog() */

static unsigned word(const void *p) { unsigned v; memcpy(&v, p, 4); return v; }
static unsigned half(const void *p) { uint16_t v; memcpy(&v, p, 2); return v; }

/* ---- rbp 1.20 --------------------------------------------------------------------- */
static unsigned (*task_id)(void) = (void *)0x0018a904u;                     /* vget_tid */
static int (*set_sort_type)(void *, unsigned, unsigned, unsigned, unsigned) =
    (void *)0x0017bd28u;                                                    /* LBGrp_SetSortType */
static int (*get_message)(unsigned, void **, unsigned) = (void *)0x00186fc4u; /* CmnFunc_get_mpf */
static int (*free_message)(unsigned, void *) = (void *)0x00187068u;          /* CmnFunc_rel_mpf */
static int (*send_message)(unsigned, void *) = (void *)0x00186e18u;          /* CmnFunc_snd_mbx */
static unsigned (*os_time)(void) = (void *)0x00187290u;                     /* CmnFunc_OsTim_GetTime */
static unsigned (*active_deck)(void) = (void *)0x001341d8u;                 /* Ui_getActiveDeck */
static int (*menu_select)(void *, unsigned, unsigned) = (void *)0x00110c50u; /* Ui_BrowseComm_ReqMenuSelect */
static int (*send_sorted)(void *, void *, void *, int, const uint16_t *) =
    (void *)0x00256030u;                                                    /* SendSortedBrwsList */
static int (*original_validate)(void *, void *);
static int (*original_dispatch)(void *, void *);
static void (*original_keys)(unsigned, unsigned, int);

#define POOL     0x03268be0u
#define MAILBOX  0x03267230u
#define SENDER   0x03268b20u
#define KIND     0x0042dcb8u
#define EVENT    0x10feu        /* our UI event's code (word 9) and mark (word 10) */
#define MARK     0x52583353u
#define ITEM     0x7e00u

/* The sort asked for, +1 (0 = none): set by a tap, posted at once, taken on rbp's
 * database client task; `posted` while our event is in the mailbox. */
static unsigned wanted, posted;
static unsigned task_sort[49];

/* Post a tap's sort into the mailbox, as SendMessage_ToBrowseComm posts a UI event.
 * From any thread: rbp's pool and mailbox (get_mpf, snd_mbx) are pthread mutexes and
 * conditions, nothing tied to a task -- so a tap posts on its release, without
 * waiting for anything to be redrawn. */
static void post(void)
{
    unsigned sort = __atomic_load_n(&wanted, __ATOMIC_SEQ_CST);
    if (!sort || __sync_lock_test_and_set(&posted, 1u))
        return;
    unsigned pool = word((void *)POOL);
    uint32_t *msg = NULL;
    if (get_message(pool, (void **)&msg, 0x1808u) || !msg) {
        __atomic_store_n(&wanted, 0u, __ATOMIC_SEQ_CST);
        __sync_lock_release(&posted);
        return;
    }
    memset(msg, 0, 0x70);
    msg[1] = word((void *)KIND);
    msg[2] = 0x32c9;
    msg[4] = word((void *)SENDER);
    msg[6] = pool;
    msg[8] = 0x70;
    msg[9] = EVENT;
    msg[10] = MARK;
    msg[0x1a] = active_deck() == 2;
    msg[0x1b] = os_time();
    if (send_message(word((void *)MAILBOX), msg)) {
        free_message(pool, msg);
        __atomic_store_n(&wanted, 0u, __ATOMIC_SEQ_CST);
        __sync_lock_release(&posted);
    }
}

/* A tap from a file, for checking it with nobody at the glass: "x y" in TAP_FILE is
 * pressed and released there once, then the file goes. */
#define TAP_FILE "/run/rbp/browse.tap"
static void tap_file(void)
{
    char text[32] = { 0 };
    int fd = open(TAP_FILE, O_RDONLY), x, y;
    if (fd < 0)
        return;
    ssize_t n = read(fd, text, sizeof text - 1);
    close(fd);
    unlink(TAP_FILE);
    text[n > 0 ? n : 0] = 0;
    if (sscanf(text, "%d %d", &x, &y) == 2) {
        int taken = browse_heading_touch(1, x, y);
        browse_heading_touch(0, x, y);
        klog("knobshim2: browse sort: tap at %d,%d from %s: %s\n", x, y, TAP_FILE,
             taken ? "a heading" : "not a heading");
    }
}

/* On the database client task: our event asks for the menu item that sorts. */
static int validate_hooked(void *context, void *message)
{
    int result = original_validate(context, message);
    if (word((uint8_t *)message + 0x24) != EVENT || word((uint8_t *)message + 0x28) != MARK)
        return result;
    unsigned sort = __atomic_load_n(&wanted, __ATOMIC_SEQ_CST);
    if (!sort || !result ||
        menu_select(context, ITEM | ((sort - 1u) & 0xffu), word((uint8_t *)message + 0x68))) {
        __atomic_store_n(&wanted, 0u, __ATOMIC_SEQ_CST);
        __sync_lock_release(&posted);
    }
    return 0;       /* consumed: rbp's own MENU_ENTER never sees it */
}

/* From browsemenu.c's SelectRightMenu hook: an item 0x7e00|n is ours. 1 = taken. */
int browsesort_select(uint8_t *context, void *mbx, uint8_t *request, int *result)
{
    unsigned item = half(request + 0x78);
    if ((item & 0xff00u) != ITEM)
        return 0;
    unsigned sort = __atomic_exchange_n(&wanted, 0u, __ATOMIC_SEQ_CST);
    __sync_lock_release(&posted);
    *result = 0;
    if (!sort || ((sort - 1u) & 0xffu) != (item & 0xffu) || word(context + 4) > 4)
        return 1;
    sort -= 1u;
    /* the low byte: the sort id, bit 7 down; 0x4000 marks one with its own direction */
    unsigned code = (sort & 0x4000u) ? (0x4000u | (sort & 127u) | (sort & 0x8000u)) : (sort & 127u);
    static const uint16_t empty[] = { 0 };
    context[0x15] = 1;
    *result = send_sorted(context, mbx, request, (int)code, empty);
    klog("knobshim2: browse sort: heading tapped, sort %u%s\n", sort & 127u,
         (sort & 0x4000u) ? ((sort & 0x8000u) ? " down" : " up") : "");
    return 1;
}

/* The server narrows the sort to a byte: keep the request's whole word for this task
 * while it runs. */
static int dispatch_hooked(void *context, void *message)
{
    unsigned task = task_id(), saved = 0;
    if (task && task < 49) {
        const uint8_t *request = (const uint8_t *)(unsigned long)word((uint8_t *)message + 0x1c);
        saved = task_sort[task];
        task_sort[task] = request ? word(request + 0x18) : 0;
    }
    int result = original_dispatch(context, message);
    if (task && task < 49)
        task_sort[task] = saved;
    return result;
}

static void keys_hooked(unsigned node, unsigned code, int grouped)
{
    unsigned task = task_id();
    if (task && task < 49 && (task_sort[task] & 0x4000u) && (task_sort[task] & 127u) == code)
        code = task_sort[task];
    if (!(code & 0x4000u)) {
        original_keys(node, code, grouped);
        return;
    }
    unsigned base = code & 127u, kind;
    switch (base) {
    case 2: case 3: case 6: case 7: case 9: case 10: case 11: case 12: kind = 2; break;
    case 4: case 5: case 8: case 13: case 15: case 16: case 17: kind = 4; break;
    default: kind = 1; break;
    }
    unsigned flags = kind | ((code & 0x8000u) ? 0x80u : 0u) | (grouped == 1 ? 0x40u : 0u);
    set_sort_type((void *)0x032c682cu, node, flags, kind == 1 ? 3u : 1u, kind == 1 ? 0u : 3u);
}

/* ---- the finger (pointsrc.c, in fbshim) -------------------------------------------
 * A press on a heading, or on the PREVIEW button, is kept, with its release, from rbp;
 * the release on the same one acts. 1 = taken. */
#define PREVIEW_FILE "/run/rbp/browse.preview"
#define ON_BUTTON    (-2)

static void preview_toggle(void)
{
    int on = !browserows_preview_shown();
    browserows_preview(on);
    int fd = open(PREVIEW_FILE ".tmp", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd >= 0) {
        ssize_t n = write(fd, on ? "on\n" : "off\n", on ? 3 : 4);
        close(fd);
        if (n > 0)
            rename(PREVIEW_FILE ".tmp", PREVIEW_FILE);
    }
    klog("knobshim2: browse: PREVIEW %s\n", on ? "on" : "off");
}

__attribute__((visibility("default"))) int browse_heading_touch(int down, int x, int y)
{
    static int held, sort, missed;
    if (!held) {
        if (!down)
            return 0;
        int at = browserows_preview_button(x, y) ? ON_BUTTON : browserows_heading_sort(x, y);
        if (at == -1)
            return 0;
        held = 1;
        sort = at;
        missed = 0;
        return 1;
    }
    int now = sort == ON_BUTTON ? (browserows_preview_button(x, y) ? ON_BUTTON : -1)
                                : browserows_heading_sort(x, y);
    if (now != sort)
        missed = 1;
    if (!down) {
        held = 0;
        if (missed)
            return 1;
        if (sort == ON_BUTTON)
            preview_toggle();
        else if (!__atomic_load_n(&wanted, __ATOMIC_SEQ_CST)) {
            __atomic_store_n(&wanted, (unsigned)sort + 1u, __ATOMIC_SEQ_CST);
            post();
        }
    }
    return 1;
}

/* From the column chooser's tick, a few times a second on rbp's threads: a tap from
 * TAP_FILE. */
void browsesort_poll(void)
{
    tap_file();
}

/* For a test or a remote: ask for `sort` as a tap would. */
void browsesort_request(unsigned sort)
{
    __atomic_store_n(&wanted, sort + 1u, __ATOMIC_SEQ_CST);
    post();
}

int browsesort_install(void)
{
    static const struct { unsigned address, a, b; } called[] = {
        { 0x18a904, 0xe92d4008, 0xebfa1021 }, { 0x17bd28, 0xe92d41f0, 0xe302596c },
        { 0x186fc4, 0xe92d40f0, 0xe24dd00c }, { 0x187068, 0xe3500000, 0x012fff1e },
        { 0x186e18, 0xeaffde69, 0xeaffded6 }, { 0x187290, 0xe52de004, 0xe24dd00c },
        { 0x1341d8, 0xe30c3a08, 0xe3403114 }, { 0x110c50, 0xe92d4070, 0xe24dd010 },
        { 0x256030, 0xe92d4ff0, 0xe1a09002 },
    };
    for (unsigned i = 0; i < sizeof(called) / sizeof(*called); i++)
        if (memcmp((const void *)(unsigned long)called[i].address, &called[i].a, 8))
            return -1;
    /* the pool and mailbox SendMessage_ToBrowseComm uses, in its literal pool */
    if (word((void *)0x00104118u) != POOL || word((void *)0x00104120u) != MAILBOX)
        return -1;
    static const uint8_t validate_guard[8] = { 0x38, 0x40, 0x2d, 0xe9, 0x01, 0x50, 0xa0, 0xe1 };
    static const uint8_t keys_guard[8] = { 0x01, 0x10, 0x41, 0xe2, 0x30, 0x40, 0x2d, 0xe9 };
    static const uint8_t dispatch_guard[8] = { 0xf0, 0x40, 0x2d, 0xe9, 0x01, 0x50, 0xa0, 0xe1 };
    if (memcmp((const void *)0x104edcul, validate_guard, 8) ||
        memcmp((const void *)0x171780ul, keys_guard, 8) ||
        memcmp((const void *)0x153530ul, dispatch_guard, 8))
        return -1;
    original_keys = rbp_hook(0x171780, keys_guard, (void *)keys_hooked);
    original_dispatch = rbp_hook(0x153530, dispatch_guard, (void *)dispatch_hooked);
    original_validate = rbp_hook(0x104edc, validate_guard, (void *)validate_hooked);
    if (!original_keys || !original_dispatch || !original_validate)
        return -1;
    return 0;
}
