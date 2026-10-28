/*
 * touchpreview.c -- the RX3's touch preview plays (docs/21 item 34: "touching a
 * waveform does not play in the headphones").
 *
 * How rbp 1.20 plays a touch preview (read 2026-10-28): BrowseUiIf::TouchPreviewProc
 * @0xd0014 asks the Ui_BrowsePreview task to load the touched row; that task calls
 * ui::PlayerPreview::loadPreview @0x30a47c (and setPreviewPosition @0x30a560 on a
 * drag, unloadPreview @0x30a618 on release), each of which posts its work to the UI
 * thread -- and refuses, printing "Err! [UI Thread]", when isOnMessageThread() says
 * the caller is the UI thread already. rbp_patch.py makes
 * PanelComPeerLinux::isOnMessageThread @0x3664a0 always say so (0x3664b4, r3 = 0;
 * the panel thread whose id it compares never runs here), so every preview was
 * refused before it reached the player.
 *
 * The answer is left as the patch has it, except for the thread inside one of
 * those three calls: it is told no, so they post as they do on an RX3.
 */
#include <stdint.h>

#include "rbp_hook.h"
#include "rbp_key.h"        /* is_rbp_process() */
#include "shimutil.h"       /* klog() */

static long caller;         /* the thread inside a preview call, or 0 */

static long thread_id(void)
{
    register long r0 __asm__("r0");
    register long r7 __asm__("r7") = 224;       /* gettid */
    __asm__ volatile("svc 0" : "=r"(r0) : "r"(r7) : "memory");
    return r0;
}

static int on_message_thread(void *peer)
{
    (void)peer;
    return __atomic_load_n(&caller, __ATOMIC_SEQ_CST) != thread_id();
}

static int (*original_load)(void *, const void *, unsigned);
static int (*original_position)(void *, unsigned);
static int (*original_unload)(void *);

/* The float arguments are passed as their bits: rbp is soft-float. */
static int load(void *preview, const void *info, unsigned position)
{
    __atomic_store_n(&caller, thread_id(), __ATOMIC_SEQ_CST);
    int r = original_load(preview, info, position);
    __atomic_store_n(&caller, 0, __ATOMIC_SEQ_CST);
    return r;
}

static int position(void *preview, unsigned at)
{
    __atomic_store_n(&caller, thread_id(), __ATOMIC_SEQ_CST);
    int r = original_position(preview, at);
    __atomic_store_n(&caller, 0, __ATOMIC_SEQ_CST);
    return r;
}

static int unload(void *preview)
{
    __atomic_store_n(&caller, thread_id(), __ATOMIC_SEQ_CST);
    int r = original_unload(preview);
    __atomic_store_n(&caller, 0, __ATOMIC_SEQ_CST);
    return r;
}

__attribute__((constructor)) static void touchpreview_init(void)
{
    if (!is_rbp_process())
        return;
    /* each: push {...,lr} ; mov rN,r0 */
    static const uint8_t thread_guard[8] = { 0x10, 0x40, 0x2d, 0xe9, 0x00, 0x40, 0xa0, 0xe1 };
    static const uint8_t load_guard[8] = { 0xf0, 0x41, 0x2d, 0xe9, 0x00, 0x50, 0xa0, 0xe1 };
    static const uint8_t position_guard[8] = { 0x70, 0x40, 0x2d, 0xe9, 0x00, 0x40, 0xa0, 0xe1 };
    static const uint8_t unload_guard[8] = { 0x38, 0x40, 0x2d, 0xe9, 0x00, 0x50, 0xa0, 0xe1 };
    /* the patch's word, so the answer kept is the one measured */
    if (*(const uint32_t *)0x3664b4ul != 0xe3a03000u) {
        klog("knobshim2: touch preview: isOnMessageThread is not the patched one -- left\n");
        return;
    }
    original_load = rbp_hook(0x30a47c, load_guard, (void *)load);
    original_position = rbp_hook(0x30a560, position_guard, (void *)position);
    original_unload = rbp_hook(0x30a618, unload_guard, (void *)unload);
    if (!original_load || !original_position || !original_unload) {
        klog("knobshim2: touch preview: rbp's preview player is not the one measured -- left\n");
        return;
    }
    /* replaced whole: its trampoline is never called */
    if (!rbp_hook(0x3664a0, thread_guard, (void *)on_message_thread))
        klog("knobshim2: touch preview: hook failed\n");
}
