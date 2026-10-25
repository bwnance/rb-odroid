/*
 * flx10_hid.h -- the DDJ-FLX10's jog screens over HID (interface 5, hidraw).
 *
 * Once the unit has taken rekordbox's display connect sequence (map_flx10.c's
 * flx10_display_init) its jog screens are rekordbox's: every deck says "Not
 * Loaded" and the MIDI ch-16 values are not drawn until the host sends a
 * per-deck state packet ("xx 21") about 50 times a second. This sends it, with
 * rekordbox's "xx 3D" heartbeat, from a thread of its own, every 20 ms.
 *
 * Packet layouts: the rekordbox USB captures decoded in
 * Veezuhz/Mixxx_FLX10_Controller_Mapping (reference only; docs/21), checked on
 * the unit with scripts/odroid/hid-loaded.py on 2026-10-25 -- rekordbox's
 * loaded packet brings the screens up, VirtualDJ's leaves "Not Loaded".
 *
 * FLX10_HID=0 turns it off (the screens then stay "Not Loaded").
 */
#ifndef RBPI4B_FLX10_HID_H
#define RBPI4B_FLX10_HID_H

#define FLX10_HID_PKT 128

/* One deck as the jog screen shows it. Fields decoded from the unit with
 * scripts/odroid/hid-decode.py (2026-10-25); see flx10_hid_state_pkt(). */
struct flx10_hid_deck {
     int loaded, playing;
     int elapsed_ms;
     int bpm_x100;          /* the BPM rbp's screen shows, x100 */
     int tempo_x100;        /* tempo %, x100, signed */
     int range;             /* getTempoRange: 0 = 6%, 1 = 10%, 2 = 16%, 3 = WIDE */
     int mt;
};

/* Publish one deck's state (deck 0 = deck 1). Starts the sender thread on the
 * first call, unless FLX10_HID=0. */
void flx10_hid_update(int deck, const struct flx10_hid_deck *s);

/* The packets, for the sender and the tests. `deck` is 0..3. */
void flx10_hid_state_pkt(unsigned char p[FLX10_HID_PKT], int deck,
                         const struct flx10_hid_deck *s);
void flx10_hid_heartbeat_pkt(unsigned char p[FLX10_HID_PKT], int n /* 1..5 */);

#endif /* RBPI4B_FLX10_HID_H */
