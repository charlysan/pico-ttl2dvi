#!/usr/bin/env python3
"""Grab one frame from the ttl2dvi `capture` console command and save a PNG.

Usage:
    python3 grab.py /dev/tty.usbmodemXXXX [out.png] [--raw] [--vscale N]

Sends "capture" to the console, reads the block the firmware emits between
"@@@BEGIN" and "@@@END":

    @@@BEGIN
    W <samples> H <lines> BPP <bits> SPP <samples per pixel>
    <lines rows of <samples> characters, one per sample>
    @@@END

Each character is a sample value: 0-9 a-z A-Z + / = 0..63 (so 2- and 4-bit
dumps read as plain hex). BPP says what the value means:

  BPP 2  MDA: VIDEO | INTENSITY<<1 -> grey. 2 (INTENSITY without VIDEO) is
         never emitted by the card; the histogram shows it if it appears.
  BPP 4  CGA, C128, EGA 200: I | R<<1 | G<<2 | B<<3 -> the 16 RGBI colours.
  BPP 6  EGA 350: sB | sG<<1 | R<<2 | G<<3 | B<<4 | sR<<5 -> 64 colours.

SPP > 1 means the line was oversampled (EGA 200 is ~2.09): pixel k is sample
floor((k + 0.5) * SPP), the same choice the firmware's view makes. --raw keeps
every sample instead. --vscale N repeats each line N times, as the firmware's
vscale does on screen: 2 for 200-line modes gives the displayed shape.
Colours are full scale (0/85/170/255), not the 75% the DVI output shows.

Writes a PNG (needs Pillow) plus a netpbm file that needs no deps. Older dumps
without BPP or SPP load as MDA at one sample per pixel.
"""
import argparse
import sys

try:
    import serial  # pyserial
except ImportError:
    sys.exit("need pyserial:  pip install pyserial")

ALPHABET = "0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ+/"
VALUE = {c: i for i, c in enumerate(ALPHABET)}

GREYS = [0, 120, 170, 255]
MDA_NAMES = {0: "black", 1: "normal", 2: "INTENSITY w/o VIDEO -- anomalous",
             3: "bright"}


def rgbi_colours():
    """Matches video.c's build_cga_rgb222, brown included."""
    pal = []
    for i in range(16):
        I, R, G, B = i & 1, (i >> 1) & 1, (i >> 2) & 1, (i >> 3) & 1
        r, g, b = (R << 1) | I, (G << 1) | I, (B << 1) | I
        if not I and R and G and not B:
            g = 1
        pal.append((r * 85, g * 85, b * 85))
    return pal


def ega_colours():
    """Matches video.c's build_ega_rgb222: level = primary << 1 | secondary."""
    pal = []
    for i in range(64):
        r = ((i >> 2) & 1) << 1 | ((i >> 5) & 1)
        g = ((i >> 3) & 1) << 1 | ((i >> 1) & 1)
        b = ((i >> 4) & 1) << 1 | (i & 1)
        pal.append((r * 85, g * 85, b * 85))
    return pal


RGBI = rgbi_colours()
EGA = ega_colours()


def colour(v, bpp):
    if bpp == 2:
        return GREYS[v & 3]
    return EGA[v & 63] if bpp == 6 else RGBI[v & 15]


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
    hdr = lines[0].split()               # "W <w> H <h> [BPP <n>] [SPP <x>]"
    if len(hdr) < 4 or hdr[0] != "W" or hdr[2] != "H":
        sys.exit("unexpected header: %r" % lines[0])
    fields = dict(zip(hdr[0::2], hdr[1::2]))
    w, h = int(fields["W"]), int(fields["H"])
    bpp = int(fields.get("BPP", 2))
    spp = float(fields.get("SPP", 1))
    rows = [[VALUE.get(c, 0) for c in r.ljust(w, "0")[:w]] for r in lines[1:1 + h]]
    return w, h, bpp, spp, rows


def resample(rows, w, spp):
    """Samples -> pixels: pixel k takes sample floor((k + 0.5) * spp)."""
    n = int(w / spp)
    pick = [min(int((k + 0.5) * spp), w - 1) for k in range(n)]
    return n, [[r[s] for s in pick] for r in rows]


def save_netpbm(path, w, h, bpp, rows):
    """PGM for mono, PPM for colour. No dependencies."""
    with open(path, "wb") as f:
        if bpp == 2:
            f.write(b"P5\n%d %d\n255\n" % (w, h))
            for r in rows:
                f.write(bytes(colour(v, bpp) for v in r))
        else:
            f.write(b"P6\n%d %d\n255\n" % (w, h))
            for r in rows:
                f.write(bytes(c for v in r for c in colour(v, bpp)))


def save_png(path, w, h, bpp, rows):
    from PIL import Image
    img = Image.new("L" if bpp == 2 else "RGB", (w, h))
    px = img.load()
    for y, r in enumerate(rows):
        for x, v in enumerate(r):
            px[x, y] = colour(v, bpp)
    img.save(path)


def histogram(rows, bpp):
    """Which sample values actually appear, most common first."""
    counts = {}
    for r in rows:
        for v in r:
            counts[v] = counts.get(v, 0) + 1
    total = sum(counts.values()) or 1
    print("\nvalues present (%d distinct):" % len(counts))
    for v, n in sorted(counts.items(), key=lambda kv: -kv[1]):
        name = MDA_NAMES.get(v, "") if bpp == 2 else "rgb%s" % (colour(v, bpp),)
        print("  %2d  0x%02x  %7d  %5.1f%%  %s" % (v, v, n, 100.0 * n / total, name))


def main():
    ap = argparse.ArgumentParser(description="Grab one frame and save a PNG.")
    ap.add_argument("port")
    ap.add_argument("out", nargs="?", default="frame.png")
    ap.add_argument("--raw", action="store_true",
                    help="keep every sample, don't resample oversampled lines")
    ap.add_argument("--vscale", type=int, default=1, metavar="N",
                    help="repeat each line N times, like the firmware's vscale")
    a = ap.parse_args()

    w, h, bpp, spp, rows = read_frame(a.port)
    print("got %d samples x %d lines, %d bpp, %.4f samples/px" % (w, h, bpp, spp))
    histogram(rows, bpp)
    if spp > 1.01 and not a.raw:
        w, rows = resample(rows, w, spp)
        print("resampled to %d px" % w)
    if a.vscale > 1:
        rows = [r for r in rows for _ in range(a.vscale)]
        print("each line x%d -> %d rows" % (a.vscale, len(rows)))
    out = a.out

    raw = out.rsplit(".", 1)[0] + (".pgm" if bpp == 2 else ".ppm")
    save_netpbm(raw, w, len(rows), bpp, rows)
    print("wrote", raw)
    try:
        save_png(out, w, len(rows), bpp, rows)
        print("wrote", out)
    except ImportError:
        print("(install Pillow for PNG:  pip install pillow -- %s still written)" % raw)


if __name__ == "__main__":
    main()
