#include "pico/stdlib.h"
#include "hardware/watchdog.h"
#include "source.h"

// Hercules clone with a 16.000 MHz crystal: 256 MHz = 16 sysclk per pixel.
static const source_mode_t src_mda16 = {
    .id         = SRC_ID_MDA16,
    .name       = "MDA16",
    .sysclk_khz = 256000,
    .active_w   = 720,
    .data_base  = 20,        // VIDEO, INTENSITY
    .data_bits  = 2,
    .sample_cyc = 16,
    .def_bp     = 16,
    .def_phase  = 0,
};

// CGA, 14.3333 MHz dot clock: 258 MHz = 18 sysclk per pixel.
// 320-wide modes arrive sampled twice per pixel, so every mode is 640 wide.
static const source_mode_t src_cga = {
    .id         = SRC_ID_CGA,
    .name       = "CGA",
    .sysclk_khz = 258000,
    .active_w   = 640,
    .data_base  = 21,        // I, R, G, B
    .data_bits  = 4,
    .sample_cyc = 18,
    .def_bp     = 120,
    .def_phase  = 4,
};

static const source_mode_t *const sources[] = {
    &src_mda16,
    &src_cga,
};
#define SOURCE_COUNT (sizeof sources / sizeof sources[0])

#define SRC_SCRATCH 5
#define SRC_MAGIC   0x5c7a01u

static uint g_active = 0;

void source_init(void)
{
    uint32_t sel = watchdog_hw->scratch[SRC_SCRATCH];
    if ((sel >> 8) != SRC_MAGIC) return;
    for (uint i = 0; i < SOURCE_COUNT; i++)
        if (sources[i]->id == (sel & 0xffu)) g_active = i;
}

uint source_count(void)                   { return SOURCE_COUNT; }
const source_mode_t *source_get(uint i)   { return i < SOURCE_COUNT ? sources[i] : NULL; }
const source_mode_t *source_active(void)  { return sources[g_active]; }
uint source_active_index(void)            { return g_active; }

void source_select(uint i)
{
    if (i >= SOURCE_COUNT) return;
    watchdog_hw->scratch[SRC_SCRATCH] = (SRC_MAGIC << 8) | sources[i]->id;
    watchdog_reboot(0, 0, 50);
    while (true) tight_loop_contents();
}
