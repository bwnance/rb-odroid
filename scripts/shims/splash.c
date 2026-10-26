/*
 * splash.c -- a picture over everything for the first seconds of rbp (splash.h).
 *
 * rbp DRAWS INTO A PRIVATE BUFFER WHILE THE SPLASH IS UP. Painting the splash
 * over rbp's own page was tried first (2026-10-26) and cannot work: rbp reads its
 * page back, so the splash's pixels went into rbp's own buffers and came back out
 * after it ended -- black holes and a stray logo on the performance screen. So
 * rbp's mmap of /dev/fb0 is answered with anonymous memory of the same size while
 * the splash is pending, the splash is drawn on the real framebuffer (through
 * menu_draw.c's own mapping), and at the end -- on rbp's render thread, between
 * two of its frames -- rbp's buffer is copied onto the framebuffer and the real
 * framebuffer is mapped over rbp's address with MAP_FIXED. rbp's pointer never
 * changes; what is behind it does.
 */
#define _GNU_SOURCE
#include "splash.h"

#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#include "envutil.h"
#include "pointsrc.h"      /* pointsrc_log() */
#include "syscalls.h"      /* real_mmap(): the syscall, never this file's mmap() */

#define EXPORT __attribute__((visibility("default")))

#define SPLASH_MS_DEFAULT   8000
#define SPLASH_NO_FRAME_MS  60000         /* rbp never drew: let the controller go */
#define SPLASH_FILE_DEFAULT "/run/rblive4/splash.rgb565"
#define SPLASH_MAX_PX       (1280 * 800)

static volatile int s_done;               /* 1 = over (or off) */
static volatile unsigned long long s_first_ms;   /* rbp's first frame, 0 = not yet */
static unsigned long long s_load_ms;
static long s_ms = SPLASH_MS_DEFAULT;
static uint16_t *s_img;
static int s_iw, s_ih;

/* rbp's framebuffer mapping, while it is diverted */
static void *s_rbp_map;
static size_t s_rbp_len;
static int s_rbp_prot, s_rbp_flags, s_rbp_fd = -1;
static unsigned long long s_rbp_off;

static unsigned long long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned long long)ts.tv_sec * 1000ull + (unsigned long long)ts.tv_nsec / 1000000ull;
}

static void splash_load(void)
{
    const char *path = env_str("SPLASH_FILE", SPLASH_FILE_DEFAULT);
    unsigned char hdr[12];
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    uint16_t w, h;
    size_t n;

    if (fd < 0) {
        pointsrc_log("splash: no picture at %s -- black for %ld ms", path, s_ms);
        return;
    }
    if (read(fd, hdr, sizeof hdr) != (ssize_t)sizeof hdr || memcmp(hdr, "RBSPLSH1", 8))
        goto bad;
    memcpy(&w, hdr + 8, 2);
    memcpy(&h, hdr + 10, 2);
    n = (size_t)w * h;
    if (!w || !h || n > SPLASH_MAX_PX || !(s_img = malloc(n * 2)))
        goto bad;
    if (read(fd, s_img, n * 2) != (ssize_t)(n * 2)) {
        free(s_img);
        s_img = NULL;
        goto bad;
    }
    close(fd);
    s_iw = w;
    s_ih = h;
    pointsrc_log("splash: %dx%d picture for %ld ms", s_iw, s_ih, s_ms);
    return;
bad:
    close(fd);
    pointsrc_log("splash: %s is not a splash picture -- black for %ld ms", path, s_ms);
}

static uint32_t to32(uint16_t v)
{
    uint32_t r = (v >> 11) & 31, g = (v >> 5) & 63, b = v & 31;
    return 0xff000000u | ((r << 3 | r >> 2) << 16) | ((g << 2 | g >> 4) << 8) | (b << 3 | b >> 2);
}

void splash_paint(void *out, int pitch, int w, int h, int bpp,
                  const uint16_t *img, int iw, int ih)
{
    int x0, y0, y;

    for (y = 0; y < h; y++)
        memset((char *)out + (size_t)y * pitch * (bpp / 8), 0, (size_t)w * (bpp / 8));
    if (!img || iw > w || ih > h)
        return;
    x0 = (w - iw) / 2;
    y0 = (h - ih) / 2;
    for (y = 0; y < ih; y++) {
        const uint16_t *src = img + (size_t)y * iw;
        if (bpp == 16) {
            memcpy((uint16_t *)out + (size_t)(y0 + y) * pitch + x0, src, (size_t)iw * 2);
        } else {
            uint32_t *dst = (uint32_t *)out + (size_t)(y0 + y) * pitch + x0;
            for (int x = 0; x < iw; x++)
                dst[x] = 0xff000000u | (to32(src[x]) & 0x00ffffffu);
        }
    }
}

/* rbp's buffer onto the framebuffer, and the framebuffer back behind rbp's
 * pointer. Called once, at the end, on rbp's render thread. */
static void splash_return_fb(void)
{
    void *real;

    if (!s_rbp_map)
        return;
    real = real_mmap(NULL, s_rbp_len, PROT_READ | PROT_WRITE, MAP_SHARED, s_rbp_fd, s_rbp_off);
    if (real != (void *)-1) {
        memcpy(real, s_rbp_map, s_rbp_len);
        syscall(SYS_munmap, real, s_rbp_len);
    } else {
        pointsrc_log("splash: could not map the framebuffer to copy rbp's page onto it");
    }
    if (real_mmap(s_rbp_map, s_rbp_len, s_rbp_prot, s_rbp_flags | MAP_FIXED, s_rbp_fd,
                  s_rbp_off) == (void *)-1)
        pointsrc_log("splash: could not give rbp its framebuffer back -- it keeps "
                     "drawing where nothing is shown");
    else
        pointsrc_log("splash: rbp's framebuffer is its own again");
    close(s_rbp_fd);
    s_rbp_fd = -1;
    s_rbp_map = NULL;
}

int splash_frame(void *page, int pitch, int w, int h, int bpp, unsigned long long t)
{
    if (s_done)
        return 0;
    if (!s_first_ms) {
        s_first_ms = t ? t : 1;
        splash_load();
        if (!s_rbp_map)
            pointsrc_log("splash: rbp's framebuffer was not diverted -- nothing is "
                         "drawn, only the controller is held");
    }
    if (t - s_first_ms >= (unsigned long long)s_ms) {
        s_done = 1;
        splash_return_fb();
        free(s_img);
        s_img = NULL;
        pointsrc_log("splash: over, %ld ms after rbp's first frame", s_ms);
        return 0;
    }
    if (!s_rbp_map || !page || (bpp != 16 && bpp != 32))
        return 0;
    splash_paint(page, pitch, w, h, bpp, s_img, s_iw, s_ih);
    return 1;
}

EXPORT int shim_splash_active(void)
{
    if (s_done)
        return 0;
    /* rbp never drew: the controller goes back to the operator. The framebuffer is
     * handed back only by splash_frame(), on rbp's own render thread. */
    if (!s_first_ms && now_ms() - s_load_ms >= SPLASH_NO_FRAME_MS)
        return 0;
    return 1;
}

/* ---- rbp's mmap ------------------------------------------------------------ */

/* By the descriptor's path, not fstat(): this library is built with newer headers
 * than rbp's glibc 2.13, and fstat() answered "not a framebuffer" for rbp's own fd
 * of /dev/fb0 (measured on the unit 2026-10-26). */
static int is_fb(int fd)
{
    char link[32], path[128];
    ssize_t n;

    if (fd < 0)
        return 0;
    snprintf(link, sizeof link, "/proc/self/fd/%d", fd);
    n = readlink(link, path, sizeof path - 1);
    if (n <= 0)
        return 0;
    path[n] = '\0';
    return strstr(path, "/dev/fb") != NULL;
}

static void *splash_mmap(void *addr, size_t len, int prot, int flags, int fd,
                         unsigned long long off)
{
    /* audioshim.so's fix, kept here because this library is ahead of it in
     * RB_LD_PRELOAD and so answers rbp's mmap first: rbp's user_space_rtc_init()
     * maps fd -1 MAP_SHARED after /dev/mem is refused, and that has to succeed as
     * zeroed private memory or it crashes later (audioshim.c has the story). */
    if (fd < 0 && (flags & MAP_SHARED) && !(flags & MAP_ANONYMOUS))
        return real_mmap(addr, len, prot, (flags & ~MAP_SHARED) | MAP_PRIVATE | MAP_ANONYMOUS,
                         -1, 0);

    if (!s_done && !s_rbp_map && (flags & MAP_SHARED) && is_fb(fd)) {
        void *anon = real_mmap(addr, len, prot,
                               (flags & ~MAP_SHARED) | MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        int keep = fcntl(fd, F_DUPFD_CLOEXEC, 0);

        /* No copy of what is on the framebuffer now: rbp clears its page itself, and
         * reading a fresh fbdev mapping this early faulted partway (crash.log,
         * 2026-10-26). */
        if (anon != (void *)-1 && keep >= 0) {
            s_rbp_map = anon;
            s_rbp_len = len;
            s_rbp_prot = prot;
            s_rbp_flags = flags & ~MAP_FIXED;
            s_rbp_fd = keep;
            s_rbp_off = off;
            pointsrc_log("splash: rbp's framebuffer mapping (%zu bytes) is private "
                         "at %p until the splash is over", len, anon);
            return anon;
        }
        if (anon != (void *)-1)
            syscall(SYS_munmap, anon, len);
        if (keep >= 0)
            close(keep);
    }
    if (is_fb(fd)) {
        void *m = real_mmap(addr, len, prot, flags, fd, off);
        pointsrc_log("splash: rbp maps the framebuffer directly at %p (%zu bytes, flags %#x)",
                     m, len, flags);
        return m;
    }
    return real_mmap(addr, len, prot, flags, fd, off);
}

/* DirectFB maps the framebuffer, unmaps it and maps it again (two maps of rbp's fd
 * at start, 2026-10-26). A diversion whose mapping rbp has let go of must be
 * forgotten -- its addresses go to whatever is mapped next, and handing the
 * framebuffer back over them with MAP_FIXED crashed rbp (a jump into a mapping
 * that was no longer code, crash.log). The next framebuffer map is diverted
 * instead. */
EXPORT int munmap(void *addr, size_t len)
{
    if (s_rbp_map && addr == s_rbp_map) {
        s_rbp_map = NULL;
        close(s_rbp_fd);
        s_rbp_fd = -1;
        pointsrc_log("splash: rbp unmapped its diverted framebuffer");
    }
    return (int)syscall(SYS_munmap, addr, len);
}

EXPORT void *mmap(void *addr, size_t len, int prot, int flags, int fd, off_t off)
{
    return splash_mmap(addr, len, prot, flags, fd, (unsigned long long)(unsigned long)off);
}

EXPORT void *mmap64(void *addr, size_t len, int prot, int flags, int fd, off64_t off)
{
    return splash_mmap(addr, len, prot, flags, fd, (unsigned long long)off);
}

__attribute__((constructor)) static void splash_init(void)
{
    s_load_ms = now_ms();
    s_ms = atol(env_str("SPLASH_MS", "8000"));
    if (s_ms <= 0)
        s_done = 1;
}
