/* SPDX-License-Identifier: MPL-2.0
 *
 * browserows.c -- rbp's BROWSE track rows, rewritten as rbp builds them
 * (browserows.h).
 *
 * Adapted from rx3-toolkit (github.com/Tratosca/rx3-toolkit at cc17a1f,
 * MPL-2.0): mod/modules/core/services/rx3_browse.c, the row hook and what it
 * reads (the MASTER's key and its green set, each track's key and one metadata
 * field, the database server's per-page snapshot), the third column's drawing and
 * the selected row's scrolling; and the PAL text/image hooks of
 * mod/modules/core/rx3_core_hook.c that call into it. Left out: the toolkit's
 * tap-a-heading sort (its four DB hooks and touch), unregistering (here a hook,
 * once in, stays in), its metrics, and its Key Shift publication -- keyshift.c's
 * shift comes in through browserows_shift instead. This file stays under the MPL.
 *
 * Every address below was checked against this tree's rbp 1.20 (sha1
 * 3513d34b...) on 2026-10-27: each hook's first eight bytes and each called
 * function's first two words, as the toolkit's guards give them, match; the
 * toolkit wrote them for 1.19 and lists 1.20, whose code these places share.
 * The row message and context layouts are the toolkit's reading, not re-derived.
 *
 * Threads: GetRightBrowseOne2One and the DB snapshot hooks run on rbp's database
 * tasks; drawing and scrolling on its UI/render thread.
 */
#include "browserows.h"

#include <string.h>
#include <sys/time.h>

#include "rbp_hook.h"

#define ROW_HOOK 0x001c8048ul       /* GetRightBrowseOne2One(context, message, request) */
#define SEP 0xfdd0u                 /* noncharacters: the column's value rides in the row */
#define END 0xfdd1u
#define MAX_MESSAGE 8192u
/* Lib_Obj_CTRL_LIST_BROWSE property index 55, not the generated name suffix 220. */
#define COLUMN_HEADING_ID 55

static const struct browse_column *column;
static const struct browse_marker *marker;
int (*browserows_shift)(int deck);

static unsigned word(const void *p) { unsigned v; memcpy(&v, p, 4); return v; }
static unsigned half(const void *p) { uint16_t v; memcpy(&v, p, 2); return v; }
static void put_half(void *p, unsigned v) { uint16_t h = (uint16_t)v; memcpy(p, &h, 2); }
static void put_word(void *p, unsigned value) { memcpy(p, &value, 4); }

/* ---- rbp 1.20 ----------------------------------------------------------------- */
static int (*original_rows)(void *, void *, void *);
static void *(*object_by_id)(void *, int) = (void *)0x0018dae8u;    /* ui_com_draw_GetObjectByID */
static void (*refresh_object)(void *, int) = (void *)0x0018e1a0u;
unsigned int (*browserows_track_key)(void *, unsigned int) = (void *)0x00265f48u;
static int (*metadata)(void *, unsigned, unsigned) = (void *)0x002637f4u;
static int (*records)(void *, unsigned, unsigned *, unsigned, unsigned, unsigned, unsigned) =
    (void *)0x00262884u;
static int (*next_record)(void *, uint32_t *, int, void *) = (void *)0x00262a28u;
static void (*skip)(void *) = (void *)0x00262da0u;
static uint16_t *(*format)(uint32_t, uint32_t, unsigned, unsigned) = (void *)0x0026a480u;
static void (*free_item)(void *) = (void *)0x001f648cu;
static void (*free_text)(void *) = (void *)0x00175038u;
static int (*menu_visible)(void) = (void *)0x00112e10u;             /* IsMenuOn */
static const void *root_pointer = (void *)0x02684700u;
unsigned int (*browserows_master_on)(unsigned int) = (void *)0x0018579cu;

/* the functions called, each by its first two words (the toolkit's guards) */
static const struct { unsigned address, a, b; } called[] = {
    {0x3dd464, 0xe92d4ff0, 0xe24dd04c}, {0x3ddd78, 0xe92d41f0, 0xe24dd020},
    {0x3dde5c, 0xeaffe2fa, 0xe92d4038}, {0x3ddb24, 0xe92d40f0, 0xe24dd01c},
    {0x16095c, 0xe92d41f0, 0xe24dd028}, {0x19e360, 0xe92d41f0, 0xe24dd0c8},
    {0x265f48, 0xe92d40f0, 0xe2507000}, {0x2637f4, 0xe1a0c001, 0xe1a03002},
    {0x262884, 0xe92d4ff0, 0xe24dd00c}, {0x262a28, 0xe92d4ff0, 0xe24dd014},
    {0x262da0, 0xe92d4ff0, 0xe24dd00c}, {0x26a480, 0xe352000d, 0x13520011},
    {0x1f648c, 0xe3500000, 0x012fff1e}, {0x175038, 0xe92d4070, 0xe2504000},
    {0x18dae8, 0xe2503000, 0xe92d4010}, {0x18e1a0, 0xe92d4070, 0xe1a04000},
    {0x18579c, 0xe3033564, 0xe3403325}, {0x112e10, 0xe30f38b8, 0xe3403326},
    {0x18e260, 0xe92d4070, 0xe24dd008}, {0x18e2a0, 0xe92d4070, 0xe1a04001},
    {0x18e730, 0xe92d4010, 0xe1a04000}, {0x1cec2c, 0xe3500000, 0x0a00000a},
};

/* ---- the MASTER's key ------------------------------------------------------- */
static int master_deck(void)
{
    unsigned left = browserows_master_on(0) != 0, right = browserows_master_on(1) != 0;
    return left == right ? -1 : left ? 0 : 1;
}

/* One word couples rbp's green set, the key it is for and the MASTER deck:
 * bits 0..23 the set, 24..28 the key (1..24), 29..31 the deck + 1. */
static unsigned harmonic_snapshot;

static void observe_harmony(const uint8_t *context)
{
    int deck = master_deck();
    unsigned key = word(context + 0x10b08), mask = 0;
    if (deck < 0 || key < 1 || key > 24) {
        __atomic_store_n(&harmonic_snapshot, 0u, __ATOMIC_SEQ_CST);
        return;
    }
    for (unsigned i = 0; i < 4; i++) {
        unsigned compatible = word(context + 0x10b08 + i * 4);
        if (compatible >= 1 && compatible <= 24)
            mask |= 1u << (compatible - 1);
    }
    __atomic_store_n(&harmonic_snapshot, mask | (key << 24) | ((unsigned)(deck + 1) << 29),
                     __ATOMIC_SEQ_CST);
}

struct harmonic_reference { int deck, key, shift; unsigned native_keys; };

static int shifted(int key, int shift)
{
    int result = (key + shift * 14) % 24;   /* a semitone is 7 Camelot steps, A/B kept */
    return result < 0 ? result + 24 : result;
}

static struct harmonic_reference harmonic_reference(void)
{
    struct harmonic_reference result = { -1, -1, 0, 0 };
    int deck = master_deck();
    if (deck < 0)
        return result;
    unsigned packed = __atomic_load_n(&harmonic_snapshot, __ATOMIC_SEQ_CST);
    int source = (int)((packed >> 24) & 31u) - 1;
    if ((packed >> 29) != (unsigned)deck + 1 || source < 0 || source >= 24)
        return result;
    unsigned mask = packed & 0xffffffu;
    int shift = browserows_shift ? browserows_shift(deck) : 0;
    if (shift < -12 || shift > 12)
        shift = 0;
    result.deck = deck;
    result.shift = shift;
    result.key = shifted(source, shift);
    /* rbp's set is for the key as loaded: turned with it, for the key heard */
    for (int key = 0; key < 24; key++)
        if (mask & (1u << key))
            result.native_keys |= 1u << shifted(key, shift);
    return result;
}

/* ---- rows --------------------------------------------------------------------- */
static unsigned track_list;
static const uint8_t *list_context;

/* rbp's track-row kinds: a list of these is a track list; anything else is the
 * two-pane folder/category navigator, left alone */
static int track(unsigned k)
{
    switch (k & 255u) {
    case 4: case 0x5a: case 0x5d: case 0x5b: case 0x5c: case 0xc: case 0x2f:
    case 0x60: case 0x31: case 0x61: case 0x65: case 0x66: case 0x68: case 0x69:
    case 0x6b: case 0x6c: case 0x6e: case 0x6f: case 0x47: case 0x48: case 0x49:
    case 0x74: case 0x76:
        return 1;
    default:
        return 0;
    }
}

/* Category navigation does not always resend the right-column stream: recheck
 * the live left-list cache rather than a flag the row hook set. */
static int track_layout(void)
{
    const uint8_t *context = __atomic_load_n(&list_context, __ATOMIC_SEQ_CST);
    if (!context)
        return 0;
    unsigned index = word(context + 4);
    if (index > 4)
        return 0;
    unsigned offset = 0x17eacu + index * 0x1d1cu, count = word(context + offset - 4);
    return count && count <= 14 && track(half(context + offset + 4));
}

static unsigned copy_text(uint16_t *out, const uint16_t *in, unsigned max)
{
    unsigned n = 0;
    if (in)
        while (n < max && in[n]) {
            out[n] = in[n];
            n++;
        }
    if (n && out[n - 1] >= 0xd800 && out[n - 1] <= 0xdbff)
        n--;
    out[n] = 0;
    return n;
}

void browserows_key_text(unsigned key, uint16_t *out)
{
    unsigned n = 0;
    if (key >= 1 && key <= 24) {
        unsigned number = (key + 1) / 2;
        if (number >= 10)
            out[n++] = '1';
        out[n++] = (uint16_t)('0' + number % 10);
        out[n++] = (key & 1) ? 'A' : 'B';
    }
    out[n] = 0;
}

/* On rbp's database task, after its own row stream is consumed. Stream records
 * and strings keep rbp's allocator; only bounded UTF-16 copies leave. */
static int values_for(void *db, unsigned id, unsigned key, unsigned field, uint16_t out[128])
{
    out[0] = 0;
    if (field == BROWSE_FIELD_KEY)
        browserows_key_text(key, out);
    if (!field || field == BROWSE_FIELD_KEY)
        return 1;
    unsigned total = (unsigned)metadata(db, 2, id), first = 0;
    if (!total || total > 64)
        return 0;
    uint8_t *connection = (void *)(unsigned long)word(db);
    unsigned traffic = connection ? word(connection + 0xb94u) : 0;
    unsigned consumed = 0, formatted = 1;
    if (records(db, 2, &first, total, 0, total, 0)) {
        for (unsigned i = 0; i < total; i++) {
            uint32_t r[10] = { 0 };
            if (!next_record(db, r, 0, 0))
                break;
            consumed++;
            unsigned kind = r[2] & 255u;
            if (kind != BROWSE_FIELD_KEY && kind == field) {
                uint16_t *text = format(r[0], r[1], kind, 0xffffu);
                if (!text)
                    formatted = 0;
                copy_text(out, text, 127);
                if (text)
                    free_text(text);
            }
            if (r[7])
                free_item((void *)(unsigned long)r[7]);
        }
        skip(db);
    }
    if (connection)
        memcpy(connection + 0xb94u, &traffic, 4);
    if (field == BROWSE_FIELD_BPM) {
        unsigned n = 0;
        while (n < 127 && out[n])
            n++;
        if (n >= 4 && out[n - 4] == ' ' && out[n - 3] == 'b' && out[n - 2] == 'p' && out[n - 1] == 'm')
            out[n - 4] = 0;
    }
    return consumed == total && formatted;
}

/* ---- the database server's page snapshot ----------------------------------------
 * Local USB metadata captured on the database SERVER task while it prepares the
 * visible records, so the client's rows need no per-track DB round trip. Each
 * page replaces the snapshot, across USB remounts too. */
struct local_metadata {
    unsigned id, drive, key, bpm, length, valid, generation, has_artist, has_key;
    uint16_t artist[128];
};
static struct local_metadata local_page[32];
static unsigned local_gate, local_next, local_generation;
static int (*original_local_page)(void *, void *);
static void *(*original_local_record)(void *, const uint32_t *, unsigned, unsigned, unsigned, unsigned);
static void *(*edb_select)(int *, const char *, const char *, unsigned, unsigned, unsigned,
                           const char *, unsigned, const unsigned **) = (void *)0x3dd464u;
static void *(*edb_next)(void *, int *) = (void *)0x3ddd78u;
static void *(*edb_column)(void *, unsigned) = (void *)0x3dde5cu;
static int (*edb_close)(void *) = (void *)0x3ddb24u;
static unsigned (*local_key)(unsigned, unsigned) = (void *)0x16095cu;
static int (*table_string)(unsigned, unsigned, unsigned, uint16_t **, unsigned, unsigned) =
    (void *)0x19e360u;
static void (*edb_free)(void *) = (void *)0x175038u;

static int local_lock(void) { return !__sync_lock_test_and_set(&local_gate, 1u); }
static void local_unlock(void) { __sync_lock_release(&local_gate); }

static int local_page_rows(void *context, void *request)
{
    __sync_fetch_and_add(&local_generation, 1u);
    if (local_lock()) {
        memset(local_page, 0, sizeof(local_page));
        local_next = 0;
        local_unlock();
    }
    return original_local_page(context, request);
}

static void *local_record(void *context, const uint32_t *record, unsigned drive,
                          unsigned kind, unsigned mode, unsigned category)
{
    void *result = original_local_record(context, record, drive, kind, mode, category);
    const struct browse_column *c = column;
    if (result && kind == 1 && drive > 1 && drive < 4 && (mode == 1 || mode == 2) &&
        track(record[4] & 255u) && (c || marker)) {
        /* Each lookup is a DeviceSQL read on the stick being browsed: fetch only
         * what the column or the marker can ask for. */
        unsigned wanted = c ? c->field : 0;
        int want_artist = wanted == BROWSE_FIELD_ARTIST;
        int want_key = marker || wanted == BROWSE_FIELD_KEY;
        struct local_metadata item;
        memset(&item, 0, sizeof(item));
        item.generation = __atomic_load_n(&local_generation, __ATOMIC_SEQ_CST);
        item.id = record[0];
        item.drive = drive;
        const unsigned *id = &item.id;
        int error = 0;
        void *set = edb_select(&error, "djdbContent", "idxContent", 0, 0, 0, "=", 1, &id);
        if (set && !error) {
            void *row = edb_next(set, &error);
            if (row && !error) {
                const void *bpm = edb_column(row, 8), *length = edb_column(row, 9),
                           *artist = edb_column(row, 5);
                if (bpm && length && artist) {
                    item.bpm = word(bpm);
                    item.length = half(length);
                    item.valid = 1;
                    if (want_artist) {
                        uint16_t *name = 0;
                        if (table_string(7, word(artist), item.id, &name, 1, drive) >= 0) {
                            copy_text(item.artist, name, 127);
                            item.has_artist = 1;
                        } else
                            item.valid = 0;
                        if (name)
                            edb_free(name);
                    }
                }
            }
        }
        if (set)
            edb_close(set);
        if (item.valid) {
            if (want_key) {
                item.key = local_key(item.id, drive);
                item.has_key = 1;
            }
            if (local_lock()) {
                local_page[local_next] = item;
                local_next = (local_next + 1u) % 32u;
                local_unlock();
            }
        }
    }
    return result;
}

static int local_field(unsigned field)
{
    return !field || field == BROWSE_FIELD_ARTIST || field == BROWSE_FIELD_TIME ||
           field == BROWSE_FIELD_BPM || field == BROWSE_FIELD_KEY;
}

static unsigned decimal_text(uint16_t *out, unsigned value)
{
    uint16_t reverse[10];
    unsigned n = 0;
    do {
        reverse[n++] = (uint16_t)('0' + value % 10u);
        value /= 10u;
    } while (value && n < 10);
    for (unsigned i = 0; i < n; i++)
        out[i] = reverse[n - i - 1];
    return n;
}

static int local_value(const struct local_metadata *item, unsigned field, uint16_t out[128])
{
    out[0] = 0;
    if (!field)
        return 1;
    if (field == BROWSE_FIELD_ARTIST) {
        if (!item->has_artist)
            return 0;
        copy_text(out, item->artist, 127);
        return 1;
    }
    if (field == BROWSE_FIELD_KEY) {
        if (!item->has_key)
            return 0;
        browserows_key_text(item->key, out);
        return 1;
    }
    /* rbp's number formatter takes a DBCCmd-owned string these snapshots do not
     * own: format the bounded number here */
    unsigned n = 0;
    if (field == BROWSE_FIELD_BPM && item->bpm && item->bpm != 0x7fffffffu) {
        unsigned tenths = (item->bpm + 5u) / 10u;
        n = decimal_text(out, tenths / 10u);
        out[n++] = '.';
        out[n++] = (uint16_t)('0' + tenths % 10u);
    } else if (field == BROWSE_FIELD_TIME && item->length) {
        n = decimal_text(out, item->length / 60u);
        out[n++] = ':';
        out[n++] = (uint16_t)('0' + item->length % 60u / 10u);
        out[n++] = (uint16_t)('0' + item->length % 10u);
    }
    out[n] = 0;
    return 1;
}

static int local_values(void *db, unsigned id, unsigned field, uint16_t value[128], unsigned *key)
{
    if (!original_local_page || !original_local_record || !local_field(field))
        return 0;
    const uint8_t *connection = (void *)(unsigned long)word(db);
    /* db[1] is the drive, db[2] the database kind; a remote player's connection
     * has connection[1] set and cannot use this snapshot */
    if (!connection || word(connection + 4) || word((uint8_t *)db + 8) != 1)
        return 0;
    unsigned drive = word((uint8_t *)db + 4);
    struct local_metadata item;
    int found = 0;
    if (!local_lock())
        return 0;
    unsigned generation = __atomic_load_n(&local_generation, __ATOMIC_SEQ_CST);
    for (unsigned i = 0; i < 32; i++)
        if (local_page[i].valid && local_page[i].generation == generation &&
            local_page[i].id == id && local_page[i].drive == drive) {
            item = local_page[i];
            found = 1;
            break;
        }
    local_unlock();
    if (!found || (marker && !item.has_key))
        return 0;
    *key = item.key;
    return local_value(&item, field, value);
}

/* ---- the client's cache ----------------------------------------------------------
 * Two viewports, raw metadata only (never colours: MASTER and its set are applied
 * on every page). A change of list, source, sort or field empties it; a failed
 * lookup is retried. */
enum { METADATA_SLOTS = 32 };
static struct {
    const void *context, *db;
    unsigned connection, index, depth, sort, field, epoch;
} metadata_scope;
static struct {
    unsigned used, id, key, key_valid, values_valid;
    uint8_t identity[532];
    uint16_t value[128];
} metadata_cache[METADATA_SLOTS];
static unsigned metadata_next, metadata_epoch;

static void invalidate_metadata(void) { __sync_fetch_and_add(&metadata_epoch, 1u); }

static void metadata_bind(const void *context, void *db, unsigned index, unsigned depth,
                          unsigned sort, unsigned field)
{
    unsigned epoch = __atomic_load_n(&metadata_epoch, __ATOMIC_SEQ_CST), connection = word(db);
    if (metadata_scope.context != context || metadata_scope.db != db ||
        metadata_scope.connection != connection || metadata_scope.index != index ||
        metadata_scope.depth != depth || metadata_scope.sort != sort ||
        metadata_scope.field != field || metadata_scope.epoch != epoch) {
        memset(metadata_cache, 0, sizeof(metadata_cache));
        metadata_next = 0;
        metadata_scope.context = context;
        metadata_scope.db = db;
        metadata_scope.connection = connection;
        metadata_scope.index = index;
        metadata_scope.depth = depth;
        metadata_scope.sort = sort;
        metadata_scope.field = field;
        metadata_scope.epoch = epoch;
    }
}

static unsigned metadata_values(void *db, unsigned id, const uint8_t identity[532], int need_key,
                                unsigned field, uint16_t value[128])
{
    value[0] = 0;
    if (!need_key && !field)
        return 0;
    unsigned slot = METADATA_SLOTS;
    for (unsigned i = 0; i < METADATA_SLOTS; i++)
        if (metadata_cache[i].used && metadata_cache[i].id == id &&
            !memcmp(metadata_cache[i].identity, identity, 532)) {
            slot = i;
            break;
        }
    if (slot == METADATA_SLOTS) {
        slot = metadata_next;
        metadata_next = (metadata_next + 1u) % METADATA_SLOTS;
        memset(&metadata_cache[slot], 0, sizeof(metadata_cache[slot]));
        metadata_cache[slot].used = 1;
        metadata_cache[slot].id = id;
        memcpy(metadata_cache[slot].identity, identity, 532);
    }
    {
        unsigned key = 0;
        if (local_values(db, id, field, metadata_cache[slot].value, &key)) {
            metadata_cache[slot].values_valid = 1;
            metadata_cache[slot].key = key;
            metadata_cache[slot].key_valid = key <= 24;
        }
    }
    unsigned key = 0;
    if (need_key) {
        if (!metadata_cache[slot].key_valid) {
            metadata_cache[slot].key = browserows_track_key(db, id);
            metadata_cache[slot].key_valid = metadata_cache[slot].key >= 1 &&
                                             metadata_cache[slot].key <= 24;
        }
        key = metadata_cache[slot].key;
    }
    if (field) {
        if (!metadata_cache[slot].values_valid)
            metadata_cache[slot].values_valid =
                values_for(db, id, key, field, metadata_cache[slot].value);
        memcpy(value, metadata_cache[slot].value, 256);
        if (field == BROWSE_FIELD_KEY)
            browserows_key_text(key, value);
    }
    return key;
}

/* The column's value travels inside its own row, so queued updates, scrolling and
 * duplicate names cannot pair a value with the wrong track: the row's text, then
 * SEP, badge+1, the value, END, SEP. Only browserows_draw() reads the suffix. */
unsigned browserows_append(uint8_t *dest, const uint8_t *entry, const uint16_t *value,
                           unsigned image)
{
    unsigned len = half(entry + 20), extra = 0;
    while (extra < 127 && value[extra])
        extra++;
    unsigned prefix = len, room = 255u - extra - 4u;
    if (prefix > room)
        prefix = room;
    const uint16_t *text = (const void *)(entry + 22);
    if (prefix && text[prefix - 1] >= 0xd800 && text[prefix - 1] <= 0xdbff)
        prefix--;
    memcpy(dest, entry, 22 + prefix * 2);
    if (prefix < len && prefix)
        put_half(dest + 22 + (prefix - 1) * 2, 0x2026);
    unsigned n = prefix;
    put_half(dest + 22 + n++ * 2, SEP);
    put_half(dest + 22 + n++ * 2, image + 1u);
    memcpy(dest + 22 + n * 2, value, extra * 2);
    n += extra;
    put_half(dest + 22 + n++ * 2, END);
    put_half(dest + 22 + n++ * 2, SEP);
    put_half(dest + 20, n);
    return 22 + n * 2;
}

int browserows_split(const uint16_t *s, unsigned n, unsigned *split, unsigned *badge,
                     unsigned *extra)
{
    unsigned at = 0;
    if (!s || n < 4 || s[n - 2] != END || s[n - 1] != SEP)
        return 0;
    while (at < n - 3 && s[at] != SEP)
        at++;
    if (at >= n - 3 || !s[at + 1] || n - at - 4u > 127)
        return 0;
    *split = at;
    *badge = s[at + 1] - 1u;
    *extra = n - at - 4u;
    return 1;
}

static uint8_t rebuilt[MAX_MESSAGE];
static unsigned busy;

void browserows_process(uint8_t *context, uint8_t *message,
                        const struct browse_column *column, const struct browse_marker *marker)
{
    observe_harmony(context);
    __atomic_store_n(&list_context, context, __ATOMIC_SEQ_CST);
    __atomic_store_n(&track_list, 0u, __ATOMIC_SEQ_CST);
    unsigned index = word(context + 4), bytes = word(message + 0x1c), count = half(message + 0x80);
    if (index > 4 || bytes < 20 || bytes > MAX_MESSAGE - 0x70 || count < 2 || count > 15) {
        invalidate_metadata();
        return;
    }
    unsigned rows = 0x17eacu + index * 0x1d1cu, available = word(context + rows - 4);
    if (available > 14 || count > available + 1) {
        invalidate_metadata();
        return;
    }
    void *db = (void *)(unsigned long)word(context + (index + 0x1bceu) * 4u + 4u);
    if (!db || !word(db)) {
        invalidate_metadata();
        return;
    }
    unsigned native_sort = word((uint8_t *)db + 16);
    /* a folder/category list stays rbp's own two-pane navigator */
    for (unsigned i = 0; i < count - 1; i++)
        if (!track(half(context + rows + i * 532 + 4))) {
            invalidate_metadata();
            return;
        }
    __atomic_store_n(&track_list, 1u, __ATOMIC_SEQ_CST);
    unsigned source = 20;
    for (unsigned i = 0; i < count; i++) {
        if (source + 22 > bytes) {
            invalidate_metadata();
            return;
        }
        unsigned len = half(message + 0x70 + source + 20);
        if (len > 255 || source + 22 + len * 2 > bytes) {
            invalidate_metadata();
            return;
        }
        source += 22 + len * 2;
    }
    unsigned depth = word((uint8_t *)db + 4);
    if (!__sync_bool_compare_and_swap(&busy, 0u, 1u)) {
        invalidate_metadata();
        return;
    }
    memcpy(rebuilt, message, 0x84);
    source = 20;
    unsigned dest = 20, field = column ? column->field : 0;
    metadata_bind(context, db, index, depth, native_sort, field);
    struct harmonic_reference harmony = harmonic_reference();
    unsigned ref = word(context + 0x10af8) && harmony.key >= 0 ? (unsigned)harmony.key + 1 : 0;
    for (unsigned i = 0; i < count; i++) {
        uint8_t *entry = message + 0x70 + source;
        unsigned old_size = 22 + half(entry + 20) * 2, category = half(entry), key = 0;
        uint16_t value[128] = { 0 };
        unsigned badge = 0;
        if (i) {
            unsigned id = word(context + rows + (i - 1) * 532);
            int need_key = (marker && ref) || field == BROWSE_FIELD_KEY;
            key = metadata_values(db, id, context + rows + (i - 1) * 532, need_key, field, value);
            if (marker && ref && key >= 1 && key <= 24) {
                if (harmony.native_keys & (1u << (key - 1)))
                    category = BROWSE_CATEGORY_GREEN;
                else if (harmony.shift && category == BROWSE_CATEGORY_GREEN)
                    category = 51;      /* rbp's "no match": green was for the unshifted key */
            }
            if (marker)
                category = marker->category(ref, key, category);
            if (column) {
                if (!value[0]) {
                    value[0] = 0x2014;
                    value[1] = 0;
                }
                if (field == BROWSE_FIELD_KEY && marker)
                    badge = marker->image(category);
            }
        } else if (column)
            copy_text(value, column->caption, 127);
        unsigned size = column ? browserows_append(rebuilt + 0x70 + dest, entry, value, badge)
                               : old_size;
        if (!column)
            memcpy(rebuilt + 0x70 + dest, entry, size);
        put_half(rebuilt + 0x70 + dest, category);
        dest += size;
        source += old_size;
    }
    /* 15 maximum-length rows fit rbp's own 8 KiB message */
    memcpy(rebuilt + 0x1c, &dest, 4);
    memcpy(message, rebuilt, 0x70 + dest);
    __sync_lock_release(&busy);
}

static int rows_hooked(void *context, void *message, void *request)
{
    int result = original_rows(context, message, request);
    const struct browse_column *c = __atomic_load_n(&column, __ATOMIC_SEQ_CST);
    const struct browse_marker *m = __atomic_load_n(&marker, __ATOMIC_SEQ_CST);
    if (c || m)
        browserows_process(context, message, c, m);
    return result;
}

static int functions_measured(void)
{
    for (unsigned i = 0; i < sizeof(called) / sizeof(*called); i++)
        if (memcmp((const void *)(unsigned long)called[i].address, &called[i].a, 8))
            return 0;
    return 1;
}

static int ensure(void)
{
    if (original_rows)
        return 1;
    if (!functions_measured())
        return 0;
    /* push {r4-r11,lr} ; sub sp,sp,#44 */
    static const uint8_t guard[8] = { 0xf0, 0x4f, 0x2d, 0xe9, 0x2c, 0xd0, 0x4d, 0xe2 };
    original_rows = rbp_hook(ROW_HOOK, guard, (void *)rows_hooked);
    if (!original_rows)
        return 0;
    /* DBSMain_ListBufGetContents, DBSMain_ListBufRec2CommCmd: push ; sub sp */
    static const uint32_t page_guard[2] = { 0xe92d4ff0u, 0xe24dd064u };
    static const uint32_t record_guard[2] = { 0xe92d4ff0u, 0xe24dd014u };
    original_local_record = rbp_hook(0x207dd0u, (const uint8_t *)record_guard, (void *)local_record);
    if (original_local_record)
        original_local_page = rbp_hook(0x20805cu, (const uint8_t *)page_guard,
                                       (void *)local_page_rows);
    /* without the snapshot, each row asks the database itself */
    return 1;
}

/* ---- the third column on the glass ---------------------------------------------- */
/* The on-screen LOAD 1 / LOAD 2 under a selected row give their room to the
 * column: their touch does nothing while it is shown. The FLX10's LOAD keys call
 * UiKey_Load1/2 directly and still load. */
static int (*original_load[2])(unsigned);
static int list_load1(unsigned event) { return column ? 0 : original_load[0](event); }
static int list_load2(unsigned event) { return column ? 0 : original_load[1](event); }

/* The selected row's text scrolls on rbp's two native surfaces and its timers. */
static void (*original_scroll_prepare)(unsigned, unsigned);
static void (*original_scroll_start)(const uint16_t *, void *, unsigned);
static void (*original_scroll_run)(unsigned);
static void (*scroll_timer)(unsigned, unsigned, unsigned) = (void *)0x18e260u;
static void (*scroll_kill)(unsigned, unsigned) = (void *)0x18e2a0u;
static int (*scroll_destroy)(void *) = (void *)0x18e730u;
static void (*scroll_show)(void *, unsigned) = (void *)0x1cec2cu;
static uint8_t *scroll_state = (void *)0x02684610u;
static uint8_t *scroll_strings = (void *)0x0551abcau;
static const uint8_t *scroll_ids = (void *)0x004c97b8u;
static uint16_t scroll_text[2][256];
static unsigned scroll_owned[2], scroll_phase[2], scroll_seen[2], scroll_row[2];
enum { SCROLL_STEP_MS = 60, SCROLL_TAIL_PX = 24 };
static const void *scroll_selection = (void *)0x05b528ecu;
static const void *browse_view = (void *)0x0114c2d0u;

/* BrowseUiIf::GetDisplayState's primary 3 is the browser */
static int browse_visible(void)
{
    void *view = (void *)(unsigned long)word(browse_view);
    return view && word((uint8_t *)view + 4) == 3;
}

static void scroll_prepare(unsigned lane, unsigned row)
{
    if (lane > 1 || row > 11 || !column || !track_layout()) {
        if (lane < 2) {
            scroll_owned[lane] = 0;
            scroll_phase[lane] = 0;
        }
        original_scroll_prepare(lane, row);
        return;
    }
    uint8_t *state = scroll_state + lane * 0x74;
    uint16_t *source = (void *)(scroll_strings + lane * 0x1ce4 + (row + 1) * 0x238);
    uint16_t saved[256];
    memcpy(saved, source, sizeof(saved));
    unsigned n = 0;
    while (n < 255 && saved[n] && saved[n] != SEP)
        n++;
    memcpy(scroll_text[lane], saved, n * 2);
    scroll_text[lane][n] = 0;
    void *root = (void *)(unsigned long)word(root_pointer);
    void *glyph = root ? object_by_id(root, (int)(int16_t)half(scroll_ids + 0x100 + (row + lane * 12) * 2)) : 0;
    unsigned right = glyph ? half((uint8_t *)glyph + 0x1c) : 0;
    /* the right lane measures its own text against its own width only */
    if (lane == 1 && glyph)
        put_half((uint8_t *)glyph + 0x1c, 1110);
    memcpy(source, scroll_text[lane], (n + 1) * 2);
    original_scroll_prepare(lane, row);
    memcpy(source, saved, sizeof(saved));
    if (lane == 1 && glyph)
        put_half((uint8_t *)glyph + 0x1c, right);
    put_word(state + 0x20, (unsigned)(unsigned long)scroll_text[lane]);
    scroll_owned[lane] = 1;
    scroll_phase[lane] = 0;
    unsigned timer = word(state + 0x38);
    if (timer) {
        scroll_kill(0x82a, timer);
        scroll_timer(0x82a, timer, 2000);
    }
}

static void scroll_start(const uint16_t *text, void *glyph, unsigned lane)
{
    original_scroll_start(text, glyph, lane);
    if (lane < 2 && column && scroll_owned[lane]) {
        unsigned timer = word(scroll_state + lane * 0x74 + 0x10);
        scroll_kill(0x82a, timer);
        scroll_timer(0x82a, timer, SCROLL_STEP_MS);
    }
    /* keep the column visible: the ordinary draw leaves out only the prefix
     * while the native surface paints it */
    if (lane == 1 && column && scroll_owned[1])
        scroll_show(glyph, 1);
}

/* Move rbp's repeated copy out of the viewport while it paints, keeping the
 * measured width between calls. */
static void scroll_native_frame(unsigned lane, uint8_t *state, unsigned width, unsigned viewport)
{
    put_word(state + 0x48, width + viewport + SCROLL_TAIL_PX);
    original_scroll_run(lane);
    put_word(state + 0x48, width);
}

static void scroll_run(unsigned lane)
{
    if (lane < 2 && column && scroll_owned[lane]) {
        uint8_t *state = scroll_state + lane * 0x74;
        unsigned width = word(state + 0x48), viewport = word(state + 0x2c), timer = word(state + 0x10);
        if (word(state + 0x24) && width > viewport) {
            int offset = (int)word(state + 0x58);
            unsigned end = width + SCROLL_TAIL_PX;
            if (scroll_phase[lane] == 1) {
                /* rbp's run moves 3 px: draw the start again, then pause 2 s */
                put_word(state + 0x58, 3);
                put_word(state + 0x64, 0);
                scroll_native_frame(lane, state, width, viewport);
                scroll_phase[lane] = 2;
                scroll_kill(0x82a, timer);
                scroll_timer(0x82a, timer, 2000);
                return;
            }
            if (scroll_phase[lane] == 2) {
                scroll_phase[lane] = 0;
                scroll_kill(0x82a, timer);
                scroll_timer(0x82a, timer, SCROLL_STEP_MS);
            }
            if (offset <= 0 && (unsigned)(-offset) + 3 >= end) {
                put_word(state + 0x58, (unsigned)(3 - (int)end));
                scroll_phase[lane] = 1;
            }
            scroll_native_frame(lane, state, width, viewport);
            return;
        }
    }
    original_scroll_run(lane);
}

/* rbp decided whether the selected row scrolls before the column narrowed it:
 * redo its setup for the selected glyph once the layout is final. */
static void scroll_observe(void *model)
{
    if (!column || !track_layout() || !browse_visible() || menu_visible())
        return;
    unsigned row = word(scroll_selection);
    if (row >= 12)
        return;
    void *root = (void *)(unsigned long)word(root_pointer);
    if (!root)
        return;
    const uint8_t *context = list_context;
    unsigned index = word(context + 4);
    unsigned id = word(context + 0x17eac + index * 0x1d1c + row * 532);
    for (unsigned lane = 0; lane < 2; lane++) {
        int glyph_id = (int)(int16_t)half(scroll_ids + 0x100 + (row + lane * 12) * 2);
        if (model != object_by_id(root, glyph_id))
            continue;
        if (scroll_seen[lane] == id && scroll_row[lane] == row && scroll_owned[lane])
            return;
        uint8_t *state = scroll_state + lane * 0x74;
        if (scroll_owned[lane]) {
            scroll_kill(0x82a, word(state + 8));
            scroll_kill(0x82a, word(state + 0x10));
            if (word(state + 0x24))
                scroll_destroy(state + 0x24);
            scroll_show((void *)(unsigned long)word(state + 0x1c), 1);
            int old_id = (int)(int16_t)half(scroll_ids + 0x100 + (scroll_row[lane] + lane * 12) * 2);
            refresh_object(root, old_id);
        }
        scroll_seen[lane] = id;
        scroll_row[lane] = row;
        scroll_prepare(lane, row);
        return;
    }
}

/* NS_PALRender_DrawText / DrawImage. A text model: rect at 0x18 (left, top,
 * right, bottom), UTF-16 at 0x34, its length byte at 0x38; an image model's ID
 * at 0x44. rbp's font, colour, alignment and window ancestry are kept. */
typedef void (*draw_fn)(void *, void *);
static draw_fn original_draw_text, original_draw_image;
static unsigned caption_ready;
static uint32_t caption_model[21];
static void *caption_object;

static int column_draw(void *render, void *model)
{
    const struct browse_column *selected = column;
    scroll_observe(model);
    uint8_t *m = model;
    unsigned left = half(m + 0x18), right = half(m + 0x1c);
    const uint16_t *s = (void *)(unsigned long)word(m + 0x34);
    unsigned n = m[0x38], split, badge, extra;
    if (!s || !n)
        return 0;
    if (n == 255) {
        n = 0;
        while (n < 255 && s[n])
            n++;
    }
    if (!browserows_split(s, n, &split, &badge, &extra))
        return 0;
    if (right <= left + 24)
        return 0;
    uint32_t clone[21];
    memcpy(clone, model, sizeof(clone));
    uint8_t *c = (void *)clone;
    /* the selected row ends at LOAD 1: reclaim the same right edge as the rest */
    if (right == 1020)
        right = 1258;
    unsigned edge = right - 140;
    uint16_t prefix[256] = { 0 }, value[128] = { 0 };
    memcpy(prefix, s, split * 2);
    memcpy(value, s + split + 2, extra * 2);
    uint32_t ptr = (uint32_t)(unsigned long)prefix;
    memcpy(c + 0x34, &ptr, 4);
    c[0x38] = (uint8_t)split;
    put_half(c + 0x1c, edge - 8);
    int native_prefix = scroll_owned[1] && word(scroll_state + 0x74 + 0x24) &&
                        (void *)(unsigned long)word(scroll_state + 0x74 + 0x1c) == model;
    if (!native_prefix)
        original_draw_text(render, c);
    memcpy(clone, model, sizeof(clone));
    ptr = (uint32_t)(unsigned long)value;
    memcpy(c + 0x34, &ptr, 4);
    c[0x38] = (uint8_t)extra;
    put_half(c + 0x18, edge + 8);
    put_half(c + 0x1c, right);
    c[0x3c] = (uint8_t)(c[0x3c] % 3);
    if (badge && right > edge + 80) {
        uint32_t icon[21];
        memcpy(icon, c, sizeof(icon));
        uint8_t *im = (void *)icon;
        unsigned top = half(c + 0x1a), bottom = half(c + 0x1e);
        unsigned y = top + (bottom - top > 24 ? (bottom - top - 24) / 2 : 0);
        put_half(im + 0x18, edge + 8);
        put_half(im + 0x1c, edge + 32);
        put_half(im + 0x1a, y);
        put_half(im + 0x1e, y + 24);
        memcpy(im + 0x44, &badge, 4);
        original_draw_image(render, im);
        put_half(c + 0x18, edge + 38);
    }
    original_draw_text(render, c);
    if (selected && !caption_ready) {
        void *root = (void *)(unsigned long)word(root_pointer);
        if (root) {
            caption_object = object_by_id(root, COLUMN_HEADING_ID);
            if (caption_object) {
                memcpy(caption_model, model, sizeof(caption_model));
                caption_ready = 1;
                refresh_object(root, COLUMN_HEADING_ID);
            }
        }
    }
    return 1;
}

/* The headings are images in another render context than the rows: draw the
 * column's after the heading image, in its window and rectangle, with a row's
 * font -- the first row drawn schedules one refresh of the heading for that. */
static void column_caption(void *render, void *model)
{
    const struct browse_column *c = column;
    if (!caption_ready || model != caption_object || !__atomic_load_n(&track_list, __ATOMIC_SEQ_CST) ||
        !track_layout())
        return;
    uint32_t clone[21];
    memcpy(clone, caption_model, sizeof(clone));
    uint8_t *m = (void *)clone, *source = model;
    memcpy(m + 8, source + 8, 12);
    memcpy(m + 0x18, source + 0x18, 8);
    uint16_t label[128] = { 0 };
    unsigned n = copy_text(label, c->caption, 127);
    uint32_t ptr = (uint32_t)(unsigned long)label, color = 0, fill = 0;
    memcpy(m + 0x34, &ptr, 4);
    m[0x38] = (uint8_t)n;
    m[0x3c] = 1;
    put_half(m + 0x18, 1126);
    put_half(m + 0x1c, 1258);
    unsigned center = (half(source + 0x1a) + half(source + 0x1e)) / 2;
    put_half(m + 0x1a, center > 20 ? center - 20 : 0);
    put_half(m + 0x1e, center + 20);
    /* BROWSE text's neutral style, ALL_COLOR_LIST_TEXT[0] */
    memcpy(m + 0x28, &color, 4);
    memcpy(m + 0x40, &fill, 4);
    original_draw_text(render, m);
}

static void draw_text_hooked(void *render, void *text)
{
    if (column && column_draw(render, text))
        return;
    original_draw_text(render, text);
}

static void draw_image_hooked(void *render, void *image)
{
    if (column) {
        /* objects 8 and 9 are the floating LOAD 1 / LOAD 2 images of BROWSE;
         * the same image IDs elsewhere are left alone */
        void *root = (void *)(unsigned long)word(root_pointer);
        if (root && (image == object_by_id(root, 8) || image == object_by_id(root, 9)))
            return;
    }
    original_draw_image(render, image);
    if (column)
        column_caption(render, image);
}

int browserows_marker(const struct browse_marker *m)
{
    if (!m || !m->category || !m->image || marker || !ensure())
        return -1;
    __atomic_store_n(&marker, m, __ATOMIC_SEQ_CST);
    return 0;
}

int browserows_column(const struct browse_column *c)
{
    if (!c || !c->caption || column || !ensure())
        return -1;
    /* every hook below is checked first: the column goes in whole or not at all */
    static const uint8_t load_guard[8] = { 0x10, 0x40, 0x2d, 0xe9, 0x00, 0x40, 0xa0, 0xe1 };
    /* add r3,r0,r0,lsl#1 ; ldr r2,[pc,#0x2b0] -- the literal load rbp_hook moves */
    static const uint8_t prep_guard[8] = { 0x80, 0x30, 0x80, 0xe0, 0xb0, 0x22, 0x9f, 0xe5 };
    static const uint8_t start_guard[8] = { 0xf0, 0x4f, 0x2d, 0xe9, 0x74, 0xa0, 0xa0, 0xe3 };
    static const uint8_t run_guard[8] = { 0xf0, 0x4f, 0x2d, 0xe9, 0x74, 0x40, 0xa0, 0xe3 };
    static const uint8_t text_guard[8] = { 0xf0, 0x4f, 0x2d, 0xe9, 0x4d, 0xdf, 0x4d, 0xe2 };
    static const uint8_t image_guard[8] = { 0xf0, 0x4f, 0x2d, 0xe9, 0xcc, 0xd0, 0x4d, 0xe2 };
    static const struct { unsigned long at; const uint8_t *guard; } sites[] = {
        { 0x11d9b0, load_guard }, { 0x11d83c, load_guard }, { 0x2995c8, prep_guard },
        { 0x2955d8, start_guard }, { 0x29992c, run_guard }, { 0x1d23e4, text_guard },
        { 0x1d3284, image_guard },
    };
    for (unsigned i = 0; i < sizeof(sites) / sizeof(*sites); i++)
        if (memcmp((const void *)sites[i].at, sites[i].guard, 8))
            return -1;
    original_load[0] = rbp_hook(0x11d9b0, load_guard, (void *)list_load1);     /* UiKey_BtnOnListLoad1 */
    original_load[1] = rbp_hook(0x11d83c, load_guard, (void *)list_load2);     /* UiKey_BtnOnListLoad2 */
    original_scroll_prepare = rbp_hook(0x2995c8, prep_guard, (void *)scroll_prepare);
    original_scroll_start = rbp_hook(0x2955d8, start_guard, (void *)scroll_start);
    original_scroll_run = rbp_hook(0x29992c, run_guard, (void *)scroll_run);
    original_draw_text = (draw_fn)rbp_hook(0x1d23e4, text_guard, (void *)draw_text_hooked);
    original_draw_image = (draw_fn)rbp_hook(0x1d3284, image_guard, (void *)draw_image_hooked);
    if (!original_load[0] || !original_load[1] || !original_scroll_prepare ||
        !original_scroll_start || !original_scroll_run || !original_draw_text ||
        !original_draw_image)
        return -1;      /* only on mmap/mprotect failing; whatever went in is inert */
    __atomic_store_n(&column, c, __ATOMIC_SEQ_CST);
    return 0;
}
