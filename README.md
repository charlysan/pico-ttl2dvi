# pico-ttl2dvi
[![Build](https://github.com/charlysan/pico-ttl2dvi/actions/workflows/build_and_publish.yml/badge.svg?branch=main)](https://github.com/charlysan/pico-ttl2dvi/actions/workflows/build_and_publish.yml)

Firmware for the RP2350 that captures **digital TTL video** from retro PC
graphics cards and outputs **DVI/HDMI**, live.

TTL signals go straight into the RP2350, are sampled by PIO, 
reconstructed into a framebuffer, and re-emitted as DVI by 
[PicoDVI](https://github.com/Wren6991/PicoDVI).

Every source is captured at full frame rate, and the firmware can detect which
card is connected and switch to it on its own.

## Supported sources

| Source | Data lines | Dot clock | Output |
|---|---|---|---|
| MDA / Hercules (16 MHz clone crystal) | VIDEO + INTENSITY, 2-bit | 16.000 MHz | 4-level grey |
| CGA, 640-wide modes | RGBI, 4-bit | 14.323 MHz | 16 colours |
| Commodore 128 VDC, 80-column | RGBI, 4-bit | 16.000 MHz | 16 colours |
| EGA, 350-line modes | RGBrgb, 6-bit | 17.750 MHz | 64 colours |
| EGA, 200-line modes | RGBI, 4-bit | 14.161 MHz | 16 colours |

Supports text and graphics modes. CGA and EGA 200-line modes use the standard
CGA palette (including the IBM 5153 "brown" correction). EGA switches between
its 200 and 350-line modes live, following the card.

Colour sources are encoded as RGB222, which gives about 75% of full brightness.
Turn up the monitor's brightness to compensate.

## Wiring

DE-9 from the card, 330 Ω in series on every line, into the RP2350's 5 V-tolerant
inputs.

| Signal | DE-9 | GPIO |
|---|---|---|
| VIDEO (MDA) / Secondary BLUE (EGA) | 7 | 20 |
| INTENSITY / Secondary GREEN (EGA) | 6 | 21 |
| RED | 3 | 22 |
| GREEN | 4 | 23 |
| BLUE | 5 | 24 |
| Secondary RED (EGA) | 2 | 25 |
| VSYNC | 9 | 26 |
| HSYNC | 8 | 27 |
| GND | 1 | GND |

On EGA 350-line modes, pins 2, 6 and 7 carry secondary red, green and blue. In
200-line modes the card drives it like CGA: pin 6 is INTENSITY and pins 2 and 7
are unused. Pin 2 is never ground on EGA, so use only pin 1 for GND.

DVI output pins are per board (`cmake/boards.cmake` + `src/config/board.h`);
on the default **Waveshare RP2350-PiZero** they are GPIO 32-39.

### Direct (resistors only)

The minimal hookup. The 330 Ω limits current into the RP2350 pad's clamp diode
- that diode *is* the "5 V tolerance", and the resistor is what keeps it safe.

```
   graphics card (TTL, 5V)                     RP2350
   ───────────────────────                     ──────
   DE-9 7   VIDEO      ─────[ 330Ω ]────────►  GPIO 20
   DE-9 6   INTENSITY  ─────[ 330Ω ]────────►  GPIO 21
   DE-9 3   RED        ─────[ 330Ω ]────────►  GPIO 22
   DE-9 4   GREEN      ─────[ 330Ω ]────────►  GPIO 23
   DE-9 5   BLUE       ─────[ 330Ω ]────────►  GPIO 24
   DE-9 2   sRED       ─────[ 330Ω ]────────►  GPIO 25
   DE-9 9   VSYNC      ─────[ 330Ω ]────────►  GPIO 26
   DE-9 8   HSYNC      ─────[ 330Ω ]────────►  GPIO 27
   DE-9 1   GND        ────────────────────►   GND
```

With the pad clamping, each asserted line draws a few mA from the card. Power
the Pico first, then the PC; power the PC down first.

### Buffered (74HCT541 + resistors) - recommended, through-hole

A 5 V octal buffer between card and Pico. Its CMOS inputs draw ~0 current, so
the video card's drivers are not loaded, and a fault on the Pico side cannot
reach the card. The 330 Ω moves to the buffer's *outputs*, where 5 V logic meets
the 3.3 V pad.

```
                        ┌───────── 74HCT541 ────────┐
                        │ (5V, 100nF across 19-20p) |
   DE-9 7   VIDEO  ────►│ 2  A1              Y1  18 │──[330Ω]──►  GPIO 20
   DE-9 6   INTEN  ────►│ 3  A2              Y2  17 │──[330Ω]──►  GPIO 21
   DE-9 3   RED    ────►│ 4  A3              Y3  16 │──[330Ω]──►  GPIO 22
   DE-9 4   GREEN  ────►│ 5  A4              Y4  15 │──[330Ω]──►  GPIO 23
   DE-9 5   BLUE   ────►│ 6  A5              Y5  14 │──[330Ω]──►  GPIO 24
   DE-9 9   VSYNC  ────►│ 7  A6              Y6  13 │──[330Ω]──►  GPIO 26
   DE-9 8   HSYNC  ────►│ 8  A7              Y7  12 │──[330Ω]──►  GPIO 27
   DE-9 2   sRED   ────►│ 9  A8              Y8  11 │──[330Ω]──►  GPIO 25
                        │                           │
                 GND ──►│ 1  /OE1          /OE2  19 │◄── GND
                 GND ──►│ 10 GND            Vcc  20 │◄── +5V
                        └───────────────────────────┘

   DE-9 1   GND ─────────────── common ground ──────────────►  GND
```

Both `/OE` pins (1 and 19) must go to GND, and keep one common ground between
card, buffer and Pico.

Use the **HCT** part, not plain HC: its TTL-level input thresholds read a 5 V
TTL high reliably at 5 V Vcc.

### Buffered (74LVC245A, no resistors) - best

The same idea, but powered at **3.3 V**. LVC inputs accept 5.5 V regardless of
Vcc - 5 V tolerance is a designed-in property of the family, so the card connects 
straight in and the outputs are already 3.3 V logic.

```
                    ┌───────── 74LVC245A ─────────┐
                    │ (3.3V, 100nF across 10-20p) │
   DE-9 7  VIDEO ──►│ 2  A1                B1  18 │────►  GPIO 20
   DE-9 6  INTEN ──►│ 3  A2                B2  17 │────►  GPIO 21
   DE-9 3  RED   ──►│ 4  A3                B3  16 │────►  GPIO 22
   DE-9 4  GREEN ──►│ 5  A4                B4  15 │────►  GPIO 23
   DE-9 5  BLUE  ──►│ 6  A5                B5  14 │────►  GPIO 24
   DE-9 9  VSYNC ──►│ 7  A6                B6  13 │────►  GPIO 26
   DE-9 8  HSYNC ──►│ 8  A7                B7  12 │────►  GPIO 27
   DE-9 2  sRED  ──►│ 9  A8                B8  11 │────►  GPIO 25
                    │                             │
            +3V3 ──►│ 1  DIR                      │  DIR high = A→B
             GND ──►│ 19 /OE              Vcc  20 │◄── +3V3 (Pico 3V3 pin)
                    │ 10 GND                      │
                    └─────────────────────────────┘

   DE-9 1  GND ─────────────── common ground ──────────────►  GND
```

`DIR` high and `/OE` low enable the A→B direction; both must be tied, not left
floating. Power it from the Pico's **3V3** pin - the 5 V tolerance depends on
Vcc being 3.3 V.

Beyond dropping the resistors, this also removes the power-sequencing rule: LVC
supports partial power-down, so the card may be live while the Pico is off.

### Controls: buttons and IR remote (optional)

Three push buttons and an IR receiver drive a one-line on-screen menu, so the
box can be tuned without a computer. Both are optional.

| Function | GPIO | Header pin (Pi layout) |
|---|---|---|
| IR receiver output | 6 | 31 |
| Button UP (previous / -1) | 7 | 26 |
| Button DOWN (next / +1) | 8 | 24 |
| Button ENTER (short: select, long: back) | 9 | 21 |

```
   push buttons (each one to GND)              RP2350
   ──────────────────────────────              ──────
          ┌──── UP ────┐
   GND ───┤            ├───────────────────►  GPIO 7
          └─── o  o ───┘
          ┌─── DOWN ───┐
   GND ───┤            ├───────────────────►  GPIO 8
          └─── o  o ───┘
          ┌── ENTER ───┐
   GND ───┤            ├───────────────────►  GPIO 9
          └─── o  o ───┘

   IR receiver (KY-022 / VS1838B)
   ──────────────────────────────
          ┌───────────┐
          │  KY-022   │
          │         S ├─────────────────────►  GPIO 6
          │       VCC ├──────┬──────────────◄  3V3
          │           │     ═╪═ 100nF 
          │         - ├──────┴───────────────  GND
          └───────────┘
```

- **No resistors.** The firmware enables the internal pull-ups, so a button
  reads low while pressed. Wire buttons to **GND**, not to 3V3 with
  pull-downs: RP2350 erratum E9 can latch an input with the internal pull-down
  at ~2 V.
- **Power the IR receiver from 3V3**, not 5 V: its output is pulled up to its
  own supply, so at 3V3 it connects straight to the GPIO.
- **Put a 100 nF capacitor across the receiver's VCC and GND**, right at the
  module. Without it, the switching noise from the video inputs reaches the
  receiver through its supply, and its LED flickers 
  (observed when switching to EGA 350-line (1 px stripes or checkerboards).
- The remote must speak **NEC**, which most cheap remotes do.

To use a remote, either:

- **on the box:** Menu → Remote → Learn, then follow the on-screen prompts
  with the push buttons. It asks for the navigation keys, then offers
  shortcuts from a list (OSD status/live/hide, scanlines, load slot 0-7); or
- **from a computer:** record its buttons with `tools/irlearn.py --learn
  ir.txt`, turn the map into a UF2 with `tools/irmap2uf2.py ir.txt`, and drop
  `irmap.uf2` on the BOOTSEL drive (or `picotool load -f irmap.uf2`). This
  route can put any console command on a key.

Firmware updates leave the map in place.


## PCB Design 

WIP - KiCad through-hole design will be uploaded soon.


## How it works

```
  TTL video ──> [capture] ──> rawbuf ──> [view] ──> framebuffer ──> [video] ──> DVI
                pio0/core0                core0                   pio1/core1
                   ^
                   |
                [sync] ──> [sigcheck] ──> [detect] ──> reboot into another source
             HSYNC/VSYNC   rate check    auto source
             measurement
```

**No CPU sits in any per-line timing path.** PIO waits on sync edges; 
the CPU only acts once per frame, during vertical blanking. Capture and
DVI are on separate PIOs and separate cores, so neither can stall the other.

Core 0 only does the per-frame work: it waits for the VSYNC edge, sizes the
sampling window from the sync measurement, arms the DMA and the sampler, and
when the frame ends reconstructs it with `view`. Per line, the PIO locks to each
HSYNC edge and DMA stores the samples, with no CPU involved.

The source (card descriptor) configures both sides at boot: capture gets the
data pins, dot clock and default `bp`/`phase`; video gets the DVI raster and
encoder.

Per frame:

1. **`sync`** measures the HSYNC period and pulse width with a PIO countdown.
2. **`capture`** sizes its sampling window from that measurement, then a single
   DMA transfer records one VSYNC-bounded frame of raw samples into `rawbuf`.
   Sampling is 1x or 2x, picked per source: 2x when the dot clock is not close
   enough to a whole number of system clocks, so reconstruction can pick the
   sample nearest each pixel's centre rather than its edge.
3. **`view`** reconstructs each line, maps sample values to grey levels or
   RGB222 colours, and writes the result into the framebuffer at the current
   position (`hpos`/`vpos`).
4. **`video`** on core 1 walks the framebuffer, repeats rows for vertical scale,
   adds the letterbox, TMDS-encodes each scanline and feeds the serialiser
   continuously.

**Signal check and auto source.** `sigcheck` checks the HSYNC/VSYNC rates
against known ranges. Once they are stable, `detect` picks the source from the
line rate, and from the measured dot clock when several sources share a line
rate. If it finds a different card, it reboots into that source.

## Modules

| Path | Role |
|---|---|
| `src/config` | Board pins and DVI pin config; the source-card descriptors (pins, dot clock, sysclk, defaults) |
| `src/capture` | PIO sampler + DMA into `rawbuf`; per-line reconstruction; signal check. `capture.pio`, `sync_period.pio` |
| `src/video` | Framebuffer, DVI raster tables, TMDS encoding on core 1, colour tables |
| `src/view` | Scan converter: joins capture and video, owns all display framing |
| `src/settings` | Persistent settings slots in the last flash sector |
| `src/console` | USB CDC command console |
| `apps/ttl2dvi` | The application: startup order + command table (`main.c`), command bodies (`commands.c`), source detection (`detect.c`), OSD messages (`osd.c`), buttons and menu (`buttons.c`, `menu.c`), IR remote and learning (`remote.c`, `learn.c`), auto tune (`tune.c`) |
| `src/ir` | NEC IR decoder on pio2 |
| `apps/dvi_test` | Standalone DVI test card, links libdvi only |
| `extern/dvi` | Vendored PicoDVI (`libdvi`) (fork from [mlorenzati](https://github.com/mlorenzati/PicoDVI)) |
| `tools` | `ttl2dviPanel.py` control panel, `grab.py` frame grabber, `autotune.py` dot clock and phase tuning, `irlearn.py` / `irmap2uf2.py` IR key maps |


Everything that differs between cards - data pins, dot clock, system clock,
framebuffer depth, TMDS encoder, DVI raster - is selected from the source
descriptor at boot. Switching card reboots into the new configuration.

## Console

Connect to the USB serial port (115200 baud, any terminal) and type `help`.

```
source [n]           select the card (reboots)
source auto on|off   automatic source detection (stored in flash)
mode [n]             DVI output raster (reboots)
status / state       signal and settings; state prints one line for tools
bp / phase           capture framing: sampling window, sub-pixel sampling instant
dotclock [MHz]       card dot clock, or default
hpos / vpos          display position
vscale [1..4]        vertical scale
scanlines on|off|switch  blank every other line (vscale >= 2)
mdalevels            MDA grey levels
slots                saved settings for this source
save / load / clear  settings slots in flash (8 per source)
default [n|off]      slot applied at boot
detect / measure     which source auto would pick / measure the dot clock
tune [apply]         measure the dot clock and pick the phase (auto tune)
osd ...              on-screen display; osd live toggles live measurements
ir on|off / ir map   print IR remote codes / list the loaded key map
capture / fastcap    dump a frame over USB (tools/grab.py, tools/autotune.py)
dvi_test             on-screen test pattern
```

Example:
```
screen /dev/tty.usbmodem1101 115200

ttl2dvi> source
  auto detection: on
  0  MDA16  2 data bits @ GP20, 720 px, 16.000 MHz dot, sysclk 256 MHz   <- current
  1  CGA    4 data bits @ GP21, 640 px, 14.322 MHz dot, sysclk 258 MHz
  2  C128   4 data bits @ GP21, 640 px, 16.000 MHz dot, sysclk 256 MHz
  3  EGA    6 data bits @ GP20, 640 px, 17.750 MHz dot, sysclk 266.4 MHz, both families
ttl2dvi> source 1
switching to CGA...
```

## Tuning a card

If a card's crystal differs from the defaults, the picture shimmers or shows
evenly spaced vertical bars. Put text or a test pattern on screen, then:

- **on the box:** Menu → Sampling → Auto tune. It measures the dot clock from
  pixel edges, picks the phase, shows the result, and offers Apply, Apply and
  save to a slot, or Cancel;
- **on the console:** `tune` prints the same result, `tune apply` applies it;
- **from a computer:** `tools/autotune.py` does the same from a `fastcap`
  dump, with an edge histogram and the best phases listed.

Auto tune and `tune` search from -25% to +33% of the current dot clock, so a
card on a different crystal is usually found without changing the source.
If the result isn't a whole fraction of the system clock it says "may
shimmer": it works, but a source built for that clock would be cleaner.
`autotune.py` searches ±5%; for it, set `dotclock` near the card's first.
`bp` (the left edge) is not tuned. Save the result in a slot.

## ttl2dviPanel

`tools/ttl2dviPanel.py` is a Tk GUI over the same console
(`pip install -r tools/requirements.txt`). It polls `state` once a second, so
the knobs follow changes made from the console.

<p align="center">
  <img width="80%" src="./docs/img/control_pannel.jpg">
</p>

## Build

Requires the Pico SDK (2.3+) and CMake:

```sh
mkdir build && cd build
cmake -DTTL_BOARD=pizero ..
make -j
```

Or you can use [Docker](https://github.com/lukstep/raspberry-pi-pico-docker-sdk):

```sh
 docker run -d -it --name pico-sdk \
 --mount type=bind,source=${PWD},target=/home/dev \
 lukstep/raspberry-pi-pico-sdk:v1.2.0
 ```


```sh
docker exec -it pico-sdk sh -c \
"cd /home/dev/build && cmake \
-DTTL_BOARD=pizero \
-DPICO_PLATFORM=rp2350-arm-s .. \
&& make -j\$(nproc)"
```

`TTL_BOARD` selects the hardware profile (`pizero` | `pico2_dvi`) and is the
single source of truth for the board. The output is `build/apps/ttl2dvi/ttl2dvi.uf2`.


Then just upload the .uf2 file to rp2350 (in BOOTSEL mode). For example:
```sh
cp build/apps/ttl2dvi/ttl2dvi.uf2 /Volumes/RP2350
```

## Hardware

Tested on a [Waveshare RP2350-PiZero](https://www.waveshare.com/wiki/RP2350-PiZero) (RP2350B, 48 GPIO, 520 KB SRAM).

## Screenshots

Check [Wiki](https://github.com/charlysan/pico-ttl2dvi/wiki/Pico-TTL2DVI-Intro) for some screenshots.

## Discussion

- [VCFed](https://forum.vcfed.org/index.php?threads/pico-ttl2dvi-convert-ttl-video-to-hdmi.1258490/)

## Acknowledgements

Inspired by [mlorenzati/pico-rgb2hdmi](https://github.com/mlorenzati/pico-rgb2hdmi), which
converts analog RGB to HDMI on an RP2040. 

DVI output is [PicoDVI](https://github.com/Wren6991/PicoDVI) by Luke Wren.

## Licence

MIT.

`extern/dvi` is vendored PicoDVI and keeps its own licence - see
`extern/dvi/LICENSE`.
