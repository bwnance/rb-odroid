/* SPDX-License-Identifier: MPL-2.0
 *
 * browsemenu.c -- COLUMN in rbp's LIST MENU: the third BROWSE column picked from
 * the popup that has SORT and PLAYSTATE (browserows.h; the column is browsecol.c).
 *
 * The operator's ask (2026-10-28): choose the column where the sort is chosen, as a
 * second way beside the gear SETTINGS row, not a button combo. A pick writes the
 * same word the gear row does (browsecol_choose()).
 *
 * How rbp 1.20 runs a playlist's LIST MENU (read 2026-10-28; context = the browse
 * context in r0 of each function):
 *   - SendBrowseMenu @0x257dd0 builds the menu at depth context+0x6fc8 into the block
 *     M = context+0x43378: the menu's type a word at M+depth*4 (0x400 = LIST MENU),
 *     the count at M+0xc, the first row shown at M+0x10, and entry i at
 *     M+0x22c+i*0x210 (an id word, its state word at +4, its UTF-16 text at +0xc).
 *     LIST MENU is SORT (state 1) and PLAYSTATE; SendRightMenu @0x256218 sends it.
 *   - A pick at depth 0 goes to SelectRightMenu @0x2596cc (the item is the halfword
 *     at request+0x78): for a playlist rbp steps to depth 1, answers GetListDown and
 *     keeps the item in context+0x6fd0 (and the byte +0x6fcc). Whatever the item,
 *     so a third entry steps down too.
 *   - At depth 1 SendBrowseMenu builds the item's own menu: SORT's list when
 *     +0x6fd0 is 0, PLAYSTATE's (type 0x500) otherwise; SendRightMenu titles it
 *     SORT or PLAYSTATE by the byte +0x6fcc.
 *   - A pick at depth 1 under SORT calls SendSortedBrwsList @0x256030 (the sort's
 *     id, its name), which reloads the list and closes the menu, then clears the
 *     depth, +0x6fd0, +0x6fcc and M's first 12 bytes.
 *
 * So: SendRightMenu gets COLUMN as entry 2 of LIST MENU; SendBrowseMenu at depth 1
 * with item 2 builds OFF..TIME (its title COLUMN, through SetMbxComAddString
 * @0x1bb7f8 while that one menu is sent); SelectRightMenu at depth 1 with item 2
 * takes the pick and closes the menu as a SORT pick does, with the list's own
 * sort, so the list reloads with the new column.
 *
 * Threads: all of it runs on rbp's DB_Client task, which serves the GUI's menu
 * requests one at a time.
 */
#include "browserows.h"

#include <string.h>

#include "rbp_hook.h"
#include "shimutil.h"       /* klog() */

#define MENU          0x43378u      /* M */
#define DEPTH         0x6fc8u
#define PICKED        0x6fd0u
#define PICKED_BYTE   0x6fccu
#define OWNER_FROM    0x6f54u       /* [[context+0x6f54]+0x10] is an entry's id */
#define ENTRY(i)      (MENU + 0x22cu + (i) * 0x210u)
#define ENTRY_TEXT    0xcu          /* from ENTRY(i) */
#define ENTRY_CHARS   0x100u
#define ENTRIES       20u
#define LIST_MENU     0x400u
#define SUBMENU       0x500u        /* PLAYSTATE's type, kept for ours */
#define COLUMN_ITEM   2u
#define STATE_FOLDER  1u            /* LIST MENU's SORT */
#define STATE_OPTION  0x63u         /* PLAYSTATE's PLAYED / UNPLAYED */
#define STATE_CHECKED 0x400u
/* SendRightMenu's depth-1 title for a playlist when +0x6fcc is not 0: "PLAYSTATE" */
#define PLAYSTATE_TITLE 0x004c6f78ul

static const char *const words[] = { "off", "artist", "album", "bpm", "key", "time" };
#define WORDS (sizeof(words) / sizeof(*words))
static const char *const shown[WORDS] = { "OFF", "ARTIST", "ALBUM", "BPM", "KEY", "TIME" };
static const uint16_t column_text[] = { 'C', 'O', 'L', 'U', 'M', 'N', 0 };

static unsigned word(const uint8_t *p) { unsigned v; memcpy(&v, p, 4); return v; }
static void put_word(uint8_t *p, unsigned v) { memcpy(p, &v, 4); }
static unsigned half(const uint8_t *p) { uint16_t v; memcpy(&v, p, 2); return v; }

static void put_text(uint8_t *at, const char *ascii)
{
    unsigned n = 0;
    for (; ascii[n] && n < ENTRY_CHARS - 1; n++) {
        uint16_t c = (uint8_t)ascii[n];
        memcpy(at + n * 2, &c, 2);
    }
    memset(at + n * 2, 0, 2);
}

static int text_is(const uint8_t *at, const char *ascii)
{
    unsigned n = 0;
    for (; ascii[n]; n++)
        if (half(at + n * 2) != (uint8_t)ascii[n])
            return 0;
    return half(at + n * 2) == 0;
}

/* rbp's LIST MENU, just built at depth 0 in a track list: SORT and PLAYSTATE. */
static int list_menu(const uint8_t *context)
{
    return word(context) == 2 && word(context + DEPTH) == 0 &&
           word(context + MENU) == LIST_MENU && word(context + MENU + 0xc) == 2 &&
           text_is(context + ENTRY(0) + ENTRY_TEXT, "SORT");
}

/* COLUMN's own menu: depth 1, stepped into from LIST MENU's third entry. */
static int column_menu(const uint8_t *context)
{
    return word(context) == 2 && word(context + DEPTH) == 1 &&
           word(context + MENU) == LIST_MENU && word(context + PICKED) == COLUMN_ITEM &&
           context[PICKED_BYTE] == COLUMN_ITEM;
}

int browsemenu_add_column(uint8_t *context)
{
    if (!list_menu(context))
        return 0;
    memcpy(context + ENTRY(COLUMN_ITEM), context + ENTRY(0), ENTRY_TEXT);
    put_word(context + ENTRY(COLUMN_ITEM) + 4, STATE_FOLDER);
    memcpy(context + ENTRY(COLUMN_ITEM) + ENTRY_TEXT, column_text, sizeof column_text);
    put_word(context + MENU + 0xc, 3);
    return 1;
}

/* The field `value` shows, 0 for off or unknown. */
static unsigned field_of(const char *value)
{
    const uint16_t *caption;
    return value ? browsecol_field(value, &caption) : 0;
}

void browsemenu_fill(uint8_t *context, unsigned first, const char *current)
{
    unsigned owner = 0, now = field_of(current);
    const uint8_t *from = (const uint8_t *)(unsigned long)word(context + OWNER_FROM);
    if (from)
        owner = word(from + 0x10);
    for (unsigned i = 0; i < ENTRIES; i++) {
        put_word(context + ENTRY(i), 0);
        memset(context + ENTRY(i) + ENTRY_TEXT, 0, 2);
    }
    put_word(context + MENU + 4, SUBMENU);
    for (unsigned i = 0; i < WORDS; i++) {
        put_word(context + ENTRY(i), owner);
        put_word(context + ENTRY(i) + 4,
                 STATE_OPTION | (field_of(words[i]) == now ? STATE_CHECKED : 0));
        put_text(context + ENTRY(i) + ENTRY_TEXT, shown[i]);
    }
    put_word(context + MENU + 0xc, WORDS);
    put_word(context + MENU + 0x10, first);
    put_word(context + MENU + 0x14, 0);
}

const char *browsemenu_word(unsigned item)
{
    return item < WORDS ? words[item] : NULL;
}

/* ---- rbp 1.20 --------------------------------------------------------------------- */
static int (*send_browse_menu)(uint8_t *, void *, uint8_t *);
static int (*send_right_menu)(uint8_t *, void *, uint8_t *);
static int (*select_right_menu)(uint8_t *, void *, uint8_t *);
static int (*add_string)(void *, unsigned, unsigned, unsigned, unsigned, const uint16_t *,
                         unsigned);
static int (*send_sorted)(uint8_t *, void *, uint8_t *, unsigned, const uint16_t *) =
    (void *)0x00256030ul;                                           /* SendSortedBrwsList */
static volatile unsigned titling;

static int add_string_hooked(void *mbx, unsigned id, unsigned a, unsigned b, unsigned c,
                             const uint16_t *text, unsigned max)
{
    if (titling && (unsigned long)text == PLAYSTATE_TITLE)
        text = column_text;
    return add_string(mbx, id, a, b, c, text, max);
}

static int send_right_hooked(uint8_t *context, void *mbx, uint8_t *request)
{
    browsemenu_add_column(context);
    if (!column_menu(context))
        return send_right_menu(context, mbx, request);
    titling = 1;
    int result = send_right_menu(context, mbx, request);
    titling = 0;
    return result;
}

static int send_browse_hooked(uint8_t *context, void *mbx, uint8_t *request)
{
    /* request+124 == 1 opens a menu afresh: rbp resets the depth itself */
    if (half(request + 124) == 1 || !column_menu(context))
        return send_browse_menu(context, mbx, request);
    browsemenu_fill(context, half(request + 120), browsecol_current());
    if (!context[0x14])
        send_right_hooked(context, mbx, request);
    return 0;
}

static int select_hooked(uint8_t *context, void *mbx, uint8_t *request)
{
    int taken;
    if (browsesort_select(context, mbx, request, &taken))
        return taken;       /* a heading tapped (browsesort.c) */
    if (!column_menu(context))
        return select_right_menu(context, mbx, request);
    /* the list as SendSortedBrwsList finds it: its sort's id at +0x10, name at +24 */
    unsigned index = word(context + 4);
    const uint8_t *list = index <= 4 ? (const uint8_t *)(unsigned long)word(
                                           context + (index + 0x1bc0u) * 4u + 0x3cu)
                                     : NULL;
    if (!list)
        return select_right_menu(context, mbx, request);
    unsigned item = half(request + 0x78);
    const char *chosen = browsemenu_word(item);
    if (chosen) {
        browsecol_choose(chosen);
        klog("knobshim2: browse column: %s from LIST MENU\n", chosen);
    }
    /* close as a SORT pick does: the list again, under its own sort */
    uint16_t name[ENTRY_CHARS] = { 0 };
    unsigned sort = word(list + 0x10);
    for (unsigned n = 0; n < ENTRY_CHARS - 1 && half(list + 24 + n * 2); n++)
        name[n] = (uint16_t)half(list + 24 + n * 2);
    context[0x15] = 1;
    send_sorted(context, mbx, request, sort, name);
    if (!context[0x14]) {
        put_word(context + PICKED, 0);
        put_word(context + DEPTH, 0);
        context[PICKED_BYTE] = 0;
        memset(context + MENU, 0, 12);
    }
    return 0;
}

int browsemenu_install(void)
{
    /* each: push {r4-r11,lr}, then sub sp / add r7,r0,#0x43000 / movw r8,#0x6fc8 --
     * none pc-relative */
    static const uint8_t browse_guard[8] = { 0xf0, 0x4f, 0x2d, 0xe9, 0x43, 0x7a, 0x80, 0xe2 };
    static const uint8_t right_guard[8] = { 0xf0, 0x4f, 0x2d, 0xe9, 0x1c, 0xd0, 0x4d, 0xe2 };
    static const uint8_t select_guard[8] = { 0xf0, 0x4f, 0x2d, 0xe9, 0xc8, 0x8f, 0x06, 0xe3 };
    static const uint8_t string_guard[8] = { 0xf0, 0x4f, 0x2d, 0xe9, 0x0c, 0xd0, 0x4d, 0xe2 };
    /* SendSortedBrwsList: push {r4-r11,lr} ; mov r9,r2. "PLAYSTATE" at the title. */
    static const uint8_t sorted_guard[8] = { 0xf0, 0x4f, 0x2d, 0xe9, 0x02, 0x90, 0xa0, 0xe1 };
    static const uint8_t title[8] = { 'P', 0, 'L', 0, 'A', 0, 'Y', 0 };
    static const struct { unsigned long at; const uint8_t *guard; } sites[] = {
        { 0x257dd0, browse_guard }, { 0x256218, right_guard }, { 0x2596cc, select_guard },
        { 0x1bb7f8, string_guard }, { 0x256030, sorted_guard }, { PLAYSTATE_TITLE, title },
    };
    for (unsigned i = 0; i < sizeof(sites) / sizeof(*sites); i++)
        if (memcmp((const void *)sites[i].at, sites[i].guard, 8))
            return -1;
    add_string = rbp_hook(0x1bb7f8, string_guard, (void *)add_string_hooked);
    if (!add_string)
        return -1;
    send_right_menu = rbp_hook(0x256218, right_guard, (void *)send_right_hooked);
    send_browse_menu = rbp_hook(0x257dd0, browse_guard, (void *)send_browse_hooked);
    select_right_menu = rbp_hook(0x2596cc, select_guard, (void *)select_hooked);
    if (!send_right_menu || !send_browse_menu || !select_right_menu)
        return -1;      /* only on mmap/mprotect failing; whatever went in passes through */
    return 0;
}
