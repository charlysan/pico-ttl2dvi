#include "pico/stdlib.h"
#include "hardware/watchdog.h"
#include "source.h"

// MDA / Hercules, STANDARD 16.257 MHz (882 x 18.432 kHz). The default: most
// real MDA and Hercules cards are this. sysclk 260 -> k=16.
const source_mode_t src_mda = {
    .id           = SRC_ID_MDA,
    .name         = "MDA",
    .data_base    = 20,
    .data_bits    = 2,
    .active_w     = 720,
    .dot_clock_hz = 16257000,
    .sysclk_khz   = 260000,
    .mode_set     = MODE_SET_MDA260,
    .color        = false,
    .def_bp       = 17,
    .def_phase    = 4,
};

// Hercules clone with a 16.000 MHz crystal [MEASURED: HSYNC 18155 Hz x h_total
// 881.3]. 1.6% slower than standard, which is ~11 px of width error over 720 if
// you run it as `mda`. sysclk / dot clock = 256 / 16.000 -> k=16.
const source_mode_t src_mda16 = {
    .id           = SRC_ID_MDA16,
    .name         = "MDA16",
    .data_base    = 20,
    .data_bits    = 2,
    .active_w     = 720,
    .dot_clock_hz = 16000000,
    .sysclk_khz   = 256000,
    .mode_set     = MODE_SET_MDA,
    .color        = false,
    .def_bp       = 17,
    .def_phase    = 4,
};

// CGA
// sysclk / dot clock =  256 / 16.000 -> k=18.0195 (0.11% off)
const source_mode_t src_cga640 = {
    .id           = SRC_ID_CGA640,
    .name         = "CGA640",
    .data_base    = 21,
    .data_bits    = 4,
    .active_w     = 640,
    .dot_clock_hz = 14318000,
    .sysclk_khz   = 258000,
    .mode_set     = MODE_SET_CGA,
    .color        = true,
    .def_bp       = 116,
    .def_phase    = 4,
};

// Commodore 128, VDC (8563/8568) 80-column RGBI: 80x25 chars of 8x8 = 640x200.
// Same 4-bit RGBI wiring and same 16-colour palette as CGA - only the CLOCK differs
// sysclk / dot clock =  256 / 16.000 -> k=16
const source_mode_t src_c128 = {
    .id           = SRC_ID_C128,
    .name         = "C128",
    .data_base    = 21,      // I,R,G,B on GP21-24, as CGA
    .data_bits    = 4,
    .active_w     = 640,
    .dot_clock_hz = 16000000,
    .sysclk_khz   = 256000,
    .mode_set     = MODE_SET_C128,
    .color        = true,
    .def_bp       = 130,
    .def_phase    = 4,
};

// Listing order for the `source` command. Safe to reorder: everything that
// persists stores the card's stable `id`, never its position here.
static const source_mode_t *const s_sources[] = {
    &src_mda, &src_mda16, &src_cga640, &src_c128
};
#define N_SOURCES (sizeof s_sources / sizeof s_sources[0])

// Active card. MDA16 by default
const source_mode_t *g_src = &src_mda16;

// The choice survives the switch reboot in watchdog scratch. [5]/[6] matches
// the cga branch so the two converge; see video.c for the full register map
// ([4] is unusable -- watchdog_reboot() clears it on its way out).
#define SRC_MAGIC         0x7712ca00u
#define SRC_SCRATCH_MAGIC 5
#define SRC_SCRATCH_IDX   6

uint source_count(void) { return N_SOURCES; }

const source_mode_t *source_at(uint i)
{
    return (i < N_SOURCES) ? s_sources[i] : NULL;
}

uint source_current_index(void)
{
    for (uint i = 0; i < N_SOURCES; i++)
        if (s_sources[i] == g_src) return i;
    return 0;
}

uint8_t source_current_id(void) { return g_src->id; }

const source_mode_t *source_by_id(uint8_t id)
{
    for (uint i = 0; i < N_SOURCES; i++)
        if (s_sources[i]->id == id) return s_sources[i];
    return NULL;
}

void source_set(uint i)
{
    if (i >= N_SOURCES) return;
    watchdog_hw->scratch[SRC_SCRATCH_MAGIC] = SRC_MAGIC;
    watchdog_hw->scratch[SRC_SCRATCH_IDX]   = s_sources[i]->id;   // ID, not index
    watchdog_reboot(0, 0, 50);
    while (1) tight_loop_contents();
}

void source_apply_boot_choice(void)
{
    if (watchdog_hw->scratch[SRC_SCRATCH_MAGIC] != SRC_MAGIC) return;
    const source_mode_t *s = source_by_id((uint8_t)watchdog_hw->scratch[SRC_SCRATCH_IDX]);
    if (s) g_src = s;
    // The magic is NOT consumed: so the choice
    // should survive every later reboot (save, mode switch, `reboot`) until you
    // change cables. Only a power cycle clears scratch and falls back to MDA.
}
