#!/usr/bin/env python3
"""led-probe.py -- find which MIDI message lights which LED on a surface, guided.

The LED half of midi-learn.py (docs/21, "FLX10 LEDs"). On the DDJ-FLX4 every LED
turned out to be lit by its button's OWN note on its own channel (velocity 127 =
on, 0 = off); this checks that for the DDJ-FLX10 one LED at a time instead of
assuming it, because a wrong guess sent to a surface can be a phantom control
rather than a dark light. Pure Python 3, stdlib only: it writes the card's raw
MIDI device directly.

The player must NOT be holding the port: stop it first.

    systemctl stop rbp
    python3 scripts/odroid/led-probe.py                # everything
    python3 scripts/odroid/led-probe.py --only pads    # one group
    python3 scripts/odroid/led-probe.py --list         # what it will try
    python3 scripts/odroid/led-probe.py --send         # type raw messages
    systemctl start rbp

At each prompt the candidate is ON:
    y        the named LED lit
    n        nothing lit
    anything else   a description, saved as typed ("deck 2's PLAY lit", "red")
    r        send it again
    q        save and quit (--resume carries on)

Groups: deck (both decks' buttons), modes, mixer, fx, pads (one pad's colour
palette), meters. Output: flx10-leds.json beside the repo root.
"""
import argparse
import json
import os
import sys
import time

# (group, id, what should light, channel 1-16, note)
def notes():
    out = []
    for d in (1, 2):
        g = "deck"
        for nid, n, what in (
            ("play", 11, "PLAY/PAUSE"),
            ("cue", 12, "CUE"),
            ("sync", 88, "BEAT SYNC"),
            ("keysync", 101, "KEY SYNC"),
            ("keysync_long", 26, "KEY SYNC (or anything)"),
            ("tempo_reset", 65, "TEMPO RESET"),
            ("slip", 64, "SLIP"),
            ("slip_reverse", 21, "SLIP REVERSE"),
            ("quantize", 53, "QUANTIZE"),
            ("loop_in", 16, "loop IN"),
            ("loop_out", 17, "loop OUT"),
            ("loop_4beat", 20, "4 BEAT / EXIT"),
            ("call_prev", 81, "CUE/LOOP CALL <"),
            ("call_next", 83, "CUE/LOOP CALL >"),
            ("link", 74, "LINK"),
            ("part_drums", 13, "ACTIVE PART: DRUMS"),
            ("part_vocal", 14, "ACTIVE PART: VOCAL"),
            ("part_inst", 15, "ACTIVE PART: INST"),
            ("ch_cue", 84, "channel %d CUE" % d),
            ("shift", 63, "SHIFT"),
            ("jog_touch", 54, "anything on the jog / its display"),
        ):
            out.append((g, "d%d_%s" % (d, nid), "DECK %d: %s" % (d, what), d, n))
        g = "modes"
        for nid, n, what in (
            ("mode_hotcue", 27, "pad mode HOT CUE"),
            ("mode_padfx1", 30, "pad mode PAD FX1"),
            ("mode_beatjump", 32, "pad mode BEAT JUMP"),
            ("mode_sampler", 34, "pad mode SAMPLER"),
            ("mode_keyboard", 105, "HOT CUE (as KEYBOARD) or anything"),
            ("mode_padfx2", 107, "PAD FX1 (as PAD FX2) or anything"),
            ("mode_beatloop", 109, "BEAT JUMP (as BEAT LOOP) or anything"),
            ("mode_keyshift", 111, "SAMPLER (as KEY SHIFT) or anything"),
            ("page_prev", 43, "PAGE <"),
            ("page_next", 51, "PAGE >"),
        ):
            out.append((g, "d%d_%s" % (d, nid), "DECK %d: %s" % (d, what), d, n))
    g = "mixer"
    out += [
        (g, "ch3_cue", "channel 3 CUE", 3, 84),
        (g, "ch4_cue", "channel 4 CUE", 4, 84),
        (g, "master_cue", "the master CUE (by the meter)", 7, 99),
        (g, "sampler_cue", "SAMPLER CUE", 7, 105),
        (g, "scfx_space", "SOUND COLOR FX: SPACE", 7, 0),
        (g, "scfx_decho", "SOUND COLOR FX: D.ECHO", 7, 1),
        (g, "scfx_crush", "SOUND COLOR FX: CRUSH", 7, 2),
        (g, "scfx_pitch", "SOUND COLOR FX: PITCH", 7, 3),
        (g, "scfx_noise", "SOUND COLOR FX: NOISE", 7, 4),
        (g, "scfx_filter", "SOUND COLOR FX: FILTER", 7, 5),
        (g, "back", "BACK", 7, 101),
        (g, "view", "VIEW", 7, 122),
    ]
    g = "fx"
    out += [
        (g, "fx_on", "BEAT FX ON/OFF", 5, 71),
        (g, "beat_prev", "BEAT <", 5, 74),
        (g, "beat_next", "BEAT >", 5, 75),
        (g, "part_vocal", "FX PART SELECT: VOCAL", 7, 27),
        (g, "part_drums", "FX PART SELECT: DRUMS", 7, 28),
        (g, "part_inst", "FX PART SELECT: INST", 7, 29),
        (g, "fx_assign_1", "the BEAT FX light under channel 1", 1, 71),
        (g, "fx_assign_1b", "the BEAT FX light under channel 1 (2nd guess)", 5, 76),
    ]
    return out


# Meters: (id, what should move, channel, cc). On the FLX4 a channel meter is CC 2
# on the deck's channel, 0x26 (bottom) .. 0x7F (top).
METERS = [
    ("meter_ch1", "channel 1's level meter", 1, 2),
    ("meter_ch2", "channel 2's level meter", 2, 2),
    ("meter_ch3", "channel 3's level meter", 3, 2),
    ("meter_ch4", "channel 4's level meter", 4, 2),
    ("meter_master_l", "the MASTER meter, left", 7, 2),
    ("meter_master_r", "the MASTER meter, right", 7, 3),
]


def find_card(name):
    with open("/proc/asound/cards") as f:
        for line in f:
            parts = line.split()
            if len(parts) > 1 and parts[0].isdigit() and name.lower() in line.lower():
                return int(parts[0])
    return None


class Out:
    def __init__(self, path):
        self.fd = os.open(path, os.O_WRONLY)

    def send(self, *b):
        os.write(self.fd, bytes(b))

    def note(self, ch, n, v):
        self.send(0x90 | (ch - 1), n & 0x7F, v & 0x7F)

    def cc(self, ch, c, v):
        self.send(0xB0 | (ch - 1), c & 0x7F, v & 0x7F)


def ask(prompt):
    try:
        return input(prompt).strip()
    except EOFError:
        return "q"


def probe_notes(out, items, data, save):
    for g, cid, what, ch, n in items:
        while True:
            out.note(ch, n, 127)
            a = ask("[%s] ch%d note %d vel 127 -> did %s light? " % (cid, ch, n, what))
            out.note(ch, n, 0)
            if a.lower() == "r":
                continue
            break
        if a.lower() == "q":
            return False
        if a.lower() in ("y", "yes"):
            res = "lit"
        elif a.lower() in ("n", "no", ""):
            res = "nothing"
        else:
            res = a
        data["notes"][cid] = {"group": g, "expect": what, "ch": ch, "note": n, "result": res}
        # No brightness question: on the FLX10 a button LED at velocity 1 looked
        # exactly like 127 every time (operator, 2026-10-24) -- on/off only.
        save()
    return True


def probe_pads(out, data, save):
    """One pad's colour: pad 1 of deck 1 in HOT CUE (ch 8 note 0), velocity by
    velocity in steps of 4. The FLX10's pads are RGB; this finds out whether
    velocity picks a colour, and from what table."""
    ch, n = 8, 0
    print("\nPAD COLOURS: deck 1, pad 1 (top left). Put deck 1 in HOT CUE first.")
    print("Type the colour you see for each velocity (Enter = same as the last, "
          "'off' = dark, q = stop).")
    pal = data.setdefault("pad_palette", {})
    last = ""
    for v in list(range(0, 128, 4)) + [127]:
        if str(v) in pal:
            continue
        out.note(ch, n, v)
        a = ask("  vel %3d -> colour? " % v)
        if a.lower() == "q":
            out.note(ch, n, 0)
            save()
            return False
        if a == "":
            a = last
        last = a
        pal[str(v)] = a
        save()
    out.note(ch, n, 0)
    a = ask("  Does pad 1 in other modes use other notes? Light ch8 note 16 now? (y to try) ")
    if a.lower().startswith("y"):
        out.note(8, 16, 64)
        b = ask("  ch8 note 16 vel 64 -> did a pad light (which)? ")
        out.note(8, 16, 0)
        data["notes"]["pad_base16"] = {"ch": 8, "note": 16, "result": b}
        out.note(10, 0, 64)
        b = ask("  ch10 note 0 vel 64 -> did deck 2's pad 1 light? ")
        out.note(10, 0, 0)
        data["notes"]["pad_deck2"] = {"ch": 10, "note": 0, "result": b}
        save()
    return True


def probe_meters(out, data, save):
    print("\nMETERS: each candidate is swept bottom to top and back twice.")
    for mid, what, ch, c in METERS:
        if mid in data["meters"]:
            continue
        for _ in range(2):
            for v in list(range(0, 128, 6)) + list(range(127, -1, -6)):
                out.cc(ch, c, v)
                time.sleep(0.02)
        out.cc(ch, c, 0)
        a = ask("[%s] ch%d CC %d -> did %s move? " % (mid, ch, c, what))
        if a.lower() == "q":
            return False
        data["meters"][mid] = {"ch": ch, "cc": c, "expect": what,
                               "result": "moved" if a.lower() in ("y", "yes") else (a or "nothing")}
        save()
    return True


def raw_send(out):
    print("Type messages as hex bytes, e.g. '90 0B 7F' (note on ch1 #11 vel 127). "
          "Empty line quits.")
    while True:
        a = ask("send> ")
        if not a or a.lower() == "q":
            return
        try:
            out.send(*[int(x, 16) for x in a.split()])
        except (ValueError, OSError) as e:
            print("  not sent:", e)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--card", default="DDJFLX10")
    ap.add_argument("--out", default=None)
    ap.add_argument("--only", default=None, help="deck modes mixer fx pads meters")
    ap.add_argument("--resume", action="store_true")
    ap.add_argument("--list", action="store_true")
    ap.add_argument("--send", action="store_true", help="type raw messages instead")
    a = ap.parse_args()

    items = notes()
    if a.list:
        for g, cid, what, ch, n in items:
            print("%-6s %-20s ch%-2d note %-3d %s" % (g, cid, ch, n, what))
        for mid, what, ch, c in METERS:
            print("%-6s %-20s ch%-2d cc   %-3d %s" % ("meters", mid, ch, c, what))
        print("pads   (deck 1 pad 1's colour by velocity, 0..127 step 4)")
        return 0

    repo = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
    path = a.out or os.path.join(repo, "flx10-leds.json")
    data = {"card": a.card, "notes": {}, "meters": {}}
    if os.path.exists(path):
        with open(path) as f:
            data = json.load(f)
        data.setdefault("notes", {})
        data.setdefault("meters", {})

    def save():
        tmp = path + ".tmp"
        with open(tmp, "w") as f:
            json.dump(data, f, indent=1, sort_keys=True)
        os.replace(tmp, path)

    card = find_card(a.card)
    if card is None:
        print("no ALSA card matching %r -- is it plugged in?" % a.card)
        return 1
    dev = "/dev/snd/midiC%dD0" % card
    try:
        out = Out(dev)
    except OSError as e:
        print("cannot open %s: %s" % (dev, e))
        print("if it is busy, the player holds it: systemctl stop rbp")
        return 1

    if a.send:
        raw_send(out)
        return 0

    groups = [a.only] if a.only else ["deck", "modes", "mixer", "fx", "pads", "meters"]
    print("writing %s (%s). y = lit, n = nothing, text = what you saw, r = again, "
          "q = save+quit.\n" % (dev, a.card))
    for g in groups:
        if g == "pads":
            if not probe_pads(out, data, save):
                break
        elif g == "meters":
            if not probe_meters(out, data, save):
                break
        else:
            todo = [i for i in items if i[0] == g and
                    not (a.resume and i[1] in data["notes"])]
            if not probe_notes(out, todo, data, save):
                break
    save()
    print("\nwrote %s" % path)
    return 0


if __name__ == "__main__":
    sys.exit(main())
