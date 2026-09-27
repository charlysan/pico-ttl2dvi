#ifndef TTL2DVI_BOARD_H
#define TTL2DVI_BOARD_H

// Per-board hardware config. The build selects a profile with
//   cmake -DTTL_BOARD=<name> ..
// which defines TTL_BOARD_<name> (see cmake/boards.cmake); this header maps
// that to the code-time constants: the DVI TMDS pin config, the DVI PIO GPIO
// base, and the capture pins.
//
// The DVI serialiser configs (the {pins_tmds, pins_clk} structs) come from the
// vendored extern/dvi/common_dvi_pin_configs.h. To add a board whose wiring
// isn't in that file, define a `struct dvi_serialiser_cfg` HERE (project-owned)
// rather than editing the vendored header -- keeps extern/ a clean snapshot.

#include "common_dvi_pin_configs.h"

// ---- capture pins (shared across boards; both RP2350A/B expose 20-27) ----
// Data pins are per source (source.c), inside GP20-25.
#define PIN_VSYNC 26
#define PIN_HSYNC 27

// ---- per-board DVI output ----
#if defined(TTL_BOARD_pizero)
    // Waveshare RP2350-PiZero (RP2350B): TMDS {36,34,32} clk 38 -> base 16
    // (pins are outside PIO's default 0-31 window).
    #define BOARD_NAME       "waveshare_rp2350_pizero"
    #define DVI_SERIAL_CFG   waveshare_rp2350_zero
    #define DVI_GPIO_BASE    16

#elif defined(TTL_BOARD_pico2_dvi)
    // RP2350A pico2 + DVI board on GPIO 12-19 -> base 0 (pins within 0-31).
    // TODO: set the real serialiser config for your board's wiring. If none in
    // common_dvi_pin_configs.h matches, define one here, e.g.:
    //   static const struct dvi_serialiser_cfg ttl_pico2_dvi_cfg = {
    //       .pio = DVI_DEFAULT_PIO_INST,
    //       .sm_tmds = {0, 1, 2},
    //       .pins_tmds = {14, 16, 18},   // lane 0/1/2 (+); (-) is pin+1
    //       .pins_clk  = 12,
    //       .invert_diffpairs = false,
    //   };
    //   #define DVI_SERIAL_CFG ttl_pico2_dvi_cfg
    #define BOARD_NAME       "pico2_dvi"
    #define DVI_SERIAL_CFG   pico_sock_cfg   // PLACEHOLDER -- verify/replace
    #define DVI_GPIO_BASE    0

#else
    #error "No TTL board profile selected (define TTL_BOARD_<name> via cmake/boards.cmake)"
#endif

#endif // TTL2DVI_BOARD_H
