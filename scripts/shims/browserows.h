/* SPDX-License-Identifier: MPL-2.0
 *
 * browserows.h -- rbp's BROWSE track rows, rewritten as rbp builds them, for a
 * third column (browsecol.c; browserows.c).
 */
#ifndef RBPI4B_BROWSEROWS_H
#define RBPI4B_BROWSEROWS_H

#include <stdint.h>

/* rbp's metadata kinds, as its rows and DB records carry them */
#define BROWSE_FIELD_ALBUM  2u
#define BROWSE_FIELD_ARTIST 7u
#define BROWSE_FIELD_TIME   11u
#define BROWSE_FIELD_BPM    13u
#define BROWSE_FIELD_KEY    15u

struct browse_column {
    unsigned int field;             /* BROWSE_FIELD_* */
    const uint16_t *caption;        /* the heading, UTF-16 */
};

/* Every hook the column needs, once, for good; until a column is set they pass
 * everything through. 0 = in. */
int browserows_install(void);
/* The column shown from the next page on; NULL = none. Any thread. */
void browserows_set(const struct browse_column *column);
/* Called often from rbp's threads (a page built, a text drawn): the chooser's
 * chance to look for a new choice. Set before installing; it must be cheap. */
extern void (*browserows_tick)(void);

/* ---- the choice (browsecol.c) and its LIST MENU entry (browsemenu.c) ------- */
/* A word's field (0 for off or unknown) and its heading. */
unsigned int browsecol_field(const char *value, const uint16_t **caption);
/* Make `word` the choice, from the next page on; the word in effect. */
void browsecol_choose(const char *word);
const char *browsecol_current(void);
/* COLUMN in LIST MENU, beside SORT; once. 0 = in. */
int browsemenu_install(void);
/* LIST MENU as rbp just built it in `context` gets COLUMN as its third entry;
 * answers 1 if it did. */
int browsemenu_add_column(uint8_t *context);
/* COLUMN's own menu, OFF..TIME, the current one checked, into `context`. */
void browsemenu_fill(uint8_t *context, unsigned int first, const char *current);
/* The word of that menu's item `item`, or NULL. */
const char *browsemenu_word(unsigned int item);

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
