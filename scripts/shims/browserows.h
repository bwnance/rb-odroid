/* SPDX-License-Identifier: MPL-2.0
 *
 * browserows.h -- rbp's BROWSE track rows, rewritten as rbp builds them
 * (browserows.c): a key-match marker (keymatch.c) and a third column
 * (browsecol.c) share the one row hook.
 */
#ifndef RBPI4B_BROWSEROWS_H
#define RBPI4B_BROWSEROWS_H

#include <stdint.h>

/* rbp's metadata kinds, as its rows and DB records carry them */
#define BROWSE_FIELD_ARTIST 7u
#define BROWSE_FIELD_TIME   11u
#define BROWSE_FIELD_BPM    13u
#define BROWSE_FIELD_KEY    15u

/* A row's icon category: 52 is rbp's own green key match. */
#define BROWSE_CATEGORY_GREEN 52u

struct browse_column {
    unsigned int field;             /* BROWSE_FIELD_* */
    const uint16_t *caption;        /* the heading, UTF-16 */
};

struct browse_marker {
    /* A track row's icon category, given the MASTER's key and the track's (1..24,
     * 0 unknown) and the category rbp gave it. On rbp's database task. */
    unsigned int (*category)(unsigned int reference, unsigned int key, unsigned int native);
    /* The image ID to draw beside a key in the column for `category`, or 0. */
    unsigned int (*image)(unsigned int category);
};

/* Each registers once, for good; the row hook goes in with the first. 0 = in. */
int browserows_marker(const struct browse_marker *marker);
int browserows_column(const struct browse_column *column);

/* The shift deck `deck` plays at, in semitones, so the MASTER's key is the one
 * heard (keyshift.c's keyshift_get); NULL = none. Set before registering. */
extern int (*browserows_shift)(int deck);

/* ---- for the test --------------------------------------------------------- */
/* rbp's functions the row rewrite calls, reassignable */
extern unsigned int (*browserows_master_on)(unsigned int deck);
extern unsigned int (*browserows_track_key)(void *db, unsigned int id);
/* The rewrite itself, on rbp's row message, after GetRightBrowseOne2One built it. */
void browserows_process(uint8_t *context, uint8_t *message,
                        const struct browse_column *column, const struct browse_marker *marker);
/* "12A"-style text for rbp key 1..24; empty otherwise. */
void browserows_key_text(unsigned int key, uint16_t *out);
/* A row entry with the column's `value` and badge `image` appended behind
 * noncharacters; answers its size. browserows_split() takes one apart. */
unsigned int browserows_append(uint8_t *dest, const uint8_t *entry, const uint16_t *value,
                               unsigned int image);
int browserows_split(const uint16_t *s, unsigned int n, unsigned int *split,
                     unsigned int *badge, unsigned int *extra);

#endif /* RBPI4B_BROWSEROWS_H */
