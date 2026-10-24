/*
 * test_drm_card.c -- pins drm_card.h: the connector-name parse, the sysfs scan
 * against a fake /sys/class/drm, and the environment overrides.
 */
#include "drm_card.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int fails;
#define CHECK(c, msg) do { if (!(c)) { printf("FAIL: %s\n", msg); fails++; } } while (0)

static void mk(const char *dir, const char *name)
{
    char p[512];
    snprintf(p, sizeof p, "%s/%s", dir, name);
    mkdir(p, 0755);
}

int main(void)
{
    char tmpl[] = "/tmp/drmcardXXXXXX";
    char *dir;
    char buf[64];

    CHECK(drm_card_connector_index("card1-HDMI-A-1") == 1, "card1-HDMI-A-1 is card 1");
    CHECK(drm_card_connector_index("card12-DSI-1") == 12, "two digits");
    CHECK(drm_card_connector_index("card0") == -1, "a bare card has no connector");
    CHECK(drm_card_connector_index("renderD128") == -1, "a render node is not a card");
    CHECK(drm_card_connector_index("card-HDMI") == -1, "no index");
    CHECK(drm_card_connector_index("card1-") == -1, "empty connector name");
    CHECK(drm_card_connector_index("version") == -1, "unrelated entry");

    dir = mkdtemp(tmpl);
    CHECK(dir != NULL, "mkdtemp");
    if (!dir) return 1;

    /* Pi shape: card0 = v3d (no connectors), card1 = vc4 with two HDMI. */
    mk(dir, "card0"); mk(dir, "card1"); mk(dir, "renderD128");
    mk(dir, "card1-HDMI-A-1"); mk(dir, "card1-HDMI-A-2");
    CHECK(drm_card_scan(dir) == 1, "Pi: card1 is the display");

    /* N2+ shape with meson-drm probing first: card0 has the connector. */
    mk(dir, "card0-HDMI-A-1");
    CHECK(drm_card_scan(dir) == 0, "lowest card with a connector wins");

    CHECK(drm_card_scan("/nonexistent/drm") == -1, "unreadable dir is -1");

    setenv("DRM_CARD", "/dev/dri/card7", 1);
    CHECK(strcmp(drm_card_path(buf, sizeof buf), "/dev/dri/card7") == 0, "DRM_CARD wins");
    unsetenv("DRM_CARD");
    setenv("RB_DRM_CARD", "/dev/dri/card5", 1);
    CHECK(strcmp(drm_card_path(buf, sizeof buf), "/dev/dri/card5") == 0, "RB_DRM_CARD next");
    setenv("DRM_CARD", "", 1);
    CHECK(strcmp(drm_card_path(buf, sizeof buf), "/dev/dri/card5") == 0, "empty DRM_CARD is unset");

    {   /* tidy */
        char cmd[600];
        snprintf(cmd, sizeof cmd, "rm -rf '%s'", dir);
        if (system(cmd) != 0) printf("note: could not remove %s\n", dir);
    }
    printf("%s\n", fails ? "test_drm_card: FAILED" : "test_drm_card: ok");
    return fails ? 1 : 0;
}
