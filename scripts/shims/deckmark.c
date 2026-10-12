/*
 * deckmark.c -- see deckmark.h for why this route and not a plane.
 *
 * IT PAINTS INTO rbp's PAGE, WHICH IS NOT OURS, and that has three consequences this file exists to
 * handle and no other file does:
 *
 *   1. THE MAPPING IS THE REAL ONE. The page is opened and mmapped with the shim's UN-interposed
 *      entry points (real_open/real_ioctl/real_mmap), because fb_shim.c interposes
 *      FBIOGET_VSCREENINFO to LIE about the depth and geometry so that rbp and DirectFB size their
 *      layers from the lie. A mapping taken through the interposed calls would be the wrong size in
 *      the wrong format. This is menu_draw.c:2282-2341's pattern and fb_cursor.c:150's.
 *   2. WHAT WAS UNDER THE BAR IS KEPT. The bar is reversible: when it goes up the pixels it covers
 *      are saved, and when it comes down they are put back. The page never keeps a green scar.
 *   3. IT IS DRAWN EVERY FRAME, not once. rbp repaints its own frame; the rows this bar uses are
 *      empty margin today (measured -- see deckmark_zone.h), but "empty today" is not a contract
 *      anyone signed. A few hundred bytes of memcpy per frame settles it, and the cost is the
 *      reason this route was chosen in the first place.
 */
#include "deckmark.h"

#include <fcntl.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include <linux/fb.h>

#include "deckmark_paint.h"
#include "deckmark_zone.h"
#include "drmband.h"         /* the bar's own overlay plane: steady, because nothing overwrites it */
#include "envutil.h"         /* env_flag, env_str, env_int */
#include "menu_draw.h"       /* menu_draw's own state */
#include "pointsrc.h"        /* pointsrc_log, rbp_perf_screen */
#include "rbp_transport.h"   /* rbp_transport_get, RBP_TRANSPORT_PLAY_SOLID */
#include "syscalls.h"        /* real_open/ioctl/mmap/close */

#define DM_DEV_DEFAULT "/dev/fb0"

/* The most rows the marker will carry, which is what sizes the save buffers. Well past the 20 the
 * default uses: a taller bar is a knob for the operator's eye, not for the code, and an env that
 * asks for more is refused with a line in the log rather than a buffer overrun. */
#define DM_MAX_ROWS 48
#define DM_MAX_ROW_BYTES (619 * 4)                    /* the widest strip at the deepest pixel */

static unsigned char *page;
static int  page_pitch;                  /* BYTES per row, the REAL one */
static int  page_bpp;
static int  page_ready;                  /* an attempt has been made, whatever it answered */
static int  usable;

static int  on[2];                       /* the bar is on the page for this deck right now */
static unsigned char saved[2][DM_MAX_ROWS * DM_MAX_ROW_BYTES];
static int  saved_rows[2];
static int  told[2];                     /* the log has said what this deck is doing once */

static unsigned int mark_rgb = 0x28d246u;      /* the project's accent green, as 0xRRGGBB */
static int  mark_y0 = DECKMARK_Y0_DEFAULT;
static int  mark_y1 = DECKMARK_Y1_DEFAULT;
static int  mark_on = 1;

/* THE BAR'S OWN PLANE, and this is the route that cannot drop a frame. The page route below is
 * still here as the fallback for a machine where no plane can be had, and it is the route that was
 * tried first -- it works, it reaches the screen, and it measured 99.7% duty. But that residual is
 * the REPAIR LATENCY (one tick, 0.1 ms of a 17.5 ms frame), and the operator can see it: "i do see
 * the small drop off and its something i don't want. 99.11 isn't enough". A tick cannot run faster
 * than the scheduler allows, so the page route has a ceiling. A PLANE HAS NONE: the hardware
 * composites it, nothing writes over it, and there is nothing to repair.
 *
 * It is also CHEAPER than the ring: the bar is a solid colour, so it needs no copy of rbp's pixels
 * -- the plane's buffer is filled once and simply shown and hidden. That makes it the only surface
 * in this shim that touches neither the framebuffer nor a read(). */
#define DM_BAND_RANK DRM_RANK_RING       /* below the drawers: they are full height and overlap */

static struct drm_band bar[2];
static int  plane_dead[2];              /* no plane for this deck; the page route has it */
static int  plane_ok[2];                 /* this deck has a plane carrying the bar */
static int  plane_up[2];                 /* ...and it is on the glass */

static void dm_read_env(void)
{
    const char *v = env_str("DECKMARK_BAR", "");
    const char *c = env_str("DECKMARK_RGB", "");

    mark_on = env_flag("DECKMARK", 1);
    if (v && *v) {
        int a = 0, b = 0;

        if (sscanf(v, "%d,%d", &a, &b) == 2 && a >= 0 && b >= a && b < 4096) {
            mark_y0 = a;
            mark_y1 = b;
        } else {
            pointsrc_log("deckmark: DECKMARK_BAR='%s' is not y0,y1 -- keeping %d,%d",
                         v, mark_y0, mark_y1);
        }
    }
    if (c && *c) {
        unsigned int rgb = 0;

        if (sscanf(c, "%x", &rgb) == 1)
            mark_rgb = rgb & 0xffffffu;
        else
            pointsrc_log("deckmark: DECKMARK_RGB='%s' is not hex -- keeping %#x", c, mark_rgb);
    }
}

static void dm_map_once(void)
{
    struct fb_var_screeninfo var;
    struct fb_fix_screeninfo fix;
    const char *dev = env_str("FB_DEV", DM_DEV_DEFAULT);
    size_t len;
    int fd;

    if (page_ready)
        return;
    page_ready = 1;
    dm_read_env();

    if (!mark_on) {
        pointsrc_log("deckmark: off (DECKMARK=0)");
        return;
    }

    fd = real_open(dev, O_RDWR, 0);
    if (fd < 0) {
        pointsrc_log("deckmark: no %s (%m) -- no playing marker", dev);
        return;
    }
    memset(&var, 0, sizeof var);
    memset(&fix, 0, sizeof fix);
    if (real_ioctl(fd, FBIOGET_VSCREENINFO, &var) < 0 ||
        real_ioctl(fd, FBIOGET_FSCREENINFO, &fix) < 0) {
        pointsrc_log("deckmark: %s will not say what it is (%m) -- no playing marker", dev);
        real_close(fd);
        return;
    }

    /* THE REAL GEOMETRY, and the bar's coordinates are screen coordinates, so anything else is a
     * machine this module has no business painting (the DFB_PRESENT modes scale or letterbox the
     * picture inside the page, and the bar would land somewhere rbp never draws). */
    if (var.xres != 1280 || var.yres != 800) {
        pointsrc_log("deckmark: %s is %ux%u, not 1280x800 -- no playing marker",
                     dev, var.xres, var.yres);
        real_close(fd);
        return;
    }
    if (var.bits_per_pixel != 16 && var.bits_per_pixel != 32) {
        pointsrc_log("deckmark: %s is %u bpp -- no playing marker", dev, var.bits_per_pixel);
        real_close(fd);
        return;
    }

    len = (size_t)fix.line_length * (size_t)var.yres;
    page = real_mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (page == (void *)-1 || page == NULL) {
        page = NULL;
        pointsrc_log("deckmark: could not map %s (%m) -- no playing marker", dev);
        real_close(fd);
        return;
    }
    real_close(fd);

    page_pitch = (int)fix.line_length;
    page_bpp   = (int)var.bits_per_pixel;
    usable     = 1;
    pointsrc_log("deckmark: %s mapped, %dx%d %d bpp pitch %d -- the playing bar is drawn into "
                 "rbp's own frame, rows %d..%d, %#06x",
                 dev, var.xres, var.yres, page_bpp, page_pitch, mark_y0, mark_y1, mark_rgb);
}

/* ONE PIXEL OF THE PAGE, and the page may be 16 or 32 bpp -- the same two depths the painter
 * writes. It is a memory read: the page is mapped, so asking what is there costs nothing like the
 * read()/lseek() pair this project removed from the badge's copy. */
static unsigned int dm_peek(int x, int y)
{
    const unsigned char *p = page + (size_t)y * (size_t)page_pitch
                                  + (size_t)x * (size_t)(page_bpp / 8);

    if (page_bpp == 16)
        return *(const unsigned short *)p;
    return *(const unsigned int *)p;
}

/* IS THE BAR STILL THERE? Three points along its middle row, which is the whole of the repair
 * logic: rbp repaints its own frame every frame and its sweep passes over these rows, so "is it
 * still there" has to be ASKED, cheaply, rather than assumed -- and re-filling it 10,000 times a
 * second without asking would be 240 MB/s of uncached writes for nothing. This is menu_draw.c's
 * menu_intact() idiom, which the band's page route proved: a witness on a fast tick measured
 * 99.11% duty against 53.83% for redrawing on a frame's tick. */
static int dm_intact(int x0, int y0, int x1, int y1, unsigned int pv)
{
    int ym = (y0 + y1) / 2;

    return dm_peek(x0, ym) == pv && dm_peek((x0 + x1) / 2, ym) == pv && dm_peek(x1, ym) == pv;
}

/* The two calls that go with a deck's state, so the drawing and the bookkeeping cannot drift. */
static void dm_up(int deck, int x0, int y0, int x1, int y1)
{
    int rb = deckmark_rect_bytes(page_bpp, page_pitch, x0, y0, x1, y1);
    unsigned int pv = deckmark_pixel(page_bpp, mark_rgb);

    if (rb <= 0 || rb > (int)sizeof saved[0]) {
        if (!told[deck]) {
            told[deck] = 1;
            pointsrc_log("deckmark: deck%d's bar does not fit the save buffer (%d bytes)", deck + 1, rb);
        }
        return;
    }

    if (!on[deck]) {
        deckmark_save(page, page_pitch, page_bpp, x0, y0, x1, y1, saved[deck]);
        saved_rows[deck] = y1 - y0 + 1;
        on[deck] = 1;
        if (!told[deck]) {
            told[deck] = 1;
            pointsrc_log("deckmark: deck%d playing -- bar up at %d,%d %dx%d in %#06x",
                         deck + 1, x0, y0, x1 - x0 + 1, y1 - y0 + 1, mark_rgb);
        }
    }

    /* AND ONLY WHEN IT IS NOT. This is called at the frame boundary AND from the paint thread's
     * fast tick, so it has to be free when there is nothing to do -- three reads -- and immediate
     * when rbp's sweep has taken the bar. */
    if (!dm_intact(x0, y0, x1, y1, pv))
        deckmark_fill(page, page_pitch, page_bpp, x0, y0, x1, y1, pv);
}

static void dm_down(int deck, int x0, int y0, int x1, int y1)
{
    if (!on[deck])
        return;
    deckmark_restore(page, page_pitch, page_bpp, x0, y0, x1, y1, saved[deck]);
    on[deck] = 0;
    if (told[deck]) {
        told[deck] = 0;
        pointsrc_log("deckmark: deck%d stopped -- bar taken away", deck + 1);
    }
}

/* WHAT A DECK'S MARKER SHOULD BE DOING THIS MOMENT, from the two gates and rbp's own play bit.
 * BIT 3 IS PLAY_SOLID, NOT BIT 2 -- the same distinction rbp_led.c makes for the ring, and for the
 * same reason: bit 2 blinks for a paused deck, and a marker that blinks is a marker that flickers.
 * A failed read is -1, whose bit 3 tests as SET, so it is tested for.
 *
 * THE PERFORMANCE SCREEN IS PART OF THE GATE, from the touch thread's own answer: the strips exist
 * on that screen, and a bar left on any other would run across whatever rbp draws there. AND SO IS
 * ANYTHING OPEN THAT PAINTS THESE ROWS -- the side drawers are full height and reach x 0..179 and
 * x 1100..1279, the bar's first and last columns, and the browser window can sit over them. Drawing
 * last would put the bar ON TOP of an open drawer, and a drawer is rbp's surface that the operator
 * opened deliberately. The marker stands down while any of them is up and returns when they close. */
static int dm_want(int deck)
{
    int tr;

    if (!rbp_perf_screen())
        return 0;
    /* AND NOTHING ELSE. An earlier version stood the bar down while a side drawer or the window was
     * open, on the theory that drawing over an operator-opened surface would be wrong. It is wrong,
     * and it CANNOT HAPPEN HERE -- which is why the gate is gone: a drawer is an overlay plane too,
     * at a HIGHER RANK than this bar's, so the display hardware composites it OVER the bar already.
     * The operator's report is what settled it ("the bars disappear when the side panels show up"):
     * the drawer covers the part of the bar it covers, correctly, and the rest of the bar should
     * stay visible instead of blinking out. */
    tr = rbp_transport_get(deck);
    return (tr < 0) ? 0 : ((tr >> 3) & 1);
}

/* THE PLANE ROUTE. Setup once per deck, then only show and hide -- and the buffer is written ONCE,
 * when it is made, because the bar is a solid colour that never changes. Nothing here reads rbp's
 * pixels at all, which is why this route costs less than the ring it replaces as well as dropping
 * no frames.
 *
 * TICK THREAD ONLY: drm_band_setup()/show()/hide() are DRM ioctls, and the other caller of this
 * module runs on rbp's SCHED_FIFO render thread. See the two entry points at the foot of the file. */
static int dm_plane_up(int deck, int x0, int y0, int x1, int y1)
{
    const int w = x1 - x0 + 1;
    const int h = y1 - y0 + 1;
    unsigned short *p;
    int i;

    if (plane_dead[deck])
        return 0;                       /* tried, refused: the page route has this deck */

    if (!plane_ok[deck]) {
        drm_band_want_rank(DM_BAND_RANK);
        if (drm_band_setup(&bar[deck], w, h, 16) != 0) {
            plane_dead[deck] = 1;
            pointsrc_log("deckmark: deck%d has no overlay plane -- the bar goes on the page "
                         "instead (which is repaired, and can drop a frame)", deck + 1);
            return 0;
        }
        /* ONE WRITE, EVER: the bar's colour is a constant, so the plane's buffer is filled here and
         * never touched again. This is also why nothing needs saving or restoring -- every pixel of
         * this buffer is ours, which is the property the page route cannot have. */
        p = (unsigned short *)bar[deck].pix;
        for (i = 0; i < w * h; i++)
            p[i] = (unsigned short)deckmark_pixel(16, mark_rgb);
        plane_ok[deck] = 1;
    }

    if (!plane_ok[deck])
        return 0;

    if (!plane_up[deck]) {
        if (drm_band_show(&bar[deck], x0, y0) != 0) {
            pointsrc_log("deckmark: deck%d's bar could not be shown -- page instead", deck + 1);
            plane_ok[deck] = 0;
            return 0;
        }
        plane_up[deck] = 1;
        if (!told[deck]) {
            told[deck] = 1;
            pointsrc_log("deckmark: deck%d playing -- bar up at %d,%d %dx%d in %#06x on plane %u",
                         deck + 1, x0, y0, w, h, mark_rgb, bar[deck].plane);
        }
    }
    return 1;
}

static void dm_plane_down(int deck)
{
    if (plane_ok[deck] && plane_up[deck]) {
        drm_band_hide(&bar[deck]);
        plane_up[deck] = 0;
        if (told[deck]) {
            told[deck] = 0;
            pointsrc_log("deckmark: deck%d stopped -- bar taken away", deck + 1);
        }
    }
}

/* THE TICK THREAD'S ENTRY, and the one that owns the plane: called from menu_draw.c's paint loop,
 * which already runs at POINT_MENU_MS for the band. A deck with a plane costs two integer reads
 * here; a deck without one falls back to the page route, which does its repair from this same
 * call (and from the seam) -- see dm_up()'s witness. */
void deckmark_tick(void)
{
    int deck;

    dm_map_once();

    for (deck = 0; deck < 2; deck++) {
        int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
        int want = dm_want(deck);

        if (!deckmark_bar(deck, mark_y0, mark_y1, &x0, &y0, &x1, &y1))
            continue;

        if (want && dm_plane_up(deck, x0, y0, x1, y1))
            continue;                   /* the plane has it; the page needs no part of this */

        if (!want)
            dm_plane_down(deck);

        /* THE FALLBACK, and only for a deck the plane is not carrying. `usable` is the page's own
         * state, so a machine with neither route simply gets nothing and has been told why. */
        if (!usable)
            continue;
        if (want)
            dm_up(deck, x0, y0, x1, y1);
        else
            dm_down(deck, x0, y0, x1, y1);
    }
}

/* THE RENDER THREAD'S ENTRY, from fb_shim.c's FBIO_WAITFORVSYNC. It does the PAGE route's cheap
 * part only -- the witness and, if rbp's sweep has taken the bar, the re-fill -- because that is a
 * few memory reads and a memcpy, and it is called on the thread that must not block. When the plane
 * is carrying a deck this returns almost immediately: nothing to witness, nothing to repair.
 *
 * The plane is NOT touched from here: its ioctls belong to the tick thread. */
void deckmark_seam(void)
{
    int deck;

    if (!usable)
        return;

    for (deck = 0; deck < 2; deck++) {
        int x0 = 0, y0 = 0, x1 = 0, y1 = 0;

        if (plane_ok[deck] && plane_up[deck])
            continue;                   /* the hardware is compositing it; rbp cannot reach it */
        if (!deckmark_bar(deck, mark_y0, mark_y1, &x0, &y0, &x1, &y1))
            continue;
        if (dm_want(deck))
            dm_up(deck, x0, y0, x1, y1);
        else
            dm_down(deck, x0, y0, x1, y1);
    }
}

void deckmark_reset(void)
{
    int deck;

    for (deck = 0; deck < 2; deck++) {
        on[deck] = 0;
        told[deck] = 0;
        saved_rows[deck] = 0;
    }
}
