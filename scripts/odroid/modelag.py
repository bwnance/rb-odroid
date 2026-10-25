#!/usr/bin/env python3
"""modelag.py -- how long the screen takes to react to a pad-mode button.

Run on the board while rb is running, then press the FLX10's pad-mode buttons
(HOT CUE, PAD FX1, BEAT JUMP, SAMPLER, or SHIFT + them), a couple of seconds
apart. For each press it prints when the framebuffer changed afterwards:

    deck1 HOT CUE   first change  +212 ms  rows 520-700   settled +248 ms

`first` is the first frame that differs after the note arrived; `settled` is
the last change inside the window, so a redraw in two steps shows as a gap.
A change that is only the clock (a few rows, every second) is labelled.

The MIDI comes from `aseqdump`, subscribed alongside rbp (a sequencer port
takes any number of readers), and the screen from /dev/fb0, read whole and
compared row by row against the previous read.

    python3 scripts/odroid/modelag.py            # Ctrl-C to stop
    python3 scripts/odroid/modelag.py --window 1500
"""
import argparse
import os
import re
import subprocess
import sys
import threading
import time

MODES = {
    27: "HOT CUE", 30: "PAD FX1", 32: "BEAT JUMP", 34: "SAMPLER",
    105: "SHIFT+HOT CUE", 107: "SHIFT+PAD FX1", 109: "SHIFT+BEAT JUMP",
    111: "SHIFT+SAMPLER",
}


def fb_geometry():
    base = "/sys/class/graphics/fb0"
    try:
        w, h = open(base + "/virtual_size").read().strip().split(",")
        bpp = int(open(base + "/bits_per_pixel").read())
        stride = int(open(base + "/stride").read())
    except OSError as e:
        sys.exit("cannot read fb0 geometry: %s" % e)
    return int(w), int(h), bpp, stride


def find_port():
    out = subprocess.run(["aseqdump", "-l"], capture_output=True, text=True).stdout
    for line in out.splitlines():
        if "FLX10" in line:
            return line.split()[0]
    sys.exit("no FLX10 sequencer port in `aseqdump -l`:\n" + out)


class Screen:
    def __init__(self, h, stride):
        self.h, self.stride = h, stride
        self.size = h * stride
        self.fd = os.open("/dev/fb0", os.O_RDONLY)
        self.prev = self.read()

    def read(self):
        os.lseek(self.fd, 0, 0)
        buf = b""
        while len(buf) < self.size:
            chunk = os.read(self.fd, self.size - len(buf))
            if not chunk:
                break
            buf += chunk
        return buf

    def changed_rows(self):
        cur = self.read()
        if cur == self.prev:              # one C-speed compare for the usual case
            return []
        s, p = self.stride, self.prev
        rows = [r for r in range(self.h)
                if cur[r * s:(r + 1) * s] != p[r * s:(r + 1) * s]]
        self.prev = cur
        return rows


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--window", type=int, default=1200,
                    help="ms to watch after each press (default 1200)")
    a = ap.parse_args()

    w, h, bpp, stride = fb_geometry()
    port = find_port()
    print("fb0 %dx%d %d bpp, stride %d; MIDI from %s" % (w, h, bpp, stride, port))
    print("press the pad-mode buttons a couple of seconds apart; Ctrl-C to stop\n")

    scr = Screen(h, stride)
    events = []                       # (t, rows) for every changed frame
    lock = threading.Lock()
    stop = threading.Event()

    def poll():
        while not stop.is_set():
            rows = scr.changed_rows()
            t = time.monotonic()
            if rows:
                with lock:
                    events.append((t, rows))
                    if len(events) > 2000:
                        del events[:1000]

    threading.Thread(target=poll, daemon=True).start()

    # sample the poll rate so the numbers have an error bar
    t0 = time.monotonic()
    for _ in range(5):
        scr.read()
    per = (time.monotonic() - t0) / 5 * 1000
    print("one screen read takes ~%.0f ms (the resolution of every number below)\n" % per)

    dump = subprocess.Popen(["aseqdump", "-p", port], stdout=subprocess.PIPE,
                            text=True, bufsize=1)
    pat = re.compile(r"Note on\s+(\d+), note (\d+), velocity (\d+)")
    try:
        for line in dump.stdout:
            m = pat.search(line)
            if not m or int(m.group(3)) == 0:
                continue
            ch, note = int(m.group(1)), int(m.group(2))
            if ch > 1 or note not in MODES:
                continue
            tp = time.monotonic()
            time.sleep(a.window / 1000.0)
            with lock:
                after = [(t, r) for t, r in events if t > tp]
            label = "deck%d %-16s" % (ch + 1, MODES[note])
            if not after:
                print("%s no screen change within %d ms" % (label, a.window))
                continue
            big = [(t, r) for t, r in after if len(r) > 30]
            first = after[0]
            line = "%s first change %+5.0f ms rows %d-%d" % (
                label, (first[0] - tp) * 1000, first[1][0], first[1][-1])
            if len(first[1]) <= 30:
                line += " (small: clock?)"
            if big:
                line += " | first big %+5.0f ms rows %d-%d" % (
                    (big[0][0] - tp) * 1000, big[0][1][0], big[0][1][-1])
                line += " | settled %+5.0f ms" % ((big[-1][0] - tp) * 1000)
            print(line, flush=True)
    except KeyboardInterrupt:
        pass
    finally:
        stop.set()
        dump.terminate()


if __name__ == "__main__":
    main()
