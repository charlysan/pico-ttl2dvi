#ifndef TTL2DVI_SOURCE_H
#define TTL2DVI_SOURCE_H

#include <stdbool.h>
#include <stdint.h>
#include "pico/types.h"   // uint

#define SRC_ID_MDA16   2     // stable: stored across reboots
#define SRC_ID_CGA     3
#define SRC_ID_C128    4
#define SRC_ID_EGA     5

typedef struct {
    uint8_t     id;
    const char *name;
    uint        sysclk_khz;
    uint        active_w;    // visible pixels per line
    uint        data_base;   // first data GPIO
    uint        data_bits;   // data lines sampled: 2, 4 or 6
    uint        dot_hz;      // pixel clock
    int         def_bp;
    int         def_phase;
    uint        def_vscale;  // display lines per source line
    uint        hsync_hz;    // EGA variants only: the line rate that selects it
} source_mode_t;

// Call first in main(): resolves the active source before the overclock.
void source_init(void);

uint source_count(void);
const source_mode_t *source_get(uint i);
const source_mode_t *source_active(void);
uint source_active_index(void);

// Switching reboots; the choice survives in watchdog scratch[5].
void source_select(uint i);

// EGA runs one of two variants (350-line / 200-line families) and follows the
// card live, without a reboot. Call source_ega_check() after every grab with
// the measured line period: it returns the variant to switch to, or -1.
// source_ega_select() then updates source_active() in place; the caller must
// reconfigure capture and view around it.
int  source_ega_check(uint32_t line_cycles);
void source_ega_select(uint variant);

// False when the measured line rate doesn't belong to the running source
// (only sources with an hsync_hz, i.e. EGA): the frame is the wrong family.
bool source_line_ok(uint32_t line_cycles);

#endif
