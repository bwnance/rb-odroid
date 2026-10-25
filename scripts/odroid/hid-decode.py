#!/usr/bin/env python3
"""hid-decode.py -- step through byte edits of the FLX10's xx 21 HID packet.

Turns rb's own HID sender off (RB_FLX10_HID=0 in rb.local.conf, rb restarted),
waits for you to load a track on both decks, then sends hid-loaded.py's
"loaded, paused" packet with one edit at a time and asks what changed on the
jogs. Answers go to ~/flx10-hid.txt. rb.local.conf and rb are put back at the
end, Ctrl-C included.

    python3 scripts/odroid/hid-decode.py
    python3 scripts/odroid/hid-decode.py --keep-rb   # leave rb.local.conf alone
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
STEPS = [
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
    cur = {"edits": {}}
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
            t0 += 0.02
            time.sleep(max(0.0, t0 - time.monotonic()))

    out = open(OUT, "w")
    try:
        input("Load a track on deck 1 and deck 2 (rb up, jogs showing info), then press Enter: ")
        threading.Thread(target=sender, daemon=True).start()
        for i, (ed, watch) in enumerate(STEPS, 1):
            if stop.is_set():
                break
            cur["edits"] = dict(ed)
            desc = " ".join("%d=%02x" % kv for kv in sorted(ed.items())) or "(none)"
            time.sleep(0.3)
            print("\n[%d/%d] %s   -- watch: %s" % (i, len(STEPS), desc, watch))
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
