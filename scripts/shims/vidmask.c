/*
 * vidmask.c -- a rectangle of the panel showing raw video, masked from rbp.
 *
 * ===========================================================================
 * THE PROBLEM THIS SOLVES, MEASURED
 * ===========================================================================
 *
 * rbp repaints its own dirty windows into fb0 -- the page -- at its own ~57 Hz.
 * Anything written into that page is therefore erased within a frame or two, and
 * a video written there reads on the glass as a flashing band with rbp's UI
 * drawn through it. That is not a theory: the same 3-second clip was pushed
 * straight into /dev/fb0 twice, once with rbp running and once with rbp
 * SIGSTOPped. Running, it flickered and only a strip of it survived; stopped, it
 * filled the panel perfectly, clean, at a sustained 10.0 fps over 200 frames.
 *
 * So the picture is fine and the writing is fine. What fails is SHARING THE PAGE.
 *
 * ===========================================================================
 * THE CURE IS TO DRAW OVER rbp, NOT ON IT
 * ===========================================================================
 *
 * An OPAQUE overlay plane in the rectangle shows the video and hides whatever rbp
 * is painting underneath -- no copy of rbp's pixels, no cooperation from rbp, and
 * rbp is not stopped, slowed or modified. It keeps drawing; it is simply not
 * visible there. This is the same mechanism the band and the two drawers already
 * use (drmband.c, shipped), and this is the one surface that WANTS to be opaque,
 * which makes it SIMPLER than any of them: the band has to copy rbp's pixels in
 * first because an RGB565 carrier is opaque and it wants rbp to show through the
 * gaps; a mask wants the opposite and fills its whole buffer.
 *
 * It takes DRM_RANK_MASK -- the rank the removed browser window left free, which
 * is above the band. A mask has to be above everything, including the band.
 *
 * ===========================================================================
 * WHY THIS HAS TO BE A SHIM
 * ===========================================================================
 *
 * A plane can only be set by the DRM master and rbp holds it. Measured: an
 * external tool's SET_MASTER fails **EBUSY** and `drmplane bar` refuses to
 * continue -- "a plane set without master may be dropped at any commit". So there
 * is no outside route, which is exactly why the band's planes work and an
 * external one cannot.
 *
 * It also inherits DRM_BAND_SKIP_OVERLAYS from drmband.c: the picker steps over
 * the first four overlays because their contents never composite, and the kernel
 * reports them bound and enabled the whole time. A mask that landed on one of
 * those would be invisible while every state dump said it was fine -- and note
 * the external tool, having no such knowledge, picked **plane 127**, the first of
 * the four.
 *
 * ===========================================================================
 * THE FEED, AND ITS HONEST LIMIT
 * ===========================================================================
 *
 * Frames come from a FILE, mapped once and walked -- never read() per frame. The
 * per-frame cost is then one memcpy into the plane's own buffer, which the plane
 * scans out continuously, so there is nothing to commit per frame either.
 *
 * The file is raw RGB565, one frame after another, each exactly W*H*2 bytes: that
 * is what makes the per-frame copy a single memcpy. `work/fbplay.py`'s header has
 * the ffmpeg line that produces one.
 *
 * THIS IS NOT A DECODER. The frames are already decoded, on the workstation. rbp
 * has no decoder on it (ffmpeg is not installed) and this file does not add one --
 * the browser workstream died at the decoder, not here, and this does not reopen
 * it. What this proves is the DISPLAY half: that a rectangle of the panel can show
 * moving pictures at frame rate while rbp runs.
 *
 * CONFIGURATION -- all empty-means-unset, per envutil.h:
 *
 *   RB_VIDEO      path to the raw frames. UNSET = this module does nothing at all.
 *   RB_VIDEO_X/Y  where the rectangle sits      (default: centred)
 *   RB_VIDEO_W/H  its size in pixels            (default 640x400)
 *   RB_VIDEO_FPS  frames per second             (default 10)
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <time.h>

#include "vidmask.h"
#include "drmband.h"
#include "envutil.h"
#include "pointsrc.h"       /* pointsrc_log */

/* The panel these defaults are centred in. Read only to place a default; the
 * mask itself is sized and placed entirely by the environment. */
#define VM_SCREEN_W 1280
#define VM_SCREEN_H  800

/* The runtime geometry: "<x> <y> <w> <h> <seq>". See vm_control(). */
#define VM_CTL "/tmp/vm.ctl"

struct vidmask {
    struct drm_band band;
    const unsigned char *blob;      /* the whole file, mapped once */
    size_t  blob_len;
    int     frame_bytes;            /* w * h * 2 */
    int     n_frames;
    int     cur;
    int     fps;
    double  next;                   /* CLOCK_MONOTONIC seconds for the next frame */
    int     tried;                  /* setup has been attempted, one way or another */
    int     on;                     /* the plane is carrying our frames */
    /* WHAT IT ACTUALLY DID, not what it was asked for. The target is a clock, so the
     * rate the operator sees is min(RB_VIDEO_FPS, the tick rate) -- and the tick rate
     * is rbp's own frame rate, which this module can only ride. Logging the two
     * together is the only way to tell "paced correctly" from "the tick cannot keep
     * up", which look identical from outside. */
    unsigned long ticks, advances;
    double  last_log;
    char    ctl_last[64];            /* the last control line acted on */
    int     hidden;                  /* the plane is disabled, area back to rbp */
};

static struct vidmask vm;

/* CLOCK_MONOTONIC in double seconds, for pacing only.
 *
 * NOT the ns clock the rest of this project was bitten by: there a `long` holds a
 * nanosecond CLOCK_MONOTONIC and wraps every 4.29 s on this 32-bit target. Here the
 * split into tv_sec/tv_nsec happens first and only the RESULT is a double, so the
 * wrap cannot reach it. Do not "simplify" this into a single integer nanosecond
 * value. */
static double vm_now(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1000000000.0;
}

static void vidmask_setup(void)
{
    const char *path;
    int w, h, x, y, fd;
    off_t n;

    vm.tried = 1;

    path = env_str("RB_VIDEO", "");
    if (path == NULL || path[0] == '\0')
        return;                             /* not asked for: stay silent */

    w = env_int("RB_VIDEO_W", 640);
    h = env_int("RB_VIDEO_H", 400);
    x = env_int("RB_VIDEO_X", (VM_SCREEN_W - w) / 2);
    y = env_int("RB_VIDEO_Y", (VM_SCREEN_H - h) / 2);
    vm.fps = env_int("RB_VIDEO_FPS", 10);
    if (vm.fps <= 0)
        vm.fps = 10;
    if (w <= 0 || h <= 0) {
        pointsrc_log("vidmask: RB_VIDEO_W/H are %dx%d -- nothing to show", w, h);
        return;
    }

    /* NO stat() HERE, and that is not an oversight: this target's libc has no
     * stat/fstat, and a shim that calls one links fine and then kills rbp at
     * start-up. lseek to the end is how the size is got instead -- once, at setup,
     * where it costs nothing. */
    fd = open(path, O_RDONLY);
    if (fd < 0) {
        pointsrc_log("vidmask: cannot open %s (%m) -- rbp stays visible", path);
        return;
    }
    n = lseek(fd, 0, SEEK_END);
    lseek(fd, 0, SEEK_SET);

    vm.frame_bytes = w * h * 2;
    if (n <= 0 || n < (off_t)vm.frame_bytes) {
        pointsrc_log("vidmask: %s is %ld bytes, short of one %dx%d RGB565 frame (%d)"
                     " -- rbp stays visible", path, (long)n, w, h, vm.frame_bytes);
        close(fd);
        return;
    }

    vm.blob = mmap(NULL, (size_t)n, PROT_READ, MAP_SHARED, fd, 0);
    close(fd);                              /* the mapping holds the pages */
    if (vm.blob == MAP_FAILED) {
        vm.blob = NULL;
        pointsrc_log("vidmask: mmap of %s failed (%m) -- rbp stays visible", path);
        return;
    }
    vm.blob_len = (size_t)n;
    vm.n_frames = (int)((size_t)n / (size_t)vm.frame_bytes);

    /* ABOVE EVERYTHING, including the band -- a mask that a panel could cover is
     * not a mask. This is the rank the browser window left free. */
    drm_band_want_rank(DRM_RANK_MASK);
    if (drm_band_setup(&vm.band, w, h, 16) != 0) {
        pointsrc_log("vidmask: no overlay plane for a %dx%d mask -- rbp stays visible", w, h);
        return;
    }

    memcpy(vm.band.pix, vm.blob, (size_t)vm.frame_bytes);
    if (drm_band_show(&vm.band, x, y) != 0) {
        pointsrc_log("vidmask: the plane would not show at %d,%d -- rbp stays visible", x, y);
        return;
    }

    vm.on = 1;
    vm.next = vm_now() + 1.0 / vm.fps;
    pointsrc_log("vidmask: %dx%d at %d,%d on plane %u -- %s, %d frames at %d fps",
                 w, h, x, y, vm.band.plane, path, vm.n_frames, vm.fps);
}

/* THE RUNTIME GEOMETRY, re-read every frame and committed ONLY when it changes.
 *
 * WHY A FILE THE SHIM WATCHES, and not env read once: the point of a dynamic mask is
 * that whatever is driving it can move it, resize it or give the area back at any
 * moment without a restart, and a watched file is how this project already does that
 * -- rbp_load.c's /tmp/rb_load. Same rule too: a line acts ONCE, so repeating a
 * geometry means CHANGING it, and the sequence number is what makes an identical
 * line a new instruction rather than a duplicate.
 *
 * A width or height of 0 is not a zero-sized mask, it is HIDE: the plane is disabled
 * outright and the rectangle goes back to rbp.
 *
 * THE CONTENT IS NOT RE-SIZED HERE. The buffer stays whatever RB_VIDEO_W/H made it,
 * and a geometry line selects the rectangle it is shown in -- so the difference
 * between the two is what the HVS scales. That is the whole experiment this slice
 * exists to run. */
static void vm_control(void)
{
    char buf[64];
    char *end;
    long x, y, w, h, fw, fh, fx, fy;
    char mode;
    ssize_t n;
    int fd;

    fd = open(VM_CTL, O_RDONLY);
    if (fd < 0)
        return;
    n = read(fd, buf, sizeof buf - 1);
    close(fd);
    if (n <= 0)
        return;
    buf[n] = '\0';
    if (strcmp(buf, vm.ctl_last) == 0)
        return;                          /* the same instruction is not a new one */
    strncpy(vm.ctl_last, buf, sizeof vm.ctl_last - 1);
    vm.ctl_last[sizeof vm.ctl_last - 1] = '\0';

    x = strtol(buf,   &end, 10);
    y = strtol(end,   &end, 10);
    w = strtol(end,   &end, 10);
    h = strtol(end,   &end, 10);

    /* The fifth token is the ASPECT MODE and it is ONE CHARACTER, because the whole
     * line is typed by hand at a shell:
     *
     *     f   FIT -- the largest rectangle of the content's OWN aspect that fits
     *              inside the one asked for, centred. Nothing is distorted, and the
     *              bar left over is rbp's again, because a plane can only produce
     *              one rectangle. This is the default, and it is the default because
     *              a stretched picture is the thing that looks broken.
     *     s   STRETCH -- fill the rectangle exactly, whatever that does to the
     *              aspect. What the first scaling test used on purpose.
     */
    while (*end == ' ' || *end == '\t')
        end++;
    mode = *end;

    if (w <= 0 || h <= 0) {
        vm.hidden = 1;
        drm_band_hide(&vm.band);
        pointsrc_log("vidmask: hidden (%ldx%ld) -- the area is rbp's again", w, h);
        return;
    }
    if (vm.band.w <= 0 || vm.band.h <= 0)
        return;

    /* FIT: pick the axis that runs out first. The content's aspect is the buffer's. */
    fw = w; fh = h;
    if (mode != 's') {
        /* Cross-multiplied, so no floating point and no rounding to compare two
         * aspect ratios: cw/ch > w/h is cw*h > w*ch. Whichever axis runs out first
         * is the one to fit by, and the OTHER is then derived from it. Deriving the
         * right one matters -- the pair (w*cw/ch, h) is not the same rectangle as
         * (w, w*ch/cw), and the wrong one is still "inside the rect" so nothing
         * complains while the picture is the wrong shape. Checked against four
         * cases including the identity before it went near the unit. */
        if ((long)vm.band.w * h > (long)w * vm.band.h) {
            fw = w;                                   /* content is WIDER: fit by width */
            fh = ((long)w * vm.band.h) / vm.band.w;
        } else {
            fh = h;                                   /* content is TALLER: fit by height */
            fw = ((long)h * vm.band.w) / vm.band.h;
        }
        if (fw < 1) fw = 1;
        if (fh < 1) fh = 1;
    }
    fx = x + (w - fw) / 2;
    fy = y + (h - fh) / 2;

    vm.hidden = 0;
    if (drm_band_show_rect(&vm.band, 0, 0, vm.band.w, vm.band.h,
                           (int)fx, (int)fy, (int)fw, (int)fh) == 0)
        pointsrc_log("vidmask: asked %ldx%ld at %ld,%ld -> %s %ldx%ld at %ld,%ld"
                     " (buffer %dx%d, scaled by the HVS)",
                     w, h, x, y, mode == 's' ? "stretched" : "fitted",
                     fw, fh, fx, fy, vm.band.w, vm.band.h);
}

void vidmask_tick(void)
{
    double t;

    if (!vm.tried)
        vidmask_setup();
    if (!vm.on)
        return;

    vm_control();                        /* move / resize / hide, if asked */
    if (vm.hidden)
        return;

    vm.ticks++;
    t = vm_now();

    /* Once a second: the two rates side by side. They are the whole story of whether
     * the picture is as smooth as it was asked to be. */
    if (vm.last_log == 0.0)
        vm.last_log = t;
    else if (t - vm.last_log >= 1.0) {
        double dt = t - vm.last_log;
        pointsrc_log("vidmask: %.1f fps advanced, tick %.1f Hz (%lu frames / %lu ticks)",
                     (double)vm.advances / dt, (double)vm.ticks / dt, vm.advances, vm.ticks);
        vm.advances = 0;
        vm.ticks = 0;
        vm.last_log = t;
    }

    if (t < vm.next)
        return;
    vm.next = t + 1.0 / vm.fps;

    vm.cur = (vm.cur + 1) % vm.n_frames;
    vm.advances++;
    /* The plane scans this buffer continuously, so a write into it IS the frame --
     * there is no commit and no flip, and that is the whole reason a plane can
     * carry video at all. See deckbadge_plane.c's note on the same subject.
     *
     * THE PRICE IS TEARING. The write can land while the plane is part-way through
     * scanning the buffer, so a frame boundary can be visible as a seam. Double
     * buffering would cure it -- two fbs, alternate -- and is NOT done here. */
    memcpy(vm.band.pix, vm.blob + (size_t)vm.cur * (size_t)vm.frame_bytes,
           (size_t)vm.frame_bytes);
}
