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

unsigned int browsecol_field(const char *value, const uint16_t **caption);
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

static void test_rows(void)
{
    static const unsigned ids[3] = { 1, 2, 3 };
    static const uint16_t caption[] = { 'K', 'E', 'Y', 0 };
    static const struct browse_column column = { BROWSE_FIELD_KEY, caption };
    unsigned split, extra;
    browserows_track_key = stub_track_key;
    track_keys[1] = KEY(10, 0);
    track_keys[2] = 0;              /* no key known */
    track_keys[3] = KEY(12, 1);

    build(ids);
    browserows_process(context, message, &column);
    const uint8_t *e = row_entry(1);
    const uint16_t *s = (const void *)(e + 22);
    CHECK(browserows_split(s, u16(e + 20), &split, &extra) && split == 3 && extra == 3 &&
          s[0] == 'O' && s[split + 1] == '1' && s[split + 2] == '0' && s[split + 3] == 'A',
          "row 1: its title, then 10A");
    CHECK(u16(e) == 13, "row category kept");
    e = row_entry(2);
    s = (const void *)(e + 22);
    CHECK(browserows_split(s, u16(e + 20), &split, &extra) && extra == 1 && s[split + 1] == 0x2014,
          "no key: a dash");
    e = row_entry(3);
    s = (const void *)(e + 22);
    CHECK(browserows_split(s, u16(e + 20), &split, &extra) && s[split + 1] == '1' &&
          s[split + 2] == '2' && s[split + 3] == 'B', "row 3: 12B");
    e = row_entry(0);
    CHECK(u16(e + 20) == 5 && u16(e) == 0x10, "the heading row as rbp made it");
    unsigned bytes;
    memcpy(&bytes, message + 0x1c, 4);
    CHECK(bytes == (unsigned)(row_entry(3) - message - 0x70) + 22u + u16(row_entry(3) + 20) * 2u,
          "the message's size follows its rows");

    native_kind = 52;               /* rbp's own second column is KEY (a green key row) */
    build(ids);
    browserows_process(context, message, &column);
    e = row_entry(1);
    CHECK(!browserows_split((const void *)(e + 22), u16(e + 20), &split, &extra) &&
          u16(e + 20) == 3, "KEY already shown by rbp: not shown twice");
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

int main(void)
{
    test_relocate();
    test_rows();
    test_transport();
    test_config();
    printf("%s: %d checks, %d failures\n", failures ? "FAIL" : "ok", checks, failures);
    return failures ? 1 : 0;
}
