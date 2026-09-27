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

// VESA 640x480 vertical; htotal 820 instead of 800 so 25.8 MHz lands on ~60 Hz.
static const struct dvi_timing dvi_timing_640x480p_60hz_258 = {
    .h_sync_polarity = false,
    .h_front_porch   = 16,
    .h_sync_width    = 96,
    .h_back_porch    = 68,
    .h_active_pixels = 640,

    .v_sync_polarity = false,
    .v_front_porch   = 10,
    .v_sync_width    = 2,
    .v_back_porch    = 33,
    .v_active_lines  = 480,

    .bit_clk_khz     = 258000
};

typedef struct {
    const char              *name;
    const struct dvi_timing *timing;
    uint                     bpp;    // 2 = mono tmds_encode_2bpp, 8 = RGB222
    uint                     rep;    // display lines per framebuffer row
} video_mode_t;

// A mode's bit_clk_khz must equal its source's sysclk_khz.
static const video_mode_t mda16_modes[] = {
    { "640x480@60", &dvi_timing_640x480p_60hz,     2, 1 },
    { "720x576@50", &dvi_timing_720x576p_50hz,     2, 1 },
};

static const video_mode_t cga640_modes[] = {
    { "640x480@60", &dvi_timing_640x480p_60hz_258, 8, 2 },
};

typedef struct {
    uint8_t             src_id;
    const video_mode_t *modes;
    uint                count;
    uint                def;
} mode_set_t;

#define MODES(m) m, sizeof m / sizeof m[0]
static const mode_set_t mode_sets[] = {
    { SRC_ID_MDA16,  MODES(mda16_modes),  1 },
    { SRC_ID_CGA640, MODES(cga640_modes), 0 },
};

// scratch[4] is off limits: watchdog_reboot() clears it.
// Packed as (MAGIC << 16) | (src_id << 8) | index, so an index saved under
// another source is ignored.
#define MODE_SCRATCH 3
#define MODE_MAGIC   0x77d0u

// Largest mode: CGA 640x240 at 8bpp. MDA 720x576 at 2bpp is 25,920.
#define FB_WORDS     38400u
static uint32_t framebuf[FB_WORDS];

// RGB222 field positions in a framebuffer byte: R 5:4, G 3:2, B 1:0.
#define R_MSB 5
#define R_LSB 4
#define G_MSB 3
#define G_LSB 2
#define B_MSB 1
#define B_LSB 0

static uint8_t cga_rgb222[16];

static struct dvi_inst dvi0;
static const mode_set_t *g_set = &mode_sets[0];
static uint g_mode;
static uint g_fb_w, g_fb_h, g_fb_words, g_bpp, g_rep;

uint32_t *video_fb(void)    { return framebuf; }
uint video_fb_width(void)   { return g_fb_w; }
uint video_fb_height(void)  { return g_fb_h; }
uint video_fb_words(void)   { return g_fb_words; }
uint video_fb_bpp(void)     { return g_bpp; }
const uint8_t *video_cga_rgb222(void) { return cga_rgb222; }

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

// Sample I | R<<1 | G<<2 | B<<3 -> RGB222. Each channel's level is
// (colour << 1) | intensity; colour 6 is brown, not dark yellow.
static void build_cga_rgb222(void)
{
    for (uint i = 0; i < 16; i++) {
        uint I = i & 1, R = (i >> 1) & 1, G = (i >> 2) & 1, B = (i >> 3) & 1;
        uint r = (R << 1) | I, g = (G << 1) | I, b = (B << 1) | I;
        if (!I && R && G && !B) g = 1;
        cga_rgb222[i] = (uint8_t)((r << R_LSB) | (g << G_LSB) | (b << B_LSB));
    }
}

void video_clear(void)
{
    memset(framebuf, 0, sizeof framebuf);
}

// Mono: eight 80px bands cycling greys 0-3 twice.
// RGB222: the 16 CGA colours in 40px bars.
void video_test_pattern_stripes(void)
{
    const uint ppw = 32u / g_bpp;
    for (uint y = 0; y < g_fb_h; y++) {
        uint32_t *line = &framebuf[y * g_fb_words];
        for (uint w = 0; w < g_fb_words; w++) {
            uint32_t word = 0;
            for (uint i = 0; i < ppw; i++) {
                uint x = w * ppw + i;
                uint v = g_bpp == 8 ? cga_rgb222[x / 40u & 15u] : (x / 80u & 3u);
                word |= v << (g_bpp * i);
            }
            line[w] = word;
        }
    }
}

// Core1 keeps phase only by pushing exactly v_active_lines buffers per pass --
// nothing re-syncs it, so a new mode must keep that count right.
// RAM-resident: an XIP stall costs the per-scanline budget.
static void __not_in_flash_func(core1_mono)(void)
{
    const uint wpc = dvi0.timing_derived.tmds_words_per_channel;
    const uint lines = g_fb_h * g_rep;
    while (true) {
        for (uint y = 0; y < lines; y++) {
            uint32_t *tb;
            queue_remove_blocking_u32(&dvi0.q_tmds_free, &tb);
            tmds_encode_2bpp(&framebuf[y / g_rep * g_fb_words], tb, g_fb_w);
            memcpy(tb + wpc,     tb, wpc * sizeof(uint32_t));
            memcpy(tb + 2 * wpc, tb, wpc * sizeof(uint32_t));
            queue_add_blocking_u32(&dvi0.q_tmds_valid, &tb);
        }
    }
}

// Lanes 0/1/2 are blue/green/red.
static void __not_in_flash_func(core1_rgb222)(void)
{
    const uint wpc = dvi0.timing_derived.tmds_words_per_channel;
    const uint lines = g_fb_h * g_rep;
    while (true) {
        for (uint y = 0; y < lines; y++) {
            const uint32_t *pix = &framebuf[y / g_rep * g_fb_words];
            uint32_t *tb;
            queue_remove_blocking_u32(&dvi0.q_tmds_free, &tb);
            tmds_encode_data_channel_8bpp_fullres(pix, tb,           g_fb_w, B_MSB, B_LSB);
            tmds_encode_data_channel_8bpp_fullres(pix, tb + wpc,     g_fb_w, G_MSB, G_LSB);
            tmds_encode_data_channel_8bpp_fullres(pix, tb + 2 * wpc, g_fb_w, R_MSB, R_LSB);
            queue_add_blocking_u32(&dvi0.q_tmds_valid, &tb);
        }
    }
}

static void core1_main(void)
{
    dvi_register_irqs_this_core(&dvi0, DMA_IRQ_0);
    dvi_start(&dvi0);      // starts in vblank; no prebuffered line needed
    if (g_bpp == 8) core1_rgb222();
    else            core1_mono();
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

    const video_mode_t *m = &g_set->modes[g_mode];
    const struct dvi_timing *t = m->timing;
    g_bpp      = m->bpp;
    g_rep      = m->rep;
    g_fb_w     = t->h_active_pixels;
    g_fb_h     = t->v_active_lines / g_rep;
    g_fb_words = g_fb_w / (32u / g_bpp);

    build_cga_rgb222();

#if PICO_PIO_USE_GPIO_BASE
    pio_set_gpio_base(DVI_SERIAL_CFG.pio, DVI_GPIO_BASE);   // must precede dvi_init
#endif

    dvi0.timing  = t;
    dvi0.ser_cfg = DVI_SERIAL_CFG;
    dvi_init(&dvi0, next_striped_spin_lock_num(), next_striped_spin_lock_num());

    video_clear();
    multicore_launch_core1(core1_main);
}
