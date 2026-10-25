#!/usr/bin/env python3
"""hid-decode.py -- step through byte edits of the FLX10's xx 21 HID packet.

Turns rb's own HID sender off (RB_FLX10_HID=0 in rb.local.conf, rb restarted),
waits for you to load a track on both decks, then sends hid-loaded.py's
"loaded, paused" packet with one edit at a time and asks what changed on the
jogs. Answers go to ~/flx10-hid.txt. rb.local.conf and rb are put back at the
end, Ctrl-C included.

    python3 scripts/odroid/hid-decode.py
    python3 scripts/odroid/hid-decode.py --keep-rb   # leave rb.local.conf alone
    python3 scripts/odroid/hid-decode.py --round 1   # an earlier list
"""
import argparse
import importlib.util
import os
import subprocess
import sys
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
spec = importlib.util.spec_from_file_location("hidloaded", os.path.join(HERE, "hid-loaded.py"))
hl = importlib.util.module_from_spec(spec)
spec.loader.exec_module(hl)

CONF = os.path.join(os.environ.get("RB_DEPLOY_ROOT", "/opt/rblive4"), "rb.local.conf")
LINE = "RB_FLX10_HID=0"
OUT = os.path.expanduser("~/flx10-hid.txt")

# (edits, what to watch). Each step starts from the plain captured packet.
# Round 1 (2026-10-25): 21-22 = BPM integer (LE16: 22=01 -> 380), 23 = tempo %
# x100 (64 -> 1.0%, e8 -> 2.3%), 24 00 drops the .03, 16 00 blanks the key,
# 9 00 -> Not Loaded, 4 00 blanks tempo %/range (MT?).
ROUND1 = [
    ({}, "baseline: note BPM, decimals, key, tempo %, range box, MT, time"),
    ({23: 0x00}, "BPM decimals / tempo % / range box"),
    ({23: 0x64}, "tempo % 1.0 if 23 = tempo x100?"),
    ({23: 0xE8}, "tempo % 2.3?"),
    ({22: 0x01, 23: 0x00}, "tempo % 2.5? (22-23 one number)"),
    ({24: 0x00}, "anything? BPM decimals?"),
    ({19: 0x05}, "anything? BPM decimals?"),
    ({20: 0x05}, "anything? BPM decimals?"),
    ({16: 0x00}, "key / range / anything"),
    ({17: 0x00}, "key / range / anything"),
    ({18: 0x00}, "key / range / anything"),
    ({18: 0x01}, "key / range / anything"),
    ({18: 0x02}, "key / range / anything"),
    ({9: 0x00}, "MT / key / anything"),
    ({4: 0x00}, "MT / key / anything"),
    ({7: 0x01}, "MT / key / anything (01 may read as empty deck)"),
    ({49: 0x00}, "MT / key / anything"),
    ({55: 0x00}, "MT / key / anything"),
    ({58: 0x00}, "MT / key / anything"),
    ({60: 0x81, 7: 0x01}, "deck shown unloaded, key byte 81"),
    ({60: 0x81}, "...and loaded again: key 8B now?"),
]

ROUND2 = [
    ({}, "baseline: BPM 124.03, key 10B, tempo 0.0%, range +-10, MT on?"),
    # key: byte 16 (captured 26 = 10B)
    ({16: 0x01}, "key?"),
    ({16: 0x02}, "key?"),
    ({16: 0x05}, "key?"),
    ({16: 0x06}, "key?"),
    ({16: 0x18}, "key?"),
    ({16: 0x25}, "key?"),
    ({16: 0x27}, "key?"),
    ({16: 0x28}, "key?"),
    ({16: 0x81}, "key?"),
    # tempo % and the BPM decimals: does the unit multiply BPM by the tempo?
    ({23: 0x00, 24: 0x00}, "BPM / decimals / tempo %"),
    ({23: 0x64, 24: 0x00}, "BPM / decimals / tempo % (125.24 if it multiplies)"),
    ({23: 0x64, 24: 0xFF}, "BPM / decimals / tempo %"),
    ({23: 0x9C, 24: 0xFF}, "BPM / decimals / tempo % (-1.00%? 122.76?)"),
    ({25: 0x32}, "BPM decimals?"),
    ({26: 0x32}, "BPM decimals?"),
    ({20: 0x80}, "BPM decimals?"),
    # byte 4 (captured 2c) bit by bit: MT, tempo %, range box
    ({4: 0x0C}, "MT / tempo % / range box"),
    ({4: 0x24}, "MT / tempo % / range box"),
    ({4: 0x28}, "MT / tempo % / range box"),
    ({4: 0x3C}, "MT / tempo % / range box"),
    ({4: 0x2D}, "MT / tempo % / range box"),
    # tempo range candidates
    ({13: 0x01}, "range box?"),
    ({13: 0x03}, "range box?"),
    ({15: 0x01}, "range box?"),
    ({15: 0x03}, "range box?"),
    ({31: 0x01}, "range box?"),
    ({31: 0x03}, "range box?"),
    ({27: 0x00}, "anything?"),
]
# Round 2 (2026-10-25): 23-24 tempo % x100 signed LE (64 00 +1.0, 9c ff -1.0);
# byte 4 0x04 = MT, 0x30 = range box (20 +-10, 30 +-16, 00 none); 13 moves the
# arm (sub-second); byte 16 values other than 00 change nothing.
#
# Round 3: which MIDI display messages still show while the HID packet runs
# (a step's "midi" is sent every 100 ms, deck 1 = the LEFT jog; rb sends its own
# once a second, so a hit may flicker), then key-after-reload, WIDE, BPM
# decimals, and where remaining time comes from. Pause both decks first.
ROUND3 = [
    ({}, "baseline (LEFT jog): BPM, key, tempo %, range, MT, time, arm"),
    {"midi": ["B0 49 01"], "watch": "key 8B instead of 10B? (MIDI key)"},
    {"midi": ["90 5B 00"], "watch": "key in classic notation? (MIDI key format)"},
    {"midi": ["BF 14 09 BF 34 62"], "watch": "BPM 125.0? (MIDI BPM)"},
    {"midi": ["BF 18 08 BF 38 1A"], "watch": "tempo +5.0%? (MIDI speed)"},
    {"midi": ["BF 42 05 BF 43 1E"], "watch": "time 5:30? (MIDI time)"},
    {"midi": ["9F 14 7F"], "watch": "time switches to remaining? (MIDI time mode)"},
    {"midi": ["9F 14 00"], "watch": "time elapsed? (MIDI time mode)"},
    {"midi": ["BF 10 00 BF 30 5A"], "watch": "arm at 3 o'clock? (MIDI marker)"},
    {"midi": ["BF 10 01 BF 30 34"], "watch": "arm at 6 o'clock? (MIDI marker)"},
    {"midi": ["9F 18 7F"], "watch": "MASTER shown? (MIDI)"},
    {"midi": ["9F 1C 7F"], "watch": "SYNC shown? (MIDI)"},
    {"midi": ["9F 20 00"], "watch": "MT off? (MIDI; HID says on)"},
    {"midi": ["9F 5D 7F"], "watch": "rekordbox logo instead of info? (MIDI show info)"},
    {"pre": {9: 0x00}, "edits": {16: 0x01}, "watch": "after a blink of Not Loaded: key?"},
    {"pre": {9: 0x00}, "edits": {16: 0x05}, "watch": "after a blink of Not Loaded: key?"},
    {"pre": {9: 0x00}, "edits": {16: 0x26}, "watch": "after a blink of Not Loaded: key (10B as captured?)"},
    ({4: 0x1C}, "range box +-6?"),
    ({4: 0x4C}, "range box WIDE?"),
    ({4: 0x6C}, "range box?"),
    ({4: 0xAC}, "range box?"),
    ({10: 0x32}, "BPM decimals?"),
    ({14: 0x32}, "BPM decimals?"),
    ({28: 0x32}, "BPM decimals?"),
    ({29: 0x32}, "BPM decimals?"),
    ({30: 0x32}, "BPM decimals?"),
    ({3: 0x0A}, "remaining time: what does it show?"),
    ({3: 0x0A, 17: 0x00}, "remaining time changed?"),
    ({3: 0x0A, 18: 0x00}, "remaining time changed?"),
    ({3: 0x08}, "remaining time? (0x08 alone)"),
]
ROUNDS = {1: ROUND1, 2: ROUND2, 3: ROUND3}


def step_parts(st):
    """(edits, pre-edits, midi messages, watch) from either step form."""
    if isinstance(st, tuple):
        return st[0], {}, [], st[1]
    return st.get("edits", {}), st.get("pre", {}), st.get("midi", []), st["watch"]


def find_seq_port():
    out = subprocess.run(["aseqdump", "-l"], capture_output=True, text=True).stdout
    for line in out.splitlines():
        if "FLX10" in line:
            return line.split()[0]
    return None


def conf_has():
    try:
        return any(l.strip() == LINE for l in open(CONF))
    except OSError:
        return False


def conf_set(on):
    lines = []
    try:
        lines = [l for l in open(CONF) if l.strip() != LINE]
    except OSError:
        pass
    if on:
        lines.append(LINE + "\n")
    with open(CONF, "w") as f:
        f.writelines(lines)


def restart_rb():
    print("restarting rb ...", flush=True)
    subprocess.run(["systemctl", "restart", "rblive4"])


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--keep-rb", action="store_true",
                    help="do not touch rb.local.conf / restart rb")
    ap.add_argument("--round", type=int, default=max(ROUNDS), choices=sorted(ROUNDS),
                    help="which list of tests (default: the newest)")
    a = ap.parse_args()

    path = hl.find_hidraw()
    if not path:
        sys.exit("no FLX10 hidraw node (is it plugged in and unlocked?)")

    added = False
    if not a.keep_rb and not conf_has():
        conf_set(True)
        added = True
        restart_rb()

    fd = os.open(path, os.O_WRONLY)
    cur = {"edits": {}, "midi": []}
    port = find_seq_port()
    if any(step_parts(st)[2] for st in ROUNDS[a.round]) and not port:
        print("no FLX10 sequencer port: the MIDI steps will send nothing")
    stop = threading.Event()

    def sender():
        n = 0
        t0 = time.monotonic()
        while not stop.is_set():
            n = n % 5 + 1
            pk = [hl.heartbeat(n)]
            for d in hl.DECKS:
                p = hl.rb_state(d, True, False, 0)
                for k, v in cur["edits"].items():
                    p[k] = v
                pk.append(p)
            try:
                for p in pk:
                    os.write(fd, b"\x00" + bytes(p))
            except OSError as e:
                print("\nwrite failed: %s (FLX10 unplugged?)" % e)
                stop.set()
                return
            if port and cur["midi"] and n == 5:          # every 100 ms
                for m in cur["midi"]:
                    subprocess.run(["aseqsend", "-p", port] + m.split())
            t0 += 0.02
            time.sleep(max(0.0, t0 - time.monotonic()))

    out = open(OUT, "w")
    try:
        input("Load a track on deck 1 and deck 2 and leave both PAUSED (rb up, jogs showing info), then press Enter: ")
        threading.Thread(target=sender, daemon=True).start()
        steps = ROUNDS[a.round]
        for i, st in enumerate(steps, 1):
            if stop.is_set():
                break
            ed, pre, midi, watch = step_parts(st)
            cur["midi"] = []
            if pre:
                cur["edits"] = dict(pre)
                time.sleep(0.8)
            cur["edits"] = dict(ed)
            cur["midi"] = list(midi)
            desc = " ".join("%d=%02x" % kv for kv in sorted(ed.items()))
            if pre:
                desc = "(" + " ".join("%d=%02x" % kv for kv in sorted(pre.items())) + " first) " + desc
            if midi:
                desc = (desc + " " if desc else "") + "MIDI " + " / ".join(midi)
            desc = desc or "(none)"
            time.sleep(0.3)
            print("\n[%d/%d] %s   -- watch: %s" % (i, len(steps), desc, watch))
            ans = input("  what changed? (Enter = nothing): ").strip() or "nothing"
            out.write("%s | %s\n" % (desc, ans))
            out.flush()
    except (KeyboardInterrupt, EOFError):
        print("\nstopped")
    finally:
        stop.set()
        out.close()
        time.sleep(0.1)
        os.close(fd)
        if added:
            conf_set(False)
            restart_rb()
    print("\nsaved to %s:" % OUT)
    print(open(OUT).read())


if __name__ == "__main__":
    main()
