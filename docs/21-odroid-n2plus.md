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
**current** kernel (6.18.x at the time of writing). Flash it to microSD or eMMC,
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
* **Audio.** The board's own ALSA card is `ODROIDN2` (`axg-sound-card`), which is
  the candidate for the HDMI mirror later. `/dev/snd/seq` is present without a
  modprobe.

## Open questions

* Whether meson's HDMI PHY will clock 1280×800@60 (71 MHz, CVT-RB). O4 answers it.
* fb0's default depth under meson-drm. O5 answers it.
* Which `cardN` meson-drm gets when panfrost is loaded. `drm_card.h` makes this a
  non-question, but it is worth recording.
* The board's HDMI audio card name and formats, for the mirror.
