/*
 * utility_rows.h -- rows of the shim's own in rbp's UTILITY menu.
 *
 * rbp's UTILITY screen is a table (`stUtilityObjects` @0x5140bc, after its count
 * `ulUtilityItemNum` @0x5140b8) of 56-byte rows, each with its label, its value
 * strings and eight handlers; everything that walks it is driven by the count.
 * The table is exactly full, so a row of ours means a bigger copy: the shim
 * builds one at load -- rbp's 33 rows with ours inserted -- and points rbp's
 * seven literal-pool words at it (docs/21 has the measurements). The first row
 * is HDMI AUDIO, after MIXER MODE (operator, 2026-10-26), which writes the same
 * /run/rblive4/mirror.mode the shim's own panel does.
 *
 * The table building and the row's handlers are pure enough for a host test
 * (test_utility.c); utility_rows_install() is the part that touches rbp.
 */
#ifndef RBPI4B_UTILITY_ROWS_H
#define RBPI4B_UTILITY_ROWS_H

#include <stdint.h>

/* One row, as rbp lays it out. */
struct ut_entry {
    const uint16_t *label;          /* +0  UTF-16, NUL-terminated */
    const uint16_t *const *values;  /* +4  the value strings */
    uint32_t nvalues;               /* +8  rbp reads it as a halfword */
    int32_t cur;                    /* +12 the value in force */
    int32_t edit;                   /* +16 the value being chosen, -1 none */
    uint32_t editable;              /* +20 */
    uint32_t fn[8];                 /* +24 InitItem SetLine StartEdit SelMenu
                                     *     IsModified EnterMenu IsGrey Reset */
};

#define UT_FN_SETLINE 1
#define UT_FN_ENTER   5

/* The bigger table: the count, then the rows, exactly the shape of rbp's. */
#define UT_ROWS_MAX 40
struct ut_table {
    uint32_t count;
    struct ut_entry rows[UT_ROWS_MAX];
};

/* rbp's `n` rows (`src`) into `dst`, with `ours[0..n_ours)` inserted after row
 * `after`. Answers the new count, or -1 when it would not fit. Pure. */
int ut_build(struct ut_table *dst, const struct ut_entry *src, int n, int after,
             const struct ut_entry *ours, int n_ours);

/* The HDMI AUDIO row's two handlers, with rbp's calling convention:
 * SetLine(entry, lines) fills lines[0] with the label and lines[1] with the value
 * (each line: +32 the length in UTF-16 units, +34 the text, 512 bytes cleared),
 * reading mirror.mode into `cur` while nothing is being chosen; Enter(entry)
 * applies `edit`, writing the file, and answers 1 if the value changed. Index 0
 * is OFF and 1 ON, the order of the value strings it borrows. */
int ut_mirror_setline(struct ut_entry *e, unsigned char **lines);
int ut_mirror_enter(struct ut_entry *e);

/* Build the table and point rbp at it, in rbp only and only if every word it is
 * about to change still holds what it was measured to hold. 0 = done. */
int utility_rows_install(void);

#endif /* RBPI4B_UTILITY_ROWS_H */
