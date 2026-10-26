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

**Open, as of the end of 2026-10-25** (the start-here list for the next
session; details further down):

1. **HDMI audio mirror**: run `scripts/odroid/hdmi-audio-probe.sh`, read
   `~/hdmi-audio.txt`, then set the mixer route in start-rb.sh and
   `RB_AUDIO_MIRROR_DEV=hw:CARD=ODROIDN2,DEV=0` with a format that works.
2. **Waveforms on the jog screens**: the unit caches waveform data it is sent
   (rekordbox's showed up after a capture session until a power cycle). Decode
   rekordbox's waveform HID packets from the captures in ~/Downloads on the
   Mac (capture, capture_2, capture_jog; tools/flx10-pcap-decode.py --dump
   <type>), then feed rbp's own waveform per loaded track.
3. **Album art on the jog screens**: same route, image data (rbp has
   sendJogArtworkData / UiSetJogArtwork for the RX3's own jog screens).
4. **Splash until rbp is ready**, input held until then (item below). Needs
   the operator's description of the start-up glitches first.
5. **Booth and master outputs**, by ear.
6. Parked: HOT CUE mode switch lag; RX3 effects matching the FLX10's.

Done and confirmed 2026-10-25 (slip and pads):
- SLIP REVERSE with quantize ON ends on release and puts slip off after. The
  release clears rbp's asked-for reverse (Player+0xe9b) at once while the
  deck reverses on to the beat (+0xe9c, rbp_abi.h); slip went off in between,
  which ends the slip and leaves the reverse running. The map now waits for
  both (map_flx10.c, flx10_reversing).
- rbp's slip flash (the SLIP key puts all eight pad entries in state 3 for
  ~0.5 s) no longer lights the pads: empty hot cue pads went white, other
  modes' pads bright. The bridge leaves the pads alone while all eight are at
  3 (rbp_led.c). The previous fix (UiGetHotCueINtime) never ran on the FLX10:
  its dim/bright pad style returns first.
- No stale lights on the first visit to a pad page after a restart: the
  connect sequence sends every pad page dark but the one shown, as rekordbox
  does (map_flx10.c).
- Pads dark during SLIP REVERSE (some BEAT LOOP / BEAT JUMP pads, varying) are
  rbp's own: it marks them unusable while the deck slip-reverses, and they do
  nothing when pressed. Left as is.
- The deck badge no longer retries a refused overlay plane on every playing
  tick: on the ODROID that logged two lines per tick and filled the 1.9 GB
  /tmp (pointsrc.log), which failed the next install.

Done and confirmed 2026-10-25 (HID jog screens): arm tracks the platter (top
and side ring, ten turns land back on 12:00), smooth while playing, paused,
held, jogged, bent and reversed; MASTER, SYNC, key + shift, BPM decimals,
tempo/range, loop size 1/128..512, cue marker, remaining/elapsed (follows
rbp's time mode; SHIFT + 4 BEAT flips it), white edges = ON AIR, SLIP icon
(empty decks too), track-end warning on the jog ring (rbp's own, slow at 30 s,
fast at 15, not while paused); synced non-master deck shows its playing tempo
and holds % / key while jogged; BEAT SYNC range fit (only widens); BEAT SYNC
button's off-beat blink slowed. The centre BEAT SYNC text: nothing lights it
(full scan) -- dropped.

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
  at the Odroid's HDMI card (open item 2 above).
* **HOT CUE mode switch lag** (parked, low priority). Switching to HOT CUE shows on
  screen a fraction of a second late, even with no track loaded. Not the key
  path: `onKey_HotCue` @0x3030ec switches the mode on the press, and SYNC (the
  only other key with HOT CUE's 1 s long-press time) reacts instantly. Likely
  the hot cue grid's redraw. `scripts/odroid/modelag.py` times it if revisited.
* **Booth and master outputs**, by ear, once on the audio system.
* **Jog wheel displays.** Phase 1 (MIDI ch 16) done; under HID the unit draws
  none of it. The HID phase (flx10_hid.c) is done but for waveform and
  artwork (open items 3-4).
* **Controls, done and confirmed on the unit 2026-10-25:** TEMPO RESET toggle with
  its LED; MASTER TEMPO on the KEY SYNC hold (KEY SYNC itself does nothing: rbp
  has no key sync or key shift); SLIP REVERSE momentary + always slipping and
  SHIFT = latching REVERSE that never slips, LED on note 21; 4 BEAT JUMP < > =
  real -4/+4 beat jumps and SHIFT = -16/+16 beats (the old SEARCH keys only
  stepped a frame on a paused deck); SHIFT + CUE = back to the track's start;
  MIX POINT < > = previous / next track; SHIFT + BEAT < = Beat FX BPM AUTO;
  SHIFT + BACK on the player = PLAYLIST; VINYL disabled (no light -- showing
  the mode on the jog screen is for later). Follow-ups, also confirmed: beat
  jumps through the plain loop object (instant, repeatable; SHIFT = 16 beats);
  SHIFT + CUE / MIX POINT < seek straight to the auto cue point (MIX POINT <
  at the start = previous track); TRACK SEARCH taps that never repeat; KEY
  SYNC light dark; SHIFT + BACK toggles playlist / player; SLIP REVERSE with
  quantize on; crossfader assign switches; loop cache refreshed before loop
  keys (fresh-track IN / OUT loops); empty-deck range box and tempo %. See
  the table above.
* **Track-end warning on the jog ring**: done -- see "Track-end warning" below.
* **Waveform smoothness** (parked, good enough 2026-10-25). Fixed the main cause: rbp
  paced on FBIO_WAITFORVSYNC, which meson's drm_fbdev answers ENOTTY, so its own
  limiter ran it at 57.7 fps against 60 Hz (~2 repeated refreshes a second). fb_shim
  now does a real DRM vblank wait there and skips the render thread's limiter sleeps
  (RB_FB_VBLANK, default 1). Measured after (FB_VSYNC_RATE `frame:` lines): 60-61 fps,
  draw 13.3-13.6 ms avg, our per-frame work <0.05 ms, ~2 frames in 10 s miss when
  rbp's own draw spikes to 17-20 ms. Left for later: 1280x800@57 (17.5 ms budget,
  absorbs most spikes; `video=...@57` in armbianEnv.txt, if the meson clock and the
  monitor take it), and double-buffering if tearing (rbp draws the band in two
  passes ~2 ms apart into the scanned-out buffer) is ever visible.
* **Clock arm** (solved 2026-10-25): it is the unit's own per-deck setting,
  toggled ON THE FLX10 with a double tap of SHIFT -- deck 1's was simply off.
  The unit reports its display settings (ch 16 9F 3C..4E / BF 41..4A) when asked
  with 9F 70 00; map_flx10.c now sends 9F 70 00 first, records the answer and
  sends those values back in the rest of the connect sequence 300 ms later, so a
  reconnect keeps what was set on the unit (a change on the unit is logged as
  "FLX10: unit setting ..."). Retry the two-decimal BPM SysEx now that the HID
  session is up.
* **Jogs say Not Loaded** (solved 2026-10-25): since the reconnect change the
  connect sequence takes properly and the jogs switch to rekordbox's HID-fed
  screens (the unit's display-format button starts working); they then draw
  nothing until the host sends an "xx 21" state packet per deck on hidraw at
  ~50 Hz. `scripts/odroid/hid-loaded.py` proved it: rekordbox's loaded packet
  brings BPM/time/arm up (with `--playing` the mini waveform's position marker
  goes solid white), VirtualDJ's does not. `shims/flx10_hid.c` now sends the
  heartbeat + one packet per deck every 20 ms from its own thread, from the deck
  values rbp_led.c reads; `FLX10_HID=0` turns it off. The MIDI display values
  still go out alongside it.
* **xx 21 fields** (decoded with `scripts/odroid/hid-decode.py`, rounds 1-2):
  [2] 00 playing / 02 paused; [4] 0x04 MT, 0x70 range box (0x10 +-6, 0x20 +-10,
  0x30 +-16, 0x40 WIDE, 0x00/0x60 none; 0x80 moved the blue tone arm);
  [9] b4, 00 = Not Loaded;
  [11..12] elapsed s BE, [13] fraction /256 (moves the arm); [16] key (only 00 =
  blank found -- sent blank); [21..22] BPM whole number LE; [23..24] tempo % x100
  signed LE; [3] 0a shows remaining time (-02:38 with elapsed 0 -- from what
  total, not found; 08 hid the white edge arcs). Round 3: under HID the unit
  draws NONE of the MIDI ch-16 display messages (key, key format, BPM, speed,
  time, time mode, marker, MASTER, SYNC, MT, show info -- each sent every
  100 ms, no change), so everything on the screen has to come over HID. Byte 16
  is not the key code: after a reload any value leaves the key blank, so the
  key likely comes in another packet sent at load. Not found yet: key, BPM
  decimals (every BPM shows .03 from some captured byte; [18], captured 03,
  changed nothing when filled), MASTER/SYNC, arm position, track length -- a rekordbox HID capture
  with a track loaded is the way to get them.
* **xx 21 decoded from a rekordbox capture** (2026-10-25,
  `tools/captures/flx10-rekordbox-hid-notes.txt`): position and track length
  (min / sec / ms), BPM with tenths ([22] high nibble) and hundredths ([49]),
  key ([60] = 0x80 | key code) and key shift ([61], MT off), cue point ms
  ([31..33] and [55..57]), SYNC ([5] 0x10), loop in/out ms ([25..30]).
  flx10_hid.c now builds the packet from that layout; the rounds-1-3 guesses
  above are superseded where they differ. Loop in/out ([25..30], from
  PlayEngine::getCurrentLoopTime) and looping ([9] bc, [10] 0e) are sent; the
  screens go out at 100 Hz with the position carried forward between rbp's
  20 Hz readings (the arm stepped). A second capture gave MASTER ([5] 0x80),
  the loop-size code ([10], computed from the loop's length and the track's
  BPM) and jog touch ([3] 0x10); [3] 0x02 shows remaining time and follows
  rbp's own time mode (UiGetKeyTimeMode). A loop cut short by the track's end
  (512) and 1/128 fall back to rbp's own beat count (UiGetPlayLoopBeatNumer/
  Denom); 1/128 is sent as 0x03, a guess. Since 2026-10-25 the sender reads
  rbp's playhead itself every 10 ms (flx10_pos_now): the arm follows the jog
  (no running ahead while it is turned) and holds still on a pause instead of
  stepping back.
* **HID jog screens, to do** (operator, 2026-10-25) -- the capture
  (`tools/flx10-pcap-decode.py`) should show most of these. (Empty decks now
  carry the range box and tempo % too -- byte 4 and 23..24 of the empty packet.)
  - SYNC icon lit when the deck is synced (done). The centre BEAT SYNC text:
    no bit lights it in the full scan -- dropped.
  - White edges = rbp's ON AIR icons per deck: done ([3] 0x02, default of
    `RB_FLX10_ONAIR`).
  - BEAT SYNC **button** flashing when sync is lost. rbp has the two states
    the RX3's BEAT SYNC LED uses: DjEngineIF::isOutOfBeatSync(ch) @0x4b910
    (sync on but off the beat, e.g. after a jog nudge; FALSE / STAGE_1 /
    STAGE_2 in SyncInfo) and DjEngineIF::isOutOfRange(ch) @0x4b860 (the
    master's tempo beyond the deck's range; counted only while the sync
    master is valid). ui::PlayerInnards::getStat @0x301848 puts them in
    tPlayerStat byte 47 bits 3 and 4. Done differently: rbp's own SYNC LED
    already blinks off-beat (LEDSTAT_SYNC state 2); the blink is slowed to
    twice rbp's period (`RB_SYNC_BLINK_MS`).
  - BPM decimals, key: done (rekordbox capture).
  - Album art; waveforms: open items 3-4.
  - Arm: done (1.8 s a turn, cue arm at the cue point).
* **Finding a jog-screen byte with hid-loaded.py** (ON AIR edges, the
  centre BEAT SYNC icon). Set `RB_FLX10_HID=0` in rb.local.conf and restart rb
  (or the two senders fight), load a track on deck 1, then
  `python3 scripts/odroid/hid-loaded.py` and type edits, watching deck 1's
  screen: `3=08` (the base), then `3=0c`, `3=0a`, `3=06`, `3=04`, `3=00`,
  `3=18`; for other bytes try one bit at a time in bytes that are 00 in the
  base packet (`show` prints it), e.g. `5=03`, `5=05`, `5=09`, `6=03`,
  `8=81`, `46=01`, `47=01`, `48=01`, `50=01`..`54=01`, `59=01`, `62=01`.
  `clear` undoes. A byte:mask that lights only the edges goes in
  `RB_FLX10_ONAIR` (then `RB_FLX10_HID=1` again).
* **Replug** (2026-10-25): a FLX10 that is power cycled or replugged gets the vendor
  unlock from udev (`99-flx10-unlock.rules`, installed by install.sh), the display
  connect sequence from the map, and every deck that was playing is paused when the
  surface is found gone (`CTRL_PAUSE_ON_LOSS=1`, any surface; ctrlshim.c's
  `pause_on_loss`). Decks stay paused on reconnect. Works on the unit
  (operator, 2026-10-25).
* **hid-scan.py round 1** (2026-10-25, bytes 3-10 flipped one bit at a time on
  deck 1): [3] 0x08 is REMAINING time -- rekordbox's default, so every capture
  had it; without it the jog shows elapsed. Now follows rbp's time mode
  (SHIFT + 4 BEAT flips both). Round 2 (bytes 8-9): [8] 0x80 is the jog's
  light ring (dark without it -- a candidate for the track-end warning flash);
  [9] 0x04 is the cue marker on the screen, now cleared when there is no cue
  point.
* **hid-scan.py full pass** (2026-10-25, every bit outside the known
  time/BPM/tempo/loop/cue bytes, 0.1 s a step): [2] 0x01 is SLIP on the jog
  screen (now sent from rbp's slip mode). Nothing lit the centre BEAT SYNC
  text -- taken as not host-driven (operator's hunch; a single-bit scan cannot
  rule out a multi-bit value, so a rekordbox capture would be the last word).
* **Track-end warning** (2026-10-25): the jog's light ring ([8] 0x80) flashes
  with rbp's own warning -- the one that flashes the overview waveform on the
  screen. Ui_CulcRemainCaution @0x134244 keeps int
  ui_remainCautionReverse[deck] @0x03275750 as the flash phase: 30 s or less
  left, 450 ms on / 450 off; 15 s or less, 100 / 100; 0 otherwise. The HID
  sender reads it at 100 Hz (flx10_end_caution) and clears [8] 0x80 in the
  "on" half.
* **Splash until rbp is ready** (operator, 2026-10-25). A splash covering the
  whole screen from launch until rbp has fully initialized, so none of its
  start-up drawing shows, and no input (FLX10, keyboard, touch) reaching rbp
  until then. What exists: the Pi's boot screen (`rblive4-boot`, docs/11 "The
  boot screen") paints stage lines on fb0 and leaves at rbp's FIRST frame
  (`rbp-drew`) -- too early, and it looks for fb0 named `vc4drmfb`, so on the
  ODROID (meson's drm_fbdev) it likely never shows. Plan: fb_shim keeps
  presenting the splash in place of rbp's frames until a "ready" signal, and
  ctrlshim / pointsrc drop input until the same signal (the FLX10 connect
  sequence and LEDs can still go out). The ready signal is to be chosen --
  candidates: rbp's UI reaching the player screen (its screen-mode state),
  the PlayEngine and LED table populated and the frame unchanged for ~1 s, or
  a fixed delay after the first frame as a fallback. Needs a look at what the
  odd start-up drawing actually is.
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
| PLAY, CUE / SHIFT + CUE | PLAY, CUE / back to the start: a seek straight to the auto cue point (the main cue when auto cue set it, else the top), play/pause kept, nothing repeats while held |
| BEAT SYNC / SHIFT | SYNC / MASTER (tempo master); long press nothing |
| KEY SYNC / held | nothing (rbp has no key sync) / MASTER TEMPO; SHIFT nothing |
| TEMPO RESET / SHIFT | toggle: tempo held at 0.0%, fader ignored; off = back to the fader (LED lit while on) / TEMPO RANGE |
| TEMPO fader, jog (top press = touch, top spin, side ring) | the same |
| SHIFT + 4 BEAT (loop section) | flip the deck's elapsed / remaining time (rbp's TIME MODE, `UiSetKeyTimeMode`; the jog screen follows) |
| 4 BEAT JUMP < > / SHIFT | beat jump -4 / +4 beats / -16 / +16 beats (`DjEngineIF::playBeatJump`, paused or playing, one jump each) |
| SLIP REVERSE / SHIFT | momentary reverse that always slips / latching reverse that never slips (LED lit while held or reversing) |
| QUANTIZE; SLIP / SHIFT | QUANTIZE; SLIP / nothing (VINYL disabled: no light on the button) |
| CUE/LOOP CALL < > and SHIFT | not mapped |
| MIX POINT SELECT < > | < = as SHIFT + CUE, or the previous track when already within 1.5 s of the start / > = next track (TRACK SEARCH tapped 100 ms, no repeat while held; < stamped as rbp's "second press") |
| SHIFT + MIX POINT, LINK, ACTIVE PART, PAGE, DECK SELECT, SAMPLER, BOOTH | nothing |
| browse knobs (one shared message) / SHIFT | scroll / 8 steps per detent |
| browse push (per deck) / held 0.5 s / double | on a track row LOAD that deck, on anything else SELECT (opens the folder/playlist/category) -- the row type is rbp's own `UiBrowseComm_IsTrackList` (@0x105e60) / **rbp's own long press of the selector = FAST SCROLL** (selector PRESS + op 1, held until the knob comes up; TAG TRACK is unmapped) / twice; **on the SOURCE screen with a stick in slot 1: open USB 1** (rbp ignores the knob there) |
| BACK / VIEW; SHIFT+BACK / SHIFT+VIEW | BACK / SOURCE from the player, SOURCE again on the SOURCE menu (closes it), BROWSE (back to the player) from a browse list; PLAYLIST from the player, back to the player when pressed again from that list, TAG LIST elsewhere / SHORTCUT |
| SHIFT + deck 1's browse push | TRACK FILTER (deck 2's SHIFT push not yet recorded) |
| pad modes HOT CUE / PAD FX1 / BEAT JUMP / SAMPLER | HOT CUE / SLIP LOOP / BEAT JUMP / BEAT LOOP 2 |
| SHIFT + those: KEYBOARD / PAD FX2 / BEAT LOOP / KEY SHIFT | GATE CUE / RELEASE FX / BEAT LOOP / BEAT JUMP 2 (a SHIFT button goes straight to its mode; pressing a main button again does what SHIFT + it does, and a third press comes back -- the FLX10's double-press behaviour) |
| pads; SHIFT + pad in HOT CUE | pads in rbp's mode; delete that cue |
| ch 1/2 TRIM, EQ, COLOR, FADER, CUE; master CUE, crossfader, HP mix/level | the same |
| crossfader assign A / THRU / B (deck notes 22 / 29 / 24) | rbp's crossfader assign for that channel (`DjEngineIF::setCrossFaderAssign`; the RX3 only sets it from its curve switch, so it sat at THRU) |
| MASTER LEVEL | the unit's own volume; feeds only the HDMI mirror (as on the FLX4) |
| SCFX SPACE, D.ECHO, CRUSH, NOISE, FILTER / PITCH | the same names / SWEEP. SPACE is 0x50a1 and CRUSH 0x50a5: rbp_abi.h had them swapped, corrected from the unit (2026-10-24) |
| SAMPLER VOL | SCFX PARAMETER (0x50a7, channels 1 and 2) |
| BEAT < > / SHIFT; LEVEL/DEPTH / SHIFT; ON/OFF | BEAT < > / AUTO (BPM back to auto) and TAP; DEPTH / TIME; Beat FX ON/OFF |
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

**Jog scaling = rekordbox's, from a capture** (2026-10-25). The 12960/5484 gain
(tuned by ear, "one turn = 4 beats") moved the track ~2.35 s a platter turn: the
jog screen's arm, which turns once per 1.8 s of track, ran 3.92 turns for 3.
`tools/flx10-pcap-decode.py --jog` on a rekordbox capture (paused deck, three
turns each way) gives rekordbox's track ms per jog count from the platter's own
MIDI beside the jog screen's position, with no turns counted by hand:

| control | ms per count | counts per turn (1.8 s) |
|---|---|---|
| platter top (CC 34/35/31) | 0.1390 | ~12950 |
| side ring (CC 33/38) | 0.1650 | ~10900 |

**Then measured on the unit** (JOG_CAL, ten taped turns each): the top gives
129410 counts in 10 turns (12941 a turn: rekordbox's 0.1390 ms a count is 1.8 s
a turn, the arm exactly), the side ring 125437 (12544 a turn -- the capture's
hand count was off, and rekordbox's 0.165 runs the side ring ~2.07 s a turn,
ahead of the arm). The map uses 1.8 s a turn on both: **top 0.1391, side 0.1435
ms a count**. Confirmed on the unit (operator, 2026-10-25): ten turns from
12:00 end with the arm back at 12:00.

(Single stray jog CCs on the other deck channels, value 0x58, show up in the
capture and are ignored.) The map now moves rbp by exactly those: a count is
`ms_per_count x 720 / RBP_TURN_MS` of rbp's 1/720-turn units, speed and position
alike, where RBP_TURN_MS is the track one turn of rbp's jog moves: **1000 ms**,
measured on the unit with `RB_JOG_CAL=1` (`jogcal ... rbp_turn_ms=` 1000.3,
1000.0, 1002.3, 999.1 over three-turn gestures, top and side, both ways;
`RB_JOG_RBP_TURN_MS` overrides). `RB_JOG_GAIN` multiplies both controls (default 1);
`RB_JOG_PPR` no longer applies to the FLX10.

### FLX10 LEDs

Probed 2026-10-24 with `scripts/odroid/led-probe.py` (`flx10-leds.json`, gitignored).
Every LED answers on its own button's input note and channel, ON = 0x7F, OFF = 0.

| What | Note (channel) | Driven from |
| --- | --- | --- |
| PLAY / CUE / SYNC | 11 / 12 / 88 (deck) | the same transport decision the drawers show (CDJ / rekordbox CUE and PLAY, below) |
| KEY SYNC | 101 (deck) | kept dark (sent off on connect) |
| TEMPO RESET | 65 (deck) | the map's tempo lock |
| SLIP REVERSE | 21 (deck) | held, or the deck reversing (`PlayEngine::getReverse`) |
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
| marker | CC 0x10+d / 0x30+d | 0..359 deg -- the clock arm; works (deck 2, after a load), but a deck can hide it -- condition still open |
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
every start before rbp opens the card, and `99-flx10-unlock.rules` runs it again
whenever the unit (2b73:0041) is added to the bus; `--rebind` also rebinds
snd-usb-audio.

`rbp_led.c` reads the deck values each tick (`PE_GETTOTALTIME`, `PE_GETPLAYINGTIME`
-- ms --, `PE_GETQBPMDISPLAY`, `PE_GETTEMPOX100`, `PE_GETTEMPORANGE`, MT / SYNC /
MASTER) and hands them to the map's `deck_display` hook; `map_flx10.c` sends them.
The marker (clock arm) IS sent, one turn per 1.8 s of play: a first hand test on
deck 1 moved nothing, but deck 2's arm appeared after a load and followed hand-sent
values (2026-10-25). Whether a deck shows it is the unit's own setting (double tap
SHIFT on the FLX10), kept across reconnects (see "To revisit"). The cue marker
is not sent yet. BPM is rbp's own screen value (StatWatcher getPlayBpm, x100) sent x10 and
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
