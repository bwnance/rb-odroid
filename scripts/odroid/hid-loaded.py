#!/usr/bin/env python3
"""hid-loaded.py -- tell the FLX10's jogs, over HID, that decks 1-2 are loaded.

Once the unit takes rekordbox's display connect sequence its jog screens are
fed over the HID interface (interface 5, /dev/hidrawN), not MIDI: every deck
says "Not Loaded" and ignores the MIDI BPM/time/marker until the host sends a
per-deck state packet ("xx 21"), ~50 times a second. This sends one, plus
rekordbox's "xx 3D" heartbeat, for decks 1 and 2, until Ctrl-C. It does not
touch rb; run it with rb up and tracks loaded and watch the jogs.

Packet layouts are the rekordbox / VirtualDJ USB captures decoded in
Veezuhz/Mixxx_FLX10_Controller_Mapping (docs/21): 128 bytes, byte 0 the deck
(10/20/30/40), byte 1 the type.

    python3 scripts/odroid/hid-loaded.py              # loaded, paused (rekordbox)
    python3 scripts/odroid/hid-loaded.py --playing    # loaded, playing
    python3 scripts/odroid/hid-loaded.py --empty      # rekordbox's empty-deck packet
    python3 scripts/odroid/hid-loaded.py --vdj        # VirtualDJ's simpler layout
    python3 scripts/odroid/hid-loaded.py --pos 75     # elapsed seconds in bytes 11-12
    python3 scripts/odroid/hid-loaded.py --no-heartbeat
"""
import argparse
import os
import sys
import time

VID, PID = "2B73", "0041"
DECKS = (0x10, 0x20)

# rekordbox, deck 1 loaded and paused, bytes 0..63 (rest zero)
RB_LOADED = bytes.fromhex(
    "10210000" "2c010002" "80b40000" "00020002"
    "26320300" "007c0003" "00000080" "00000002"
    "00000000" "00000000" "0000ffff" "00000000"
    "30000000" "00000200" "00010085" "0d000000")


def find_hidraw():
    base = "/sys/class/hidraw"
    for name in sorted(os.listdir(base) if os.path.isdir(base) else []):
        try:
            ev = open(os.path.join(base, name, "device", "uevent")).read().upper()
        except OSError:
            continue
        if VID in ev and PID in ev:
            return "/dev/" + name
    return None


def rb_state(deck, loaded, playing, pos):
    p = bytearray(128)
    if not loaded:                       # 10 21 00 00 20 01 00 00 80 10
        p[0], p[1], p[4], p[5], p[8], p[9] = deck, 0x21, 0x20, 0x01, 0x80, 0x10
        return p
    p[:len(RB_LOADED)] = RB_LOADED
    p[0] = deck
    p[2] = 0x00 if playing else 0x02     # 00 playing, 02 paused
    p[5] = 0x81                          # active
    p[11], p[12] = (pos >> 8) & 0xFF, pos & 0xFF
    return p


def vdj_state(deck, loaded, playing, pos):
    p = bytearray(128)
    p[0], p[1], p[3] = deck, 0x21, 0x0A
    p[4] = 0x0C if playing else 0x04
    p[5] = 0x81 if playing else 0x01
    p[7] = 0x02 if loaded else 0x01
    p[10] = 0x0E
    if loaded:
        p[21:25] = b"\x75\x40\x2a\xff"
        p[49] = 0x20 if playing else 0x30
    else:
        p[21] = 0x78
    p[27], p[58], p[59], p[61] = 0x80, 0x03, 0x1E, 0x0D
    return p


def heartbeat(n):
    p = bytearray(128)
    p[1], p[2], p[4] = 0x3D, n, 0x05
    return p


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--playing", action="store_true")
    ap.add_argument("--empty", action="store_true")
    ap.add_argument("--vdj", action="store_true")
    ap.add_argument("--pos", type=int, default=0, help="elapsed seconds (rekordbox layout)")
    ap.add_argument("--no-heartbeat", action="store_true")
    ap.add_argument("--hz", type=float, default=50.0)
    a = ap.parse_args()

    path = find_hidraw()
    if not path:
        sys.exit("no FLX10 hidraw node (is it plugged in and unlocked?)")
    fd = os.open(path, os.O_WRONLY)
    build = vdj_state if a.vdj else rb_state
    hb = not a.no_heartbeat and not a.vdj      # VirtualDJ sends none
    print("%s: %s, %s%s, %.0f Hz; Ctrl-C to stop" % (
        path, "VirtualDJ" if a.vdj else "rekordbox",
        "empty" if a.empty else ("loaded, playing" if a.playing else "loaded, paused"),
        ", heartbeat" if hb else "", a.hz))

    period, n, sent, t0 = 1.0 / a.hz, 0, 0, time.monotonic()
    try:
        while True:
            pkts = []
            if hb:
                n = n % 5 + 1
                pkts.append(heartbeat(n))
            pkts += [build(d, not a.empty, a.playing, a.pos) for d in DECKS]
            for p in pkts:
                # hidraw: byte 0 is the report number; 0 = the unit has none
                os.write(fd, b"\x00" + bytes(p))
                sent += 1
            t0 += period
            time.sleep(max(0.0, t0 - time.monotonic()))
    except KeyboardInterrupt:
        pass
    except OSError as e:
        print("write failed: %s" % e)
    print("sent %d packets" % sent)


if __name__ == "__main__":
    main()
