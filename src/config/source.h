#ifndef TTL2DVI_SOURCE_H
#define TTL2DVI_SOURCE_H

#include <stdbool.h>
#include "pico/types.h"   // uint

// Runtime descriptor for the connected source card. 
typedef struct {
    const char *name;        // "MDA"
    uint  data_bits;         // data lines: MDA 2 (VIDEO+INTENSITY), CGA 4, EGA 6
    uint  active_w;          // active pixels per line: MDA 720
    uint  dot_clock_hz;      // pixel clock: MDA ~16 MHz, CGA 14.318 MHz
    bool  color;             // false = monochrome (MDA); true = palette (CGA/EGA)
} source_mode_t;

extern const source_mode_t  src_mda;   // MDA / Hercules profile
extern const source_mode_t *g_src;     // active card (points at one of the above)

#endif
