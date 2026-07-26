#include <string.h>
#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "hardware/vreg.h"
#include "hardware/clocks.h"
#include "hardware/pio.h"

#include "dvi.h"
#include "dvi_serialiser.h"
#include "tmds_encode.h"
#include "board.h"          // DVI_SERIAL_CFG, DVI_GPIO_BASE

// Custom 736x480 @ 50 Hz at 256 MHz. 736 = 23*32 is the smallest multiple of 32
// >= 720, so all 720 MDA/Hercules active pixels fit (8px black border each side)
// -- no crop, no scale, so the 1:1 checkerboard stays perfect. 256 MHz gives an
// integer 16 cyc/pixel dot clock (the proven-clean ratio); 50 Hz (via a wider
// raster, htotal 975 x vtotal 525) is frame-matched to the ~50 Hz MDA source.
// (640x480@60 and 736x480@60 exist too; a runtime mode-switch command is a
// later knob -- this single mode is the faithful default.)
static const struct dvi_timing dvi_timing_736x480p_50hz = {
    .h_sync_polarity = false, .h_front_porch = 24, .h_sync_width = 96,
    .h_back_porch = 119, .h_active_pixels = 736,             // htotal 975
    .v_sync_polarity = false, .v_front_porch = 10, .v_sync_width = 2,
    .v_back_porch = 33, .v_active_lines = 480,               // vtotal 525
    .bit_clk_khz = 256000,                                   // 25.6 MHz pixel -> 50.0 Hz
};
#define DVI_TIMING dvi_timing_736x480p_50hz
#define DVI_CLK_KHZ 256000

#define FB_W 736
#define FB_H 480
#define FB_WORDS (FB_W / 16)          // 2bpp: 16 px/word -> 46 words/scanline

static struct dvi_inst dvi0;
static uint32_t framebuf[FB_WORDS * FB_H];   // 2 bits/pixel, level 0..3

// --- framebuffer access (for the view/render module on core0) ---
uint32_t *video_framebuffer(void) { return framebuf; }
uint      video_fb_width(void)    { return FB_W; }
uint      video_fb_height(void)   { return FB_H; }
uint      video_fb_words(void)    { return FB_WORDS; }   // 32-bit words per scanline

// core1 owns DVI end to end: walk the framebuffer, TMDS-encode each line as
// 4-level grayscale, feed the serialiser. Runs forever off whatever core0 wrote.
static void core1_main(void) {
    dvi_register_irqs_this_core(&dvi0, DMA_IRQ_0);
    dvi_start(&dvi0);
    while (true) {
        for (uint y = 0; y < FB_H; y++) {
            const uint32_t *cbuf = &framebuf[y * FB_WORDS];
            uint32_t *tmdsbuf;
            queue_remove_blocking_u32(&dvi0.q_tmds_free, &tmdsbuf);
            tmds_encode_2bpp(cbuf, tmdsbuf, FB_W);
            queue_add_blocking_u32(&dvi0.q_tmds_valid, &tmdsbuf);
        }
    }
}

void video_init(void) {
    vreg_set_voltage(VREG_VOLTAGE_1_20);
    sleep_ms(10);
    set_sys_clock_khz(DVI_CLK_KHZ, true);

    memset(framebuf, 0, sizeof framebuf);      // black

    // DVI on pio1 (base 16 on the pizero for TMDS pins 32-39; base 0 on boards
    // with TMDS in 0-31). Capture will later own pio0 -- a PIO's GPIO base is
    // global, so the two can't share a PIO.
    pio_set_gpio_base(pio1, DVI_GPIO_BASE);
    dvi0.timing  = &DVI_TIMING;
    dvi0.ser_cfg = DVI_SERIAL_CFG;
    dvi0.ser_cfg.pio = pio1;
    dvi_init(&dvi0, next_striped_spin_lock_num(), next_striped_spin_lock_num());

    multicore_launch_core1(core1_main);
}

void video_test_pattern_stripes(void) {
    // 0xCC.. = 2-bit levels 0,3,0,3,... -> 1px black/white stripes.
    for (uint r = 0; r < FB_H; r++) {
        uint32_t *row = &framebuf[r * FB_WORDS];
        for (uint w = 0; w < FB_WORDS; w++) row[w] = 0xCCCCCCCCu;
    }
}

void video_test_pattern_checkerboard(void) {
    // 1px checkerboard: even rows 0xCC (B,W,B,W..), odd rows 0x33 (W,B,W,B..) --
    // opposite phase per row.
    for (uint r = 0; r < FB_H; r+=1) {
        uint32_t *row = &framebuf[r * FB_WORDS];
        uint32_t pat = (r & 1) ? 0x33333333u : 0xCCCCCCCCu;
        for (uint w = 0; w < FB_WORDS; w++)
            row[w] = pat;
    }
}