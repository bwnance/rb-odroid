/*
 * flx10_media.h -- the DDJ-FLX10 jog screens' artwork and waveforms.
 *
 * rekordbox sends a deck's track to the unit over HID (flx10_hid.h) as a few
 * multi-packet transfers, each packet [0] deck 0x10/0x20, [1] type, [2..3]
 * packet index from 1 (LE16), [4..5] packet count (LE16), [6..127] the next
 * 122 bytes of the payload, the last one zero-padded. Decoded from the
 * operator's rekordbox USB captures (tools/flx10-pcap-decode.py) and checked
 * byte for byte, both decks, against what this file builds from the same
 * tracks on the export stick (2026-10-26). In rekordbox's order:
 *   0x30  empty (all zero), as a load starts
 *   0x30  track length: minutes, seconds, ms (LE16)
 *   0x2d  the length again, then 0a 00 and ten 5-byte entries -- the cue
 *         list, by the look of it; sent as rekordbox sent it for tracks with
 *         no cues ("9e 2f 27 01 00" each), the format of a real cue unknown
 *   0x39  00 "HOT CUE", zero-padded to 3 packets (meaning not known)
 *   0x3b  zeros, 5 packets (meaning not known; one deck had values 0..4)
 *   0x3e  the played part of the overview (a bitmap rekordbox fills as the
 *         track plays): all zero
 *   0x2b  artwork, small: LE16 length + the export's 80x80 JPEG,
 *         PIONEER/Artwork/NNNNN/aID.jpg, as it is on the stick
 *   0x33  artwork, large: LE24 length + the 240x240 one, aID_m.jpg
 *   0x37  overview waveform: 0x80 + the track's PWV6 (3-band, 1200 x 3 bytes)
 *   0x2f  beat grid: LE16 count + per beat its number (1..4) and time in ms
 *         (LE24), from the DAT file's PQTZ
 *   0x38  detail waveform: LE32 entry count + the track's PWV7 (3-band,
 *         150 entries a second, 3 bytes each)
 *   0x3e  again, once the rest is in
 * With only the artwork, waveforms, beat grid and length, the jog drew a
 * newly loaded track's waveform only after its display mode was changed;
 * with 0x2d / 0x3e added it redrew by itself over a loaded track but not on
 * a first load (operator, 2026-10-26), so all of rekordbox's load sequence is
 * sent but 0x3c, which rekordbox computes and which is in no export file --
 * with it the waveform comes up by itself, quickly (operator, the same day).
 * Which of 0x30 / 0x39 / 0x3b the first load needed is not narrowed down.
 * PWV6 / PWV7 are in the track's ANLZ0000.2EX, which the export writes beside
 * the .DAT that export.pdb names for the track. Nothing here is converted:
 * every payload is the export's own bytes behind a length.
 */
#ifndef RBPI4B_FLX10_MEDIA_H
#define RBPI4B_FLX10_MEDIA_H

#include <stddef.h>

#define FLX10_MEDIA_MAX 13

struct flx10_media_item {
     unsigned char type;
     unsigned char *data;           /* malloc'd payload */
     size_t len;
};

struct flx10_media {
     int n;
     struct flx10_media_item it[FLX10_MEDIA_MAX];
     int have_art, have_wave;       /* for the log line */
};

/* One track's files, read into memory. Any of them may be NULL / 0. */
struct flx10_media_src {
     const unsigned char *ex2;  size_t ex2_len;   /* ANLZ0000.2EX */
     const unsigned char *dat;  size_t dat_len;   /* ANLZ0000.DAT */
     const unsigned char *art;  size_t art_len;   /* aID.jpg */
     const unsigned char *artm; size_t artm_len;  /* aID_m.jpg */
     int total_ms;
};

/* The payloads for one track, in sending order. A missing piece still goes
 * out, empty (no artwork: a zero length; no waveform: a flat overview and no
 * detail entries), so the unit does not keep showing the previous track's.
 * Returns 0, or -1 when out of memory. */
int flx10_media_from_src(const struct flx10_media_src *src, struct flx10_media *m);
void flx10_media_free(struct flx10_media *m);

/* Reads a loaded track's files and builds its payloads. `track_path` is the
 * audio file as rbp has it (/media/usb1/sda1/Contents/...); the export is
 * found by walking up from it to the directory holding
 * PIONEER/rekordbox/export.pdb. Returns 0 when the track is in an export
 * (pieces may still be missing), -1 when it is not (nothing is built). */
int flx10_media_load(const char *track_path, int total_ms, struct flx10_media *m);

/* export.pdb: the track whose file path (relative to the export's root, as
 * the export stores it: "/Contents/...") is `rel`. Fills its artwork path and
 * its analysis (.DAT) path, both relative to the root ("" when it has none).
 * Returns 0, or -1 when no such track. After Deep Symmetry's
 * rekordbox_pdb.ksy; strings are converted to UTF-8. */
int flx10_pdb_find(const unsigned char *pdb, size_t n, const char *rel,
                   char *art, size_t art_n, char *anlz, size_t anlz_n);

/* An ANLZ file's tagged section (e.g. "PWV7"): returns the offset of its body
 * and sets the header's offset and both lengths, or -1 when absent. */
long flx10_anlz_section(const unsigned char *d, size_t n, const char tag[4],
                        size_t *hdr_off, size_t *hdr_len, size_t *body_len);

/* Transfer packets: how many a payload takes, and packet `idx` (1-based). */
int flx10_bulk_count(size_t len);
void flx10_bulk_pkt(unsigned char p[128], int deck, int type,
                    const unsigned char *data, size_t len, int idx);

#endif /* RBPI4B_FLX10_MEDIA_H */
