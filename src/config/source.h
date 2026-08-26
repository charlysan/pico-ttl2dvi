#ifndef TTL2DVI_SOURCE_H
#define TTL2DVI_SOURCE_H

#include <stdbool.h>
#include <stdint.h>
#include "pico/types.h"   // uint

#define SRC_ID_MDA     1     // standard 16.257 MHz
#define SRC_ID_MDA16   2     // 16.000 MHz clone crystal

typedef struct {
    uint8_t id;              // SRC_ID_*, stable across releases
    const char *name;        // "MDA" / "CGA640"
    uint  data_base;         // first data GPIO (PIO in_base): MDA 20, CGA 21
    uint  data_bits;         // data lines / `in pins, N`: MDA 2, CGA 4, EGA 6
    uint  fb_bpp;            // framebuffer depth
    uint  oversample;        // raw samples per source pixel: 2, or 1
    uint  active_w;          // active pixels per line: MDA 720, CGA640 640
    uint  dot_clock_hz;      // pixel clock: MDA ~16 MHz, CGA640 14.318 MHz
    uint  sysclk_khz;        // REQUIRED sysclk for this card
    bool  color;             // false = mono 2bpp path; true = palette path
    int   def_bp;            // default back-porch trim (source px)
    int   def_phase;         // default sampling phase (sysclk cycles)
} source_mode_t;