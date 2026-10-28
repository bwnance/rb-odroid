/*
 * test_browse.c -- the BROWSE mods: Key Match's rules and colours (keymatch.c,
 * rx3_harmony.h), the recoloured icons (rbp_images.c), the row rewrite and the
 * third column's transport (browserows.c, browsecol.c), and rbp_hook.c's literal
 * relocation. The PRODUCTION files are linked; rbp itself is a synthetic row
 * context and message laid out as browserows.c reads them.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "browserows.h"
#include "rbp_hook.h"
#include "rbp_images.h"
#include "rx3_harmony.h"

static int checks, failures;
#define CHECK(cond, ...) do { checks++; if (!(cond)) { failures++;           \
        printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__);      \
        printf("\n"); } } while (0)

unsigned int keymatch_category(unsigned int reference, unsigned int key, unsigned int native,
                               unsigned int rule_mask);
unsigned int browsecol_field(const char *value, const uint16_t **caption);
int is_rbp_process(void) { return 0; }
void klog(const char *fmt, ...) { (void)fmt; }
int keyshift_get(int deck) { (void)deck; return 0; }

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

static void test_rules(void)
{
    unsigned all = rx3_match_rules("14");
    CHECK(all == 14 && rx3_match_rules("") == 2 && rx3_match_rules(NULL) == 2 &&
          rx3_match_rules("15") == 14 && rx3_match_rules("16") == 2 && rx3_match_rules("4") == 4,
          "RB_KEY_MATCH_RULES parsed as the toolkit's mask");
    unsigned ref = KEY(8, 0);       /* 8A */
    CHECK(keymatch_category(ref, KEY(10, 0), 51, all) == 120, "8A -> 10A yellow (+2)");
    CHECK(keymatch_category(ref, KEY(3, 0), 51, all) == 121, "8A -> 3A orange (+7)");
    CHECK(keymatch_category(ref, KEY(12, 0), 51, all) == 122, "8A -> 12A red (+4)");
    CHECK(keymatch_category(KEY(8, 1), KEY(10, 1), 51, all) == 120, "8B -> 10B yellow");
    CHECK(keymatch_category(ref, KEY(10, 1), 51, all) == 51, "8A -> 10B: A to B is no boost");
    CHECK(keymatch_category(ref, KEY(9, 0), 51, all) == 51, "8A -> 9A is rbp's to mark");
    CHECK(keymatch_category(ref, KEY(6, 0), 51, all) == 51, "8A -> 6A (-2) not a boost");
    CHECK(keymatch_category(ref, KEY(10, 0), 52, all) == 52, "rbp's green kept");
    CHECK(keymatch_category(ref, KEY(3, 0), 51, 2) == 51 && keymatch_category(ref, KEY(10, 0), 51, 4) == 51,
          "only the chosen rules colour");
    CHECK(keymatch_category(0, KEY(10, 0), 51, all) == 51 && keymatch_category(ref, 0, 51, all) == 51,
          "no MASTER key, or no track key: rbp's");
    CHECK(keymatch_category(KEY(12, 0), KEY(2, 0), 51, all) == 120, "12A -> 2A wraps (+2)");
}

/* ---- images ---- */
static uint8_t table[0x2000];

static void *fake_lookup(unsigned int id)
{
    return id < 8 ? table + id * 44u : NULL;
}

static void test_images(void)
{
    CHECK(rbp_images_shade(0x07e0, 0x07e0, 0xf800) == 0xf800, "the colour itself recoloured");
    CHECK(rbp_images_shade(0x001f, 0x07e0, 0xf800) == 0x001f, "off the ray: as it is");
    uint16_t half = rbp_images_shade(0x03e0, 0x07e0, 0xffe0);   /* green 31 of 63 */
    CHECK((half >> 11) == 15 && ((half >> 5) & 63) == 31 && (half & 31) == 0,
          "an antialiased shade keeps its share: %04x", half);
    CHECK(rbp_images_shade(0, 0x07e0, 0xf800) == 0, "black stays black");

    /* image 5: 2x2 RGB565, keyed (format 2), unpaletted; pixels at 0x1000 */
    uint8_t *rec = table + 5 * 44;
    uint16_t w = 2, h = 2, px[4] = { 0x07e0, 0x03e0, 0x001f, 0 };
    uint32_t off = 0x1000;
    memcpy(rec + 4, &w, 2);
    memcpy(rec + 6, &h, 2);
    rec[0x18] = 2;
    rec[0x19] = 0;
    memcpy(rec + 0x20, &off, 4);
    memcpy(table + off, px, sizeof(px));
    /* image 6: paletted -- not for recolouring */
    memcpy(table + 6 * 44, rec, 44);
    table[6 * 44 + 0x19] = 1;

    unsigned id = rbp_images_recolour(5, 0x07e0, 0xf800);
    unsigned bad = rbp_images_recolour(6, 0x07e0, 0xf800);
    CHECK(id == RBP_IMAGES_FIRST && bad == RBP_IMAGES_FIRST + 1, "private IDs from 0x1700: %x", id);
    CHECK(rbp_images_recolour(RBP_IMAGES_FIRST, 1, 2) == 0 && rbp_images_recolour(5, 3, 3) == 0,
          "no recolour of a private ID, nor to the same colour");
    const uint8_t *got = rbp_images_resolve(id, fake_lookup, table);
    CHECK(got != NULL, "a recoloured record");
    if (got) {
        uint32_t abs, rel;
        memcpy(&abs, got + 0x0c, 4);
        memcpy(&rel, got + 0x20, 4);
        const uint16_t *p = (const uint16_t *)(unsigned long)abs;
        CHECK(u16(got + 4) == 2 && u16(got + 6) == 2 && got[0x18] == 2, "size and format kept");
        CHECK((unsigned long)table + rel == abs, "offset and pointer agree");
        CHECK(p[0] == 0xf800 && p[1] == 0x7800 && p[2] == 0x001f && p[3] == 0,
              "pixels recoloured: %04x %04x %04x %04x", p[0], p[1], p[2], p[3]);
        CHECK(px[0] == 0x07e0 && u16(table + off) == 0x07e0, "rbp's own pixels untouched");
    }
    CHECK(rbp_images_resolve(bad, fake_lookup, table) == NULL, "a paletted source refused");
    CHECK(rbp_images_resolve(5, fake_lookup, table) == NULL &&
          rbp_images_resolve(RBP_IMAGES_FIRST + 9, fake_lookup, table) == NULL,
          "stock and unregistered IDs are not ours");
}

/* ---- rows ---- */
#define ROWS 0x17eacu
static uint8_t context[0x20000];
static uint8_t message[8192];
static uint8_t db[32], connection[0xc00];
static unsigned masters[2], track_keys[8];
static int shift;

static unsigned stub_master(unsigned deck) { return masters[deck & 1]; }
static unsigned stub_track_key(void *d, unsigned id) { (void)d; return id < 8 ? track_keys[id] : 0; }
static int stub_shift(int deck) { (void)deck; return shift; }
static unsigned test_image(unsigned category) { return 100u + category; }
static unsigned test_category(unsigned reference, unsigned key, unsigned native)
{
    return keymatch_category(reference, key, native, 14);
}
static const struct browse_marker marker = { test_category, test_image };

/* rbp's MASTER at 8A, its green set {8A 7A 9A 8B}; a header and three tracks */
static void build(const unsigned categories[4], const unsigned ids[3])
{
    static const char *texts[4] = { "TITLE", "One", "Two", "Three" };
    memset(context, 0, sizeof(context));
    memset(message, 0, sizeof(message));
    put32(context + 4, 0);                          /* list index 0 */
    put32(context + 0x10af8, 1);                    /* a reference is shown */
    put32(context + 0x10b08, KEY(8, 0));
    put32(context + 0x10b0c, KEY(7, 0));
    put32(context + 0x10b10, KEY(9, 0));
    put32(context + 0x10b14, KEY(8, 1));
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
        put16(e, categories[i]);
        put16(e + 20, n);
        for (unsigned k = 0; k < n; k++)
            put16(e + 22 + k * 2, (unsigned char)texts[i][k]);
        at += 22 + n * 2;
    }
    put32(message + 0x1c, at);
    put16(message + 0x80, 4);
}

static unsigned row_category(unsigned row)
{
    unsigned at = 20;
    for (unsigned i = 0; i < row; i++)
        at += 22 + u16(message + 0x70 + at + 20) * 2;
    return u16(message + 0x70 + at);
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
    static const unsigned plain[4] = { 0x10, 51, 51, 51 }, ids[3] = { 1, 2, 3 };
    browserows_master_on = stub_master;
    browserows_track_key = stub_track_key;
    track_keys[1] = KEY(10, 0);     /* yellow */
    track_keys[2] = KEY(9, 0);      /* rbp's green */
    track_keys[3] = KEY(10, 1);     /* nothing */
    masters[0] = 1;

    build(plain, ids);
    browserows_process(context, message, NULL, &marker);
    CHECK(row_category(0) == 0x10, "the heading row left alone");
    CHECK(row_category(1) == 120 && row_category(2) == 52 && row_category(3) == 51,
          "MASTER 8A: 10A yellow, 9A green, 10B none -- %u %u %u",
          row_category(1), row_category(2), row_category(3));
    CHECK(u16(row_entry(1) + 20) == 3 && u16(row_entry(1) + 22) == 'O', "row text untouched");

    masters[0] = 1;
    masters[1] = 1;                 /* both or neither: no MASTER */
    build(plain, ids);
    browserows_process(context, message, NULL, &marker);
    CHECK(row_category(1) == 51 && row_category(2) == 51, "no single MASTER: rbp's rows");
    masters[1] = 0;

    build(plain, ids);
    put16(context + ROWS + 1 * 532 + 4, 0x99);     /* a folder among them */
    browserows_process(context, message, NULL, &marker);
    CHECK(row_category(1) == 51, "a folder list is not rewritten");

    /* the MASTER shifted up 2 semitones: 8A heard as 10A */
    static const unsigned with_green[4] = { 0x10, 51, 51, 52 }, ids2[3] = { 4, 5, 6 };
    track_keys[4] = KEY(12, 0);     /* 10A +2 -> yellow */
    track_keys[5] = KEY(11, 0);     /* a neighbour of 10A -> green */
    track_keys[6] = KEY(8, 0);      /* rbp's green for 8A, no longer */
    browserows_shift = stub_shift;
    shift = 2;
    build(with_green, ids2);
    browserows_process(context, message, NULL, &marker);
    CHECK(row_category(1) == 120 && row_category(2) == 52 && row_category(3) == 51,
          "MASTER 8A shifted to 10A: 12A yellow, 11A green, 8A cleared -- %u %u %u",
          row_category(1), row_category(2), row_category(3));
    shift = 0;
    browserows_shift = NULL;

    /* the third column: KEY, with the marker's badge */
    static const uint16_t caption[] = { 'K', 'E', 'Y', 0 };
    static const struct browse_column column = { BROWSE_FIELD_KEY, caption };
    build(plain, ids);
    browserows_process(context, message, &column, &marker);
    unsigned split, badge, extra;
    const uint8_t *e = row_entry(1);
    const uint16_t *s = (const void *)(e + 22);
    CHECK(browserows_split(s, u16(e + 20), &split, &badge, &extra) && split == 3 &&
          badge == 100 + 120 && extra == 3 && s[split + 2] == '1' && s[split + 3] == '0' &&
          s[split + 4] == 'A', "row 1 carries 10A and the yellow badge");
    CHECK(row_category(1) == 120, "and its icon category");
    e = row_entry(0);
    s = (const void *)(e + 22);
    CHECK(browserows_split(s, u16(e + 20), &split, &badge, &extra) && extra == 3 &&
          s[split + 2] == 'K' && badge == 0, "the heading row carries the caption");
    CHECK(row_category(0) == 0x10, "heading category kept");
    unsigned bytes;
    memcpy(&bytes, message + 0x1c, 4);
    CHECK(bytes == (unsigned)(row_entry(3) - message - 0x70) + 22u + u16(row_entry(3) + 20) * 2u,
          "the message's size follows its rows");
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
    unsigned size = browserows_append(dest, entry, value, 7);
    unsigned n = u16(dest + 20), split, badge, extra;
    const uint16_t *s = (const void *)(dest + 22);
    CHECK(size == 22 + n * 2 && n <= 255, "a long row stays within 255: %u", n);
    CHECK(browserows_split(s, n, &split, &badge, &extra) && badge == 7 && extra == 3 &&
          s[split - 1] == 0x2026 && s[split + 2] == 'A', "cut with an ellipsis, value whole");
    CHECK(!browserows_split(s, n - 1, &split, &badge, &extra), "an ordinary text is not ours");
}

static void test_config(void)
{
    const uint16_t *caption = NULL;
    CHECK(browsecol_field("artist", &caption) == BROWSE_FIELD_ARTIST && caption && caption[0] == 'A',
          "artist");
    CHECK(browsecol_field("BPM", &caption) == BROWSE_FIELD_BPM && browsecol_field("15", &caption) ==
          BROWSE_FIELD_KEY && browsecol_field("length", &caption) == BROWSE_FIELD_TIME,
          "bpm, 15, length");
    CHECK(browsecol_field("album", &caption) == 0 && browsecol_field("", &caption) == 0 &&
          browsecol_field(NULL, &caption) == 0, "anything else: off");
}

int main(void)
{
    test_relocate();
    test_rules();
    test_images();
    test_rows();
    test_transport();
    test_config();
    printf("%s: %d checks, %d failures\n", failures ? "FAIL" : "ok", checks, failures);
    return failures ? 1 : 0;
}
