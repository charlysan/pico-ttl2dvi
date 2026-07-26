#ifndef TTL2DVI_CAPTURE_H
#define TTL2DVI_CAPTURE_H

// Frame capture engine (pio0, core0). Samples VIDEO+INTENSITY at 2 bits/pixel
// with per-line HSYNC re-lock; the window is sized each grab from the MEASURED
// line (so text and this card's shorter graphics line both fit). Depends on
// sync_init() having run (it uses the sync SMs to measure the line).
//
// For now the only consumer is capture_dump_frame() (USB dump -> host PNG via
// tools/grab.py). Later the same engine reconstructs into the DVI framebuffer.

// Claim the sampler SM + a DMA channel on pio0. Call once, after sync_init()
// and after the overclock (video_init()).
void capture_init(void);

// Grab one VSYNC-bounded frame and dump it over stdout between "@@@BEGIN" and
// "@@@END": a header line "W <w> H <h>" then <h> rows of <w> digits, each digit
// the 2-bit sample (0..3 = VIDEO | INTENSITY<<1). Prints an error line instead
// if there is no sync signal.
void capture_dump_frame(void);

#endif
