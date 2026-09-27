#include <string.h>
#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "pico/sync.h"
#include "hardware/pio.h"
#include "hardware/watchdog.h"

#include "dvi.h"
#include "dvi_serialiser.h"
#include "tmds_encode.h"
#include "board.h"
#include "source.h"
#include "video.h"

// CEA-861 576p vertical timing exactly; horizontal blanking squeezed 144 -> 100
// so the 25.6 MHz pixel clock still lands on 625 lines at ~50 Hz.
static const struct dvi_timing dvi_timing_720x576p_50hz = {
    .h_sync_polarity = false,
    .h_front_porch   = 12,
    .h_sync_width    = 64,
    .h_back_porch    = 24,
    .h_active_pixels = 720,

    .v_sync_polarity = false,
    .v_front_porch   = 5,
    .v_sync_width    = 5,
    .v_back_porch    = 39,
    .v_active_lines  = 576,

    .bit_clk_khz     = 256000
};

typedef struct {
    const char              *name;
    const struct dvi_timing *timing;
} video_mode_t;

// A mode's bit_clk_khz must equal its source's sysclk_khz.
static const video_mode_t mda16_modes[] = {
    { "640x480@60", &dvi_timing_640x480p_60hz },
    { "720x576@50", &dvi_timing_720x576p_50hz },
};

typedef struct {
    uint8_t             src_id;
    const video_mode_t *modes;
    uint                count;
    uint                def;
} mode_set_t;

static const mode_set_t mode_sets[] = {
    { SRC_ID_MDA16, mda16_modes, sizeof mda16_modes / sizeof mda16_modes[0], 1 },
};

// scratch[4] is off limits: watchdog_reboot() clears it.
// Packed as (MAGIC << 16) | (src_id << 8) | index, so an index saved under
// another source is ignored.
#define MODE_SCRATCH 3
#define MODE_MAGIC   0x77d0u

#define FB_W_MAX     720u
#define FB_H_MAX     576u
static uint32_t framebuf[FB_W_MAX / VIDEO_FB_PPW * FB_H_MAX];

static struct dvi_inst dvi0;
static const mode_set_t *g_set = &mode_sets[0];
static uint g_mode;
static uint g_fb_w, g_fb_h, g_fb_words;

uint32_t *video_fb(void)    { return framebuf; }
uint video_fb_width(void)   { return g_fb_w; }
uint video_fb_height(void)  { return g_fb_h; }
uint video_fb_words(void)   { return g_fb_words; }

uint video_mode_count(void)   { return g_set->count; }
uint video_mode_current(void) { return g_mode; }
const char *video_mode_name(uint i) { return i < g_set->count ? g_set->modes[i].name : ""; }

void video_set_mode(uint i)
{
    if (i >= g_set->count) return;
    watchdog_hw->scratch[MODE_SCRATCH] =
        (MODE_MAGIC << 16) | ((uint32_t)g_set->src_id << 8) | i;
    watchdog_reboot(0, 0, 50);
    while (true) tight_loop_contents();
}

void video_clear(void)
{
    memset(framebuf, 0, sizeof framebuf);
}

// Eight 80px bands cycling greys 0-3 twice.
void video_test_pattern_stripes(void)
{
    for (uint y = 0; y < g_fb_h; y++) {
        uint32_t *line = &framebuf[y * g_fb_words];
        for (uint w = 0; w < g_fb_words; w++) {
            uint32_t word = 0;
            for (uint i = 0; i < VIDEO_FB_PPW; i++)
                word |= ((w * VIDEO_FB_PPW + i) / 80u & 3u) << (VIDEO_FB_BPP * i);
            line[w] = word;
        }
    }
}

// Core1 keeps phase only by pushing exactly g_fb_h buffers per pass against
// libdvi's v_active_lines / v_repeat -- nothing re-syncs it, so a new mode must
// keep that count right.
// RAM-resident: an XIP stall costs the per-scanline budget.
static void __not_in_flash_func(core1_mono)(void)
{
    while (true) {
        for (uint y = 0; y < g_fb_h; y++) {
            uint32_t *tmdsbuf;
            queue_remove_blocking_u32(&dvi0.q_tmds_free, &tmdsbuf);
            tmds_encode_2bpp(&framebuf[y * g_fb_words], tmdsbuf, g_fb_w);
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
    const uint8_t src_id = source_active()->id;
    for (uint i = 0; i < sizeof mode_sets / sizeof mode_sets[0]; i++)
        if (mode_sets[i].src_id == src_id) g_set = &mode_sets[i];

    g_mode = g_set->def;
    uint32_t sel = watchdog_hw->scratch[MODE_SCRATCH];
    if ((sel >> 16) == MODE_MAGIC && ((sel >> 8) & 0xffu) == g_set->src_id
        && (sel & 0xffu) < g_set->count)
        g_mode = sel & 0xffu;

    const struct dvi_timing *t = g_set->modes[g_mode].timing;
    g_fb_w     = t->h_active_pixels;
    g_fb_h     = t->v_active_lines;
    g_fb_words = g_fb_w / VIDEO_FB_PPW;

#if PICO_PIO_USE_GPIO_BASE
    pio_set_gpio_base(DVI_SERIAL_CFG.pio, DVI_GPIO_BASE);   // must precede dvi_init
#endif

    dvi0.timing  = t;
    dvi0.ser_cfg = DVI_SERIAL_CFG;
    dvi_init(&dvi0, next_striped_spin_lock_num(), next_striped_spin_lock_num());

    video_clear();
    multicore_launch_core1(core1_main);
}
