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

// Same at 256 MHz: htotal 812 so 25.6 MHz lands on ~60 Hz.
static const struct dvi_timing dvi_timing_640x480p_60hz_256 = {
    .h_sync_polarity = false,
    .h_front_porch   = 16,
    .h_sync_width    = 96,
    .h_back_porch    = 60,
    .h_active_pixels = 640,

    .v_sync_polarity = false,
    .v_front_porch   = 10,
    .v_sync_width    = 2,
    .v_back_porch    = 33,
    .v_active_lines  = 480,

    .bit_clk_khz     = 256000
};

// Same at 266.4 MHz: htotal 846 so 26.64 MHz lands on 59.98 Hz.
static const struct dvi_timing dvi_timing_640x480p_60hz_266 = {
    .h_sync_polarity = false,
    .h_front_porch   = 16,
    .h_sync_width    = 96,
    .h_back_porch    = 94,
    .h_active_pixels = 640,

    .v_sync_polarity = false,
    .v_front_porch   = 10,
    .v_sync_width    = 2,
    .v_back_porch    = 33,
    .v_active_lines  = 480,

    .bit_clk_khz     = 266400
};

typedef struct {
    const char              *name;
    const struct dvi_timing *timing;
    uint                     bpp;    // 2 = mono tmds_encode_2bpp, 8 = RGB222
} video_mode_t;

// A mode's bit_clk_khz must equal its source's sysclk_khz.
static const video_mode_t mda16_modes[] = {
    { "640x480@60", &dvi_timing_640x480p_60hz,     2 },
    { "720x576@50", &dvi_timing_720x576p_50hz,     2 },
};

static const video_mode_t cga_modes[] = {
    { "640x480@60", &dvi_timing_640x480p_60hz_258, 8 },
};

static const video_mode_t c128_modes[] = {
    { "640x480@60", &dvi_timing_640x480p_60hz_256, 8 },
};

static const video_mode_t ega_modes[] = {
    { "640x480@60", &dvi_timing_640x480p_60hz_266, 8 },
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
    { SRC_ID_CGA,    MODES(cga_modes),    0 },
    { SRC_ID_C128,   MODES(c128_modes),   0 },
    { SRC_ID_EGA,    MODES(ega_modes),    0 },
};

// scratch[4] is off limits: watchdog_reboot() clears it.
// Packed as (MAGIC << 16) | (src_id << 8) | index, so an index saved under
// another source is ignored.
#define MODE_SCRATCH 3
#define MODE_MAGIC   0x77d0u

// One row per stored source line: 350 rows at 640 x 8bpp (EGA), all 576 at
// 720 x 2bpp (MDA).
#define FB_WORDS     56000u
#define FB_W_MAX     720u
static uint32_t framebuf[FB_WORDS];
static uint32_t zero_row[FB_W_MAX / 4];

// Display line y shows stored row (y - first) / rep, or black outside the
// n * rep lines from first. One word so core1 never sees half an update:
// first in bits 0-11, rep in 12-15, n in 16-27.
static volatile uint32_t g_vmap;

// RGB222 field positions in a framebuffer byte: R 5:4, G 3:2, B 1:0.
#define R_MSB 5
#define R_LSB 4
#define G_MSB 3
#define G_LSB 2
#define B_MSB 1
#define B_LSB 0

static uint8_t cga_rgb222[16];
static uint8_t ega_rgb222[64];

static struct dvi_inst dvi0;
static const mode_set_t *g_set = &mode_sets[0];
static uint g_mode;
static uint g_fb_w, g_fb_rows, g_fb_words, g_bpp, g_lines;

uint32_t *video_fb(void)    { return framebuf; }
uint video_fb_width(void)   { return g_fb_w; }
uint video_fb_rows(void)    { return g_fb_rows; }
uint video_fb_words(void)   { return g_fb_words; }
uint video_fb_bpp(void)     { return g_bpp; }
uint video_lines(void)      { return g_lines; }
const uint8_t *video_cga_rgb222(void) { return cga_rgb222; }
const uint8_t *video_ega_rgb222(void) { return ega_rgb222; }

void video_set_vmap(uint first, uint rep, uint n)
{
    if (n > g_fb_rows) n = g_fb_rows;
    g_vmap = (first & 0xfffu) | (rep & 0xfu) << 12 | (n & 0xfffu) << 16;
}

// The stored row for display line y, or the zero row.
static volatile bool g_scanlines;

void video_set_scanlines(bool on) { g_scanlines = on; }
bool video_get_scanlines(void)    { return g_scanlines; }

// With scanlines on, the last repeat of each row is black.
static inline const uint32_t *row_for(uint y)
{
    uint32_t m = g_vmap;
    uint first = m & 0xfffu, rep = (m >> 12) & 0xfu, n = (m >> 16) & 0xfffu;
    if (y < first || !rep) return zero_row;
    uint d = y - first, r = d / rep;
    if (g_scanlines && rep > 1 && d - r * rep == rep - 1) return zero_row;
    return r < n ? &framebuf[r * g_fb_words] : zero_row;
}

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

// Sample sB | sG<<1 | R<<2 | G<<3 | B<<4 | sR<<5 -> RGB222. Each channel's level
// is (primary << 1) | secondary: EGA's 4 levels, lossless.
static void build_ega_rgb222(void)
{
    for (uint i = 0; i < 64; i++) {
        uint r = ((i >> 2) & 1) << 1 | ((i >> 5) & 1);
        uint g = ((i >> 3) & 1) << 1 | ((i >> 1) & 1);
        uint b = ((i >> 4) & 1) << 1 | (i & 1);
        ega_rgb222[i] = (uint8_t)((r << R_LSB) | (g << G_LSB) | (b << B_LSB));
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
    const uint rep = (g_lines + g_fb_rows - 1) / g_fb_rows;
    const uint n   = (g_lines + rep - 1) / rep;
    for (uint y = 0; y < n; y++) {
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
    video_set_vmap(0, rep, n);
}

// Core1 keeps phase only by pushing exactly v_active_lines buffers per pass --
// nothing re-syncs it, so that count must never change.
// RAM-resident: an XIP stall costs the per-scanline budget.
static void __not_in_flash_func(core1_mono)(void)
{
    const uint wpc = dvi0.timing_derived.tmds_words_per_channel;
    while (true) {
        for (uint y = 0; y < g_lines; y++) {
            uint32_t *tb;
            queue_remove_blocking_u32(&dvi0.q_tmds_free, &tb);
            tmds_encode_2bpp(row_for(y), tb, g_fb_w);
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
    while (true) {
        for (uint y = 0; y < g_lines; y++) {
            const uint32_t *pix = row_for(y);
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
    g_lines    = t->v_active_lines;
    g_fb_w     = t->h_active_pixels;
    g_fb_words = g_fb_w / (32u / g_bpp);
    g_fb_rows  = FB_WORDS / g_fb_words;
    if (g_fb_rows > g_lines) g_fb_rows = g_lines;

    build_cga_rgb222();
    build_ega_rgb222();

#if PICO_PIO_USE_GPIO_BASE
    pio_set_gpio_base(DVI_SERIAL_CFG.pio, DVI_GPIO_BASE);   // must precede dvi_init
#endif

    dvi0.timing  = t;
    dvi0.ser_cfg = DVI_SERIAL_CFG;
    dvi_init(&dvi0, next_striped_spin_lock_num(), next_striped_spin_lock_num());

    video_clear();
    multicore_launch_core1(core1_main);
}
