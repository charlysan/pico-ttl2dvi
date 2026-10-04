#!/usr/bin/env python3
"""Turn an IR key map (from irlearn.py) into a UF2 that installs it.

Usage:
    python3 irmap2uf2.py ir.txt [-o irmap.uf2]

The firmware reads the map as plain text from the flash sector before the
settings one (0x10FFE000 on the 16 MB board) at boot. This writes the text,
NUL-terminated, to that sector and nothing else: the firmware and the
settings slots are untouched. Install it either way:

    drag irmap.uf2 onto the RP2350 drive (hold BOOTSEL while resetting)
    picotool load -f irmap.uf2 && picotool reboot

Then `ir map` on the console lists what was loaded.

Lines are checked with the firmware's rules, so a typo is caught here rather
than silently ignored at boot: `CODE  r|-  action`, CODE 8 hex digits,
action one of @prev @next @enter @back (@up/@down are old names for
@prev/@next) or a console command. '#' starts a comment line. Several codes
may share an action; a code may appear only once.
"""
import argparse
import re
import struct
import sys

SECTOR = 4096
FLASH_BASE = 0x10000000
FAMILY_ABSOLUTE = 0xE48BFF57    # written to the block's address as-is
NAV = {"@prev", "@next", "@enter", "@back", "@up", "@down"}
LINE = re.compile(r"^([0-9A-Fa-f]{8})\s+([r-])\s+(.+?)\s*$")


def check(text):
    ok = True
    seen = {}
    for n, line in enumerate(text.splitlines(), 1):
        s = line.strip()
        if not s or s.startswith("#"):
            continue
        m = LINE.match(s)
        if not m:
            print(f"line {n}: expected 'CODE  r|-  action': {line}")
            ok = False
            continue
        code = m.group(1).upper()
        if m.group(3).startswith("@") and m.group(3) not in NAV:
            print(f"line {n}: unknown action {m.group(3)} (use {' '.join(sorted(NAV))})")
            ok = False
        if code in seen:
            print(f"line {n}: {code} is already on line {seen[code]}")
            ok = False
        seen[code] = n
    return ok


def uf2(data, addr):
    blocks = [data[i:i + 256] for i in range(0, len(data), 256)]
    out = b""
    for n, b in enumerate(blocks):
        b = b.ljust(256, b"\0")
        head = struct.pack("<8I", 0x0A324655, 0x9E5D5157, 0x00002000, addr + 256 * n,
                           256, n, len(blocks), FAMILY_ABSOLUTE)
        out += head + b.ljust(476, b"\0") + struct.pack("<I", 0x0AB16F30)
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("map")
    ap.add_argument("-o", "--out", default="irmap.uf2")
    ap.add_argument("--flash-mb", type=int, default=16, help="flash size (default 16)")
    args = ap.parse_args()

    text = open(args.map, encoding="ascii").read()
    if not check(text):
        sys.exit("not written")
    data = text.encode("ascii") + b"\0"
    if len(data) > SECTOR:
        sys.exit(f"map is {len(data)} bytes, the sector holds {SECTOR}")

    addr = FLASH_BASE + args.flash_mb * 1024 * 1024 - 2 * SECTOR
    with open(args.out, "wb") as f:
        f.write(uf2(data, addr))
    print(f"wrote {args.out}: {len(data)} bytes at 0x{addr:08X}")


if __name__ == "__main__":
    main()
