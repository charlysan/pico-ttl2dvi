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

// 640x480@~60 at 256 MHz (the same proven timing dvi_test uses, just 2bpp
// monochrome instead of 16bpp colour). 256 MHz is the capture engine's clock,
// so once capture lands, output and capture share one sysclk. The custom 736
// modes + a mode table come later; this is enough for the test pattern.
#define DVI_TIMING dvi_timing_640x480p_60hz
#define DVI_CLK_KHZ 256000

#define FB_W 640
#define FB_H 480
#define FB_WORDS (FB_W / 16)          // 2bpp: 16 px/word -> 40 words/scanline

static struct dvi_inst dvi0;
static uint32_t framebuf[FB_WORDS * FB_H];   // 2 bits/pixel, level 0..3

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

void video_test_pattern(void) {
    // 0xCC.. = 2-bit levels 0,3,0,3,... -> 1px black/white stripes.
    for (uint r = 0; r < FB_H; r++) {
        uint32_t *row = &framebuf[r * FB_WORDS];
        for (uint w = 0; w < FB_WORDS; w++) row[w] = 0xCCCCCCCCu;
    }
}
