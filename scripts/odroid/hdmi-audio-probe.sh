#!/bin/sh
# hdmi-audio-probe.sh -- find how the ODROID-N2's HDMI audio is reached, for the
# HDMI audio mirror (RB_AUDIO_MIRROR_DEV, docs/21).
#
# The N2's sound card (G12A-ODROIDN2, axg-sound-card, ALSA id ODROIDN2) reaches
# HDMI through a routing chain that is OFF until mixer controls set it:
#   FRDDR_A (PCM device 0) -> TDMOUT_B -> TDM-B -> TOHDMITX -> HDMI
# (the board's sound-card DT patch, lkml 2019-06; Armbian forum 27722). This:
#   1. dumps the card, its PCMs, every mixer control and device 0's hw params,
#   2. sets that route (only switches on the ODROIDN2 card -- the FLX10 is a
#      different card and rb keeps running),
#   3. plays a 2 s tone on HDMI at 44.1 kHz, then 48 kHz, and asks whether you
#      heard each from the monitor.
# Everything goes to ~/hdmi-audio.txt; paste that back.
#   sh scripts/odroid/hdmi-audio-probe.sh
set -u
OUT=~/hdmi-audio.txt
: > "$OUT"
log() { echo "$*" | tee -a "$OUT"; }
run() { echo "\$ $*" >> "$OUT"; "$@" >> "$OUT" 2>&1; echo >> "$OUT"; }

CARD=$(awk '/ODROIDN2|ODROID-N2|G12A/ {print $1; exit}' /proc/asound/cards)
[ -n "$CARD" ] || { log "no ODROIDN2 card in /proc/asound/cards:"; cat /proc/asound/cards | tee -a "$OUT"; exit 1; }
log "card index: $CARD"
run cat /proc/asound/cards
run aplay -l
run cat /proc/asound/pcm
run amixer -c "$CARD" controls
run amixer -c "$CARD" contents

# the route; a control this kernel does not have just logs an error
log "== setting the HDMI route"
for c in "FRDDR_A SRC 1 EN|on" "FRDDR_A SINK 1 SEL|OUT 1" "TDMOUT_B SRC SEL|IN 0" \
         "TOHDMITX I2S SRC|I2S B" "TOHDMITX|on"; do
  name=${c%|*}; val=${c#*|}
  run amixer -c "$CARD" sset "$name" "$val"
done
run amixer -c "$CARD" contents

DEV="hw:$CARD,0"
log "== hw params of $DEV"
run sh -c "aplay -D $DEV --dump-hw-params -d 1 /dev/zero 2>&1 | head -40"

ask() {
  printf '%s -- heard it from the monitor? [y/n]: ' "$1"
  read -r a
  log "$1 | heard=$a"
}
for r in 44100 48000; do
  for f in S16_LE S24_LE; do
    log "== tone $DEV ${r} Hz $f"
    echo "playing a tone on HDMI ($r Hz, $f)..."
    timeout 4 speaker-test -D "$DEV" -c 2 -r "$r" -F "$f" -t sine -f 440 -l 1 -p 100000 >> "$OUT" 2>&1
    ask "$r Hz $f"
  done
done
log ""
log "saved to $OUT"
