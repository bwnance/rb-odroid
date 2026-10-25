#!/usr/bin/env python3
"""flx10-pcap-decode.py -- what rekordbox sends the DDJ-FLX10, from a USB capture.

Reads a Wireshark/tcpdump USB capture (.pcapng or .pcap; macOS XHC interface,
Windows USBPcap or Linux usbmon) and prints:

  1. every OUT endpoint seen, with packet counts and sizes, so the HID one
     (128-byte packets) and the MIDI one stand out;
  2. the HID packets by type (byte 1) and deck (byte 0): count, first and last
     time, and the first packet in hex;
  3. for the per-deck state packet (type 0x21, or --type), every change: the
     time and which bytes changed from what to what, deck by deck -- line these
     up with the notes taken during the capture;
  4. with --midi, the MIDI rekordbox sends (USB-MIDI event packets) in time
     order, so a HID change can be read beside the MIDI around it.

Times are shown as the capture's wall clock (local time) and seconds from the
start.

    python3 tools/flx10-pcap-decode.py capture.pcapng
    python3 tools/flx10-pcap-decode.py capture.pcapng --midi
    python3 tools/flx10-pcap-decode.py capture.pcapng --type 0x21 --deck 1
    python3 tools/flx10-pcap-decode.py capture.pcapng --dump 0x2c   # every packet of a type
    python3 tools/flx10-pcap-decode.py capture.pcapng --ep 0x05     # pick the HID endpoint

Standard library only.
"""
import argparse
import collections
import datetime
import struct
import sys

LINKTYPE_USB_LINUX = 189
LINKTYPE_USB_LINUX_MMAPPED = 220
LINKTYPE_USBPCAP = 249
LINKTYPE_USB_DARWIN = 266
HID_LEN = 128


# ---- capture files ---------------------------------------------------------

def read_pcapng(f):
    """Yield (timestamp_seconds, linktype, data)."""
    ifaces = []          # (linktype, ts_divisor)
    endian = "<"
    while True:
        hdr = f.read(8)
        if len(hdr) < 8:
            return
        btype, blen = struct.unpack(endian + "II", hdr)
        if btype == 0x0A0D0D0A:                       # section header
            bom = f.read(4)
            endian = "<" if bom == b"\x4d\x3c\x2b\x1a" else ">"
            blen = struct.unpack(endian + "I", hdr[4:])[0]
            f.read(blen - 12)
            ifaces = []
            continue
        body = f.read(blen - 12)
        f.read(4)
        if btype == 1:                                # interface description
            lt = struct.unpack(endian + "H", body[:2])[0]
            div = 1e6
            opts = body[8:]
            while len(opts) >= 4:
                code, ln = struct.unpack(endian + "HH", opts[:4])
                if code == 0:
                    break
                val = opts[4:4 + ln]
                if code == 9 and ln >= 1:             # if_tsresol
                    r = val[0]
                    div = float(2 ** (r & 0x7F)) if r & 0x80 else float(10 ** r)
                opts = opts[4 + ((ln + 3) & ~3):]
            ifaces.append((lt, div))
        elif btype == 6:                              # enhanced packet
            iid, hi, lo, cap, _orig = struct.unpack(endian + "IIIII", body[:20])
            if iid < len(ifaces):
                lt, div = ifaces[iid]
                yield ((hi << 32) | lo) / div, lt, body[20:20 + cap]
        elif btype == 3:                              # simple packet
            if ifaces:
                orig = struct.unpack(endian + "I", body[:4])[0]
                yield 0.0, ifaces[0][0], body[4:4 + orig]


def read_pcap(f, magic):
    endian = "<" if magic in (b"\xd4\xc3\xb2\xa1", b"\x4d\x3c\xb2\xa1") else ">"
    nano = magic in (b"\x4d\x3c\xb2\xa1", b"\xa1\xb2\x3c\x4d")
    rest = f.read(20)
    lt = struct.unpack(endian + "I", rest[16:20])[0] & 0xFFFF
    while True:
        h = f.read(16)
        if len(h) < 16:
            return
        sec, frac, cap, _orig = struct.unpack(endian + "IIII", h)
        yield sec + frac / (1e9 if nano else 1e6), lt, f.read(cap)


def packets(path):
    with open(path, "rb") as f:
        magic = f.read(4)
        if magic == b"\x0a\x0d\x0d\x0a":
            f.seek(0)
            yield from read_pcapng(f)
        elif magic in (b"\xd4\xc3\xb2\xa1", b"\xa1\xb2\xc3\xd4",
                       b"\x4d\x3c\xb2\xa1", b"\xa1\xb2\x3c\x4d"):
            yield from read_pcap(f, magic)
        else:
            sys.exit("%s: not a pcap or pcapng file" % path)


# ---- USB headers -> (device, endpoint, ep_type, payload) for OUT data ---------

def usb_out(lt, d):
    """The host->device data in one captured record, or None.
    ep_type: 0 control, 1 isochronous, 2 bulk, 3 interrupt."""
    if lt == LINKTYPE_USB_DARWIN:
        if len(d) < 32:
            return None
        hlen = d[2]
        req = d[3]                 # 0 submit, 1 complete
        io_len = struct.unpack("<I", d[4:8])[0]
        dev, ep, ept = d[29], d[30], d[31]
        if ep & 0x80:
            return None
        # OUT data normally travels with the submission (req 0); main() keeps
        # the completions only for an endpoint whose submissions carry none
        return dev, ep | (0x100 if req else 0), ept, d[hlen:hlen + io_len] if io_len else d[hlen:]
    if lt == LINKTYPE_USBPCAP:
        if len(d) < 27:
            return None
        hlen = struct.unpack("<H", d[0:2])[0]
        info = d[16]
        dev = struct.unpack("<H", d[19:21])[0]
        ep, xfer = d[21], d[22]
        ept = {0: 1, 1: 3, 2: 0, 3: 2}.get(xfer, xfer)   # USBPcap order -> ours
        if ep & 0x80 or info & 1:
            return None
        return dev, ep, ept, d[hlen:]
    if lt in (LINKTYPE_USB_LINUX, LINKTYPE_USB_LINUX_MMAPPED):
        if len(d) < 48:
            return None
        typ = chr(d[8])
        xfer, ep, dev = d[9], d[10], d[11]
        ept = {0: 1, 1: 3, 2: 0, 3: 2}.get(xfer, xfer)
        hlen = 64 if lt == LINKTYPE_USB_LINUX_MMAPPED else 48
        if ep & 0x80 or typ != "S":
            return None
        return dev, ep, ept, d[hlen:]
    return None


# ---- USB-MIDI -----------------------------------------------------------------

CIN_LEN = {0x2: 2, 0x3: 3, 0x4: 3, 0x5: 1, 0x6: 2, 0x7: 3, 0x8: 3, 0x9: 3,
           0xA: 3, 0xB: 3, 0xC: 2, 0xD: 2, 0xE: 3, 0xF: 1}


def midi_events(payload):
    """USB-MIDI event packets -> MIDI messages (SysEx reassembled)."""
    out, sx = [], []
    for i in range(0, len(payload) - 3, 4):
        cin = payload[i] & 0x0F
        b = payload[i + 1:i + 4]
        if cin == 0 and not any(b):
            continue
        if cin == 0x4:
            sx += b
        elif cin in (0x5, 0x6, 0x7) and (sx or b[0] == 0xF0):
            sx += b[:CIN_LEN[cin]]
            out.append(bytes(sx))
            sx = []
        else:
            out.append(bytes(b[:CIN_LEN.get(cin, 3)]))
    return out


# ---- report -------------------------------------------------------------------

def clock(ts):
    return datetime.datetime.fromtimestamp(ts).strftime("%H:%M:%S.%f")[:-3]


def hexs(b):
    return " ".join("%02x" % x for x in b)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("capture")
    ap.add_argument("--ep", type=lambda s: int(s, 0), help="HID OUT endpoint (default: auto)")
    ap.add_argument("--type", type=lambda s: int(s, 0), default=0x21,
                    help="packet type to diff (default 0x21)")
    ap.add_argument("--deck", type=int, help="only this deck (1-4) in the diff")
    ap.add_argument("--dump", type=lambda s: int(s, 0), help="print every packet of this type")
    ap.add_argument("--midi", action="store_true", help="also list the MIDI out")
    ap.add_argument("--ignore", default="", help="bytes to leave out of the diff, e.g. 11,12,13")
    a = ap.parse_args()
    ignore = {int(x) for x in a.ignore.split(",") if x.strip()}

    recs = []                                    # (ts, dev, ep, ept, payload)
    linktypes = collections.Counter()
    for ts, lt, d in packets(a.capture):
        linktypes[lt] += 1
        u = usb_out(lt, d)
        if u and u[3]:
            recs.append((ts,) + u)
    if not recs:
        sys.exit("no host->device USB data found (link types: %s)" % dict(linktypes))
    # macOS: endpoints marked 0x100 are completion records; use them only where
    # the submissions had no data
    with_submit = {(r[1], r[2]) for r in recs if not r[2] & 0x100}
    recs = [(ts, dev, ep & 0xFF, ept, p) for ts, dev, ep, ept, p in recs
            if not ep & 0x100 or (dev, ep & 0xFF) not in with_submit]
    t0 = recs[0][0]

    # 1. endpoints
    eps = collections.defaultdict(lambda: [0, collections.Counter()])
    for ts, dev, ep, ept, p in recs:
        e = eps[(dev, ep, ept)]
        e[0] += 1
        e[1][len(p)] += 1
    print("== OUT endpoints (device, endpoint, type: count, sizes)")
    names = {0: "control", 1: "iso", 2: "bulk", 3: "interrupt"}
    for (dev, ep, ept), (n, sizes) in sorted(eps.items()):
        top = ", ".join("%dB x%d" % kv for kv in sizes.most_common(4))
        print("  dev %d ep 0x%02x %-9s %6d  %s" % (dev, ep, names.get(ept, ept), n, top))

    if a.ep is not None:
        hid_key = next((k for k in eps if k[1] == a.ep), None)
    else:
        cands = [(v[1][HID_LEN], k) for k, v in eps.items() if k[2] in (2, 3) and v[1][HID_LEN]]
        hid_key = max(cands)[1] if cands else None
    if hid_key is None:
        print("\nno HID endpoint with %d-byte packets; pick one with --ep" % HID_LEN)
    else:
        print("\nHID: dev %d ep 0x%02x" % (hid_key[0], hid_key[1]))
        hid = [(ts, p[:HID_LEN]) for ts, dev, ep, ept, p in recs
               if (dev, ep, ept) == hid_key and len(p) >= HID_LEN]

        # 2. by type and deck
        groups = collections.OrderedDict()
        for ts, p in hid:
            k = (p[1], p[0])
            g = groups.setdefault(k, [0, ts, ts, p])
            g[0] += 1
            g[2] = ts
        print("\n== HID packets by type / deck byte (count, first .. last, first packet)")
        for (typ, deck), (n, first, last, p) in sorted(groups.items()):
            print("  type 0x%02x deck 0x%02x  %6d  %s .. %s" % (typ, deck, n, clock(first), clock(last)))
            print("      " + hexs(p[:32]) + (" ..." if any(p[32:]) else ""))

        if a.dump is not None:
            print("\n== every type 0x%02x packet" % a.dump)
            for ts, p in hid:
                if p[1] == a.dump:
                    last = max((i for i, b in enumerate(p) if b), default=0)
                    print("%s %+9.3f  %s" % (clock(ts), ts - t0, hexs(p[:last + 1])))

        # 3. changes in the state packet
        print("\n== type 0x%02x changes, by deck (byte: old -> new)%s" % (
            a.type, " ignoring " + a.ignore if ignore else ""))
        prev = {}
        for ts, p in hid:
            if p[1] != a.type:
                continue
            deck = p[0] >> 4
            if a.deck and deck != a.deck:
                continue
            old = prev.get(deck)
            prev[deck] = p
            if old is None:
                print("%s %+9.3f  deck %d first: %s" % (clock(ts), ts - t0, deck, hexs(p[:64])))
                continue
            ch = [i for i in range(HID_LEN) if p[i] != old[i] and i not in ignore]
            if ch:
                print("%s %+9.3f  deck %d  %s" % (clock(ts), ts - t0, deck,
                      "  ".join("[%d] %02x->%02x" % (i, old[i], p[i]) for i in ch)))

    # 4. MIDI
    if a.midi:
        print("\n== MIDI out (USB-MIDI endpoints)")
        for ts, dev, ep, ept, p in recs:
            if (dev, ep, ept) == hid_key or ept not in (2, 3) or len(p) % 4:
                continue
            for m in midi_events(p):
                if m[:10] == bytes.fromhex("f0004005000004010050"):
                    continue                     # keepalive
                print("%s %+9.3f  ep 0x%02x  %s" % (clock(ts), ts - t0, ep, hexs(m)))


if __name__ == "__main__":
    main()
