#!/usr/bin/env python3
"""Read IR remote codes through the ttl2dvi console, and write a key map.

Usage:
    python3 irlearn.py /dev/tty.usbmodemXXXX                 # print codes
    python3 irlearn.py /dev/tty.usbmodemXXXX --learn my.txt  # write a map

Turns on the firmware's `ir on` (every decoded NEC frame is printed as
"ir: XXXXXXXX ...") and turns it off again on exit.

--learn asks for the four navigation actions, each with as many buttons as
you like (e.g. LEFT and UP both for PREV), then for any number of shortcuts:
press a button, type the console command it should run. Ctrl-C ends it and
writes the file:

    # code     repeat  action
    F807FF00   r       @prev
    BF40FF00   r       @prev
    F30CFF00   -       osd status

`@prev` (previous item, -1), `@next` (next item, +1), `@enter` and `@back`
drive the menu; anything else is a console command. `r` repeats the action
while the button is held.
"""
import argparse
import re
import sys
import time

try:
    import serial  # pyserial
except ImportError:
    sys.exit("need pyserial:  pip install pyserial")

CODE = re.compile(r"^ir: ([0-9A-F]{8})")

NAV = [
    ("@prev",  "r", "PREV (previous item / -1, e.g. LEFT)"),
    ("@next",  "r", "NEXT (next item / +1, e.g. RIGHT)"),
    ("@enter", "-", "ENTER (OK)"),
    ("@back",  "-", "BACK"),
]


def send(ser, line):
    ser.write((line + "\n").encode())
    ser.flush()


def next_code(ser):
    """Blocks until a frame arrives; repeats and console chatter are skipped."""
    while True:
        line = ser.readline().decode(errors="replace").strip()
        m = CODE.match(line)
        if m:
            return m.group(1)


def fresh_code(ser):
    """The next press, ignoring whatever arrived before the prompt."""
    time.sleep(0.3)
    ser.reset_input_buffer()
    return next_code(ser)


def monitor(ser):
    print("press buttons on the remote, Ctrl-C to stop")
    while True:
        line = ser.readline().decode(errors="replace").strip()
        if line.startswith("ir: "):
            print(line[4:])


def learn(ser, path):
    entries = []        # (code, repeat, action)
    used = {}

    def take(prompt):
        print(prompt, end="", flush=True)
        while True:
            code = fresh_code(ser)
            if code not in used:
                print(f"  {code}")
                return code
            print(f"\n  {code} is already {used[code]}, press another: ", end="", flush=True)

    for action, rep, what in NAV:
        prompt = f"press {what}: "
        while True:
            code = take(prompt)
            entries.append((code, rep, action))
            used[code] = action
            if input("  another button for this? [y/N] ").strip().lower() != "y":
                break
            prompt = f"press another button for {action}: "

    print("\nshortcuts: press a button, then type its command. Ctrl-C to finish.")
    try:
        while True:
            code = take("press a button: ")
            cmd = input("  command (empty to skip): ").strip()
            if not cmd:
                continue
            rep = "r" if input("  repeat while held? [y/N] ").strip().lower() == "y" else "-"
            entries.append((code, rep, cmd))
            used[code] = cmd
    except KeyboardInterrupt:
        print()

    with open(path, "w") as f:
        f.write("# ttl2dvi IR key map, written by tools/irlearn.py\n")
        f.write("# code     repeat  action\n")
        for code, rep, action in entries:
            f.write(f"{code}   {rep}       {action}\n")
    print(f"wrote {len(entries)} keys to {path}")


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("port")
    ap.add_argument("--learn", metavar="FILE", help="write a key map to FILE")
    args = ap.parse_args()

    ser = serial.Serial(args.port, 115200, timeout=None)
    send(ser, "ir on")
    try:
        if args.learn:
            learn(ser, args.learn)
        else:
            monitor(ser)
    except KeyboardInterrupt:
        print()
    finally:
        send(ser, "ir off")
        ser.close()


if __name__ == "__main__":
    main()
