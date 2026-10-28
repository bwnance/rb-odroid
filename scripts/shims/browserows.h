/* SPDX-License-Identifier: MPL-2.0
 *
 * browserows.h -- rbp's BROWSE track rows, rewritten as rbp builds them, for a
 * third column (browsecol.c; browserows.c).
 */
#ifndef RBPI4B_BROWSEROWS_H
#define RBPI4B_BROWSEROWS_H

#include <stdint.h>

/* rbp's metadata kinds, as its rows and DB records carry them */
#define BROWSE_FIELD_ARTIST 7u
#define BROWSE_FIELD_TIME   11u
#define BROWSE_FIELD_BPM    13u
#define BROWSE_FIELD_KEY    15u

struct browse_column {
    unsigned int field;             /* BROWSE_FIELD_* */
    const uint16_t *caption;        /* the heading, UTF-16 */
};

/* Registers the column, once, for good, with every hook it needs. 0 = in. */
int browserows_column(const struct browse_column *column);

/* ---- for the test --------------------------------------------------------- */
/* rbp's track-key lookup the KEY column calls, reassignable */
extern unsigned int (*browserows_track_key)(void *db, unsigned int id);
/* The rewrite itself, on rbp's row message, after GetRightBrowseOne2One built it. */
void browserows_process(uint8_t *context, uint8_t *message, const struct browse_column *column);
/* "12A"-style text for rbp key 1..24; empty otherwise. */
void browserows_key_text(unsigned int key, uint16_t *out);
/* A row entry with the column's `value` appended behind noncharacters; answers
 * its size. browserows_split() takes one apart. */
unsigned int browserows_append(uint8_t *dest, const uint8_t *entry, const uint16_t *value);
int browserows_split(const uint16_t *s, unsigned int n, unsigned int *split, unsigned int *extra);

#endif /* RBPI4B_BROWSEROWS_H */
