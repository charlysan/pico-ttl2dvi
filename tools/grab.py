#!/usr/bin/env python3
"""Grab one frame from the ttl2dvi `capture` console command and save a PNG.

Usage:
    python3 grab.py /dev/tty.usbmodemXXXX [out.png]

Sends "capture" to the console, reads the block the firmware emits between
"@@@BEGIN" and "@@@END":

    @@@BEGIN
    W <width> H <height> BPP 2
    <height rows of <width> digits>
    @@@END

MDA is 2bpp: each digit is VIDEO | INTENSITY<<1, so 0..3 -> grayscale
(0, 120, 170, 255). 0 = black, 1 = normal, 3 = bright; 2 is INTENSITY with no
VIDEO, which the card should never produce - see the histogram.

Writes a PNG (needs Pillow) plus a PGM that needs no deps. The `BPP` field is
optional so dumps from firmware before it existed still load.
"""
import sys

try:
    import serial  # pyserial
except ImportError:
    sys.exit("need pyserial:  pip install pyserial")

LEVELS = [0, 120, 170, 255]   # 2-bit value -> gray

NAMES = {0: "black", 1: "normal", 2: "INTENSITY w/o VIDEO -- anomalous",
         3: "bright"}


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
    if bpp != 2:
        sys.exit("this grab.py is MDA-only (2bpp); firmware sent BPP %d" % bpp)
    return w, h, lines[1:1 + h]


def row_values(r, w):
    """One row of digits -> w integers, one digit per pixel."""
    return [int(c, 16) & 3 for c in r.ljust(w, "0")[:w]]


def save_pgm(path, w, h, rows):
    """Grayscale netpbm. No dependencies."""
    with open(path, "wb") as f:
        f.write(b"P5\n%d %d\n255\n" % (w, h))
        for r in rows:
            f.write(bytes(LEVELS[v] for v in row_values(r, w)))


def save_png(path, w, h, rows):
    from PIL import Image
    img = Image.new("L", (w, h))
    px = img.load()
    for y, r in enumerate(rows):
        for x, v in enumerate(row_values(r, w)):
            px[x, y] = LEVELS[v]
    img.save(path)


def histogram(w, rows):
    """Which sample values actually appear, most common first."""
    counts = {}
    for r in rows:
        for v in row_values(r, w):
            counts[v] = counts.get(v, 0) + 1
    total = sum(counts.values()) or 1
    print("\nvalues present (%d distinct):" % len(counts))
    for v, n in sorted(counts.items(), key=lambda kv: -kv[1]):
        print("  %d  %7d  %5.1f%%  %s" % (v, n, 100.0 * n / total, NAMES[v]))


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    port = sys.argv[1]
    out = sys.argv[2] if len(sys.argv) > 2 else "frame.png"
    w, h, rows = read_frame(port)
    print("got %dx%d, 2 bpp (mono), %d rows" % (w, h, len(rows)))
    histogram(w, rows)

    raw = out.rsplit(".", 1)[0] + ".pgm"
    save_pgm(raw, w, h, rows)
    print("wrote", raw)
    try:
        save_png(out, w, h, rows)
        print("wrote", out)
    except ImportError:
        print("(install Pillow for PNG:  pip install pillow -- %s still written)" % raw)


if __name__ == "__main__":
    main()
