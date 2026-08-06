#!/usr/bin/env python3
"""
ttl2dvi live control panel.

A single-page Tk GUI to drive the `ttl2dvi` firmware over its USB serial
console. 
A background poller reads `status` and keeps the live-readings panel in sync.

Self-contained: needs only pyserial and tkinter (both usually already present).
Run:  python3 ttl2dviPanel.py
      python3 ttl2dviPanel.py -p /dev/tty.usbmodem1101

The firmware console (src/console/console.c) is a plain interactive shell: the
prompt is "ttl2dvi> ", there is no ack line, and each command echoes one or more
reply lines. Replies this panel parses:

    status      HSYNC 18155.41 Hz   VSYNC 49.04 Hz      ("--" when no input)
    bp          bp = 17 px (horizontal back-porch trim, whole source pixels)
    phase       phase = 4/16 px (sampling instant within the pixel, ...)
    vscale      vscale = 1x vertical (horizontal is always 1:1)
    vpos        vpos = 0 source lines (+ moves the image down)
    hpos        hpos = 0 source px (+ moves the image right)
    mdalevels   mda_levels normal=2 bright=3
    mode        one line per mode, the active one tagged "<- current"

Device detection uses `version` (the reply starts "ttl2dvi ")
"""

import re
import argparse
import queue
import threading
import time

import serial
from serial.tools import list_ports
import tkinter as tk
from tkinter import ttk
from tkinter.scrolledtext import ScrolledText

PROMPT = "ttl2dvi>"
BAUD = 115200
DEFAULT_PORT = "/dev/tty.usbmodem1101"

# Test patterns: (label, `test` argument). Only visible when capture has no
# input -- the live loop overwrites the framebuffer otherwise. The firmware
# holds the pattern with a sleep, so these replies come back slowly.
TESTS = [("Stripes", "0"), ("Checkerboard", "1")]
TEST_TIMEOUT = 8.0        # `test` blocks in the firmware (~5 s) before prompting

# Commands that ALWAYS reboot the device, so USB drops mid-reply and pyserial
# raises "[Errno 6] Device not configured". That is success, not a fault: every
# command that writes flash restarts, because a flash write stops core1 and with
# it the DVI output (see src/settings/settings.c). Sending one of these
# suppresses the I/O error and schedules a reconnect.
REBOOT_CMDS = {"mode", "reboot"}

# Commands that reboot only SOMETIMES. Every one of these can also refuse and
# reply normally -- `load` applies live when the profile's mode already matches;
# save/clear/boot reject a bad slot; restore rejects a bad checksum -- and those
# replies are the ones you most need to see. So they are sent NOT quiet, and the
# reconnect is scheduled only if the port actually dropped. Sending them as
# unconditional reboots hid the failure message and reconnected for nothing.
MAYBE_REBOOT_CMDS = {"save", "clear", "boot", "restore", "load"}

RECONNECT_MS = 3500       # give the device time to re-enumerate over USB

# Knob rows: (attr, command, label, lo, hi, default, tooltip-ish note)
# Capture-side: these move the sampling window itself.
CAPTURE_KNOBS = [
    ("bp",    "bp",    "Back porch / left  (bp)", 0, 400, 17),
    ("phase", "phase", "Sample phase  (sysclk)",  0,  15,  4),
]
# Display-side: these move/scale the already-captured image in the framebuffer.
DISPLAY_KNOBS = [
    ("vscale", "vscale", "Vertical scale  (1 | 2)",    1,   4,  1),
    ("vpos",   "vpos",   "V position  (+ = down)",  -400, 400,  0),
    ("hpos",   "hpos",   "H position  (+ = right)", -400, 400,  0),
]


# ---------------------------------------------------------------------------
# reply parsing
# ---------------------------------------------------------------------------
def parse_status(text):
    """Pull the firmware `status` line out of a reply -> dict, or None.
    `HSYNC <hz> Hz   VSYNC <hz> Hz`, either of which may read `--`."""
    for ln in (text or "").splitlines():
        if "HSYNC" not in ln:
            continue
        mh = re.search(r"HSYNC\s+([\d.]+)\s*Hz", ln)
        mv = re.search(r"VSYNC\s+([\d.]+)\s*Hz", ln)
        return {"hsync": mh.group(1) if mh else None,
                "vsync": mv.group(1) if mv else None}
    return None


def parse_kv(text, key):
    """Parse a `<key> = <int>...` reply (bp / phase / vscale / vpos / hpos).
    Tolerates the trailing unit or fraction, e.g. `phase = 4/16 px`."""
    m = re.search(re.escape(key) + r"\s*=\s*(-?\d+)", text or "")
    return int(m.group(1)) if m else None


def parse_levels(text):
    """`mda_levels normal=2 bright=3` -> (2, 3); missing parts come back None."""
    mn = re.search(r"normal\s*=\s*(\d+)", text or "")
    mb = re.search(r"bright\s*=\s*(\d+)", text or "")
    return (int(mn.group(1)) if mn else None,
            int(mb.group(1)) if mb else None)


def parse_modes(text):
    """Parse the `mode` listing -> (names_by_index, current_index).
        0  736x480@50   <- current
        1  720x576@50
    """
    names, cur = [], None
    for ln in (text or "").splitlines():
        m = re.match(r"\s*(\d+)\s+(\S+)", ln)
        if not m:
            continue
        idx, name = int(m.group(1)), m.group(2)
        while len(names) <= idx:
            names.append(None)
        names[idx] = name
        if "current" in ln:
            cur = idx
    return names, cur


def is_ttl2dvi(text):
    """True if a `version` reply looks like our firmware."""
    return bool(re.search(r"\bttl2dvi\b", text or "", re.I))


# ---------------------------------------------------------------------------
# Serial worker: owns the port on one background thread so the UI never blocks
# and commands never overlap on the wire.
# ---------------------------------------------------------------------------
class SerialWorker:
    def __init__(self, log_fn, status_fn):
        self._log = log_fn
        self._status = status_fn
        self._jobs = queue.Queue()
        self._port = None
        self._running = True
        threading.Thread(target=self._loop, daemon=True).start()

    # ---- prompt-aware conversation ----
    def _converse(self, cmd, initial_timeout=2.0, idle_gap=0.3):
        ser = self._port
        ser.reset_input_buffer()
        ser.write((cmd + "\r").encode("ascii"))
        # A command that BLOCKS in the firmware (`test` sleeps ~5 s) prints its
        # reply and only prompts afterwards, so an idle gap there does not mean
        # "finished". When the caller allows extra time, wait for the prompt or
        # the deadline and ignore the gap; otherwise the gap ends it as usual.
        patient = initial_timeout > 2.0
        buf = bytearray()
        start = last = time.time()
        while True:
            n = ser.in_waiting
            if n:
                buf += ser.read(n)
                last = time.time()
                tail = bytes(buf[-len(PROMPT) - 4:]).decode("ascii", "replace")
                if tail.rstrip().endswith(PROMPT):
                    break
            else:
                now = time.time()
                if now - start > initial_timeout:
                    break
                if buf and not patient and now - last > idle_gap:
                    break
                time.sleep(0.01)
        return buf.decode("ascii", "replace")

    def _parse(self, cmd, raw):
        """Strip the echo and trailing prompt; return the reply body lines."""
        idx = raw.rfind(PROMPT)
        if idx != -1:
            raw = raw[:idx]
        body = []
        for ln in raw.replace("\r", "").split("\n"):
            s = ln.strip()
            if s.startswith(PROMPT):
                s = s[len(PROMPT):].strip()
            if not s or s == cmd:
                continue
            body.append(s)
        return "\n".join(body)

    # ---- public API (called from UI thread) ----
    def connect(self, port=None, skip_scan=False):
        self._jobs.put(("connect", (port, skip_scan)))

    def send(self, cmd, on_done=None, quiet=False, timeout=2.0, reboots=False):
        self._jobs.put(("cmd", (cmd, on_done, quiet, timeout, reboots)))

    def stop(self):
        self._running = False
        self._jobs.put(("quit", None))

    @property
    def connected(self):
        return self._port is not None

    # ---- worker thread ----
    def _loop(self):
        last_check = 0.0
        while self._running:
            try:
                kind, arg = self._jobs.get(timeout=1.0)
            except queue.Empty:
                now = time.time()
                if self._port is not None and now - last_check > 2.0:
                    last_check = now
                    if not self._alive():
                        self._log("* Device disconnected")
                        self._close()
                        self._status(False, "")
                continue
            if kind == "quit":
                break
            if kind == "connect":
                self._do_connect(*arg)
            elif kind == "cmd":
                self._do_cmd(*arg)

    def _open(self, port):
        return serial.Serial(port=port, baudrate=BAUD, timeout=1, write_timeout=2)

    def _close(self):
        if self._port is not None:
            try:
                self._port.close()
            except Exception:
                pass
            self._port = None

    def _alive(self):
        try:
            return self._port.is_open and self._port.in_waiting >= 0
        except Exception:
            return False

    def _try_port(self, port):
        """Open + probe one port; keep it if `version` says it is our firmware."""
        try:
            self._port = self._open(port)
        except Exception:
            return False
        try:
            body = self._parse("version", self._converse("version"))
        except Exception:
            body = ""
        if is_ttl2dvi(body):
            return True
        self._close()
        return False

    def _do_connect(self, port, skip_scan):
        self._close()
        if skip_scan:
            if not port:
                self._log("No port set; cannot connect without auto-detect")
                self._status(False, "")
                return
            try:
                self._port = self._open(port)
            except Exception as e:
                self._log("Cannot open %s: %s" % (port, e))
                self._status(False, "")
                return
            self._log("Connected on %s (no auto-detect)" % port)
            self._status(True, port)
            return
        # Auto-detect: try the preferred port first, then every other one.
        candidates = ([port] if port else []) + [
            p.device for p in list_ports.comports()
            if p.device != port and "bluetooth" not in p.device.lower()]
        for cand in candidates:
            self._log("Probing %s ..." % cand)
            if self._try_port(cand):
                self._log("Connected on %s" % cand)
                self._status(True, cand)
                return
        self._log("No ttl2dvi device found. Flash the `ttl2dvi` build, or set the "
                  "port and tick 'No auto-detect'.")
        self._status(False, "")

    def _do_cmd(self, cmd, on_done, quiet, timeout, reboots=False):
        if self._port is None:
            if not quiet:
                self._log("! Not connected: %s" % cmd)
            if on_done:
                _ui_call(on_done, False, "")
            return
        if not quiet:
            self._log(">> %s" % cmd)
        try:
            raw = self._converse(cmd, initial_timeout=timeout)
        except Exception as e:
            # A rebooting command drops USB mid-reply; that is the command
            # working, not failing, so say so rather than crying I/O error.
            if reboots:
                self._log("* device rebooting (USB dropped) -- reconnecting...")
            else:
                self._log("!! I/O error on '%s': %s" % (cmd, e))
            self._close()
            self._status(False, "")
            if on_done:
                _ui_call(on_done, False, "")
            return
        body = self._parse(cmd, raw)
        if body and not quiet:
            self._log(body)
        if on_done:
            _ui_call(on_done, True, body)


# ---------------------------------------------------------------------------
# UI-thread marshalling
# ---------------------------------------------------------------------------
_root = None


def _ui_call(fn, *args):
    if _root is not None:
        _root.after(0, lambda: fn(*args))


# ---------------------------------------------------------------------------
# Application
# ---------------------------------------------------------------------------
class App:
    def __init__(self, root, default_port=DEFAULT_PORT):
        global _root
        _root = root
        self.root = root
        self.default_port = default_port
        self.worker = SerialWorker(self.log, self.set_status)
        self.auto_set = None              # BooleanVar, built with the status bar
        self._poll_busy = False
        self.console_win = None           # Toplevel while the console is detached
        self.history = []                 # sent raw commands (Up/Down recall)
        self.hist_pos = 0
        self.spins = {}                   # knob attr -> Spinbox
        self.mode_buttons = []

        root.title("ttl2dvi control panel")
        root.minsize(620, 700)
        outer = ttk.Frame(root, padding=8)
        outer.pack(fill="both", expand=True)

        self._build_status_bar(outer)

        # Controls on top, console (detachable) in a resizable bottom pane.
        self.paned = ttk.PanedWindow(outer, orient="vertical")
        self.paned.pack(fill="both", expand=True)
        top = ttk.Frame(self.paned)
        self.bottom = ttk.Frame(self.paned)
        self.paned.add(top, weight=0)
        self.paned.add(self.bottom, weight=1)

        left = ttk.Frame(top)
        left.pack(side="left", fill="both", expand=True, padx=(0, 4))
        right = ttk.Frame(top)
        right.pack(side="left", fill="both", expand=True, padx=(4, 0))

        self._build_readings(left)
        self._build_mode(left)
        self._build_capture(left)

        self._build_display(left)
        self._build_levels(right)
        self._build_profiles(right)
        self._build_test(right)
        self._build_diag(right)

        self._build_raw(self.bottom)
        self._build_console(self.bottom)

        root.protocol("WM_DELETE_WINDOW", self.on_close)
        root.after(200, self.do_connect)
        root.after(1500, self._poll_tick)

    # ---- small helpers ----
    def _group(self, parent, title):
        f = ttk.LabelFrame(parent, text=title, padding=6)
        f.pack(fill="x", pady=4)
        return f

    def _spin(self, parent, lo, hi, default, width=6, increment=1):
        s = ttk.Spinbox(parent, width=width, from_=lo, to=hi, increment=increment)
        s.set(default)
        return s

    def _int(self, spin, fallback):
        try:
            return int(float(spin.get()))
        except (ValueError, tk.TclError):
            return fallback

    def _autowire(self, spin, send_fn):
        """Spinner sends on the up/down arrows when Auto-set is on, always on Enter."""
        spin.configure(command=lambda: self._auto(send_fn))
        spin.bind("<Return>", lambda _e: send_fn())

    def _auto(self, send_fn):
        if self.auto_set.get():
            send_fn()

    def _set_widget(self, widget, value):
        """Update a spinner from polled data - but never while the user is
        editing it (it has focus), and only when the value changed."""
        if value is None:
            return
        try:
            if self.root.focus_get() is widget:
                return
        except (KeyError, tk.TclError):
            pass
        if str(widget.get()) == str(value):
            return
        widget.set(str(value))

    # ---- generic knob row: label + spinner + Set, absolute value ----
    def _knob_row(self, parent, attr, cmd, label, lo, hi, default):
        row = ttk.Frame(parent)
        row.pack(fill="x", pady=1)
        ttk.Label(row, text=label, width=24).pack(side="left")
        spin = self._spin(row, lo, hi, default)
        spin.pack(side="left", padx=2)
        self.spins[attr] = spin
        send = lambda: self._send_knob(attr, cmd, default)
        self._autowire(spin, send)
        ttk.Button(row, text="Set", width=5, command=send).pack(side="left", padx=4)

    def _send_knob(self, attr, cmd, default):
        val = self._int(self.spins[attr], default)
        self.worker.send("%s %d" % (cmd, val),
                         on_done=lambda ok, t: self._on_knob(attr, cmd, t))

    def _on_knob(self, attr, cmd, text):
        self._set_widget(self.spins[attr], parse_kv(text, cmd))

    # ---- status bar ----
    def _build_status_bar(self, parent):
        bar = ttk.Frame(parent)
        bar.pack(fill="x", pady=(0, 6))
        self.dot = ttk.Label(bar, text="●", foreground="#c0392b")
        self.dot.pack(side="left")
        self.status_label = ttk.Label(bar, text="Disconnected")
        self.status_label.pack(side="left", padx=6)
        self.auto_set = tk.BooleanVar(value=True)
        ttk.Checkbutton(bar, text="Auto-set spinners", variable=self.auto_set).pack(side="left", padx=12)

        ttk.Button(bar, text="Reconnect", command=self.do_connect).pack(side="right")
        self.no_scan = tk.BooleanVar(value=False)
        ttk.Checkbutton(bar, text="No auto-detect", variable=self.no_scan).pack(side="right", padx=6)
        self.port_entry = ttk.Entry(bar, width=22)
        self.port_entry.insert(0, self.default_port)
        self.port_entry.pack(side="right")
        ttk.Label(bar, text="Port:").pack(side="right", padx=(0, 3))

    def do_connect(self):
        self.worker.connect(port=self.port_entry.get().strip(),
                            skip_scan=self.no_scan.get())

    def set_status(self, connected, port):
        if connected:
            self.dot.config(foreground="#27ae60")
            self.status_label.config(text="Connected  %s" % port)
            self.root.after(0, self._refresh_all)
        else:
            self.dot.config(foreground="#c0392b")
            self.status_label.config(text="Disconnected")

    # ---- live readings + poller ----
    def _build_readings(self, parent):
        g = self._group(parent, "Live readings")
        top = ttk.Frame(g)
        top.pack(fill="x")
        self.poll_enabled = tk.BooleanVar(value=True)
        ttk.Checkbutton(top, text="Poll every", variable=self.poll_enabled).pack(side="left")
        self.poll_int = self._spin(top, 1, 30, 1, width=3)
        self.poll_int.pack(side="left", padx=2)
        ttk.Label(top, text="s").pack(side="left")

        grid = ttk.Frame(g)
        grid.pack(fill="x", pady=(4, 0))
        self.meas = {}
        for i, (label, key) in enumerate([("HSYNC", "hsync"), ("VSYNC", "vsync"),
                                          ("Mode", "mode")]):
            ttk.Label(grid, text=label + ":").grid(row=i, column=0, sticky="w",
                                                   padx=(0, 4), pady=1)
            v = ttk.Label(grid, text="—", width=18)
            v.grid(row=i, column=1, sticky="w", padx=(0, 10), pady=1)
            self.meas[key] = v

    def _poll_tick(self):
        if self.worker.connected and self.poll_enabled.get() and not self._poll_busy:
            self._poll_busy = True
            self.worker.send("status", on_done=self._on_status, quiet=True)
        interval = max(1, self._int(self.poll_int, 1))
        self.root.after(interval * 1000, self._poll_tick)

    def _on_status(self, ok, text):
        self._poll_busy = False
        st = parse_status(text)
        if not st:
            return
        self.meas["hsync"].config(text=("%s Hz" % st["hsync"]) if st["hsync"] else "no signal")
        self.meas["vsync"].config(text=("%s Hz" % st["vsync"]) if st["vsync"] else "no signal")

    # ---- DVI output mode (switching reboots) ----
    def _build_mode(self, parent):
        g = self._group(parent, "Output mode  (switching reboots the device)")
        self.mode_row = ttk.Frame(g)
        self.mode_row.pack(fill="x")
        ttk.Label(g, wraplength=380, foreground="#888",
                  text="736x480@50 default (non-standard raster).\n"
                       "720x576@50 real CEA 576p\n"
                       "640x480@60 crops 40 px each side."
                  ).pack(anchor="w", pady=(4, 0))

    def _rebuild_mode_buttons(self, names, current):
        for b in self.mode_buttons:
            b.destroy()
        self.mode_buttons = []
        for i, name in enumerate(names):
            if name is None:
                continue
            b = ttk.Button(self.mode_row, text=name, width=13,
                           command=lambda n=i, s=name: self.switch_mode(n, s))
            b.pack(side="left", padx=2)
            if i == current:
                b.state(["disabled"])        # already there
            self.mode_buttons.append(b)
        if current is not None and current < len(names):
            self.meas["mode"].config(text=names[current] or "—")

    # Send a command that restarts the device: the reply is cut short by the USB
    # drop, so log our own note, suppress the I/O error, and reconnect once it
    # has re-enumerated. Every flash writer (save/clear/boot/restore) reboots.
    def send_rebooting(self, cmd, note):
        self.log(note)
        self.worker.send(cmd, quiet=True, reboots=True)
        self.root.after(RECONNECT_MS, self.do_connect)

    # For a command that MIGHT reboot (`load`): send it normally, and decide
    # afterwards from what actually happened. A reply means it applied live, so
    # just re-read the knobs; a dropped port means it rebooted, so reconnect.
    # Never schedules a reconnect it does not need.
    def send_maybe_rebooting(self, cmd, note):
        self.log(note)
        self.worker.send(cmd, reboots=True, on_done=self._on_maybe_reboot)

    def _on_maybe_reboot(self, ok, text):
        if ok:
            self._refresh_all()                      # applied live: resync the UI
        else:
            self.root.after(RECONNECT_MS, self.do_connect)   # it rebooted

    def switch_mode(self, index, name):
        self.send_rebooting("mode %d" % index,
                            "Switching to %s (device reboots, USB will drop) ..." % name)

    def _on_modes(self, ok, text):
        names, current = parse_modes(text)
        if names:
            self._rebuild_mode_buttons(names, current)

    # ---- capture framing: back-porch trim + sampling phase ----
    def _build_capture(self, parent):
        g = self._group(parent, "Capture framing  (moves the sampling window)")
        for attr, cmd, label, lo, hi, default in CAPTURE_KNOBS:
            self._knob_row(g, attr, cmd, label, lo, hi, default)
        ttk.Label(g, wraplength=380, foreground="#888",
                  text="bp finds the active picture (whole source pixels). "
                       "phase is the sampling instant inside one pixel, in sysclk "
                       "(1/16 px): nudge it until edge shimmer nulls."
                  ).pack(anchor="w", pady=(4, 0))

    # ---- display framing: vertical scale + h/v position ----
    def _build_display(self, parent):
        g = self._group(parent, "Display framing  (moves the captured image)")
        for attr, cmd, label, lo, hi, default in DISPLAY_KNOBS:
            self._knob_row(g, attr, cmd, label, lo, hi, default)
        ttk.Label(g, wraplength=380, foreground="#888",
                  text="vscale 2 line-doubles, useful for games under "
                       "Hercules that look squat at 1:1"
                  ).pack(anchor="w", pady=(4, 0))

    # ---- MDA grey levels ----
    def _build_levels(self, parent):
        g = self._group(parent, "MDA grey levels  (0..3)")
        row = ttk.Frame(g)
        row.pack(fill="x", pady=1)
        ttk.Label(row, text="Normal / Bright", width=16).pack(side="left")
        self.lvl_normal = self._spin(row, 0, 3, 2, width=4)
        self.lvl_normal.pack(side="left", padx=2)
        self.lvl_bright = self._spin(row, 0, 3, 3, width=4)
        self.lvl_bright.pack(side="left", padx=2)
        self._autowire(self.lvl_normal, self._send_levels)
        self._autowire(self.lvl_bright, self._send_levels)
        ttk.Button(row, text="Set", width=5, command=self._send_levels).pack(side="left", padx=4)

    def _send_levels(self):
        self.worker.send("mdalevels %d %d" % (self._int(self.lvl_normal, 2),
                                              self._int(self.lvl_bright, 3)),
                         on_done=self._on_levels)

    def _on_levels(self, ok, text):
        normal, bright = parse_levels(text)
        self._set_widget(self.lvl_normal, normal)
        self._set_widget(self.lvl_bright, bright)

    # ---- saved profiles (flash) ----
    def _build_profiles(self, parent):
        g = self._group(parent, "Profiles  (flash -- every write reboots)")
        row = ttk.Frame(g)
        row.pack(fill="x", pady=1)
        ttk.Label(row, text="Slot", width=5).pack(side="left")
        self.slot_spin = self._spin(row, 0, 3, 0, width=3)
        self.slot_spin.pack(side="left", padx=2)
        ttk.Label(row, text="Name").pack(side="left", padx=(8, 2))
        self.slot_name = ttk.Entry(row, width=14)
        self.slot_name.pack(side="left", padx=2)

        row = ttk.Frame(g)
        row.pack(fill="x", pady=(4, 1))
        ttk.Button(row, text="Save", width=8, command=self._save_slot).pack(side="left", padx=2)
        ttk.Button(row, text="Load", width=8, command=self._load_slot).pack(side="left", padx=2)
        ttk.Button(row, text="Clear", width=8, command=self._clear_slot).pack(side="left", padx=2)
        ttk.Button(row, text="Boot", width=8, command=self._boot_slot).pack(side="left", padx=2)

        row = ttk.Frame(g)
        row.pack(fill="x", pady=1)
        ttk.Button(row, text="List", width=8,
                   command=lambda: self.worker.send("slots")).pack(side="left", padx=2)
        ttk.Button(row, text="Boot off", width=8,
                   command=lambda: self.send_maybe_rebooting("boot off",
                                                             "Disabling auto-load ...")
                   ).pack(side="left", padx=2)
        ttk.Button(row, text="Dump", width=8,
                   command=lambda: self.worker.send("dump")).pack(side="left", padx=2)

        ttk.Label(g, wraplength=380, foreground="#888",
                  text="Save stores the CURRENT settings (mode, bp/phase, "
                       "vscale/vpos/hpos, levels). Boot picks the profile applied "
                       "at power-on. Dump prints a hex backup -- copy it from the "
                       "console and paste it into `restore <hex>` to recover."
                  ).pack(anchor="w", pady=(4, 0))

    def _slot(self):
        return self._int(self.slot_spin, 0)

    def _save_slot(self):
        n = self._slot()
        name = self.slot_name.get().strip()
        self.send_maybe_rebooting("save %d %s" % (n, name) if name else "save %d" % n,
                                  "Saving slot %d ..." % n)

    def _load_slot(self):
        n = self._slot()
        self.send_maybe_rebooting("load %d" % n, "Loading slot %d ..." % n)

    def _clear_slot(self):
        n = self._slot()
        self.send_maybe_rebooting("clear %d" % n, "Clearing slot %d ..." % n)

    def _boot_slot(self):
        n = self._slot()
        self.send_maybe_rebooting("boot %d" % n, "Setting boot slot %d ..." % n)

    # ---- test patterns ----
    def _build_test(self, parent):
        g = self._group(parent, "Test pattern  (needs NO input -- capture overwrites it)")
        row = ttk.Frame(g)
        row.pack(fill="x")
        for label, arg in TESTS:
            ttk.Button(row, text=label, width=13,
                       command=lambda a=arg: self.worker.send(
                           "test %s" % a, timeout=TEST_TIMEOUT)).pack(side="left", padx=2)

    # ---- diagnostics ----
    def _build_diag(self, parent):
        g = self._group(parent, "Diagnostics")
        row = ttk.Frame(g)
        row.pack(fill="x", pady=1)
        ttk.Button(row, text="Status", width=9,
                   command=lambda: self.worker.send("status", on_done=self._on_status)).pack(side="left", padx=2)
        ttk.Button(row, text="Version", width=9,
                   command=lambda: self.worker.send("version")).pack(side="left", padx=2)
        ttk.Button(row, text="Modes", width=9,
                   command=lambda: self.worker.send("mode", on_done=self._on_modes)).pack(side="left", padx=2)
        row = ttk.Frame(g)
        row.pack(fill="x", pady=1)
        ttk.Button(row, text="Reboot", width=9,
                   command=self._reboot).pack(side="left", padx=2)
        ttk.Button(row, text="BOOTSEL", width=9,
                   command=lambda: self.worker.send("bootsel", quiet=True)).pack(side="left", padx=2)
        ttk.Button(row, text="Re-read all", width=9,
                   command=self._refresh_all).pack(side="left", padx=2)

        ttk.Label(g, wraplength=380, foreground="#888",
                  text="Frame grab is a large dump - use tools/grab.py (close "
                       "this panel first; it holds the port)."
                  ).pack(anchor="w", pady=(4, 0))

    def _reboot(self):
        self.send_rebooting("reboot", "Rebooting (USB will drop) ...")

    # ---- sync every control from the device ----
    def _refresh_all(self):
        for attr, cmd, _label, _lo, _hi, _d in CAPTURE_KNOBS + DISPLAY_KNOBS:
            self.worker.send(cmd, quiet=True,
                             on_done=lambda ok, t, a=attr, c=cmd: self._on_knob(a, c, t))
        self.worker.send("mdalevels", on_done=self._on_levels, quiet=True)
        self.worker.send("mode", on_done=self._on_modes, quiet=True)

    # ---- raw command (follows the console when detached) ----
    def _build_raw(self, parent):
        self._build_raw_row(parent)

    def _build_raw_row(self, parent):
        row = ttk.Frame(parent)
        row.pack(fill="x", pady=(6, 2))
        ttk.Label(row, text="Raw command").pack(side="left")
        entry = ttk.Entry(row)
        entry.pack(side="left", fill="x", expand=True, padx=6)
        entry.bind("<Return>", lambda _e: self.send_raw())
        entry.bind("<Up>", lambda _e: self._history_recall(-1))
        entry.bind("<Down>", lambda _e: self._history_recall(+1))
        ttk.Button(row, text="Send", command=self.send_raw).pack(side="left")
        self.raw = entry
        return entry

    def send_raw(self):
        cmd = self.raw.get().strip()
        if not cmd:
            return
        if not self.history or self.history[-1] != cmd:
            self.history.append(cmd)
        self.hist_pos = len(self.history)
        self.raw.delete(0, "end")
        # A hand-typed rebooting command needs the same treatment as the buttons,
        # or it reports "[Errno 6] Device not configured" and sits disconnected.
        # All of them only act (and so only reboot) WITH an argument -- bare
        # `mode`/`boot`/`load` just query -- except `reboot` itself.
        parts = cmd.split()
        head = parts[0]
        has_arg = len(parts) >= 2
        if head in REBOOT_CMDS and (head == "reboot" or has_arg):
            self.send_rebooting(cmd, ">> %s  (reboots)" % cmd)
        elif head in MAYBE_REBOOT_CMDS and has_arg:
            self.send_maybe_rebooting(cmd, ">> %s" % cmd)
        else:
            self.worker.send(cmd, on_done=self._on_raw_done,
                             timeout=TEST_TIMEOUT if head == "test" else 2.0)

    def _on_raw_done(self, ok, text):
        """A hand-typed command may have changed anything -- refresh what it
        touched rather than guessing, so the spinners never drift from truth."""
        self._on_status(ok, text)
        st = parse_status(text)
        if st is None and text:
            for attr, cmd, _l, _lo, _hi, _d in CAPTURE_KNOBS + DISPLAY_KNOBS:
                if parse_kv(text, cmd) is not None:
                    self._on_knob(attr, cmd, text)
            if "normal=" in text:
                self._on_levels(ok, text)
            if "current" in text:
                self._on_modes(ok, text)

    def _history_recall(self, step):
        if not self.history:
            return "break"
        self.hist_pos = max(0, min(len(self.history), self.hist_pos + step))
        self.raw.delete(0, "end")
        if self.hist_pos < len(self.history):
            self.raw.insert(0, self.history[self.hist_pos])
        return "break"

    # ---- console (attachable/detachable) ----
    def _build_console(self, parent):
        row = ttk.Frame(parent)
        row.pack(fill="x")
        ttk.Label(row, text="Console").pack(side="left")
        ttk.Button(row, text="Clear", command=self.clear_console).pack(side="right")
        ttk.Button(row, text="Detach", command=self.detach_console).pack(side="right", padx=4)
        self.console_embedded = ScrolledText(parent, height=8, state="disabled", wrap="word")
        self.console_embedded.pack(fill="both", expand=True, pady=(2, 0))
        self.console = self.console_embedded
        self.raw_embedded = self.raw

    def detach_console(self):
        if self.console_win is not None:
            self.console_win.lift()
            return
        win = tk.Toplevel(self.root)
        win.title("ttl2dvi console")
        win.geometry("820x360")
        win.minsize(400, 150)

        row = ttk.Frame(win, padding=(6, 6, 6, 0))
        row.pack(fill="x")
        ttk.Label(row, text="Console").pack(side="left")
        ttk.Button(row, text="Clear", command=self.clear_console).pack(side="right")
        ttk.Button(row, text="Attach", command=self.attach_console).pack(side="right", padx=4)
        if not hasattr(self, "console_topmost"):
            self.console_topmost = tk.BooleanVar(value=False)
        ttk.Checkbutton(row, text="Always on top", variable=self.console_topmost,
                        command=lambda: win.attributes("-topmost", self.console_topmost.get())
                        ).pack(side="right", padx=4)
        win.attributes("-topmost", self.console_topmost.get())

        raw_holder = ttk.Frame(win, padding=(6, 0))
        raw_holder.pack(fill="x")
        pending = self.raw.get()
        detached_raw = self._build_raw_row(raw_holder)
        detached_raw.insert(0, pending)

        detached = ScrolledText(win, state="disabled", wrap="word")
        detached.pack(fill="both", expand=True, padx=6, pady=6)
        self._copy_console_text(self.console_embedded, detached)
        self.console = detached
        self.console_win = win

        self.paned.forget(self.bottom)
        win.protocol("WM_DELETE_WINDOW", self.attach_console)
        detached_raw.focus_set()

    def attach_console(self):
        if self.console_win is None:
            return
        self._copy_console_text(self.console, self.console_embedded)
        self.console = self.console_embedded
        pending = self.raw.get()
        self.raw = self.raw_embedded
        self.raw.delete(0, "end")
        self.raw.insert(0, pending)
        win, self.console_win = self.console_win, None
        win.destroy()
        self.paned.add(self.bottom, weight=1)
        self.root.lift()
        self.root.focus_force()
        self.raw.focus_set()

    @staticmethod
    def _copy_console_text(src, dst):
        text = src.get("1.0", "end-1c")
        dst.config(state="normal")
        dst.delete("1.0", "end")
        dst.insert("end", text)
        dst.see("end")
        dst.config(state="disabled")

    def clear_console(self):
        self.console.config(state="normal")
        self.console.delete("1.0", "end")
        self.console.config(state="disabled")

    def log(self, text):
        _ui_call(self._log_ui, text)

    def _log_ui(self, text):
        self.console.config(state="normal")
        self.console.insert("end", text.rstrip("\n") + "\n")
        self.console.see("end")
        self.console.config(state="disabled")

    def on_close(self):
        self.worker.stop()
        self.root.destroy()


def main():
    ap = argparse.ArgumentParser(description="ttl2dvi live control panel")
    ap.add_argument("-p", "--port", default=DEFAULT_PORT,
                    help="serial port shown in the Port box (default: %(default)s)")
    ap.add_argument("--no-scan", action="store_true",
                    help="start with auto-detect off (open the port directly)")
    args = ap.parse_args()

    root = tk.Tk()
    app = App(root, default_port=args.port)
    if args.no_scan:
        app.no_scan.set(True)
    root.mainloop()


if __name__ == "__main__":
    main()
