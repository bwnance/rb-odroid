/*
 * test_browse.c -- BROWSE's third column: the row rewrite and the value's
 * transport (browserows.c), its setting (browsecol.c), and rbp_hook.c's literal
 * relocation. The PRODUCTION files are linked; rbp itself is a synthetic row
 * context and message laid out as browserows.c reads them.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "browserows.h"
#include "rbp_hook.h"

static int checks, failures;
#define CHECK(cond, ...) do { checks++; if (!(cond)) { failures++;           \
        printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__);      \
        printf("\n"); } } while (0)

unsigned int browsecol_page_column(const uint8_t *page, unsigned int size);
const char *browsecol_category_word(unsigned category);
int is_rbp_process(void) { return 0; }
void klog(const char *fmt, ...) { (void)fmt; }

/* rbp's numbering: 1A 1B 2A ... -> 1 2 3 ... */
#define KEY(n, b) (2u * (n) - 1u + (b))

static uint16_t u16(const uint8_t *p) { uint16_t v; memcpy(&v, p, 2); return v; }
static void put16(uint8_t *p, unsigned v) { uint16_t h = (uint16_t)v; memcpy(p, &h, 2); }
static void put32(uint8_t *p, unsigned v) { memcpy(p, &v, 4); }
static int same(const uint16_t *a, const char *b)
{
    while (*b)
        if (*a++ != (uint16_t)*b++)
            return 0;
    return *a == 0;
}

static void test_relocate(void)
{
    static uint32_t mem[16];
    uint32_t lit = 0;
    for (unsigned i = 0; i < 16; i++)
        mem[i] = 0x1000u + i;
    /* ldr r2,[pc,#16] at mem[0]: pc reads mem+8, +16 = mem[6] */
    CHECK(rbp_hook_relocate(0xe59f2010u, (unsigned long)&mem[0], &lit) == 0xe59f2008u &&
          lit == 0x1006u, "ldr r2,[pc,#16] -> ldr r2,[pc,#8], literal %x", (unsigned)lit);
    /* ldr r5,[pc,#-4] at mem[4]: mem+24-4 = mem[5]; the condition is kept */
    lit = 0;
    CHECK(rbp_hook_relocate(0x151f5004u, (unsigned long)&mem[4], &lit) == 0x159f5008u &&
          lit == 0x1005u, "ldrne r5,[pc,#-4] -> ldrne r5,[pc,#8], literal %x", (unsigned)lit);
    lit = 7;
    CHECK(rbp_hook_relocate(0xe92d4ff0u, (unsigned long)&mem[0], &lit) == 0xe92d4ff0u && lit == 7,
          "push left as it is");
    CHECK(rbp_hook_relocate(0xe59ff004u, (unsigned long)&mem[0], &lit) == 0xe59ff004u,
          "ldr pc,[pc,#4] left as it is (a jump)");
    CHECK(rbp_hook_relocate(0xe5902004u, (unsigned long)&mem[0], &lit) == 0xe5902004u,
          "ldr r2,[r0,#4] left as it is");
    /* rbp's own: ui_ListBrowse_SetTextStartScrollTimer's second instruction */
    CHECK((rbp_hook_relocate(0xe59f22b0u, (unsigned long)&mem[0] - 0x2b0u + 8u, &lit) & 0xfffff000u)
          == 0xe59f2000u, "0x2995cc's ldr r2 relocates");
}

/* ---- rows ---- */
#define ROWS 0x17eacu
static uint8_t context[0x20000];
static uint8_t message[8192];
static uint8_t db[32], connection[0xc00];
static unsigned track_keys[8], native_kind = 13;     /* rbp sorted by BPM */

static unsigned stub_track_key(void *d, unsigned id) { (void)d; return id < 8 ? track_keys[id] : 0; }

/* a heading and three tracks */
static void build(const unsigned ids[3])
{
    static const char *texts[4] = { "TITLE", "One", "Two", "Three" };
    memset(context, 0, sizeof(context));
    memset(message, 0, sizeof(message));
    put32(context + 4, 0);                          /* list index 0 */
    put32(context + ROWS - 4, 3);
    for (unsigned i = 0; i < 3; i++) {
        put32(context + ROWS + i * 532, ids[i]);
        put16(context + ROWS + i * 532 + 4, 4);     /* a track row */
    }
    put32(db, (unsigned)(unsigned long)connection);
    put32(context + 0x1bceu * 4u + 4u, (unsigned)(unsigned long)db);
    unsigned at = 20;
    for (unsigned i = 0; i < 4; i++) {
        uint8_t *e = message + 0x70 + at;
        unsigned n = (unsigned)strlen(texts[i]);
        put16(e, i ? native_kind : 0x10);
        put16(e + 20, n);
        for (unsigned k = 0; k < n; k++)
            put16(e + 22 + k * 2, (unsigned char)texts[i][k]);
        at += 22 + n * 2;
    }
    put32(message + 0x1c, at);
    put16(message + 0x80, 4);
}

static const uint8_t *row_entry(unsigned row)
{
    unsigned at = 20;
    for (unsigned i = 0; i < row; i++)
        at += 22 + u16(message + 0x70 + at + 20) * 2;
    return message + 0x70 + at;
}

static int stub_values(void *d, unsigned id, unsigned key, unsigned field, uint16_t *out)
{
    static const char bpm[] = "170.0";
    (void)d; (void)id; (void)key;
    unsigned n = 0;
    if (field == BROWSE_FIELD_BPM)
        for (; bpm[n]; n++)
            out[n] = (uint16_t)bpm[n];
    out[n] = 0;
    return 1;
}

/* Row `row`'s value as "chosen|bpm|key" in ASCII (PART as '|', LIT as '*', a dash as
 * '~'), or "" when it carries none. */
static const char *row_value(unsigned row)
{
    static char out[160];
    const uint8_t *e = row_entry(row);
    const uint16_t *s = (const void *)(e + 22);
    unsigned split, extra, n = 0;
    out[0] = 0;
    if (!browserows_split(s, u16(e + 20), &split, &extra))
        return out;
    for (unsigned i = 0; i < extra && n < sizeof out - 1; i++) {
        uint16_t c = s[split + 1 + i];
        out[n++] = c == 0xfdd2 ? '|' : c == 0xfdd3 ? '*' : c == 0x2014 ? '~' : (char)c;
    }
    out[n] = 0;
    return out;
}

static void test_rows(void)
{
    static const unsigned ids[3] = { 1, 2, 3 };
    static const uint16_t caption[] = { 'K', 'E', 'Y', 0 };
    static const struct browse_column column = { BROWSE_FIELD_KEY, caption };
    static const struct browse_column none = { 0, NULL };
    unsigned split, extra;
    browserows_track_key = stub_track_key;
    browserows_values = stub_values;
    track_keys[1] = KEY(10, 0);
    track_keys[2] = 0;              /* no key known */
    track_keys[3] = KEY(12, 1);

    build(ids);
    browserows_process(context, message, &column);
    const uint8_t *e = row_entry(1);
    const uint16_t *s = (const void *)(e + 22);
    CHECK(browserows_split(s, u16(e + 20), &split, &extra) && split == 3 && s[0] == 'O',
          "row 1: its own text first");
    CHECK(!strcmp(row_value(1), "10A|170.0|10A"), "row 1: the chosen KEY, BPM, KEY: %s",
          row_value(1));
    CHECK(u16(e) == 13, "row category kept");
    CHECK(!strcmp(row_value(2), "~|170.0|"), "no key: a dash for the chosen KEY, no KEY: %s",
          row_value(2));
    CHECK(!strcmp(row_value(3), "12B|170.0|12B"), "row 3: 12B: %s", row_value(3));
    e = row_entry(0);
    CHECK(u16(e + 20) == 5 && u16(e) == 0x10, "the heading row as rbp made it");
    unsigned bytes;
    memcpy(&bytes, message + 0x1c, 4);
    CHECK(bytes == (unsigned)(row_entry(3) - message - 0x70) + 22u + u16(row_entry(3) + 20) * 2u,
          "the message's size follows its rows");

    native_kind = 52;               /* rbp's own second column is KEY (a green key row) */
    build(ids);
    browserows_process(context, message, &column);
    CHECK(!strcmp(row_value(1), "10A|170.0|*10A"),
          "rbp showing KEY, every row green: BPM and KEY carried, KEY lit: %s", row_value(1));
    native_kind = 13;

    build(ids);
    browserows_process(context, message, &none);
    CHECK(!strcmp(row_value(1), "|170.0|10A") && !strcmp(row_value(3), "|170.0|12B"),
          "no column chosen: BPM and KEY all the same: %s", row_value(1));

    native_kind = 7;                /* rbp's own column is ARTIST... */
    build(ids);
    put16((uint8_t *)row_entry(1), 52);     /* ...and the traffic light lit row 1 */
    browserows_process(context, message, &none);
    CHECK(!strcmp(row_value(1), "|170.0|*10A") && !strcmp(row_value(2), "|170.0|"),
          "a lit row's KEY is marked, the others not: %s / %s", row_value(1), row_value(2));
    native_kind = 13;

    build(ids);
    put16(context + ROWS + 1 * 532 + 4, 0x99);     /* a folder among them */
    browserows_process(context, message, &column);
    e = row_entry(1);
    CHECK(!browserows_split((const void *)(e + 22), u16(e + 20), &split, &extra) &&
          u16(e + 20) == 3, "a folder list is left as rbp built it");
}

static void test_transport(void)
{
    uint16_t out[8];
    browserows_key_text(KEY(8, 0), out);
    CHECK(same(out, "8A"), "8A");
    browserows_key_text(24, out);
    CHECK(same(out, "12B"), "12B");
    browserows_key_text(0, out);
    CHECK(out[0] == 0, "no key, no text");

    uint8_t entry[22 + 300 * 2] = { 0 }, dest[600];
    uint16_t value[] = { 'A', 'r', 't', 0 };
    put16(entry + 20, 250);
    for (unsigned i = 0; i < 250; i++)
        put16(entry + 22 + i * 2, 'x');
    unsigned size = browserows_append(dest, entry, value);
    unsigned n = u16(dest + 20), split, extra;
    const uint16_t *s = (const void *)(dest + 22);
    CHECK(size == 22 + n * 2 && n <= 255, "a long row stays within 255: %u", n);
    CHECK(browserows_split(s, n, &split, &extra) && extra == 3 &&
          s[split - 1] == 0x2026 && s[split + 1] == 'A', "cut with an ellipsis, value whole");
    CHECK(!browserows_split(s, n - 1, &split, &extra), "an ordinary text is not ours");
}

static void test_config(void)
{
    const uint16_t *caption = NULL;
    CHECK(browsecol_field("artist", &caption) == BROWSE_FIELD_ARTIST && caption && caption[0] == 'A',
          "artist");
    CHECK(browsecol_field("BPM", &caption) == BROWSE_FIELD_BPM && browsecol_field("15", &caption) ==
          BROWSE_FIELD_KEY && browsecol_field("length", &caption) == BROWSE_FIELD_TIME,
          "bpm, 15, length");
    CHECK(browsecol_field("album", &caption) == BROWSE_FIELD_ALBUM, "album");
    CHECK(browsecol_field("genre", &caption) == 0 && browsecol_field("", &caption) == 0 &&
          browsecol_field(NULL, &caption) == 0, "anything else: off");
}

/* A table-18 page as rekordbox 7.2.8 wrote it on the operator's stick with
 * MY SETTINGS > Column = ALBUM (2026-10-28), cut to its rows. */
static void test_stick_column(void)
{
    static uint8_t page[4096];
    static const uint8_t rows[][8] = {
        { 0x06, 0, 0x05, 0, 0, 6, 0, 0 }, { 0x1a, 0, 0x01, 0, 0, 3, 0, 0 },
        { 0x19, 0, 0x00, 0, 0, 2, 0, 0 }, { 0x05, 0, 0x04, 0, 0, 1, 0, 0 },
        { 0x02, 0, 0x02, 0, 0, 4, 0, 0 }, { 0x03, 0, 0x03, 0, 2, 5, 0, 0 },
    };
    unsigned n = sizeof rows / sizeof *rows, present = 0;
    page[0x18] = (uint8_t)n;
    for (unsigned r = 0; r < n; r++) {
        memcpy(page + 0x28 + r * 8, rows[r], 8);
        put16(page + 4096 - 6 - r * 2, r * 8);
        present |= 1u << r;
    }
    put16(page + 4096 - 4, present);
    CHECK(browsecol_page_column(page, 4096) == 3, "ALBUM (rbp's category 3) is the flagged row");
    CHECK(!strcmp(browsecol_category_word(3), "album") && !strcmp(browsecol_category_word(2), "artist") &&
          !strcmp(browsecol_category_word(12), "key") && browsecol_category_word(5) == NULL,
          "categories as words; RATING is not one shown");
    put16(page + 4096 - 4, present & ~(1u << 5));
    CHECK(browsecol_page_column(page, 4096) == 0, "a deleted row does not count");
    page[0x28 + 4 * 8 + 4] = 2;
    CHECK(browsecol_page_column(page, 4096) == 2, "flag on ARTIST: 2");
    page[0x1b] = 0x40;
    CHECK(browsecol_page_column(page, 4096) == 0, "an index page is not read");
}

static unsigned found_entry[8], found_track[8], found_n;
static void found(unsigned entry, unsigned track, void *arg)
{
    (void)arg;
    if (found_n < 8) {
        found_entry[found_n] = entry;
        found_track[found_n] = track;
    }
    found_n++;
}

static void test_playlist_entries(void)
{
    static uint8_t page[4096];
    /* entry, track, playlist -- as export.pdb's table 8 holds them */
    static const unsigned rows[][3] = { { 1, 135, 1 }, { 2, 136, 1 }, { 1, 40, 2 }, { 5, 139, 1 } };
    unsigned n = sizeof rows / sizeof *rows, present = 0;
    page[0x18] = (uint8_t)n;
    for (unsigned r = 0; r < n; r++) {
        for (unsigned k = 0; k < 3; k++)
            put32(page + 0x28 + r * 12 + k * 4, rows[r][k]);
        put16(page + 4096 - 6 - r * 2, r * 12);
        present |= 1u << r;
    }
    put16(page + 4096 - 4, present & ~(1u << 1));
    browsecol_page_entries(page, 4096, 1, found, NULL);
    CHECK(found_n == 2 && found_entry[0] == 1 && found_track[0] == 135 && found_entry[1] == 5 &&
          found_track[1] == 139, "playlist 1's present entries, with their tracks");
    found_n = 0;
    page[0x1b] = 0x40;
    browsecol_page_entries(page, 4096, 1, found, NULL);
    CHECK(found_n == 0, "an index page holds no entries");
}

/* rbp's browse context around its popup menus, as browsemenu.c reads it */
#define M 0x43378u
#define ENTRY(i) (M + 0x22cu + (i) * 0x210u)
static uint8_t menu_context[ENTRY(20)];
static uint8_t menu_owner[0x20];

static void put_text(uint8_t *at, const char *s)
{
    while (*s) {
        put16(at, (uint8_t)*s++);
        at += 2;
    }
    put16(at, 0);
}

/* LIST MENU as SendBrowseMenu builds it in a playlist, depth 0 */
static void list_menu(void)
{
    memset(menu_context, 0, sizeof menu_context);
    put32(menu_context, 2);
    put32(menu_owner + 0x10, 0x51);
    put32(menu_context + 0x6f54, (unsigned)(unsigned long)menu_owner);
    put32(menu_context + M, 0x400);
    put32(menu_context + M + 0xc, 2);
    put32(menu_context + ENTRY(0), 0x51);
    put32(menu_context + ENTRY(0) + 4, 1);
    put_text(menu_context + ENTRY(0) + 0xc, "SORT");
    put32(menu_context + ENTRY(1), 0x51);
    put32(menu_context + ENTRY(1) + 4, 0x401);
    put_text(menu_context + ENTRY(1) + 0xc, "PLAYSTATE");
}

static unsigned w32(const uint8_t *p) { unsigned v; memcpy(&v, p, 4); return v; }

static void test_menu(void)
{
    list_menu();
    CHECK(browsemenu_add_column(menu_context) && w32(menu_context + M + 0xc) == 3,
          "LIST MENU gets a third entry");
    CHECK(same((const void *)(menu_context + ENTRY(2) + 0xc), "COLUMN") &&
          w32(menu_context + ENTRY(2)) == 0x51 && w32(menu_context + ENTRY(2) + 4) == 1,
          "COLUMN, as SORT is: its id, state 1");
    CHECK(same((const void *)(menu_context + ENTRY(0) + 0xc), "SORT") &&
          same((const void *)(menu_context + ENTRY(1) + 0xc), "PLAYSTATE") &&
          w32(menu_context + ENTRY(1) + 4) == 0x401, "SORT and PLAYSTATE untouched");
    CHECK(!browsemenu_add_column(menu_context) && w32(menu_context + M + 0xc) == 3,
          "sent again (a scroll): not added twice");

    list_menu();
    put32(menu_context + M, 0xa00);
    CHECK(!browsemenu_add_column(menu_context), "another menu: left alone");
    list_menu();
    put32(menu_context + 0x6fc8, 1);
    CHECK(!browsemenu_add_column(menu_context), "a menu one level down: left alone");
    list_menu();
    put_text(menu_context + ENTRY(0) + 0xc, "SORTED");
    CHECK(!browsemenu_add_column(menu_context), "first entry not SORT: left alone");
    list_menu();
    put32(menu_context, 1);
    CHECK(!browsemenu_add_column(menu_context), "not BROWSE: left alone");

    /* COLUMN picked: rbp steps to depth 1 with the item in +0x6fd0 / +0x6fcc */
    list_menu();
    browsemenu_add_column(menu_context);
    put32(menu_context + 0x6fc8, 1);
    put32(menu_context + 0x6fd0, 2);
    menu_context[0x6fcc] = 2;
    browsemenu_fill(menu_context, 0, "album");
    static const char *const shown[] = { "OFF", "ARTIST", "ALBUM", "BPM", "KEY", "TIME" };
    unsigned ok = w32(menu_context + M + 0xc) == 6 && w32(menu_context + M + 4) == 0x500 &&
                  w32(menu_context + M) == 0x400;
    for (unsigned i = 0; i < 6; i++)
        ok = ok && same((const void *)(menu_context + ENTRY(i) + 0xc), shown[i]) &&
             w32(menu_context + ENTRY(i)) == 0x51 &&
             w32(menu_context + ENTRY(i) + 4) == (i == 2 ? 0x463u : 0x63u);
    CHECK(ok, "OFF..TIME, ALBUM checked, at depth 1; LIST MENU's type kept below it");
    CHECK(w32(menu_context + ENTRY(6)) == 0 && u16(menu_context + ENTRY(6) + 0xc) == 0,
          "nothing after TIME");
    browsemenu_fill(menu_context, 0, "off");
    CHECK(w32(menu_context + ENTRY(0) + 4) == 0x463 && w32(menu_context + ENTRY(2) + 4) == 0x63,
          "off: OFF checked");
    browsemenu_fill(menu_context, 0, "length");
    CHECK(w32(menu_context + ENTRY(5) + 4) == 0x463, "length is TIME");
    CHECK(!strcmp(browsemenu_word(0), "off") && !strcmp(browsemenu_word(2), "album") &&
          !strcmp(browsemenu_word(5), "time") && browsemenu_word(6) == NULL,
          "items as the words the column file takes");
    for (unsigned i = 0; i < 6; i++) {
        const uint16_t *caption = NULL;
        unsigned field = browsecol_field(browsemenu_word(i), &caption);
        CHECK(i ? field != 0 : field == 0, "item %u's word is one the column reads", i);
    }
}

static void test_sort_tap(void)
{
    CHECK(browserows_sort_for_tap(12, 4) == 4, "a tap on BPM in a KEY list: BPM, rbp's way");
    CHECK(browserows_sort_for_tap(4, 4) == (0x4000u | 0x8000u | 4u),
          "BPM again: BPM turned round, down");
    CHECK(browserows_sort_for_tap(0x4000u | 0x8000u | 4u, 4) == (0x4000u | 4u),
          "and again: up");
    CHECK(browserows_sort_for_tap(17, 17) == (0x4000u | 17u),
          "DATE ADDED runs down in rbp: turned round is up");
    CHECK(browserows_sort_for_tap(21, 4) == 4, "BPM (UNSORTED) is not BPM's sort");
    CHECK(browserows_sort_for_tap(4, 0) == 0, "the #: the list's own order");
    CHECK(browserows_blue_pixel(0xf81f) == 0xf81f, "the transparent key stays");
    CHECK(browserows_blue_pixel(0x3186) == 0x3186, "the heading's grey ground stays");
    CHECK(browserows_blue_pixel(0xbdf7) == 0x03fc, "its text becomes rbp's blue (0,125,231)");
}

int main(void)
{
    test_relocate();
    test_rows();
    test_stick_column();
    test_transport();
    test_config();
    test_menu();
    test_sort_tap();
    test_playlist_entries();
    printf("%s: %d checks, %d failures\n", failures ? "FAIL" : "ok", checks, failures);
    return failures ? 1 : 0;
}
