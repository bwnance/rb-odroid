# 21 — ODROID-N2+ (Armbian)

A second board for the same tree: the **Hardkernel ODROID-N2+** (Amlogic S922X,
4× Cortex-A73 + 2× Cortex-A53, 4 GB) on **Armbian**, with an HDMI monitor and a
**DDJ-FLX4**. Nothing here has run on the board yet. This document is the bring-up
plan and the record of what had to change. Each check below gets its result
written in once it has been run, the same way [13](13-raspberrypi4.md) records the Pi.

## Why it should work

[13](13-raspberrypi4.md) names the three expensive parts of the port as the
chroot recipe, the `rbp` patch table and the shims' `rbp` ABI. All three belong
to the *player*, not the board. What the board has to provide is the same short
list it was on the Pi:

| Needs | Pi 4B (measured) | N2+ (expected) |
|---|---|---|
| a kernel that runs 32-bit ARM ELF (`CONFIG_COMPAT`) | 64-bit `-v8` kernel, "32-bit EL0" | A73/A53 support AArch32 at EL0; Armbian's arm64 kernels build `CONFIG_COMPAT` |
| `/dev/fb0` from DRM fbdev emulation | `vc4drmfb` | `meson-drm`'s fbdev emulation |
| a 1280×800 mode on HDMI | `video=HDMI-A-1:1280x800@60` accepted | **unknown**: the meson HDMI clock tables may refuse a non-CEA mode |
| ALSA seq + the FLX4's USB audio | yes | board-independent |
| evdev pointer | yes | board-independent |

## The image

Armbian for the **ODROID N2 / N2+** (not the N2L), **Debian 13 trixie Minimal**,
**current** kernel (6.18.x at the time of writing; measured on 6.18.43). Flash it to microSD or eMMC,
set the boot switch to **MMC**, and finish Armbian's first-login setup.

Install what the deploy needs, as on the Pi:

```sh
apt-get install -y gcc make python3 alsa-utils
```

## What changed in the tree

Everything is selected by `RB_BOARD`, which `rb.conf` reads from
`/proc/device-tree/model` (`*ODROID-N2*` → `odroid-n2`; anything else → `rpi`,
the old behavior). To override it, set it in the environment, not in rb.local.conf.
rb.local.conf is sourced after the board defaults have already been chosen.

| Area | Pi | ODROID-N2 | Where |
|---|---|---|---|
| kernel arguments | the one line of `/boot/firmware/cmdline.txt` | the `extraargs=` line of `/boot/armbianEnv.txt`, every other line kept byte for byte | `boot-trim.sh` (`RB_BOOT_DIR`, `RB_BOOT_FORMAT`) |
| `config.txt` directives | `boot_delay=0`, `disable_splash=1` | none. Armbian has no `config.txt` | `boot-trim.sh` |
| `video=HDMI-A-2` status note | reported | not reported (one HDMI port) | `boot-trim.sh status` |
| DRM card for overlay planes and VNC | was hardcoded `card1` | the first card with connectors in `/sys/class/drm`; `RB_DRM_CARD` overrides | `shims/drm_card.h` (copy in `vncserve/`), `drmband.c`, `vnc_capture.c` |
| HDMI audio mirror | on, `vc4hdmi0/1` as IEC958 subframes | **off** until the board's HDMI card is measured | `rb.conf` `RB_AUDIO_MIRROR_DEV` |
| health log `thr` column | `vcgencmd get_throttled` | `t<NN>C`, the SoC temperature | `healthwatch.sh` |
| install/doctor hints | Pi OS, `vc4-kms-v3d` | Armbian, `CONFIG_COMPAT`, meson-drm | `install.sh`, `doctor.sh` |

The DRM card lookup reads sysfs and does not open nodes to probe them. The first
open of a primary node is granted DRM master, and a probe loop that took master
could take the display away from whatever needs it.

## Firmware assets

The same as the Pi: an extracted **XDJ-RX3 v1.20** tree (see
[04](04-firmware-assets.md)). The version matters. The patch table and every ABI
address in the shims are for the v1.20 `rbp`. The rx3-pi project's recovery script
fetches **v1.19**, so its output cannot be used here as-is.

## Building on the board

The N2+ is fast enough to be its own build machine, and its arm64 kernel runs
the armel build outputs natively. `scripts/odroid/build-on-odroid.sh` does the
whole build there, as root from the repo root, with `extracted/` in place:

```sh
fsck.cramfs --extract=extracted/XDJRX3-rootfs extracted/XDJRX3/images/rootfs.cramfs
sh scripts/odroid/build-on-odroid.sh         # -> work/rb-odroid.tgz
sh scripts/device/install.sh work/rb-odroid.tgz
```

It builds inside a **Debian bookworm** chroot (`/srv/rb-bookworm`, made with
debootstrap the first time), not the board's trixie. Bookworm is the toolchain
every recipe here was measured with (gcc 12, glibc 2.36 headers, the same base as
`tools/build-toolchain/Dockerfile`). Trixie's gcc 14 rejects DirectFB 1.4's
implicit declarations, and glibc 2.38+ headers redirect `strtol` and friends to
`__isoc23_*` symbols that glibc 2.13 lacks. That kind of failure builds cleanly
and then fails to load. The DirectFB steps and checks are
`tools/build-directfb/README.md`'s, unchanged.

## Bring-up checks (O0–O7)

Run in order. Each one rules out a whole class of later failure.

| # | Check | Pass looks like | If it fails |
|---|---|---|---|
| O0 | `lscpu \| grep -i op-mode` | `32-bit, 64-bit` | the kernel lacks `CONFIG_COMPAT`. Use a different Armbian kernel branch |
| O1 | `cat /proc/device-tree/model` | contains `ODROID-N2` | set `RB_BOARD=odroid-n2` in the environment |
| O2 | `cat /sys/class/graphics/fb0/name`, `virtual_size`, `bits_per_pixel` | a `meson` fb device exists | `CONFIG_DRM_FBDEV_EMULATION`; check `dmesg \| grep -i fb0` |
| O3 | `cat /sys/class/drm/card*-HDMI-A-1/modes` | the monitor's mode list | try another cable or port; the connector must be detected |
| O4 ✅ | `sh scripts/device/boot-trim.sh apply bootfiles` as root, reboot, then O2 again | fb0 is **1280×800** | **the main risk.** If meson refuses the mode, fall back to a CEA mode (1280×720 or 1920×1080) and let the DirectFB present path scale, or reuse rx3-pi's DRM presenter. Record what the mode list offered |
| O5 | fb0 `bits_per_pixel` | 16 is the Pi's zero-copy case; 32 also works through the present path | `video=HDMI-A-1:1280x800-16@60` may ask fbdev emulation for 16 bpp (unverified on meson) |
| O6 | `install.sh` | "chroot executes: ok" | O0 again |
| O7 | `start-rb.sh`, then the Pi's S4–S6 rows in [13](13-raspberrypi4.md) | UI on the monitor, FLX4 audio, controls, a USB stick | per row, as on the Pi |

After O7, the HDMI audio mirror can be measured: `aplay -l` for the card name,
then `tools/pcmprobe` for the formats it takes, and an `RB_AUDIO_MIRROR_DEV=` line
in `rb.local.conf`. Do this only after a listening check, as [09](09-audio.md#the-hdmi-mirror) warns.

## Measured so far

On the unit, Armbian `6.18.43-current-meson64`, with a 1024×768 bench monitor
(the target monitor is 1280×800):

* **O0 pass.** `lscpu`: `CPU op-mode(s): 32-bit, 64-bit`; Cortex-A53 + A73.
* **O2.** `/dev/fb0` is `mesondrmfb`, 1024×768, **32 bpp** (the monitor's own
  mode; fbdev emulation picked its preferred depth, not 16).
* **O3.** The display is **card0** (`card0-HDMI-A-1: connected`, modes
  1024x768, 800x600…); card1 is the render-only GPU. This is the reverse of
  the Pi, where the display is card1, and it is why `drm_card.h` finds the card
  instead of hardcoding it.
* **O1 pass.** Device tree model `Hardkernel ODROID-N2Plus` (matches `*ODROID-N2*`).
* **O4 + O5 pass.** With `extraargs=video=HDMI-A-1:1280x800-16@60e` in
  armbianEnv.txt, fb0 came up **1280×800 at 16 bpp**, the same surface the Pi
  presents with no blit. The 1024×768 bench monitor, whose EDID lists nothing
  above 1024×768, displayed the forced mode. **But the tree's default player
  renders at 32 bpp** (`RB_FB_LIE_BPP=32`, the rbp `99040424…` variant), which on
  the Pi's 16 bpp panel costs a full-frame 32→16 convert (rb.conf measured ~18 %
  fewer frames and ~2× rbp CPU). meson-drm can give a 32 bpp fb, so
  `boot-trim.sh` writes **`video=HDMI-A-1:1280x800-32@60`** on Armbian so the
  player's surface is the panel's own format. That is the expected zero-copy
  case here, still to be confirmed with the `PRESENT:` log line. The trailing
  `e` is dropped because the Pi never needed it. If a monitor is ever not
  detected, `e` is the thing to add back.
* **Firmware.** `XDJ-RX3_v120.zip` (sha256 `e81f34ef…`) decrypted with the
  source-package key to the `UsbAuto` ISO (`images/release.txt`: 1.20). Stock
  `rbp` `4f2efcfc…`, `rbp-audio` `3dda2d4e…` and `rbp-nopc` `97aa2223…` are
  identical to the Pi's (13, S0.2), so the patch table applies unchanged.
* **O6/O7 (first boot).** Built on the board with `build-on-odroid.sh`, installed
  with `install.sh` (boot trim applied). After the reboot fb0 is **1280×800 at
  32 bpp**, and `rblive4.service` is active with `edb_streamd`, `rbp -a`,
  `usb-watch` and `display-watch` running (~500 MB RSS). The `PRESENT:` line is
  in `/tmp/dfbdig9.log`, not `rbp.log`.
* **Display: zero-copy confirmed.** `PRESENT: mode=off angle=0 real_fb=1280x800
  pitch=5120 bpp=32 pages=1 pan=0 (logical 1280x800)`. rbp's 32 bpp layer *is*
  the fb page, with no convert pass. This is better than the Pi, whose 16 bpp
  panel needs a 32→16 convert every frame.
  The rekordbox UI is on the monitor (confirmed by eye, 2026-10-24).
* With no FLX4 attached, `rbp.log` repeats `ALSA lib pcm_hw.c:1401:(_snd_pcm_hw_open)
  Invalid value for card`. That is audioshim retrying `hw:CARD=DDJFLX4,DEV=0` (the
  controller table's card), and it is expected until the controller is plugged in.
* **Audio.** The board's own ALSA card is `ODROIDN2` (`axg-sound-card`), which is
  the candidate for the HDMI mirror later. `/dev/snd/seq` is present without a
  modprobe.

## Open questions

* Whether meson's HDMI PHY will clock 1280×800@60 (71 MHz, CVT-RB). O4 answers it.
* fb0's default depth under meson-drm. O5 answers it.
* Which `cardN` meson-drm gets when panfrost is loaded. `drm_card.h` makes this a
  non-question, but it is worth recording.
* The board's HDMI audio card name and formats, for the mirror.

## To revisit (operator's list)

*Fixed 2026-10-25:* **mount table explosion.** `fix-dev.sh` bound `/dev` over the
chroot's mounted `/dev` on every redeploy, and with Debian's shared propagation each
stacked bind also landed on the host's `/dev` -- the table roughly doubled per
redeploy (slow restarts, `mountpoint` eating CPU, `/dev/pts` covered so sudo had no
pty). Now a mounted chroot `/dev` is left alone and every chroot bind is private.
Check: `wc -l /proc/self/mountinfo` should stay small across redeploys.

*Fixed 2026-10-25:* **headphone zap on restart.** The startup mute covered only the
master pair; the headphone/booth pairs and the external-mode deck taps now get the
same `startup_gain()` (default 1.5 s mute + 0.3 s fade). Confirmed on the unit.

*Fixed 2026-10-25:* **ON AIR icons and hot cue state at start-up.** The FLX10 now gets
rekordbox's position query at start-up (`F0 00 40 05 00 00 04 01 00 03 01 F7`,
captured with MIDI Monitor, `tools/captures/flx10-rekordbox-startup.txt`) and its
keepalive every 200 ms, so the faders start where they really are rather than
seeded at unity. Confirmed on the unit; the odd hot cue start-up state went with it.

* **HDMI audio mirror** looks for the Pi's `vc4hdmi0/1` cards, which the Odroid
  does not have (harmless retry noise in audioshim.log). Point `RB_AUDIO_MIRROR_DEV`
  at the Odroid's HDMI card.
* **HOT CUE mode switch lag** (parked, low priority). Switching to HOT CUE shows on
  screen a fraction of a second late, even with no track loaded. Not the key
  path: `onKey_HotCue` @0x3030ec switches the mode on the press, and SYNC (the
  only other key with HOT CUE's 1 s long-press time) reacts instantly. Likely
  the hot cue grid's redraw. `scripts/odroid/modelag.py` times it if revisited.
* **Booth and master outputs**, by ear, once on the audio system.
* **Jog wheel displays.** Phase 1 done (vendor unlock + MIDI ch 16: BPM, tempo %,
  time remaining, marker, MT, MASTER, SYNC). Phase 1 is complete (key included).
  Left: the HID phase (tempo range box, waveform, clock arm and cue
  marker, two-decimal BPM, artwork) -- see "FLX10 jog displays" above.
* **TEMPO RESET** (operator, 2026-10-25): the FLX10's TEMPO RESET button (deck note
  65, unbound today, its LED dark) should reset the deck's tempo to 0% the way
  rekordbox does -- find whether rbp has a reset of its own, else hold the tempo at
  centre until the fader is moved.
* **MASTER TEMPO on KEY SYNC long press** (operator, 2026-10-25): today a short
  KEY SYNC press toggles master tempo; move that to the long press (the unit sends
  note 26 for it, unbound today) so the short press can be KEY SYNC.
* **KEY SYNC** (operator, 2026-10-25): the short press should do key sync -- find
  whether rbp has a key sync of its own (the RX3 has none on its panel), and what its
  KEY SYNC LED should follow.
* **SLIP REVERSE / REVERSE** (operator, 2026-10-25): today both the button (note 21)
  and SHIFT + it (note 56) send rbp's REV key, which toggles. Wanted, as labelled:
  - SLIP REVERSE (no SHIFT): momentary, and always a SLIP reverse -- reverse while
    held, and on release the playhead returns to where it would have been, whether
    or not slip mode is on.
  - REVERSE (SHIFT): a latching plain reverse, on until pressed again, and NEVER
    slipping -- even with slip mode on, the playhead does not jump back.
  - LED (note 21, measured): lit while the button is held and while reverse is on.
* **Waveform jittering** (operator, 2026-10-25): on the HDMI screen, during playback;
  slight but visible. Suspects: rbp drawing into a single-buffered fb0 with no vsync
  (tearing), uneven frame pacing (rbp runs without its SCHED_FIFO request,
  RB_SCHED_RT=1), the fbshim depth conversion. To characterise first: stutter in the
  scroll (uneven steps) vs. tearing (a horizontal split) vs. shimmer.
* **Later, maybe:** RX3 effects matching the FLX10's (ENIGMA JET, MOBIUS, ...).

## DDJ-FLX10

The surface this fork is ultimately for. No map yet; facts measured on the N2+:

| | DDJ-FLX10 | (DDJ-FLX4, for comparison) |
|---|---|---|
| USB id | `2b73:0041` (AlphaTheta Corporation DDJ-FLX10) | `2b73:0045` |
| ALSA card id | `DDJFLX10` | `DDJFLX4` |
| sequencer port | `DDJ-FLX10 MIDI 1` (client 20) | `DDJ-FLX4 MIDI 1` |
| playback | 4 ch, `S24_3LE`, 44100 only, async, high speed; channel map FL FR FC LFE | 4 ch, `S24_3LE`, 44100 |

The playback format is the FLX4's exactly, so audioshim's `hw:` path and
`S24_3LE` packing carry over. **The headphone pair is reversed:**
`speaker-test -c 4` put channel 2 (FC) in the RIGHT ear and channel 3 (LFE) in the
LEFT, so the FLX10 wants `headphones=3,2` where the FLX4 default is `2,3`
(audioshim's pair is `left,right`, any order). Master on 0,1 is still to be heard.
Until the FLX10 has its own row, the per-device memory carries it, keyed by USB id:
`RB_AUDIO_PAIRS_MEMORY='2b73:0041=headphones=3,2'` in rb.local.conf.

No published MIDI message list for the FLX10 was found, so the bindings are
recorded from the hardware with `scripts/odroid/midi-learn.py`. It is a guided
prompt per control: stop the player, run it, and it writes `flx10-learn.json`.
The map (`map_flx10.c`) and the controller-table row are written from that file.
The RX3 is a 2-deck player, so FLX10 decks 1 and 2 (and channel strips 1 and 2)
drive it. Decks 3/4 have nothing to control.

### FLX10 map (`scripts/shims/map_flx10.c`)

Selected with `RB_MIDI_MAP=flx10` in `/opt/rblive4/rb.local.conf`. That also
points audio at the `DDJFLX10` card through the controller table's new `flx10`
row. The headphone pair needs the per-device memory line above. The bindings
are the operator's table, agreed control by control on 2026-10-24:

| FLX10 | RX3 |
|---|---|
| PLAY, CUE | PLAY, CUE |
| BEAT SYNC / SHIFT | SYNC / MASTER (tempo master); long press nothing |
| KEY SYNC | toggles MASTER TEMPO; hold and SHIFT nothing for now |
| TEMPO RESET / SHIFT | nothing / TEMPO RANGE |
| TEMPO fader, jog (top press = touch, top spin, side ring) | the same |
| 4 BEAT JUMP < > / SHIFT | SEARCH < > / TRACK SEARCH < > |
| SLIP REVERSE (and SHIFT) | REV |
| QUANTIZE; SLIP / SHIFT | QUANTIZE; SLIP / VINYL |
| CUE/LOOP CALL < > and SHIFT | not mapped |
| MIX POINT, LINK, ACTIVE PART, PAGE, DECK SELECT, SAMPLER, BOOTH | nothing |
| browse knobs (one shared message) / SHIFT | scroll / 8 steps per detent |
| browse push (per deck) / held 0.5 s / double | on a track row LOAD that deck, on anything else SELECT (opens the folder/playlist/category) -- the row type is rbp's own `UiBrowseComm_IsTrackList` (@0x105e60) / **rbp's own long press of the selector = FAST SCROLL** (selector PRESS + op 1, held until the knob comes up; TAG TRACK is unmapped) / twice; **on the SOURCE screen with a stick in slot 1: open USB 1** (rbp ignores the knob there) |
| BACK / VIEW; SHIFT+BACK / SHIFT+VIEW | BACK / SOURCE from the player, SOURCE again on the SOURCE menu (closes it), BROWSE (back to the player) from a browse list; TAG LIST / SHORTCUT |
| SHIFT + deck 1's browse push | TRACK FILTER (deck 2's SHIFT push not yet recorded) |
| pad modes HOT CUE / PAD FX1 / BEAT JUMP / SAMPLER | HOT CUE / SLIP LOOP / BEAT JUMP / BEAT LOOP 2 |
| SHIFT + those: KEYBOARD / PAD FX2 / BEAT LOOP / KEY SHIFT | GATE CUE / RELEASE FX / BEAT LOOP / BEAT JUMP 2 (a SHIFT button goes straight to its mode; pressing a main button again does what SHIFT + it does, and a third press comes back -- the FLX10's double-press behaviour) |
| pads; SHIFT + pad in HOT CUE | pads in rbp's mode; delete that cue |
| ch 1/2 TRIM, EQ, COLOR, FADER, CUE; master CUE, crossfader, HP mix/level | the same |
| MASTER LEVEL | the unit's own volume; feeds only the HDMI mirror (as on the FLX4) |
| SCFX SPACE, D.ECHO, CRUSH, NOISE, FILTER / PITCH | the same names / SWEEP. SPACE is 0x50a1 and CRUSH 0x50a5: rbp_abi.h had them swapped, corrected from the unit (2026-10-24) |
| SAMPLER VOL | SCFX PARAMETER (0x50a7, channels 1 and 2) |
| BEAT < > / SHIFT; LEVEL/DEPTH / SHIFT; ON/OFF | BEAT < > / TAP; DEPTH / TIME; Beat FX ON/OFF |
| effect selector | by name: ECHO, MT DELAY→DELAY, SPIRAL, REVERB, TRANS, FLANGER, PHASER, SLIP ROLL, ROLL; others nothing. Note 32 is fully counter-clockwise (confirmed). rbp's positions are its own order, not the RX3 knob's printing (docs/13 S5.9) |
| channel selector (SP MIC 3 1 2 4 MST = notes 22 21 18 16 17 19 20) | 1 → player 1, 2 → player 2, MIC → MIC, MST → MASTER; 3, 4, SP → nothing (rbp's target unchanged; crossfader A/B on 3/4 would contradict the deck lights). Sent only when the knob moves |

**Loops**, decided against rbp's `isLooping` at the moment of the press:

| | not in a loop | in a loop |
|---|---|---|
| IN · 1/2X | loop IN | halve (rbp's CALL <, 0x4323) |
| OUT · 2X | loop OUT | double (rbp's CALL >, 0x4322) |
| 4 BEAT / EXIT | 4-beat loop, immediately | exit |
| SHIFT + IN | loop IN | IN ADJUST |
| SHIFT + OUT | RELOOP | OUT ADJUST |

Both loop findings come from the player binary, read with `objdump`, and are
confirmed on the unit (2026-10-24):

- **CALL < / >.** `ui::Player::asEventCode` @0x2f16d4 maps key 0x4322 → event
  32 and 0x4323 → event 33. `PlayerSkeleton::onKeyEvent` sends those (PRESS only)
  to `PlayerInnards` vtable +0xf0 `onEv_CallNext` and +0xf4 `onEv_CallPrev`. Both
  call `DjEngineIF::setLoopCutterDouble` while looping. (0x4124 is event 34 =
  `onEv_DeleteCue`, and 0x4125 is event 31.)
- **4 BEAT.** `onKey_LoopIn` @0x303b18 starts the 4-beat auto loop on the key's
  REPEAT edge (op 1, branch at 0x303c98 → `setAutoBeatLoop(ch, {4,1}, 3, …)`).
  rbp's key manager only produces that edge after about a second of holding, so
  the map sends press + repeat + release together.

The rest of the FLX10's keycodes, found the same way (2026-10-24; still to be
confirmed on the unit):

- **SHIFT pad modes need no keycode.** A mode key pressed while rbp is already
  in that mode flips the mode's second bank. `onKey_HotCue` flips +0x79,
  `onKey_AutoBeatLoop` +0x7a, `onKey_SlipBeatLoop` +0x7b and
  `onKey_BeatJumpLoopMove` +0x7c, and entering a mode from another clears it.
  `PlayerInnards::getStat` publishes the pad-mode byte that `rbp_led.c` reads as
  **0 HOT CUE, 1 BEAT LOOP, 2 SLIP LOOP, 3 BEAT JUMP, 4 GATE CUE, 5 BEAT LOOP 2,
  6 RELEASE FX, 7 BEAT JUMP 2**. That is the "4" the FLX4 notes recorded. The map
  reads it and sends 0, 1 or 2 presses of the mode's key to reach the mode the
  button names.
- **SCFX PARAMETER = 0x50a7.** `ui::Mixer::asEventCode` → event 0x100e →
  `MixerChInnards` vtable +0x40 `onEv_ColorFxParameter(float)`, the slot after
  COLOR's (0x509d → 0x100b → +0x3c `onEv_ColorFxVolume`). It is per channel, so it
  is sent to 1 and 2.
- **Fast scroll = the selector held.** rbp's long press for 0x420c is 500 ms
  (`KeyInput::getLongPressTime` @0x37de18) and arrives as op 1 (`isLongEnter`
  @0x37e1cc); while scrolling rbp asks `isRotarySelectorPressed` (@0x37e028).
- **TAG TRACK = 0x420e** (unmapped since 2026-10-25). `BrowseUiIfDpl::onKey` → `InputKey(UKEY 16)`, and
  `InitUiBrowseKey` installs `UiKey_AddTag` (→ `TagTrack`) in slot 16. The same
  path takes BACK 0x420d to UKEY 17 `UiKey_Back`.
- (Booth: `MasterInnards::onEv_BoothLevel` exists; its key, likely 0x4404, is not
  traced. The FLX10's own booth output and knob are expected to cover it.)

`RB_FLX10_KEYS` still overrides `callprev`, `callnext` and `param`.
`gatecue`/`releasefx`/`beatjump2` are no longer keys and are ignored with a log
line.

**Jog (2026-10-24).** The platter is about **13083 counts/rev**: five hand-counted
turns gave 65416 net counts in 7103 messages, about 18× the FLX4's 720. Turns by
hand are good to about ±2%, and no design value is published. The map uses
**12960**: 360 × 36 (36 counts per degree) and exactly 18 × the FLX4's 720. It is
the one value in that range with that structure, 0.95% below the measurement
(about 5.05 turns counted as 5). A 10-turn count (≈129600 if right) would settle
it. It also reports in
bursts, several messages per millisecond. The FLX4's per-message speed therefore
saw 0 ms gaps and pinned nearly every update at the 8 rev/s clamp, so the jog
"moved at one speed whatever the hand did". `map_flx10.c` now accumulates counts
and sends at most one update per 4 ms, with speed = counts / real elapsed time on a
microsecond clock. rbp's position is fed at the FLX4's 720 per revolution.
`RB_JOG_PPR` overrides the count.

**Jog gain = 12960 / 5484 ≈ 2.363** (2026-10-25). At the true count one turn moved
the track about 2 beats; rekordbox on the same FLX10 moves 4. The operator tuned it
by ear against the grid (16 turns within a beat of 64 beats) with `RB_JOG_PPR`, and
the result is built in as a separate gain on speed and position alike, so the count
keeps meaning counts. `RB_JOG_GAIN` overrides it.

### FLX10 LEDs

Probed 2026-10-24 with `scripts/odroid/led-probe.py` (`flx10-leds.json`, gitignored).
Every LED answers on its own button's input note and channel, ON = 0x7F, OFF = 0.

| What | Note (channel) | Driven from |
| --- | --- | --- |
| PLAY / CUE / SYNC | 11 / 12 / 88 (deck) | the same transport decision the drawers show (CDJ / rekordbox CUE and PLAY, below) |
| KEY SYNC | 101 (deck) | MASTER TEMPO |
| SLIP | 64 (deck) | slip mode |
| QUANTIZE | 53 (deck) | rbp's QUANTIZE (per-deck struct +0x2e bit 6, `StatWatcher::getPlayQuantizeOn`) |
| IN / OUT / 4 BEAT | 16 / 17 / 20 (deck) | rekordbox style: IN solid, 4 flashes when a loop-in point is set; OUT solid; 4 BEAT/EXIT lit while any loop runs |
| channel CUE | 84 (deck) | headphone cue |
| mode buttons | 27, 109, 30, 32; banks 105, 34, 107, 111 | rbp's mode byte 0..7 |
| pads | page base + 0..7 on ch 8 / 10 | rbp's pad LEDs, palette-encoded |
| meters | CC 2 on ch 1 / 2 | as the FLX4 |

CUE and PLAY follow rekordbox (watched on the FLX10, 2026-10-24), for every
surface and the screen's drawers alike. The source is rbp's back-cue state,
`PlayEngine::isInCueStandby` (0x5e910) and `isCueSampling` (0x5e958):

| Deck | CUE | PLAY |
| --- | --- | --- |
| nothing loaded | off | off |
| paused away from the cue point | flashing | flashing |
| paused on the cue point | solid | flashing |
| CUE held (previewing) | solid | flashing |
| playing | solid | solid |

PLAY flashes 0.5 s on / 0.5 s off, CUE at twice that rate (0.25 / 0.25), on one clock.

The SHIFT mode notes light their parent button *flashing* (105 flashes HOT CUE, 109
BEAT JUMP, 107 PAD FX1, 111 SAMPLER) -- the unit's own way of showing a shifted
mode. BEAT LOOP 2 lights SAMPLER solid (34).

The pads only show the page the unit itself is on, which moves with the unit's mode
buttons whatever rbp does, so `map_flx10.c` tracks the page per deck and hands the
base to the bridge (`struct led_notes.pad_base`). A main mode button toggles the unit
between its page and its SHIFT page on repeated presses (0/64, 16/80, 32/96, 48/112 --
measured: BEAT JUMP twice put the pads on 96), a SHIFT button goes straight to its
page, and any pad hit resets the page to the one that pad's note is on. SAMPLER's page (48) is
BEAT LOOP 2; before any mode button the page is guessed from rbp's mode.

### FLX10 jog displays (phase 1: the MIDI display channel)

Pioneer's MIDI list (`DDJ-FLX10_MIDI_Message_List_E1.pdf`, "Illumination Control")
documents a display channel, **MIDI ch 16**, whose fields the unit renders itself
(no HID, no session; also what the community Mixxx mapping uses). d = deck - 1:

| Field | Message | Encoding |
| --- | --- | --- |
| BPM (two decimals) | SysEx `F0 00 40 05 00 00 04 01 00 <11+d> n5..n0 F7` | hex digits of BPM x 100, high first (82.00 = `00 00 02 00 00 08`); `0F` x6 blanks. What rekordbox sends (captured 2026-10-25, decoded from a tempo-range sweep). **Ignored outside rekordbox's HID session** -- sent by hand alone and after its `00 0A`/`0B`/`0C` setup messages, no change; not sent (HID phase) |
| BPM (one decimal) | CC 0x14+d / 0x34+d | BPM x 10, 14-bit (Pioneer's list); rekordbox never sends it; **what we send** |
| playing speed | CC 0x18+d / 0x38+d | (percent + 100) x 10, 14-bit |
| time | CC 0x42+2d min, 0x43+2d sec | 0..99 |
| time mode | Note 0x14+d | 0x7F remaining |
| marker | CC 0x10+d / 0x30+d | 0..359 deg -- **ignored by this firmware** |
| cue marker | CC 0x1C+d / 0x3C+d | 0..359; 0x7F/0x7F hides -- **ignored** |
| MASTER / SYNC | Note 0x18+d / 0x1C+d | on/off |
| master tempo | Note 0x20+d | on/off |
| key | CC 0x49 on the deck channel | 0 none; 1..24 = the major keys chromatically from C, each followed by its relative minor (1 8B, 2 8A, 3 3B, ... 23 1B, 24 1A; read off the unit) |
| key change | CC 0x4A on the deck channel | 0x0D = 0 |
| key display format | Note 0x5B on the deck channel (the list prints 9F; only the deck channel works) | 0x00 classic, 0x7F Camelot; follows rbp's notation |
| show info | Note 0x5D+d | 0x00 show, 0x7F hide (shows the rekordbox logo) |

**The vendor unlock.** Until it gets seven vendor control requests (bmRequestType
0x40, bRequest 3, wLength 0; (wValue, wIndex) = (0100,C028) (0000,C029) (0200,C013)
(0000,C02B) (0100,C026) (0000,C01D) (0100,C027), from the community USB captures) the
FLX10's jogs say NO AUDIO DRIVER and ignore ch 16. `scripts/device/flx10-unlock.py`
sends them (stdlib, USBDEVFS_CONTROL, no interface claim) and `start-rb.sh` runs it on
every start before rbp opens the card; `--rebind` also rebinds snd-usb-audio.

`rbp_led.c` reads the deck values each tick (`PE_GETTOTALTIME`, `PE_GETPLAYINGTIME`
-- ms --, `PE_GETQBPMDISPLAY`, `PE_GETTEMPOX100`, `PE_GETTEMPORANGE`, MT / SYNC /
MASTER) and hands them to the map's `deck_display` hook; `map_flx10.c` sends them.
Neither marker is sent: both were sent by hand with `aseqsend` on 2026-10-25 and
nothing moved, though rekordbox shows a rotating clock arm -- it comes from the HID
state packet. BPM is rbp's own screen value (StatWatcher getPlayBpm, x100) sent x10 and
TRUNCATED like rbp's screen; tried x100 on 2026-10-25 -- the firmware rounds it to tenths
(105.69 -> 105.70), so a true second decimal needs the HID path. `JOG_DISPLAY_VERBOSE=1` logs the raw
readings once a second (the BPM x100 and tempo x100 units are to be confirmed against
the screen). Key: rbp's deck panel keeps a copy of the key text it draws at `0x02683fc0 + deck *
0x200` (UTF-16, `ui_Deck_Update`); `flx10_key_code()` turns Camelot or classic text into
the code. Not yet: the HID-only parts -- tempo range box, waveform, artwork
(interface 5; the community notes in Veezuhz/Mixxx_FLX10_Controller_Mapping never
got those rendering on Linux).

Pad brightness (`LED_PAD_DIM_BRIGHT`, the operator against rekordbox): HOT CUE and
GATE CUE show a set cue bright in its colour and flash white for 0.12 s on a press;
every other mode shows the dim colour, and the bright one solid while the pad is
held or rbp marks it engaged (the running loop). The map tells the bridge which
pads are held (`struct led_notes.pad_touch`).

Pad palette (velocity): 4 blue, 8 light blue, 12 teal, 16 greener teal, 20 green,
24 yellow-green, 28 yellow, 32 orange, 36 darker orange, 40 even darker orange, 44
pink, 48 light pink, 52 purpler light pink, 56 darker purple pink, 60 darker purple,
64 white; +64 is the dim version; **0 is the unit's own dim purple, not off**. rbp's
RGB goes to the nearest hue, low saturation to white (`led_flx10_palette()` in
`led_table.c`, pinned by `test_led_table`).

No LED answered for: SHIFT, BACK, VIEW, BEAT < / >, CALL < / >, FX ON / assign,
Sound Color FX, master CUE, master meter (the FLX10 drives its master meter itself, from its own audio -- confirmed working 2026-10-25). Left dark on purpose: stems/PART, PAGE,
LINK, TEMPO RESET, SLIP REVERSE, sampler CUE, channels 3/4.

**Still to check on the unit:** how the jog feels
after the fix; LEDs against the tables above; the loop-in flash (0.3 s off / 0.3 s on, four times) against rekordbox's. Also,
once on the audio system: master and booth output by ear.

**Later, maybe:** RX3 effects matching the FLX10's (ENIGMA JET, MOBIUS, ...).

`test_flx10.c` pins the table: 99 checks, built and run natively on the host
(`cc ... test_flx10.c map_flx10.c ctrl_map.c shimutil.c shmstate.c pitch_state.c`).
`make test_flx10` builds it like the other shim tests.
