/*
 * utility_rows.c -- rows of the shim's own in rbp's UTILITY menu (utility_rows.h).
 */
#include "utility_rows.h"

#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "envutil.h"
#include "pointsrc.h"      /* pointsrc_log() */
#include "rbp_key.h"       /* is_rbp_process() */
#include "settings_zone.h" /* ST_ROW_*, struct settings_state: the panel's rows and state */

/* ---- rbp's side, measured on the RX3 binary 2026-10-26 (docs/21) ---------- */
#define UT_RBP_COUNT     0x005140b8u      /* ulUtilityItemNum, the rows after it */
#define UT_RBP_ROWS      (UT_RBP_COUNT + 4)
#define UT_RBP_N         33
/* The literal-pool words that hold those two addresses -- the only way rbp's
 * code reaches the table (no movw/movt pair, no anchored base). */
static const uint32_t ut_lit_count[6] = {
    0x0013c9a8, 0x0013cbcc, 0x0013cd1c, 0x0013cf30, 0x0013cfc4, 0x0013d9ec };
static const uint32_t ut_lit_rows = 0x0013d9e4;
/* rbp's generic row handlers, as SLIP FLASHING (row 7) uses them */
#define UTIL_INITITEM_DUMMY 0x0013a924
#define UTIL_STARTEDIT      0x0013a928
#define UTIL_SELMENU        0x0013c360
#define UTIL_ISMODIFIED     0x0013a958
#define UTIL_ISGREY         0x0013a98c
#define UTIL_RESET          0x0013a99c
#define UT_ROW_SLIPFLASH    7
#define UT_ROW_MIXERMODE    23            /* ours go after it, at the MIXER section's end */
#define UT_ROW_JOGIMAGE     13            /* IMAGE ON JOG DISPLAY: the DECK section's last */
#define UT_ROW_BOOTHATT     19            /* BOOTH MONITOR ATTENUATOR */
#define UT_ROW_SCREENSAVER  29            /* SCREEN SAVER, in GENERAL */
#define UT_ROW_JOGLCD       28            /* JOG LCD BRIGHTNESS: the FLX10 has none */
#define UT_ROW_VERSION      32            /* VERSION No.: "Ver Can't get version." here */
#define UT_STRSET_OFF_ON    0x00513db8u   /* SLIP FLASHING's values: OFF, ON */
#define UTIL_SETLINE_MIXERMODE 0x0013b118u /* checked, not called */
#define UT_ROW_MIXERCAT     15            /* MIXER: its heading row, copied for ours */
#define UT_ROW_BLANK        24            /* the blank row before GENERAL, copied */
#define UTIL_SETLINE_MIXERCATEGORY 0x0013aae8u
#define UTIL_SETLINE_BLANK  0x0013a9b4u

#define MIRROR_FILE_DEFAULT "/run/rbp/mirror.mode"
#define TRAFFIC_FILE_DEFAULT "/run/rbp/browse.trafficlight"
#define UT_LINE_LEN   32                  /* +32 the length, +34 the text */
#define UT_LINE_TEXT  34
#define UT_LINE_BYTES 512

/* "      HDMI AUDIO": six spaces, as rbp indents a row under its section */
static const uint16_t ut_label_mirror[] = {
    ' ', ' ', ' ', ' ', ' ', ' ', 'H', 'D', 'M', 'I', ' ', 'A', 'U', 'D', 'I', 'O', 0 };
/* "      TRAFFIC LIGHT": BROWSE's KEY cell lit green on a row whose key matches the
 * MASTER's (browserows.c, which reads the file; operator, 2026-10-28) */
const uint16_t ut_label_traffic[] = {
    ' ', ' ', ' ', ' ', ' ', ' ', 'T', 'R', 'A', 'F', 'F', 'I', 'C', ' ', 'L', 'I', 'G', 'H',
    'T', 0 };

static struct ut_table ut_table __attribute__((aligned(8)));
static struct ut_table ut_deck;      /* rbp's rows with TRAFFIC LIGHT, before the rest */
static struct ut_table ut_mixer;     /* ...and the DDJ-FLX10's rows */
static struct ut_table ut_rest;      /* ...and HDMI AUDIO and the sections */

int ut_build(struct ut_table *dst, const struct ut_entry *src, int n, int after,
             const struct ut_entry *ours, int n_ours)
{
    int k = 0, i;

    if (n < 0 || n_ours < 0 || n + n_ours > UT_ROWS_MAX || after < -1 || after >= n)
        return -1;
    for (i = 0; i <= after; i++)
        dst->rows[k++] = src[i];
    for (i = 0; i < n_ours; i++)
        dst->rows[k++] = ours[i];
    for (i = after + 1; i < n; i++)
        dst->rows[k++] = src[i];
    dst->count = (uint32_t)k;
    return k;
}

/* The two OFF/ON rows share their handlers and differ in the file they keep. */
static const char *mirror_path(const struct ut_entry *e)
{
    if (e->label == ut_label_traffic)
        return env_str("BROWSE_TRAFFIC_FILE", TRAFFIC_FILE_DEFAULT);
    return env_str("AUDIO_MIRROR_FILE", MIRROR_FILE_DEFAULT);
}

static const char *row_name(const struct ut_entry *e)
{
    return e->label == ut_label_traffic ? "TRAFFIC LIGHT" : "HDMI AUDIO";
}

/* 0 = OFF, 1 = ON; no file, or anything but "off", is ON (audioshim's rule). */
static int mirror_read(const struct ut_entry *e)
{
    char buf[16];
    int fd = open(mirror_path(e), O_RDONLY | O_CLOEXEC), n;

    if (fd < 0)
        return 1;
    n = (int)read(fd, buf, sizeof buf - 1);
    close(fd);
    if (n <= 0)
        return 1;
    buf[n] = '\0';
    return strncmp(buf, "off", 3) == 0 ? 0 : 1;
}

static int mirror_write(const struct ut_entry *e, int on)
{
    const char *w = on ? "on\n" : "off\n";
    int fd = open(mirror_path(e), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644), ok;

    if (fd < 0)
        return -1;
    ok = write(fd, w, strlen(w)) == (ssize_t)strlen(w);
    close(fd);
    return ok ? 0 : -1;
}

static void line_put(unsigned char *line, const uint16_t *s)
{
    uint16_t n = 0;

    memset(line + UT_LINE_TEXT, 0, UT_LINE_BYTES);
    if (!s)
        return;
    while (s[n] && n < UT_LINE_BYTES / 2)
        n++;
    memcpy(line + UT_LINE_LEN, &n, sizeof n);
    memcpy(line + UT_LINE_TEXT, s, (size_t)n * 2);
}

int ut_mirror_setline(struct ut_entry *e, unsigned char **lines)
{
    int idx = e->edit;

    line_put(lines[0], e->label);
    if (idx == -1) {
        idx = mirror_read(e);
        e->cur = idx;
    }
    line_put(lines[1], idx >= 0 && (uint32_t)idx < (e->nvalues & 0xffff) ?
                       e->values[idx] : 0);
    return 0;
}

int ut_mirror_enter(struct ut_entry *e)
{
    int v = e->edit;

    if ((e->nvalues & 0xffff) <= (uint32_t)(uint16_t)v)
        return 0;
    if (v == e->cur)
        return 0;
    e->cur = v;
    if (mirror_write(e, v) != 0)
        pointsrc_log("utility: %s -> %s, but %s could not be written", row_name(e),
                     v ? "ON" : "OFF", mirror_path(e));
    else
        pointsrc_log("utility: %s -> %s (wrote %s)", row_name(e), v ? "ON" : "OFF",
                     mirror_path(e));
    return 1;
}

/* ---- the DJM and VNC sections ----------------------------------------------
 * The shim's drawn panel had MIXER ROUTE and the two DECK PAIR rows (an external
 * digital mixer, a DJM) and SHARE / ENCODE / POINTER INJECT (the screen viewer),
 * each group behind its own config value; in UTILITY they are two sections of
 * their own, each a blank row, a heading and its rows (operator, 2026-10-26), and
 * still only with RB_SETTINGS_DJM=1 / RB_SETTINGS_VNC=1. A UTILITY row CHOOSES a
 * value from a list where the panel's row cycled to the next, so each row here is
 * its list, the index the unit is on now (the panel's own state,
 * pointsrc_settings_state()) and the word a choice writes (the panel's own file,
 * pointsrc_settings_write()). */
#define W(...) ((const uint16_t[]){ __VA_ARGS__, 0 })
static const uint16_t *const v_mixer[]   = { W('I','N','T','E','R','N','A','L'),
                                             W('E','X','T','E','R','N','A','L') };
static const uint16_t *const v_offon[]   = { W('O','F','F'), W('O','N') };
static const uint16_t *const v_encode[]  = { W('R','A','W'), W('H','W','J','P','E','G') };
static const uint16_t *const v_pairs[]   = { W('0','-','1'), W('2','-','3'), W('4','-','5'),
                                             W('6','-','7') };
static const int pair_a[4] = { 0, 2, 4, 6 };

#define SP6 ' ', ' ', ' ', ' ', ' ', ' '
static const uint16_t l_djm[]     = { 'D', 'J', 'M', 0 };
static const uint16_t l_vnc[]     = { 'V', 'N', 'C', 0 };
static const uint16_t l_route[]   = { SP6, 'M','I','X','E','R',' ','R','O','U','T','E', 0 };
static const uint16_t l_deck1[]   = { SP6, 'D','E','C','K',' ','1',' ','P','A','I','R', 0 };
static const uint16_t l_deck2[]   = { SP6, 'D','E','C','K',' ','2',' ','P','A','I','R', 0 };
static const uint16_t l_share[]   = { SP6, 'S','H','A','R','E', 0 };
static const uint16_t l_encode[]  = { SP6, 'E','N','C','O','D','E', 0 };
static const uint16_t l_pointer[] = { SP6, 'P','O','I','N','T','E','R',' ','I','N','J','E','C','T', 0 };

struct ut_def {
    const uint16_t *label;
    int st_row;                          /* the panel's row number (settings_zone.h) */
    const uint16_t *const *values;
    int nvalues;
};
static const struct ut_def ut_defs[] = {
    { l_route,   ST_ROW_MIXER,   v_mixer,  2 },
    { l_deck1,   ST_ROW_DECK1,   v_pairs,  4 },
    { l_deck2,   ST_ROW_DECK2,   v_pairs,  4 },
    { l_share,   ST_ROW_SHARE,   v_offon,  2 },
    { l_encode,  ST_ROW_ENCODE,  v_encode, 2 },
    { l_pointer, ST_ROW_POINTER, v_offon,  2 },
};

static const struct ut_def *ut_def_of(const struct ut_entry *e)
{
    for (unsigned i = 0; i < sizeof ut_defs / sizeof ut_defs[0]; i++)
        if (ut_defs[i].label == e->label)
            return &ut_defs[i];
    return NULL;
}

/* The pair deck `slot` is on, the shim's own default for a state not read (the panel's
 * deck_pair_of(), settings_zone.c). */
static void ut_deck_pair(const struct settings_state *S, int slot, int *a, int *b)
{
    *a = S->deck_a[slot];
    *b = S->deck_b[slot];
    if (*a == 0 && *b == 0) {
        *a = slot ? 4 : 2;
        *b = slot ? 5 : 3;
    }
}

/* The value index the unit is on now, -1 for a deck on a pair the list does not hold. */
int ut_row_current(int st_row, const struct settings_state *S)
{
    int a, b;

    switch (st_row) {
    case ST_ROW_MIXER:   return S->mixer_present && S->mixer_external ? 1 : 0;
    case ST_ROW_SHARE:   return S->share_on ? 1 : 0;
    case ST_ROW_ENCODE:  return S->encode_hw ? 1 : 0;
    case ST_ROW_POINTER: return S->pointer_on ? 1 : 0;
    case ST_ROW_DECK1:
    case ST_ROW_DECK2:
        ut_deck_pair(S, st_row == ST_ROW_DECK2, &a, &b);
        for (int i = 0; i < 4; i++)
            if (pair_a[i] == a && b == a + 1)
                return i;
        return -1;
    }
    return -1;
}

/* The word choosing index `v` writes, into `out`; NULL when the choice is refused -- the
 * mixer route with no mixer on the bus (the panel's "refuse the tap"), a pair past the
 * card's width, or the pair the other deck is on (the panel's rule: two decks on one pair
 * would sum them). A deck word names both decks, as the panel's does (settings_zone.c's
 * deck_pair_next: the live map is authoritative for what it names). */
const char *ut_row_word(int st_row, int v, const struct settings_state *S, char *out, int max)
{
    int slot, oa, ob;

    switch (st_row) {
    case ST_ROW_MIXER:
        if (!S->mixer_present)
            return NULL;
        return v ? "external" : "internal";
    case ST_ROW_SHARE:   return v ? "on" : "off";
    case ST_ROW_ENCODE:  return v ? "hwjpeg" : "raw";
    case ST_ROW_POINTER: return v ? "on" : "off";
    case ST_ROW_DECK1:
    case ST_ROW_DECK2:
        slot = st_row == ST_ROW_DECK2;
        if (v < 0 || v > 3)
            return NULL;
        if (S->audio_channels > 0 && pair_a[v] + 1 >= S->audio_channels)
            return NULL;
        ut_deck_pair(S, !slot, &oa, &ob);
        if (oa == pair_a[v] && ob == pair_a[v] + 1)
            return NULL;
        snprintf(out, (size_t)max, "deck%d=%d,%d;deck%d=%d,%d", slot + 1, pair_a[v],
                 pair_a[v] + 1, !slot + 1, oa, ob);
        return out;
    }
    return NULL;
}

static int ut_gen_setline(struct ut_entry *e, unsigned char **lines)
{
    const struct ut_def *d = ut_def_of(e);
    struct settings_state S;
    int idx = e->edit;

    line_put(lines[0], e->label);
    if (d && idx == -1) {
        memset(&S, 0, sizeof S);
        pointsrc_settings_state(&S);
        idx = ut_row_current(d->st_row, &S);
        e->cur = idx;
    }
    line_put(lines[1], d && idx >= 0 && idx < d->nvalues ? d->values[idx] : 0);
    return 0;
}

static int ut_gen_enter(struct ut_entry *e)
{
    const struct ut_def *d = ut_def_of(e);
    struct settings_state S;
    const char *word;
    char buf[48];
    int v = e->edit;

    if (!d || (e->nvalues & 0xffff) <= (uint32_t)(uint16_t)v || v == e->cur)
        return 0;
    memset(&S, 0, sizeof S);
    pointsrc_settings_state(&S);
    word = ut_row_word(d->st_row, v, &S, buf, sizeof buf);
    if (!word || pointsrc_settings_write(d->st_row, word) != 0) {
        pointsrc_log("utility: '%s' -> value %d refused", settings_row_label(d->st_row), v);
        return 0;
    }
    e->cur = v;
    return 1;
}

/* Grey: the mixer route with no recognised mixer on the bus, as the panel dims it. */
static int ut_gen_isgrey(struct ut_entry *e)
{
    const struct ut_def *d = ut_def_of(e);
    struct settings_state S;

    if (!d || d->st_row != ST_ROW_MIXER)
        return 0;
    memset(&S, 0, sizeof S);
    pointsrc_settings_state(&S);
    return !S.mixer_present;
}

/* ---- the DDJ-FLX10's own audio settings -------------------------------------
 * What rekordbox sets on the unit and rbp has no row for (operator, 2026-10-28):
 * master and booth mono / stereo, the master's peak limiter, the mic to the master.
 * Kept on this machine and sent to the unit at every start (map_flx10.c's
 * flx10_setting_get / flx10_setting_set, knobshim.so: WEAK, greyed without it).
 * Index 1 is the note at 7F. */
__attribute__((weak)) int flx10_setting_get(int num);
__attribute__((weak)) int flx10_setting_set(int num, int val);

static const uint16_t *const v_monostereo[] = { W('M','O','N','O'), W('S','T','E','R','E','O') };
static const uint16_t l_master_st[] = { SP6, 'M','A','S','T','E','R',' ','M','O','N','O','/',
                                        'S','T','E','R','E','O', 0 };
static const uint16_t l_booth_st[]  = { SP6, 'B','O','O','T','H',' ','M','O','N','O','/',
                                        'S','T','E','R','E','O', 0 };
static const uint16_t l_peak[]      = { SP6, 'P','E','A','K',' ','L','I','M','I','T','E','R', 0 };
static const uint16_t l_mic_master[] = { SP6, 'M','I','C',' ','O','U','T',' ','T','O',' ',
                                         'M','A','S','T','E','R', 0 };
/* the unit's demo / screen saver, CC 0x44 = index x 0x10 (operator, 2026-10-28) */
static const uint16_t *const v_demo[] = {
    W('D','I','S','A','B','L','E'), W('D','E','M','O',' ','1','m','i','n'),
    W('D','E','M','O',' ','5','m','i','n'), W('D','E','M','O',' ','1','0','m','i','n'),
    W('S','C','R','E','E','N',' ','S','A','V','E','R') };
static const uint16_t l_demo[] = { SP6, 'F','L','X','1','0',' ','D','E','M','O','/',
                                   'S','C','R','E','E','N',' ','S','A','V','E','R', 0 };
/* step 0: a note, index 1 = 7F; otherwise the value is index x step */
static const struct {
    const uint16_t *label; int num; const uint16_t *const *values; int nvalues, step;
} unit_defs[] = {
    { l_master_st,  0x47, v_monostereo, 2, 0 },
    { l_booth_st,   0x48, v_monostereo, 2, 0 },
    { l_peak,       0x49, v_offon,      2, 0 },
    { l_mic_master, 0x4A, v_offon,      2, 0 },
    { l_demo,       0x44, v_demo,       5, 0x10 },   /* in GENERAL, not MIXER */
};
#define UNIT_ROWS (int)(sizeof unit_defs / sizeof unit_defs[0])
#define UNIT_MIXER_ROWS 4

static int unit_def_of(const struct ut_entry *e)
{
    for (int i = 0; i < UNIT_ROWS; i++)
        if (unit_defs[i].label == e->label)
            return i;
    return -1;
}

/* The row's index for the kept value, -1 without knobshim's map. */
static int unit_index(const struct ut_entry *e)
{
    int d = unit_def_of(e), v = d >= 0 && flx10_setting_get ? flx10_setting_get(unit_defs[d].num) : -1;
    if (v < 0)
        return -1;
    if (!unit_defs[d].step)
        return v ? 1 : 0;
    return v / unit_defs[d].step < unit_defs[d].nvalues ? v / unit_defs[d].step : -1;
}

static int ut_unit_setline(struct ut_entry *e, unsigned char **lines)
{
    int idx = e->edit;

    line_put(lines[0], e->label);
    if (idx == -1)
        e->cur = idx = unit_index(e);
    line_put(lines[1], idx >= 0 && (uint32_t)idx < (e->nvalues & 0xffff) ? e->values[idx] : 0);
    return 0;
}

static int ut_unit_enter(struct ut_entry *e)
{
    int v = e->edit, d = unit_def_of(e), val;

    if (d < 0 || v < 0 || v >= unit_defs[d].nvalues || v == e->cur)
        return 0;
    val = unit_defs[d].step ? v * unit_defs[d].step : v ? 0x7F : 0x00;
    if (!flx10_setting_set || !flx10_setting_set(unit_defs[d].num, val)) {
        pointsrc_log("utility: DDJ-FLX10 setting 0x%02X -> %d not kept", unit_defs[d].num, v);
        return 0;
    }
    e->cur = v;
    return 1;
}

static int ut_unit_isgrey(struct ut_entry *e)
{
    return unit_index(e) < 0;
}

/* ---- BEAT FX QUANTIZE, in MIXER (operator, 2026-10-28) ------------------------------
 * The touch panel's own switch as a row: it shows rbp's setting (UiGetMixBeatFxQuantizeOn,
 * which a stick's MY SETTINGS sets when it mounts) and a choice goes through the panel's
 * key (knobshim's flx10_bfxq_set), so rbp stores it as a tap would. */
#define UI_GET_BFX_QUANTIZE 0x000fe4e0u
#define UI_SET_BFX_QUANTIZE 0x000fe6b8u
__attribute__((weak)) int flx10_bfxq_set(int on);
static const uint16_t l_bfxq[] = { SP6, 'B','E','A','T',' ','F','X',' ','Q','U','A','N','T','I',
                                   'Z','E', 0 };

static int ut_bfxq_setline(struct ut_entry *e, unsigned char **lines)
{
    int idx = e->edit;

    line_put(lines[0], e->label);
    if (idx == -1)
        e->cur = idx = ((int (*)(void))(uintptr_t)UI_GET_BFX_QUANTIZE)() ? 1 : 0;
    line_put(lines[1], idx >= 0 && idx < 2 ? e->values[idx] : 0);
    return 0;
}

static int ut_bfxq_enter(struct ut_entry *e)
{
    int v = e->edit;

    if (v < 0 || v > 1 || v == e->cur)
        return 0;
    if (!flx10_bfxq_set || !flx10_bfxq_set(v))
        ((void (*)(int))(uintptr_t)UI_SET_BFX_QUANTIZE)(v);
    pointsrc_log("utility: BEAT FX QUANTIZE -> %s", v ? "ON" : "OFF");
    e->cur = v;
    return 1;
}

/* VERSION No.'s value: "Ver " and VERSION_TEXT (operator, 2026-10-28), ASCII. */
static uint16_t version_text[40];

static int ut_version_setline(struct ut_entry *e, unsigned char **lines)
{
    line_put(lines[0], e->label);
    line_put(lines[1], version_text);
    return 0;
}

static void version_init(void)
{
    const char *v = env_str("VERSION_TEXT", "69.420");
    unsigned n = 0;

    for (const char *p = "Ver "; *p; p++)
        version_text[n++] = (uint16_t)*p;
    for (; *v && n < sizeof version_text / sizeof version_text[0] - 1; v++)
        version_text[n++] = (uint16_t)(unsigned char)*v;
    version_text[n] = 0;
}

/* ---- rbp's SCREEN SAVER: off, and not offered (operator, 2026-10-28) ---------
 * On the RX3 it dims the LCD's backlight (LcdBacklightPwm::setScreenSaverOn), which
 * this machine does not have. The row is left out of the table, and the setting is
 * put to OFF whenever it reads ON -- rbp saves that in XdjSettings.dat, so once is
 * enough. UiGetUtilScreenSaver answers 0 until rbp's StatWatcher is up, so the
 * watch runs through rbp's start. */
#define UI_GET_SCREENSAVER 0x000feea0u
#define UI_SET_SCREENSAVER 0x000feec4u
#define SCREENSAVER_WATCH_S 120

static void *screensaver_off(void *arg)
{
    (void)arg;
    for (int i = 0; i < SCREENSAVER_WATCH_S; i++) {
        sleep(1);
        if (((int (*)(void))(uintptr_t)UI_GET_SCREENSAVER)() != 1)
            continue;
        ((void (*)(int))(uintptr_t)UI_SET_SCREENSAVER)(0);
        pointsrc_log("utility: SCREEN SAVER was ON -- put to OFF");
    }
    return NULL;
}

/* A section: rbp's blank row, MIXER's heading with our label, then the rows of `defs`.
 * Answers how many entries it wrote. */
static int ut_section(struct ut_entry *out, const struct ut_entry *src, const uint16_t *heading,
                      const struct ut_def *defs, int n, const struct ut_entry *model)
{
    int k = 0;

    out[k++] = src[UT_ROW_BLANK];
    out[k] = src[UT_ROW_MIXERCAT];
    out[k++].label = heading;
    for (int i = 0; i < n; i++) {
        out[k] = *model;
        out[k].label = defs[i].label;
        out[k].values = defs[i].values;
        out[k].nvalues = (uint32_t)defs[i].nvalues;
        out[k].fn[UT_FN_SETLINE] = (uint32_t)(uintptr_t)ut_gen_setline;
        out[k].fn[UT_FN_ENTER] = (uint32_t)(uintptr_t)ut_gen_enter;
        if (defs[i].st_row == ST_ROW_MIXER)
            out[k].fn[UT_FN_ISGREY] = (uint32_t)(uintptr_t)ut_gen_isgrey;
        k++;
    }
    return k;
}

/* Is rbp's own text mapped where it was measured? rbp runs under ld-linux.so.3,
 * so /proc/self/exe is the loader; the maps line for the binary is the check,
 * and it is what makes the reads below safe in any process that gets here. */
static int rbp_text_mapped(void)
{
    char line[256];
    FILE *f = fopen("/proc/self/maps", "re");
    int ok = 0;

    if (!f)
        return 0;
    while (!ok && fgets(line, sizeof line, f)) {
        unsigned long lo, hi;
        const char *name = strrchr(line, '/');
        if (sscanf(line, "%lx-%lx", &lo, &hi) == 2 && name && !strcmp(name, "/rbp\n") &&
            lo <= 0x0013c9a8ul && hi > 0x0013d9ecul)
            ok = 1;
    }
    fclose(f);
    return ok;
}

/* One literal word, rewritten under a writable mapping of its page. */
static int patch_word(uint32_t addr, uint32_t val)
{
    long pg = sysconf(_SC_PAGESIZE);
    uintptr_t page = (uintptr_t)addr & ~((uintptr_t)pg - 1);

    if (mprotect((void *)page, (size_t)pg, PROT_READ | PROT_WRITE | PROT_EXEC) != 0)
        return -1;
    *(volatile uint32_t *)(uintptr_t)addr = val;
    __builtin___clear_cache((char *)(uintptr_t)addr, (char *)(uintptr_t)addr + 4);
    mprotect((void *)page, (size_t)pg, PROT_READ | PROT_EXEC);
    return 0;
}

int utility_rows_install(void)
{
    const struct ut_entry *src = (const struct ut_entry *)(uintptr_t)UT_RBP_ROWS;
    const struct ut_entry *slip;
    struct ut_entry ours[2 + 2 * 5];
    int i, n, k;

    if (!is_rbp_process() || !rbp_text_mapped())
        return -1;
    if (!atoi(env_str("UTILITY_ROWS", "1")))
        return -1;
    /* Everything it relies on, checked before anything is changed. */
    for (i = 0; i < 6; i++)
        if (*(volatile uint32_t *)(uintptr_t)ut_lit_count[i] != UT_RBP_COUNT)
            goto refuse;
    if (*(volatile uint32_t *)(uintptr_t)ut_lit_rows != UT_RBP_ROWS ||
        *(volatile uint32_t *)(uintptr_t)UT_RBP_COUNT != UT_RBP_N)
        goto refuse;
    slip = &src[UT_ROW_SLIPFLASH];
    if (slip->fn[0] != UTIL_INITITEM_DUMMY || slip->fn[2] != UTIL_STARTEDIT ||
        slip->fn[3] != UTIL_SELMENU || slip->fn[4] != UTIL_ISMODIFIED ||
        slip->fn[6] != UTIL_ISGREY || slip->fn[7] != UTIL_RESET ||
        (uintptr_t)slip->values != UT_STRSET_OFF_ON || slip->nvalues != 2 ||
        src[UT_ROW_MIXERMODE].fn[UT_FN_SETLINE] != UTIL_SETLINE_MIXERMODE ||
        src[UT_ROW_MIXERCAT].fn[UT_FN_SETLINE] != UTIL_SETLINE_MIXERCATEGORY ||
        src[UT_ROW_BLANK].fn[UT_FN_SETLINE] != UTIL_SETLINE_BLANK)
        goto refuse;

    memset(ours, 0, sizeof ours);
    ours[0].label = ut_label_mirror;
    ours[0].values = slip->values;
    ours[0].nvalues = 2;
    ours[0].cur = -1;
    ours[0].edit = -1;
    ours[0].editable = 1;
    ours[0].fn[0] = UTIL_INITITEM_DUMMY;
    ours[0].fn[UT_FN_SETLINE] = (uint32_t)(uintptr_t)ut_mirror_setline;
    ours[0].fn[2] = UTIL_STARTEDIT;
    ours[0].fn[3] = UTIL_SELMENU;
    ours[0].fn[4] = UTIL_ISMODIFIED;
    ours[0].fn[UT_FN_ENTER] = (uint32_t)(uintptr_t)ut_mirror_enter;
    ours[0].fn[UT_FN_ISGREY] = UTIL_ISGREY;
    ours[0].fn[7] = UTIL_RESET;
    /* TRAFFIC LIGHT ends the DECK section (operator, 2026-10-28: not under MIXER) */
    struct ut_entry traffic = ours[0];
    traffic.label = ut_label_traffic;
    /* the DDJ-FLX10's own settings: in MIXER after the two attenuators (operator,
     * 2026-10-28) */
    struct ut_entry unit_rows[UNIT_ROWS];
    for (i = 0; i < UNIT_ROWS; i++) {
        unit_rows[i] = ours[0];
        unit_rows[i].label = unit_defs[i].label;
        unit_rows[i].values = unit_defs[i].values;
        unit_rows[i].nvalues = (uint32_t)unit_defs[i].nvalues;
        unit_rows[i].fn[UT_FN_SETLINE] = (uint32_t)(uintptr_t)ut_unit_setline;
        unit_rows[i].fn[UT_FN_ENTER] = (uint32_t)(uintptr_t)ut_unit_enter;
        unit_rows[i].fn[UT_FN_ISGREY] = (uint32_t)(uintptr_t)ut_unit_isgrey;
    }
    /* BEAT FX QUANTIZE before HDMI AUDIO, at MIXER's end */
    ours[1] = ours[0];
    ours[0].label = l_bfxq;
    ours[0].values = v_offon;
    ours[0].fn[UT_FN_SETLINE] = (uint32_t)(uintptr_t)ut_bfxq_setline;
    ours[0].fn[UT_FN_ENTER] = (uint32_t)(uintptr_t)ut_bfxq_enter;
    ours[0].fn[UT_FN_ISGREY] = UTIL_ISGREY;
    k = 2;
    if (atoi(env_str("SETTINGS_DJM", "0")))
        k += ut_section(ours + k, src, l_djm, ut_defs, 3, &ours[1]);
    if (atoi(env_str("SETTINGS_VNC", "0")))
        k += ut_section(ours + k, src, l_vnc, ut_defs + 3, 3, &ours[1]);

    n = ut_build(&ut_deck, src, UT_RBP_N, UT_ROW_JOGIMAGE, &traffic, 1);
    if (n < 0)
        goto refuse;
    n = ut_build(&ut_mixer, ut_deck.rows, n, UT_ROW_BOOTHATT + 1, unit_rows, UNIT_MIXER_ROWS);
    if (n < 0)
        goto refuse;
    n = ut_build(&ut_rest, ut_mixer.rows, n, UT_ROW_MIXERMODE + 1 + UNIT_MIXER_ROWS, ours, k);
    if (n < 0)
        goto refuse;
    /* the demo / screen saver where rbp's SCREEN SAVER was, in GENERAL */
    n = ut_build(&ut_table, ut_rest.rows, n, UT_ROW_SCREENSAVER + 1 + UNIT_MIXER_ROWS + k,
                 unit_rows + UNIT_MIXER_ROWS, UNIT_ROWS - UNIT_MIXER_ROWS);
    if (n < 0)
        goto refuse;
    version_init();
    for (i = 0; i < n; i++)
        if (ut_table.rows[i].label == src[UT_ROW_VERSION].label)
            ut_table.rows[i].fn[UT_FN_SETLINE] = (uint32_t)(uintptr_t)ut_version_setline;
    /* SCREEN SAVER and JOG LCD BRIGHTNESS left out (operator, 2026-10-28: the FLX10's
     * jog screens take no brightness from it) */
    for (i = k = 0; i < n; i++)
        if (ut_table.rows[i].label != src[UT_ROW_SCREENSAVER].label &&
            ut_table.rows[i].label != src[UT_ROW_JOGLCD].label)
            ut_table.rows[k++] = ut_table.rows[i];
    n = k;
    ut_table.count = (uint32_t)n;
    for (i = 0; i < 6; i++)
        if (patch_word(ut_lit_count[i], (uint32_t)(uintptr_t)&ut_table) != 0)
            goto failed;
    if (patch_word(ut_lit_rows, (uint32_t)(uintptr_t)ut_table.rows) != 0)
        goto failed;
    {
        pthread_t th;
        if (pthread_create(&th, NULL, screensaver_off, NULL) == 0)
            pthread_detach(th);
    }
    pointsrc_log("utility: %d rows (SCREEN SAVER left out, TRAFFIC LIGHT after IMAGE ON JOG DISPLAY, the DDJ-FLX10's "
                 "four after BOOTH MONITOR ATTENUATOR, HDMI AUDIO after MIXER MODE, its demo "
                 "in SCREEN SAVER's place%s%s), table at %p", n,
                 atoi(env_str("SETTINGS_DJM", "0")) ? ", the DJM section" : "",
                 atoi(env_str("SETTINGS_VNC", "0")) ? ", the VNC section" : "",
                 (void *)&ut_table);
    return 0;

refuse:
    pointsrc_log("utility: rbp's UTILITY table is not as measured -- left alone");
    return -1;
failed:
    /* mprotect refused partway: put back what was changed, so rbp reads one table */
    for (i = 0; i < 6; i++)
        if (*(volatile uint32_t *)(uintptr_t)ut_lit_count[i] != UT_RBP_COUNT)
            patch_word(ut_lit_count[i], UT_RBP_COUNT);
    pointsrc_log("utility: could not make rbp's code writable -- left alone");
    return -1;
}

__attribute__((constructor)) static void utility_rows_init(void)
{
    utility_rows_install();
}
