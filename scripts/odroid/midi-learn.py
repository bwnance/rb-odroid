#!/usr/bin/env python3
"""midi-learn.py -- record what each control of a MIDI surface sends, guided.

Built for mapping the DDJ-FLX10 (docs/21, "FLX10"), where no published MIDI
message list was at hand. It prompts for one control at a time, records the
messages that control produces, and writes them to a JSON file a map can be
written from. Pure Python 3, no modules beyond the standard library: it reads the
card's raw MIDI device directly.

The player must NOT be holding the port: stop it first.

    systemctl stop rblive4
    python3 scripts/odroid/midi-learn.py                 # all prompts
    python3 scripts/odroid/midi-learn.py --only mixer    # one group
    python3 scripts/odroid/midi-learn.py --list          # what it will ask
    systemctl start rblive4

At each prompt:  do the action, and it records it.
    Enter   skip this control (records it as skipped)
    r       redo the previous control
    q       save and quit (re-run with --resume to carry on)

Buttons are recorded from the first press until the release. Knobs, faders and
jogs are recorded for a few seconds after the first message: move them through
their whole range. Output: flx10-learn.json (the data) beside the repo root.
"""
import argparse
import json
import os
import select
import sys
import termios
import time
import tty

# (group, id, prompt, kind). kind: "button" or "move".
def controls():
    out = []
    def deck(n):
        g = "deck%d" % n
        b = lambda i, p: out.append((g, "d%d_%s" % (n, i), "DECK %d: %s" % (n, p), "button"))
        m = lambda i, p: out.append((g, "d%d_%s" % (n, i), "DECK %d: %s" % (n, p), "move"))
        b("play", "press PLAY/PAUSE")
        b("cue", "press CUE")
        b("shift", "press SHIFT")
        b("shift_play", "hold SHIFT and press PLAY/PAUSE")
        b("shift_cue", "hold SHIFT and press CUE")
        b("sync", "press BEAT SYNC (short)")
        b("sync_long", "press and HOLD BEAT SYNC for 2 s")
        b("master", "press MASTER (tempo master), if the deck has one")
        b("keylock", "press KEY LOCK / MASTER TEMPO, if present")
        b("tempo_range", "press the TEMPO RANGE button, if present")
        b("loop_in", "press loop IN")
        b("loop_out", "press loop OUT")
        b("loop_4beat", "press 4 BEAT / RELOOP-EXIT (or AUTO LOOP)")
        b("call_prev", "press CUE/LOOP CALL < (or loop 1/2X)")
        b("call_next", "press CUE/LOOP CALL > (or loop 2X)")
        b("jog_touch", "touch the TOP of the jog platter, then let go")
        m("jog_top", "spin the jog by its TOP (while touching) both ways")
        m("jog_side", "spin the jog by its SIDE ring both ways")
        m("shift_jog", "hold SHIFT and spin the jog top")
        m("tempo", "move the TEMPO fader end to end")
        b("deck_select", "press the DECK 1/3 or 2/4 select button for this side (then back)")
        b("mode_hotcue", "press pad mode HOT CUE")
        b("mode_2", "press the 2nd pad mode button (whatever it is labelled)")
        b("mode_3", "press the 3rd pad mode button")
        b("mode_4", "press the 4th pad mode button")
        b("mode_shift", "hold SHIFT and press HOT CUE mode (secondary mode)")
        b("pad1_hot", "in HOT CUE mode: press pad 1")
        b("pad8_hot", "in HOT CUE mode: press pad 8")
        b("shift_pad1_hot", "in HOT CUE mode: hold SHIFT and press pad 1")
        b("pad1_mode2", "in the 2nd pad mode: press pad 1")
        b("pad1_mode3", "in the 3rd pad mode: press pad 1")
        b("pad1_mode4", "in the 4th pad mode: press pad 1")
        b("ch_cue", "press this deck's channel headphone CUE")
        m("trim", "turn this channel's TRIM end to end")
        m("eq_hi", "turn this channel's EQ HI end to end")
        m("eq_mid", "turn this channel's EQ MID end to end")
        m("eq_low", "turn this channel's EQ LOW end to end")
        m("cfx", "turn this channel's SOUND COLOR FX / FILTER knob end to end")
        m("fader", "move this channel's FADER end to end")
    deck(1)
    deck(2)

    g = "mixer"
    b = lambda i, p: out.append((g, i, "MIXER: " + p, "button"))
    m = lambda i, p: out.append((g, i, "MIXER: " + p, "move"))
    m("xfader", "move the CROSSFADER end to end")
    m("master_level", "turn MASTER LEVEL end to end")
    m("booth_level", "turn BOOTH LEVEL end to end, if present")
    m("hp_mix", "turn HEADPHONES MIX end to end")
    m("hp_level", "turn HEADPHONES LEVEL end to end")
    b("master_cue", "press MASTER CUE")
    m("browse", "turn the BROWSE knob a few clicks each way")
    m("shift_browse", "hold SHIFT and turn BROWSE a few clicks")
    b("browse_push", "push the BROWSE knob")
    b("shift_browse_push", "hold SHIFT and push BROWSE")
    b("back", "press BACK / VIEW, if present")
    b("load1", "press LOAD for deck 1")
    b("load2", "press LOAD for deck 2")
    b("shift_load1", "hold SHIFT and press LOAD deck 1")
    b("cfx_select", "press a SOUND COLOR FX select button (any one)")
    b("ch3_cue", "press channel 3's headphone CUE (to learn the 3/4 strips)")
    m("ch3_fader", "move channel 3's FADER")

    g = "fx"
    b = lambda i, p: out.append((g, i, "BEAT FX: " + p, "button"))
    m = lambda i, p: out.append((g, i, "BEAT FX: " + p, "move"))
    b("fx_select", "press FX SELECT / the effect selector")
    b("beat_prev", "press BEAT <")
    b("beat_next", "press BEAT >")
    m("fx_channel", "move the FX CHANNEL SELECT switch through every position")
    m("fx_level", "turn LEVEL/DEPTH end to end")
    b("fx_on", "press BEAT FX ON/OFF")
    b("shift_fx_on", "hold SHIFT and press BEAT FX ON/OFF")
    return out


def find_card(name):
    with open("/proc/asound/cards") as f:
        for line in f:
            parts = line.split()
            if len(parts) > 1 and parts[0].isdigit() and name.lower() in line.lower():
                return int(parts[0])
    return None


class Midi:
    """Raw MIDI in, parsed into (status, data1, data2) with running status."""
    LEN = {0x80: 2, 0x90: 2, 0xA0: 2, 0xB0: 2, 0xC0: 1, 0xD0: 1, 0xE0: 2}

    def __init__(self, path):
        self.fd = os.open(path, os.O_RDONLY | os.O_NONBLOCK)
        self.status = None
        self.data = []
        self.in_sysex = False

    def read(self):
        msgs = []
        try:
            buf = os.read(self.fd, 4096)
        except BlockingIOError:
            return msgs
        for byte in buf:
            if byte >= 0xF8:            # realtime: clock etc., ignore
                continue
            if byte == 0xF0:
                self.in_sysex = True
                continue
            if byte == 0xF7:
                self.in_sysex = False
                continue
            if self.in_sysex:
                continue
            if byte & 0x80:
                self.status = byte
                self.data = []
                continue
            if self.status is None:
                continue
            self.data.append(byte)
            if len(self.data) == self.LEN.get(self.status & 0xF0, 2):
                d = self.data + [0]
                msgs.append((self.status, d[0], d[1]))
                self.data = []
        return msgs

    def drain(self, secs=0.15):
        end = time.time() + secs
        while time.time() < end:
            self.read()
            time.sleep(0.01)


def describe(status, d1, d2):
    kind = status & 0xF0
    ch = (status & 0x0F) + 1
    name = {0x90: "note", 0x80: "note-off", 0xB0: "cc", 0xE0: "pitchbend",
            0xA0: "aftertouch", 0xC0: "program", 0xD0: "chanpress"}.get(kind, hex(kind))
    if kind == 0x90 and d2 == 0:
        name = "note-off"
    return name, ch


def summarise(msgs):
    """Group messages by (type, channel, number); keep count and value range."""
    seen = {}
    order = []
    for t, (s, d1, d2) in msgs:
        name, ch = describe(s, d1, d2)
        if name == "note-off":
            name = "note"
        key = (name, ch, d1)
        if key not in seen:
            seen[key] = {"type": name, "ch": ch, "num": d1, "count": 0,
                         "min": d2, "max": d2, "first": d2, "values": []}
            order.append(key)
        e = seen[key]
        e["count"] += 1
        e["min"] = min(e["min"], d2)
        e["max"] = max(e["max"], d2)
        if len(e["values"]) < 24:
            e["values"].append(d2)
    return [seen[k] for k in order]


def getkey(timeout):
    r, _, _ = select.select([sys.stdin], [], [], timeout)
    if r:
        return sys.stdin.read(1)
    return None


def record(midi, kind, idle_s, move_s):
    """Wait for the first message or a key; then collect per kind."""
    msgs = []
    while True:
        r, _, _ = select.select([sys.stdin, midi.fd], [], [], 0.5)
        if sys.stdin in r:
            k = sys.stdin.read(1)
            if k in ("\r", "\n"):
                return "skip", []
            if k in ("q", "Q"):
                return "quit", []
            if k in ("r", "R"):
                return "redo", []
        if midi.fd in r:
            got = midi.read()
            if got:
                now = time.time()
                msgs += [(now, m) for m in got]
                break
    start = time.time()
    if kind == "button":
        # until every note/CC that went "on" has come back to 0, plus a tail,
        # capped so a held long-press (BEAT SYNC long) is still caught whole
        last = time.time()
        while time.time() - start < 6.0:
            got = midi.read()
            if got:
                last = time.time()
                msgs += [(last, m) for m in got]
            down = {}
            for _, (s, d1, d2) in msgs:
                k = (s & 0x0F, d1, s & 0xF0 in (0x80, 0x90))
                down[k] = d2 if (s & 0xF0) != 0x80 else 0
            if all(v == 0 for v in down.values()) and time.time() - last > idle_s:
                break
            time.sleep(0.005)
    else:
        last = time.time()
        while time.time() - start < move_s or time.time() - last < idle_s:
            got = midi.read()
            if got:
                last = time.time()
                msgs += [(last, m) for m in got]
            if time.time() - start > move_s + 10:
                break
            time.sleep(0.005)
    return "ok", msgs


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--card", default="DDJFLX10", help="ALSA card id/name substring")
    ap.add_argument("--out", default=None, help="output JSON (default: repo/flx10-learn.json)")
    ap.add_argument("--only", default=None, help="one group: deck1 deck2 mixer fx")
    ap.add_argument("--resume", action="store_true", help="skip ids already in --out")
    ap.add_argument("--list", action="store_true", help="print the prompts and exit")
    ap.add_argument("--move-secs", type=float, default=4.0)
    a = ap.parse_args()

    items = controls()
    if a.only:
        items = [c for c in items if c[0] == a.only]
    if a.list:
        for g, i, p, k in items:
            print("%-7s %-22s %-6s %s" % (g, i, k, p))
        return 0

    repo = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
    out = a.out or os.path.join(repo, "flx10-learn.json")
    data = {"card": a.card, "controls": {}}
    if os.path.exists(out):
        with open(out) as f:
            data = json.load(f)
    if a.resume:
        items = [c for c in items if c[1] not in data["controls"]]

    card = find_card(a.card)
    if card is None:
        print("no ALSA card matching %r in /proc/asound/cards -- is it plugged in?" % a.card)
        return 1
    path = "/dev/snd/midiC%dD0" % card
    try:
        midi = Midi(path)
    except OSError as e:
        print("cannot open %s: %s" % (path, e))
        print("if it is busy, the player holds it: systemctl stop rblive4")
        return 1

    def save():
        tmp = out + ".tmp"
        with open(tmp, "w") as f:
            json.dump(data, f, indent=1, sort_keys=True)
        os.replace(tmp, out)

    print("reading %s (%s). %d prompts. Enter = skip, r = redo, q = save+quit.\n"
          % (path, a.card, len(items)))
    old = termios.tcgetattr(sys.stdin)
    tty.setcbreak(sys.stdin.fileno())
    try:
        i = 0
        while i < len(items):
            g, cid, prompt, kind = items[i]
            midi.drain()
            sys.stdout.write("[%d/%d] %s ... " % (i + 1, len(items), prompt))
            sys.stdout.flush()
            what, msgs = record(midi, kind, 0.4 if kind == "button" else 0.8, a.move_secs)
            if what == "quit":
                print("saving and quitting")
                break
            if what == "redo":
                print("redo")
                i = max(0, i - 1)
                continue
            if what == "skip":
                data["controls"][cid] = {"group": g, "prompt": prompt, "kind": kind, "skipped": True}
                print("skipped")
            else:
                s = summarise(msgs)
                data["controls"][cid] = {"group": g, "prompt": prompt, "kind": kind,
                                         "messages": s, "raw": [list(m) for _, m in msgs[:64]]}
                print("; ".join("%s ch%d #%d x%d [%d..%d]" % (e["type"], e["ch"], e["num"],
                                e["count"], e["min"], e["max"]) for e in s))
            save()
            i += 1
    finally:
        termios.tcsetattr(sys.stdin, termios.TCSADRAIN, old)
    save()
    print("\nwrote %s (%d controls)" % (out, len(data["controls"])))
    return 0


if __name__ == "__main__":
    sys.exit(main())
