#ifndef TTL2DVI_SOURCE_H
#define TTL2DVI_SOURCE_H

#include <stdint.h>
#include "pico/types.h"   // uint

#define SRC_ID_MDA16   2     // stable: stored across reboots
#define SRC_ID_CGA640  3

typedef struct {
    uint8_t     id;
    const char *name;
    uint        sysclk_khz;
    uint        active_w;    // visible pixels per line
    uint        data_base;   // first data GPIO
    uint        data_bits;   // data lines sampled: 2 or 4
    uint        sample_cyc;  // sysclk per sample = sysclk / dot clock
    int         def_bp;
    int         def_phase;
} source_mode_t;

// Call first in main(): resolves the active source before the overclock.
void source_init(void);

uint source_count(void);
const source_mode_t *source_get(uint i);
const source_mode_t *source_active(void);
uint source_active_index(void);

// Switching reboots; the choice survives in watchdog scratch[5].
void source_select(uint i);

#endif
