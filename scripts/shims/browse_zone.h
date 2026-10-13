/*
 * browse_zone.h -- a tap on the browse list's RIGHT pane acts on the row you tapped.
 *
 * THE BUG, MEASURED 2026-10-13. rbp's browse screen is two panes: the left is the
 * current level's items, the right is the CONTENTS of the left selection. A tap on
 * the right pane descends into a row -- but always ROW 0, whatever was tapped. The
 * reason is the pane's own cursor, `uiBrowse`'s list-1 cursor at `+0x70`: the tap
 * acts on it, and **nothing in the UI ever moves it**, so it is always 0.
 *
 * The left pane is fine -- a tap there honours the row (verified in rbp's own state:
 * a tap on row 2 moved `cursorNo[0]` 0 -> 2). So this is an asymmetry, not a general
 * failure, and it is rbp's own: no shim zone owns the browse list.
 *
 * THE FIX IS A STATE WRITE, not a gesture. Proved before it was written: with
 * `cursorNo[1]` written to 2 from OUTSIDE rbp, a tap on the right pane went straight
 * into Sundrop and showed its track -- the operator's own expectation, and the row
 * that had been unreachable. So this writes the cursor to the tapped row and then
 * lets the report through; rbp descends into the row the finger actually hit.
 *
 * WHY NOT A ROTATION. `K_SELECTOR` moves the LIST-0 cursor -- the left pane -- so
 * injecting rotations would move the folder selection, changing the right pane's
 * contents entirely. There is no key that reaches list 1. The cursor is written.
 *
 * WHAT THIS DOES NOT DO, and the thing to check on the glass: `browseMode() == 3`
 * covers the folder list AND the track list, because rbp uses one mode for both. The
 * gate here is therefore the mode plus the x range, and a tap in the track view's
 * right-hand columns will move the cursor too. That is either the same fix (a tap
 * selects the track it landed on, so LOAD 1/LOAD 2 target the row you touched) or it
 * is not, and only the operator can say which.
 */
#ifndef RBPI4B_BROWSE_ZONE_H
#define RBPI4B_BROWSE_ZONE_H

/* Feed one pointer report. Writes the right pane's cursor when the report is a press
 * on the browse list's right pane, and otherwise does nothing at all. Never claims
 * the report: every path here leaves rbp to receive it unchanged. */
void browse_zone_note(int down, int logical_x, int logical_y);

#endif /* RBPI4B_BROWSE_ZONE_H */
