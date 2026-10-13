/*
 * rbp_load.c -- load a track onto a deck by its DATABASE ID, over a file.
 *
 * ===========================================================================
 * THE DOOR
 * ===========================================================================
 *
 * rbp's LOAD key does not carry a track.  `K_LOAD` (0x4311) reaches
 * `BrowseLoadItemEnter`, whose only argument is the DECK: the track is read out
 * of rbp's own browse state as `getInitialNo() + getCursorNo()` -- a position in
 * whatever list happens to be up -- and posted as mailbox message 0x712.  That
 * is why a keycode can never name a track, and why the touch route needs the
 * browse screen, a search and a row selection to get anywhere.
 *
 * There is a second message.  **0x40e carries a MusicID** (24-bit hi at +36, low
 * at +40) and loads it onto the deck at +68.  And there is a supported function
 * that builds it, which is what this file calls:
 *
 *     CmnFunc_CmnInfo_Local2CdjMusicDragAndDropHandler(deck, &music_id, kind)
 *         @0x001818f0
 *
 * It validates the id (`CmnFunc_ErrChekMusicID`), splits it
 * (`CmnFunc_GetMusicID_Hi`/`_Low`), and posts through
 * `Total_MainSendMessage_Arg` @0x000ee8a8 -- whose r3 lands at message +36,
 * exactly where the receiver's `CmnFunc_SetMusicID_Hi(msg+36)` reads it. The two
 * ends agree, so this is an exercised path and not a reconstruction.
 *
 * The shipped caller is `db::DbIF::request_DuplicateLoad(int deck, music_id)` at
 * 0x0014287c, and it is what fixes KIND: it passes **3**, the branch that reads
 * the source out of the id's own bytes.  (Kind 0 would call `setDragAndDrop`
 * and mark the deck as drag-and-drop loaded -- not what a load wants.)
 *
 * THE ID IS THE DATABASE'S OWN.  `MusicID_Low` is the same number as the track
 * id in `export.pdb` -- measured twice, live: deck 2 held id 24 (*Runnin'
 * (Original Mix)*, Deetron) and deck 1 id 663 (*Can't Get You Out Of My Head*,
 * Kylie Minogue), both read back off the running player and then looked up in
 * the database.  So a track can be named by the number rekordbox itself uses,
 * and `work/pdb.py` reads that number out of the export.
 *
 * ===========================================================================
 * WHY AN ABSOLUTE ADDRESS IS SAFE HERE
 * ===========================================================================
 *
 * rbp is a non-PIE ELF, so its symbols sit at fixed addresses -- but the port
 * runs a PATCHED rbp, and a patch that inserted instructions would move every
 * address after it.  It does not: `work/patched/rbp-audio` is byte-for-byte the
 * same SIZE as the stock binary, and `BrowseLoadItemEnter`,
 * `CmnFunc_CmnInfo_Local2CdjMusicDragAndDropHandler`, `Total_MainSendMessage_Arg`
 * and `uiBrowse` all read at the SAME addresses in both.  The patches overwrite
 * in place.  (Checked, not assumed -- if a future patch ever inserts, this
 * constant and `rbp_abi.h`'s go stale together, and this is the note that says
 * so.)
 *
 * ===========================================================================
 * THE COMMAND
 * ===========================================================================
 *
 *   /tmp/rb_load        "<deck> <trackid> [hi]"
 *
 *     deck     1 or 2, as printed on the deck strip (rbp's own argument is
 *              0-indexed, so this file subtracts one).
 *     trackid  the `export.pdb` track id.  Decimal.
 *     hi       the SOURCE descriptor, `PlNo | DevKind<<8 | BrowseKind<<16`.
 *              Defaults to 0x00010200 -- PlNo 0, DevKind 2, BrowseKind 1 -- which
 *              is what this port's USB 1 measures.  A track pulled from another
 *              slot or a playlist needs that slot's value.
 *
 * THE FILE IS READ, NEVER WRITTEN, and a line acts once: the watcher remembers
 * the last contents it saw, so leaving the file in place does not reload the
 * track forever, and re-loading the same track is done by writing a DIFFERENT
 * line (or touching the file with the same text after another write).  Nothing
 * here can spin: an unreadable or absent file is a no-op, and the only write to
 * the filesystem is this file's own log.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <pthread.h>

#include "rbp_load.h"

/* rbp's own load-by-music-id entry point. See the header for why this is a fixed
 * address and what would invalidate it. */
#define RBP_LOAD_FN 0x001818f0u

#define RBP_LOAD_CMD      "/tmp/rb_load"
#define RBP_LOAD_LOG      "/tmp/rbload.log"
#define RBP_LOAD_POLL_MS  200
#define RBP_LOAD_HI_DEF   0x00010200u   /* PlNo 0, DevKind 2, BrowseKind 1 */
#define RBP_LOAD_KIND     3             /* what request_DuplicateLoad passes */

/* The MusicID as rbp holds it: a 24-bit source descriptor and the track's id.
 * `hi` is written as three bytes by CmnFunc_SetMusicID_Hi, so only its low
 * three bytes survive the trip. */
struct music_id {
    unsigned int hi;
    unsigned int lo;
};

typedef int (*rbp_load_fn)(int deck, const struct music_id *id, int kind);

static FILE *rl_fp;
static pthread_once_t rl_once = PTHREAD_ONCE_INIT;

static void rl_open(void)
{
    rl_fp = fopen(RBP_LOAD_LOG, "a");
}

static void rl_log(const char *fmt, ...)
{
    va_list ap;

    pthread_once(&rl_once, rl_open);
    if (rl_fp == NULL)
        return;
    va_start(ap, fmt);
    vfprintf(rl_fp, fmt, ap);
    va_end(ap);
    fputc('\n', rl_fp);
    fflush(rl_fp);
}

static void rl_sleep_ms(long ms)
{
    struct timespec ts;

    ts.tv_sec  = ms / 1000;
    ts.tv_nsec = (ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
}

/* Parse "<deck> <trackid> [hi]". Returns 1 and fills the outputs on success.
 *
 * Deliberately hand-rolled rather than sscanf: this is a preloaded shim on a
 * vendor libc with gaps (see the note about stat/fstat in the port docs), so the
 * fewer library entry points the better. strtoul is the only one used. */
static int rl_parse(const char *s, int *deck, unsigned int *track, unsigned int *hi)
{
    char *end;
    unsigned long a, b, c;

    a = strtoul(s, &end, 10);
    if (end == s)
        return 0;
    b = strtoul(end, &end, 10);
    if (end == s || b == 0)
        return 0;
    while (*end == ' ' || *end == '\t')
        end++;
    *hi = RBP_LOAD_HI_DEF;
    if (*end) {
        c = strtoul(end, &end, 16);      /* hex, so 0x00010200 reads as itself */
        if (c)
            *hi = (unsigned int)c;
    }
    *deck  = (int)a;
    *track = (unsigned int)b;
    return 1;
}

void *rbp_load_thread(void *arg)
{
    char buf[128];
    char last[128];
    (void)arg;

    last[0] = '\0';
    rl_log("rbp_load: watching %s every %d ms -- a line is \"<deck> <trackid> [hi]\"",
           RBP_LOAD_CMD, RBP_LOAD_POLL_MS);

    for (;;) {
        int fd;
        ssize_t n;
        int deck;
        unsigned int track, hi;
        struct music_id id;
        int rc;

        rl_sleep_ms(RBP_LOAD_POLL_MS);

        fd = open(RBP_LOAD_CMD, O_RDONLY);
        if (fd < 0)
            continue;                    /* absent = disarmed, and silent */
        n = read(fd, buf, sizeof(buf) - 1);
        close(fd);
        if (n <= 0)
            continue;
        buf[n] = '\0';

        if (strcmp(buf, last) == 0)
            continue;                    /* the same command is not a new one */
        strncpy(last, buf, sizeof(last) - 1);
        last[sizeof(last) - 1] = '\0';

        if (!rl_parse(buf, &deck, &track, &hi) || deck < 1 || deck > 2) {
            rl_log("rbp_load: \"%s\" is not \"<deck 1|2> <trackid> [hi]\" -- ignored", buf);
            continue;
        }

        id.hi = hi;
        id.lo = track;
        rl_log("rbp_load: deck %d <- track %u  (music_id %08x / %u, kind %d)",
               deck, track, id.hi, id.lo, RBP_LOAD_KIND);

        rc = ((rbp_load_fn)RBP_LOAD_FN)(deck - 1, &id, RBP_LOAD_KIND);
        rl_log("rbp_load: %s returned %d", "Local2CdjMusicDragAndDropHandler", rc);
    }

    return NULL;
}
