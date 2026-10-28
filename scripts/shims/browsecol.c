/* SPDX-License-Identifier: MPL-2.0
 *
 * browsecol.c -- Browse Columns: a third column in BROWSE's track lists.
 *
 * rbp's BROWSE track list shows two fields; this adds a third at the right --
 * ARTIST, BPM, KEY or TIME -- with its heading, rbp's two columns as they are.
 * Folder and category lists stay rbp's own. While it is on, the floating
 * LOAD 1 / LOAD 2 under the selected row give their room to it (the FLX10's own
 * LOAD keys still load), and the selected row's long text scrolls short of it.
 *
 * Adapted from rx3-toolkit (github.com/Tratosca/rx3-toolkit at cc17a1f,
 * MPL-2.0): mod/modules/browse-columns/rx3_browse_columns_module.c; the rows,
 * the drawing and the scrolling are browserows.c. Its tap-a-heading sort is not
 * ported. This file stays under the MPL.
 *
 * RB_BROWSE_COLUMN=artist|album|bpm|key|time (or the field number, 7 2 13 15 11); empty
 * or 0 = off, BROWSE as rbp ships it.
 */
#include <stdint.h>
#include <string.h>
#include <strings.h>

#include "browserows.h"
#include "envutil.h"
#include "rbp_key.h"        /* is_rbp_process() */
#include "shimutil.h"       /* klog() */

static const uint16_t bpm[] = { 'B', 'P', 'M', 0 }, key[] = { 'K', 'E', 'Y', 0 },
                      artist[] = { 'A', 'R', 'T', 'I', 'S', 'T', 0 },
                      duration[] = { 'T', 'I', 'M', 'E', 0 },
                      album[] = { 'A', 'L', 'B', 'U', 'M', 0 };
static struct browse_column selected;

/* RB_BROWSE_COLUMN's value as a field and its heading: 0 for off or unknown. Pure,
 * for the test. */
unsigned int browsecol_field(const char *value, const uint16_t **caption)
{
    static const struct { const char *name, *number; unsigned field; const uint16_t *caption; } names[] = {
        { "artist", "7", BROWSE_FIELD_ARTIST, artist }, { "album", "2", BROWSE_FIELD_ALBUM, album },
        { "bpm", "13", BROWSE_FIELD_BPM, bpm }, { "key", "15", BROWSE_FIELD_KEY, key },
        { "time", "11", BROWSE_FIELD_TIME, duration },
        { "length", "11", BROWSE_FIELD_TIME, duration },
    };
    if (!value)
        return 0;
    for (unsigned i = 0; i < sizeof(names) / sizeof(*names); i++)
        if (!strcasecmp(value, names[i].name) || !strcmp(value, names[i].number)) {
            *caption = names[i].caption;
            return names[i].field;
        }
    return 0;
}

__attribute__((constructor)) static void browsecol_init(void)
{
    const char *value = env_str("BROWSE_COLUMN", "");
    if (!value[0] || !strcmp(value, "0") || !is_rbp_process())
        return;
    selected.field = browsecol_field(value, &selected.caption);
    if (!selected.field) {
        klog("knobshim2: browse column: RB_BROWSE_COLUMN=%s is not artist, album, bpm, key or time -- off\n",
             value);
        return;
    }
    if (browserows_column(&selected)) {
        klog("knobshim2: browse column: BROWSE's row builder or renderer is not the one measured -- off\n");
        return;
    }
    klog("knobshim2: browse column: %s, third in BROWSE's track lists\n", value);
}
