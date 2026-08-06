#ifndef TTL2DVI_CAPTURE_H
#define TTL2DVI_CAPTURE_H

#include <stdbool.h>
#include <stdint.h>
#include "pico/types.h"     // uint

// Frame capture engine (pio0, core0). Samples VIDEO+INTENSITY at 2 bits/pixel
// with per-line HSYNC re-lock; the window is sized each grab from the MEASURED
// line (so text and this card's shorter graphics line both fit). Depends on
// sync_init() having run (it uses the sync SMs to measure the line).
//
// Consumers: the view/render module (live -> DVI framebuffer) and
// capture_dump_frame() (USB dump -> host PNG via tools/grab.py).

// Upper bound on capture_width() -- size row buffers passed to capture_get_line
// with this (the actual width is measured per grab and is <= this).
#define CAPTURE_MAX_WIDTH 864

// Claim the sampler SM + a DMA channel on pio0. Call once, after sync_init()
// and after the overclock (video_init()).
void capture_init(void);

// Grab one VSYNC-bounded frame into the internal buffer. Returns false on a
// no-signal timeout. After this, capture_width/height and capture_get_line
// describe/read the grabbed frame.
bool capture_grab(void);

// Geometry of the last successful grab (the source frame, ~720 x ~369).
uint capture_width(void);
uint capture_height(void);

// Horizontal framing / sampling-instant knobs (console: `bp`, `phase`), applied
// on the next grab. Split by unit: bp slides the capture window by WHOLE source
// pixels, phase moves the sampling instant WITHIN a pixel by one sysclk cycle
// at a time (capture_px_cyc() steps per pixel = 16 at 256 MHz / 16 MHz MDA).
// Phase is the "avoid sampling on the pixel transition" knob: nudge it until
// edge shimmer nulls; it is clamped to [0, capture_px_cyc()-1].
void capture_set_bp(int bp);
int  capture_get_bp(void);
void capture_set_phase(int phase);
int  capture_get_phase(void);
uint capture_px_cyc(void);

// Reconstruct one source line (0..height-1) into dst[0..width-1]. Values are
// capture_data_bits() wide: MDA 0..3 (VIDEO | INTENSITY<<1), CGA640 0..15
// (I | R<<1 | G<<2 | B<<3). dst must hold at least capture_width() bytes.
void capture_get_line(uint line, uint8_t *dst);

// Bits per sample for the active card: 2 (MDA, gray) or 4 (CGA640, RGBI).
// Fixed for the whole boot -- a card switch reboots.
uint capture_data_bits(void);

// Dot clock (console `dotclock`), defaulting to the active card's
// dot_clock_hz. Trim it for a card whose crystal differs from its profile's:
// it is the ONLY knob that scales horizontally, so a wrong-width image can be
// fixed no other way. Saved in a profile slot. Applies on the next grab.
void     capture_set_dot_hz(uint32_t hz);
uint32_t capture_get_dot_hz(void);

// Grab a frame and dump it over stdout between "@@@BEGIN" and "@@@END": a header
// "W <w> H <h>" then <h> rows of <w> digits (the 2-bit samples). Prints an error
// line instead if there is no sync signal.
void capture_dump_frame(void);

#endif
