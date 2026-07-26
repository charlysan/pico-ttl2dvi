#include "source.h"

// MDA / Hercules: 2 data bits (VIDEO+INTENSITY), 720 active px, ~16 MHz dot
// clock, monochrome.
const source_mode_t src_mda = {
    .name         = "MDA",
    .data_bits    = 2,
    .active_w     = 720,
    .dot_clock_hz = 16000000,
    .color        = false,
};

// Active card. When CGA/EGA arrive this is reassigned (console command, later
// auto-detected from the measured sync timing).
const source_mode_t *g_src = &src_mda;
