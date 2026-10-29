# rb-odroid

Credit to: [![Instagram: @i.erhan.es](https://img.shields.io/badge/Instagram-%40i.erhan.es-E4405F?logo=instagram&logoColor=white)](https://instagram.com/i.erhan.es) for the original base of this code.

**Run the Pioneer DJ XDJ-RX3's *rekordbox* player on an ODROID-N2+, played from a
DDJ-FLX10.**

rb-odroid runs the ARM32 `rekordbox` player application (`rbp`, called `rb`
internally) extracted from **XDJ-RX3 firmware v1.20**, unmodified apart from a
small patch table, inside a soft-float chroot. A set of `LD_PRELOAD` shims connect
it to the board's display, touch, USB and audio, and to a controller in place of
the RX3's own panel.

* **The target now:** a **Hardkernel ODROID-N2+** on **Armbian** (64-bit kernel,
  32-bit chroot), an HDMI touch monitor at 1280×800, and a **Pioneer DDJ-FLX10**
  for controls, jog screens, LEDs and audio. This is in daily use.
* **The original target:** a **Raspberry Pi 4B** on Pi OS with a **DDJ-FLX4**. It is
  this fork's starting point ([rbpi4b](https://github.com/salesforcedoc/rbpi4b)) and is
  still selected by the same tree (`RB_BOARD`, read from the device tree), but the
  work since has been on the ODROID and the FLX10.

> This is an **interoperability / preservation** project. It contains **no
> Pioneer/AlphaTheta firmware, no `rbp` binary and no rekordbox content.** You
> supply your own extracted assets. See [NOTICE.md](NOTICE.md).

The full record of the ODROID and FLX10 work (what was measured on the unit, every
address in `rbp` the shims rely on, and what is still open) is
**[docs/21](docs/21-odroid-n2plus.md)**.

---

## What works

On the ODROID-N2+ with the DDJ-FLX10, checked on the unit:

| Area | What you get |
|---|---|
| Display | 1280×800 at 32 bpp over HDMI. rbp's layer is the framebuffer page itself, with no conversion pass. rbp's renderer runs at about a third of its stock cost |
| Touch | an HDMI touch monitor (or a mouse) drives rbp's touch screen, including smooth list scrolling and the touch preview |
| Controls | the whole FLX10 surface mapped to the RX3: transport, jogs (touch, scratch, vinyl), tempo, SYNC/MASTER, all eight pad pages, BEAT JUMP, loops, SLIP / REVERSE, the mixer, Beat FX, BROWSE / LOAD / VIEW / BACK. Bindings were recorded from the hardware (`scripts/odroid/midi-learn.py`), as Pioneer publishes no FLX10 MIDI list |
| Jog screens | each jog's display shows the deck's artwork, waveform, position, hot cues, BPM, key and time, sent over the FLX10's HID channel |
| LEDs | buttons and pads follow rbp's state, with hot cue colours on the pads and the channel VU meters |
| Audio | master and headphones on the FLX10's USB audio, booth from its own master mix, and the same master on the ODROID's HDMI. MASTER ATT. uses the FLX10's own attenuator |
| Key sync | the FLX10's KEY SYNC, which the RX3 lacks, as a whole-semitone shift matched to the master deck (`keyshift.c`) |
| Browse | a third column (ARTIST, BPM, KEY and more), sort arrows on the headings, accent-insensitive search, album art in the list, the CDJ-3000-style PREVIEW button |
| Remote | rbp's screen over VNC (also in a browser), and a status and settings page on the LAN (`http://<unit>:5904/`) |
| Boot | a boot splash, autostart under systemd, and a health log |

---

## Setting up the ODROID

The full procedure, with each bring-up check, is [docs/21](docs/21-odroid-n2plus.md).
In short:

1. **Flash Armbian** for the ODROID N2 / N2+ (Debian trixie Minimal, *current*
   kernel), set the boot switch to MMC, finish first-login, then
   `apt-get install -y gcc make python3 alsa-utils`.
2. **Extract the XDJ-RX3 v1.20 firmware** into `extracted/` in the repo
   ([docs/04](docs/04-firmware-assets.md)). The version matters: every address in
   the shims is for v1.20.
3. **Build on the board**, in a Debian bookworm chroot the script makes the first
   time:
   ```sh
   fsck.cramfs --extract=extracted/XDJRX3-rootfs extracted/XDJRX3/images/rootfs.cramfs
   sh scripts/odroid/build-on-odroid.sh        # -> work/rb-odroid.tgz
   sh scripts/device/install.sh work/rb-odroid.tgz
   ```
   `install.sh` applies the boot settings (1280×800 at 32 bpp), installs the
   services and starts the player. Reboot once for the display mode.
4. **Select the FLX10** in `/opt/rbp/rb.local.conf` (the unit's own settings,
   which a redeploy never overwrites):
   ```sh
   export RB_MIDI_MAP=flx10
   export RB_AUDIO_PAIRS_MEMORY='2b73:0041=headphones=3,2'
   ```

After a change to the tree, `scripts/odroid/redeploy.sh` pulls, rebuilds, installs
and restarts in one go (`--no-pull` builds what is checked out). Each step's output
is kept in `/tmp/redeploy-*.log`.

For the Raspberry Pi 4B, see **[TUTORIAL.md](TUTORIAL.md)** and
[docs/13](docs/13-raspberrypi4.md).

---

## How it fits together

```
        XDJ-RX3 v1.20 assets (extracted by you)
                              │
       ┌──────────────────────┴───────────────────────┐
       │ pdj/rbp  (ARM32, soft-float)                 │
       │ + a small patch table                        │  tools/patch-rbp
       └──────────────────────┬───────────────────────┘
                              │
   ODROID-N2+ (Armbian) ── soft-float glibc-2.13 chroot (/opt/rbp/rbx3-run) ── rbp
        │
        ├── display  : rebuilt DirectFB fbdev module (the present path)
        ├── touch    : fbshim.so    (evdev touch/pointer → the RX3's tsc2007 protocol)
        ├── controls : knobshim.so  (FLX10 MIDI + HID → rbp's keys and engine;
        │                            LEDs, jog screens, key shift)
        ├── audio    : audioshim.so (rbp's ALSA output → the FLX10 and HDMI)
        ├── network  : netshim.so   (Pro DJ Link interface, see docs/18)
        ├── usb      : usb-watch.sh + DeviceSQL import of the stick's export.pdb
        └── services : rbp.service, display-watch, healthwatch, VNC, config page
```

There is no emulation: the real `rbp` from the firmware runs directly. The shims
interpose on its library calls and hook a few of its functions, addresses taken
from the v1.20 binary.

---

## Still open

* **Pro DJ Link** is built but stays down: nothing in rbp asks the network to come
  up ([docs/18](docs/18-prodjlink.md)).
* **KEY SYNC** is built and waiting to be confirmed by ear on the unit.
* **The FLX10's decks 3/4** have nothing to control: the RX3 is a two-deck player.

The numbered list in [docs/21](docs/21-odroid-n2plus.md) ("To revisit") has each
item's status and details.

---

## Repository layout

```
rb-odroid/
├── README.md                 you are here
├── TUTORIAL.md               build → deploy → run on the Raspberry Pi 4B
├── NOTICE.md, LICENSE        legal notes; MIT for our code
├── docs/                     findings and subsystem docs (docs/README.md)
│   ├── 21-odroid-n2plus.md   the ODROID-N2+ and DDJ-FLX10: the main record
│   └── 13-raspberrypi4.md    the Raspberry Pi 4B target
├── scripts/
│   ├── build-chroot.sh       assemble the soft-float chroot tarball
│   ├── odroid/               build-on-odroid.sh, redeploy.sh, MIDI-learn and HID probes
│   ├── device/               scripts and services that run on the unit (install.sh, rb.conf, ...)
│   └── shims/                the LD_PRELOAD shims, their maps and their tests
├── tools/
│   ├── patch-rbp/            the rbp patch table
│   ├── build-directfb/       the DirectFB 1.4.16 patch and build notes
│   └── ...                   diagnostics and generators
└── work/                     build output and scratch (gitignored)
```

## What is *not* in this repo

* any firmware image, `.UPD`, `.iso` or `rbp` executable,
* rekordbox databases or music,
* any firmware key or decryption tooling,
* built binaries of the shims (build them from source).

## Related projects

This tree started as [rbpi4b](https://github.com/salesforcedoc/rbpi4b), the
Raspberry Pi 4B port, which itself came from a Denon SC Live 4 port and the Prime
GO work before it. Firmware acquisition and decryption are out of scope here; the
**PrimeBox** project (the Prime GO port) covers that. **rx3-toolkit** is the source
of the key shifter and the track-finding ideas (credited where used).

## Credits

* Pioneer DJ / AlphaTheta: the XDJ-RX3, the DDJ-FLX10 and the XDJ-RX3's GPL source
  distribution.
* Hardkernel (ODROID-N2+), the Armbian project, and Raspberry Pi Ltd.
* Denon DJ / inMusic: the SC Live 4, the earlier port's starting point.
* DirectFB, JUCE, ALSA and glibc maintainers.

See [NOTICE.md](NOTICE.md) for licensing details.
