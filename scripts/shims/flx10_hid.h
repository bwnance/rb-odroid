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
     int loaded, playing, cue_standby;
     int elapsed_ms, total_ms;
     int bpm_x100;          /* the BPM rbp's screen shows, x100 */
     int tempo_x100;        /* tempo %, x100, signed */
     int range;             /* getTempoRange: 0 = 6%, 1 = 10%, 2 = 16%, 3 = WIDE */
     int mt, sync;
     int key_code;          /* the track's key, flx10_key_code()'s 1..24, 0 = none */
     int cue_ms;            /* the main cue, ms; -1 = none */
     int looping;           /* a loop is playing */
     int loop_in_ms, loop_out_ms;   /* the current loop, ms; -1 = none */
     int loop_num, loop_den;        /* its size in beats as rbp shows it; 0 = unknown */
     int master;            /* this deck is the tempo MASTER */
     int jog_touch;         /* the platter is pressed */
     int time_remain;       /* rbp shows remaining time (not sent: no byte found) */
     int on_air;            /* rbp's ON AIR icon for the deck: the white edges */
};

/* The jog screen's loop-size code (byte 10) for a loop of len_ms at the track's
 * own BPM (bpm_x100 / (1 + tempo%)): 1/64..1/8 beat = 0x04..0x07, 1/4 = 0x08,
 * 1/2 = 0x0a, 1..512 beats = 0x0c..0x15, anything else 0x01 (the screen's
 * "--"). From the second rekordbox capture. */
int flx10_loop_code(int len_ms, int bpm_x100, int tempo_x100);

/* The same code from rbp's own beat count (num/den, as the RX3 screen shows
 * it), used when the length gives "--": a 512-beat loop clipped at the
 * track's end, and 1/128 (0x03 -- a guess, the next code below 1/64), which
 * the length test cannot resolve. 0x01 when not a power of two. */
int flx10_loop_code_beats(int num, int den);

/* Where the playhead is dt_ms after it was base_ms, at the playing speed
 * (1 + tempo%). Capped at EXTRAP_MAX_MS ahead, and at the track's end. */
int flx10_hid_extrapolate(int base_ms, long dt_ms, int moving, int tempo_x100,
                          int total_ms);

/* The arm's position, one deck, sent 100 times a second. Each tick takes a
 * fresh reading of rbp's playhead and returns what the screen shows:
 *   playing  -- runs ahead of the reading at the tempo (the reading moves in
 *               audio blocks) and ignores a reading less than NO_BACKSTEP_MS
 *               behind, which is block lag and not a seek;
 *   stopped  -- the reading; right after a pause, the last position shown,
 *               while rbp's position stays where it stopped (no step back);
 *   turning  -- the reading carried on at its own measured speed to
 *               JOG_LEAD_MS past now (it is a block old when drawn), so the
 *               arm keeps up with the hand;
 *   held     -- (paused, platter held still) the reading only once it moves
 *               more than JOG_STILL_MS: rbp's wobbles around the spot.
 * Exposed for the tests. */
struct flx10_hid_track {
     int base_ms, shown, was_moving, hold, hold_src;
     unsigned long long at;
     int last_reading;                /* rbp's reading and when it last changed */
     unsigned long long changed_at;
     float vel;                       /* its speed, ms per ms */
};
#define FLX10_JOG_HELD    1           /* `jog`: platter held, not turning */
#define FLX10_JOG_TURNING 2           /* `jog`: counts in the last JOG_ACTIVE_MS */
void flx10_hid_track_init(struct flx10_hid_track *k);
int flx10_hid_merge(struct flx10_hid_track *k, int reading, int moving, int jog,
                    int tempo_x100, int total_ms, unsigned long long now_ms);

/* The tempo % / BPM on a synced deck while its jog is turned: rbp's tempo
 * follows the bend (and the key shift with it), which the screen should not
 * show. The value from before the jog is kept until rbp's tempo is back
 * within 0.05% of it, or TEMPO_HOLD_MS after the last turn. Exposed for the
 * tests. */
struct flx10_hid_tempo_hold {
     int on, tempo_x100, bpm_x100, last_tempo, last_bpm;
     unsigned long long until;
};
void flx10_hid_tempo_hold(struct flx10_hid_tempo_hold *h, struct flx10_hid_deck *s,
                          int turning, unsigned long long now_ms);

/* Where the sender reads rbp's playhead (map_flx10.c sets it): fills the
 * position in ms and whether the deck is moving (playing, not in cue
 * standby), returns 0 when it cannot read. Without one, the 20 Hz value from
 * flx10_hid_update is used. */
void flx10_hid_set_pos_reader(int (*fn)(int deck, int *pos_ms, int *moving));

/* The jog was turned on this deck (0..3). */
void flx10_hid_jog_activity(int deck);

/* Key shown with master tempo off: the tempo moves it by a whole number of
 * semitones (round(12 * log2(1 + tempo%/100))), as rekordbox draws it -- the
 * key moved and "+5" beside it. Exposed for the tests. */
int flx10_key_shift(int tempo_x100);
int flx10_key_shifted(int key_code, int semitones);

/* Where on_air goes in the packet: OR `mask` into byte `byte` while the deck
 * is on air. [3] 0x02 lights the jog's white edges (found 2026-10-25); the
 * default, overridden by FLX10_ONAIR="byte:mask" in hex. (-1, 0) = not sent. */
void flx10_hid_set_onair(int byte, int mask);

/* Publish one deck's state (deck 0 = deck 1). Starts the sender thread on the
 * first call, unless FLX10_HID=0. */
void flx10_hid_update(int deck, const struct flx10_hid_deck *s);

/* The packets, for the sender and the tests. `deck` is 0..3. */
void flx10_hid_state_pkt(unsigned char p[FLX10_HID_PKT], int deck,
                         const struct flx10_hid_deck *s);
void flx10_hid_heartbeat_pkt(unsigned char p[FLX10_HID_PKT], int n /* 1..5 */);

#endif /* RBPI4B_FLX10_HID_H */
