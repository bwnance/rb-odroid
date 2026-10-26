#!/usr/bin/env python3
"""flx10-hid-transfers.py CAPTURE OUTDIR -- rekordbox's multi-packet HID
transfers to a DDJ-FLX10 (artwork, waveforms, beat grid, ...), reassembled.

Pulls the HID OUT packets other than the per-deck state (0x21) and the
heartbeat (0x3d) out of a USB capture into OUTDIR/hid.bin (f64 time + 128
bytes each; reused on the next run), then joins each (type, deck) transfer --
[2..3] packet index from 1 (LE16), [4..5] packet count (LE16), [6..127] the
payload -- into OUTDIR/<type>_<deck>_<n>.bin and prints one line per transfer.
tools/captures/flx10-rekordbox-hid-notes.txt has what the types are.

    python3 tools/flx10-hid-transfers.py capture_2.pcapng /tmp/c2

Standard library only; uses flx10-pcap-decode.py (beside it) to read the file.
"""
import importlib.util, os, struct, sys, collections

spec = importlib.util.spec_from_file_location(
    "dec", os.path.join(os.path.dirname(__file__) or ".", "flx10-pcap-decode.py"))
dec = importlib.util.module_from_spec(spec)
spec.loader.exec_module(dec)

cap, out = sys.argv[1], sys.argv[2]
os.makedirs(out, exist_ok=True)
binp = os.path.join(out, "hid.bin")
if not os.path.exists(binp):
    recs = []
    for ts, lt, d in dec.packets(cap):
        u = dec.usb_out(lt, d)
        if u and u[3] and (u[1] & 0xFF) == 0x05 and len(u[3]) >= 128:
            recs.append((ts, u[1], bytes(u[3][:128])))
    subm = [r for r in recs if not r[1] & 0x100]
    recs = subm if subm else recs
    with open(binp, "wb") as f:
        for ts, ep, p in recs:
            if p[1] in (0x21, 0x3d):
                continue
            f.write(struct.pack("<d", ts) + p)
pk = []
with open(binp, "rb") as f:
    while True:
        b = f.read(136)
        if len(b) < 136:
            break
        pk.append((struct.unpack("<d", b[:8])[0], b[8:]))
t0 = pk[0][0] if pk else 0
cur = {}
n = collections.Counter()
for ts, p in pk:
    key = (p[1], p[0])
    idx, cnt = struct.unpack("<HH", p[2:6])
    if idx == 1:
        cur[key] = [ts, cnt, [p[6:]]]
    elif key in cur:
        cur[key][2].append(p[6:])
    if key in cur and len(cur[key][2]) == cur[key][1]:
        t, c, parts = cur.pop(key)
        data = b"".join(parts)
        n[key] += 1
        name = "%02x_%02x_%d.bin" % (key[0], key[1], n[key])
        open(os.path.join(out, name), "wb").write(data)
        print("%8.3f type %02x deck %02x  %4d pkts  %6d B  %s  %s" %
              (t - t0, key[0], key[1], c, len(data), name, data[:24].hex(" ")))
