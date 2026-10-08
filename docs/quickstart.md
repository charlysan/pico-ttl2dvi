# Quickstart

From power-on to a tuned picture, using the on-screen menu. Wiring is in the
README; details are in `architecture.md`.

## 1. First boot

Power the Pico first, then the PC. Power the PC down first.

The board detects the card and shows a status line once the signal is stable:

```
MDA16    256.0 MHz  H 18140 Hz  V 49.03 Hz
```

Source, system clock, line rate, frame rate. It hides after 5 s.

## 2. Buttons

| button | press | hold |
|---|---|---|
| PREV | previous / -1 | repeats |
| NEXT | next / +1 | repeats |
| ENTER | open / select | back (0.6 s) |
| ENTER + PREV | | reset (2 s) |
| ENTER + NEXT | | USB BOOTSEL (2 s) |

The menu closes after 10 s without input.

## 3. The menu

ENTER opens it. One line, PREV/NEXT to move:

```
Menu      <  Picture       >
```

ENTER on a submenu opens it. Values show on the line:

```
Picture   <  V position       12   >
```

ENTER on a value edits it. PREV/NEXT change it live, ENTER ends:

```
Picture   >  V position   [    12]
```

Anything that writes flash or reboots asks first, with No selected:

```
Save      2  -?  [No]  Yes
```

The tree:

```
Picture   H position · V position · V scale · Scanlines · MDA normal · MDA bright
Sampling  Phase · Back porch · Dot clock · Dot default · Auto tune
Profiles  Load · Save · Clear · Default
Source    Select · Auto detect · Detect now
Output    the source's output modes
OSD       Status · Live · Hold
Remote    Learn
Info      Version · Status
```

## 4. Tune a card

Shimmer, vertical bars, or the wrong width mean the card's clock differs from
the default. Put text or a test pattern on screen.

Sampling > Auto tune:

```
Tune  measuring...
```

```
Tune 16.2600 MHz +1.62%  phase 7  margin 3.8  98 ch  may shimmer  < Apply >
```

- **MHz**: the card's dot clock. **%**: how far from the default.
- **margin**: distance from the samples to the pixel edges. Bigger is better.
- **ch**: characters per line. A whole number means the clock is right.
- **may shimmer**: the clock isn't a whole fraction of the system clock. It
  usually still looks clean.

PREV/NEXT choose Apply, Apply and save, or Cancel.

If the right or left edge is cut, adjust Sampling > Back porch until the
picture fits, then run Auto tune again: the phase depends on it.

## 5. Save it

Apply and save, or Profiles > Save:

```
Tune  Save to slot  <  0  -             >
```

Saving reboots. Profiles > Default applies a slot at every boot.

## 6. Learn a remote

Needs an NEC remote (most cheap ones) and the push buttons.

Remote > Learn. Press the remote key for each action:

```
Learn 1/4  Press PREV on the remote
```

```
Learn 1/4  PREV: 1 key  (F807FF00)  ENTER: next
```

More than one key per action is fine (the remote's LEFT and UP arrows both
for PREV). With the push buttons: ENTER next action, PREV clear this one, hold
ENTER to cancel.

Then PREV, NEXT, ENTER, BACK. After that, optional shortcuts:

```
Shortcuts  <  Done, save         >
Shortcuts  <  OSD live          >
Shortcuts  Press a key for OSD live
```

PREV/NEXT pick, ENTER assigns, then press the remote key. Done, save:

```
Learn  Save remote and reboot?  [No]  Yes
```

On a computer, `tools/irlearn.py` does the same and can map any console
command to a key.

## 7. Reset and BOOTSEL

Hold ENTER, then PREV (reset) or NEXT (BOOTSEL), for 2 s:

```
Reset in 2...  release to cancel
```

BOOTSEL shows the board as a USB drive. Drop a `.uf2` on it to update.

## 8. Live readings

OSD > Live:

```
MDA16    H 18140  V 49.031  L 367  49.0 fps  stable
```

Line rate, frame rate, lines per frame, captured frames per second, signal.
`fps` below `V` means frames are being dropped.

## Console

USB serial, 115200, type `help`. Everything in the menu is there too:

```
status      tune [apply]    save <n> [name]    ir on|off
reboot      bootsel         osd live           scanlines switch
```
