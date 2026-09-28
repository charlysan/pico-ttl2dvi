#ifndef TTL2DVI_CAPTURE_H
#define TTL2DVI_CAPTURE_H

#include <stdbool.h>
#include <stdint.h>
#include "pico/types.h"     // uint

#define CAPTURE_MAX_WIDTH   864                         // pixels
#define CAPTURE_MAX_SAMPLES (CAPTURE_MAX_WIDTH * 2)     // at 2x oversampling

// Claim the sampler SM + DMA on pio0 and configure it for the active source.
// Call once, after sync_init()
void capture_init(void);
// Re-apply the active source (pins, bit count, sample period, bp/phase
// defaults) without claiming anything again. For a live source change.
void capture_reconfigure(void);

uint capture_px_cyc(void);                       // sysclk per pixel, rounded
uint32_t capture_spp(void);                      // samples per pixel, 16.16
uint32_t capture_line_cycles(void);              // last measured line, sysclk
uint32_t capture_frames(void);                   // frames grabbed since boot
void capture_lines_range(uint *lo, uint *hi);    // frame heights since the last call
bool capture_grab(void); // true = got a frame, false = no sync
void capture_set_bp(int bp);
int  capture_get_bp(void);
void capture_set_phase(int phase);
int  capture_get_phase(void);
uint capture_width(void);                        // pixels
uint capture_samples(void);                      // samples per line
uint capture_height(void);
void capture_get_line(uint line, uint8_t *dst);  // unpack one line, one sample per byte
const uint8_t *capture_raw_line(uint line);      // packed samples, LSB first
void capture_hold(void);                         // stop the sampler; next grab re-syncs
void capture_dump_frame(void);
#endif
