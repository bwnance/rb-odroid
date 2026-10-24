/*
 * drm_card.h -- which /dev/dri/cardN carries the display. Header-only.
 *
 * The Pi 4B's answer is card1: card0 is v3d, a render-only device with no CRTC,
 * and the shims used to hardcode that. The ODROID-N2+ has meson-drm (display) and
 * panfrost (render-only), and which of them is card0 depends on probe order, so
 * the answer there is found rather than written down.
 *
 * Order:
 *   1. $DRM_CARD, then $RB_DRM_CARD (start-rb.sh maps rb.conf's RB_DRM_CARD to
 *      DRM_CARD for the shims; vncserve runs on the host with RB_* as-is).
 *   2. The lowest-numbered card with at least one connector under /sys/class/drm
 *      ("card1-HDMI-A-1" and the like). Only KMS devices have connectors, so a
 *      render-only GPU is never picked. Reading sysfs, not opening the node,
 *      matters: the first open of a primary node is granted DRM master, and
 *      probing every card that way could take the display from whoever needs it.
 *   3. /dev/dri/card1, the historical answer, if sysfs says nothing (a chroot
 *      without /sys bound, for instance).
 *
 * A copy of this file lives in scripts/device/vncserve/ (the deploy tree builds
 * vncserve without scripts/shims); keep the two byte-identical.
 */
#ifndef DRM_CARD_H
#define DRM_CARD_H

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DRM_CARD_FALLBACK "/dev/dri/card1"

/* Parse "cardN-<connector>" into N; -1 for anything else ("card1", "renderD128",
 * "version"). Split out so a host test can pin it without a /sys. */
static inline int drm_card_connector_index(const char *name)
{
    int n = 0, digits = 0;

    if (strncmp(name, "card", 4) != 0)
        return -1;
    name += 4;
    while (*name >= '0' && *name <= '9') {
        n = n * 10 + (*name - '0');
        name++;
        digits++;
        if (digits > 3)
            return -1;
    }
    if (digits == 0 || *name != '-' || name[1] == '\0')
        return -1;
    return n;
}

/* The lowest card index with a connector in SYSDIR (normally /sys/class/drm), or
 * -1 when there is none or the directory cannot be read. */
static inline int drm_card_scan(const char *sysdir)
{
    DIR *d = opendir(sysdir);
    struct dirent *e;
    int best = -1;

    if (!d)
        return -1;
    while ((e = readdir(d)) != NULL) {
        int n = drm_card_connector_index(e->d_name);
        if (n >= 0 && (best < 0 || n < best))
            best = n;
    }
    closedir(d);
    return best;
}

/* The card path, written into BUF (always NUL-terminated) and returned. */
static inline const char *drm_card_path(char *buf, size_t len)
{
    const char *env = getenv("DRM_CARD");
    int n;

    if (!env || !*env)
        env = getenv("RB_DRM_CARD");
    if (env && *env) {
        snprintf(buf, len, "%s", env);
        return buf;
    }
    n = drm_card_scan("/sys/class/drm");
    if (n >= 0)
        snprintf(buf, len, "/dev/dri/card%d", n);
    else
        snprintf(buf, len, "%s", DRM_CARD_FALLBACK);
    return buf;
}

#endif /* DRM_CARD_H */
