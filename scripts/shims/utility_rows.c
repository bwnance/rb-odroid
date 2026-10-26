/*
 * utility_rows.c -- rows of the shim's own in rbp's UTILITY menu (utility_rows.h).
 */
#include "utility_rows.h"

#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "envutil.h"
#include "pointsrc.h"      /* pointsrc_log() */
#include "rbp_key.h"       /* is_rbp_process() */

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
#define UT_STRSET_OFF_ON    0x00513db8u   /* SLIP FLASHING's values: OFF, ON */
#define UTIL_SETLINE_MIXERMODE 0x0013b118u /* checked, not called */

#define MIRROR_FILE_DEFAULT "/run/rblive4/mirror.mode"
#define UT_LINE_LEN   32                  /* +32 the length, +34 the text */
#define UT_LINE_TEXT  34
#define UT_LINE_BYTES 512

/* "      HDMI AUDIO": six spaces, as rbp indents a row under its section */
static const uint16_t ut_label_mirror[] = {
    ' ', ' ', ' ', ' ', ' ', ' ', 'H', 'D', 'M', 'I', ' ', 'A', 'U', 'D', 'I', 'O', 0 };

static struct ut_table ut_table __attribute__((aligned(8)));

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

static const char *mirror_path(void)
{
    return env_str("AUDIO_MIRROR_FILE", MIRROR_FILE_DEFAULT);
}

/* 0 = OFF, 1 = ON; no file, or anything but "off", is ON (audioshim's rule). */
static int mirror_read(void)
{
    char buf[16];
    int fd = open(mirror_path(), O_RDONLY | O_CLOEXEC), n;

    if (fd < 0)
        return 1;
    n = (int)read(fd, buf, sizeof buf - 1);
    close(fd);
    if (n <= 0)
        return 1;
    buf[n] = '\0';
    return strncmp(buf, "off", 3) == 0 ? 0 : 1;
}

static int mirror_write(int on)
{
    const char *w = on ? "on\n" : "off\n";
    int fd = open(mirror_path(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644), ok;

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
        idx = mirror_read();
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
    if (mirror_write(v) != 0)
        pointsrc_log("utility: HDMI AUDIO -> %s, but %s could not be written",
                     v ? "ON" : "OFF", mirror_path());
    else
        pointsrc_log("utility: HDMI AUDIO -> %s (wrote %s)", v ? "ON" : "OFF",
                     mirror_path());
    return 1;
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
    struct ut_entry ours;
    int i, n;

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
        src[UT_ROW_MIXERMODE].fn[UT_FN_SETLINE] != UTIL_SETLINE_MIXERMODE)
        goto refuse;

    memset(&ours, 0, sizeof ours);
    ours.label = ut_label_mirror;
    ours.values = slip->values;
    ours.nvalues = 2;
    ours.cur = -1;
    ours.edit = -1;
    ours.editable = 1;
    ours.fn[0] = UTIL_INITITEM_DUMMY;
    ours.fn[UT_FN_SETLINE] = (uint32_t)(uintptr_t)ut_mirror_setline;
    ours.fn[2] = UTIL_STARTEDIT;
    ours.fn[3] = UTIL_SELMENU;
    ours.fn[4] = UTIL_ISMODIFIED;
    ours.fn[UT_FN_ENTER] = (uint32_t)(uintptr_t)ut_mirror_enter;
    ours.fn[6] = UTIL_ISGREY;
    ours.fn[7] = UTIL_RESET;

    n = ut_build(&ut_table, src, UT_RBP_N, UT_ROW_MIXERMODE, &ours, 1);
    if (n < 0)
        goto refuse;
    for (i = 0; i < 6; i++)
        if (patch_word(ut_lit_count[i], (uint32_t)(uintptr_t)&ut_table) != 0)
            goto failed;
    if (patch_word(ut_lit_rows, (uint32_t)(uintptr_t)ut_table.rows) != 0)
        goto failed;
    pointsrc_log("utility: %d rows (HDMI AUDIO after MIXER MODE), table at %p", n,
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
