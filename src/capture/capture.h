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

// Reconstruct one source line (0..height-1) into dst[0..width-1] as 2-bit
// values (0..3 = VIDEO | INTENSITY<<1). dst must hold at least capture_width().
void capture_get_line(uint line, uint8_t *dst);

// Grab a frame and dump it over stdout between "@@@BEGIN" and "@@@END": a header
// "W <w> H <h>" then <h> rows of <w> digits (the 2-bit samples). Prints an error
// line instead if there is no sync signal.
void capture_dump_frame(void);

#endif
