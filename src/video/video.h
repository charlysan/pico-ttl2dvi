#ifndef TTL2DVI_VIDEO_H
#define TTL2DVI_VIDEO_H

// DVI output subsystem. Overclocks to 256 MHz and runs PicoDVI on core1 (pio1,
// GPIO base + pin config from board.h), scanning out a 2bpp grayscale
// framebuffer continuously. core0 just writes the framebuffer.
//
// 2bpp = 4 grey levels (tmds_encode_2bpp): 0 black .. 3 white. Later the
// capture engine reconstructs into this framebuffer; for now the only writer
// is the test pattern.

// Bring up the overclock + DVI on core1. Call ONCE from core0, and BEFORE
// stdio_init_all() (it changes clk_sys). Framebuffer starts black.
void video_init(void);

// Fill the framebuffer with 1px alternating black/white vertical stripes -- the
// highest-frequency pattern, the TMDS torture test. Proves the DVI path.
void video_test_pattern(void);

#endif
