#include <string.h>
#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "pico/sync.h"
#include "hardware/pio.h"

#include "dvi.h"
#include "dvi_serialiser.h"
#include "tmds_encode.h"
#include "board.h"
#include "video.h"

#define DVI_TIMING  dvi_timing_640x480p_60hz

static struct dvi_inst dvi0;
static uint32_t framebuf[VIDEO_FB_WORDS * VIDEO_FB_H];

uint32_t *video_fb(void)    { return framebuf; }
uint video_fb_width(void)   { return VIDEO_FB_W; }
uint video_fb_height(void)  { return VIDEO_FB_H; }
uint video_fb_words(void)   { return VIDEO_FB_WORDS; }

void video_clear(void)
{
    memset(framebuf, 0, sizeof framebuf);
}

// Eight 80px bands cycling greys 0-3 twice.
void video_test_pattern_stripes(void)
{
    for (uint y = 0; y < VIDEO_FB_H; y++) {
        uint32_t *line = &framebuf[y * VIDEO_FB_WORDS];
        for (uint w = 0; w < VIDEO_FB_WORDS; w++) {
            uint32_t word = 0;
            for (uint i = 0; i < VIDEO_FB_PPW; i++)
                word |= ((w * VIDEO_FB_PPW + i) / 80u & 3u) << (VIDEO_FB_BPP * i);
            line[w] = word;
        }
    }
}

// Core1 keeps phase only by pushing exactly VIDEO_FB_H buffers per pass against
// libdvi's v_active_lines / v_repeat -- nothing re-syncs it, so a new mode must
// keep that count right.
// RAM-resident: an XIP stall costs the per-scanline budget.
static void __not_in_flash_func(core1_mono)(void)
{
    while (true) {
        for (uint y = 0; y < VIDEO_FB_H; y++) {
            uint32_t *tmdsbuf;
            queue_remove_blocking_u32(&dvi0.q_tmds_free, &tmdsbuf);
            tmds_encode_2bpp(&framebuf[y * VIDEO_FB_WORDS], tmdsbuf, VIDEO_FB_W);
            queue_add_blocking_u32(&dvi0.q_tmds_valid, &tmdsbuf);
        }
    }
}

static void core1_main(void)
{
    dvi_register_irqs_this_core(&dvi0, DMA_IRQ_0);
    dvi_start(&dvi0);      // starts in vblank; no prebuffered line needed
    core1_mono();
}

void video_init(void)
{
#if PICO_PIO_USE_GPIO_BASE
    pio_set_gpio_base(DVI_SERIAL_CFG.pio, DVI_GPIO_BASE);   // must precede dvi_init
#endif

    dvi0.timing  = &DVI_TIMING;
    dvi0.ser_cfg = DVI_SERIAL_CFG;
    dvi_init(&dvi0, next_striped_spin_lock_num(), next_striped_spin_lock_num());

    video_clear();
    multicore_launch_core1(core1_main);
}
