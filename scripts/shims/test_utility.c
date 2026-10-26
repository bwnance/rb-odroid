/*
 * test_utility.c -- utility_rows.c: the bigger UTILITY table and the HDMI AUDIO
 * row's handlers, as rbp calls them.
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "utility_rows.h"

static int checks, failures;
#define CHECK(cond, ...) do { checks++; if (!(cond)) { failures++;           \
        printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__);      \
        printf("\n"); } } while (0)

/* what utility_rows.c needs from the rest of fbshim.so */
int is_rbp_process(void) { return 0; }
void pointsrc_log(const char *fmt, ...) { (void)fmt; }

static const uint16_t w_off[] = { 'O', 'F', 'F', 0 }, w_on[] = { 'O', 'N', 0 };
static const uint16_t *const offon[2] = { w_off, w_on };

static int line_is(const unsigned char *line, const char *s)
{
    uint16_t n, c;
    memcpy(&n, line + 32, 2);
    if (n != strlen(s))
        return 0;
    for (int i = 0; i < n; i++) {
        memcpy(&c, line + 34 + 2 * i, 2);
        if (c != (unsigned char)s[i])
            return 0;
    }
    return 1;
}

static void file_put(const char *path, const char *s)
{
    FILE *f = fopen(path, "w");
    fputs(s, f);
    fclose(f);
}

static int file_is(const char *path, const char *s)
{
    char buf[16] = { 0 };
    FILE *f = fopen(path, "r");
    if (!f)
        return 0;
    if (!fgets(buf, sizeof buf, f))
        buf[0] = 0;
    fclose(f);
    return !strcmp(buf, s);
}

int main(void)
{
    /* the table: rbp's rows with ours after row `after`, count updated */
    {
        static struct ut_entry src[33], ours[2];
        static struct ut_table t;
        int n, ok = 1;

        for (int i = 0; i < 33; i++)
            src[i].cur = i;
        ours[0].cur = 100;
        ours[1].cur = 101;
        n = ut_build(&t, src, 33, 23, ours, 1);
        CHECK(n == 34 && t.count == 34, "33 + 1 rows (got %d)", n);
        for (int i = 0; i <= 23; i++)
            ok &= t.rows[i].cur == i;
        CHECK(ok && t.rows[24].cur == 100, "rows 0..23 as they were, ours at 24");
        ok = 1;
        for (int i = 25; i < 34; i++)
            ok &= t.rows[i].cur == i - 1;
        CHECK(ok, "the rest after ours, in order");
        CHECK(ut_build(&t, src, 33, 32, ours, 2) == 35 && t.rows[33].cur == 100 &&
              t.rows[34].cur == 101, "two at the end");
        CHECK(ut_build(&t, src, 33, 33, ours, 1) == -1 &&
              ut_build(&t, src, 33, 0, ours, UT_ROWS_MAX) == -1,
              "past the rows, or more than fit: refused");
        CHECK(sizeof(struct ut_entry) == 56 && sizeof(void *) == 4,
              "a row is rbp's 56 bytes (%u)", (unsigned)sizeof(struct ut_entry));
    }

    /* the HDMI AUDIO row, as rbp calls it */
    {
        static const uint16_t label[] = { ' ', ' ', 'H', 'D', 'M', 'I', 0 };
        static unsigned char l0[600], l1[600];
        unsigned char *lines[2] = { l0, l1 };
        char path[] = "/tmp/test_utility_XXXXXX";
        struct ut_entry e;
        int fd = mkstemp(path);

        close(fd);
        unlink(path);
        setenv("AUDIO_MIRROR_FILE", path, 1);
        memset(&e, 0, sizeof e);
        e.label = label;
        e.values = offon;
        e.nvalues = 2;
        e.cur = e.edit = -1;

        memset(l0, 0xee, sizeof l0);
        CHECK(ut_mirror_setline(&e, lines) == 0 && line_is(l0, "  HDMI") &&
              line_is(l1, "ON") && e.cur == 1, "no file: the row reads ON");
        CHECK(l0[34 + 12] == 0 && l0[34 + 511] == 0, "the rest of the label line is cleared");
        file_put(path, "off\n");
        ut_mirror_setline(&e, lines);
        CHECK(line_is(l1, "OFF") && e.cur == 0, "\"off\" in the file: OFF");
        e.edit = 1;
        ut_mirror_setline(&e, lines);
        CHECK(line_is(l1, "ON") && e.cur == 0, "while choosing: the choice, not the file");
        CHECK(ut_mirror_enter(&e) == 1 && e.cur == 1 && file_is(path, "on\n"),
              "ENTER on ON writes on, answers changed");
        CHECK(ut_mirror_enter(&e) == 0 && file_is(path, "on\n"), "the same value again: no change");
        e.edit = 0;
        CHECK(ut_mirror_enter(&e) == 1 && file_is(path, "off\n"), "...and OFF writes off");
        e.edit = -1;
        CHECK(ut_mirror_enter(&e) == 0, "nothing chosen: nothing applied");
        e.edit = 2;
        CHECK(ut_mirror_enter(&e) == 0 && file_is(path, "off\n"), "a value past the list: refused");
        unlink(path);
    }

    printf("%s: %d checks, %d failures\n", failures ? "FAIL" : "ok", checks, failures);
    return failures ? 1 : 0;
}
