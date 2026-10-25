#!/bin/sh
# arm-probe.sh -- find what turns the FLX10 jog's clock arm on, per deck.
#
# Runs ALONGSIDE rb: rb keeps the unit unlocked, polled (the 200 ms keepalive)
# and on its deck screens, and sends the arm position itself. Stopping rb -- what
# the first version did -- left the jogs on the splash screen, where no arm can
# show. rb sends its display connect sequence only on (re)connect now, so nothing
# undoes a flip here while you look.
#
# Load a track on both decks and play them, then for each candidate ch-16 note
# it sends 7F, asks, sends 00, asks, and puts back the value rekordbox's capture
# has (00 for the ones the capture does not send). Answers go to ~/flx10-arm.txt.
#   sh scripts/odroid/arm-probe.sh
set -u
PORT=$(aseqdump -l | awk '/FLX10/{print $1; exit}')
[ -n "$PORT" ] || { echo "no FLX10 sequencer port"; exit 1; }
systemctl is-active --quiet rblive4 || { echo "rb is not running; start it first"; exit 1; }
OUT=~/flx10-arm.txt; : > "$OUT"
s() { aseqsend -p "$PORT" "$@"; }

# the value each note has in rekordbox's connect sequence (map_flx10.c's
# flx10_init_short); anything not listed is not sent there and goes back to 00
captured() {
  case $1 in
    3C|3D|41|47|70) echo 00 ;;
    3F|40|42|45|48|49|4A|4C|4D|4E) echo 7F ;;
    *) echo 00 ;;
  esac
}
ask() {
  printf '%s -> deck 1 arm? [y/n]  deck 2 arm? [y/n]  (e.g. "n y"): ' "$1"
  read -r a b
  echo "$1 | deck1=$a deck2=$b" | tee -a "$OUT"
}

echo "Load a track on deck 1 and deck 2 and press PLAY on both, then press Enter."
read -r _
ask "baseline (nothing changed)"

for n in 3C 3D 3E 3F 40 41 42 43 44 45 46 47 48 49 4A 4B 4C 4D 4E 4F 70; do
  s 9F $n 7F; sleep 0.3; ask "9F $n 7F"
  s 9F $n 00; sleep 0.3; ask "9F $n 00"
  s 9F $n "$(captured $n)"
done

echo; echo "saved to $OUT"; cat "$OUT"
