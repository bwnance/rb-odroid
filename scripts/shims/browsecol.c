/* SPDX-License-Identifier: MPL-2.0
 *
 * browsecol.c -- Browse Columns: a third column in BROWSE's track lists, and
 * which one.
 *
 * rbp's BROWSE track list shows the title and one more field -- the sort's, or in
 * an unsorted list the stick's MY SETTINGS > Column. This adds a third, as on a
 * CDJ-3000: TRACK | the chosen field | rbp's sort column (browserows.c draws it).
 *
 * WHICH FIELD (operator, 2026-10-28): the stick's own MY SETTINGS > Column by
 * default, changed live from the gear SETTINGS panel. The choice is one word in
 * RB_BROWSE_COLUMN_FILE (/run/rbp/browse.column): off, artist, album, bpm, key or
 * time. The panel writes it; this re-reads it a few times a second. A stick put in,
 * or re-exported, writes its own Column there -- rekordbox keeps it in the stick's
 * export.pdb, table 18 (the players' SORT list, rows of 8 bytes: rekordbox's id,
 * rbp's MODULEMAIN_CATESORT id, a flag, the menu position), as the one row whose
 * flag is 2: found by exporting ARTIST, then ALBUM, and comparing, 2026-10-28.
 * rbp reads the same flag for its own unsorted second column. With neither,
 * RB_BROWSE_COLUMN.
 *
 * Adapted from rx3-toolkit (github.com/Tratosca/rx3-toolkit at cc17a1f,
 * MPL-2.0): mod/modules/browse-columns/rx3_browse_columns_module.c. This file
 * stays under the MPL.
 *
 * RB_BROWSE_COLUMNS=0 leaves BROWSE exactly as rbp ships it (no hook at all).
 */
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/time.h>
#include <unistd.h>

#include "browserows.h"
#include "envutil.h"
#include "rbp_key.h"        /* is_rbp_process() */
#include "shimutil.h"       /* klog() */

static const uint16_t bpm[] = { 'B', 'P', 'M', 0 }, key[] = { 'K', 'E', 'Y', 0 },
                      artist[] = { 'A', 'R', 'T', 'I', 'S', 'T', 0 },
                      duration[] = { 'T', 'I', 'M', 'E', 0 },
                      album[] = { 'A', 'L', 'B', 'U', 'M', 0 };

static const struct {
    const char *name, *number;
    unsigned category;          /* rbp's MODULEMAIN_CATESORT id */
    struct browse_column column;
} names[] = {
    { "artist", "7", 2, { BROWSE_FIELD_ARTIST, artist } },
    { "album", "2", 3, { BROWSE_FIELD_ALBUM, album } },
    { "bpm", "13", 4, { BROWSE_FIELD_BPM, bpm } },
    { "key", "15", 12, { BROWSE_FIELD_KEY, key } },
    { "time", "11", 8, { BROWSE_FIELD_TIME, duration } },
    { "length", "11", 8, { BROWSE_FIELD_TIME, duration } },
};
#define NAMES (sizeof(names) / sizeof(*names))

static int find(const char *value)
{
    if (!value)
        return -1;
    for (unsigned i = 0; i < NAMES; i++)
        if (!strcasecmp(value, names[i].name) || !strcmp(value, names[i].number))
            return (int)i;
    return -1;
}

/* A word as a field and its heading: 0 for off or unknown. Pure, for the test. */
unsigned int browsecol_field(const char *value, const uint16_t **caption)
{
    int i = find(value);
    if (i < 0)
        return 0;
    *caption = names[i].column.caption;
    return names[i].column.field;
}

/* The word for rbp's category `category`, or NULL for one not shown here. */
const char *browsecol_category_word(unsigned category)
{
    for (unsigned i = 0; i < NAMES; i++)
        if (names[i].category == category)
            return names[i].name;
    return NULL;
}

/* ---- the stick's MY SETTINGS > Column ----------------------------------------
 * export.pdb (DeviceSQL; Deep Symmetry's crate-digger notes): a header page whose
 * table list starts at 0x1c, 16 bytes per table (type, empty candidate, first page,
 * last page); data pages chained by their word at +12; a page's rows counted at
 * +0x18 (or +0x22 when larger), their heap at +0x28, and the row index at the page's
 * end, in groups of sixteen: offsets backwards, then a presence mask. */
#define PDB_COLUMN_TABLE 18u
static unsigned rd16(const uint8_t *p) { return (unsigned)p[0] | (unsigned)p[1] << 8; }
static unsigned rd32(const uint8_t *p) { return rd16(p) | rd16(p + 2) << 16; }

/* rbp's category of the flag-2 row on one table-18 page of `size` bytes, or 0.
 * Pure, for the test. */
unsigned int browsecol_page_column(const uint8_t *page, unsigned int size)
{
    unsigned small = page[0x18], large = rd16(page + 0x22);
    unsigned rows = large > small && large != 0x1fff ? large : small;
    if (size < 0x100 || (page[0x1b] & 0x40) || rows > 16 * ((size - 0x28) / 0x24))
        return 0;
    for (unsigned g = 0; g * 16 < rows; g++) {
        unsigned base = size - g * 0x24, present = rd16(page + base - 4);
        for (unsigned r = 0; r < 16 && g * 16 + r < rows; r++) {
            unsigned off = rd16(page + base - 6 - r * 2);
            if (!(present >> r & 1) || 0x28 + off + 8 > size - (g + 1) * 0x24)
                continue;
            const uint8_t *row = page + 0x28 + off;
            if (row[4] == 2)
                return rd16(row + 2);
        }
    }
    return 0;
}

/* The flag-2 row's category in the export.pdb at `path`; 0 if none or unreadable. */
static unsigned stick_column(const char *path)
{
    static uint8_t page[8192];
    unsigned found = 0;
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return 0;
    if (pread(fd, page, 0x200, 0) == 0x200) {
        unsigned size = rd32(page + 4), tables = rd32(page + 8);
        for (unsigned t = 0; size >= 0x100 && size <= sizeof page && t < tables && t < 30; t++) {
            const uint8_t *e = page + 0x1c + t * 16;
            if (rd32(e) != PDB_COLUMN_TABLE)
                continue;
            unsigned at = rd32(e + 8), last = rd32(e + 12);
            for (unsigned n = 0; n < 64 && at && !found; n++) {
                if (pread(fd, page, size, (off_t)at * size) != (ssize_t)size)
                    break;
                found = browsecol_page_column(page, size);
                if (at == last)
                    break;
                at = rd32(page + 12);
            }
            break;
        }
    }
    close(fd);
    return found;
}

/* ---- the choice ------------------------------------------------------------- */
static char choice_path[128];
static const char *start_word = "";
static unsigned tick_gate;
static unsigned last_tick_ms;     /* 32 bits: armv5 has no 64-bit atomics */
static char current[16] = "-";
/* each stick's export.pdb as last seen: its header page's first 0x40 bytes (DeviceSQL
 * counts every write there) and its length. NOT stat(): rbp's glibc 2.13 exports no
 * `stat` symbol, and a shim calling it fails to load (vidmask.c says the same). */
static struct { uint8_t head[0x40]; long long size; int there; } sticks[2];

static unsigned now_ms(void)
{
    struct timeval tv;      /* not clock_gettime: glibc 2.13 keeps it in librt */
    gettimeofday(&tv, NULL);
    return (unsigned)tv.tv_sec * 1000u + (unsigned)tv.tv_usec / 1000u;
}

static void apply(const char *word)
{
    if (!word)
        word = "";
    int i = find(word);
    if (!strcmp(word, current))
        return;
    snprintf(current, sizeof current, "%s", word);
    browserows_set(i < 0 ? NULL : &names[i].column);
    klog("knobshim2: browse column: %s\n", i < 0 ? "off" : names[i].name);
}

static void write_choice(const char *word)
{
    char tmp[160];
    snprintf(tmp, sizeof tmp, "%s.tmp", choice_path);
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0)
        return;
    ssize_t n = write(fd, word, strlen(word));
    if (n >= 0)
        n = write(fd, "\n", 1);
    close(fd);
    if (n >= 0)
        rename(tmp, choice_path);
}

/* A stick put in or re-exported: its Column becomes the choice. */
static void look_at_sticks(void)
{
    static const char *const pdb[2] = { "/media/usb1/%s/PIONEER/rekordbox/export.pdb",
                                        "/media/usb2/%s/PIONEER/rekordbox/export.pdb" };
    static const char *const parts[] = { "sda1", "sdb1", "sdc1", "sdd1", "sda", "sdb" };
    for (unsigned s = 0; s < 2; s++) {
        char path[96];
        int fd = -1;
        for (unsigned p = 0; p < sizeof parts / sizeof *parts && fd < 0; p++) {
            snprintf(path, sizeof path, pdb[s], parts[p]);
            fd = open(path, O_RDONLY);
        }
        if (fd < 0) {
            sticks[s].there = 0;
            continue;
        }
        uint8_t head[0x40];
        long long size = (long long)lseek(fd, 0, SEEK_END);
        int same = pread(fd, head, sizeof head, 0) == (ssize_t)sizeof head && sticks[s].there &&
                   sticks[s].size == size && !memcmp(head, sticks[s].head, sizeof head);
        close(fd);
        if (same)
            continue;
        memcpy(sticks[s].head, head, sizeof head);
        sticks[s].size = size;
        sticks[s].there = 1;
        unsigned category = stick_column(path);
        const char *word = browsecol_category_word(category);
        klog("knobshim2: browse column: USB%u's MY SETTINGS column is category %u (%s)\n", s + 1,
             category, word ? word : "not one shown here");
        if (word)
            write_choice(word);
    }
}

/* A choice made in rbp's LIST MENU (browsemenu.c): the file, as the gear row writes
 * it, read at the next page -- the one the pick reloads. */
void browsecol_choose(const char *word)
{
    write_choice(word);
    __atomic_store_n(&last_tick_ms, now_ms() - 1000u, __ATOMIC_RELAXED);
}

/* The word in effect ("" or "-" for none yet). */
const char *browsecol_current(void)
{
    return current;
}

static void tick(void)
{
    unsigned now = now_ms();
    if (now - __atomic_load_n(&last_tick_ms, __ATOMIC_RELAXED) < 300u ||
        __sync_lock_test_and_set(&tick_gate, 1u))
        return;
    last_tick_ms = now;
    static unsigned last_sticks, looked;
    if (!looked || now - last_sticks >= 1000u) {
        looked = 1;
        last_sticks = now;
        look_at_sticks();
    }
    char word[16] = { 0 };
    int fd = open(choice_path, O_RDONLY);
    if (fd >= 0) {
        ssize_t n = read(fd, word, sizeof word - 1);
        close(fd);
        word[n > 0 ? n : 0] = 0;
        word[strcspn(word, " \t\r\n")] = 0;
    }
    apply(word[0] ? word : start_word);
    __sync_lock_release(&tick_gate);
}

__attribute__((constructor)) static void browsecol_init(void)
{
    if (!env_flag("BROWSE_COLUMNS", 1) || !is_rbp_process())
        return;
    snprintf(choice_path, sizeof choice_path, "%s",
             env_str("BROWSE_COLUMN_FILE", "/run/rbp/browse.column"));
    start_word = env_str("BROWSE_COLUMN", "");
    if (start_word[0] && strcmp(start_word, "0") && strcmp(start_word, "off") && find(start_word) < 0)
        klog("knobshim2: browse column: RB_BROWSE_COLUMN=%s is not artist, album, bpm, key or "
             "time -- off until chosen\n", start_word);
    browserows_tick = tick;
    if (browserows_install()) {
        browserows_tick = NULL;
        klog("knobshim2: browse column: BROWSE's row builder or renderer is not the one "
             "measured -- off\n");
        return;
    }
    klog("knobshim2: browse column: hooks in; the choice is %s\n", choice_path);
    if (browsemenu_install())
        klog("knobshim2: browse column: rbp's LIST MENU is not the one measured -- no "
             "COLUMN there\n");
}
