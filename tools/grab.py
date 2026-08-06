#!/usr/bin/env python3
"""Grab one frame from the ttl2dvi `capture` console command and save a PNG.

Usage:
    python3 grab.py /dev/tty.usbmodemXXXX [out.png]

Sends "capture" to the console, reads the block the firmware emits between
"@@@BEGIN" and "@@@END":

    @@@BEGIN
    W <width> H <height> BPP <bits>
    <height rows of <width> HEX digits>
    @@@END

BPP says how to read the digits, and the two sources differ:

  BPP 2  MDA: 0..3 = VIDEO | INTENSITY<<1 -> grayscale (0,85,170,255).
              0 = black, 1 = normal, 3 = bright, 2 = anomalous.
  BPP 4  CGA640: 0..f = I | R<<1 | G<<2 | B<<3 -> the 16-colour RGBI palette.

Writes a PNG (needs Pillow) plus a netpbm file that needs no deps: PGM for
mono, PPM for colour. The `BPP` field is optional so dumps from firmware before
it existed still load, as mono.
"""
import sys

try:
    import serial  # pyserial
except ImportError:
    sys.exit("need pyserial:  pip install pyserial")

LEVELS = [0, 85, 170, 255]   # 2-bit value -> gray


def cga_palette():
    """CGA RGBI -> RGB, matching video.c's build_cga_palette exactly (including
    the brown fix), so a dump looks like what the DVI output shows."""
    pal = []
    for i in range(16):
        I, R, G, B = i & 1, (i >> 1) & 1, (i >> 2) & 1, (i >> 3) & 1
        hi, lo = (255, 85) if I else (170, 0)
        r, g, b = (hi if R else lo), (hi if G else lo), (hi if B else lo)
        if not I and R and G and not B:
            g = 85                      # brown, not dark yellow
        pal.append((r, g, b))
    return pal


CGA = cga_palette()


def read_frame(port):
    s = serial.Serial(port, 115200, timeout=5)
    s.reset_input_buffer()
    s.write(b"capture\r")
    data = bytearray()
    while b"@@@END" not in data:
        chunk = s.read(4096)
        if not chunk:
            sys.exit("timed out (is the board flashed and the PC/card on?)")
        data += chunk

    body = data.split(b"@@@BEGIN", 1)[1].split(b"@@@END", 1)[0]
    lines = [ln.strip() for ln in body.decode("ascii", "ignore").splitlines()]
    lines = [ln for ln in lines if ln]
    hdr = lines[0].split()                       # "W <w> H <h> [BPP <n>]"
    if len(hdr) < 4 or hdr[0] != "W" or hdr[2] != "H":
        sys.exit("unexpected header: %r" % lines[0])
    w, h = int(hdr[1]), int(hdr[3])
    bpp = int(hdr[5]) if len(hdr) >= 6 and hdr[4] == "BPP" else 2
    rows = lines[1:1 + h]
    return w, h, bpp, rows


def save_netpbm(path, w, h, bpp, rows):
    """PGM (grayscale) for mono, PPM (colour) for CGA. No dependencies."""
    if bpp == 2:
        with open(path, "wb") as f:
            f.write(b"P5\n%d %d\n255\n" % (w, h))
            for r in rows:
                r = r.ljust(w, "0")[:w]
                f.write(bytes(LEVELS[int(c, 16) & 3] for c in r))
    else:
        with open(path, "wb") as f:
            f.write(b"P6\n%d %d\n255\n" % (w, h))
            for r in rows:
                r = r.ljust(w, "0")[:w]
                f.write(bytes(v for c in r for v in CGA[int(c, 16) & 15]))


def save_png(path, w, h, bpp, rows):
    from PIL import Image
    img = Image.new("L" if bpp == 2 else "RGB", (w, h))
    px = img.load()
    for y, r in enumerate(rows):
        for x in range(min(w, len(r))):
            v = int(r[x], 16)
            px[x, y] = LEVELS[v & 3] if bpp == 2 else CGA[v & 15]
    img.save(path)


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    port = sys.argv[1]
    out = sys.argv[2] if len(sys.argv) > 2 else "frame.png"
    w, h, bpp, rows = read_frame(port)
    print("got %dx%d, %d bpp (%s), %d rows"
          % (w, h, bpp, "mono" if bpp == 2 else "CGA colour", len(rows)))

    raw = out.rsplit(".", 1)[0] + (".pgm" if bpp == 2 else ".ppm")
    save_netpbm(raw, w, h, bpp, rows)
    print("wrote", raw)
    try:
        save_png(out, w, h, bpp, rows)
        print("wrote", out)
    except ImportError:
        print("(install Pillow for PNG:  pip install pillow -- %s still written)" % raw)


if __name__ == "__main__":
    main()
