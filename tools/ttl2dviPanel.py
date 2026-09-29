#!/usr/bin/env python3
"""ttl2dvi control panel: a Tk GUI for the firmware's USB serial console.

Run:  python3 ttl2dviPanel.py [-p /dev/tty.usbmodem1101]
Needs pyserial and tkinter.
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

PANEL_VERSION = "v1.0"
PROMPT = "ttl2dvi>"
BAUD = 115200
DEFAULT_PORT = "/dev/tty.usbmodem1101"
REPLY_TIMEOUT = 3.0       # measure/detect can be silent for ~0.5 s
RECONNECT_MS = 3500       # time for the device to re-enumerate after a reboot

CAPTURE_KNOBS = [
    ("bp",    "bp",    "Back porch  (bp)",       0, 255, 16),
    ("phase", "phase", "Phase  (sysclk)",        0,  31,  0),
]
DISPLAY_KNOBS = [
    ("vscale", "vscale", "Vertical scale",          1,   4, 1),
    ("vpos",   "vpos",   "V position  (+ = down)", -400, 400, 0),
    ("hpos",   "hpos",   "H position  (+ = right)", -400, 400, 0),
]
SLOTS = 8


def parse_state(text):
    """The `state` line -> dict of strings, or None."""
    for ln in (text or "").splitlines():
        parts = ln.split()
        if len(parts) > 1 and parts[0] == "state":
            return dict(kv.split("=", 1) for kv in parts[1:] if "=" in kv)
    return None


def parse_list(text):
    """`mode` / `source` listings -> (names by index, current index)."""
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
    return bool(re.search(r"\bttl2dvi\b", text or "", re.I))


def reboots(cmd):
    """'always', 'maybe' (load: only if the slot's mode differs) or None."""
    parts = cmd.split()
    if len(parts) < 2 or parts[1:] == ["auto"]:
        return None
    if parts[0] in ("mode", "source", "save", "clear", "default"):
        return "always"
    if parts[0] == "load":
        return "maybe"
    return None


class SerialWorker:
    """Owns the port on one background thread, so the UI never blocks and
    commands never overlap on the wire."""

    def __init__(self, log_fn, status_fn):
        self._log = log_fn
        self._status = status_fn
        self._jobs = queue.Queue()
        self._port = None
        self._running = True
        threading.Thread(target=self._loop, daemon=True).start()

    def _converse(self, cmd, timeout=REPLY_TIMEOUT):
        ser = self._port
        ser.reset_input_buffer()
        ser.write((cmd + "\r").encode("ascii"))
        buf = bytearray()
        start = time.time()
        while time.time() - start < timeout:
            n = ser.in_waiting
            if n:
                buf += ser.read(n)
                tail = bytes(buf[-len(PROMPT) - 4:]).decode("ascii", "replace")
                if tail.rstrip().endswith(PROMPT):
                    break
            else:
                time.sleep(0.01)
        return buf.decode("ascii", "replace")

    def _parse(self, cmd, raw):
        idx = raw.rfind(PROMPT)
        if idx != -1:
            raw = raw[:idx]
        body = []
        for ln in raw.replace("\r", "").split("\n"):
            s = ln.rstrip()
            if s.strip().startswith(PROMPT):
                s = s.strip()[len(PROMPT):]
            if not s.strip() or s.strip() == cmd:
                continue
            body.append(s)
        return "\n".join(body)

    def connect(self, port=None, auto_detect=True):
        self._jobs.put(("connect", (port, auto_detect)))

    def send(self, cmd, on_done=None, quiet=False, rebooting=False):
        self._jobs.put(("cmd", (cmd, on_done, quiet, rebooting)))

    def stop(self):
        self._running = False
        self._jobs.put(("quit", None))

    @property
    def connected(self):
        return self._port is not None

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

    def _do_connect(self, port, auto_detect):
        self._close()
        if not auto_detect:
            if not port:
                self._log("No port set")
                self._status(False, "")
                return
            try:
                self._port = self._open(port)
            except Exception as e:
                self._log("Cannot open %s: %s" % (port, e))
                self._status(False, "")
                return
            self._log("Connected on %s" % port)
            self._status(True, port)
            return
        candidates = ([port] if port else []) + [
            p.device for p in list_ports.comports()
            if p.device != port and "bluetooth" not in p.device.lower()]
        for cand in candidates:
            self._log("Probing %s ..." % cand)
            if self._try_port(cand):
                self._log("Connected on %s" % cand)
                self._status(True, cand)
                return
        self._log("No ttl2dvi device found")
        self._status(False, "")

    def _do_cmd(self, cmd, on_done, quiet, rebooting):
        if self._port is None:
            if not quiet:
                self._log("! Not connected: %s" % cmd)
            if on_done:
                _ui_call(on_done, False, "")
            return
        if not quiet:
            self._log(">> %s" % cmd)
        try:
            raw = self._converse(cmd)
        except Exception as e:
            if rebooting:
                self._log("* device rebooting, reconnecting...")
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


_root = None


def _ui_call(fn, *args):
    if _root is not None:
        _root.after(0, lambda: fn(*args))


class App:
    def __init__(self, root, default_port=DEFAULT_PORT):
        global _root
        _root = root
        self.root = root
        self.default_port = default_port
        self.worker = SerialWorker(self.log, self.set_status)
        self._poll_busy = False
        self.console_win = None
        self.history = []
        self.hist_pos = 0
        self.spins = {}
        self.mode_buttons, self.mode_names, self.cur_mode = [], [], None
        self.source_buttons, self.source_names, self.cur_src = [], [], None

        root.title("ttl2dvi control panel  %s" % PANEL_VERSION)
        root.minsize(620, 700)
        outer = ttk.Frame(root, padding=8)
        outer.pack(fill="both", expand=True)

        self._build_status_bar(outer)

        self.paned = ttk.PanedWindow(outer, orient="vertical")
        self.paned.pack(fill="both", expand=True)
        top = ttk.Frame(self.paned)
        self.bottom = ttk.Frame(self.paned)
        self.paned.add(top, weight=0)
        self.paned.add(self.bottom, weight=1)

        left = ttk.Frame(top)
        left.pack(side="left", fill="y", padx=(0, 4))
        right = ttk.Frame(top)
        right.pack(side="left", fill="both", expand=True, padx=(4, 0))

        self._build_readings(left)
        self._build_source(left)
        self._build_mode(left)
        self._build_capture(left)

        self._build_display(right)
        self._build_levels(right)
        self._build_slots(right)
        self._build_diag(right)

        self._build_raw_row(self.bottom)
        self._build_console(self.bottom)

        root.protocol("WM_DELETE_WINDOW", self.on_close)
        root.after(200, self.do_connect)
        root.after(1500, self._poll_tick)

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
        """Arrows send when Auto-set is on; Enter always sends."""
        spin.configure(command=lambda: self.auto_set.get() and send_fn())
        spin.bind("<Return>", lambda _e: send_fn())

    def _set_widget(self, widget, value):
        """Update from the device, but not while the user is editing it."""
        if value is None:
            return
        try:
            if self.root.focus_get() is widget:
                return
        except (KeyError, tk.TclError):
            pass
        if str(widget.get()) != str(value):
            widget.set(str(value))

    def _knob_row(self, parent, attr, cmd, label, lo, hi, default, width=24):
        row = ttk.Frame(parent)
        row.pack(fill="x", pady=1)
        ttk.Label(row, text=label, width=width).pack(side="left")
        spin = self._spin(row, lo, hi, default)
        spin.pack(side="left", padx=2)
        self.spins[attr] = spin
        send = lambda: self.send_cmd("%s %d" % (cmd, self._int(self.spins[attr], default)))
        self._autowire(spin, send)
        ttk.Button(row, text="Set", width=5, command=send).pack(side="left", padx=4)

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
        self.auto_detect = tk.BooleanVar(value=True)
        ttk.Checkbutton(bar, text="Auto-detect", variable=self.auto_detect).pack(side="right", padx=6)
        self.port_entry = ttk.Entry(bar, width=22)
        self.port_entry.insert(0, self.default_port)
        self.port_entry.pack(side="right")
        ttk.Label(bar, text="Port:").pack(side="right", padx=(0, 3))

    def do_connect(self):
        self.worker.connect(port=self.port_entry.get().strip(),
                            auto_detect=self.auto_detect.get())

    def set_status(self, connected, port):
        if connected:
            self.dot.config(foreground="#27ae60")
            self.status_label.config(text="Connected  %s" % port)
            self.root.after(0, self._refresh_all)
        else:
            self.dot.config(foreground="#c0392b")
            self.status_label.config(text="Disconnected")

    # ---- status + poller ----
    def _build_readings(self, parent):
        g = self._group(parent, "Status")
        top = ttk.Frame(g)
        top.pack(fill="x")
        self.poll_enabled = tk.BooleanVar(value=True)
        ttk.Checkbutton(top, text="Poll every", variable=self.poll_enabled).pack(side="left")
        self.poll_int = self._spin(top, 1, 30, 2, width=3)
        self.poll_int.pack(side="left", padx=2)
        ttk.Label(top, text="s").pack(side="left")

        grid = ttk.Frame(g)
        grid.pack(fill="x", pady=(4, 0))
        self.meas = {}
        for i, (label, key) in enumerate([("Source", "source"), ("HSYNC", "hsync"),
                                          ("VSYNC", "vsync"), ("Signal", "signal"),
                                          ("Mode", "mode")]):
            ttk.Label(grid, text=label + ":").grid(row=i, column=0, sticky="w",
                                                   padx=(0, 4), pady=1)
            v = ttk.Label(grid, text="-")
            v.grid(row=i, column=1, sticky="w", pady=1)
            self.meas[key] = v

    def _poll_tick(self):
        if self.worker.connected and self.poll_enabled.get() and not self._poll_busy:
            self._poll_busy = True
            self.request_state()
        interval = max(1, self._int(self.poll_int, 2))
        self.root.after(interval * 1000, self._poll_tick)

    def request_state(self):
        self.worker.send("state", on_done=self._on_state, quiet=True)

    def _on_state(self, ok, text):
        """Every reading and control, from one `state` line."""
        self._poll_busy = False
        s = parse_state(text)
        if not s:
            return
        num = lambda k, d=0: int(s[k]) if s.get(k, "").lstrip("-").isdigit() else d

        src, mode = num("src", -1), num("mode", -1)
        name = self.source_names[src] if 0 <= src < len(self.source_names) else "?"
        self.meas["source"].config(text="%s, %s-bit, %sx" % (name, s.get("bits", "?"),
                                                           s.get("os", "?")))
        live = s.get("sig", "none") != "none"
        self.meas["hsync"].config(text="%s Hz" % s["hs"] if live else "no signal")
        self.meas["vsync"].config(text="%s Hz" % s["vs"] if live else "no signal")
        self.meas["signal"].config(text=s.get("sig", "-"))
        if 0 <= mode < len(self.mode_names):
            self.meas["mode"].config(text=self.mode_names[mode] or "-")

        if src != self.cur_src and self.source_names:
            self.cur_src = src
            self.source_buttons = self._rebuild(self.source_row, self.source_buttons,
                                                self.source_names, src, "source", 6)
        if mode != self.cur_mode and self.mode_names:
            self.cur_mode = mode
            self.mode_buttons = self._rebuild(self.mode_row, self.mode_buttons,
                                              self.mode_names, mode, "mode", 10)

        self.spins["phase"].configure(to=max(0, num("pxcyc", 32) - 1))
        for attr in ("bp", "phase", "vscale", "vpos", "hpos"):
            self._set_widget(self.spins[attr], num(attr))
        self._set_widget(self.dot_spin, "%.4f" % (num("dot") / 1e6))
        lvl = s.get("lvl", "2,3").split(",")
        self._set_widget(self.lvl_normal, lvl[0])
        self._set_widget(self.lvl_bright, lvl[-1])
        self._set_var(self.scanlines, num("scan") == 1)
        self._set_var(self.source_auto, num("auto") == 1)
        d = num("def", -1)
        self.default_label.config(text="Default: %s" % (d if d >= 0 else "none"))

    @staticmethod
    def _set_var(var, value):
        if var.get() != value:
            var.set(value)

    # ---- rebooting commands ----
    def send_cmd(self, cmd, note=None):
        """Send anything, handling the commands that reboot the device."""
        kind = reboots(cmd)
        if note:
            self.log(note)
        if kind == "always":
            self.worker.send(cmd, rebooting=True)
            self.root.after(RECONNECT_MS, self.do_connect)
        elif kind == "maybe":
            self.worker.send(cmd, rebooting=True, on_done=self._on_maybe_reboot)
        else:
            self.worker.send(cmd, on_done=self._on_reply)

    def _on_maybe_reboot(self, ok, text):
        if ok:
            self.request_state()
        else:
            self.root.after(RECONNECT_MS, self.do_connect)

    # ---- source ----
    def _build_source(self, parent):
        g = self._group(parent, "Source  (switching reboots)")
        self.source_row = ttk.Frame(g)
        self.source_row.pack(fill="x")
        self.source_auto = tk.BooleanVar(value=False)
        ttk.Checkbutton(g, text="Auto", variable=self.source_auto,
                        command=lambda: self.send_cmd(
                            "source auto %s" % ("on" if self.source_auto.get() else "off"))
                        ).pack(anchor="w", pady=(4, 0))

    def _on_sources(self, ok, text):
        names, current = parse_list(text)
        if names:
            self.source_names, self.cur_src = names, current
            self.source_buttons = self._rebuild(self.source_row, self.source_buttons,
                                                names, current, "source", 6)

    # ---- mode ----
    def _build_mode(self, parent):
        g = self._group(parent, "Output mode  (switching reboots)")
        self.mode_row = ttk.Frame(g)
        self.mode_row.pack(fill="x")

    def _on_modes(self, ok, text):
        names, current = parse_list(text)
        if names:
            self.mode_names, self.cur_mode = names, current
            self.mode_buttons = self._rebuild(self.mode_row, self.mode_buttons,
                                              names, current, "mode", 10)

    def _rebuild(self, row, old, names, current, cmd, width):
        for b in old:
            b.destroy()
        buttons = []
        for i, name in enumerate(names):
            if name is None:
                continue
            b = ttk.Button(row, text=name, width=width,
                           command=lambda n=i, s=name: self.send_cmd(
                               "%s %d" % (cmd, n), "Switching to %s ..." % s))
            b.pack(side="left", padx=2)
            if i == current:
                b.state(["disabled"])
            buttons.append(b)
        return buttons

    # ---- capture ----
    def _build_capture(self, parent):
        g = self._group(parent, "Capture")
        for attr, cmd, label, lo, hi, default in CAPTURE_KNOBS:
            self._knob_row(g, attr, cmd, label, lo, hi, default, width=16)

        row = ttk.Frame(g)
        row.pack(fill="x", pady=1)
        ttk.Label(row, text="Dot clock  (MHz)", width=16).pack(side="left")
        self.dot_spin = ttk.Spinbox(row, width=9, from_=1.0, to=100.0,
                                    increment=0.0001, format="%.4f")
        self.dot_spin.set("16.0000")
        self.dot_spin.pack(side="left", padx=2)
        self._autowire(self.dot_spin, self._send_dotclock)
        ttk.Button(row, text="Set", width=5, command=self._send_dotclock).pack(side="left", padx=4)
        ttk.Button(row, text="Default", width=7,
                   command=lambda: self.send_cmd("dotclock default")).pack(side="left")

    def _send_dotclock(self):
        try:
            mhz = float(self.dot_spin.get())
        except (ValueError, tk.TclError):
            return
        self.send_cmd("dotclock %.4f" % mhz)

    # ---- display ----
    def _build_display(self, parent):
        g = self._group(parent, "Display")
        for attr, cmd, label, lo, hi, default in DISPLAY_KNOBS:
            self._knob_row(g, attr, cmd, label, lo, hi, default)
        self.scanlines = tk.BooleanVar(value=False)
        ttk.Checkbutton(g, text="Scanlines", variable=self.scanlines,
                        command=lambda: self.send_cmd(
                            "scanlines %s" % ("on" if self.scanlines.get() else "off"))
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
        self.send_cmd("mdalevels %d %d" % (self._int(self.lvl_normal, 2),
                                           self._int(self.lvl_bright, 3)))

    # ---- slots ----
    def _build_slots(self, parent):
        g = self._group(parent, "Slots  (save, clear and default reboot)")
        row = ttk.Frame(g)
        row.pack(fill="x", pady=1)
        ttk.Label(row, text="Slot", width=5).pack(side="left")
        self.slot_spin = self._spin(row, 0, SLOTS - 1, 0, width=3)
        self.slot_spin.pack(side="left", padx=2)
        ttk.Label(row, text="Name").pack(side="left", padx=(8, 2))
        self.slot_name = ttk.Entry(row, width=10)
        self.slot_name.pack(side="left", padx=2)

        row = ttk.Frame(g)
        row.pack(fill="x", pady=(4, 1))
        for label, fn in [("Save", self._save_slot), ("Load", lambda: self._slot_cmd("load")),
                          ("Clear", lambda: self._slot_cmd("clear")),
                          ("Default", lambda: self._slot_cmd("default"))]:
            ttk.Button(row, text=label, width=8, command=fn).pack(side="left", padx=2)

        row = ttk.Frame(g)
        row.pack(fill="x", pady=1)
        ttk.Button(row, text="List", width=8,
                   command=lambda: self.send_cmd("slots")).pack(side="left", padx=2)
        ttk.Button(row, text="Default off", width=10,
                   command=lambda: self.send_cmd("default off")).pack(side="left", padx=2)
        self.default_label = ttk.Label(row, text="Default: -")
        self.default_label.pack(side="left", padx=8)

    def _save_slot(self):
        n = self._int(self.slot_spin, 0)
        name = self.slot_name.get().strip()
        self.send_cmd("save %d %s" % (n, name) if name else "save %d" % n)

    def _slot_cmd(self, cmd):
        self.send_cmd("%s %d" % (cmd, self._int(self.slot_spin, 0)))

    # ---- diagnostics ----
    def _build_diag(self, parent):
        g = self._group(parent, "Diagnostics")
        row = ttk.Frame(g)
        row.pack(fill="x", pady=1)
        for label, cmd in [("Status", "status"), ("Detect", "detect"),
                           ("Measure", "measure"), ("Version", "version")]:
            ttk.Button(row, text=label, width=9,
                       command=lambda c=cmd: self.send_cmd(c)).pack(side="left", padx=2)

    # ---- sync the controls from the device ----
    def _refresh_all(self):
        """On connect: the source and mode names (fixed until a reboot), then
        everything else from `state`."""
        self.worker.send("source", on_done=self._on_sources, quiet=True)
        self.worker.send("mode", on_done=self._on_modes, quiet=True)
        self.request_state()

    def _on_reply(self, ok, text):
        """After any command, re-read `state` so the controls follow it."""
        if ok:
            self.request_state()

    # ---- raw command ----
    def _build_raw_row(self, parent):
        row = ttk.Frame(parent)
        row.pack(fill="x", pady=(6, 2))
        ttk.Label(row, text="Command").pack(side="left")
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
        self.send_cmd(cmd)

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
    ap = argparse.ArgumentParser(description="ttl2dvi control panel")
    ap.add_argument("-p", "--port", default=DEFAULT_PORT,
                    help="serial port shown in the Port box (default: %(default)s)")
    args = ap.parse_args()
    root = tk.Tk()
    App(root, default_port=args.port)
    root.mainloop()


if __name__ == "__main__":
    main()
