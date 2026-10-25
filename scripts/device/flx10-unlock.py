#!/usr/bin/env python3
"""flx10-unlock.py -- send the DDJ-FLX10 the vendor "unlock" rekordbox sends.

Until it gets these seven vendor control requests the FLX10 shows NO AUDIO
DRIVER on both jog displays and ignores the MIDI display channel (ch 16).
They are what rekordbox / Serato / VirtualDJ send at connect (community USB
captures, Veezuhz/Mixxx_FLX10_Controller_Mapping, docs/21):

    bmRequestType 0x40 (vendor, host->device, recipient device), bRequest 3,
    wLength 0, (wValue, wIndex) =
      (0x0100,0xC028) (0x0000,0xC029) (0x0200,0xC013) (0x0000,0xC02B)
      (0x0100,0xC026) (0x0000,0xC01D) (0x0100,0xC027)

Standard library only: USBDEVFS_CONTROL on the device node, which needs no
interface claim for a request addressed to the device, so snd-usb-audio and
the MIDI driver keep their interfaces. Run as root.

    python3 flx10-unlock.py            # unlock; exit 0 sent, 1 failed, 2 absent
    python3 flx10-unlock.py --rebind   # also unbind/rebind snd-usb-audio after
"""
import ctypes
import fcntl
import glob
import os
import sys
import time

VID, PID = 0x2B73, 0x0041
CMDS = [(0x0100, 0xC028), (0x0000, 0xC029), (0x0200, 0xC013), (0x0000, 0xC02B),
        (0x0100, 0xC026), (0x0000, 0xC01D), (0x0100, 0xC027)]


class CtrlTransfer(ctypes.Structure):          # struct usbdevfs_ctrltransfer
    _fields_ = [("bRequestType", ctypes.c_uint8), ("bRequest", ctypes.c_uint8),
                ("wValue", ctypes.c_uint16), ("wIndex", ctypes.c_uint16),
                ("wLength", ctypes.c_uint16), ("timeout", ctypes.c_uint32),
                ("data", ctypes.c_void_p)]


# _IOWR('U', 0, struct usbdevfs_ctrltransfer)
USBDEVFS_CONTROL = (3 << 30) | (ctypes.sizeof(CtrlTransfer) << 16) | (ord("U") << 8) | 0


def find():
    for vf in glob.glob("/sys/bus/usb/devices/*/idVendor"):
        d = os.path.dirname(vf)
        try:
            if (int(open(vf).read(), 16) == VID and
                    int(open(d + "/idProduct").read(), 16) == PID):
                bus = int(open(d + "/busnum").read())
                dev = int(open(d + "/devnum").read())
                return d, "/dev/bus/usb/%03d/%03d" % (bus, dev)
        except (OSError, ValueError):
            continue
    return None, None


def rebind(sysdir):
    intfs = [os.path.basename(i) for i in sorted(glob.glob(sysdir + "/*:*"))
             if os.path.islink(i + "/driver") and
             os.path.basename(os.readlink(i + "/driver")) == "snd-usb-audio"]
    for action in ("unbind", "bind"):
        for i in intfs:
            try:
                with open("/sys/bus/usb/drivers/snd-usb-audio/" + action, "w") as f:
                    f.write(i)
            except OSError as e:
                print("flx10-unlock: %s %s: %s" % (action, i, e))
        time.sleep(0.3)
    print("flx10-unlock: snd-usb-audio rebound on %s" % (", ".join(intfs) or "nothing"))


def main():
    sysdir, node = find()
    if not node:
        print("flx10-unlock: no DDJ-FLX10 (2b73:0041) on the bus")
        return 2
    fails = 0
    fd = os.open(node, os.O_RDWR)
    try:
        for i, (val, idx) in enumerate(CMDS, 1):
            ct = CtrlTransfer(0x40, 3, val, idx, 0, 500, None)
            try:
                fcntl.ioctl(fd, USBDEVFS_CONTROL, ct)
                res = "ok"
            except OSError as e:
                res = "FAILED (%s)" % e
                fails += 1
            print("flx10-unlock: [%d/7] wValue=0x%04X wIndex=0x%04X %s" % (i, val, idx, res))
            time.sleep(0.005)
    finally:
        os.close(fd)
    if "--rebind" in sys.argv:
        time.sleep(0.5)
        rebind(sysdir)
    print("flx10-unlock: %s (%s)" % ("done" if not fails else "%d of 7 failed" % fails, node))
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
