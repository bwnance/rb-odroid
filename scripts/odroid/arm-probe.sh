#!/bin/sh
# arm-probe.sh -- find what turns the FLX10 jog's clock arm on, per deck.
#
# Stops rb (so nothing else talks to the unit), sends the vendor unlock and
# rekordbox's display connect sequence, puts both decks' info screens up with
# their arm at 3 o'clock, then flips ONE candidate message at a time and asks
# whether deck 1's / deck 2's arm is visible. Answers go to ~/flx10-arm.txt.
#   sh scripts/odroid/arm-probe.sh          (rb is restarted at the end)
set -u
PORT=$(aseqdump -l | awk '/FLX10/{print $1; exit}')
[ -n "$PORT" ] || { echo "no FLX10 sequencer port"; exit 1; }
OUT=~/flx10-arm.txt; : > "$OUT"
HERE=$(cd "$(dirname "$0")/.." && pwd)
s() { aseqsend -p "$PORT" "$@"; }

systemctl stop rblive4
python3 "$HERE/device/flx10-unlock.py" >/dev/null 2>&1
sleep 0.5

init() {
  s F0 00 40 05 00 00 04 01 00 00 0A 00 28 00 26 00 1A 3D 12 14 29 55 29 20 28 49 4A 04 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 F7
  s F0 00 40 05 00 00 04 01 00 00 0B 2B 68 00 00 00 00 F7
  s F0 00 40 05 00 00 04 01 00 00 0C 00 00 02 0E 0E 00 00 00 F7
  for m in "9F 70 00" "9F 40 7F" "9F 41 00" "9F 42 7F" "BF 46 30" "BF 3E 20" "9F 3C 00" \
           "9F 3D 00" "9F 3F 7F" "BF 0D 00" "BF 41 0A" "BF 42 00" "9F 4D 7F" "9F 47 00" \
           "BF 43 00" "9F 48 7F" "9F 49 7F" "9F 4A 7F" "9F 4E 7F" "BF 44 30" "9F 45 7F" \
           "9F 4C 7F" "BF 47 20" "BF 49 20" "BF 48 40" "BF 4A 20"; do s $m; done
  # info screens on, times/BPM so there is something on them, arms at 90 deg
  s 9F 5D 00; s 9F 5E 00
  s BF 14 06 BF 34 40; s BF 15 06 BF 35 40
  s BF 10 00 BF 30 5A; s BF 11 00 BF 31 5A
}
ask() {
  printf '%s -> deck 1 arm? [y/n]  deck 2 arm? [y/n]  (e.g. "n y"): ' "$1"
  read -r a b
  echo "$1 | deck1=$a deck2=$b" | tee -a "$OUT"
}

init; sleep 0.3
ask "baseline (rekordbox's sequence as captured)"

# each candidate: the captured message, flipped (7F<->00), with the rest as captured
for c in "9F 40" "9F 41" "9F 42" "9F 43" "9F 3C" "9F 3D" "9F 3E" "9F 3F" "9F 44" "9F 45" \
         "9F 46" "9F 47" "9F 48" "9F 49" "9F 4A" "9F 4B" "9F 4C" "9F 4D" "9F 4E" "9F 4F" "9F 70"; do
  init
  s $c 7F; s BF 10 00 BF 30 5A; s BF 11 00 BF 31 5A; sleep 0.2
  ask "$c 7F"
  init
  s $c 00; s BF 10 00 BF 30 5A; s BF 11 00 BF 31 5A; sleep 0.2
  ask "$c 00"
done

echo; echo "saved to $OUT"; cat "$OUT"
systemctl start rblive4
