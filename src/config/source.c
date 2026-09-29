#include "pico/stdlib.h"
#include "hardware/watchdog.h"
#include "hardware/clocks.h"
#include "source.h"

// Hercules clone with a 16.000 MHz crystal: 256 MHz = 16 sysclk per pixel.
static const source_mode_t src_mda16 = {
    .id         = SRC_ID_MDA16,
    .name       = "MDA16",
    .sysclk_khz = 256000,
    .active_w   = 720,
    .data_base  = 20,        // VIDEO, INTENSITY
    .data_bits  = 2,
    .dot_hz     = 16000000,
    .def_bp     = 16,
    .def_phase  = 2,
    .def_vscale = 1,
    .hsync_hz   = 18155,
};

// CGA, 14.3226 MHz dot clock (measured): 258 MHz = 18.013 sysclk per pixel,
// close enough to 18 for one sample per pixel.
// 320-wide modes arrive sampled twice per pixel, so every mode is 640 wide.
static const source_mode_t src_cga = {
    .id         = SRC_ID_CGA,
    .name       = "CGA",
    .sysclk_khz = 258000,
    .active_w   = 640,
    .data_base  = 21,        // I, R, G, B
    .data_bits  = 4,
    .dot_hz     = 14322600,
    .def_bp     = 120,
    .def_phase  = 2,
    .def_vscale = 2,
    .hsync_hz   = 15709,
};

// Commodore 128 80-column VDC: same RGBI lines and colours as CGA, own 16.000
// MHz crystal: 256 MHz = 16 sysclk per pixel.
static const source_mode_t src_c128 = {
    .id         = SRC_ID_C128,
    .name       = "C128",
    .sysclk_khz = 256000,
    .active_w   = 640,
    .data_base  = 21,        // I, R, G, B
    .data_bits  = 4,
    .dot_hz     = 16000000,
    .def_bp     = 180,
    .def_phase  = 10,
    .def_vscale = 2,
    .hsync_hz   = 15752,
};

// EGA, 350-line family (21.98 kHz): 640x350 graphics and 80x25 text. All six
// lines are driven. 17.750 MHz (35.500 crystal / 2): 266.4 MHz = 15.008 sysclk
// per pixel, so at one sample per pixel the first slip lands past pixel 640.
static const source_mode_t ega_v350 = {
    .id         = SRC_ID_EGA,
    .name       = "EGA",
    .sysclk_khz = 266400,
    .active_w   = 640,
    .data_base  = 20,        // sB, sG, R, G, B, sR
    .data_bits  = 6,
    .dot_hz     = 17750000,
    .def_bp     = 53,
    .def_phase  = 6,
    .def_vscale = 1,
    .hsync_hz   = 21976,
};

// EGA, 200-line family (15.81 kHz): 320x200, 640x200 and the CGA modes. Only
// RGBI is driven: pin 6 is INTENSITY, as on CGA. 14.161 MHz (28.322 / 2):
// 266.4 MHz = 18.82 sysclk per pixel, too far from an integer for one sample
// per pixel, so capture samples it at 2x. Same sysclk and raster as the 350
// family, which is what lets the two switch live.
static const source_mode_t ega_v200 = {
    .id         = SRC_ID_EGA,
    .name       = "EGA",
    .sysclk_khz = 266400,
    .active_w   = 640,
    .data_base  = 21,        // I, R, G, B
    .data_bits  = 4,
    .dot_hz     = 14160700,
    .def_bp     = 85,
    .def_phase  = 9,
    .def_vscale = 2,
    .hsync_hz   = 15809,
};

static const source_mode_t *const ega_variants[] = { &ega_v350, &ega_v200 };
#define EGA_VARIANTS (sizeof ega_variants / sizeof ega_variants[0])

// The live EGA entry: a copy of the running variant.
static source_mode_t s_ega;
static uint s_ega_var;

static const source_mode_t *const sources[] = {
    &src_mda16,
    &src_cga,
    &src_c128,
    &s_ega,
};
#define SOURCE_COUNT (sizeof sources / sizeof sources[0])

#define SRC_SCRATCH 5
#define SRC_MAGIC   0x5c7a01u

static uint g_active = 0;

void source_init(void)
{
    s_ega = *ega_variants[0];

    uint32_t sel = watchdog_hw->scratch[SRC_SCRATCH];
    if ((sel >> 8) != SRC_MAGIC) return;
    for (uint i = 0; i < SOURCE_COUNT; i++)
        if (sources[i]->id == (sel & 0xffu)) g_active = i;
}

uint source_count(void)                   { return SOURCE_COUNT; }
const source_mode_t *source_get(uint i)   { return i < SOURCE_COUNT ? sources[i] : NULL; }
const source_mode_t *source_active(void)  { return sources[g_active]; }
uint source_active_index(void)            { return g_active; }

// Auto detection on/off, in scratch[6]; survives the reboots it causes.
#define AUTO_SCRATCH 6
#define AUTO_MAGIC   0xa0707a01u

bool source_auto(void) { return watchdog_hw->scratch[AUTO_SCRATCH] == AUTO_MAGIC; }

void source_set_auto(bool on)
{
    watchdog_hw->scratch[AUTO_SCRATCH] = on ? AUTO_MAGIC : 0;
}

void source_select(uint i)
{
    if (i >= SOURCE_COUNT) return;
    watchdog_hw->scratch[SRC_SCRATCH] = (SRC_MAGIC << 8) | sources[i]->id;
    watchdog_reboot(0, 0, 50);
    while (true) tight_loop_contents();
}

// A mode change produces a few frames of nonsense, so a new rate must hold for
// EGA_DEBOUNCE grabs in a row. The families are 39% apart; +/-5% is plenty.
// A rate matching neither decides nothing.
#define EGA_DEBOUNCE 8

static bool rate_matches(uint32_t line_cycles, uint32_t want)
{
    if (!line_cycles) return false;
    const uint32_t hz = clock_get_hz(clk_sys) / line_cycles;
    return hz * 20 >= want * 19 && hz * 20 <= want * 21;
}

bool source_line_ok(uint32_t line_cycles)
{
    const uint32_t want = sources[g_active]->hsync_hz;
    return !want || rate_matches(line_cycles, want);
}

int source_ega_check(uint32_t line_cycles)
{
    static int  cand = -1;
    static uint run;

    if (sources[g_active] != &s_ega || !line_cycles) return -1;

    int match = -1;
    for (uint i = 0; i < EGA_VARIANTS; i++)
        if (rate_matches(line_cycles, ega_variants[i]->hsync_hz)) match = (int)i;
    if (match < 0 || match == (int)s_ega_var) { run = 0; return -1; }

    if (match != cand) { cand = match; run = 0; }
    if (++run < EGA_DEBOUNCE) return -1;
    run = 0;
    return match;
}

void source_ega_select(uint variant)
{
    if (variant >= EGA_VARIANTS) return;
    s_ega = *ega_variants[variant];
    s_ega_var = variant;
}

// Settings groups: one per source, EGA split by family (variant order: 350, 200).
uint source_group(void)
{
    return sources[g_active] == &s_ega ? g_active + s_ega_var : g_active;
}

const char *source_group_name(uint g)
{
    static const char *const names[SOURCE_GROUPS] = { "MDA16", "CGA", "C128", "EGA 350", "EGA 200" };
    return g < SOURCE_GROUPS ? names[g] : "?";
}

// Every source (each EGA variant separately) whose line rate is within 3%.
#define CAND_TOL_PCT 3u

uint source_candidates(uint32_t hsync_hz, source_cand_t *out, uint max)
{
    uint n = 0;
    for (uint i = 0; i < SOURCE_COUNT; i++) {
        const bool ega = sources[i] == &s_ega;
        const uint k = ega ? EGA_VARIANTS : 1u;
        for (uint v = 0; v < k && n < max; v++) {
            const source_mode_t *m = ega ? ega_variants[v] : sources[i];
            const uint32_t tol = m->hsync_hz * CAND_TOL_PCT / 100u;
            if (hsync_hz + tol >= m->hsync_hz && hsync_hz <= m->hsync_hz + tol)
                out[n++] = (source_cand_t){ i, m };
        }
    }
    return n;
}
