/* fb_shim.c — the framebuffer half of the old fbshim-tsc.c, plus the five
 * interposed libc entry points that route everything else.
 *
 * What it still does, and why each one is load-bearing:
 *
 *   FBIOGET_VSCREENINFO -> lies: 1280x800, XRGB at 32 bpp -- the default, and the
 *     depth the player itself runs at -- or RGB565 at 16 bpp when FB_LIE_BPP=16.
 *     rbp and DirectFB both size their layers from this. The lie is what makes
 *     the logical geometry 1280x800 regardless of what the panel actually is,
 *     and the DirectFB fbdev driver reads the *real* geometry with a raw
 *     syscall() to bypass it. Do not "fix" this.
 *   FBIOGET_FSCREENINFO -> line_length = 1280 * the lied bpp / 8: 5120 at 32 bpp,
 *     2560 at 16, because the depth and the pitch must move together.
 *   FBIOPUT_VSCREENINFO -> accepted and ignored: the real mode is the kernel's.
 *   FBIOPAN_DISPLAY -> the frame pacer (see below). Not reached on this unit.
 *   FBIO_WAITFORVSYNC -> the top menu's compositor, before the wait (see below).
 *   /dev/mem -> EACCES, which is how an mmap of it ends up anonymous.
 *   gpiodrv -> one zero byte per read, and poll() sleeps instead of spinning.
 *
 * The pan pacer: vc4's drm_fbdev returns from FBIOPAN_DISPLAY without waiting
 * for vblank, exactly as the Rockchip driver did, so whichever thread drives the
 * present path spins as fast as it can. With rbp's render thread at SCHED_FIFO
 * 98 on core 0, that starved gui_task and took the frame rate to ~7 fps. Pacing
 * each pan to a 60 Hz budget is what fixed it. It is env-gated as PAN_PACER_MS
 * (fractional, default 16.666666; 0 disables it) so the behaviour can be turned
 * off for an experiment without a rebuild.
 *
 * MEASURED 2026-09-29, and it corrects what this comment used to claim: with
 * RB_DFB_PRESENT=off, FBIOPAN_DISPLAY is never issued on this unit at all. Every
 * distinct framebuffer ioctl that reached this switch over a whole session was
 * logged at its first occurrence -- 0x4600 FBIOGET_VSCREENINFO, 0x4602
 * FBIOGET_FSCREENINFO, 0x4601 FBIOPUT_VSCREENINFO, 0x4604 FBIOGETCMAP, 0x4605
 * FBIOPUTCMAP and 0x40044620 FBIO_WAITFORVSYNC -- and 0x4606 is not among them.
 * It is not DirectFB's panstep early-out either: this unit's vc4drmfb reports
 * xpanstep 1 / ypanstep 1, so that test passes; the call simply does not happen.
 * So on this configuration the pacer above is dead code, and the boundary that
 * really paces rbp's render thread is the vsync wait, at a measured 57.1/s. The
 * pacer is kept because it governs the present modes that do pan -- it is what
 * the docs/13 frame-rate fix rests on -- but nothing may hang off it, and the
 * top menu does not: it draws from the vsync case below.
 *
 * Removed in the M2a split: an unused `pthread_mutex_t fb_lock` (dead since the
 * pacer became lock-free — hence "zero mutexes" below), and the touch emulation,
 * which moved to tscfake.c/pointsrc.c. One stale comment went with it: the old
 * file header claimed the transform produced `lx = 1279 - ry*1280/2048`, while
 * transform() in the same file produced `lx = ry*1280/2048`. The code is what
 * ran and what is preserved; the in-code derivation next to it is the reasoned
 * one, and POINT_INVERT_X is the knob if it turns out to be wrong.
 */
#define _GNU_SOURCE
#include "tscfake.h"
#include "fbdev.h"
#include "menu_draw.h"
#include "vidmask.h"        /* the video mask, on the plane above them all */

#include "syscalls.h"
#include "envutil.h"
#include "pointsrc.h"          /* pointsrc_log, for the vsync counter below */
#include "drm_card.h"          /* which DRM card scans out, for the vblank wait */
#include "drmband.h"           /* drm_live_mode(), for the band hold */

#include <stdarg.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/syscall.h>

#define MAX_GPIO_FDS 256
static char is_gpio_fd[MAX_GPIO_FDS];

/* The depth rbp is TOLD the fb is, and the line length that goes with it. 32 is
 * the DEFAULT, and it is what rbp is actually run at: it is the depth the
 * rx3-handoff port renders its UI at, and the depth `.239` was measured to need
 * -- at 16 the deck-1 strip frame's right vertical comes out at 1/4 and 1/8
 * coverage instead of whole. 32 makes DirectFB's AUTO-FALLBACK pick `convert`:
 * a bpp-only mismatch at an unchanged geometry (fbdev.c), which is what lets the
 * layer surface be 32 bpp while the panel stays 16. 16 is kept because it is the
 * RX3's own framebuffer and the configuration everything else was verified on.
 *
 * The fallback here is only reached when the variable is absent ENTIRELY, which
 * the launcher never does -- rb.conf always sets it and start-rb.sh's SHIM_VARS
 * always exports it. It is 32 so that it agrees with rb.conf's default: the two
 * disagreeing is the one way this can run silently wrong, since rbp's own
 * pixel-format word must say the same thing (its DS_HW driver plugin refuses the
 * layer otherwise) and nothing here can see that word. Read the shim-side name:
 * rb.conf's RB_FB_LIE_BPP reaches it through SHIM_VARS. */
static int fb_lie_bpp(void)
{
    return strcmp(env_str("FB_LIE_BPP", "32"), "32") == 0 ? 32 : 16;
}

/* Every consumer of FSCREENINFO (DirectFB's pitch, rbp's stride arithmetic)
 * assumes the logical width, not the panel's. The depth must move with it: the
 * driver tells a format-only mismatch from a geometry one by the PIXEL stride
 * (pitch / bpp), so 32 bpp with a 2560-byte line reads as 640 px against the
 * real 1280 and selects `scale` instead of `convert`. */
#define FB_LOGICAL_LINE_LENGTH (1280 * fb_lie_bpp() / 8)

/* The toolchain's linux/fb.h predates this uapi entry, so it is spelled out here
 * -- and its value is not a guess: _IOW('F', 0x20, __u32) is (1<<30)|(4<<16)|
 * ('F'<<8)|0x20 = 0x40044620, which is the number measured arriving at this
 * switch from rbp's render thread at 57.1/s. The case below is dead code if this
 * is ever wrong, and dead code here means the top menu loses its frame boundary,
 * not that anything breaks. */
#ifndef FBIO_WAITFORVSYNC
#define FBIO_WAITFORVSYNC 0x40044620UL
#endif

#define PAN_PACER_DEFAULT_MS 16.666666

static long pan_pacer_ns(void)
{
    static long cached = -1;
    if (cached < 0) {
        double ms = env_double("PAN_PACER_MS", PAN_PACER_DEFAULT_MS);
        cached = (long)(ms * 1000000.0);
        if (cached < 0)
            cached = 0;
    }
    return cached;
}

/* The frame rate, counted where it is cheapest and on the thread that actually
 * draws it. The FBIO_WAITFORVSYNC case below IS rbp's frame boundary -- that
 * case documents the measurement: one issue per frame, ~57/s -- so counting the
 * calls and printing once a second gives the frame rate directly, rather than
 * inferring it from the process's CPU. That distinction is the whole point of
 * the instrument: it is the one number that says whether a heavier present path
 * costs *frames* or only cycles.
 *
 * Env-gated by FB_VSYNC_RATE, because the read is a clock_gettime and the print
 * is a lock plus a write, on rbp's SCHED_FIFO 98 render thread -- cheap, but not
 * free enough to leave on. Set once and cached: a getenv() in this path is a
 * lock and a write to the process's environment block from the render thread.
 *
 * A DIAGNOSTIC, deliberately, and so it must NOT go into start-rb.sh's
 * SHIM_VARS: that loop exports every name in its list unconditionally, so an
 * ad-hoc `FB_VSYNC_RATE=1 sh start-rb.sh` would be overwritten with the empty
 * string. Set it in the launcher's environment instead -- a systemd drop-in
 * carrying `Environment=FB_VSYNC_RATE=1` reaches rbp, because nothing in the
 * launcher uses `env -i`. Same rule as POINT_MENU_MS and MIDI_DUMP's contents.
 *
 * The window is held in MILLISECONDS IN A 64-BIT TYPE, and that is not style:
 * `long` is 4 bytes in this soft-float ARM32 build, so a nanosecond
 * CLOCK_MONOTONIC held in one wraps every 4.29 s
 * (rbp-32bit-long-clock-wrap). */
static void vsync_rate_tick(void)
{
    static int enabled = -1;
    static unsigned long long window_start_ms;
    static unsigned int frames;
    struct timespec now;
    unsigned long long ms, elapsed;

    if (enabled < 0)
        enabled = env_flag("FB_VSYNC_RATE", 0);
    if (!enabled)
        return;

    frames++;
    clock_gettime(CLOCK_MONOTONIC, &now);
    ms = (unsigned long long)now.tv_sec * 1000ULL
       + (unsigned long long)(now.tv_nsec / 1000000);
    if (window_start_ms == 0) {
        /* The first call starts the window rather than reporting a partial one:
         * a line covering the launch's slow first milliseconds would read as a
         * frame drop that never happened. */
        window_start_ms = ms;
        return;
    }
    elapsed = ms - window_start_ms;
    if (elapsed >= 1000ULL) {
        pointsrc_log("vsync: %u frames in %llu ms = %.1f/s",
                     frames, elapsed, (double)frames * 1000.0 / (double)elapsed);
        frames = 0;
        window_start_ms = ms;
    }
}

/* ---- the real vblank wait (2026-10-25, ODROID-N2+) ------------------------
 *
 * rbp paces its render loop on FBIO_WAITFORVSYNC, once per frame. meson's
 * drm_fbdev returns from it without waiting for vblank, so rbp free-ran on its
 * own timer at 57.7/s against a 60 Hz panel (FB_VSYNC_RATE), and fbpace.py saw
 * frames every ~17.0 ms that drifted past the 16.67 ms refresh: ~2 refreshes a
 * second repeated a frame, which is the "uneven scrolling" the operator filmed.
 *
 * So the wait is done for real, on the DRM device: DRM_IOCTL_WAIT_VBLANK,
 * relative, one vblank, on CRTC 0 (the one HDMI drives here). Defined here
 * rather than taken from libdrm, which this shim does not link. 32-bit ARM:
 * request {u32 type, u32 sequence, ulong signal} / reply {u32, u32, long, long}
 * = a 16-byte union, so _IOWR('d', 0x3a, 16) = 0xC010643A.
 *
 * RB_FB_VBLANK=0 (as FB_VBLANK in the shim's environment) restores the old
 * pass-through. Any failure -- no card, an ioctl error -- turns the wait off for
 * the rest of the run and falls back to the pass-through, logged once. */
union fb_drm_wait_vblank {
    struct { unsigned int type, sequence; unsigned long signal; } request;
    struct { unsigned int type, sequence; long tval_sec, tval_usec; } reply;
};
#define FB_DRM_IOCTL_WAIT_VBLANK 0xC010643AUL
#define FB_DRM_VBLANK_RELATIVE   0x1

/* rbp's own frame limiter, and why it has to go while the vblank wait is on.
 * strace of the render thread (2026-10-25): FBIO_WAITFORVSYNC returns ENOTTY at
 * once, the frame is drawn in ~12.7 ms, and then the thread nanosleeps ~4.7 ms
 * that it computes itself to fill the frame to ~17.4 ms -- a ~57.5 fps limiter,
 * presumably the RX3 panel's rate. Kept beside a real vblank wait it pushes each
 * frame past one refresh, so every frame took two (30 fps, measured). So while
 * the wait is active, the RENDER THREAD's (the one that calls
 * FBIO_WAITFORVSYNC) short sleeps return at once and the panel paces it alone.
 * Every other thread's sleeps are untouched. */
#define FB_LIMITER_MAX_NS 20000000L
static volatile int fb_vblank_on;          /* the DRM wait is live */
static volatile long fb_render_tid;        /* the thread that calls WAITFORVSYNC */

static int fb_is_render_thread(void)
{
    return fb_vblank_on && fb_render_tid &&
           (long)syscall(SYS_gettid) == fb_render_tid;
}

int nanosleep(const struct timespec *req, struct timespec *rem)
{
    if (req && req->tv_sec == 0 && req->tv_nsec < FB_LIMITER_MAX_NS &&
        fb_is_render_thread())
        return 0;
    return (int)syscall(SYS_nanosleep, req, rem);
}

int usleep(useconds_t usec)
{
    struct timespec ts;
    if (usec < FB_LIMITER_MAX_NS / 1000 && fb_is_render_thread())
        return 0;
    ts.tv_sec = usec / 1000000;
    ts.tv_nsec = (long)(usec % 1000000) * 1000L;
    return (int)syscall(SYS_nanosleep, &ts, NULL);
}

/* THE BAND HOLD (2026-10-28, the operator: "the swipe down from top ... flickers"). On
 * this board the menu's band is painted onto rbp's own page (no overlay plane carries
 * it: meson's one overlay takes YUV and sits under the primary), after each rbp frame,
 * from the hook below. But rbp starts its next frame the moment the vblank wait returns,
 * which is just when the top of the screen -- the band -- is being scanned out, and
 * rbp's title bar is among the first things it draws: the scan caught rbp's pixels
 * where the band was. So while the band is open on the page, rbp's return from the wait
 * is held until the scan is past the band's last row: the blank's lines, the band's,
 * and FB_BAND_HOLD_LINES more. rbp's frame is ~13.5 ms of the 16.7 (fb_frame_stats), so
 * ~2 ms fits; it costs nothing with the band shut. FB_BAND_HOLD=0 turns it off. */
#define FB_BAND_HOLD_LINES 8
#define FB_BAND_HOLD_MAX_NS 4000000L

static void fb_band_hold(int drm_fd, const union fb_drm_wait_vblank *vb)
{
    static int timing = -1;                 /* -1 unread, 0 none, 1 read */
    static unsigned int vtotal, vdisplay, line_ns;
    int bottom = menu_page_band_bottom();
    struct timespec until;
    long ns;

    if (bottom <= 0)
        return;
    if (timing < 0) {
        timing = env_flag("FB_BAND_HOLD", 1) &&
                 drm_live_mode(drm_fd, &vtotal, &vdisplay, &line_ns) == 0;
        pointsrc_log(timing ? "fb: band hold on (%u/%u lines, %u ns a line)"
                            : "fb: band hold off", vtotal, vdisplay, line_ns);
    }
    if (!timing)
        return;
    ns = (long)(vtotal - vdisplay + (unsigned)bottom + FB_BAND_HOLD_LINES) * (long)line_ns;
    if (ns <= 0 || ns > FB_BAND_HOLD_MAX_NS)
        return;
    /* from the vblank's own timestamp (CLOCK_MONOTONIC), not from now */
    until.tv_sec = vb->reply.tval_sec;
    until.tv_nsec = vb->reply.tval_usec * 1000L + ns;
    while (until.tv_nsec >= 1000000000L) {
        until.tv_nsec -= 1000000000L;
        until.tv_sec++;
    }
    syscall(SYS_clock_nanosleep, CLOCK_MONOTONIC, TIMER_ABSTIME, &until, NULL);
}

static int fb_vblank_wait(void)
{
    static int state = -1;          /* -1 unknown, 0 off/failed, 1 on */
    static int drm_fd = -1;
    union fb_drm_wait_vblank vb;

    if (state < 0) {
        char path[64];
        state = 0;
        if (env_flag("FB_VBLANK", 1)) {
            drm_card_path(path, sizeof path);
            drm_fd = real_open(path, O_RDWR | O_CLOEXEC, 0);
            if (drm_fd >= 0) {
                state = 1;
                fb_vblank_on = 1;
                pointsrc_log("fb: real vblank wait on %s (FBIO_WAITFORVSYNC paced to the panel)",
                             path);
            } else {
                pointsrc_log("fb: no vblank wait -- cannot open %s (errno %d); pass-through",
                             path, errno);
            }
        }
    }
    if (state != 1)
        return -1;
    memset(&vb, 0, sizeof vb);
    vb.request.type = FB_DRM_VBLANK_RELATIVE;
    vb.request.sequence = 1;
    if (real_ioctl(drm_fd, FB_DRM_IOCTL_WAIT_VBLANK, &vb) != 0) {
        if (errno == EINTR)
            return 0;
        pointsrc_log("fb: DRM vblank wait failed (errno %d); back to pass-through", errno);
        state = 0;
        fb_vblank_on = 0;                  /* rbp's own limiter paces it again */
        return -1;
    }
    fb_band_hold(drm_fd, &vb);
    return 0;
}

/* ---- where a frame's time goes (FB_VSYNC_RATE) -----------------------------
 * Per FBIO_WAITFORVSYNC: t0 entry, t1 after our own per-frame work (the menu
 * and the video mask), t2 after the wait. rbp's draw is t0 minus the previous
 * t2; ours is t1 - t0; the wait is t2 - t1; the whole frame is t2 - prev t2.
 * Printed once a second beside the vsync rate, same gate (a diagnostic). */
static unsigned long long fb_now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned long long)ts.tv_sec * 1000000ULL + (unsigned long long)ts.tv_nsec / 1000ULL;
}

static void fb_frame_stats(unsigned long long t0, unsigned long long t1, unsigned long long t2)
{
    static int enabled = -1;
    static unsigned long long prev2, win;
    static unsigned long long sd, md, so, mo, sw, mw, mf;
    static unsigned int n, over;

    if (enabled < 0)
        enabled = env_flag("FB_VSYNC_RATE", 0);
    if (!enabled)
        return;
    if (prev2) {
        unsigned long long d = t0 > prev2 ? t0 - prev2 : 0, o = t1 - t0, w = t2 - t1,
                           f = t2 - prev2;
        sd += d; so += o; sw += w; n++;
        if (d > md) md = d;
        if (o > mo) mo = o;
        if (w > mw) mw = w;
        if (f > mf) mf = f;
        if (f > 17500) over++;
    }
    prev2 = t2;
    if (!win)
        win = t2;
    if (t2 - win >= 1000000ULL && n) {
        pointsrc_log("frame: n=%u draw avg %.1f max %.1f | ours avg %.2f max %.2f | "
                     "wait avg %.1f max %.1f | frame max %.1f ms, %u over 17.5 ms",
                     n, sd / 1000.0 / n, md / 1000.0, so / 1000.0 / n, mo / 1000.0,
                     sw / 1000.0 / n, mw / 1000.0, mf / 1000.0, over);
        sd = md = so = mo = sw = mw = mf = 0;
        n = over = 0;
        win = t2;
    }
}

int open(const char *pathname, int flags, ...)
{
    mode_t mode = 0;
    va_list ap;
    if (flags & (O_CREAT | O_TMPFILE)) {
        va_start(ap, flags);
        mode = va_arg(ap, mode_t);
        va_end(ap);
    }
    /* rbp's user_space_rtc_init() mmaps /dev/mem to reach a hardware RTC. There
     * is no such mapping here, and refusing the open is what funnels that mmap
     * into the anonymous-memory path the audio shim then relies on. On a Pi it
     * would fail anyway (CONFIG_STRICT_DEVMEM), but only after a kernel warning. */
    if (pathname && strcmp(pathname, "/dev/mem") == 0) {
        errno = EACCES;
        return -1;
    }
    if (pathname && strcmp(pathname, TSC_DEVICE_PATH) == 0)
        return tscfake_open();
    int fd = real_open(pathname, flags, mode);
    if (fd >= 0 && fd < MAX_GPIO_FDS && pathname && strstr(pathname, "gpiodrv"))
        is_gpio_fd[fd] = 1;
    return fd;
}

int open64(const char *pathname, int flags, ...)
{
    mode_t mode = 0;
    va_list ap;
    if (flags & (O_CREAT | O_TMPFILE)) {
        va_start(ap, flags);
        mode = va_arg(ap, mode_t);
        va_end(ap);
    }
    if (pathname && strcmp(pathname, "/dev/mem") == 0) {
        errno = EACCES;
        return -1;
    }
    if (pathname && strcmp(pathname, TSC_DEVICE_PATH) == 0)
        return tscfake_open();
    int fd = real_open(pathname, flags | O_LARGEFILE, mode);
    if (fd >= 0 && fd < MAX_GPIO_FDS && pathname && strstr(pathname, "gpiodrv"))
        is_gpio_fd[fd] = 1;
    return fd;
}

ssize_t read(int fd, void *buf, size_t count)
{
    if (fd >= 0 && fd < MAX_GPIO_FDS && is_gpio_fd[fd] && count >= 1) {
        memset(buf, 0, 1);
        return 1;
    }
    if (tscfake_is_fd(fd))
        return tscfake_read(fd, buf, count);
    return real_read(fd, buf, count);
}

int close(int fd)
{
    if (fd >= 0 && fd < MAX_GPIO_FDS)
        is_gpio_fd[fd] = 0;
    if (tscfake_is_fd(fd))
        return tscfake_close(fd);
    return real_close(fd);
}

int poll(struct pollfd *fds, nfds_t nfds, int timeout)
{
    if (fds && nfds == 1 && fds[0].fd >= 0 && fds[0].fd < MAX_GPIO_FDS && is_gpio_fd[fds[0].fd]) {
        /* GpioManager polls /dev/gpiodrv for hardware interrupts that do not
         * exist here. A regular file is always "ready", so without this the poll
         * returns immediately and the caller spins at 100% CPU — under
         * SCHED_FIFO 98, at the expense of the render and audio threads. */
        if (timeout < 0) {
            sleep(3600);
            return 0;
        } else if (timeout > 0) {
            usleep((useconds_t)timeout * 1000);
            return 0;
        } else {
            return 0;
        }
    }
    return real_poll(fds, nfds, timeout);
}

int ioctl(int fd, unsigned long request, ...)
{
    void *arg;
    va_list ap;
    va_start(ap, request);
    arg = va_arg(ap, void *);
    va_end(ap);

    /* The emulated tsc2007's ioctls. Answered for any fd on purpose: they are
     * unambiguous, and tracking which fd is the fake one across dup() and fork()
     * would be more machinery than the discrimination is worth. */
    if (tscfake_ioctl(request, arg))
        return 0;

    /* Everything outside the framebuffer ioctl range goes straight to the kernel
     * with no locks and no work in between: this is the path ALSA PCM, the ALSA
     * sequencer and every socket take, and it has to stay free for the audio and
     * event threads. (0x4600..0x4620 is the fb range; the tsc ioctls above are
     * 0x6b00-family and are handled before this test.) */
    if ((request & 0xffff) < 0x4600 || (request & 0xffff) > 0x4620)
        return real_ioctl(fd, request, arg);

    switch (request) {
    case FBIOGET_VSCREENINFO: {
        struct fb_var_screeninfo *v = arg;
        int res = real_ioctl(fd, request, v);
        if (res == 0 && v) {
            int bpp = fb_lie_bpp();

            v->xres = 1280; v->yres = 800;
            v->xres_virtual = 1280; v->yres_virtual = 800;
            v->bits_per_pixel = bpp;
            v->grayscale = 0; v->nonstd = 0;
            if (bpp == 32) {
                /* XRGB with no alpha channel, which is DirectFB's DSPF_RGB32 --
                 * the format rbp's DS_HW_Core_Layer_Create asks for when its
                 * pixel-format word carries the handoff's 0x00400c03. transp
                 * stays length 0 on purpose: an 8-bit alpha channel here would
                 * describe ARGB instead, and the whole point is that this and
                 * rbp's word agree. */
                v->red.offset = 16; v->red.length = 8; v->red.msb_right = 0;
                v->green.offset = 8; v->green.length = 8; v->green.msb_right = 0;
                v->blue.offset = 0; v->blue.length = 8; v->blue.msb_right = 0;
                v->transp.offset = 0; v->transp.length = 0; v->transp.msb_right = 0;
            } else {
                v->red.offset = 11; v->red.length = 5; v->red.msb_right = 0;
                v->green.offset = 5; v->green.length = 6; v->green.msb_right = 0;
                v->blue.offset = 0; v->blue.length = 5; v->blue.msb_right = 0;
                v->transp.offset = 0; v->transp.length = 0; v->transp.msb_right = 0;
            }
            v->rotate = 0;
        }
        return res;
    }
    case FBIOGET_FSCREENINFO: {
        struct fb_fix_screeninfo *f = arg;
        int res = real_ioctl(fd, request, f);
        if (res == 0 && f)
            f->line_length = FB_LOGICAL_LINE_LENGTH;
        return res;
    }
    case FBIOPUT_VSCREENINFO:
        return 0;
    case FBIO_WAITFORVSYNC:
        /* The frame boundary rbp actually drives its render loop from on this unit
         * -- measured at 57.1/s -- and, unlike the pan below, the boundary that is
         * really reached. menu_frame_tick() runs BEFORE the wait, not after, and
         * that is the whole reason this is the right side of rbp's frame: whether
         * the loop is `draw; wait` or `wait; draw`, the entry of the wait comes
         * immediately after a completed draw, so the panel is composited just
         * after rbp has finished a frame and then has the whole inter-frame gap to
         * itself. Painting after the wait would put it immediately before the next
         * draw, i.e. under it. menu_draw.c has the rest.
         *
         * The counter first, and it is the same frame boundary read as a rate:
         * with FB_VSYNC_RATE set this prints one "vsync: N frames in M ms" line
         * per second -- on rbp's own render thread, so the number is the rate
         * frames arrive at, not a rate inferred from CPU. */
        {
        unsigned long long t0 = fb_now_us(), t1;
        vsync_rate_tick();
        menu_frame_tick_vsync();
        menu_splash_tick();             /* the start-up splash, over all of it */
        /* THE VIDEO MASK, AND IT BELONGS HERE RATHER THAN IN menu_frame_tick(), which is the
         * mistake this line fixes. That function runs from TWO threads, and the second is a
         * builder loop whose period is POINT_MENU_MS -- shipped at 0.1 ms, i.e. ~8,900 calls a
         * second -- so a mask tick hung inside it put a clock_gettime on the thread the
         * operator's whole UI responsiveness lives on, and made the mask's state a race between
         * two writers. Here it runs ONCE PER rbp FRAME on rbp's own render thread, which is both
         * the correct rate for video and the only writer there is.
         *
         * It is inert unless RB_VIDEO was set: one load and a compare when nobody asked. */
        vidmask_tick();
        t1 = fb_now_us();
        fb_render_tid = (long)syscall(SYS_gettid);
        if (fb_vblank_wait() == 0) {
            fb_frame_stats(t0, t1, fb_now_us());
            return 0;
        }
        {
            int res = real_ioctl(fd, request, arg);
            fb_frame_stats(t0, t1, fb_now_us());
            return res;
        }
        }
    case FBIOPAN_DISPLAY: {
        long pace_ns = pan_pacer_ns();
        if (pace_ns > 0) {
            static struct timespec last_pan;
            struct timespec now;
            clock_gettime(CLOCK_MONOTONIC, &now);
            if (last_pan.tv_sec > 0) {
                long elapsed_ns = (now.tv_sec - last_pan.tv_sec) * 1000000000L
                                + (now.tv_nsec - last_pan.tv_nsec);
                if (elapsed_ns > 0 && elapsed_ns < pace_ns) {
                    struct timespec req;
                    req.tv_sec = 0;
                    req.tv_nsec = pace_ns - elapsed_ns;
                    nanosleep(&req, NULL);
                }
            }
            clock_gettime(CLOCK_MONOTONIC, &last_pan);
        }
        return real_ioctl(fd, request, arg);
    }
    default:
        return real_ioctl(fd, request, arg);
    }
}
