#ifndef TTL2DVI_VIDEO_H
#define TTL2DVI_VIDEO_H

// DVI output subsystem. Overclocks to 256 MHz and runs PicoDVI on core1 (pio1,
// GPIO base + pin config from board.h), scanning out a 2bpp grayscale
// framebuffer continuously. core0 just writes the framebuffer.
//
// 2bpp = 4 grey levels (tmds_encode_2bpp): 0 black .. 3 white. Later the
// capture engine reconstructs into this framebuffer;

// Bring up the overclock + DVI on core1. Call ONCE from core0, and BEFORE
// stdio_init_all() (it changes clk_sys). Framebuffer starts black. 
void video_init(void);

// Framebuffer access for the view/render module (core0 writes; core1 reads).
// The buffer is 2bpp packed: 16 pixels/word, each pixel a level 0..3.
// The dimensions are PER-MODE, so read them at run time
uint32_t *video_framebuffer(void);   // base pointer
uint      video_fb_width(void);      // pixels (736 / 720 / 640)
uint      video_fb_height(void);     // ENCODED scanlines (480 / 576 / 240)
uint      video_fb_words(void);      // 32-bit words per scanline
uint      video_fb_bpp(void);        // bits per pixel: 2 (MDA gray) or 4 (CGA)

// NOTE video_fb_height() is the number of lines the view must WRITE, which is
// the raster's active lines divided by the mode's vertical repeat -- 240 for
// CGA640, whose 480-line raster is hardware line-doubled. Always drive loops
// from these accessors; nothing about the geometry is constant across sources.

// Output modes (all at sysclk 256 MHz; only the raster differs):
//   0  736x480@50
//   1  720x576@50
//   2  640x480@60
// video_set_mode() stores the choice and REBOOTS into it (it does not return);
// the choice then persists across later reboots.
uint        video_mode_count(void);
uint        video_mode_current(void);
const char *video_mode_name(uint i);
void        video_set_mode(uint i);

// Seed the mode video_init() will bring up - for restoring a saved profile at
// boot. MUST be called BEFORE video_init()
void        video_preselect_mode(uint i);

// Fill the framebuffer with 1px alternating black/white vertical stripes -- the
// highest-frequency pattern, the TMDS torture test. Proves the DVI path.
void video_test_pattern_stripes(void);

// Fill the framebuffer with 1px checkerboard
void video_test_pattern_checkerboard(void);

#endif
