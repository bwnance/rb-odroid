#!/usr/bin/env python3
"""fbpace.py -- how evenly rbp redraws the screen (the waveform's scroll).

Polls /dev/fb0 as fast as it can for a few seconds while a deck plays and logs
the time between frames that changed, then prints a histogram and the worst
gaps. Steady intervals with the odd long one = something occasionally steals
the CPU; a spread everywhere = rbp's own pacing or the display path.

    python3 scripts/odroid/fbpace.py                 # 10 s, whole screen
    python3 scripts/odroid/fbpace.py --rows 300-420  # just the waveform band
    python3 scripts/odroid/fbpace.py --secs 20

Pick the band with --rows if the clock or meters change on their own: only
frames that change inside the band count. --min-rows ignores changes touching
fewer rows than that (the clock, a blinking light).
"""
import argparse
import os
import sys
import time


def geometry():
    b = "/sys/class/graphics/fb0"
    w, h = open(b + "/virtual_size").read().strip().split(",")
    return int(w), int(h), int(open(b + "/stride").read())


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--secs", type=float, default=10.0)
    ap.add_argument("--rows", default="", help="y0-y1 band to watch")
    ap.add_argument("--min-rows", type=int, default=4)
    a = ap.parse_args()

    w, h, stride = geometry()
    y0, y1 = 0, h - 1
    if a.rows:
        y0, y1 = (int(v) for v in a.rows.split("-"))
    nrows = y1 - y0 + 1
    fd = os.open("/dev/fb0", os.O_RDONLY)
    off, size = y0 * stride, nrows * stride

    def grab():
        return os.pread(fd, size, off)

    # read cost = resolution
    t = time.monotonic()
    for _ in range(10):
        grab()
    per = (time.monotonic() - t) / 10 * 1000
    print("fb0 %dx%d, watching rows %d-%d; one read ~%.1f ms (the resolution)"
          % (w, h, y0, y1, per))
    print("play a track; measuring %.0f s ..." % a.secs)

    prev = grab()
    changes = []
    end = time.monotonic() + a.secs
    while time.monotonic() < end:
        cur = grab()
        now = time.monotonic()
        if cur != prev:
            n = sum(1 for r in range(nrows)
                    if cur[r * stride:(r + 1) * stride] != prev[r * stride:(r + 1) * stride])
            if n >= a.min_rows:
                changes.append(now)
            prev = cur
    os.close(fd)

    if len(changes) < 3:
        print("only %d changing frames -- is a deck playing, and the band right?" % len(changes))
        return 1
    iv = [(b - a2) * 1000 for a2, b in zip(changes, changes[1:])]
    iv_sorted = sorted(iv)
    n = len(iv)
    pct = lambda p: iv_sorted[min(n - 1, int(p / 100 * n))]
    print("\n%d frames in %.1f s = %.1f fps" % (len(changes), changes[-1] - changes[0],
                                                 (len(changes) - 1) / (changes[-1] - changes[0])))
    print("interval ms: min %.1f  p10 %.1f  median %.1f  p90 %.1f  p99 %.1f  max %.1f"
          % (iv_sorted[0], pct(10), pct(50), pct(90), pct(99), iv_sorted[-1]))
    med = pct(50)
    late = [x for x in iv if x > 1.5 * med]
    print("late frames (> 1.5 x median): %d of %d (%.1f%%)" % (len(late), n, 100.0 * len(late) / n))
    print("\nhistogram (5 ms buckets):")
    buckets = {}
    for x in iv:
        buckets[int(x // 5) * 5] = buckets.get(int(x // 5) * 5, 0) + 1
    top = max(buckets.values())
    for k in sorted(buckets):
        print("  %4d-%-4d %5d %s" % (k, k + 5, buckets[k], "#" * max(1, buckets[k] * 50 // top)))
    print("\nworst gaps (ms, at s into the run):")
    t0 = changes[0]
    worst = sorted(((iv[i], changes[i + 1] - t0) for i in range(n)), reverse=True)[:8]
    for g, at in worst:
        print("  %6.1f at %5.2f s" % (g, at))
    return 0


if __name__ == "__main__":
    sys.exit(main())
