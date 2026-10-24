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

FREE MODE (--free) is for controls the guided list does not know about -- the
FLX10 has more than the FLX4 the list was modelled on. You type a name, then use
the control; it stops recording once everything is released and nothing has
moved for a moment. Names are free text; use the label printed on the unit and
say which deck, e.g. "d1 slip", "d2 vocal", "merge fx". An empty name finishes.

    python3 scripts/odroid/midi-learn.py --free

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
#
# The list follows the DDJ-FLX10's own panel labels (the SHIFT functions are the
# small grey labels under each button). Ids that the first deck-1 pass already
# recorded keep their names, so `--resume` asks only for what is new:
# mode_2/3/4 are PAD FX1 / BEAT JUMP / SAMPLER by position.
def controls():
    out = []

    def deck(n):
        g = "deck%d" % n
        side = "3/1" if n == 1 else "2/4"

        def b(i, p):
            out.append((g, "d%d_%s" % (n, i), "DECK %d: %s" % (n, p), "button"))

        def m(i, p):
            out.append((g, "d%d_%s" % (n, i), "DECK %d: %s" % (n, p), "move"))

        # transport
        b("play", "press PLAY/PAUSE")
        b("cue", "press CUE")
        b("shift", "press SHIFT")
        b("shift_play", "hold SHIFT + press PLAY/PAUSE")
        b("shift_cue", "hold SHIFT + press CUE")
        # sync / tempo / key
        b("sync", "press BEAT SYNC (short)")
        b("shift_sync", "hold SHIFT + press BEAT SYNC (MASTER)")
        b("sync_long", "press and HOLD BEAT SYNC 2 s (SYNC RATE)")
        b("tempo_reset", "press TEMPO RESET")
        b("shift_tempo_reset", "hold SHIFT + press TEMPO RESET (TEMPO RANGE)")
        b("key_sync", "press KEY SYNC (short)")
        b("key_sync_long", "press and HOLD KEY SYNC 2 s (MASTER TEMPO)")
        b("shift_key_sync", "hold SHIFT + press KEY SYNC (KEY RESET)")
        m("tempo", "move the TEMPO fader end to end")
        # loop
        b("loop_in", "press IN / 1/2X")
        b("loop_out", "press OUT / 2X")
        b("loop_4beat", "press 4 BEAT / EXIT")
        b("shift_loop_in", "hold SHIFT + press IN (IN ADJUST)")
        b("shift_loop_out", "hold SHIFT + press OUT (OUT ADJUST / RELOOP)")
        b("shift_4beat", "hold SHIFT + press 4 BEAT/EXIT (ACTIVE)")
        # cue/loop call, mix point, beat jump
        b("call_prev", "press CUE/LOOP CALL <")
        b("call_next", "press CUE/LOOP CALL >")
        b("shift_call_prev", "hold SHIFT + press CALL < (MEMORY)")
        b("shift_call_next", "hold SHIFT + press CALL > (DELETE)")
        b("mixpoint_prev", "press MIX POINT SELECT <")
        b("mixpoint_next", "press MIX POINT SELECT >")
        b("link", "press LINK")
        b("shift_mixpoint_prev", "hold SHIFT + press MIX POINT < (EXIT)")
        b("shift_mixpoint_next", "hold SHIFT + press MIX POINT > (DECK)")
        b("shift_link", "hold SHIFT + press LINK (SILENT)")
        b("beatjump_prev", "press 4 BEAT JUMP <")
        b("beatjump_next", "press 4 BEAT JUMP >")
        b("shift_beatjump_prev", "hold SHIFT + press BEAT JUMP < (SEARCH)")
        b("shift_beatjump_next", "hold SHIFT + press BEAT JUMP > (SEARCH)")
        # slip / quantize / reverse
        b("slip_reverse", "press SLIP REVERSE")
        b("shift_slip_reverse", "hold SHIFT + press SLIP REVERSE (REVERSE)")
        b("quantize", "press QUANTIZE")
        b("shift_quantize", "hold SHIFT + press QUANTIZE (WAKE UP)")
        b("slip", "press SLIP")
        b("shift_slip", "hold SHIFT + press SLIP (VINYL)")
        # this deck's browse knob (top corner) and BACK / VIEW
        m("browse", "turn this deck's top-corner BROWSE knob a few clicks each way")
        m("shift_browse", "hold SHIFT + turn the BROWSE knob a few clicks")
        b("browse_push", "push the BROWSE knob once (LOAD)")
        b("browse_push_double", "push the BROWSE knob TWICE quickly (INST. DOUBLES)")
        b("browse_push_hold", "push and HOLD the BROWSE knob 2 s (TAG TRACK)")
        b("back", "press BACK")
        b("view", "press VIEW")
        b("shift_back", "hold SHIFT + press BACK (PLAYLIST PALETTE)")
        b("shift_view", "hold SHIFT + press VIEW (RELATED TRACKS)")
        # stems
        b("part_drums", "press ACTIVE PART: DRUMS")
        b("part_vocal", "press ACTIVE PART: VOCAL")
        b("part_inst", "press ACTIVE PART: INST")
        b("shift_part_vocal", "hold SHIFT + press VOCAL (PART INSTANT DOUBLES)")
        # deck select, jog
        b("deck_select", "press DECK SELECT %s (then press it again to come back)" % side)
        b("jog_touch", "press DOWN on the jog platter top, then let go")
        m("jog_top", "press the platter top and spin it both ways")
        m("jog_side", "spin the jog by its SIDE ring both ways")
        m("shift_jog", "hold SHIFT + press the platter top and spin it (top ONLY)")
        m("shift_jog_side", "hold SHIFT + spin the SIDE ring only")
        # pad modes and pages
        b("mode_hotcue", "press pad mode HOT CUE")
        b("mode_2", "press pad mode PAD FX1")
        b("mode_3", "press pad mode BEAT JUMP")
        b("mode_4", "press pad mode SAMPLER")
        b("shift_mode_hotcue", "hold SHIFT + press HOT CUE (KEYBOARD)")
        b("shift_mode_2", "hold SHIFT + press PAD FX1 (PAD FX2)")
        b("shift_mode_3", "hold SHIFT + press BEAT JUMP (BEAT LOOP)")
        b("shift_mode_4", "hold SHIFT + press SAMPLER (KEY SHIFT)")
        b("page_prev", "press PAGE <")
        b("page_next", "press PAGE >")
        # pads
        b("pad1_hot", "in HOT CUE mode: press pad 1 (top left)")
        b("pad8_hot", "in HOT CUE mode: press pad 8 (bottom right)")
        b("shift_pad1_hot", "in HOT CUE mode: hold SHIFT + press pad 1")
        b("pad1_mode2", "in PAD FX1 mode: press pad 1")
        b("pad1_mode3", "in BEAT JUMP mode: press pad 1")
        b("pad1_mode4", "in SAMPLER mode: press pad 1")
        b("pad1_beatloop", "in BEAT LOOP mode (SHIFT + BEAT JUMP): press pad 1")
        b("pad1_keyboard", "in KEYBOARD mode (SHIFT + HOT CUE): press pad 1")
        # this deck's mixer channel (channel %d)
        b("ch_cue", "press channel %d's CUE (headphone cue)" % n)
        b("shift_ch_cue", "hold SHIFT + press channel %d's CUE (PART ISO)" % n)
        m("trim", "turn channel %d's TRIM end to end" % n)
        m("eq_hi", "turn channel %d's HI end to end" % n)
        m("eq_mid", "turn channel %d's MID end to end" % n)
        m("eq_low", "turn channel %d's LOW end to end" % n)
        m("cfx", "turn channel %d's COLOR knob end to end" % n)
        m("fader", "move channel %d's FADER end to end" % n)
        m("xf_assign", "move channel %d's crossfader assign switch A-THRU-B-THRU-A" % n)
    deck(1)
    deck(2)

    g = "mixer"

    def b(i, p):
        out.append((g, i, "MIXER: " + p, "button"))

    def m(i, p):
        out.append((g, i, "MIXER: " + p, "move"))
    m("xfader", "move the CROSSFADER end to end")
    m("master_level", "turn MASTER LEVEL end to end")
    m("booth_level", "turn BOOTH LEVEL end to end")
    b("master_cue", "press the master CUE (under the level meter)")
    m("hp_mix", "turn HEADPHONES MIX end to end")
    m("hp_level", "turn HEADPHONES LEVEL end to end")
    b("sampler_cue", "press SAMPLER CUE")
    m("sampler_vol", "turn SAMPLER VOL end to end")
    b("scfx_space", "press SOUND COLOR FX: SPACE")
    b("scfx_decho", "press SOUND COLOR FX: D.ECHO")
    b("scfx_crush", "press SOUND COLOR FX: CRUSH")
    b("scfx_pitch", "press SOUND COLOR FX: PITCH")
    b("scfx_noise", "press SOUND COLOR FX: NOISE")
    b("scfx_filter", "press SOUND COLOR FX: FILTER")
    b("ch3_cue", "press channel 3's CUE (to learn the 3/4 strips)")
    m("ch3_fader", "move channel 3's FADER")
    b("ch4_cue", "press channel 4's CUE")

    g = "fx"

    def b(i, p):
        out.append((g, i, "BEAT FX: " + p, "button"))

    def m(i, p):
        out.append((g, i, "BEAT FX: " + p, "move"))
    b("part_vocal", "press FX PART SELECT: VOCAL (on the ring)")
    b("part_drums", "press FX PART SELECT: DRUMS")
    b("part_inst", "press FX PART SELECT: INST")
    b("beat_prev", "press BEAT <")
    b("beat_next", "press BEAT >")
    b("shift_beat_prev", "hold SHIFT + press BEAT < (AUTO)")
    b("shift_beat_next", "hold SHIFT + press BEAT > (TAP)")
    m("fx_select", "turn the effect selector knob through every effect")
    m("fx_channel", "turn the channel selector (3 2 1 MIC SP MST) through every position")
    m("fx_level", "turn LEVEL/DEPTH end to end")
    b("fx_on", "press BEAT FX ON/OFF")
    b("shift_fx_on", "hold SHIFT + press ON/OFF (RELEASE FX)")
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


def record_auto(midi, idle_s=0.8, cap_s=20.0):
    """Free mode: wait for the first message, then record until every note/CC
    that went non-zero is back at zero AND nothing has arrived for idle_s.
    Works for buttons and for knobs alike: stop moving and it ends."""
    msgs = []
    while True:
        r, _, _ = select.select([sys.stdin, midi.fd], [], [], 0.5)
        if sys.stdin in r:
            k = sys.stdin.read(1)
            if k in ("\r", "\n"):
                return "skip", []
        if midi.fd in r:
            got = midi.read()
            if got:
                msgs += [(time.time(), m) for m in got]
                break
    start = last = time.time()
    while time.time() - start < cap_s:
        got = midi.read()
        if got:
            last = time.time()
            msgs += [(last, m) for m in got]
        notes = {}
        for _, (st, d1, d2) in msgs:
            if st & 0xF0 in (0x80, 0x90):
                notes[(st & 0x0F, d1)] = 0 if (st & 0xF0) == 0x80 else d2
        if all(v == 0 for v in notes.values()) and time.time() - last > idle_s:
            break
        time.sleep(0.005)
    return "ok", msgs


def free_mode(midi, data, save):
    print("FREE MODE: type a name and press Enter, then use the control.")
    print("While waiting for the control, Enter cancels that name. Empty name = done.\n")
    old = termios.tcgetattr(sys.stdin)
    while True:
        termios.tcsetattr(sys.stdin, termios.TCSADRAIN, old)
        try:
            name = input("name> ").strip()
        except EOFError:
            break
        if not name:
            break
        cid = "free_" + "_".join(name.lower().split())
        tty.setcbreak(sys.stdin.fileno())
        midi.drain()
        sys.stdout.write("   now use '%s' ... " % name)
        sys.stdout.flush()
        what, msgs = record_auto(midi)
        termios.tcsetattr(sys.stdin, termios.TCSADRAIN, old)
        if what == "skip":
            print("cancelled")
            continue
        sm = summarise(msgs)
        data["controls"][cid] = {"group": "free", "prompt": name, "kind": "auto",
                                 "messages": sm, "raw": [list(m) for _, m in msgs[:64]]}
        print("; ".join("%s ch%d #%d x%d [%d..%d]" % (e["type"], e["ch"], e["num"],
                        e["count"], e["min"], e["max"]) for e in sm))
        save()


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
    ap.add_argument("--free", action="store_true",
                    help="name each control yourself (for what the guided list lacks)")
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

    if a.free:
        print("reading %s (%s)." % (path, a.card))
        free_mode(midi, data, save)
        save()
        print("\nwrote %s (%d controls)" % (out, len(data["controls"])))
        return 0

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
