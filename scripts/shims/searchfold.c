/* SPDX-License-Identifier: MPL-2.0
 *
 * searchfold.c -- rbp's track search, accent-insensitive.
 *
 * Typing NINO on rbp's search keyboard finds NIÑO. rbp's search compares the
 * typed words, as typed, against an upper-cased copy of each track's text that
 * shape() @0x1644fc makes -- and shape() upper-cases a..z only, so a title with
 * an accented letter never matches its plain spelling. This folds the copy:
 * Latin-1's letters (U+00C0..U+00FF, and U+0178 Ÿ) to plain upper-case ASCII
 * (Ð D, Þ T, ß S -- one letter for one, so no length changes; × and ÷ are left).
 *
 * rx3-toolkit's search-latin module (github.com/Tratosca/rx3-toolkit at
 * cc17a1f, MPL-2.0), the fold table as it is, with one change: the toolkit's
 * hook returns nothing, but shape() RETURNS THE CHARACTER COUNT, which its
 * callers (IsMatchSearchTarget @0x164230, IsMatchSearchTargetNarrowDown
 * @0x164018) keep as the target's length -- read on this binary 2026-10-26 --
 * so this hook hands the original's count back. The typed words are not folded:
 * rbp's keyboard has no accented letters.
 *
 * RB_SEARCH_FOLD=0 leaves rbp's search as it ships. Off the glass until a
 * touchscreen (or a keyboard) types into rbp's search.
 */
#include <stdint.h>
#include <stdlib.h>

#include "envutil.h"
#include "rbp_hook.h"
#include "rbp_key.h"        /* is_rbp_process() */
#include "shimutil.h"       /* klog() */

#define SHAPE 0x001644fcul  /* unsigned shape(uint16_t *dst, const uint16_t *src, unsigned max) */
/* sub sp,sp,#24 ; str r0,[sp,#12] -- nothing pc-relative */
static const uint8_t shape_guard[8] = { 0x18, 0xd0, 0x4d, 0xe2, 0x0c, 0x00, 0x8d, 0xe5 };

typedef unsigned (*shape_fn)(uint16_t *, const uint16_t *, unsigned);
static shape_fn original_shape;

static const uint16_t fold_table[64] = {
    0x41, 0x41, 0x41, 0x41, 0x41, 0x41, 0x41, 0x43, 0x45, 0x45, 0x45, 0x45, 0x49, 0x49, 0x49, 0x49,
    0x44, 0x4e, 0x4f, 0x4f, 0x4f, 0x4f, 0x4f, 0xd7, 0x4f, 0x55, 0x55, 0x55, 0x55, 0x59, 0x54, 0x53,
    0x41, 0x41, 0x41, 0x41, 0x41, 0x41, 0x41, 0x43, 0x45, 0x45, 0x45, 0x45, 0x49, 0x49, 0x49, 0x49,
    0x44, 0x4e, 0x4f, 0x4f, 0x4f, 0x4f, 0x4f, 0xf7, 0x4f, 0x55, 0x55, 0x55, 0x55, 0x59, 0x54, 0x59
};

/* `n` characters of `text`, folded in place. Pure, for the test. */
void searchfold_fold(uint16_t *text, unsigned n)
{
    for (unsigned i = 0; i < n; i++) {
        uint16_t g = text[i];
        if (!g)
            break;
        if (g >= 0xC0 && g <= 0xFF)
            text[i] = fold_table[g - 0xC0];
        else if (g == 0x178)
            text[i] = 'Y';
    }
}

static unsigned hooked_shape(uint16_t *dst, const uint16_t *src, unsigned max)
{
    unsigned n = original_shape(dst, src, max);

    if (dst)
        searchfold_fold(dst, n < max ? n : max);
    return n;
}

__attribute__((constructor)) static void searchfold_init(void)
{
    if (!atoi(env_str("SEARCH_FOLD", "1")) || !is_rbp_process())
        return;
    original_shape = (shape_fn)rbp_hook(SHAPE, shape_guard, (void *)hooked_shape);
    klog(original_shape ? "knobshim2: search: accents folded (shape @0x1644fc hooked)\n"
                        : "knobshim2: search: shape() is not the one measured -- search "
                          "left as rbp ships it\n");
}
