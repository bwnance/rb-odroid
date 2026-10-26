#!/usr/bin/env python3
"""rbassets.py -- the start-up's two picture jobs, done before rbp launches.

1. THE SPLASH (fbshim's splash.c draws it over everything for RB_SPLASH_MS after
   rbp's first frame, so the start-up's half-made screens and the blue hot cue
   pads are never seen -- operator, 2026-10-26). This writes the picture it draws:

       /run/rblive4/splash.rgb565   "RBSPLSH1", u16 width, u16 height, then
                                    width x height RGB565 pixels, little-endian

   from RB_SPLASH_IMAGE (a PNG: 8-bit grey, grey+alpha, RGB or RGBA, not
   interlaced; alpha is blended onto black; bigger than the screen is shrunk by a
   whole factor) or, with none, from rbp's own white rekordbox logo in the
   firmware's imagedata.dat.

2. TRANS READS "TRANNY" on rbp's own screen too (operator, 2026-10-26; the FLX10's
   FX screen already says it). rbp draws its Beat FX names as pictures in
   imagedata.dat, so the two TRANS pictures (normal and selected) are rebuilt from
   rbp's own letters -- T R A N from TRANS, Y from VINYL BRAKE -- at TRANS's own
   spacing. A redeploy puts the original file back and the next start patches it
   again; a file already patched is left alone.

imagedata.dat (decoded 2026-10-26): a 0x18-byte header, then 44-byte records
until the first whose fourth word is not the file's size. A record: [0] 2,
[2] the picture's offset, [3] the file size, [6] width | height << 16. Pictures
are RGB565, 0xF81F transparent; they are not stored in record order.

Every failure is a line on stderr and exit 0: a picture is never a reason for rbp
not to start.
"""
import os
import struct
import sys
import zlib

KEY = 0xF81F

# THE TRANS PICTURES, family by family, and where each finds a Y in its own letters.
# Every family is checked against the letter runs measured before anything is written;
# a picture identical to one of these elsewhere in the file is patched with it.
#   the selector lists (120x28): Y from VINYL BRAKE, columns 34..43
#   the BEAT FX window's name (160x40: gray, white, faded, blue -- operator,
#     2026-10-26: "it just shows TRANS on the beat fx window"): Y from DELAY's last letter
TRANS_RUNS = [(30, 40), (41, 51), (52, 63), (64, 74), (76, 86)]   # T R A N S, 120x28
VINYL_Y = (34, 44)                # the Y of VINYL BRAKE
BG_COL = 3                        # a column of tile margin in every family, row by row
FAMILIES = [
    {"size": (120, 28), "trans": (1059, 1060), "ysrc": (1069, 1070),
     "runs": TRANS_RUNS, "y": VINYL_Y},
    {"size": (160, 40), "trans": (5225, 5226, 5227, 5228), "ysrc": (5177, 5178, 5179, 5180),
     "runs": [(41, 54), (57, 70), (71, 86), (88, 101), (104, 118)], "y": (102, 116)},
]
REC_LOGO = 1598                   # the white rekordbox logo, 492 x 70
LOGO_SHIFT = 300                  # its pixels start this many after the record says: the
                                  # x's tail ends in two blank columns here (295 wrapped
                                  # it round as two white dots left of the icon)


class ImageData:
    def __init__(self, path):
        self.path = path
        with open(path, "rb") as f:
            self.b = f.read()
        self.recs = []
        p = 0x18
        while p + 44 <= len(self.b):
            r = struct.unpack_from("<11I", self.b, p)
            if r[3] != len(self.b):
                break
            self.recs.append(r)
            p += 44

    def size(self, i):
        r = self.recs[i]
        return r[6] & 0xFFFF, r[6] >> 16

    def pixels(self, i, shift=0):
        w, h = self.size(i)
        off = self.recs[i][2] + 2 * shift
        return list(struct.unpack_from("<%dH" % (w * h), self.b, off))

    def offset(self, i):
        return self.recs[i][2]


def ink_runs(px, w, h, bg=KEY, bgcol=None):
    """Columns with ink, as runs. With `bgcol`, a pixel is ink when it is not that
    column's pixel on its row -- for pictures with an opaque, shaded background."""
    runs, start = [], None
    for x in range(w):
        if bgcol is None:
            ink = any(px[y * w + x] != bg for y in range(h))
        else:
            ink = any(px[y * w + x] != px[y * w + bgcol] for y in range(h))
        if ink and start is None:
            start = x
        if not ink and start is not None:
            runs.append((start, x))
            start = None
    if start is not None:
        runs.append((start, w))
    return runs


def tranny(trans, ysrc, w, h, runs=TRANS_RUNS, ycols=VINYL_Y):
    """TRANNY from TRANS's T R A N and a Y from `ysrc`'s columns `ycols`, at TRANS's
    own letter gaps, centred. The background is TRANS's own: the text's columns are
    cleared to column BG_COL, row by row, so a shaded tile stays shaded. None if
    TRANS is not the picture measured."""
    bgcol = BG_COL
    if ink_runs(trans, w, h, bgcol=bgcol) != runs:
        return None
    gaps = [runs[i + 1][0] - runs[i][1] for i in range(4)]       # T-R R-A A-N N-S
    glyphs = [("t", runs[0]), ("t", runs[1]), ("t", runs[2]), ("t", runs[3]),
              ("t", runs[3]), ("y", ycols)]
    sep = [gaps[0], gaps[1], gaps[2], gaps[3], gaps[2]]
    cols = []
    for i, (src, (a, b)) in enumerate(glyphs):
        if i:
            cols += [None] * sep[i - 1]
        cols += [(src, x) for x in range(a, b)]
    if len(cols) > w - 2 * 2:
        return None
    out = list(trans)
    for y in range(h):
        for x in range(runs[0][0], runs[-1][1]):
            out[y * w + x] = trans[y * w + bgcol]
    x0 = (w - len(cols)) // 2
    for i, c in enumerate(cols):
        if c is None:
            continue
        src = trans if c[0] == "t" else ysrc
        for y in range(h):
            out[y * w + x0 + i] = src[y * w + c[1]]
    return out


def patch_trans(path):
    img = ImageData(path)
    writes, notes, already = [], [], 0
    for fam in FAMILIES:
        size = fam["size"]
        if len(img.recs) <= max(fam["trans"] + fam["ysrc"]):
            notes.append("%dx%d: not in this file" % size)
            continue
        for rt, ry in zip(fam["trans"], fam["ysrc"]):
            if img.size(rt) != size or img.size(ry) != size:
                notes.append("%d is not %dx%d" % ((rt,) + size))
                continue
            cur = img.pixels(rt)
            if len(ink_runs(cur, size[0], size[1], bgcol=BG_COL)) == 6:
                already += 1                          # TRANNY already
                continue
            new = tranny(cur, img.pixels(ry), size[0], size[1], fam["runs"], fam["y"])
            if new is None:
                notes.append("%d is not the TRANS measured" % rt)
                continue
            data = struct.pack("<%dH" % len(new), *new)
            old = struct.pack("<%dH" % len(cur), *cur)
            # the picture itself, and every byte-identical copy of it in the file
            for i in range(len(img.recs)):
                if img.size(i) == size and img.b[img.offset(i):img.offset(i) + len(old)] == old:
                    writes.append((img.offset(i), data))
    if writes:
        with open(path, "r+b") as f:
            for off, data in writes:
                f.seek(off)
                f.write(data)
    msg = "TRANS -> TRANNY in %d picture(s)" % len(writes)
    if already:
        msg += ", %d already" % already
    if notes:
        msg += " (left alone: %s)" % "; ".join(notes)
    return msg


# ---- the splash -------------------------------------------------------------

def rgb565(r, g, b):
    return (r >> 3) << 11 | (g >> 2) << 5 | (b >> 3)


def firmware_logo(path):
    """rbp's white rekordbox logo, cropped to its own pixels."""
    img = ImageData(path)
    w, h = img.size(REC_LOGO)
    px = img.pixels(REC_LOGO, LOGO_SHIFT)

    def lum(v):
        return ((v >> 11) * 255 // 31 + (v >> 5 & 63) * 255 // 63 + (v & 31) * 255 // 31) // 3

    # The logo is white on black; the first rows hold a few bytes of the
    # picture before it, so only bright pixels below them count.
    pts = [(x, y) for y in range(6, h) for x in range(w)
           if px[y * w + x] != KEY and lum(px[y * w + x]) > 160]
    if not pts:
        raise ValueError("no logo in record %d" % REC_LOGO)
    x0 = max(min(p[0] for p in pts) - 2, 0)
    x1 = min(max(p[0] for p in pts) + 3, w)
    y0 = max(min(p[1] for p in pts) - 2, 6)
    y1 = min(max(p[1] for p in pts) + 3, h)
    out = []
    for y in range(y0, y1):
        for x in range(x0, x1):
            v = px[y * w + x]
            out.append(0 if v == KEY else v)
    return x1 - x0, y1 - y0, out


def read_png(path):
    """(w, h, RGB565 list) from an 8-bit, non-interlaced PNG; alpha onto black."""
    with open(path, "rb") as f:
        data = f.read()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError("not a PNG")
    pos, idat, ihdr, plte = 8, b"", None, None
    while pos < len(data):
        n, kind = struct.unpack_from(">I4s", data, pos)
        body = data[pos + 8:pos + 8 + n]
        if kind == b"IHDR":
            ihdr = struct.unpack(">IIBBBBB", body)
        elif kind == b"PLTE":
            plte = body
        elif kind == b"IDAT":
            idat += body
        pos += 12 + n
    w, h, depth, ctype, _, _, interlace = ihdr
    if depth != 8 or interlace:
        raise ValueError("only 8-bit, non-interlaced PNGs")
    ch = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}[ctype]
    raw = zlib.decompress(idat)
    stride = w * ch
    rows, prev, p = [], bytearray(stride), 0
    for _ in range(h):
        ft = raw[p]
        line = bytearray(raw[p + 1:p + 1 + stride])
        p += 1 + stride
        for i in range(stride):
            a = line[i - ch] if i >= ch else 0
            b = prev[i]
            c = prev[i - ch] if i >= ch else 0
            if ft == 1:
                line[i] = (line[i] + a) & 255
            elif ft == 2:
                line[i] = (line[i] + b) & 255
            elif ft == 3:
                line[i] = (line[i] + (a + b) // 2) & 255
            elif ft == 4:
                pa, pb, pc = abs(b - c), abs(a - c), abs(a + b - 2 * c)
                pr = a if pa <= pb and pa <= pc else (b if pb <= pc else c)
                line[i] = (line[i] + pr) & 255
        rows.append(line)
        prev = line
    out = []
    for line in rows:
        for x in range(w):
            px = line[x * ch:x * ch + ch]
            if ctype == 3:
                r, g, b_ = plte[px[0] * 3:px[0] * 3 + 3]
                a = 255
            elif ch <= 2:
                r = g = b_ = px[0]
                a = px[1] if ch == 2 else 255
            else:
                r, g, b_ = px[0], px[1], px[2]
                a = px[3] if ch == 4 else 255
            out.append(rgb565(r * a // 255, g * a // 255, b_ * a // 255))
    return w, h, out


def shrink(w, h, px, max_w, max_h):
    f = 1
    while w // f > max_w or h // f > max_h:
        f += 1
    if f == 1:
        return w, h, px
    nw, nh = w // f, h // f
    return nw, nh, [px[(y * f) * w + x * f] for y in range(nh) for x in range(nw)]


def write_splash(out, w, h, px):
    tmp = out + ".tmp"
    with open(tmp, "wb") as f:
        f.write(b"RBSPLSH1" + struct.pack("<HH", w, h) + struct.pack("<%dH" % len(px), *px))
    os.replace(tmp, out)


def main(argv):
    args = dict(zip(argv[1::2], argv[2::2]))
    imagedata = args.get("--imagedata", "")
    out = args.get("--splash-out", "")
    custom = args.get("--splash-image", "")
    if imagedata and args.get("--tranny", "1") == "1":
        try:
            print("rbassets: " + patch_trans(imagedata), file=sys.stderr)
        except Exception as e:                       # never stop the start
            print("rbassets: TRANS not patched: %s" % e, file=sys.stderr)
    if out:
        try:
            if custom:
                w, h, px = read_png(custom)
                src = custom
            else:
                w, h, px = firmware_logo(imagedata)
                src = "rbp's rekordbox logo"
            w, h, px = shrink(w, h, px, 1280, 800)
            write_splash(out, w, h, px)
            print("rbassets: splash %dx%d from %s" % (w, h, src), file=sys.stderr)
        except Exception as e:
            print("rbassets: no splash picture (%s): the splash is black" % e, file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
