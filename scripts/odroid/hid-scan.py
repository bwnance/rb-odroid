#!/usr/bin/env python3
"""hid-scan.py -- flip the FLX10 jog-screen packet one bit at a time, to find what a byte does.

Deck 1 gets a rekordbox "loaded, paused" state packet with ONE bit flipped;
deck 2 gets the same packet unflipped, as the reference to compare against.
The bits are stepped through (automatically, or by hand), and the current one
is printed, so whatever changes on deck 1's screen -- remaining time, an
icon, the BEAT SYNC text -- can be tied to a byte and bit.

Stop rb's own sender first, or the two fight:

    echo RB_FLX10_HID=0 >> /opt/rblive4/rb.local.conf && systemctl restart rblive4
    python3 ~/rb-odroid/scripts/odroid/hid-scan.py              # 1.5 s a step
    python3 ~/rb-odroid/scripts/odroid/hid-scan.py --bytes 3,4,5,6,7,8 --step 3
    sed -i '/^RB_FLX10_HID=0/d' /opt/rblive4/rb.local.conf && systemctl restart rblive4

While it runs, type a command and Enter:

    (empty)   pause / resume
    n / b     next / back one step (pauses)
    123       go to step 123 (pauses)
    m         mark the current step (listed again at the end)
    q         quit

The base packet is deck 1 from the jog capture (2026-10-25): paused at
0:33.873 of a 3:06 track, cue set, with SYNC/MASTER and the loop cleared.
Bytes 11-18 (time), 21-31 (BPM, tempo, loop) and 55-57 (cue) are left alone
unless named with --bytes.
"""
import argparse
import os
import sys
import threading
import time

VID, PID = "2B73", "0041"

BASE = bytearray(128)
BASE[:64] = bytes.fromhex(
    "10 21 02 08 28 91 02 02 80 bc 0a 00 21 69 03 03 06 e9 01 00 00 8b 30 9f 03 e9 83 00 d4 84 00 e9"
    "83 00 00 00 00 00 00 00 00 00 00 ff ff 00 00 00 00 40 00 00 00 00 00 e9 83 00 01 00 80 0f 00 00")
BASE[5] = 0x01                    # no SYNC / MASTER
BASE[9], BASE[10] = 0xb4, 0x00    # no loop
BASE[25:31] = b"\x00\x00\x80\x00\x00\x00"

DEFAULT_BYTES = ([2, 3, 4, 5, 6, 7, 8, 9, 10, 19, 20] + list(range(32, 55))
                 + list(range(58, 80)))


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


def heartbeat(n):
    p = bytearray(128)
    p[1], p[2], p[4] = 0x3D, n, 0x05
    return p


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--bytes", help="comma list of bytes to scan (default: all but time/BPM/cue)")
    ap.add_argument("--step", type=float, default=1.5, help="seconds per step (default 1.5)")
    ap.add_argument("--start", type=int, default=0, help="first step")
    a = ap.parse_args()
    nbytes = [int(x) for x in a.bytes.split(",")] if a.bytes else DEFAULT_BYTES
    steps = [(b, 1 << bit) for b in nbytes for bit in range(8)]

    path = find_hidraw()
    if not path:
        sys.exit("no FLX10 hidraw node (is it plugged in and unlocked?)")
    fd = os.open(path, os.O_WRONLY)

    st = {"i": max(0, min(a.start, len(steps) - 1)), "paused": False,
          "next_at": time.monotonic() + a.step, "quit": False, "marks": []}
    lock = threading.Lock()

    def show():
        b, m = steps[st["i"]]
        print("step %3d/%d: byte %2d ^= 0x%02x  (0x%02x -> 0x%02x)%s" % (
            st["i"], len(steps) - 1, b, m, BASE[b], BASE[b] ^ m,
            "  [paused]" if st["paused"] else ""), flush=True)

    def keys():
        for line in sys.stdin:
            w = line.strip()
            with lock:
                if w == "":
                    st["paused"] = not st["paused"]
                    st["next_at"] = time.monotonic() + a.step
                elif w in ("n", "b"):
                    st["paused"] = True
                    st["i"] = max(0, min(len(steps) - 1, st["i"] + (1 if w == "n" else -1)))
                elif w.isdigit():
                    st["paused"] = True
                    st["i"] = max(0, min(len(steps) - 1, int(w)))
                elif w == "m":
                    b, m = steps[st["i"]]
                    st["marks"].append((st["i"], b, m))
                    print("marked step %d (byte %d ^= 0x%02x)" % (st["i"], b, m), flush=True)
                    continue
                elif w == "q":
                    st["quit"] = True
                    return
                else:
                    print("? (empty, n, b, a number, m, q)", flush=True)
                    continue
                show()

    print("%s: deck 1 = base with one bit flipped, deck 2 = base; %d steps, %.1f s each"
          % (path, len(steps), a.step), flush=True)
    show()
    threading.Thread(target=keys, daemon=True).start()
    n, t0 = 0, time.monotonic()
    try:
        while not st["quit"]:
            with lock:
                now = time.monotonic()
                if not st["paused"] and now >= st["next_at"]:
                    if st["i"] < len(steps) - 1:
                        st["i"] += 1
                        st["next_at"] = now + a.step
                        show()
                    else:
                        st["paused"] = True
                        print("end of the scan (type a step number to go back)", flush=True)
                b, m = steps[st["i"]]
            d1 = bytearray(BASE)
            d1[b] ^= m
            d2 = bytearray(BASE)
            d2[0] = 0x20
            d2[7] = 0x01
            n = n % 5 + 1
            for p in (heartbeat(n), d1, d2):
                os.write(fd, b"\x00" + bytes(p))
            t0 += 0.02
            time.sleep(max(0.0, t0 - time.monotonic()))
    except KeyboardInterrupt:
        pass
    except OSError as e:
        print("write failed: %s" % e)
    if st["marks"]:
        print("marked:")
        for i, b, m in st["marks"]:
            print("  step %d: byte %d ^= 0x%02x" % (i, b, m))


if __name__ == "__main__":
    main()
