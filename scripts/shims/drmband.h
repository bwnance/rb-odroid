/*
 * drmband.h -- the menu band, on a vc4 overlay plane instead of in rbp's buffer.
 *
 * WHY. The band was *repaired*, not *presented*: menu_draw.c rewrites the top
 * strip that rbp's own 57.1 Hz repaint keeps erasing, so the miss run is bounded
 * by the repair tick and no tick setting can cure it (0.1 ms -> 99.07 % duty,
 * 16.67 ms -> 53.83 %, i.e. 49x worse; the curve is in menu_draw.c). The cure is
 * to put the band somewhere rbp's redraw cannot reach, and on this device that is
 * a second DRM plane: rbp draws through /dev/fb0, which mmaps the *primary*
 * plane's dumb buffer, so its "repaint" is CPU writes into that memory and not a
 * mode commit. A plane scanning out of its own buffer is untouched by them.
 *
 * PROVEN BEFORE IT WAS WIRED IN. work/drmplane.c held a static bar on the left
 * half of the strip while rbp redrew underneath it; the kernel state showed both
 * planes on the same CRTC with ours above (plane 127 fb 723 zpos 1 over plane 91
 * fb 722 zpos 0, both on pixelvalve-2), and the operator's eye settled it:
 * "yep. i see the magenta, its solid and the other side flickers". Taking DRM
 * master did NOT disturb the mode DirectFB set up -- the primary stayed at fb 722
 * throughout, which was the stated reason this had never been attempted.
 *
 * THE ONE THING THAT MAKES THIS SMALL. The band is already drawn in the
 * framebuffer's own format, which here is RGB565 -- the fourcc the kernel prints
 * as `RG16` in /sys/kernel/debug/dri/1/state -- and RGB565 is one of the 39
 * formats the vc4 overlays list. So the plane's buffer takes the same pixels at
 * the same pitch: menu_paint() is unchanged, and the only thing that moves is
 * where its writes land. (32 bpp is XRGB8888, which the planes also take, first
 * in their list.)
 *
 * WHY THE STRUCTS BELOW ARE WRITTEN OUT. They are KERNEL uapi -- the contents of
 * the kernel tree's include/uapi/drm/ -- and the rest of this shim takes its
 * kernel structures from kernel headers (<linux/fb.h>, <linux/input.h>) rather
 * than from libraries. Debian's linux-libc-dev does not ship the drm ones, so the
 * only alternatives are the libdrm-dev package, which is arch-specific and would
 * install a host-architecture .so into an armel build (the trap
 * tools/build-toolchain/Dockerfile already refuses for libasound2-dev), or these
 * thirteen frozen structs. They are ABI, not implementation: the ioctl NUMBER
 * encodes each struct's size, so they cannot drift without the kernel's own
 * headers drifting, and the numbers below were read out of libdrm 2.4.114 rather
 * than typed from memory. Nothing here is linked -- every call is a raw ioctl.
 */
#ifndef RBPI4B_DRMBAND_H
#define RBPI4B_DRMBAND_H

#include <stddef.h>
#include <sys/ioctl.h>

typedef unsigned char      __u8;
typedef unsigned short     __u16;
typedef unsigned int       __u32;
typedef unsigned long long __u64;
typedef signed int         __s32;

/* ------------------------------------------------------------------ uapi */

#define DRM_IOCTL_BASE           'd'
#define DRM_IO(nr)               _IO(DRM_IOCTL_BASE, nr)
#define DRM_IOW(nr, type)        _IOW(DRM_IOCTL_BASE, nr, type)
#define DRM_IOWR(nr, type)       _IOWR(DRM_IOCTL_BASE, nr, type)

#define DRM_IOCTL_SET_MASTER            DRM_IO(0x1e)
#define DRM_IOCTL_DROP_MASTER           DRM_IO(0x1f)
#define DRM_IOCTL_SET_CLIENT_CAP        DRM_IOW(0x0d, struct drm_set_client_cap)

#define DRM_IOCTL_MODE_GETRESOURCES     DRM_IOWR(0xA0, struct drm_mode_card_res)
#define DRM_IOCTL_MODE_GETCRTC          DRM_IOWR(0xA1, struct drm_mode_crtc)
#define DRM_IOCTL_MODE_GETPROPERTY      DRM_IOWR(0xAA, struct drm_mode_get_property)
#define DRM_IOCTL_MODE_ADDFB            DRM_IOWR(0xAE, struct drm_mode_fb_cmd)
#define DRM_IOCTL_MODE_RMFB             DRM_IOWR(0xAF, __u32)
#define DRM_IOCTL_MODE_CREATE_DUMB      DRM_IOWR(0xB2, struct drm_mode_create_dumb)
#define DRM_IOCTL_MODE_MAP_DUMB         DRM_IOWR(0xB3, struct drm_mode_map_dumb)
#define DRM_IOCTL_MODE_DESTROY_DUMB     DRM_IOWR(0xB4, struct drm_mode_destroy_dumb)
#define DRM_IOCTL_MODE_GETPLANERESOURCES DRM_IOWR(0xB5, struct drm_mode_get_plane_res)
#define DRM_IOCTL_MODE_GETPLANE         DRM_IOWR(0xB6, struct drm_mode_get_plane)
#define DRM_IOCTL_MODE_SETPLANE         DRM_IOWR(0xB7, struct drm_mode_set_plane)
#define DRM_IOCTL_MODE_OBJ_GETPROPERTIES DRM_IOWR(0xB9, struct drm_mode_obj_get_properties)
/* THE KERNEL'S OWN ENCODING, SPELLED OUT BECAUSE GETTING IT WRONG IS INVISIBLE. This line read
 * DRM_IOW(0xBC, ...) until 2026-10-12 -- a DIFFERENT COMMAND, since the number and the direction
 * are part of the ioctl word. Nothing called it, so nothing failed; the first caller would have got
 * ENOTTY out of a driver that supports the call perfectly well, and ENOTTY here reads exactly like
 * "this driver will not let you set properties". It is what the ARGB plane's alpha and blend mode
 * are set through, so it is the difference between a faded ring and a diagnosis that is wrong. */
#define DRM_IOCTL_MODE_OBJ_SETPROPERTY   DRM_IOWR(0xBA, struct drm_mode_obj_set_property)

/* The "type" property's enum ordinals. libdrm spells these in xf86drmMode.h,
 * which is a library header and not uapi; they are the kernel enum's order. */
#define DRM_PLANE_TYPE_OVERLAY  0
#define DRM_PLANE_TYPE_PRIMARY  1
#define DRM_PLANE_TYPE_CURSOR   2

#define DRM_MODE_OBJECT_PLANE   0xeeeeeeee
#define DRM_CLIENT_CAP_UNIVERSAL_PLANES 2

/* HOW MANY OF THE FIRST FREE OVERLAY PLANES TO PASS OVER, for every surface that asks for one.
 *
 * NOT SUPERSTITION -- MEASURED 2026-10-12. The deck badge's rings ask for DRM_RANK_RING, i.e. the
 * FIRST free overlay, which on this unit is plane 127 (deck 1) and 138 (deck 2). The kernel's own
 * state dump reports those two as bound, enabled, at the right coordinates, on the same CRTC, in
 * the same RG16 format as surfaces that DO reach the screen -- and nothing they contain ever
 * appears on it. The menu band (plane 193) and the side drawers (171, 182) share every one of those
 * properties and are visible.
 *
 * The proof that the PLANE is what differs, rather than the geometry or the surface: a probe put the
 * badge's plane at exactly the band's shape and place (1280x56 at 0,0, magenta) and it stayed
 * invisible on plane 127 -- and appeared on the screen the moment the same build asked for a late
 * rank and landed on plane 215. Same geometry, same format, same process, same rank machinery, same
 * first-to-set-up surface. Only the plane.
 *
 * So every surface steps over the first DRM_BAND_SKIP_OVERLAYS candidates, which keeps the RELATIVE
 * order the ranks exist for (a badge still lands below a drawer, a drawer below the band) while
 * moving all of them clear of the ones that go nowhere. Four rather than two because 171 is the
 * first plane this project has actually seen work, and stepping over two unverified planes costs
 * nothing. */
#define DRM_BAND_SKIP_OVERLAYS  4

#define DRM_DISPLAY_MODE_LEN 32
#define DRM_PROP_NAME_LEN    32

#define DRM_FORMAT_RGB565    0x36314752   /* 'R''G''1''6' -- fb0's own format */
#define DRM_FORMAT_XRGB8888  0x34325258   /* 'X''R''2''4' */

struct drm_mode_modeinfo {
    __u32 clock;
    __u16 hdisplay, hsync_start, hsync_end, htotal, hskew;
    __u16 vdisplay, vsync_start, vsync_end, vtotal, vscan;
    __u32 vrefresh;
    __u32 flags;
    __u32 type;
    char  name[DRM_DISPLAY_MODE_LEN];
};

struct drm_mode_card_res {
    __u64 fb_id_ptr;
    __u64 crtc_id_ptr;
    __u64 connector_id_ptr;
    __u64 encoder_id_ptr;
    __u32 count_fbs;
    __u32 count_crtcs;
    __u32 count_connectors;
    __u32 count_encoders;
    __u32 min_width, max_width, min_height, max_height;
};

struct drm_mode_crtc {
    __u64 set_connectors_ptr;
    __u32 count_connectors;
    __u32 crtc_id;
    __u32 fb_id;
    __u32 x, y;
    __u32 gamma_size;
    __u32 mode_valid;
    struct drm_mode_modeinfo mode;
};

struct drm_mode_get_plane_res {
    __u64 plane_id_ptr;
    __u32 count_planes;
};

struct drm_mode_get_plane {
    __u32 plane_id;
    __u32 crtc_id;
    __u32 fb_id;
    __u32 possible_crtcs;
    __u32 gamma_size;
    __u32 count_format_types;
    __u64 format_type_ptr;
};

struct drm_mode_create_dumb {
    __u32 height;
    __u32 width;
    __u32 bpp;
    __u32 flags;
    __u32 handle;
    __u32 pitch;
    __u64 size;
};

struct drm_mode_map_dumb {
    __u32 handle;
    __u32 pad;
    __u64 offset;
};

struct drm_mode_destroy_dumb {
    __u32 handle;
};

struct drm_mode_fb_cmd {
    __u32 fb_id;
    __u32 width;
    __u32 height;
    __u32 pitch;
    __u32 bpp;
    __u32 depth;
    __u32 handle;
};

struct drm_mode_set_plane {
    __u32 plane_id;
    __u32 crtc_id;
    __u32 fb_id;
    __u32 flags;
    __s32 crtc_x, crtc_y;
    __u32 crtc_w, crtc_h;
    __u32 src_x, src_y, src_h, src_w;   /* 16.16 fixed point */
};

struct drm_set_client_cap {
    __u64 capability;
    __u64 value;
};

struct drm_mode_get_property {
    __u64 values_ptr;
    __u64 enum_blob_ptr;
    __u32 prop_id;
    __u32 flags;
    char  name[DRM_PROP_NAME_LEN];
    __u32 count_values;
    __u32 count_enum_blobs;
};

struct drm_mode_obj_get_properties {
    __u64 props_ptr;
    __u64 prop_values_ptr;
    __u32 count_props;
    __u32 obj_id;
    __u32 obj_type;
};

/* The one SET ioctl this shim needs, for the stacking order below. Same provenance as the rest:
 * read out of libdrm's uapi rather than typed from memory, and frozen because the ioctl number
 * encodes the struct's size. */
struct drm_mode_obj_set_property {
    __u64 value;
    __u32 prop_id;
    __u32 obj_id;
    __u32 obj_type;
};

/* ------------------------------------------------------------------- band */

/* One overlay plane, held open, with a buffer the band is drawn into. Filled in
 * only by drm_band_setup(), and only ever partially on failure -- teardown is
 * safe to call on any state, including all-zero, which is what lets the caller
 * fall back to the fb0 path with one `if`. */
struct drm_band {
    int           fd;
    unsigned int  crtc;         /* the live CRTC this plane sits on */
    unsigned int  plane;        /* the overlay plane we took */
    unsigned int  fb;           /* framebuffer object wrapping the dumb buffer */
    unsigned int  handle;       /* ...and the dumb buffer's handle */
    void         *pix;          /* its mapping: what menu_paint() writes into */
    size_t        map_len;
    int           pitch;        /* PIXELS per row, as struct menu_view counts it */
    int           w, h;         /* the buffer, in pixels */
    int           bpp;
    int           master;       /* the SHARED device holds DRM master (drmband.c) */
    int           on;           /* the plane is currently scanning out our buffer */
    int           on_x, on_y;   /* ...at this crtc position */
};

/* Open /dev/dri/card1, find the live CRTC, take an overlay plane on it that can
 * carry `bpp`, create a w x h buffer and map it. Takes DRM master here -- once per
 * DEVICE, not once per band -- so that a machine where any of this fails says so at
 * start-up and the caller can fall back before a single pixel is drawn.
 *
 * MAY BE CALLED MORE THAN ONCE. Each call gets its own buffer, framebuffer and
 * plane; the card, the capability and master are shared and refcounted, so a second
 * and third band (the two edge drawers) go up alongside the first. That is the whole
 * reason the device holder exists: a second open+SET_MASTER was refused EBUSY.
 *
 * Returns 0 when `b->pix` is a buffer the band can be drawn into, and -1
 * otherwise, having changed nothing that needs undoing beyond drm_band_teardown().
 * Every failure is logged with its errno through pointsrc_log(), the shim's own
 * logger, so a machine that cannot have the plane says WHY in the log the rest of
 * the display code already writes to. */
int  drm_band_setup(struct drm_band *b, int w, int h, int bpp);

/* The same, but the caller NAMES the format. One caller needs this and it is worth its own entry
 * point: DRM_FORMAT_ARGB8888 carriers carry per-pixel alpha, so a surface can draw ONLY ITS OWN
 * pixels and let rbp's show through the rest -- where the bpp-derived formats (RGB565, XRGB8888)
 * are opaque, which is why every one of them has to COPY rbp's pixels into its buffer first.
 *
 * `bpp` is still passed because the buffer geometry is in bytes-per-pixel terms (pitch, the
 * ADDFB depth) and the two must agree; the fourcc decides the format word and the depth.
 * drm_band_setup() is this with the fourcc derived from bpp, so the two cannot drift. */
int  drm_band_setup_fmt(struct drm_band *b, int w, int h, int bpp, unsigned int fourcc);

/* ------------------------------------------------------------- the stacking order
 *
 * HIGHER SCANS OUT ON TOP, and rbp's own picture is 0. These are ASKED FOR, and the asking is the
 * whole point: the kernel's default zpos is the plane's INDEX, so without this the order is
 * whichever order the planes happened to be picked in -- which put the top menu band UNDER the
 * deck badges' play rings, and would move again the first time a surface was created in a
 * different sequence. Set with drm_band_want_z() immediately before the band's drm_band_setup().
 *
 * The order, and why: the rings are over rbp's picture and under everything the shim draws, so a
 * panel sliding out covers them; the panels are under the boxes that sit on top of them; and the
 * band is over all of them, which is what the operator asked for by name ("the band should be
 * above deck 1, 2 rings") -- it spans the full width at the top, so the drawers' full-height edge
 * panels would otherwise cover its ends. The browser window is a window: it goes over the lot. */
#define DRM_RANK_RING      0     /* the first free overlay -- the LOWEST, so panels cover it */
#define DRM_RANK_DRAWER    2
#define DRM_RANK_PROMPT    3
#define DRM_RANK_FXLIST    4
#define DRM_RANK_FXPAD     5
#define DRM_RANK_BAND      6     /* the operator's ask: above both deck rings */
#define DRM_RANK_WINDOW    7

/* The ranking the NEXT drm_band_setup() in this process will ask for: it takes the (n+1)-th free
 * overlay rather than the first. Consumed by that call, so it cannot leak onto a later band.
 *
 * WHY A RANK AND NOT A zpos. The obvious way is to SET the plane's zpos property, and an earlier
 * version of this asked for exactly that -- the kernel refused it with EINVAL on all three planes,
 * so nothing was enforced and the order was whatever the planes happened to be picked in. What is
 * MEASURED instead, from /sys/kernel/debug/dri/1/state with the surfaces up, is that the driver's
 * DEFAULT zpos follows the plane's index: our three planes read normalized-zpos 1, 2 and 3 in the
 * order they were taken. So taking a later plane is taking a higher one, and that is a fact about
 * this driver rather than a hope -- but a driver that ordered them differently would show it as a
 * surface in the wrong place, so if that ever happens, this is the assumption to re-measure. */
void drm_band_want_rank(unsigned int n);

/* Put the plane on the glass at crtc position (x, y), showing the whole buffer.
 * The buffer must already hold the image. Idempotent while it is already on at
 * the same place, so a tick may call it every frame. Returns 0 on success. */
int  drm_band_show(struct drm_band *b, int x, int y);

/* Take it off. The band is opaque -- RGB565 has no alpha -- so this is what
 * happens the moment the menu closes, and it is not optional: a plane left on
 * covers rbp's UI with a stale strip for the life of the process. */
void drm_band_hide(struct drm_band *b);

/* Hide, then release everything in reverse order. Safe on any state, and safe
 * twice. The kernel would do all of this itself when the fd closes at process
 * exit -- this exists so the band can also be given up while the process lives.
 *
 * Releases only THIS band: its framebuffer, its map and its dumb buffer go, and the
 * shared device is dropped with them only when no other band still holds it. So one
 * drawer closing leaves the other drawer's plane on the glass. */
void drm_band_teardown(struct drm_band *b);

#endif /* RBPI4B_DRMBAND_H */
