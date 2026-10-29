#!/usr/bin/env python3
"""
hcdpatch.py - point the ROM startup resident at a different host controller.

romstartup/usbromstart.c carries the HCD names in fixed-size fields tagged with their
own cookie (ROMSTART_HCD_COOKIE).  This overwrites those fields, so a Kickstart image
can embed a controller other than the one the distribution was compiled for without
rebuilding the whole tree.

Usage:  scripts/hcdpatch.py <in-module> <out-module> <device-name> [<bcm2837-device-name>]

<device-name> is the default host controller; "-" keeps the one already in the module.
<bcm2837-device-name>, if given, is the controller the resident adds instead on a board
whose devicetree root lists "brcm,bcm2837" (Raspberry Pi 3 family); "-" keeps it, and
an empty string clears it (every board then gets the default).
"""

import sys

# Must match ROMSTART_HCD_COOKIE and the romstartHcd fields in
# romstartup/usbromstart.c.  The trailing \x02 is the layout version: bump it there
# and here together if the fields ever move or change size, so an old script cannot
# silently mis-patch a new module.  Layout 1 had only the default name; this script
# refuses such a module, because writing the second field there would overwrite the
# code after the slot.
COOKIE = b"PSDHCD\x02\x00"
NAME_LEN = 32
FIELDS = ("default", "bcm2837")         # in slot order, NAME_LEN bytes each


def die(msg):
    sys.exit(f"hcdpatch: {msg}")


def encode(name, what, allow_empty):
    encoded = name.encode("latin-1", errors="strict")
    if not encoded and not allow_empty:
        die(f"{what} device name is empty")
    if len(encoded) + 1 > NAME_LEN:
        die(f"{what} device name {name!r} is {len(encoded)} bytes; the slot holds "
            f"{NAME_LEN - 1} plus a NUL")
    # Whole field, NUL-padded: never leave a tail of the previous name behind.
    return encoded + b"\x00" * (NAME_LEN - len(encoded))


def main():
    if len(sys.argv) not in (4, 5):
        die("usage: hcdpatch.py <in-module> <out-module> <device-name> "
            "[<bcm2837-device-name>]")

    src, dst = sys.argv[1], sys.argv[2]
    names = sys.argv[3:] + ["-"] * (5 - len(sys.argv))

    buf = bytearray(open(src, "rb").read())
    off = buf.find(COOKIE)
    if off < 0:
        old = b"PSDHCD\x01\x00" in buf
        die(f"cookie {COOKIE!r} not found in {src} - "
            + ("it carries the layout-1 slot (default name only); use the hcdpatch.py "
               "of its own source tree" if old else
               "is this the ROM startup resident, and was it built from a source "
               "carrying the patchable slot?"))
    if buf.find(COOKIE, off + 1) >= 0:
        die(f"cookie {COOKIE!r} occurs more than once in {src}; refusing to guess")
    if off + len(COOKIE) + NAME_LEN * len(FIELDS) > len(buf):
        die(f"slot at 0x{off:x} runs past the end of {src}")

    for i, (what, name) in enumerate(zip(FIELDS, names)):
        if name == "-":
            continue
        field = off + len(COOKIE) + i * NAME_LEN
        buf[field:field + NAME_LEN] = encode(name, what, allow_empty=(what != "default"))
        print(f"   patched {what} HCD name -> {name or '(none)'} (slot at 0x{field:x})")

    open(dst, "wb").write(buf)


if __name__ == "__main__":
    main()
