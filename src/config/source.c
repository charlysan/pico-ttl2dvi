#include "pico/stdlib.h"
#include "source.h"

// MDA / Hercules, STANDARD 16.257 MHz (882 x 18.432 kHz)
const source_mode_t src_mda = {
    .id           = SRC_ID_MDA,
    .name         = "MDA",
    .data_base    = 20,
    .data_bits    = 2,
    .fb_bpp       = 2,
    .oversample   = 2,
    .active_w     = 720,
    .dot_clock_hz = 16257000,
    .sysclk_khz   = 260000,
    .color        = false,
    .def_bp       = 17,
    .def_phase    = 4,
};

// Hercules clone with a 16.000 MHz crystal
const source_mode_t src_mda16 = {
    .id           = SRC_ID_MDA16,
    .name         = "MDA16",
    .data_base    = 20,
    .data_bits    = 2,
    .fb_bpp       = 2,
    .oversample   = 2,
    .active_w     = 720,
    .dot_clock_hz = 16000000,
    .sysclk_khz   = 256000,
    .color        = false,
    .def_bp       = 17,
    .def_phase    = 4,
};