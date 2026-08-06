#include <string.h>
#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "hardware/vreg.h"
#include "hardware/clocks.h"
#include "hardware/pio.h"
#include "hardware/watchdog.h"

#include "dvi.h"
#include "dvi_serialiser.h"
#include "tmds_encode.h"
#include "board.h"          // DVI_SERIAL_CFG, DVI_GPIO_BASE
#include "source.h"         // g_src: sysclk, colour, active width
#include "video.h"

// ---------------------------------------------------------------------------
// Modes are PER SOURCE, because sysclk is per source (source.h explains why) and
// sysclk IS the DVI bit clock - so a mode's bit_clk_khz must equal its card's
// sysclk_khz. Changing one without the other silently changes the refresh rate.
//   MDA    : 256 MHz -> 25.6 MHz pixel
//   CGA640 : 258 MHz -> 25.8 MHz pixel
// ---------------------------------------------------------------------------

// ---- MDA rasters (256 MHz) ----

// 736x480 @ 50 Hz - NON-STANDARD raster (H 26.26 kHz), the fidelity default.
static const struct dvi_timing dvi_timing_736x480p_50hz = {
    .h_sync_polarity = false, .h_front_porch = 24, .h_sync_width = 96,
    .h_back_porch = 119, .h_active_pixels = 736,             // htotal 975
    .v_sync_polarity = false, .v_front_porch = 10, .v_sync_width = 2,
    .v_back_porch = 33, .v_active_lines = 480,               // vtotal 525
    .bit_clk_khz = 256000,                                   // 25.6 MHz -> 50.0 Hz
};

// 720x576 @ 50 Hz - real CEA-861 576p, the compatibility mode.
static const struct dvi_timing dvi_timing_720x576p_50hz = {
    .h_sync_polarity = false, .h_front_porch = 8, .h_sync_width = 44,
    .h_back_porch = 48, .h_active_pixels = 720,              // htotal 820
    .v_sync_polarity = false, .v_front_porch = 5, .v_sync_width = 5,
    .v_back_porch = 39, .v_active_lines = 576,               // vtotal 625
    .bit_clk_khz = 256000,                                   // 25.6 MHz -> 49.95 Hz
};

// 640x480 @ 60 Hz - standard VESA totals. CROPS 40 source px each side on MDA.
static const struct dvi_timing dvi_timing_640x480p_60hz_256 = {
    .h_sync_polarity = false, .h_front_porch = 16, .h_sync_width = 96,
    .h_back_porch = 48, .h_active_pixels = 640,              // htotal 800
    .v_sync_polarity = false, .v_front_porch = 10, .v_sync_width = 2,
    .v_back_porch = 33, .v_active_lines = 480,               // vtotal 525
    .bit_clk_khz = 256000,                                   // 25.6 MHz -> 60.95 Hz
};

// ---- CGA640 raster (258 MHz) ----

// 640x480 @ 60 Hz
static const struct dvi_timing dvi_timing_640x480p_60hz_258 = {
    .h_sync_polarity = false, .h_front_porch = 16, .h_sync_width = 96,
    .h_back_porch = 68, .h_active_pixels = 640,              // htotal 820
    .v_sync_polarity = false, .v_front_porch = 10, .v_sync_width = 2,
    .v_back_porch = 33, .v_active_lines = 480,               // vtotal 525
    .bit_clk_khz = 258000,                                   // 25.8 MHz -> 59.93 Hz
};

// ---- standard-MDA rasters (260 MHz -> 26.0 MHz pixel) ----

static const struct dvi_timing dvi_timing_736x480p_50hz_260 = {
    .h_sync_polarity = false, .h_front_porch = 24, .h_sync_width = 96,
    .h_back_porch = 134, .h_active_pixels = 736,             // htotal 990
    .v_sync_polarity = false, .v_front_porch = 10, .v_sync_width = 2,
    .v_back_porch = 33, .v_active_lines = 480,               // vtotal 525
    .bit_clk_khz = 260000,                                   // -> 50.02 Hz
};

static const struct dvi_timing dvi_timing_720x576p_50hz_260 = {
    .h_sync_polarity = false, .h_front_porch = 12, .h_sync_width = 64,
    .h_back_porch = 36, .h_active_pixels = 720,              // htotal 832
    .v_sync_polarity = false, .v_front_porch = 5, .v_sync_width = 5,
    .v_back_porch = 39, .v_active_lines = 576,               // vtotal 625
    .bit_clk_khz = 260000,                                   // -> 50.00 Hz, H 31.25 kHz
};

static const struct dvi_timing dvi_timing_640x480p_60hz_260 = {
    .h_sync_polarity = false, .h_front_porch = 16, .h_sync_width = 96,
    .h_back_porch = 73, .h_active_pixels = 640,              // htotal 825
    .v_sync_polarity = false, .v_front_porch = 10, .v_sync_width = 2,
    .v_back_porch = 33, .v_active_lines = 480,               // vtotal 525
    .bit_clk_khz = 260000,                                   // -> 60.03 Hz
};

// ---- C128 VDC raster (256 MHz) ----

// 640x480 @ 60 Hz at 25.6 MHz. Same 640 active as CGA640 -- the VDC's 80x25 of
// 8x8 chars is exactly 640x200 
// 16.000 MHz dot clock
static const struct dvi_timing dvi_timing_640x480p_60hz_c128 = {
    .h_sync_polarity = false, .h_front_porch = 16, .h_sync_width = 96,
    .h_back_porch = 61, .h_active_pixels = 640,              // htotal 813
    .v_sync_polarity = false, .v_front_porch = 10, .v_sync_width = 2,
    .v_back_porch = 33, .v_active_lines = 480,               // vtotal 525
    .bit_clk_khz = 256000,                                   // 25.6 MHz -> 59.98 Hz
};

typedef struct {
    const char *name;
    const struct dvi_timing *timing;
    uint16_t fb_w;        // = timing->h_active_pixels
    uint16_t fb_h;        // = v_active_lines / v_repeat  (ENCODED lines)
    uint8_t  v_repeat;    // 1 = full res, 2 = hardware line doubling
} video_mode_t;

static const video_mode_t g_modes_mda[] = {
    { "736x480@50", &dvi_timing_736x480p_50hz,     736, 480, 1 },
    { "720x576@50", &dvi_timing_720x576p_50hz,     720, 576, 1 },
    { "640x480@60", &dvi_timing_640x480p_60hz_256, 640, 480, 1 },
};

// CGA640 runs v_repeat 2 -- 240 encoded lines doubled to the 480-line raster.
static const video_mode_t g_modes_cga640[] = {
    { "640x480@60", &dvi_timing_640x480p_60hz_258, 640, 240, 2 },
};

// Same shape as CGA640 (colour, v_repeat 2), different pixel clock.
static const video_mode_t g_modes_c128[] = {
    { "640x480@60", &dvi_timing_640x480p_60hz_c128, 640, 240, 2 },
};

static const video_mode_t g_modes_mda260[] = {
    { "736x480@50", &dvi_timing_736x480p_50hz_260, 736, 480, 1 },
    { "720x576@50", &dvi_timing_720x576p_50hz_260, 720, 576, 1 },
    { "640x480@60", &dvi_timing_640x480p_60hz_260, 640, 480, 1 },
};

// Raster tables indexed by g_src->mode_set. A card's sysclk IS its DVI bit
// clock, so cards sharing a sysclk share a table -- MDA and the C128 both run
// 256 MHz, CGA640 needs its own at 258.
static const struct { const video_mode_t *tbl; uint count; } g_mode_sets[] = {
    [MODE_SET_MDA]  = { g_modes_mda,    sizeof g_modes_mda    / sizeof g_modes_mda[0]    },
    [MODE_SET_CGA]  = { g_modes_cga640, sizeof g_modes_cga640 / sizeof g_modes_cga640[0] },
    [MODE_SET_C128] = { g_modes_c128,   sizeof g_modes_c128   / sizeof g_modes_c128[0]   },
    [MODE_SET_MDA260] = { g_modes_mda260, sizeof g_modes_mda260 / sizeof g_modes_mda260[0] },
};

static const video_mode_t *g_mode_tbl   = g_modes_mda;
static uint                g_mode_count = sizeof g_modes_mda / sizeof g_modes_mda[0];

// The mode choice survives the reboot in ONE watchdog scratch register, packed
// as (MAGIC << 8) | index.
//
// scratch[3], and the register choice is load-bearing `[MEASURED, SDK 2.3]`:
//   [4]      watchdog_reboot() owns it -- on the pc == 0 path (a plain reboot)
//            it does `scratch[4] = 0`, wiping anything parked there
//   [5][6]   source.c: the selected card
//   [2]      settings.c: pending profile across a load-with-mode-change
//   [0][1]   the bootrom takes USB-boot parameters there (reset_usb_boot)
// The magic is NOT consumed on read, so the mode sticks across later reboots.
// The index is per-SOURCE, so it is validated against the active table below.
#define MODE_MAGIC   0x7712d0u
#define MODE_SCRATCH 3

static struct dvi_inst dvi0;

// One framebuffer for every source/mode, sized by TOTAL WORDS (same reasoning as
// capture.c's rawbuf): the shapes differ, so the product of per-axis maxima
// over-allocates. Worst case is MDA 720x576 @2bpp = 45 words x 576 = 25,920.
//   MDA    736x480 2bpp : 46 x 480 = 22,080
//   MDA    720x576 2bpp : 45 x 576 = 25,920   <- largest
//   MDA    640x480 2bpp : 40 x 480 = 19,200
//   CGA640 640x240 4bpp : 80 x 240 = 19,200
#define FB_WORDS_TOTAL 26112                  // 102 KB
static uint32_t framebuf[FB_WORDS_TOTAL];

#define FB_W_MAX 736                          // widest active width, for line8

static uint g_mode = 0;                       // index into g_mode_tbl
static uint g_fb_w = 736, g_fb_h = 480, g_fb_words = 46, g_fb_bpp = 2;

// --- palette (colour sources only) ---
#define PALETTE_BITS 4                        // 16 entries; CGA RGBI
#define N_PALETTE    (1u << PALETTE_BITS)
static uint32_t tmds_palette[6 * N_PALETTE];  // 6 words/entry: 2 symbols x 3 lanes

static inline uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b)
{
    return (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}

// Standard CGA RGBI -> RGB. Sample value is I | R<<1 | G<<2 | B<<3, which IS the
// palette index. On = I?255:170, off = I?85:0, plus the classic BROWN fix:
// colour 6 (R+G, no I) would be dark yellow, but real CGA halves the green.
static void build_cga_palette(uint16_t *pal)
{
    for (uint i = 0; i < N_PALETTE; i++) {
        int I = i & 1, R = (i >> 1) & 1, G = (i >> 2) & 1, B = (i >> 3) & 1;
        int hi = I ? 255 : 170, lo = I ? 85 : 0;
        int r = R ? hi : lo, g = G ? hi : lo, b = B ? hi : lo;
        if (!I && R && G && !B) g = 85;                 // brown, not dark yellow
        pal[i] = rgb565((uint8_t)r, (uint8_t)g, (uint8_t)b);
    }
}

// CGA RGBI -> grayscale. Colour derivation identical to build_cga_palette;
// Rec.601 luma, weights x1000 to stay in integer maths.
static void build_cga_mono_palette(uint16_t *pal)
{
    for (uint i = 0; i < N_PALETTE; i++) {
        int I = i & 1, R = (i >> 1) & 1, G = (i >> 2) & 1, B = (i >> 3) & 1;
        int hi = I ? 255 : 170, lo = I ? 85 : 0;
        int r = R ? hi : lo, g = G ? hi : lo, b = B ? hi : lo;
        if (!I && R && G && !B) g = 85;                  // brown, not dark yellow
        int y = (299 * r + 587 * g + 114 * b) / 1000;    // luma
        pal[i] = rgb565((uint8_t)y, (uint8_t)y, (uint8_t)y);
    }
}

// --- framebuffer access (for the view/render module on core0) ---
uint32_t *video_framebuffer(void) { return framebuf; }
uint      video_fb_width(void)    { return g_fb_w; }
uint      video_fb_height(void)   { return g_fb_h; }
uint      video_fb_words(void)    { return g_fb_words; }
uint      video_fb_bpp(void)      { return g_fb_bpp; }

// --- mode query / switch ---
uint        video_mode_count(void)   { return g_mode_count; }
uint        video_mode_current(void) { return g_mode; }
const char *video_mode_name(uint i)  { return i < g_mode_count ? g_mode_tbl[i].name : "?"; }

// Switch output mode by rebooting into it.
void video_set_mode(uint i)
{
    if (i >= g_mode_count) return;
    watchdog_hw->scratch[MODE_SCRATCH] = (MODE_MAGIC << 8) | (i & 0xffu);
    watchdog_reboot(0, 0, 50);
    while (1) tight_loop_contents();
}

// ---------------------------------------------------------------------------
// core1: owns DVI end to end.
//
// The loop period MUST stay equal to g_fb_h: there is no vblank re-sync, so
// core1 and the display keep in step only because this pushes exactly g_fb_h
// buffers per pass while libdvi pops exactly v_active_lines / v_repeat per
// frame. Both come from the mode table
// ---------------------------------------------------------------------------

// Mono: encode ONE lane and copy it to the other two. In a colour build the
// three lanes are independent, and mono simply means identical symbols on all
// three -- exactly what DVI_MONOCHROME_TMDS did in hardware. Costs two memcpys
// but keeps MDA on the cheap 2bpp encoder, which is what makes v_repeat 1
// (full 480-line resolution) affordable for it.
static void core1_mono(void)
{
    const uint wpc = dvi0.timing_derived.tmds_words_per_channel;
    while (true) {
        for (uint y = 0; y < g_fb_h; y++) {
            const uint32_t *cbuf = &framebuf[y * g_fb_words];
            uint32_t *tmdsbuf;
            queue_remove_blocking_u32(&dvi0.q_tmds_free, &tmdsbuf);
            tmds_encode_2bpp(cbuf, tmdsbuf, g_fb_w);
            memcpy(tmdsbuf + wpc,     tmdsbuf, wpc * sizeof(uint32_t));
            memcpy(tmdsbuf + 2 * wpc, tmdsbuf, wpc * sizeof(uint32_t));
            queue_add_blocking_u32(&dvi0.q_tmds_valid, &tmdsbuf);
        }
    }
}

// Colour: expand the 4bpp framebuffer line to the 8bpp indices the palette
// encoder wants, then encode all three lanes. tmds_encode_palette_data does
// 80 px per pass, so the active width must be a multiple of 80 -- 640 is 8x80.
static void core1_palette(void)
{
    static uint32_t line8[FB_W_MAX / 4];     // 8bpp indices, 4 px/word
    while (true) {
        for (uint y = 0; y < g_fb_h; y++) {
            const uint32_t *fbline = &framebuf[y * g_fb_words];
            // 4bpp -> 8bpp: each fb word (8 nibbles) becomes two line8 words.
            for (uint w = 0; w < g_fb_words; w++) {
                uint32_t v = fbline[w];
                line8[2 * w] =
                    ( v         & 0x0fu)        | (((v >> 4)  & 0x0fu) << 8)  |
                    (((v >> 8)  & 0x0fu) << 16) | (((v >> 12) & 0x0fu) << 24);
                line8[2 * w + 1] =
                    (((v >> 16) & 0x0fu))       | (((v >> 20) & 0x0fu) << 8)  |
                    (((v >> 24) & 0x0fu) << 16) | (((v >> 28) & 0x0fu) << 24);
            }
            uint32_t *tmdsbuf;
            queue_remove_blocking_u32(&dvi0.q_tmds_free, &tmdsbuf);
            tmds_encode_palette_data(line8, tmds_palette, tmdsbuf, g_fb_w, PALETTE_BITS);
            queue_add_blocking_u32(&dvi0.q_tmds_valid, &tmdsbuf);
        }
    }
}

static void core1_main(void)
{
    dvi_register_irqs_this_core(&dvi0, DMA_IRQ_0);
    dvi_start(&dvi0);
    if (g_src->color) core1_palette();
    else              core1_mono();
}

// Seed the mode used by video_init(), for the settings boot slot.
// MUST be called before video_init().
void video_preselect_mode(uint i)
{
    if (i < g_mode_count) g_mode = i;
}

void video_init(void) {
    // The card decides the raster table (and the clock), so resolve it first.
    // main() must have run source_apply_boot_choice() before this.
    uint set = g_src->mode_set;
    if (set >= sizeof g_mode_sets / sizeof g_mode_sets[0]) set = MODE_SET_MDA;
    g_mode_tbl   = g_mode_sets[set].tbl;
    g_mode_count = g_mode_sets[set].count;
    if (g_mode >= g_mode_count) g_mode = 0;    // a preselect from another card

    // Mode selected by a previous `mode` command (survives the reboot), else
    // whatever video_preselect_mode() seeded, else the compiled default.
    uint32_t sel = watchdog_hw->scratch[MODE_SCRATCH];
    if ((sel >> 8) == MODE_MAGIC && (sel & 0xffu) < g_mode_count)
        g_mode = sel & 0xffu;

    const video_mode_t *m = &g_mode_tbl[g_mode];
    g_fb_w     = m->fb_w;
    g_fb_h     = m->fb_h;
    g_fb_bpp   = g_src->color ? 4 : 2;
    g_fb_words = g_fb_w / (32 / g_fb_bpp);     // 2bpp: 16 px/word, 4bpp: 8

    // A mode table entry that does not fit would silently scribble past the
    // buffer; fall back rather than corrupt memory.
    if (g_fb_words * g_fb_h > FB_WORDS_TOTAL) {
        g_mode = 0;
        m = &g_mode_tbl[0];
        g_fb_w = m->fb_w; g_fb_h = m->fb_h;
        g_fb_words = g_fb_w / (32 / g_fb_bpp);
    }

    // sysclk comes from the CARD (see source.h); the mode's bit_clk_khz mirrors
    // it. 1.20 V is fine to 256; 258 is a touch over, so lift it there.
    uint sysclk_khz = g_src->sysclk_khz;
    vreg_set_voltage(sysclk_khz > 256000 ? VREG_VOLTAGE_1_25 : VREG_VOLTAGE_1_20);
    sleep_ms(10);
    set_sys_clock_khz(sysclk_khz, true);

    memset(framebuf, 0, sizeof framebuf);      // black

    if (g_src->color) {
        uint16_t pal[N_PALETTE];
        build_cga_palette(pal);
        tmds_setup_palette_symbols(pal, tmds_palette, N_PALETTE);
    }

    // DVI on pio1 (base 16 on the pizero for TMDS pins 32-39; base 0 on boards
    // with TMDS in 0-31). Capture owns pio0 -- a PIO's GPIO base is global, so
    // the two can't share a PIO.
    pio_set_gpio_base(pio1, DVI_GPIO_BASE);
    dvi0.timing   = m->timing;
    dvi0.v_repeat = m->v_repeat;               // [ttl2dvi libdvi patch]
    dvi0.ser_cfg  = DVI_SERIAL_CFG;
    dvi0.ser_cfg.pio = pio1;
    dvi_init(&dvi0, next_striped_spin_lock_num(), next_striped_spin_lock_num());

    multicore_launch_core1(core1_main);
}

// Test patterns write raw framebuffer words, so their bit patterns depend on the
// depth: at 2bpp a word is 16 px of level 0..3, at 4bpp it is 8 px of palette
// index 0..15. 0xCC/0x33 give black/white 1px stripes at 2bpp; at 4bpp use
// 0x0F/0xF0 for black/white against the CGA palette (0 = black, 15 = bright).
void video_test_pattern_stripes(void) {
    uint32_t pat = (g_fb_bpp == 2) ? 0xCCCCCCCCu : 0xF0F0F0F0u;
    for (uint r = 0; r < g_fb_h; r++) {
        uint32_t *row = &framebuf[r * g_fb_words];
        for (uint w = 0; w < g_fb_words; w++) row[w] = pat;
    }
}

void video_test_pattern_checkerboard(void) {
    uint32_t a = (g_fb_bpp == 2) ? 0xCCCCCCCCu : 0xF0F0F0F0u;
    uint32_t b = (g_fb_bpp == 2) ? 0x33333333u : 0x0F0F0F0Fu;
    for (uint r = 0; r < g_fb_h; r += 1) {
        uint32_t *row = &framebuf[r * g_fb_words];
        uint32_t pat = (r & 1) ? b : a;
        for (uint w = 0; w < g_fb_words; w++) row[w] = pat;
    }
}
