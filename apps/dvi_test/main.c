// ttl2dvi DVI standalone test app.
//
// Purpose: prove the DVI output path in isolation, before wiring the capture
// framebuffer into it -- the DVI pin config + PIO GPIO base (from board.h),
// the overclock, and the monitor. If a monitor shows 8 colour bars, the whole
// output chain works.
//
// Clocked at 256 MHz on purpose: that is the capture engine's clock, so once
// this works we can run capture + DVI on one sysclk. It drives the 640x480
// timing ~1.7% fast (pixel clock 25.6 vs 25.175 MHz -> ~61 Hz);

#include <stdio.h>
#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "hardware/clocks.h"
#include "hardware/vreg.h"
#include "hardware/sync.h"

#include "dvi.h"
#include "dvi_serialiser.h"
#include "board.h"          // DVI_SERIAL_CFG, DVI_GPIO_BASE (+ pin config)

#define FRAME_WIDTH  320
#define FRAME_HEIGHT 240
#define DVI_TIMING   dvi_timing_640x480p_60hz
#define DVI_CLK_KHZ  256000     // match capture; use 252000 if the monitor won't lock

static struct dvi_inst dvi0;
static uint16_t framebuf[FRAME_WIDTH * FRAME_HEIGHT];

// Core 1 runs the DVI signalling forever once the first scanline is queued.
static void core1_main(void) {
    dvi_register_irqs_this_core(&dvi0, DMA_IRQ_0);
    while (queue_is_empty(&dvi0.q_colour_valid))
        __wfe();
    dvi_start(&dvi0);
    dvi_scanbuf_main_16bpp(&dvi0);
}

static void fill_testcard(void) {
    // 8 vertical colour bars: white, yellow, cyan, green, magenta, red, blue, black.
    static const uint16_t bars[8] = {
        0xFFFF, 0xFFE0, 0x07FF, 0x07E0, 0xF81F, 0xF800, 0x001F, 0x0000
    };
    for (uint y = 0; y < FRAME_HEIGHT; y++)
        for (uint x = 0; x < FRAME_WIDTH; x++)
            framebuf[y * FRAME_WIDTH + x] = bars[(x * 8) / FRAME_WIDTH];
}

int main(void) {
    vreg_set_voltage(VREG_VOLTAGE_1_20);
    sleep_ms(10);
    set_sys_clock_khz(DVI_CLK_KHZ, true);

    stdio_init_all();
    fill_testcard();

#if PICO_PIO_USE_GPIO_BASE
    // DVI_GPIO_BASE is 16 on the pizero (TMDS pins 32-39, outside the default
    // 0-31 window) and 0 on boards with TMDS in 0-31. From board.h.
    pio_set_gpio_base(DVI_SERIAL_CFG.pio, DVI_GPIO_BASE);
#endif

    dvi0.timing  = &DVI_TIMING;
    dvi0.ser_cfg = DVI_SERIAL_CFG;
    dvi_init(&dvi0, next_striped_spin_lock_num(), next_striped_spin_lock_num());

    multicore_launch_core1(core1_main);

    // Feed scanline pointers to the DVI scanout, recycling the freed ones.
    while (true) {
        for (uint y = 0; y < FRAME_HEIGHT; y++) {
            const uint16_t *scanline = &framebuf[y * FRAME_WIDTH];
            queue_add_blocking_u32(&dvi0.q_colour_valid, &scanline);
            while (queue_try_remove_u32(&dvi0.q_colour_free, &scanline))
                ;
        }
    }
}
