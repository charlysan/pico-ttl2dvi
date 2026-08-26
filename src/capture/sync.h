#ifndef TTL2DVI_SYNC_H
#define TTL2DVI_SYNC_H

#include <stdint.h>

// Sync-period measurement.
// Two PIO SMs on pio0 run the countdown program on HSYNC and VSYNC
void sync_init(void);

// Period measurement in clk_sys cycles. Frequency = clock_get_hz(clk_sys) / period.
uint32_t sync_hsync_period(void);
uint32_t sync_vsync_period(void);

// Full HSYNC measurement
// Used by capture framing to place the sampling window at active video.
uint32_t sync_hsync(uint32_t *pulse);
#endif
