#!/usr/bin/env python3
"""test_rbassets.py -- the host checks for rbassets.py: the PNG reader, the
splash file, and TRANNY built from TRANS's letters. No firmware needed: the
pictures are made here.

    python3 scripts/device/test_rbassets.py
"""
import importlib.util
import os
import struct
import sys
import tempfile
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
spec = importlib.util.spec_from_file_location("rbassets", os.path.join(HERE, "rbassets.py"))
A = importlib.util.module_from_spec(spec)
spec.loader.exec_module(A)

failures = 0


def check(cond, what):
    global failures
    if not cond:
        failures += 1
        print("FAIL: " + what)


def png(path, w, h, ctype, rows):
    def chunk(t, d):
        return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xffffffff)
    raw = b"".join(b"\x00" + bytes(r) for r in rows)
    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, ctype, 0, 0, 0))
                + chunk(b"IDAT", zlib.compress(raw)) + chunk(b"IEND", b""))


with tempfile.TemporaryDirectory() as d:
    # RGB, and RGBA blended onto black
    p = os.path.join(d, "a.png")
    png(p, 2, 1, 2, [[255, 0, 0, 0, 0, 255]])
    w, h, px = A.read_png(p)
    check((w, h) == (2, 1) and px == [0xF800, 0x001F], "RGB PNG reads as RGB565 (%r)" % px)
    png(p, 1, 1, 6, [[255, 255, 255, 128]])
    w, h, px = A.read_png(p)
    check(px == [A.rgb565(128, 128, 128)], "RGBA's alpha is blended onto black (%r)" % px)

    # the splash file: header, size, pixels
    out = os.path.join(d, "splash.rgb565")
    A.write_splash(out, 2, 1, [0x1234, 0xABCD])
    with open(out, "rb") as f:
        data = f.read()
    check(data == b"RBSPLSH1" + struct.pack("<HHHH", 2, 1, 0x1234, 0xABCD), "splash file layout")

    # too big for the screen: shrunk by a whole factor
    w, h, px = A.shrink(4, 2, list(range(8)), 2, 2)
    check((w, h, px) == (2, 1, [0, 2]), "shrink by 2 (%r)" % ((w, h, px),))

# TRANNY from TRANS's runs and VINYL's Y, 1 px apart and centred
W, H = 120, 28
trans = [A.KEY] * (W * H)
for a, b in A.TRANS_RUNS:
    for x in range(a, b):
        trans[10 * W + x] = 0x1111
vinyl = [A.KEY] * (W * H)
for x in range(*A.VINYL_Y):
    vinyl[10 * W + x] = 0x2222
out = A.tranny(trans, vinyl, W, H)
runs = A.ink_runs(out, W, H)
check(len(runs) == 6, "six letters (%r)" % runs)
width = runs[-1][1] - runs[0][0]
check(abs(runs[0][0] - (W - width - runs[0][0])) <= 1, "centred (%r)" % runs)
check(all(out[10 * W + x] == 0x2222 for x in range(*runs[-1])), "the last letter is VINYL's Y")
check(A.tranny([A.KEY] * (W * H), vinyl, W, H) is None, "a TRANS picture not as measured is refused")

print("%s: rbassets" % ("FAIL" if failures else "all checks passed"))
sys.exit(1 if failures else 0)
