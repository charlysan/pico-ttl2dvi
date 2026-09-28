#!/usr/bin/env python3
"""Find a card's dot clock and sampling phase from one high-rate capture.

Usage:
    python3 autotune.py /dev/tty.usbmodemXXXX [--skip N] [--save f.txt]
    python3 autotune.py --load f.txt

Put a screen with vertical edges on the card (text, or a checkerboard/column
pattern from mdapat/cgapat/egapat), select the source, then run this. It sends
`fastcap`, which samples every 2 sysclk from the normal window start, and:

1. times every transition on the data lines (the card only changes its
   outputs on dot-clock ticks, so edges sit at t0 + k * P);
2. finds the pixel period P that lines all edges up modulo P;
3. checks dot clock / HSYNC against a whole number of characters, and uses
   the snapped value only if it fits the edges at least as well;
4. picks the `phase` that keeps every pixel's sample furthest from an edge,
   with the firmware's own 1x/2x choice and sample selection.

It prints the `dotclock` and `phase` to type (dotclock first: it re-clamps
phase). bp is not touched; the phase is computed for the current bp.
"""
import argparse
import math
import sys

ALPHABET = "0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ+/"
VALUE = {c: i for i, c in enumerate(ALPHABET)}

try:
    import numpy as np
except ImportError:
    np = None


def read_dump(args):
    if args.load:
        text = open(args.load).read()
    else:
        try:
            import serial
        except ImportError:
            sys.exit("need pyserial:  pip install pyserial")
        s = serial.Serial(args.port, 115200, timeout=5)
        s.reset_input_buffer()
        s.write(b"fastcap %d\r" % args.skip)
        data = bytearray()
        while b"@@@END" not in data:
            chunk = s.read(65536)
            if not chunk:
                sys.exit("timed out (no signal, or firmware without fastcap?)")
            data += chunk
        text = data.decode("ascii", "ignore")
        if args.save:
            open(args.save, "w").write(text)
    if "@@@BEGIN" not in text:
        sys.exit(text.strip() or "no dump")
    body = text.split("@@@BEGIN", 1)[1].split("@@@END", 1)[0]
    lines = [ln.strip() for ln in body.splitlines() if ln.strip()]
    f = lines[0].split()
    if f[0] != "FAST":
        sys.exit("not a fastcap dump: %r" % lines[0])
    hdr = {k: int(v) for k, v in zip(f[1::2], f[2::2])}
    wh = lines[1].split()
    w, h = int(wh[1]), int(wh[3])
    rows = [ln[:w] for ln in lines[2:2 + h]]
    return hdr, rows


def find_edges(rows, cyc):
    """Transition between samples s-1 and s -> time cyc*s - cyc/2, in sysclk
    from the first sample."""
    ts = []
    for r in rows:
        prev = r[0]
        for s in range(1, len(r)):
            c = r[s]
            if c != prev:
                ts.append(cyc * s - cyc / 2.0)
                prev = c
    return ts


def concentration(ts, P):
    """How tightly edge times line up modulo P: 1 = all at one phase, ~0 =
    spread evenly. Also returns their mean position within P."""
    if np is not None:
        a = np.asarray(ts) * (2 * math.pi / P)
        c, s = np.cos(a).sum(), np.sin(a).sum()
    else:
        k = 2 * math.pi / P
        c = sum(math.cos(k * t) for t in ts)
        s = sum(math.sin(k * t) for t in ts)
    r = math.hypot(c, s) / len(ts)
    mean = (math.atan2(s, c) / (2 * math.pi) * P) % P
    return r, mean


def search(ts, P0):
    """Coarse then fine search for the P with the tightest edge alignment."""
    best = (0.0, P0)
    lo, hi, step = P0 * 0.95, P0 * 1.05, 0.005
    for _ in range(3):
        P = lo
        while P <= hi:
            r, _ = concentration(ts, P)
            if r > best[0]:
                best = (r, P)
            P += step
        lo, hi, step = best[1] - 4 * step, best[1] + 4 * step, step / 10
    return best[1], best[0]


def firmware_rate(sysclk, dot, bits, active):
    """capture.c's derive_rate(): px_cyc, 1x/2x, sample period, spp (16.16)."""
    px = sysclk / dot
    px_cyc = int(px + 0.5)
    err = abs(px / px_cyc - 1.0)
    os = 2 if bits <= 4 and err * (active + 16) > 0.5 else 1
    cyc = int(px / os + 0.5)
    spp = int(px / cyc * 65536 + 0.5)
    return px_cyc, os, cyc, spp


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("port", nargs="?")
    ap.add_argument("--skip", type=int, default=80,
                    help="lines after VSYNC before capturing (default 80)")
    ap.add_argument("--save", help="also save the raw dump to this file")
    ap.add_argument("--load", help="analyse a saved dump instead of capturing")
    args = ap.parse_args()
    if not args.port and not args.load:
        ap.error("need a port, or --load")

    hdr, rows = read_dump(args)
    sysclk, line, cyc = hdr["SYSCLK"], hdr["LINE"], hdr["CYC"]
    bits, active, bp = hdr["BITS"], hdr["ACTIVE"], hdr["BP"]
    hsync = sysclk / line
    print("%d lines x %d samples (every %d sysclk), %d bits, HSYNC %.2f Hz"
          % (len(rows), len(rows[0]) if rows else 0, cyc, bits, hsync))

    ts = find_edges(rows, cyc)
    print("%d edges" % len(ts))
    if len(ts) < 200:
        sys.exit("not enough edges: put text or a pattern on screen, or try "
                 "another --skip (the band may be in the blanking)")
    if len(ts) > 20000 and np is None:
        ts = ts[::len(ts) // 20000 + 1]

    # 1-2. The pixel period.
    P0 = sysclk / hdr["DOT"]
    P, r = search(ts, P0)
    print("\npixel period %.4f sysclk -> %.4f MHz (alignment %.3f)"
          % (P, sysclk / P / 1e6, r))

    # 3. Snap to whole characters.
    char = 9 if bits == 2 else 8
    chars = round(line / P / char)
    P_snap = line / (chars * char)
    r_snap, _ = concentration(ts, P_snap)
    print("h_total %.1f px -> %d chars of %d = %d px -> %.4f MHz (alignment %.3f)"
          % (line / P, chars, char, chars * char, sysclk / P_snap / 1e6, r_snap))
    # The edges are the direct measurement; the line period comes from the
    # sync SM and can be a few sysclk off. The snap only wins if it fits the
    # edges at least as well.
    if r_snap >= r:
        P = P_snap
    else:
        print("  the measured period fits the edges better; keeping it")
    dot = round(sysclk / P)

    # 4. Where the edges sit, measured from where the new dot clock's window
    # will start: the start is (pulse // px_cyc + bp) * px_cyc, and px_cyc
    # follows the dot clock.
    px_cyc, os, scyc, spp = firmware_rate(sysclk, dot, bits, active)
    start = (hdr["PULSE"] // px_cyc + bp) * px_cyc
    shift = hdr["DELAY"] - start
    r_e, e = concentration([t + shift for t in ts], P)
    spread = math.sqrt(max(0.0, -2 * math.log(max(r_e, 1e-9)))) * P / (2 * math.pi)
    print("\nwith dotclock %.4f: %dx, sample every %d, px_cyc %d"
          % (dot / 1e6, os, scyc, px_cyc))
    print("edges cluster at %.2f of %.2f sysclk, spread %.2f" % (e, P, spread))

    # Histogram of edge positions within a pixel.
    nb = int(round(P))
    hist = [0] * nb
    for t in ts:
        hist[int(((t + shift) % P) / P * nb) % nb] += 1
    top = max(hist) or 1
    for b in range(nb):
        print("  %5.1f %s" % (b * P / nb, "#" * (40 * hist[b] // top)))

    # Every phase: the worst distance from any pixel's sample to an edge.
    best = []
    for phase in range(px_cyc):
        worst = P
        for k in range(active):
            j = ((2 * k + 1) * spp) >> 17
            x = (phase + j * scyc - e) % P
            worst = min(worst, x, P - x)
        best.append((worst, phase))
    best.sort(reverse=True)
    print("\nphase: worst distance from a sample to an edge (sysclk)")
    for worst, phase in best[:4]:
        print("  phase %2d  %.2f%s" % (phase, worst,
                                        "  (inside the edge spread)" if worst < spread else ""))

    print("\nrecommended:\n  dotclock %.4f\n  phase %d" % (dot / 1e6, best[0][1]))


if __name__ == "__main__":
    main()
