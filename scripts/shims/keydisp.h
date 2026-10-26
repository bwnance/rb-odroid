/*
 * keydisp.h -- rbp's deck panel shows the key a deck plays in under KEY SYNC.
 *
 * rbp knows nothing of the shim's key shift (keyshift.c), so its deck panel kept
 * the track's own key (operator, 2026-10-26: "key doesn't show on the rbp display
 * correctly with key shift"). ui_Deck_Update @0x28fa1c draws the panel's key from
 * a per-deck line -- 0x03268bf0 + deck * 0x238, UTF-16 text at +34 -- and redraws
 * whenever that text changes, so this writes the shifted key there, in the
 * notation rbp used (Camelot, or classic with its own sharps and flats), and puts
 * the original back when the shift goes. Any text it did not write is rbp's: the
 * original, remembered -- the key everything else in the shim reasons from.
 */
#ifndef RBPI4B_KEYDISP_H
#define RBPI4B_KEYDISP_H

/* Once a tick per deck, on the LED thread: the panel's key brought into line with
 * the deck's shift, and the TRACK's own key -- not the shifted one -- into `out`
 * (ASCII, rbp's sharps and flats as # and b). `total_ms` is the loaded track's
 * length: a change is a new track, whatever its key text. */
void keydisp_deck(int deck, int total_ms, char *out, int max);

/* The shifted spelling of `key` (ASCII, Camelot "8A" or classic "Abm"), `semis`
 * semitones up, in the same notation, as UTF-16 into `out` (NUL-terminated).
 * Answers the length, 0 when `key` is not a key. Pure, for the test. */
int keydisp_shifted(const char *key, int semis, unsigned short *out, int max);

#endif /* RBPI4B_KEYDISP_H */
