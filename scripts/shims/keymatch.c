/* SPDX-License-Identifier: MPL-2.0
 *
 * keymatch.c -- Key Match: BROWSE's key icons for Energy Boost matches too.
 *
 * rbp marks a track whose key mixes with the MASTER deck's with its green key
 * icon. This adds, for keys rbp leaves unmarked, the same icon in yellow for an
 * Energy Boost +2 (8A -> 10A), orange for +7 (8A -> 3A) and red for the
 * experimental +4 (8A -> 12A), each from the MASTER's key, A to A and B to B.
 * Green stays rbp's own. No key is changed: it is a suggestion to audition.
 *
 * Adapted from rx3-toolkit (github.com/Tratosca/rx3-toolkit at cc17a1f,
 * MPL-2.0): mod/modules/key-match/rx3_key_match_module.c, the module as it is
 * less its unloading. This file stays under the MPL. The rows are rewritten by
 * browserows.c, the coloured icons served by rbp_images.c.
 *
 * How the colour gets on the glass (rbp 1.20, read 2026-10-27): the row hook
 * gives a matching track the category 120 + n (yellow, orange, red). rbp's
 * ui_ListBrowse_SetIcon @0x295904 asks ui_com_ListIconImageIdGet @0x194578 for
 * the row's icon -- which answers 0xffff (none) above category 119 -- and puts it
 * up with ui_com_ShowIcon @0x193c14. Here the icon lookup answers for 120..122
 * what it answers for 52, rbp's green (image 227), noting the colour; inside
 * that same SetIcon call, ShowIcon's green key states 0xa0f..0xa13 become their
 * recoloured copies. A row rbp itself marks green is never touched.
 *
 * RB_KEY_MATCH=0 leaves BROWSE as rbp ships it; RB_KEY_MATCH_RULES picks the
 * colours: 2 yellow, 4 orange, 8 red, added up (14 = all three).
 */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "browserows.h"
#include "envutil.h"
#include "keyshift.h"       /* keyshift_get(): the MASTER's key as heard */
#include "rbp_hook.h"
#include "rbp_images.h"
#include "rbp_key.h"        /* is_rbp_process() */
#include "rx3_harmony.h"
#include "shimutil.h"       /* klog() */

#define ICON_ID   0x00194578ul      /* int ui_com_ListIconImageIdGet(category, selected, side) */
#define SET_ICON  0x00295904ul      /* void ui_ListBrowse_SetIcon(side, row) */
#define SHOW_ICON 0x00193c14ul      /* int ui_com_ShowIcon(window, object, image) */
/* cmp r0,#119 ; str r4,[sp,#-4]! */
static const uint8_t icon_guard[8] = { 0x77, 0x00, 0x50, 0xe3, 0x04, 0x40, 0x2d, 0xe5 };
/* push {r4-r11,lr} ; mov r5,r1 */
static const uint8_t set_guard[8] = { 0xf0, 0x4f, 0x2d, 0xe9, 0x01, 0x50, 0xa0, 0xe1 };
/* cmn r2,#1 ; push {r4,r5,r6,lr} */
static const uint8_t show_guard[8] = { 0x01, 0x00, 0x72, 0xe3, 0x70, 0x40, 0x2d, 0xe9 };

#define KEY_GREEN  BROWSE_CATEGORY_GREEN
#define KEY_YELLOW 120u
#define KEY_ORANGE 121u
#define KEY_RED    122u
#define GREEN_ICON 227                  /* category 52's image */
#define KEY_STATE_FIRST 0xa0fu          /* ShowIcon's five green key states */
#define COLOUR_COUNT 3u

static const uint16_t colours[COLOUR_COUNT] = { 0xffe0u, 0xfc00u, 0xf800u };
static unsigned int rules, coloured_images[COLOUR_COUNT][5], badges[4];
/* rbp builds BROWSE's icons on its UI thread, one SetIcon at a time */
static unsigned int composing, pending_colour;

typedef int (*icon_fn)(unsigned int, int, int);
static icon_fn original_icon;
static void (*original_set)(unsigned int, unsigned int);
static int (*original_show)(void *, int, int);

static void set_icon(unsigned int side, unsigned int row)
{
    unsigned saved = composing, pending = pending_colour;
    composing = 1;
    pending_colour = 0;
    original_set(side, row);
    composing = saved;
    pending_colour = pending;
}

static int show_icon(void *window, int object, int image)
{
    if (composing && pending_colour && image >= (int)KEY_STATE_FIRST &&
        image < (int)KEY_STATE_FIRST + 5)
        image = (int)coloured_images[pending_colour - 1][image - KEY_STATE_FIRST];
    return original_show(window, object, image);
}

static int icon(unsigned int category, int selected, int side)
{
    if (category >= KEY_YELLOW && category <= KEY_RED) {
        int result = original_icon(KEY_GREEN, selected, side);
        if (composing && result == GREEN_ICON)
            pending_colour = category - KEY_YELLOW + 1;
        return result;
    }
    return original_icon(category, selected, side);
}

/* A track row's category: rbp's, unless rbp left it unmarked and a chosen rule
 * matches. Pure, for the test. */
unsigned int keymatch_category(unsigned int reference, unsigned int key, unsigned int native,
                               unsigned int rule_mask)
{
    if (native == KEY_GREEN || reference < 1 || reference > 24 || key < 1 || key > 24)
        return native;
    switch (rx3_camelot_extended((int)reference - 1, (int)key - 1, rule_mask)) {
    case RX3_MATCH_BOOST_TWO:
        return KEY_YELLOW;
    case RX3_MATCH_BOOST_SEVEN:
        return KEY_ORANGE;
    case RX3_MATCH_FOUR:
        return KEY_RED;
    default:
        return native;
    }
}

static unsigned category(unsigned reference, unsigned key, unsigned native)
{
    return keymatch_category(reference, key, native, rules);
}

static unsigned badge(unsigned kind)
{
    return kind == KEY_GREEN ? badges[0] :
           kind >= KEY_YELLOW && kind <= KEY_RED ? badges[kind - KEY_YELLOW + 1] : 0;
}

static const struct browse_marker markers = { category, badge };

static int deck_shift(int deck) { return keyshift_get(deck); }

__attribute__((constructor)) static void keymatch_init(void)
{
    if (!env_flag("KEY_MATCH", 1) || !is_rbp_process())
        return;
    rules = rx3_match_rules(getenv("KEY_MATCH_RULES"));
    if (!rules) {
        klog("knobshim2: key match: RB_KEY_MATCH_RULES chooses no colour -- off\n");
        return;
    }
    /* the badge beside a KEY column's value: image 227's key, recoloured */
    badges[0] = rbp_images_recolour(GREEN_ICON, 7755, 0x07e0);
    for (unsigned c = 0; c < COLOUR_COUNT; c++) {
        for (unsigned i = 0; i < 5; i++)
            coloured_images[c][i] = rbp_images_recolour(KEY_STATE_FIRST + i, 0x07e0u, colours[c]);
        badges[c + 1] = rbp_images_recolour(GREEN_ICON, 7755, colours[c]);
    }
    if (!badges[3] || !coloured_images[2][4] || rbp_images_install()) {
        klog("knobshim2: key match: NS_GetImageInfoByID is not the one measured -- off\n");
        return;
    }
    if (memcmp((const void *)ICON_ID, icon_guard, 8) || memcmp((const void *)SET_ICON, set_guard, 8) ||
        memcmp((const void *)SHOW_ICON, show_guard, 8)) {
        klog("knobshim2: key match: BROWSE's icon functions are not the ones measured -- off\n");
        return;
    }
    original_set = rbp_hook(SET_ICON, set_guard, (void *)set_icon);
    original_show = rbp_hook(SHOW_ICON, show_guard, (void *)show_icon);
    original_icon = (icon_fn)rbp_hook(ICON_ID, icon_guard, (void *)icon);
    browserows_shift = deck_shift;
    if (!original_set || !original_show || !original_icon || browserows_marker(&markers)) {
        klog("knobshim2: key match: BROWSE's row builder is not the one measured -- off\n");
        return;
    }
    klog("knobshim2: key match: on, rules %u (2 yellow +2, 4 orange +7, 8 red +4)\n", rules);
}
