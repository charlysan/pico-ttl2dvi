#ifndef TTL2DVI_SOURCE_H
#define TTL2DVI_SOURCE_H

#include <stdbool.h>
#include <stdint.h>
#include "pico/types.h"   // uint

// Runtime descriptor for the connected source card. One card is active per
// boot: switching reboots (the sampler's pin count, the sysclk and the DVI
// encoder all differ), so everything downstream can read g_src as a constant.
// Stable per-card IDs. These are what get STORED (profile slots, watchdog
// scratch) -- never the array index, which shifts whenever a card is inserted.
// Assign a new number for a new card; never reuse or renumber.
#define SRC_ID_MDA     1     // standard 16.257 MHz
#define SRC_ID_MDA16   2     // 16.000 MHz clone crystal
#define SRC_ID_CGA640  3
#define SRC_ID_C128    4

typedef struct {
    uint8_t id;              // SRC_ID_*, stable across releases
    const char *name;        // "MDA" / "CGA640"
    uint  data_base;         // first data GPIO (PIO in_base): MDA 20, CGA 21
    uint  data_bits;         // data lines / `in pins, N`: MDA 2, CGA 4, EGA 6
    uint  active_w;          // active pixels per line: MDA 720, CGA640 640
    uint  dot_clock_hz;      // pixel clock: MDA ~16 MHz, CGA640 14.318 MHz
    uint  sysclk_khz;        // REQUIRED sysclk for this card -- see below
    uint  mode_set;          // which DVI raster table video.c should use
    bool  color;             // false = mono 2bpp path; true = palette path
    int   def_bp;            // default back-porch trim (source px)
    int   def_phase;         // default sampling phase (sysclk cycles)
} source_mode_t;

// mode_set indexes video.c's raster tables. It is a plain tag rather than a
// pointer so config/ stays the bottom layer and video keeps its own tables.
//
// A set is NOT just a sysclk. It also implies the framebuffer depth and the
// vertical repeat, so a mono and a colour card CANNOT share one even at the
// same clock: MDA's 736x480 is 22k words at 2bpp but 44k at 4bpp (past the
// framebuffer), and a colour card needs v_repeat 2 for the encode budget.
// Hence three sets for three cards.
#define MODE_SET_MDA    0    // 25.6 MHz pixel, 2bpp, v_repeat 1 (full res)
#define MODE_SET_CGA    1    // 25.8 MHz pixel, 4bpp, v_repeat 2
#define MODE_SET_C128   2    // 25.6 MHz pixel, 4bpp, v_repeat 2
#define MODE_SET_MDA260 3    // 26.0 MHz pixel, 2bpp, v_repeat 1

// WHY sysclk is per-source: the reconstruction beat
// is nulled when sysclk / dot_clock is an INTEGER. MDA's ~16 MHz wants 256 MHz
// (K=16, exact); CGA640's 14.318 MHz wants ~258 MHz (K=18, 18.02). One clock
// cannot serve both, and sysclk is also the DVI bit clock -- so each card has
// its own value here AND its own entries in the video mode table, whose
// bit_clk_khz must match. Do not change one without the other.

extern const source_mode_t  src_mda;      // MDA / Hercules, standard 16.257 MHz
extern const source_mode_t  src_mda16;    // ditto, 16.000 MHz clone crystal
extern const source_mode_t  src_cga640;   // CGA 640-wide (80-col / 640x200), RGBI
extern const source_mode_t  src_c128;     // Commodore 128 VDC 80-col, RGBI
extern const source_mode_t *g_src;        // active card (points at one of the above)

// Number of selectable cards, and the descriptor for index i (NULL if out of
// range). Index order is the `source` command's argument order; it is a display
// convenience only, safe to reorder, because nothing persistent stores it.
uint                 source_count(void);
const source_mode_t *source_at(uint i);
uint                 source_current_index(void);

// Stable-ID accessors -- use these for anything that outlives a boot.
uint8_t              source_current_id(void);
const source_mode_t *source_by_id(uint8_t id);   // NULL if unknown

// Select a card by index and reboot into it. Does not return. The choice
// survives in watchdog scratch[5]/[6]; source_apply_boot_choice() picks it up.
void source_set(uint i);

// Read the pending selection from watchdog scratch and point g_src at it.
// Call FIRST in main(), before video_init() -- video reads g_src->sysclk_khz.
void source_apply_boot_choice(void);

#endif
