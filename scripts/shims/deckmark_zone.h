/*
 * deckmark_zone.h -- WHERE the playing-deck bar goes. Geometry only: a deck in, a rectangle out.
 *
 * WHY A BAR AND NOT THE RING. The badge ring is a 2-pixel border around a 95x50 box, and the
 * operator watches this unit from about 20 feet: at that distance on a 10" panel a 2-pixel line is
 * under 0.3 mm and below what the eye resolves. The bar is 619x20 per deck -- big enough to read
 * across a room -- and it is drawn into rbp's OWN framebuffer (see deckmark.c), so its size costs
 * nothing but the pixels.
 *
 * WHY THE BOTTOM 20 ROWS, which is a MEASUREMENT and not a preference. A live capture of /dev/fb0
 * was scanned row by row across both deck strips: deck 1's own content ends at y 778 (a band of
 * ~490 lit pixels at y 770..778), and rows **780..799 are entirely black on both decks**. So the
 * bar covers nothing rbp draws anywhere -- it is rbp's own empty margin -- and it therefore cannot
 * hide the title, the pads, the tempo readout or the waveform. The strips themselves start at
 * y 581 (hc_zone.h's measured table) and run to the bottom of the screen.
 */
#ifndef RBPI4B_DECKMARK_ZONE_H
#define RBPI4B_DECKMARK_ZONE_H

/* The bar's rows. Two numbers because the caller may move them: the default is the measured empty
 * band, and the operator can slide it without a rebuild (DECKMARK_BAR=y0,y1 in deckmark.c). */
#define DECKMARK_Y0_DEFAULT 780
#define DECKMARK_Y1_DEFAULT 799

/* The deck strips' columns, from hc_zone.h's table (deck 1's four pad cells span 11..629, and deck
 * 2 is the same +640). The bar spans its deck's strip and one pixel more than nothing: it is a
 * marker for that deck, so it must not reach into the other's. */
#define DECKMARK_DECK1_X0 11
#define DECKMARK_DECK1_X1 629
#define DECKMARK_DECK2_X0 651
#define DECKMARK_DECK2_X1 1269

/* The bar's rectangle for `deck`, in SCREEN coordinates, inclusive at both ends like every other
 * rect in this project. Returns 0 for a deck that has no strip (anything but 0 or 1), leaving the
 * out-parameters untouched -- a caller that ignores the return draws nothing rather than something
 * at the screen's corner. `y0`/`y1` are parameters rather than constants because the caller owns
 * the default and may have been told to move it. */
int deckmark_bar(int deck, int y0, int y1, int *x0, int *out_y0, int *x1, int *out_y1);

#endif /* RBPI4B_DECKMARK_ZONE_H */
