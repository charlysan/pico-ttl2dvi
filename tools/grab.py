#!/usr/bin/env python3
"""Grab one frame from the ttl2dvi `capture` console command and save a PNG.

Usage:
    python3 grab.py /dev/tty.usbmodemXXXX [out.png]

Sends "capture" to the console, reads the block the firmware emits between
"@@@BEGIN" and "@@@END":

    @@@BEGIN
    W <width> H <height>
    <height rows of <width> digits, each 0..3 = VIDEO | INTENSITY<<1>
    @@@END

and writes a grayscale PNG (needs Pillow) plus a PGM (always, no deps). The
2-bit value maps to grayscale linearly (0,1,2,3 -> 0,85,170,255): 0 = black,
1 = normal (VIDEO), 3 = bright (VIDEO+INTENSITY), 2 = anomalous (should not
occur).
"""
import sys

try:
    import serial  # pyserial
except ImportError:
    sys.exit("need pyserial:  pip install pyserial")

LEVELS = [0, 85, 170, 255]   # 2-bit value -> gray


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
    hdr = lines[0].split()                       # "W <w> H <h>"
    if len(hdr) != 4 or hdr[0] != "W" or hdr[2] != "H":
        sys.exit("unexpected header: %r" % lines[0])
    w, h = int(hdr[1]), int(hdr[3])
    rows = lines[1:1 + h]
    return w, h, rows


def save_pgm(path, w, h, rows):
    with open(path, "wb") as f:
        f.write(b"P5\n%d %d\n255\n" % (w, h))
        for r in rows:
            r = r.ljust(w, "0")[:w]
            f.write(bytes(LEVELS[int(c) & 3] for c in r))


def save_png(path, w, h, rows):
    from PIL import Image
    img = Image.new("L", (w, h))
    px = img.load()
    for y, r in enumerate(rows):
        for x in range(min(w, len(r))):
            px[x, y] = LEVELS[int(r[x]) & 3]
    img.save(path)


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    port = sys.argv[1]
    out = sys.argv[2] if len(sys.argv) > 2 else "frame.png"
    w, h, rows = read_frame(port)
    print("got %dx%d, %d rows" % (w, h, len(rows)))

    pgm = out.rsplit(".", 1)[0] + ".pgm"
    save_pgm(pgm, w, h, rows)
    print("wrote", pgm)
    try:
        save_png(out, w, h, rows)
        print("wrote", out)
    except ImportError:
        print("(install Pillow for PNG:  pip install pillow -- %s still written)" % pgm)


if __name__ == "__main__":
    main()
