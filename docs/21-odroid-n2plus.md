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
  32 bpp**, and `rbp.service` is active with `edb_streamd`, `rbp -a`,
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

1. **HDMI audio mirror -- done** (2026-10-26): the board card's device 0
   (`hw:CARD=ODROIDN2,DEV=0`) as plain PCM, `s24_le` at rbp's 44.1 kHz (it
   takes S16/S24/S32_LE, 8..192 kHz; the monitor's ELD offers 2 ch LPCM
   32..192 kHz). start-rb.sh sets the route on every start -- FRDDR_A SRC 1 EN
   on, FRDDR_A SINK 1 SEL = OUT 1, TDMOUT_B SRC SEL = IN 0, TOHDMITX I2S SRC
   = I2S B, TOHDMITX on -- and rb.conf makes both the Odroid default.
   Confirmed by ear on the unit; `RB_AUDIO_MIRROR_DEV=` turns it off.
2. **Waveforms and album art on the jog screens -- built, to look at on the
   unit** (2026-10-26). rekordbox sends them as HID transfers that are the
   export's own files behind a length: the 80x80 and 240x240 artwork JPEGs,
   the .2EX's PWV6 (overview) and PWV7 (detail), and the .DAT's beat grid --
   all checked byte for byte against capture_2 (both decks), layouts in
   flx10_media.h and tools/captures/flx10-rekordbox-hid-notes.txt.
   flx10_media.c finds the loaded file's track in the stick's export.pdb
   (read only) and flx10_hid.c sends them after each load. Sent and
   completed on the unit (usbmon: every packet rekordbox's, state packets
   still 100 Hz), with track 1390 on deck 1 and 692 on deck 2 left loaded --
   not yet SEEN. To check: artwork in the jog centre, overview and detail
   waveforms, beat markers; a track with no artwork (id 705) clears the
   previous one's; a power cycle brings them back (sent again 6 s after the
   unit is found). Not sent, meaning unknown: 0x2d (cue list?), 0x39,
   0x3b, 0x3c, 0x3e (played part?). `RB_FLX10_MEDIA=0` turns it off.
   Confirmed on the unit 2026-10-26: artwork, waveforms, a track with no
   artwork. Stacked waveforms: the tempo % sent is now the deck's real speed
   (it was the fader, wrong after SYNC off), and state packets go out every
   8 ms on the clock as rekordbox's do (they were every 15 ms). The scroll
   still steps a little -- it does under rekordbox too (operator), and the
   positions sent are within ~1 ms of a straight line (usbmon), smoother than
   rekordbox's own (12.5 ms steps at 8/16 ms): the unit's rendering, left.
   A loaded track's waveform needed a display-mode change to appear until
   the rest of rekordbox's load sequence went out too (0x30 empty, 0x2d,
   0x39, 0x3b, 0x3e; flx10_media.h); with it, and 5 transfer packets a tick,
   it comes up by itself and fast (operator, 2026-10-26).
3. **Album art on rbp's browse screen -- done** (2026-10-26): every track
   showed the default cover. rbp's DecodeScalingJpeg @0x176208 decodes with
   libjpeg and scales to 240x240 through the i.MX GPU (libg2d); with no such
   GPU g2d_open failed and it returned -2. fbshim.so now exports the seven
   g2d calls rbp imports, in software (g2d_sw.c). Confirmed on the unit.
   Separately, once (2026-10-26) rbp stopped drawing its HDMI waveforms
   until a restart -- the stick's files were intact; not seen again.
4. **Hot cues on the jog waveform -- done** (2026-10-26, capture_hotcues):
   they are in 0x30, not 0x2d (flx10_media.h): 8 slots of type, colour and
   IN time, built from the EXT's PCO2 at a load, byte for byte rekordbox's
   for a track with 4 cues and 2 loops. A cue set or deleted on the unit
   goes out at once from rbp's own slots (UiGetHotCueINtime / LoopFlg /
   ColorInfo). Not yet seen: a cue with its own palette colour (sent as its
   code), and 0x39's cue text ("A", "D 02:35"), still sent as the bare
   "HOT CUE" label. SHIFT + pad deletes through rbp's own SHIFT key
   (rbp_abi.h K_SHIFT) -- the direct calls took only on a playing deck and
   left the cue drawn until the next input -- and under SHIFT the HOT CUE /
   GATE CUE pads show their colours dimmed (ctrl_map.h pad_shift_mirror).
5. **rbp.log's `CmnFunc_Error( ... UniNo=0x0000102a, Val=0xffffffee )`,
   ~3 a second from ~15 s after every start -- fixed** (2026-10-26), and
   with it /tmp/rb_load "doing nothing after a restart". rbp_load.c replayed
   the command left in /tmp/rb_load at every start, before rbp had made the
   memory pool its load handler checks; CmnFunc_get_mpf_blkcnt then loops on
   ref_mpf's -18 every 300 ms for good, and the watcher thread with it. A
   line already there is now not replayed, a load waits for the pool, and
   the watcher runs only in rbp.
6. **Booth and master outputs -- done** (2026-10-27), by ear. The FLX10 plays
   back four channels (master, headphones) and makes its booth output from its
   master mix in hardware; booth was silent only because the speakers were
   off. UTILITY's MASTER ATT. scales the master samples (masteratt.c). Its
   BOOTH MONITOR ATT. does nothing: a pass-through to the unit's own booth
   attenuator (ch 16 CC 13, 0 / 1 / 2, the 0 in rekordbox's connect sequence)
   from a detour of BoothMonitor::setATT @0x5a44c was built and reverted; the
   FLX10's BOOTH knob sets the level. Booth flipping between two levels at
   fixed points in a track was the jog time CCs landing on the unit's own
   settings -- fixed, see the jog display table.
   **Since 2026-10-28 both attenuators are the unit's own** (operator's USB
   capture of rekordbox's preferences, capture_settings): ch 16 BF 42 master,
   BF 43 booth, 00 / 10 / 20 = 0 / -6 / -12 dB (the CC 13 tried before was the
   wrong number). rbp's MASTER ATTENUATOR / BOOTH MONITOR ATTENUATOR go out that
   way (masteratt.c records them, map_flx10.c flx10_unit_tick sends them) and the
   samples are no longer scaled. So do MIC OUT TO BOOTH (9F 4E), MIC OUT TO
   MASTER REC (9F 3F, the unit's "mix the MIC audio to REC OUT") and POWER
   MANAGEMENT (9F 45, the unit's auto power off; 7F on). What rbp has no row for
   are four rows of the shim's in MIXER after the attenuators, kept on this
   machine (/root/settings/flx10.dat in the chroot, beside rbp's XdjSettings.dat;
   rekordbox's defaults until changed) and sent at every start like rbp's own --
   the connect sequence never sends any of these from the capture: MASTER / BOOTH MONO/STEREO (9F 47 / 48, 7F stereo), PEAK
   LIMITER (9F 49) and MIC OUT TO MASTER (9F 4A). BF 44 is the unit's demo /
   screen saver (00 disable, 10 / 20 / 30 demo after 1 / 5 / 10 min, 40 screen
   saver): GENERAL's FLX10 DEMO/SCREEN SAVER row, kept and sent the same way
   (disabled by default). rbp's own SCREEN SAVER dims the RX3's LCD backlight
   (LcdBacklightPwm::setScreenSaverOn); nothing here drives a backlight. The MASTER / BOOTH / headphone knobs are
   applied by the unit and are not in its answer to the controls query, so the
   HDMI mirror still starts at 0 until the MASTER knob moves.
7. **Key sync -- built, to try on the unit** (2026-10-26). KEY SYNC (short
   press; held is still MASTER TEMPO) latches like a CDJ: the deck that is not
   the tempo MASTER is shifted by the fewest semitones (-5..+6) onto the
   master's Camelot number (its key or the relative major/minor), counting
   the semitones a tempo moves the pitch with MASTER TEMPO off; it follows the
   master's key, lets go on a new track or a second press, and its LED shows
   the latch (map_flx10.c, flx10_keysync_*). A track loaded already on the
   master's key lights it with no shift. Revised the same day to the CDJ-3000's
   rule as its documentation describes it: compatible is the master's Camelot
   number or one either side, either letter (same key, dominant, subdominant
   and their relatives), the shift is the least that gets there (never more
   than 2 semitones), the LED is lit whenever the deck is compatible, and a
   press undoes only a shift of ours. A load counts once its key and length
   have held 400 ms (the LED had lit, cleared and relit through a load). rbp's
   own deck panel shows the shifted key too (keydisp.c: the panel's key line
   at 0x03268bf0 + deck * 0x238, UTF-16 at +34, which ui_Deck_Update redraws
   on change; the original is remembered and put back). The jog shows the key the deck plays
   in -- the tempo's shift and KEY SYNC's together -- with the +N beside it
   (flx10_hid.c, key_semis). Heard working by the operator 2026-10-26. The shift itself is rx3-toolkit's
   (github.com/Tratosca/rx3-toolkit at cc17a1f, MPL-2.0): a detour on
   dsp::TimeStretchManager::operate @0xa0e54 (guard f0412de9 0040a0e1) that
   shifts each deck's block in place, down through rbp's own BeatEffectPitch
   and up through the toolkit's shifter (keyshift.c, rx3_pitch_shift.h, both
   MPL; built armv7 softfp for the float work). The hook installs on the unit;
   not yet heard -- the FLX10 was off. Not validated on hardware by the toolkit
   either. Also to check: the audio thread keeps up (no dropouts).
8. **Accent-insensitive search -- built, to try with a touchscreen**
   (2026-10-26): NINO finds NIÑO. searchfold.c (rx3-toolkit's search-latin,
   MPL) detours shape() @0x1644fc, which makes the upper-cased copy of each
   track's text rbp's search compares against, and folds Latin-1 letters in
   it to plain ASCII -- returning shape()'s character count, which the toolkit
   dropped and rbp's matchers use as the length. RB_SEARCH_FOLD=0 is off.
   Installs on the unit; untested until something types into rbp's search.
9. **One stretch for the same track on both decks -- done** (2026-10-26).
   With MASTER TEMPO on, the same track on both decks (instant doubles, loaded
   twice, or the same file on two sticks) sounded hollow -- a swoosh, gone with
   MT off, never with two different tracks. rbp stretches each deck on its own
   (dsp::TimeStretchManager: two foreground stretchers, a background one with a
   smoothed speed table), and the splices follow each deck's own history: the
   two decks' stretched output, recorded side by side at the same position and
   speed, correlated 0.04 with the best lag wandering +-185 samples, and still
   drifted after both restarted from one cue. Not threads: everything pinned to
   one core changed nothing. doublelock.c, called from keyshift.c's detour on
   TimeStretchManager::operate (rbp runs both decks there one after the other,
   64 frames a callback): when the second deck reads the same track -- the same
   length to the sample -- within 20 ms of the first, it plays the first's
   stretched block, crossfaded over 512 frames each way. Further apart is an
   echo and is left alone. Key shift, EQ, FX and faders stay per deck.
   RB_DOUBLES_LOCK=0 is off. The operator: "the swoosh is gone".
10. **rbp's renderer, a third of what it was -- done** (2026-10-26). gui_task
   (rbp's render loop) ran at ~82% of a core. No hardware draws here: rbp's
   DirectFB has no gfxdriver at all (directfbrc: no-hardware), as on the RX3, and
   the mainline kernel has no GE2D driver. Sampled, 302 of 500 samples were
   DirectFB's generic two-stage blit of rbp's RGB565 colour-keyed images onto the
   RGB32 layer (Sop_rgb16_Kto_Dacc, Sacc_to_Aop_rgb32): the RX3 draws 16 onto 16.
   A 16-bpp trial (the depth pair at 16) cut rbp to 75% but broke the active deck
   strip frame's right edge again (dotted, a pixel over, stray pixels) -- back to
   32. blitfast.c (fbshim) detours the first stage: for exactly that pipeline,
   forwards, it writes the 32-bit pixels straight to the layer, NEON, eight at a
   time, and the second stage skips the span. Its first 2000 spans are checked
   against DirectFB's own and it turns itself off on any difference ("2000 spans
   matched DirectFB exactly -- trusted" in pointsrc.log). gui_task 82% -> 29%, rbp
   104% -> 39% of a core; the renderer now mostly waits for vblank.
   RB_BLIT_FAST=0 is off.
11. **rx3-toolkit's track-finding mods -- a third BROWSE column working (ARTIST
   on this unit); Key Match taken out** (2026-10-27/28). Ported from rx3-toolkit
   at cc17a1f (MPL-2.0, each file says so): browsecol.c and browserows.c (the
   toolkit's BROWSE row service), in knobshim. Every hook site and called
   function was checked byte for byte against our rbp 1.20 -- the toolkit's
   guards are written for 1.19 and match.
   - **Key Match -- taken out** (operator, 2026-10-28). Yellow / orange / red
     key icons for Energy Boost +2 / +7 / +4 matches to the MASTER, beside
     rbp's green. Deployed, then at the decks on a 1,200-track playlist: the
     icons showed for a moment and went, scrolling stopped and BROWSE showed
     WAIT, rbp's CPU climbing (health.log). Most likely suspect, not proven: the
     database-server hook below, one DeviceSQL read per record rbp prepares --
     on a big playlist perhaps the whole list, not the visible page. The
     operator would rather have the columns; the code is in b2d5823 if it is
     ever wanted again (keymatch.c, rbp_images.c, rx3_harmony.h).
   - **Third column -- working on the unit** (RB_BROWSE_COLUMN=artist|album|
     bpm|key|time; empty = off; ARTIST in this unit's rb.local.conf since
     2026-10-28). No stall on the operator's 1,200-track playlist. Laid out as
     on a CDJ-3000 (the operator's photo): TRACK | the column | rbp's sort
     column, far right. Measured with a render probe on the unit: rbp's track
     list is layer 0x0801, each row two rectangles (x ..857, 857..1280), the
     title as text 474..844, the second column's icon at 864 and text 898..,
     1-pixel dividers (855), and the headings are images (ALBUM 2024, ARTIST
     2032, BPM 2045, KEY 2409, TIME 2598, TRACK 2603). With the column on, the
     title is cut at 729, a divider and rbp's own heading image for the field go
     at 740, and everything of the second column moves 185 right (rect, icon,
     text, heading, divider hooks on PAL DrawRectangle/DrawImage/DrawText). Its
     value rides inside rbp's own second-column row text after noncharacters.
     When rbp's second column is already that field (sorted by artist, say), the
     column is not shown. The floating LOAD 1/2 under the selected row are hidden
     (their touch does nothing; the FLX10's LOAD keys still load); the selected
     title scrolls inside its narrower cell, the moved column does not scroll.
     rbp_hook.c now moves a pc-relative literal load into the trampoline (one of
     the scroll hooks starts with one).
   - Fixed 2026-10-28: the column blinked in and out while scrolling. Two
     causes: the layout was switched off and on around every page (a frame drawn
     between showed rbp's own), and rbp sometimes draws a row from its own copy of
     the text, without our value in it. The layout now changes only when a page
     decides it, and every value shown is remembered by track id (64 of them): a
     row drawn without one finds its track through rbp's per-row glyph IDs and the
     list's own rows, and draws the remembered value.
   - **Which column** (operator, 2026-10-28): the stick's own MY SETTINGS >
     Column by default, changed live from the gear SETTINGS panel's new BROWSE
     COLUMN row (OFF / ARTIST / ALBUM / BPM / KEY / TIME; the panel is ten rows
     now, 677 px). Both go through /run/rbp/browse.column, which browsecol.c
     re-reads a few times a second; a stick put in or re-exported writes its own
     Column there. rekordbox keeps that Column in export.pdb, **table 18** (the
     players' SORT list: rows of 8 bytes -- rekordbox's id, rbp's
     MODULEMAIN_CATESORT id, a flag, the menu position): the one row whose flag
     is **2**. Found by exporting with ARTIST, then ALBUM: no *SETTING*.DAT byte
     moved, the flag moved from the ARTIST row to the ALBUM row, nothing else in
     the table did. rbp reads the same flag for its own unsorted second column
     (the operator saw it turn to ALBUM). With rbp sorted, the column shows the
     chosen field beside the sort's; unsorted, both would be the same, so ours
     hides. A stick whose MY SETTINGS has no Column (no flagged row), or one not
     shown here (RATING, GENRE ...), turns the column off (operator, 2026-10-28).
     RB_BROWSE_COLUMNS=0 removes every hook; RB_BROWSE_COLUMN is only the
     start before a stick or the panel says anything.
   - Lesson (2026-10-28, rbp down ~4 min): a shim must not call stat() -- rbp's
     glibc 2.13 exports no `stat` symbol, so knobshim failed to load ("undefined
     symbol: stat") and rbp crash-looped. Check new imports with
     `arm-linux-gnueabi-nm -D --undefined-only knobshim.so | grep -v @` before a
     deploy; 64-bit atomics are out too (libatomic).
   - **COLUMN in LIST MENU** (operator's ask, 2026-10-28; browsemenu.c): a
     playlist's LIST MENU is SORT, PLAYSTATE and now COLUMN, which opens OFF /
     ARTIST / ALBUM / BPM / KEY / TIME with the current one checked; a pick
     writes /run/rbp/browse.column as the gear row does, and closes the menu the
     way a SORT pick does (the list reloads under its own sort, with the new
     column). Read from rbp 1.20: SendBrowseMenu @0x257dd0 builds each level of
     the menu into context+0x43378 (type word per depth, 0x400 = LIST MENU;
     entries 0x210 apart from +0x22c: id, state, UTF-16 text); a depth-0 pick in
     SelectRightMenu @0x2596cc steps to depth 1 for any item and keeps it in
     context+0x6fd0/+0x6fcc, so a third entry steps down like SORT; at depth 1
     we build our list and take the pick (SendSortedBrwsList @0x256030 to close),
     and the level's title, rbp's "PLAYSTATE" for any item but SORT, becomes
     COLUMN in SetMbxComAddString @0x1bb7f8 while that one menu is sent.
     SendRightMenu @0x256218 gets the third entry. Every guard checked against
     the binary. Deployed 2026-10-28.
   - **Fixed 2026-10-28: BROWSE froze after a sort change and a scroll.** The
     DB_Client thread spun at 100% in rbp's own SIGSEGV handler (which returns,
     so the fault repeats; crashcatch never sees it): GetList @0x1bbb70 cleared a
     NULL message, because GetGuiDbMsg's pool had come back empty-handed (not a
     timeout: that one waits and retries). Read with ptrace on the stuck thread:
     pc in libc memset, lr 0x1bbb98. Only the column's per-row DB-client queries
     (values_for: dbcl_GetSongData_Track and the record stream) could make that
     happen, and they ran only for ALBUM, the one field the server-side snapshot
     did not carry -- so only when the sort was not ALBUM, i.e. after a sort
     change. Now the snapshot carries ALBUM too (djdbContent column 6, the album
     id, as SortTrackSetListBuf reads it; its name via Get_DsqlTableString kind
     8, djdbAlbum -- kind 2 is a djdbContent column, the file's path, which is
     what the first try showed), and a local stick's rows never query the database from the row builder:
     a row the snapshot missed shows a dash and is asked again on the next page.
     A remote player's list (PRO DJ LINK) still uses the toolkit's queries.
   - **BPM and KEY, always, as two columns** (operator, 2026-10-28: rbp's BPM
     column carries the key too, "170.0 bpm - 7A"). Every track list is now
     TRACK | the chosen field | rbp's second column | BPM | KEY, KEY at the far
     right (BPM 1110..1205, KEY 1205..1280, rbp's column 190 px, the chosen 220).
     rbp's own column goes when it is BPM or KEY; the chosen one when it is off,
     BPM, KEY, or what rbp already shows. BPM is the number alone ("170.0"), KEY
     like "10A". Each row carries the three values behind rbp's text, joined by
     U+FDD2; the server-side snapshot now reads every record's key as well as its
     BPM, so a local stick still never queries from the row builder. Seen on the
     unit with a CUSTOM-ordered playlist: # | TRACK | ARTIST | BPM | KEY.
   - **The traffic light on the KEY cell** (operator, 2026-10-28). rbp marks a row
     whose key matches the MASTER's by giving it kind 52 and drawing its green key
     icon (images 2577..2579) where its second column's icon goes. That icon, and
     the person icon by an ARTIST (2028..2031), are no longer drawn; instead the
     row's KEY cell is filled green and its key drawn black. The fill is the row's
     own rectangle (3 px shorter: rbp's reaches into the next row) recoloured
     0xff20b020 -- rectangle colours are ARGB in word 0x28. A text's word 0x28 is a
     palette entry in its second byte instead (white 0x00, the playing row's green
     0x18); swept on the unit, entries 1..11 are rekordbox's track colours, 30
     is black, which the lit key is drawn in. rbp smooths a glyph's edge toward
     dark, not toward what is under it, so a dark grey (22) wore a jagged dark
     outline on the green; giving the text its own background to smooth against
     painted the box black. The box is inset 1 px top and bottom, so two lit rows
     keep a 2 px line between them, over the row's ordinary shade across the
     cell so the selected row's blue does not show around it. rbp draws the
     selected row's own rectangle blue, so the shade is the colour most rows of
     the same parity have (the row two away was tried first, and is the
     selected one now and then). Every list row's text and
     icons are drawn 2 px lower (capitals 15..33 of 50, from 13..31), the
     selected row's scrolling title too (lowered around
     ui_ListBrowse_StartTextScroll, which places the scroller's window). A lit first row also made rbp head its column KEY; the heading is
     now the field the rows hold. UTILITY's new TRAFFIC LIGHT row (OFF / ON, ON by
     default; the end of the DECK section, after IMAGE ON JOG DISPLAY -- not under
     MIXER, the operator's word) writes /run/rbp/browse.trafficlight, which
     browsecol.c re-reads with browse.column.
   - **Fixed 2026-10-28: BPM and KEY blanked to dashes now and then while
     scrolling.** Each new page emptied the server-side snapshot, and scrolling
     fast a page's rows were built after the next page had begun. The snapshot is
     now a ring of 64 records by track and stick, emptied only when a stick goes
     in, out or is re-exported; a row it still misses shows that track's last
     values. BPM is right-aligned (the text model's alignment byte 0x3c is a 3x3
     grid, its third the horizontal: 0x1d2724 in NS_PALRender_DrawText), and one
     middle cell alone is the CDJ-3000's ARTIST width, 330 px.
   - **The # column goes when it is only dashes** (operator, 2026-10-28): sorted
     by anything but CUSTOM, rbp's # column (379..434) shows "---" in every row.
     Then its texts, its heading and its divider (434) are not drawn and TRACK
     (note icon at 442, title from 474, heading) moves 55 px left; the selected
     title's scroll is measured from there too. Decided by what rbp draws there:
     dashes hide it, numbers bring it back, with our column on or off.
   - **COLUMN stays OFF / ARTIST / ALBUM / BPM / KEY / TIME** (operator,
     2026-10-28: not every sort option). For reference, rbp's own second column
     under each SORT, probed on the unit with every sort enabled (sort id at db+16,
     its rows' kind, its heading image): ARTIST 2/7/2032, ALBUM 3/2/2024, BPM
     4/13/2045, RATING 5/100/2574 (stars, U+2606), GENRE 6/6/2392, COMMENTS
     7/35/2747, TIME 8/11/2598, REMIXER 9/41/2591, LABEL 10/14/2420, ORIGINAL
     ARTIST 11/40/2557, KEY 12/15/2409, BITRATE 13/16/2039, COLOR 15/19/2309, DJ
     PLAY COUNT 16/42/2380, DATE ADDED 17/46/2341; DEFAULT is sort 0, BPM
     (UNSORTED) 21. SortTrackSetListBuf @0x171908 reads each from djdbContent
     (artist col 5, album 6, genre 7, BPM 8, time 9, bitrate 11, comment 13 as a
     string, rating 15, remixer 17, label 18, original artist 19, key 20, date added
     21, colour 22, play count 23) should a field ever be wanted. Table 18 (MY
     SETTINGS) also lists the stick's SORT menu: flag 0 or 2 rows are on it, in
     their position byte's order; flag 1 rows are off.
   - The preview waveform switch and text size are off the list (operator,
     2026-10-28). The CDJ's PREVIEW button is item 32.
   - test_browse: the row rewrite on a synthetic rbp context (folder lists
     left alone), the column's transport, the literal relocation, and COLUMN's
     LIST MENU entry and submenu on a synthetic menu block.
12. **Four-deck mode -- wished for, not started** (operator, 2026-10-26). The
   FLX10 is a four-deck controller (DECK 1/3 and 2/4 select); the RX3 and its
   rbp are two-deck: PlayEngine's player vector holds two (SyncMaster counts 2
   on the unit), MixerEngine two channels, the UI two lanes and two deck
   panels. The first question decides the size of it: whether that two is a
   count rbp reads at start (so the shim could raise it) or built in.
   Answered for the engine (read 2026-10-26): it is READ. djengine::DjEngineIF::
   NUM_PLAYERS (2, @0x417358 in .rodata) is loaded from memory by the only two
   places that use it, PlayEngine::PlayEngine() -- a loop of createPlayer(),
   each appended to a growable vector -- and MixerEngine::MixerEngine(); the
   sync state and the per-deck getters check against the vector's size. So a 4
   written there before the engines are built should make four players and
   four mixer channels. What is two above the engine is the RX3's UI (two deck
   panels and lanes, ENUM_DECK, the StatWatcher's two deck pointers), many
   two-deck immediates, and the output routing. Next: RB_NUM_PLAYERS=4 as an
   experiment -- does rbp start, can decks 3/4 load and be heard -- with the
   operator there. If the
   engine can host four, the rest is routing and display: the FLX10's own jog
   screens already show each deck's waveform and artwork (flx10_hid.c), and
   rbp's two lanes could follow the selected decks as rekordbox's 2-lane view
   follows DECK 1/3 and 2/4 -- or the shim draws lanes for 3/4 itself, as it
   overlays other panels. If it cannot, four decks is a second engine beside
   rbp's, a much larger thing. For the screen, the operator's model: the
   XDJ-AZ's four-deck layout -- four waveform lanes, the HOT CUE pad rows
   hidden to make the room (operator, 2026-10-26). rbp has no such layout of
   its own, so it would be the shim's to draw, over the space rbp's two lanes
   and pad rows take.
13. **Line inputs on all four channels -- wished for, not started** (operator,
   2026-10-26: important; after the booth and master outputs, item 6). Not
   working today: audioshim answers rbp's capture with silence ("mapped virtual
   Capture device") and nothing reads the FLX10's inputs. The FLX10 has them:
   its USB capture is 10 channels, S24_3LE at 44.1 kHz (/proc/asound/DDJFLX10/
   stream0), five stereo pairs -- CH1..CH4's LINE/PHONO and one more (MIC or a
   master record pair) -- on the same endpoint the playback's implicit feedback
   clocks from. Which pair is which channel: record all ten with a signal on
   one line input. For channels 1/2 rbp has the mixer side already --
   MixerChInnards::onEv_DeckLineSW(SW_MIXER_SRC), a per-channel DECK/LINE switch
   playing rbp's capture stream through the channel's fader and EQ -- so: feed
   the right pair into rbp's capture stream instead of silence, and map the
   FLX10's input selector to that switch. Channels 3/4 need mixer channels 3/4
   first: the four-deck item (12).
14. Parked: HOT CUE mode switch lag.
15. **A loaded track survives its USB's eject -- wished for, not started**
   (operator, 2026-10-27). Today ejecting the stick takes the decks' tracks
   with it. Probably a copy of each loaded track (audio and analysis) held off
   the stick -- in RAM or on the eMMC -- and rbp kept reading that copy; how
   rbp reacts to its media going away is the first thing to read.
16. **Beat FX quantize -- done** (2026-10-27, commit ad91f4e; heard working
   on the decks by the operator the same night). TRANS played off then on, half a beat out. Read on the
   unit: the touch panel's Beat FX quantize was ON (MasterInnards+0x45) but
   the engine's flag was 0 -- onEv_FxQuantize sets it to the panel's state AND
   isBeatEffectQuantizeValid(), false with no track loaded, and rbp applies it
   only at start and on FX type / channel / TAP / beat changes. map_flx10.c now
   sends the panel's state back through its key (K_EFFECTQUANT 0x0493) 1.5 s
   after each load, an OFF as ON the first time after a start; the touch panel
   stays the switch ("Beat FX quantize on again after a load" in
   /tmp/knobshim.log). Never explained: a /tmp/rb_load load turned the engine flag on by itself while
   an FLX10 browse load had left it off; whether the FLX10 times its own FX ON
   press to the beat is unmeasured. TRANS's gate itself is the right way
   round (BeatEffectTrans::execute: on for the first part of each cycle).
17. **MASTER ATT. on the FLX10's own attenuator -- done after all, 2026-10-28,
   under item 21** (BF 42; item 6 has the settings). Dropped on 2026-10-27; the
   reason it was dropped is now the thing to check by ear (item 20's note).
   The idea was to drive the unit's master attenuator (if it has one among
   its settings BF 41..4A, item 6) instead of scaling the samples
   (masteratt.c). Why it was dropped then: MASTER ATT. now also scales the cue
   bus, because the FLX10 blends our master stream into its headphones
   (item 20), and an attenuator in the unit would likely sit after that blend
   and break the balance. At 24 bits, -12 dB in the samples costs nothing
   audible. The probe that was staged on the unit is removed. Kept as a
   low-priority question in item 21.
18. **MY SETTINGS: a load applies -- checked** (2026-10-27). The operator
   loaded them on the decks; rbp's state was read back from its memory:
   - What rbp read: its load buffer (0x3251f2c) holds MYSETTING.DAT's body
     at +0x10..+0x2f, MYSETTING2.DAT's at +0x30..+0x3c and DJMMYSETTING.DAT's
     at +0x68..+0x77, byte for byte as on the stick. Each field is the value
     with bit 7 set to mean "present" (0x81 = 1).
   - What it applied, both decks (PlayEngine players, StatWatcher):
     quantize on (+0x12, StatWatcher deck struct +0x2e bit 6), master tempo on
     (+0x2b, Player+0xe9a), vinyl on (+0x29, Player+0xead), tempo range index
     2 (+0x2c, [Player+0xe94]+8: 0 = +-6, 1 = +-10, 2 = +-16, 3 = WIDE; deck 1
     was at +-6 before). The Beat FX quantize panel (MasterInnards+0x45) is on,
     as the DJM file says (+0x6c), but it was already on before the load.
   - Seen on rbp's screen by the operator: time mode REMAIN (+0x28) and
     CH FADER CURVE 2 (DJM +0x68 = 1; values are 0-based --
     UiKey_Shortcut_ChannelFaderCurve2 @0x115b68 sets 1). Mono split (+0x6b
     = 0) not checked; it and the curve live in StatWatcher's keyed settings
     store (getUtilChFaderCurve @0x2c22b8).
   - XdjSettings.dat was not rewritten by the load: rbp saves it later, not
     on a load.
   - The SHORTCUT view's LOAD (key 0x0222, UiKey_Shortcut_MySettingLoad
     @0x116820) loads only once a browse has opened a stick: it needs browse
     mode 10 and that view's media record (getLastBrowseModeMediaPointer(10)
     -> 0x326f9b8). A replayed BROWSE -> SHORTCUT -> LOAD after a restart did
     nothing for that reason.
19. **RB_AUDIO_PERIODS -- closed, kept at 4 on this unit** (2026-10-27).
   A deeper master ring (docs/09): 4 periods instead of rbp's 2, about 2.9 ms
   more latency, nothing else. Clicks were never actually heard, so it is not
   a fix for anything observed; it stays on because every level and balance
   in item 20 was set with it on. Set in the unit's rb.local.conf; the repo
   default stays 0.
20. **Headphones: cue vs master -- fixed** (2026-10-27, by ear). Every
   channel cue was too quiet and the master too loud. The FLX10 builds the
   headphone mix in hardware, and rbp was mixing too (docs/09, Cue / PFL).
   Now rbp sends a plain cue bus at -12.9 dB and MASTER ATT. scales the
   headphones as well. rbp's master stays at its +5 dB pin, and
   RB_AUDIO_MASTER_BOOST_DB=3.7 lifts master and cue after rbp: rbp's trim
   centre is -8.7 dB, so the pin alone left us 3.7 dB under rekordbox on the
   same unit (matched by ear with +3.71 dB of trim). Watch clips_m.
   The headphone pair: 3,2 is right for the FLX10 (measured, see the DDJ-FLX10
   section; the pairs memory carries it). Some starts came up 2,3 because three
   re-resolve paths dropped the memory -- the cold plug-in (rbp up before the
   unit's audio) re-laid the configured map, and a device change and the live
   map looked the memory up by card id instead of USB id. All three now use
   lay_base_pairs(g_dev_key). The cold path is not yet seen on the unit.
   **Changed 2026-10-28 -- check this first when the headphone mix is looked
   at again: the master is no longer attenuated in software; the FLX10 does
   it** (item 21). MASTER ATT. now sets the unit's own attenuator (BF 42), the
   master samples are not scaled, and the cue bus no longer takes MASTER ATT.
   (g_hp_att 0). The -12.9 dB cue level above was balanced with MASTER ATT.
   scaling both master and cue in the samples; where the unit's attenuator sits
   relative to its headphone blend is NOT measured, so with MASTER ATT. below
   0 dB the cue/master balance in the headphones may have moved.

21. **Hardware attenuators instead of software gain -- done for MASTER ATT.
   and BOOTH MONITOR ATT. 2026-10-28** (item 6: BF 42 / BF 43, set from rbp's
   UTILITY rows; the master samples are no longer scaled, and the cue bus no
   longer follows MASTER ATT. -- see the note at the end of item 20; NOT
   checked by ear yet). RB_AUDIO_MASTER_BOOST_DB is still in the samples.
   The original note (operator, 2026-10-27): Today every level stage after rbp is in
   the samples: MASTER ATT. (masteratt.c), RB_AUDIO_MASTER_BOOST_DB, and the
   cue bus following both (item 20). Worth finding out whether the FLX10 has
   attenuators of its own to use instead -- a master one among its settings
   BF 41..4A (item 6; the probe for that, item 17, was dropped), next to the
   booth one on ch 16 CC 13 -- and where each sits relative to its headphone
   blend, since anything after that blend would move the cue/master balance
   item 20 set. Send a setting only on a change, never repeated (the booth
   lesson).
22. **VIEW and BACK from the player -- done** (operator, 2026-10-27; map_flx10.c).
   On the player VIEW is rbp's BROWSE key, which reopens the browser where it was
   left (list and row: checked on the unit, Bea at row 006 and back); BACK there is
   SOURCE. Until a list has been browsed since rbp started (or after slot 1's stick
   went), rbp's BROWSE is an empty "Please select a source", so VIEW on the player is
   SOURCE then, as it used to be. test_flx10 has both.
23. **EQ knobs vs rekordbox on the laptop -- closed** (operator, 2026-10-28):
   they felt more aggressive at first; on a second listen they match.
24. **Switching a channel's input away from the RX3 leaves the deck playing
   -- looked into, needs the selector's message** (operator, 2026-10-28). Why: rbp
   mixes in software and the FLX10 plays back only four channels (master and
   headphones, item 6), so the unit cannot mute one deck itself. Its per-channel
   source selector (USB A / USB B / the external inputs) only tells the computer,
   as with rekordbox, which then drops that deck from its mix -- and map_flx10.c
   binds nothing to it (unmapped messages are logged only when verbose). The FLX10
   had left USB (04:07) when this was looked at, so its message could not be read.
   Next, with the FLX10 on: `aseqdump -p 20:0` (or RB_MIDI_DUMP) while flipping each
   channel's selector off USB B and back. Then the fix is one routing word per
   channel, no hook: rbp_abi.h's ADDR_MIXER_ROUTE_PLAYER0/1 point players 0/1 at mixer
   inputs 0/1 (MIXER_ROUTE_INPUT0/1, 0x01149f08 + 8 x input); pointed at a silent
   input (aux, input 4: audioshim answers rbp's capture with silence) the deck drops
   out of the mix, as the RX3's own DECK/LINE switch does, and back on USB B it is
   restored. The same move later carries the line input (item 13). Channels 3/4 wait
   for item 12.
25. **BEAT JUMP held repeats, to seek -- done** (operator, 2026-10-28; map_flx10.c).
   The press jumps once as before; held, the same jump repeats from 400 ms every
   150 ms until it comes up (~12x through a 128 BPM track with 4 beats, four times
   that with SHIFT's 16). Checked on the unit: held 2 s on a 170 BPM track, REMAIN
   4:08 -> 3:52, eleven 4-beat jumps. test_flx10 covers the timing.
26. **Touch scrolling is jerky -- fixed, by measurement; to feel at the glass**
   (operator, 2026-10-28, the "CXDIS" capacitive panel; touchscroll.c). Measured on
   the unit with touches injected through pointsrc (a drag of 8 rows at 1,000 px/s,
   samples every 8 ms) while sampling rbp's list position: the list moved 2-3 rows at
   a time every ~130 ms, because every sample passes rbp's
   TouchAdValueHysteresis::procAdaptValue @0x363a74, a resistive panel's filter that
   holds a moving finger inside a dead band, creeps it one unit every six samples and
   takes a big move only after four samples beyond it -- TouchListScrollProc's y
   changed every ~120 ms though samples came every 8 ms. Once a touch is under way
   (filter state 2) the sample now passes as it is (its first sample is still rbp's to
   drop); after, the list moves a row every ~50 ms, in step with the finger. Also, rbp
   STORED each drag's row difference (UiKey_BrowseTouchListScroll @0x121490) for
   Ui_EventTask's TouchScroll to make, so two row changes between its rounds lost the
   first; it is now added. A drag of 8 rows moves the list 7 (the first row change is
   the drag's own start) at ordinary speeds; very fast flicks (60+ rows a second) still
   drop one or two. Taps are unchanged (a row selects, the headings sort).
   For checks like this pointsrc reads touches from a FIFO while one exists at
   /run/rbp/touch.inject ("down x y" a line, logical 1280x800; none exists normally).
27. **No touch on the BEAT FX area -- done** (operator, 2026-10-28: "it is buggy and
   i don't like it"; pointsrc.c). With RB_POINT_FX_TOUCH=0 (this unit's rb.local.conf)
   the switch now also keeps rbp's own touch controls there from rbp: on the
   performance screen a press in the panel (x 1090..1269, y 57..362) or on its STATUS /
   BEAT FX toggle (y 432..481) goes nowhere, its release with it; ZOOM / GRID between
   them are left. Checked on the unit with injected taps: the toggle and the panel did
   nothing (logged "kept from rbp"), INFO elsewhere still toggled.
28. **The deck drawers -- removed** (operator, 2026-10-28: "kill that ... it's
   useless"). The swipe-in drawers at the screen's left and right edges (a
   volume/pitch fader, nudges, SYNC / MASTER / CUE / PLAY) are gone: side_zone.c,
   side_paint.c and test_side.c deleted; pointsrc.c no longer feeds them, sends
   their keys or reports the second touch contact (it only existed for the two
   faders); menu_draw.c no longer composites their planes; menu_zone.c's band
   no longer waits on them. A swipe at either edge reaches rbp as before the
   drawers. Left behind on purpose: RB_SIDE_NUDGE_PCT in rb.conf and start-rb.sh
   (dead now; removing it wants a RB_CONF_VERSION bump, and the unit's copies of
   both files carry another session's uncommitted edits), pitch_state.c (the maps
   still publish the tempo position), and comments in other modules and in
   docs/07-touch.md / docs/08-controls.md that name the drawers as history.
29. **SOURCE does not refresh when a USB stick goes in -- couldn't reproduce**
   (operator, 2026-10-28). The SOURCE screen kept showing the old state until
   something else redrew it. rbp already asks for the refresh: the mount message
   (0x138a) in `Total_MainUsbMessageProc` calls `Pub_GuiCom_SndDispInvalidate(0x1000)`
   when ModeSelect (`0x3253564+0x152c`; SOURCE is 7) is in mask 0x47fe. That sets
   flag 0x10000, and `Ui_EventTask` runs `BrowseUpdateList` for browse mode 7 or 11.
   A slot-2 mount sent by hand (the slot-1 stick bound to /media/usb2/sda1) appeared
   on SOURCE by itself, and a real plug worked on the retry.
30. **The sort should survive going back to SOURCE -- wished for** (operator,
   2026-10-28). A sort chosen in a list is lost when BROWSE goes back to SOURCE.
31. **Sort triangles on the headings -- done** (operator, 2026-10-28; browsesort.c,
   browserows.c). As on a CDJ-3000 (the operator's photos): every heading has a
   triangle just after its text, grey as the text (189,190,189); the heading the list
   is sorted by is drawn blue (0,125,231, rbp's selected row) with a blue triangle,
   pointing down when the sort runs downwards. A tap on a heading sorts by it (rbp's
   own sort, the one LIST MENU > SORT sends); a tap on the sorted one turns it round.
   - How: a tap posts a UI event of our own into rbp's BrowseComm mailbox (pool
     0x03268be0, mailbox 0x03267230, as SendMessage_ToBrowseComm), from the touch
     thread itself (get_mpf/snd_mbx are pthread primitives). On the database client
     task UiBrowseComm_IsUIMsgValid_ListReq @0x104edc sees it and asks
     Ui_BrowseComm_ReqMenuSelect @0x110c50 for item 0x7e00|sort, which reaches
     SelectRightMenu (browsemenu.c's hook hands it on) and SendSortedBrwsList. A
     direction: sort | 0x4000 (| 0x8000 down), kept per task across
     DBSMain_OnListClientCmd @0x153530 (which narrows the sort to a byte) and turned
     into the comparator's bit 7 in ListStructSortKeySet @0x171780 (LBGrp_SetSortType).
     rbp's own BPM sort runs up; RATING, TIME, BITRATE, PLAY COUNT and DATE ADDED
     run down (flags 0x84). Adapted from rx3-toolkit (MPL).
   - Drawing: rectangle colours are 5-6-5 in the colour word's low three bytes
     (red's 5 bits in the low byte, green's 6, blue's 5; each byte's top bits
     ignored -- probed bit by bit), drawn filled (byte 0x31 = 1) with the outline
     pass off (byte 0x24). Headings are RGB16 bitmaps (image table [0x05a14f60], 44
     bytes an id; format 2) with the grey text baked onto (49,48,49) and magenta as
     key, and rbp cannot tint them (the image model's word 0x48 is the key), so the
     sorted heading is a blue copy of its bitmap made once, answered under a private
     id from a hook on NS_GetImageInfoByID @0x1d192c.
   - The # column stays (it used to go when rbp filled it with dashes) and, in a
     playlist sorted any way but its own, shows each track's entry in the playlist
     (operator: a CDJ-3000 keeps them), read from the stick's export.pdb table 8
     (entry, track, playlist) on a thread of browsecol.c's own; the playlist is
     getRowset_Playlist's last id (0x0243b574), a playlist's track list is db+12
     0x11xx. Four digits are drawn right-aligned in a wider rectangle (rbp drops
     what does not fit).
   - Checked on the unit with a test hook (/run/rbp/browse.tap, "x y": a tap there):
     BPM up 153 -> 155 and down 230 first; ARTIST up and down; LIST MENU > SORT >
     KEY lights KEY the same way a tap does.
32. **PREVIEW, the CDJ-3000's button -- done** (operator, 2026-10-28; browserows.c,
   browsesort.c). A PREVIEW button on BROWSE's title bar (x 840..956, between the
   list's name and the clock) while a track list is up: rbp's blue with white text
   while the waveforms show, dark grey while they are off; a tap turns them off or on
   and writes /run/rbp/browse.preview (on|off), read with the column's file.
   rbp draws each row's waveform (ui_ListBrowse_DrawPreviewWave @0x2980b8) and artwork
   (ui_ListBrowse_DrawJpegData @0x298c24) into DirectFB windows of their own, placed
   from one table at 0x02684610, 0x1c bytes a row (waveform handle +0xf8, x +0x100 =
   110; artwork handle +0x66c, x +0x670 = 327). With PREVIEW off the waveform windows
   are destroyed as rbp's own row without one is (ui_com_draw_DestroyWindow on the
   handle's address), the artwork's are made again 217 px left, and whatever the list
   draws from the artwork to the title's start (#, the note icon, TRACK's heading and
   triangle) moves left with it, the title keeping its right edge -- a layer under all
   of browserows.c's drawing. The switch redraws at once through
   Pub_GuiCom_SndDispInvalidate(0x1000) (BrowseUpdateList). Checked on the unit, both
   ways, and the button is absent on folder lists.

How rbp sorts a browse list (read 2026-10-27, for whoever needs it next; a
default playlist sort was built on it and dropped -- the operator loads the sort
from the stick's MY SETTINGS instead):
- The database task gets a list with djepl_getPlaylistTracks @0x164aa0 (and its
  djeplGet* siblings, from DBSMain_OnListClientCmd). Argument 1 packs the
  condition; its byte 2 is the SORT as a 1-based position, mapped by
  ConditionConvert @0x17a478 through a fixed table (1 -> 0, 2 -> 1, 3 -> 20,
  4 -> 21, 5 -> 4, 6 -> 6, 7 -> 5, 8 -> 2): 2 = the list's own order, 1 = sorted
  by the column. Argument 3 is the COLUMN, rbp's category id -- 0 as a playlist
  opens (shown as ARTIST), 4 for BPM (MODULEMAIN_CATESORT_BPM). SORT > BPM sends
  (1, 4) and sorts lowest first; (1, 0) sorts by title. Argument 2 is the list.
- The ids are bytes filled at start (MODULEMAIN_CATESORT_*: DEFAULT 0, ALPHABET
  1, ARTIST 2, ALBUM 3, BPM 4, RATING 5, GENRE 6, COMMENT 7, LENGTH 8, LABEL 10,
  KEY 12, BITRATE 13, CUE 14, COLOR 15, PLAYCOUNT 16); the SORT menu is the
  stick's own (dbcl_GetRightMenu) -- here BPM, DEFAULT, BPM (UNSORTED),
  ALPHABET, ARTIST, ALBUM, RATING.
- rbp's popups (LIST MENU, its SORT, MY SETTINGS) ignore a selector press and
  release sent in the same instant unless the knob has turned in them (a push
  straight after MENU fell through to the list and opened the TRACK MENU); a
  press held ~50 ms is taken either way, so the FLX10's push holds it.

Done 2026-10-26 (start-up splash, rbp's TRANS picture):
- For RB_SPLASH_MS (8000) after rbp's first frame the screen shows a picture on
  black (RB_SPLASH_IMAGE, a PNG, or rbp's own white rekordbox logo from
  imagedata.dat), the FLX10's LEDs are held and its input dropped; then the unit
  is asked for its controls again. rbp draws into a private buffer meanwhile:
  painting over its page was tried and failed, because rbp reads its page back
  (the splash came back out of rbp's own buffers). fbshim answers rbp's mmap of
  /dev/fb0 with anonymous memory, follows DirectFB's unmap and re-map at start,
  and at the end copies rbp's page across and maps the framebuffer back over
  rbp's address (splash.c). The empty performance screen's big rekordbox logo
  is rbp's own, not the splash's.
- rbassets.py (start-rb.sh, before the launch) writes the splash picture to
  /run/rbp and rebuilds rbp's two TRANS pictures in imagedata.dat (records
  1059/1060, RGB565, 0xF81F transparent) as TRANNY from rbp's own letters --
  T R A N from TRANS, Y from VINYL BRAKE -- and the BEAT FX window's four
  160x40 name tiles (5225..5228: gray, white, faded, blue; opaque, so the text
  is cleared to the tile's own margin column) with DELAY's Y, plus any
  byte-identical copy of either. Seen on the unit's BEAT FX window 2026-10-26.
  RB_FX_TRANNY=0 leaves them alone. The 132x38 tiles of rbp's touch FX list
  (5570..5572) are not patched: patterned backgrounds and merged letters.

Done and confirmed 2026-10-26 (rbp's UTILITY menu):
- HDMI AUDIO is a row of rbp's own UTILITY menu, after MIXER MODE
  (utility_rows.c, RB_UTILITY_ROWS=1): the shim copies rbp's row table with
  the row added and points rbp's seven literal words at the copy, after
  checking each holds what was measured. The row borrows SLIP FLASHING's
  OFF/ON and rbp's generic handlers; its SetLine and EnterMenu read and write
  /run/rbp/mirror.mode.
- SHIFT + VIEW held 1 s holds MENU 500 ms, rbp's own way into UTILITY; a
  short press is still SHORTCUT. The drawn settings panel is no longer raised
  from the controller.
- The FX screen calls TRANS "TRANNY" (the operator's own flavour).
- With RB_SETTINGS_DJM=1 / RB_SETTINGS_VNC=1, UTILITY also has a DJM section
  (MIXER ROUTE, DECK 1 PAIR, DECK 2 PAIR) and a VNC section (SHARE, ENCODE,
  POINTER INJECT) after HDMI AUDIO, each a blank row and a heading (rbp's own
  Util_SetLine_Blank / Util_SetLine_MixerCategory) like rbp's sections. The
  rows read the drawn panel's state and write its switch files
  (pointsrc_settings_state / pointsrc_settings_write); MIXER ROUTE greys with
  no mixer on the bus, and a deck refuses the other deck's pair. Read back from
  rbp's live table on the unit; not yet seen on the glass.
How the rows work, for the next row:
- The rows are `stUtilityObjects` @0x5140bc in .data, 33 entries of 56
  bytes, with their count `ulUtilityItemNum` @0x5140b8 just before them
  (DECK 0-13, blank, MIXER 15-23, blank, GENERAL 25-32; labels are UTF-16
  with six leading spaces under a category). An entry: +0 label, +4 value
  strings, +8 value count, +12 current value, +16 value being edited (-1
  none), +20 editable, then eight handlers: +24 InitItem, +28 SetLine(entry,
  lines[2]) -- fills each line's +32 length and +34 UTF-16 text, reading the
  live setting into +12 when nothing is being edited -- +32 StartEdit, +36
  SelMenu, +40 IsModified, +44 EnterMenu (applies +16 through the row's
  setter, answers 1 if it changed), +48 IsGrey, +52 Reset.
  Util_SetLine_SlipFlashing / Util_EnterMenu_SlipFlashing are the model.
- The table is exactly full (33 x 56 = the gap to the next symbol), so a row
  means moving it: rbp reaches it ONLY through seven literal-pool words --
  0x5140b8 at 0x13c9a8, 0x13cbcc, 0x13cd1c, 0x13cf30, 0x13cfc4, 0x13d9ec and
  0x5140bc at 0x13d9e4 -- no movw/movt pair and no anchored base reaches
  0x5140a0..0x5147f4. Everything is driven by the count (InitUtilityItem's
  loop, the list's item count in SetDispUtilityList, StartEdit's bound);
  the only row-number reader (CursorUtilitySet) is the value cursor. So: a
  bigger copy in the shim (count + rows, ours inserted where wanted, e.g.
  after MIXER MODE), the seven words rewritten at load under mprotect (the
  meter hook in rbp_bridge.c already patches rbp's text this way), and the
  new rows' handlers in the shim -- ON/OFF strings can be SLIP FLASHING's.
- UTILITY opens on K_MENU held ~500 ms (rbp's own timer, docs/08).

Done 2026-10-26 (FLX10 FX screen):
- In rekordbox's host mode the unit draws its FX screen only from three SysEx
  (decoded from the operator's capture_fx): 0a the name, ASCII packed 8 bits
  a character into the 7-bit bytes from bit 1 of its field; 0b the beat text,
  the same packing from bit 0; 0c the BPM x100 in hex digits and a TAP flag.
  map_flx10.c's flx10_fxscreen_tick sends rbp's effect name, its beat (the
  rung through getBeatFxNumDen's table, or the percent at +0x30 for REVERB
  and PITCH) and the Beat FX BPM (BpmManager +0x1c / +0x10).
- The effect selector matches the FLX10's labels by name (MT DELAY is DELAY)
  and fills the other five from the RX3 dial's closest detent: LOW CUT ECHO
  PING PONG, ENIGMA JET FILTER, STRETCH PITCH, MOBIUS saw VINYL BRAKE,
  MOBIUS triangle HELIX.

Done 2026-10-26 (settings panel):
- HDMI AUDIO row: ON/OFF, written to /run/rbp/mirror.mode
  (RB_AUDIO_MIRROR_FILE); audioshim's mirror_switch_tick closes the mirror on
  `off` and reopens it at once on anything else. Absent means on, and /run
  means a reboot brings it back.
- The DJM rows (MIXER ROUTE, DECK 1/2 PAIR) show only with RB_SETTINGS_DJM=1,
  the VNC rows (SHARE, ENCODE, POINTER INJECT) only with RB_SETTINGS_VNC=1;
  the box is as tall as the rows shown. Hidden is not disabled: the switch
  files and RB_MIXER_MODE work as before.

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
and holds % / key while jogged; BEAT SYNC range fit (follows the sync, floored at the deck's own range); BEAT SYNC
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

* **HDMI audio mirror**: done -- open item 1 above.
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
  until then. What exists: the Pi's boot screen (`rbp-boot`, docs/11 "The
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

Selected with `RB_MIDI_MAP=flx10` in `/opt/rbp/rb.local.conf`. That also
points audio at the `DDJFLX10` card through the controller table's new `flx10`
row. The headphone pair needs the per-device memory line above. The bindings
are the operator's table, agreed control by control on 2026-10-24:

| FLX10 | RX3 |
|---|---|
| PLAY, CUE / SHIFT + CUE | PLAY, CUE / back to the start: a seek straight to the auto cue point (the main cue when auto cue set it, else the top), play/pause kept, nothing repeats while held |
| BEAT SYNC / SHIFT / held | SYNC / MASTER (tempo master) / **INST.DOUBLES** (the RX3 manual p. 74): the FLX10 sends note 120 for a hold, and rbp gets its own BEAT SYNC held 1.1 s; the FLX10's lone SYNC release after the hold is dropped. A copy of a paused deck is paused and put at the original's position (rbp starts it, from the top) -- and muted in rbp's audio path (keyshift_mute) from just before rbp's long press loads it until then, so it is never heard |
| KEY SYNC / held | nothing (rbp has no key sync) / MASTER TEMPO; SHIFT nothing |
| TEMPO RESET / SHIFT | toggle: tempo held at 0.0%, fader ignored; off = back to the fader (LED lit while on) / TEMPO RANGE -- on a deck synced to a master, never to a range too narrow for the sync: round past it to the next that holds, or nothing (no WIDE -> +-6 flash) |
| (no control) BEAT SYNC tempo range | SYNC on widens the range first if the sync needs it. While synced and following, the range follows the need both ways -- master sped up, wider; slowed down, narrower -- never below the range the deck started on or was set to by hand since. The need is the master's playing BPM over the track's own (rbp's bpm_orig: the tempo % of a synced deck is its fader's, not the speed) |
| TEMPO fader, jog (top press = touch, top spin, side ring) | the same |
| SHIFT + 4 BEAT (loop section) | flip the deck's elapsed / remaining time (rbp's TIME MODE, `UiSetKeyTimeMode`; the jog screen follows) |
| 4 BEAT JUMP < > / SHIFT | beat jump -4 / +4 beats / -16 / +16 beats (`DjEngineIF::playBeatJump`, paused or playing, one jump each; held, repeated every 150 ms from 400 ms, to seek) |
| SLIP REVERSE / SHIFT | momentary reverse that always slips / latching reverse that never slips (LED lit while held or reversing) |
| QUANTIZE; SLIP / SHIFT | QUANTIZE; SLIP / nothing (VINYL disabled: no light on the button) |
| CUE/LOOP CALL < > and SHIFT | not mapped |
| MIX POINT SELECT < > | < = as SHIFT + CUE, or the previous track when already within 1.5 s of the start / > = next track (TRACK SEARCH tapped 100 ms, no repeat while held; < stamped as rbp's "second press") |
| LINK (deck 1 / deck 2) | that slot's USB stick (USB1 / USB2): lit while rbp has it mounted and ready; held, a slow blink for 1 s then twice as fast for 1 s, and at 2 s rbp's USB STOP for the slot, still blinking until the stick has left rbp's mount table, then dark. Let go before 2 s: no eject |
| SHIFT + MIX POINT, ACTIVE PART, PAGE, DECK SELECT, SAMPLER, BOOTH | nothing |
| browse knobs (one shared message) / SHIFT | scroll / 8 steps per detent |
| browse push (per deck) / held 0.5 s / twice | on a track row LOAD that deck, on anything else SELECT (opens the folder/playlist/category) -- the row type is rbp's own `UiBrowseComm_IsTrackList` (@0x105e60) / **rbp's own long press of the selector = FAST SCROLL** (selector PRESS + op 1, held until the knob comes up; TAG TRACK is unmapped) / on the PLAYER, two pushes within 400 ms: **INST.DOUBLES**, as BEAT SYNC held (a single push on the player is held back 400 ms, then sent as before); in BROWSE a push is a push; **on the SOURCE screen with a stick in slot 1: open USB 1** (rbp ignores the knob there) |
| BACK / VIEW; SHIFT+BACK / SHIFT+VIEW | BACK, SOURCE on the player and on the category column / BROWSE from the player (the last place browsed; SOURCE until a list has been), SOURCE again on the SOURCE menu (closes it), BROWSE (back to the player) from a browse list; PLAYLIST from the player, back to the player when pressed again from that list, TAG LIST elsewhere / SHORTCUT (on the release); held 1 s, rbp's UTILITY (MENU held 500 ms), where the browse knob, its push and BACK are rbp's own |
| SHIFT + deck 1's browse push | TRACK FILTER (deck 2's SHIFT push not yet recorded) |
| pad modes HOT CUE / PAD FX1 / BEAT JUMP / SAMPLER | HOT CUE / SLIP LOOP / BEAT JUMP / BEAT LOOP 2 |
| SHIFT + those: KEYBOARD / PAD FX2 / BEAT LOOP / KEY SHIFT | GATE CUE / RELEASE FX / BEAT LOOP / BEAT JUMP 2 (a SHIFT button goes straight to its mode; pressing a main button again does what SHIFT + it does, and a third press comes back -- the FLX10's double-press behaviour) |
| pads; SHIFT + pad in HOT CUE | pads in rbp's mode; delete that cue |
| ch 1/2 TRIM, EQ, COLOR, FADER, CUE; master CUE, crossfader, HP mix/level | the same |
| (no control) MASTER follows the faders | rbp moves the sync master from deck 1 to deck 2 the moment deck 1 goes off air, never back; the shim does deck 2 to deck 1 the same way (ON AIR from fader or crossfader, the other deck playing on air), and undoes either move when the deck left is back on air within 500 ms -- a fader cut moves nothing |
| crossfader assign A / THRU / B (deck notes 22 / 29 / 24) | rbp's crossfader assign for that channel (`DjEngineIF::setCrossFaderAssign`; the RX3 only sets it from its curve switch, so it sat at THRU) |
| MASTER LEVEL | the unit's own volume; feeds only the HDMI mirror (as on the FLX4), which starts silent -- the knob is not reported until it moves |
| SCFX SPACE, D.ECHO, CRUSH, NOISE, FILTER / PITCH | the same names / SWEEP. SPACE is 0x50a1 and CRUSH 0x50a5: rbp_abi.h had them swapped, corrected from the unit (2026-10-24) |
| SAMPLER VOL | SCFX PARAMETER (0x50a7, channels 1 and 2) |
| BEAT < > / SHIFT; LEVEL/DEPTH / SHIFT; ON/OFF | BEAT < > / AUTO (BPM back to auto) and TAP; DEPTH / TIME; Beat FX ON/OFF -- the FLX10 latches the button itself (7f flashing, 00 not, one message a push), so the velocity is the state: rbp's BFX key is tapped only when rbp differs, and the startup controls query sets rbp from the button |
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
| time | CC 0x42+2d min, 0x43+2d sec | 0..99 -- **not sent** (2026-10-27): the same numbers are the unit's own utility settings (`BF 41..4A`, what it reports to `9F 70 00`), and the remaining time written into them flipped the booth output between two levels at fixed points in each track (louder at 0:25 in a 4:25 track, as the seconds wrapped). rekordbox never sends them; the jog's time comes from the HID state packet's position |
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
