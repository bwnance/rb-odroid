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
/* ...and the rest of the stick's MY SETTINGS > Column (rbp's own second column under
 * each sort, read off the unit 2026-10-28; docs/21 item 11) */
#define BROWSE_FIELD_GENRE    6u
#define BROWSE_FIELD_LABEL    14u
#define BROWSE_FIELD_BITRATE  16u
#define BROWSE_FIELD_COLOR    19u
#define BROWSE_FIELD_COMMENTS 35u
#define BROWSE_FIELD_ORIGINAL 40u      /* ORIGINAL ARTIST */
#define BROWSE_FIELD_REMIXER  41u
#define BROWSE_FIELD_PLAYS    42u      /* DJ PLAY COUNT */
#define BROWSE_FIELD_ADDED    46u      /* DATE ADDED */
#define BROWSE_FIELD_RATING   100u

struct browse_column {
    unsigned int field;             /* BROWSE_FIELD_* */
    const uint16_t *caption;        /* the heading, UTF-16 */
};

/* Every hook the column needs, once, for good; until a column is set they pass
 * everything through. 0 = in. */
int browserows_install(void);
/* UTILITY's TRAFFIC LIGHT: a row whose key matches the MASTER's lights its KEY
 * cell green (on), or not. */
void browserows_traffic_light(int on);
/* The CDJ's PREVIEW: the list's waveform column shown (on) or taken out. */
void browserows_preview(int on);
/* Is (x, y) on the PREVIEW button (shown with a track list)? Are the waveforms shown? */
int browserows_preview_button(int x, int y);
int browserows_preview_shown(void);
/* A stick went in, out or was re-exported: forget every value read from one. */
void browserows_forget(void);
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
/* Track `track`'s entry in playlist `playlist` on the stick behind rbp's drive `drive`
 * (2 = USB1, 3 = USB2), from its export.pdb; 0 while not read yet (a thread reads it). */
unsigned int browsecol_playlist_number(unsigned int drive, unsigned int playlist,
                                       unsigned int track);
/* One page of export.pdb's playlist entries (table 8: entry, track, playlist), each row
 * of `playlist` handed to `found`. Pure, for the test. */
void browsecol_page_entries(const uint8_t *page, unsigned int size, unsigned int playlist,
                            void (*found)(unsigned int entry, unsigned int track, void *arg),
                            void *arg);
/* COLUMN in LIST MENU, beside SORT; once. 0 = in. */
int browsemenu_install(void);
/* LIST MENU as rbp just built it in `context` gets COLUMN as its third entry;
 * answers 1 if it did. */
int browsemenu_add_column(uint8_t *context);
/* COLUMN's own menu, OFF..TIME, the current one checked, into `context`. */
void browsemenu_fill(uint8_t *context, unsigned int first, const char *current);
/* The word of that menu's item `item`, or NULL. */
const char *browsemenu_word(unsigned int item);

/* ---- the headings' sort (browserows.c draws, browsesort.c asks) ------------ */
/* The sort a tap at screen (x, y) on a heading asks for, or -1. */
int browserows_heading_sort(int x, int y);
/* ...given the list's sort `now` and the heading's `sort`. Pure. */
unsigned int browserows_sort_for_tap(unsigned int now, unsigned int sort);
/* A heading's RGB16 pixel recoloured for the sorted heading's blue copy. Pure. */
unsigned int browserows_blue_pixel(unsigned int p);
/* ...with any ground and text colours (0xRRGGBB). Pure. */
unsigned int browserows_recolour_pixel(unsigned int p, unsigned int ground, unsigned int text);
/* Tap-to-sort, once. 0 = in. */
int browsesort_install(void);
/* SelectRightMenu's item 0x7e00|n is a tap's sort: 1 = taken, its answer in *result. */
int browsesort_select(uint8_t *context, void *mbx, uint8_t *request, int *result);
/* A press, drag or release from the touchscreen (pointsrc.c): 1 = a heading took it. */
int browse_heading_touch(int down, int x, int y);
void browsesort_request(unsigned int sort);
/* A few times a second from the chooser's tick: the test tap file. */
void browsesort_poll(void);

/* ---- for the test --------------------------------------------------------- */
/* rbp's track-key lookup the KEY column calls, reassignable */
extern unsigned int (*browserows_track_key)(void *db, unsigned int id);
/* ...and its field lookup (a remote player's rows: BPM and the chosen field) */
extern int (*browserows_values)(void *db, unsigned int id, unsigned int key, unsigned int field,
                                uint16_t *out);
/* The rewrite itself, on rbp's row message, after GetRightBrowseOne2One built it. */
void browserows_process(uint8_t *context, uint8_t *message, const struct browse_column *column);
/* "12A"-style text for rbp key 1..24; empty otherwise. */
void browserows_key_text(unsigned int key, uint16_t *out);
/* A row entry with the column's `value` appended behind noncharacters; answers
 * its size. browserows_split() takes one apart. */
unsigned int browserows_append(uint8_t *dest, const uint8_t *entry, const uint16_t *value);
int browserows_split(const uint16_t *s, unsigned int n, unsigned int *split, unsigned int *extra);

#endif /* RBPI4B_BROWSEROWS_H */
