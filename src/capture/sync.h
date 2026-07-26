#ifndef TTL2DVI_SYNC_H
#define TTL2DVI_SYNC_H

#include <stdint.h>

// Sync-period measurement. Two PIO SMs on pio0 run the countdown program on
// HSYNC and VSYNC (pins from board.h). Used for the `status` readout now, and
// by the capture engine later to size the sampling window.
//
// Frequency is period-only, so polarity doesn't matter here (pulse+gap = period
// regardless), and no pad inversion is applied.

// Claim + start the two measurement SMs on pio0. Call once (after any overclock,
// so the reported Hz uses the running clk_sys). Must NOT collide with the DVI
// PIO -- DVI is on pio1, this is pio0.
void sync_init(void);

// One-shot period measurement in clk_sys cycles; 0 if no edges (no signal).
// Frequency = clock_get_hz(clk_sys) / period. Each blocks up to ~250 ms then
// gives up (returns 0) rather than hanging the console when the source is off.
uint32_t sync_hsync_period(void);
uint32_t sync_vsync_period(void);

#endif
